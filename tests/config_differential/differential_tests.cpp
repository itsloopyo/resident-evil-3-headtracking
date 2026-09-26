// The config differential test. Every input is read three ways:
//
//   oracle     the newest published build's reader (oracle/), the rolling dev pre-release built
//              from 234785c, with its Mod::LoadConfig
//   import     core's REFramework import, PluginConfigLegacyImport, over the frozen
//              PluginConfig::Read, which is what this build imports HeadTracking.ini through
//   migration  the config owner PluginMod builds, importing the input, as HeadTracking.ini, into a
//              new CameraUnlock.ini beside it, then the canonical reader and table on the result
//
// Comparison 1, oracle against import, finds what a player updating from the dev build sees
// change that the conversion did not cause. The dev build read HeadTracking.ini through its own
// src/core/config.cpp; 0a84896 changed that reader and a6d6069 moved the plugin onto core's
// PluginMod and PluginConfig, whose reader the import is. Every difference it may find is listed
// in kComparisonOneDifferences with its commit, each input's differences are matched to one of
// them, and any other fails the test.
//
// Comparison 2, import against migration, is the proof for the migration: no difference in what
// the plugin starts with, and no drop but the approved one. The dev build shipped the rotation
// multipliers at 1.0, the position sensitivities at 2.0 and the inversions off. Those values stay
// in the plugin as PluginConfig::SetDefaults for this schema (positionSensitivity 2.0), and a
// value the player set away from them is dropped (pose_shaping). PluginConfig::Read clamps every
// number into a range the canonical rows hold, replaces a value that is not finite and refuses a
// hotkey code it cannot poll, so N1 and N2 never apply. No default moved, so the no-file input
// has no difference either.
//
// Every owner reads and creates one scratch Defaults.ini, which it creates with the built-in
// values, so an input whose values are the built-in ones migrates to `default` rows. The distinct
// migrated files are written beside the executable under migrated\, for lint-migrated.mjs to run
// core's canonical config lint over.
//
// Inputs: no file, an empty file, the HeadTracking.ini the dev build shipped in its installer ZIP,
// which its launcher manifest seeds byte for byte, the version committed before it (6a4132d) and
// the one committed after it and never published (0a84896), the file the dev build writes at
// first launch when there is none (extracted once into inputs/), core's corpus over the shipped
// file, and that file with all three hotkeys on each code from 0x00 to 0xFF.

#include "config_owner_options.h"
#include "oracle_adapter.h"

#include <cameraunlock/config/config_owner.h>
#include <cameraunlock/config/defaults_file.h>
#include <cameraunlock/config/legacy_import.h>
#include <cameraunlock/config/testing/ini_mutations.h>
#include <cameraunlock/input/key_binding_registration.h>
#include <cameraunlock/input/key_bindings.h>
#include <cameraunlock/reframework/plugin_config.h>
#include <cameraunlock/reframework/plugin_config_table.h>
#include <cameraunlock/tracking/tracking_mode.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace cfg = cameraunlock::config;
using RE3HT::Config;
using cameraunlock::TrackingMode;
using cameraunlock::config::testing::GenerateIniMutations;
using cameraunlock::config::testing::IniMutation;
using cameraunlock::config::testing::MutationKey;
using cameraunlock::input::KeyModifiers;

int g_failures = 0;

void Fail(const std::string& input, const std::string& what) {
    if (g_failures < 50) std::printf("FAIL [%s]: %s\n", input.c_str(), what.c_str());
    ++g_failures;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::string Narrow(const std::wstring& path) {
    const int size = WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), 'x');
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

std::string ReadBytes(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + Narrow(path));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + Narrow(path));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Every file in the folder, name and bytes, for "the import changed nothing".
std::map<std::wstring, std::string> Snapshot(const std::wstring& dir) {
    std::map<std::wstring, std::string> files;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot list the test folder");
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        files[data.cFileName] = ReadBytes(dir + L"\\" + data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return files;
}

void EmptyFolder(const std::wstring& dir) {
    for (const auto& [name, bytes] : Snapshot(dir)) {
        const std::wstring path = dir + L"\\" + name;
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(path.c_str())) throw std::runtime_error("cannot empty the test folder");
    }
}

std::wstring MakeFolder(const std::wstring& parent, const wchar_t* name) {
    const std::wstring dir = parent + L"\\" + name;
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        throw std::runtime_error("cannot create the test folder");
    }
    EmptyFolder(dir);
    return dir;
}

// ---------------------------------------------------------------------------
// Hotkeys: what each build puts on its poller
// ---------------------------------------------------------------------------

enum class Action { Toggle, CycleMode, YawMode, Reticle };

const char* ActionName(Action a) {
    switch (a) {
        case Action::Toggle: return "toggle";
        case Action::CycleMode: return "cycle mode";
        case Action::YawMode: return "yaw mode";
        case Action::Reticle: return "reticle";
    }
    throw std::logic_error("action");
}

// One key the poller watches for an action, and the modifiers it fires with: kNav is NavGuarded
// (not while Ctrl and Shift are both held), kChord is ChordGuarded (while both are held).
struct Registration {
    Action action;
    int vk;
    unsigned modifiers;

    bool operator<(const Registration& o) const {
        return std::tie(action, vk, modifiers) < std::tie(o.action, o.vk, o.modifiers);
    }
    bool operator==(const Registration& o) const {
        return action == o.action && vk == o.vk && modifiers == o.modifiers;
    }
    bool operator!=(const Registration& o) const { return !(*this == o); }
};

constexpr unsigned kNav = static_cast<unsigned>(KeyModifiers::kNone);
constexpr unsigned kChord = static_cast<unsigned>(KeyModifiers::kCtrl | KeyModifiers::kShift);

std::string Describe(const std::vector<Registration>& regs) {
    std::string out;
    for (const Registration& r : regs) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s%s:%s0x%02X", out.empty() ? "" : " ", ActionName(r.action),
                      r.modifiers == kChord ? "Ctrl+Shift+" : "", static_cast<unsigned>(r.vk));
        out += buf;
    }
    return out;
}

// The dev build's plugin_main.cpp and the non-canonical path of core's plugin_bootstrap.cpp
// register the same thing for these three: each code NavGuarded, and the chords ChordGuarded
// unconditionally. The poller never polls code 0. The dev build also registered a reticle toggle
// on ReticleToggleKey and Ctrl+Shift+U, which comparison 1 lists (reticle-key) and nothing here
// models, since the flag it changed was never read.
std::vector<Registration> CodeHotkeys(int toggle, int cycle, int yaw) {
    std::vector<Registration> regs = {
        {Action::Toggle, 'Y', kChord},
        {Action::CycleMode, 'G', kChord},
        {Action::YawMode, 'H', kChord},
    };
    if (toggle != 0) regs.push_back({Action::Toggle, toggle, kNav});
    if (cycle != 0) regs.push_back({Action::CycleMode, cycle, kNav});
    if (yaw != 0) regs.push_back({Action::YawMode, yaw, kNav});
    std::sort(regs.begin(), regs.end());
    return regs;
}

// The canonical path of plugin_bootstrap.cpp: the three lists, each binding with its modifiers.
std::vector<Registration> ListHotkeys(const Config& c) {
    std::vector<Registration> regs;
    const std::pair<Action, const std::string*> lists[] = {
        {Action::Toggle, &c.toggleKeyBindings},
        {Action::CycleMode, &c.cycleTrackingModeKeyBindings},
        {Action::YawMode, &c.yawModeKeyBindings},
    };
    for (const auto& [action, list] : lists) {
        const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(*list);
        if (!parsed.ok()) throw std::logic_error("a migrated hotkey list does not parse: " + *list);
        for (const cameraunlock::input::KeyBinding& b : parsed.bindings) {
            regs.push_back({action, b.vk, static_cast<unsigned>(b.modifiers)});
        }
    }
    std::sort(regs.begin(), regs.end());
    return regs;
}

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// ---------------------------------------------------------------------------
// What the plugin starts with
// ---------------------------------------------------------------------------

// Everything the startup code takes from the config, but pose shaping: Mod::Initialize at 234785c
// and PluginMod::Initialize now, and the hotkeys.
struct Startup {
    int port = 0;
    bool enabled = false;
    TrackingMode mode = TrackingMode::RotationAndPosition;
    bool world_yaw = false;
    uint32_t local_smoothing = 0;
    uint32_t remote_smoothing = 0;
    uint32_t limit_x = 0;
    uint32_t limit_y = 0;
    uint32_t limit_y_down = 0;
    uint32_t limit_z = 0;
    uint32_t limit_z_back = 0;
    std::vector<Registration> hotkeys;
};

// The dev build never set PositionSettings::limit_y_down, so the downward limit was that struct's
// default at its core pin (3465659), 0.20 m, whatever LimitY said.
constexpr float kPublishedLimitYDown = 0.20f;

Startup FromOracle(const oracle_api::Config& c) {
    Startup s;
    s.port = c.udpPort;
    s.enabled = c.autoEnable;
    s.mode = c.positionEnabled ? TrackingMode::RotationAndPosition : TrackingMode::RotationOnly;
    s.world_yaw = c.worldSpaceYaw;
    s.local_smoothing = Bits(c.localSmoothing);
    s.remote_smoothing = Bits(c.remoteSmoothing);
    s.limit_x = Bits(c.positionLimitX);
    s.limit_y = Bits(c.positionLimitY);
    s.limit_y_down = Bits(kPublishedLimitYDown);
    s.limit_z = Bits(c.positionLimitZ);
    s.limit_z_back = Bits(c.positionLimitZBack);
    s.hotkeys = CodeHotkeys(c.toggleKey, c.positionToggleKey, c.yawModeKey);
    return s;
}

// PluginMod::Initialize on what PluginConfig::Read gave, with the hotkeys the import maps each
// code to (the code and its chord).
Startup FromImport(const Config& c) {
    Startup s;
    s.port = c.udpPort;
    s.enabled = c.autoEnable;
    s.mode = c.positionEnabled ? TrackingMode::RotationAndPosition : TrackingMode::RotationOnly;
    s.world_yaw = c.worldSpaceYaw;
    s.local_smoothing = Bits(c.localSmoothing);
    s.remote_smoothing = Bits(c.remoteSmoothing);
    s.limit_x = Bits(c.positionLimitX);
    s.limit_y = Bits(c.positionLimitY);
    s.limit_y_down = Bits(c.positionLimitY);
    s.limit_z = Bits(c.positionLimitZ);
    s.limit_z_back = Bits(c.positionLimitZBack);
    s.hotkeys = CodeHotkeys(c.toggleKey, c.positionToggleKey, c.yawModeKey);
    return s;
}

// PluginMod::Initialize on the migrated config, and the canonical registration.
Startup FromMigration(const Config& c) {
    Startup s = FromImport(c);
    s.hotkeys = ListHotkeys(c);
    return s;
}

std::vector<std::string> StartupDifferences(const Startup& a, const Startup& b) {
    std::vector<std::string> out;
#define SAME(f) \
    if (a.f != b.f) out.push_back(#f)
    SAME(port);
    SAME(enabled);
    SAME(mode);
    SAME(world_yaw);
    SAME(local_smoothing);
    SAME(remote_smoothing);
    SAME(limit_x);
    SAME(limit_y);
    SAME(limit_y_down);
    SAME(limit_z);
    SAME(limit_z_back);
#undef SAME
    if (a.hotkeys != b.hotkeys) out.push_back("hotkeys " + Describe(a.hotkeys) + " against " + Describe(b.hotkeys));
    return out;
}

// ---------------------------------------------------------------------------
// Comparison 1: the dev build against the import
// ---------------------------------------------------------------------------

// What a player updating from the dev build sees change, and the commit that made each change.
// The changelog carries the same list.
struct ListedDifference {
    const char* id;
    const char* commit;
    const char* what;
    int seen = 0;
};

ListedDifference kComparisonOneDifferences[] = {
    {"reticle-key", "0a84896",
     "Insert (ReticleToggleKey) and Ctrl+Shift+U no longer toggle the reticle, and [Reticle] Enabled is not "
     "read. The dev build registered both keys on a flag nothing read, so they changed nothing on screen."},
    {"non-finite", "0a84896",
     "A number that is not finite (nan, inf, 1e400) keeps the setting's default. The dev build kept a nan as "
     "it was and moved an infinity to the nearer end of the setting's range."},
    {"strict-number", "a6d6069",
     "A number followed by anything but an inline comment (0,15 or 0.5abc), or written in hex (0x1), keeps "
     "the setting's default. The dev build read the number at the front of the text: 0.0 for 0,15."},
    {"limit-floor", "a6d6069",
     "A position limit below 0.01 m is kept as written, down to 0. The dev build raised it to 0.01 m."},
    {"limit-y-down", "a6d6069",
     "LimitY sets how far the view moves down as well as up. The dev build held downward travel at 0.20 m "
     "whatever LimitY said."},
    {"unpollable-hotkey", "a6d6069",
     "A hotkey code PluginConfig::Read does not accept (0, a negative code, one above 0xFE, or Shift, Ctrl or "
     "Alt) keeps that hotkey's default key. The dev build registered the code as written."},
    {"shaping-range", "a6d6069",
     "A sensitivity outside the dev build's range reads as written within 0 to 5 (rotation) or 0 to 10 "
     "(position), where the dev build clamped it to 0.1 to 5, 0 to 2 for roll, or 0.1 to 10. Either way it is "
     "not the value the dev build shipped, so it is dropped and not carried (pose_shaping)."},
};

ListedDifference& Listed(const char* id) {
    for (ListedDifference& d : kComparisonOneDifferences) {
        if (std::strcmp(d.id, id) == 0) return d;
    }
    throw std::logic_error(std::string("no listed difference ") + id);
}

// The key's value as GetPrivateProfileString gives it to both readers, or nullopt when the key is
// absent.
std::optional<std::string> RawValue(const std::string& ansiPath, const char* section, const char* key) {
    char buf[4096];
    const char* kAbsent = "\x01\x02" "absent";
    GetPrivateProfileStringA(section, key, kAbsent, buf, sizeof(buf), ansiPath.c_str());
    if (std::strcmp(buf, kAbsent) == 0) return std::nullopt;
    return std::string(buf);
}

// True when the dev build's strtod prefix parse reads a number from `raw` where the strict parse
// PluginConfig::Read uses (whole token, inline comment stripped, no hex) reads none.
bool PrefixOnlyNumber(const std::string& raw) {
    char* end = nullptr;
    std::strtod(raw.c_str(), &end);
    if (end == raw.c_str()) return false;
    std::string text = raw.substr(0, raw.find_first_of(";#"));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    if (text.empty() || text.front() == ' ' || text.front() == '\t') return true;
    if (text.find_first_of("xX") != std::string::npos) return true;
    std::strtod(text.c_str(), &end);
    return *end != '\0';
}

bool Pollable(int vk) {
    if (vk < 0x01 || vk > 0xFE) return false;
    if (vk >= 0x10 && vk <= 0x12) return false;
    if (vk >= 0xA0 && vk <= 0xA5) return false;
    return true;
}

// True when the dev build's strtod prefix parse of `raw` gives a number that is not finite: nan,
// inf, or one too large for a double.
bool NonFiniteNumber(const std::string& raw) {
    char* end = nullptr;
    const double v = std::strtod(raw.c_str(), &end);
    return end != raw.c_str() && !std::isfinite(v);
}

struct FloatField {
    const char* section;
    const char* key;
    float oracle_api::Config::*oracle;
    float Config::*read;
    enum Kind { kSetting, kLimit, kShaping } kind;
    // What the dev build shipped, for a pose-shaping value.
    float shipped;
};

const FloatField kFloatFields[] = {
    {"Smoothing", "LocalSmoothing", &oracle_api::Config::localSmoothing, &Config::localSmoothing, FloatField::kSetting, 0.0f},
    {"Smoothing", "RemoteSmoothing", &oracle_api::Config::remoteSmoothing, &Config::remoteSmoothing, FloatField::kSetting, 0.0f},
    {"Position", "LimitX", &oracle_api::Config::positionLimitX, &Config::positionLimitX, FloatField::kLimit, 0.0f},
    {"Position", "LimitY", &oracle_api::Config::positionLimitY, &Config::positionLimitY, FloatField::kLimit, 0.0f},
    {"Position", "LimitZ", &oracle_api::Config::positionLimitZ, &Config::positionLimitZ, FloatField::kLimit, 0.0f},
    {"Position", "LimitZBack", &oracle_api::Config::positionLimitZBack, &Config::positionLimitZBack, FloatField::kLimit, 0.0f},
    {"Sensitivity", "YawMultiplier", &oracle_api::Config::yawMultiplier, &Config::yawMultiplier, FloatField::kShaping, 1.0f},
    {"Sensitivity", "PitchMultiplier", &oracle_api::Config::pitchMultiplier, &Config::pitchMultiplier, FloatField::kShaping, 1.0f},
    {"Sensitivity", "RollMultiplier", &oracle_api::Config::rollMultiplier, &Config::rollMultiplier, FloatField::kShaping, 1.0f},
    {"Position", "SensitivityX", &oracle_api::Config::positionSensitivityX, &Config::positionSensitivityX, FloatField::kShaping, 2.0f},
    {"Position", "SensitivityY", &oracle_api::Config::positionSensitivityY, &Config::positionSensitivityY, FloatField::kShaping, 2.0f},
    {"Position", "SensitivityZ", &oracle_api::Config::positionSensitivityZ, &Config::positionSensitivityZ, FloatField::kShaping, 2.0f},
};

struct HotkeyField {
    const char* key;
    int oracle_api::Config::*oracle;
    int Config::*read;
};

const HotkeyField kHotkeyFields[] = {
    {"ToggleKey", &oracle_api::Config::toggleKey, &Config::toggleKey},
    {"PositionToggleKey", &oracle_api::Config::positionToggleKey, &Config::positionToggleKey},
    {"YawModeKey", &oracle_api::Config::yawModeKey, &Config::yawModeKey},
};

// Every difference between the dev build's reading of the input and PluginConfig::Read's, each matched to
// a listed difference or failed. `ansiPath` is the import's copy of the input.
void CompareOracleWithImport(const std::string& name, const std::string& ansiPath, bool oracleRead,
                             const oracle_api::Config& o, bool importFound, const Config& r) {
    if (oracleRead != importFound) {
        Fail(name, "comparison 1: the dev build and the import do not agree on whether there is a file");
        return;
    }
    const Config defaults = [] {
        Config c;
        c.SetDefaults(RE3HT::kConfigSchema);
        return c;
    }();

    std::set<std::string> seen;
    for (const FloatField& f : kFloatFields) {
        const float ov = o.*f.oracle;
        const float rv = r.*f.read;
        if (Bits(ov) == Bits(rv)) continue;
        const std::string label = std::string("[") + f.section + "] " + f.key;
        const std::optional<std::string> raw = RawValue(ansiPath, f.section, f.key);
        if (raw && NonFiniteNumber(*raw) && Bits(rv) == Bits(defaults.*f.read)) {
            seen.insert("non-finite");
        } else if (raw && PrefixOnlyNumber(*raw) && Bits(rv) == Bits(defaults.*f.read)) {
            seen.insert("strict-number");
        } else if (f.kind == FloatField::kLimit && ov == 0.01f && rv >= 0.0f && rv < 0.01f) {
            seen.insert("limit-floor");
        } else if (f.kind == FloatField::kShaping && ov != f.shipped && rv != f.shipped) {
            seen.insert("shaping-range");
        } else {
            Fail(name, "comparison 1: " + label + " is " + std::to_string(rv) + ", the dev build read " +
                           std::to_string(ov) + ", with no listed reason");
        }
    }
    for (const HotkeyField& h : kHotkeyFields) {
        const int ov = o.*h.oracle;
        const int rv = r.*h.read;
        if (ov == rv) continue;
        if (!Pollable(ov) && rv == defaults.*h.read) {
            seen.insert("unpollable-hotkey");
        } else {
            Fail(name, std::string("comparison 1: [Hotkeys] ") + h.key + " is " + std::to_string(rv) +
                           ", the dev build read " + std::to_string(ov) + ", with no listed reason");
        }
    }
    const auto same = [&](bool equal, const char* field) {
        if (!equal) {
            Fail(name, std::string("comparison 1: ") + field + " differs from the dev build with no listed reason");
        }
    };
    same(o.udpPort == r.udpPort, "udpPort");
    same(o.positionInvertX == r.positionInvertX, "positionInvertX");
    same(o.positionInvertY == r.positionInvertY, "positionInvertY");
    same(o.positionInvertZ == r.positionInvertZ, "positionInvertZ");
    same(o.positionEnabled == r.positionEnabled, "positionEnabled");
    same(o.autoEnable == r.autoEnable, "autoEnable");
    same(o.worldSpaceYaw == r.worldSpaceYaw, "worldSpaceYaw");

    // The start is built from the fields above alone, but for the downward limit, which the dev
    // build never set from the file, and the reticle toggle it always registered.
    if (Bits(r.positionLimitY) != Bits(kPublishedLimitYDown)) seen.insert("limit-y-down");
    seen.insert("reticle-key");
    for (const std::string& id : seen) ++Listed(id.c_str()).seen;
}

// ---------------------------------------------------------------------------
// The keys the import reads, and the corpus descriptors
// ---------------------------------------------------------------------------

// One valid value other than the shipped one, and one value past each bound either reader clamps
// or refuses, for every key PluginConfigLegacyImport lists for this schema.
std::vector<MutationKey> CorpusKeys(const std::vector<cfg::LegacyKey>& reads) {
    std::vector<MutationKey> keys;
    for (const cfg::LegacyKey& read : reads) {
        MutationKey k;
        k.section = read.section;
        k.key = read.key;
        const std::string& s = read.section;
        const std::string& key = read.key;
        if (s == "Network" && key == "UDPPort") {
            k.alternate = "5555";
            k.out_of_range = {"80", "70000"};
        } else if (s == "Sensitivity" && key == "RollMultiplier") {
            k.alternate = "0.5";
            k.out_of_range = {"3", "6", "-1"};
        } else if (s == "Sensitivity") {
            k.alternate = "1.5";
            k.out_of_range = {"0.05", "6", "-1"};
        } else if (s == "Smoothing") {
            k.alternate = "0.3";
            k.out_of_range = {"1.5", "-0.5"};
        } else if (s == "Position" && key == "Smoothing") {
            k.alternate = "0.5";
        } else if (s == "Hotkeys") {
            k.hotkey = true;
            k.alternate = key == "ToggleKey" ? "0x24" : key == "PositionToggleKey" ? "0x2E" : "0x70";
            k.out_of_range = {"0x11", "0xFF", "0x100", "-1"};
        } else if (s == "Position" && key.rfind("Sensitivity", 0) == 0) {
            k.alternate = "1.5";
            k.out_of_range = {"0.05", "11", "-1"};
        } else if (s == "Position" && key.rfind("Limit", 0) == 0) {
            k.alternate = "0.5";
            k.out_of_range = {"0.005", "2.5", "-0.1"};
        } else if (s == "Position" && key.rfind("Invert", 0) == 0) {
            k.alternate = "true";
        } else if (s == "Position" && key == "Enabled") {
            k.alternate = "false";
        } else if (s == "General" && key == "AutoEnable") {
            k.alternate = "false";
        } else if (s == "General" && key == "WorldSpaceYaw") {
            k.alternate = "false";
        } else if (s == "General" && key == "ConfigVersion") {
            k.alternate = "1";
        } else {
            throw std::logic_error("no corpus description for [" + s + "] " + key);
        }
        keys.push_back(std::move(k));
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Comparison 2: the import against the migration
// ---------------------------------------------------------------------------

const cfg::DroppedValue* FindDrop(const std::vector<cfg::DroppedValue>& dropped, cfg::DropRule rule,
                                  const char* section, const char* key) {
    for (const cfg::DroppedValue& d : dropped) {
        if (d.rule == rule && d.section == section && d.key == key) return &d;
    }
    return nullptr;
}

// The migrated config shapes the pose exactly as the dev build shipped it: every sensitivity and
// inversion is what SetDefaults gives for this schema, which PluginMod hands the processors.
void CheckShippedShaping(const std::string& name, const Config& c) {
    if (c.yawMultiplier != 1.0f || c.pitchMultiplier != 1.0f || c.rollMultiplier != 1.0f ||
        c.positionSensitivityX != 2.0f || c.positionSensitivityY != 2.0f || c.positionSensitivityZ != 2.0f ||
        c.positionInvertX || c.positionInvertY || c.positionInvertZ) {
        Fail(name, "the migrated config does not shape the pose as the dev build shipped it");
    }
}

// Every sensitivity and inversion PluginConfig::Read read is listed in its place, folded where it
// holds what the dev build shipped and dropped as PoseShaping where it does not. Returns how many
// were dropped.
int CheckPoseShaping(const std::string& name, const Config& r, const cfg::ImportResult& result) {
    struct Read {
        const char* section;
        const char* key;
        bool atShipped;
        const char* shipped;
    };
    const Read reads[] = {
        {"Sensitivity", "YawMultiplier", r.yawMultiplier == 1.0f, "1.0"},
        {"Sensitivity", "PitchMultiplier", r.pitchMultiplier == 1.0f, "1.0"},
        {"Sensitivity", "RollMultiplier", r.rollMultiplier == 1.0f, "1.0"},
        {"Position", "SensitivityX", r.positionSensitivityX == 2.0f, "2.0"},
        {"Position", "SensitivityY", r.positionSensitivityY == 2.0f, "2.0"},
        {"Position", "SensitivityZ", r.positionSensitivityZ == 2.0f, "2.0"},
        {"Position", "InvertX", !r.positionInvertX, "false"},
        {"Position", "InvertY", !r.positionInvertY, "false"},
        {"Position", "InvertZ", !r.positionInvertZ, "false"},
    };
    if (result.pose_shaping.size() != std::size(reads)) {
        Fail(name, "the import lists " + std::to_string(result.pose_shaping.size()) + " pose-shaping values, not 9");
        return 0;
    }
    int dropped = 0;
    for (size_t k = 0; k < std::size(reads); ++k) {
        const cfg::PoseShapingValue& v = result.pose_shaping[k];
        const bool shipped = reads[k].atShipped;
        const std::string label = std::string("[") + reads[k].section + "] " + reads[k].key;
        if (v.section != reads[k].section || v.key != reads[k].key) Fail(name, label + " is not listed in its place");
        if (v.shipped != reads[k].shipped) {
            Fail(name, label + " is held to " + v.shipped + ", not the " + reads[k].shipped + " the dev build shipped");
        }
        if (v.folded != shipped) Fail(name, label + " is " + (v.folded ? "folded" : "dropped") + " wrongly");
        const bool listed = FindDrop(result.dropped, cfg::DropRule::PoseShaping, reads[k].section, reads[k].key) != nullptr;
        if (listed == shipped) Fail(name, label + (listed ? " is dropped at its shipped value" : " is changed and not dropped"));
        if (!shipped) ++dropped;
    }
    return dropped;
}

void CheckDropRules(const std::string& name, const cfg::ImportResult& result) {
    for (const cfg::DroppedValue& d : result.dropped) {
        if (d.rule != cfg::DropRule::PoseShaping) {
            Fail(name, "the import drops [" + d.section + "] " + d.key + " by a rule other than PoseShaping");
        }
    }
}

struct MigrationTally {
    std::string committed;
    std::wstring defaults;
    std::set<std::string> migrated;
    int created = 0;
    int converted = 0;
    int with_pose_shaping_dropped = 0;
};

cfg::ConfigOwnerOptions<Config> Options(const std::wstring& dir, const MigrationTally& tally) {
    return RE3HT::testing::OwnerOptions(dir, cfg::DefaultsFile::At(tally.defaults));
}

FILETIME WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        throw std::runtime_error("cannot read a test file's attributes");
    }
    return data.ftLastWriteTime;
}

bool SameTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

// Each input is read in folders of its own: every reader goes through GetPrivateProfileString,
// and one file rewritten per reader for every input read back another input's values in some runs
// of the first native differential test (fallout-4-headtracking).
struct Folders {
    std::wstring root;
    std::wstring oracle;
    std::wstring import;
    std::wstring migration;
    std::wstring read_only;
};

Folders NextFolders(const std::wstring& root) {
    static int n = 0;
    const std::wstring dir = MakeFolder(root, std::to_wstring(n++).c_str());
    return {dir, MakeFolder(dir, L"oracle"), MakeFolder(dir, L"import"), MakeFolder(dir, L"migration"),
            MakeFolder(dir, L"read-only")};
}

void RemoveFolders(const Folders& f) {
    for (const std::wstring& dir : {f.oracle, f.import, f.migration, f.read_only, f.root}) {
        EmptyFolder(dir);
        if (!RemoveDirectoryW(dir.c_str())) throw std::runtime_error("cannot remove a test folder");
    }
}

const std::wstring kLegacyName = RE3HT::testing::kLegacyFileName;
const std::wstring kConfigName = RE3HT::testing::kConfigFileName;

struct Migration {
    cfg::ConfigLoadResult<Config> loaded;
    std::optional<std::string> bytes;
};

// The owner's load on the legacy file `legacyBytes` in `dir`, read-only when asked, with
// everything the migration must leave as it was checked afterwards. `bytes` is the created
// CameraUnlock.ini, if one was.
Migration Migrate(const std::string& name, const std::wstring& dir, const std::optional<std::string>& legacyBytes,
                  bool readOnly, const MigrationTally& tally) {
    const std::wstring legacyPath = dir + L"\\" + kLegacyName;
    const std::wstring path = dir + L"\\" + kConfigName;
    FILETIME before{};
    if (legacyBytes) {
        WriteBytes(legacyPath, *legacyBytes);
        if (readOnly) SetFileAttributesW(legacyPath.c_str(), FILE_ATTRIBUTE_READONLY);
        before = WriteTime(legacyPath);
    }

    Migration m{};
    {
        cfg::ConfigOwner<Config> owner(Options(dir, tally));
        m.loaded = owner.Load();
    }

    std::map<std::wstring, std::string> expected;
    if (legacyBytes) {
        expected[kLegacyName] = *legacyBytes;
        if (!SameTime(WriteTime(legacyPath), before)) Fail(name, "the legacy file's write time changed");
        const DWORD attributes = GetFileAttributesW(legacyPath.c_str());
        if (((attributes & FILE_ATTRIBUTE_READONLY) != 0) != readOnly) Fail(name, "the legacy file's read-only attribute changed");
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m.bytes = ReadBytes(path);
        expected[kConfigName] = *m.bytes;
    }
    if (Snapshot(dir) != expected) Fail(name, "the folder holds files other than the legacy file and CameraUnlock.ini");
    return m;
}

// The migrated file loaded again over the same Defaults.ini: it reads as canonical with nothing
// to report, gives the same start, imports nothing and changes neither file.
void CheckSecondLoad(const std::string& name, const std::wstring& dir, const Migration& first,
                     const std::optional<std::string>& legacyBytes, const MigrationTally& tally) {
    const auto before = Snapshot(dir);
    cfg::ConfigOwner<Config> owner(Options(dir, tally));
    const cfg::ConfigLoadResult<Config> again = owner.Load();
    if (again.status != cfg::ConfigLoadStatus::Canonical) Fail(name, "the second load is not Canonical");
    if (!again.diagnostics.empty()) Fail(name, "the migrated file draws diagnostics");
    if (!StartupDifferences(FromMigration(first.loaded.config), FromMigration(again.config)).empty()) {
        Fail(name, "the second load starts differently from the first");
    }
    if (Snapshot(dir) != before) Fail(name, "the second load changed a file");
    const bool saysLegacyUnread = std::any_of(again.log.begin(), again.log.end(), [](const std::string& line) {
        return line.find("is left as it was and is not read") != std::string::npos;
    });
    if (legacyBytes.has_value() != saysLegacyUnread) {
        Fail(name, "the second load's log does not say whether the legacy file was left unread");
    }
}

void MigrateInput(const Folders& f, const std::string& name, const std::optional<std::string>& bytes,
                  const Config& read, const cfg::ImportResult& result, MigrationTally& tally) {
    using cfg::ConfigLoadStatus;
    const Migration m = Migrate(name, f.migration, bytes, false, tally);
    CheckShippedShaping(name, m.loaded.config);

    if (!bytes) {
        ++tally.created;
        if (m.loaded.status != ConfigLoadStatus::Created) Fail(name, "no file is not Created");
        if (m.bytes != tally.committed) Fail(name, "the created file is not HeadTracking.ini as committed");
        for (const std::string& d : StartupDifferences(FromImport(read), FromMigration(m.loaded.config))) {
            Fail(name, "comparison 2: " + d);
        }
        CheckSecondLoad(name, f.migration, m, bytes, tally);
        return;
    }

    if (CheckPoseShaping(name, read, result) > 0) ++tally.with_pose_shaping_dropped;
    CheckDropRules(name, result);

    for (const std::string& d : StartupDifferences(FromImport(read), FromMigration(m.loaded.config))) {
        Fail(name, "comparison 2: " + d);
    }

    ++tally.converted;
    if (m.loaded.status != ConfigLoadStatus::Migrated) {
        Fail(name, std::string("the migration is ") + cfg::ConfigLoadStatusName(m.loaded.status) + ": " + m.loaded.reason);
        return;
    }
    tally.migrated.insert(*m.bytes);

    const Migration ro = Migrate(name, f.read_only, bytes, true, tally);
    if (ro.loaded.status != ConfigLoadStatus::Migrated || ro.bytes != m.bytes) {
        Fail(name, "a read-only legacy file does not import as a writable one does");
    }

    CheckSecondLoad(name, f.migration, m, bytes, tally);
}

void RunInput(const std::wstring& root, const std::string& name, const std::optional<std::string>& bytes,
              MigrationTally& tally) {
    const Folders f = NextFolders(root);
    oracle_api::Config o;
    bool oracleRead = false;
    {
        const std::wstring path = f.oracle + L"\\" + kLegacyName;
        if (bytes) WriteBytes(path, *bytes);
        oracleRead = oracle_api::LoadOrCreate(Narrow(path).c_str(), o) == oracle_api::LoadStatus::Read;
    }

    Config read;
    bool found = false;
    cfg::ImportResult result;
    const std::wstring importPath = f.import + L"\\" + kLegacyName;
    {
        if (bytes) {
            WriteBytes(importPath, *bytes);
            SetFileAttributesW(importPath.c_str(), FILE_ATTRIBUTE_READONLY);
        }
        const auto before = Snapshot(f.import);
        found = read.Read(Narrow(importPath).c_str(), RE3HT::kConfigSchema);
        Config mapped = cameraunlock::reframework::PluginConfigTable(RE3HT::kConfigSchema).defaults();
        result = cameraunlock::reframework::PluginConfigLegacyImport(RE3HT::kConfigSchema)
                     .run(cfg::detail::OwnerLegacyInput(importPath), mapped);
        const Startup mappedStart = FromMigration(mapped);
        const Startup readStart = FromImport(read);
        for (const std::string& d : StartupDifferences(readStart, mappedStart)) {
            Fail(name, "the import maps " + d + " away from what PluginConfig::Read read");
        }
        if (Snapshot(f.import) != before) Fail(name, "the import changed the folder it read from");
        if (bytes.has_value() != (result.status == cfg::ImportStatus::Imported)) {
            Fail(name, "the import's status does not say whether there was a file");
        }
    }

    CompareOracleWithImport(name, Narrow(importPath), oracleRead, o, found, read);
    MigrateInput(f, name, bytes, read, result, tally);
    RemoveFolders(f);
}

// A legacy file another program holds open with no sharing. The dev build found the file, so it wrote
// nothing, and GetPrivateProfileString read nothing from it, so it ran on its defaults. The owner
// defers the import on the defaults, which start the same, creates nothing and saves nothing that
// session.
void TestUnopenableFile(const std::wstring& root, const std::string& shipped, const MigrationTally& tally) {
    const std::string name = "a legacy file another program holds open with no sharing";
    const Folders f = NextFolders(root);
    oracle_api::Config o;
    bool oracleRead = false;
    Config read;
    bool found = false;
    std::optional<cfg::ConfigLoadResult<Config>> loaded;
    for (const std::wstring& dir : {f.oracle, f.import, f.migration}) {
        const std::wstring path = dir + L"\\" + kLegacyName;
        WriteBytes(path, shipped);
        HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (held == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot hold the test file open");
        if (dir == f.oracle) {
            oracleRead = oracle_api::LoadOrCreate(Narrow(path).c_str(), o) == oracle_api::LoadStatus::Read;
        } else if (dir == f.import) {
            found = read.Read(Narrow(path).c_str(), RE3HT::kConfigSchema);
        } else {
            cfg::ConfigOwner<Config> owner(Options(dir, tally));
            loaded.emplace(owner.Load());
            if (owner.Save([](Config& c) { c.worldSpaceYaw = false; }).status != cfg::ConfigSaveStatus::NotSaved) {
                Fail(name, "a deferred session saved");
            }
        }
        CloseHandle(held);
        if (ReadBytes(path) != shipped) Fail(name, "a build rewrote a file it could not open");
    }
    CompareOracleWithImport(name, Narrow(f.import + L"\\" + kLegacyName), oracleRead, o, found, read);
    if (loaded->status != cfg::ConfigLoadStatus::Deferred) {
        Fail(name, std::string("the owner's load is ") + cfg::ConfigLoadStatusName(loaded->status) + ", not Deferred");
    }
    for (const std::string& d : StartupDifferences(FromImport(read), FromMigration(loaded->config))) {
        Fail(name, "comparison 2: " + d);
    }
    if (Snapshot(f.migration) != std::map<std::wstring, std::string>{{kLegacyName, shipped}}) {
        Fail(name, "a deferred import created a file or changed the legacy one");
    }
    RemoveFolders(f);
}

// Registration compares the two builds by key and modifiers, which holds only while a binding
// with no modifiers fires as the old build's NavGuarded did (not while Ctrl and Shift are both
// held) and a Ctrl+Shift binding as its ChordGuarded did (while both are held). Alt changes
// neither.
void TestRegistrationModel() {
    using cameraunlock::input::detail::BindingFires;
    for (unsigned held = 0; held < 8; ++held) {
        const auto mods = static_cast<KeyModifiers>(held);
        const bool chordHeld = cameraunlock::input::HasModifiers(mods, KeyModifiers::kCtrl | KeyModifiers::kShift);
        if (BindingFires(KeyModifiers::kNone, mods) != !chordHeld) {
            Fail("registration", "a key with no modifiers does not fire as NavGuarded did, held " + std::to_string(held));
        }
        if (BindingFires(KeyModifiers::kCtrl | KeyModifiers::kShift, mods) != chordHeld) {
            Fail("registration", "a Ctrl+Shift key does not fire as ChordGuarded did, held " + std::to_string(held));
        }
    }
}

// The dev build's defaults and PluginConfig's for this schema, which the import starts every key an old
// file lacks from, are the same start.
void TestDefaults() {
    Config d;
    d.SetDefaults(RE3HT::kConfigSchema);
    for (const std::string& diff : StartupDifferences(FromOracle(oracle_api::Defaults()), FromImport(d))) {
        Fail("defaults", diff + ": PluginConfig's default differs from the dev build's");
    }
    const oracle_api::Config o = oracle_api::Defaults();
    for (const FloatField& f : kFloatFields) {
        if (f.kind == FloatField::kShaping && Bits(o.*f.oracle) != Bits(d.*f.read)) {
            Fail("defaults", std::string(f.key) + ": PluginConfig's default differs from the dev build's");
        }
    }
}

std::string ReadInput(const std::string& file) {
    return ReadBytes(Widen(std::string(RE3HT_DIFFERENTIAL_INPUTS) + "/" + file));
}

// `text` with the value of its one `key=` line replaced, the rest of that line included.
std::string WithValue(const std::string& text, const std::string& key, const std::string& value) {
    const size_t at = text.find("\n" + key + "=");
    if (at == std::string::npos || text.find("\n" + key + "=", at + 1) != std::string::npos) {
        throw std::logic_error("the shipped file does not hold exactly one " + key + " line");
    }
    const size_t start = at + 1 + key.size() + 1;
    const size_t end = text.find_first_of("\r\n", start);
    return text.substr(0, start) + value + text.substr(end);
}

// Every hotkey code from 0x00 to 0xFF on all three hotkeys at once. The corpus tries one
// alternate and a few refused codes per hotkey; this is where the codes the key table has no name
// for, the modifiers and the codes PluginConfig::Read refuses are all covered.
std::vector<std::pair<std::string, std::string>> EveryHotkeyCode(const std::string& shipped) {
    std::vector<std::pair<std::string, std::string>> inputs;
    for (int vk = 0x00; vk <= 0xFF; ++vk) {
        char code[8];
        std::snprintf(code, sizeof(code), "0x%02X", static_cast<unsigned>(vk));
        std::string bytes = shipped;
        for (const char* key : {"ToggleKey", "PositionToggleKey", "YawModeKey"}) bytes = WithValue(bytes, key, code);
        inputs.emplace_back(std::string("every hotkey ") + code, bytes);
    }
    return inputs;
}

// The dev build's first-run output, which inputs/first-run-dev.ini holds.
std::string OracleFirstRun(const std::wstring& root) {
    const Folders f = NextFolders(root);
    const std::wstring path = f.oracle + L"\\" + kLegacyName;
    oracle_api::Config created;
    if (oracle_api::LoadOrCreate(Narrow(path).c_str(), created) != oracle_api::LoadStatus::Created) {
        throw std::logic_error("the oracle read a file in an empty folder");
    }
    std::string bytes = ReadBytes(path);
    RemoveFolders(f);
    return bytes;
}

}  // namespace

int main() {
    try {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring root = std::wstring(temp) + L"re3ht-config-differential-" +
                                  std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), nullptr);

        MigrationTally tally;
        tally.committed = ReadBytes(Widen(RE3HT_COMMITTED_CONFIG));
        tally.defaults = MakeFolder(root, L"global") + L"\\Defaults.ini";

        TestDefaults();
        TestRegistrationModel();

        const std::string shipped = ReadInput("shipped-dev.ini");
        const std::string firstRun = ReadInput("first-run-dev.ini");
        if (OracleFirstRun(root) != firstRun) {
            Fail("first run", "the oracle's first-run output is not inputs/first-run-dev.ini");
        }

        const std::vector<std::pair<std::string, std::optional<std::string>>> inputs = {
            {"no file", std::nullopt},
            {"empty file", std::string()},
            {"shipped and seeded, the dev build", shipped},
            {"committed, 6a4132d", ReadInput("committed-6a4132d.ini")},
            {"committed, 0a84896, never published", ReadInput("committed-0a84896.ini")},
            {"first run, the dev build", firstRun},
        };
        for (const auto& [name, bytes] : inputs) RunInput(root, name, bytes, tally);
        TestUnopenableFile(root, shipped, tally);

        // Fresh equals upgrade: every file the dev build shipped, seeded or wrote at first launch,
        // and each other committed version, converts to the committed file, as no file is created
        // as it. The sensitivities and inversions are not rows, and neither are the reticle keys or
        // the retired smoothing and recenter keys, so they leave no trace there.
        for (const auto& [name, bytes] : inputs) {
            if (!bytes || bytes->empty()) continue;
            const Folders f = NextFolders(root);
            const Migration m = Migrate(name, f.migration, bytes, false, tally);
            if (m.bytes != tally.committed) {
                Fail("fresh equals upgrade", name + " does not convert to the committed file");
            }
            RemoveFolders(f);
        }

        const cfg::LegacyImport<Config> import = cameraunlock::reframework::PluginConfigLegacyImport(RE3HT::kConfigSchema);
        const std::vector<IniMutation> corpus = GenerateIniMutations(shipped, import.keys, CorpusKeys(import.keys));
        for (const IniMutation& m : corpus) RunInput(root, "corpus: " + m.name, m.bytes, tally);

        const auto codes = EveryHotkeyCode(shipped);
        for (const auto& [name, bytes] : codes) RunInput(root, name, bytes, tally);

        std::printf("%zu inputs, %zu of them from the corpus and %zu with every hotkey on one code\n",
                    inputs.size() + corpus.size() + codes.size(), corpus.size(), codes.size());
        std::printf("comparison 1, the dev build against the import: %zu listed differences\n",
                    std::size(kComparisonOneDifferences));
        for (const ListedDifference& d : kComparisonOneDifferences) {
            std::printf("  %s (%s): %d inputs\n    %s\n", d.id, d.commit, d.seen, d.what);
            if (d.seen == 0) Fail(d.id, "a listed difference no input shows");
        }
        std::printf("comparison 2, the import against the migration: %d created, %d converted, %zu distinct files\n",
                    tally.created, tally.converted, tally.migrated.size());
        std::printf("  %d with a changed sensitivity or inversion dropped (pose_shaping)\n",
                    tally.with_pose_shaping_dropped);
        if (tally.with_pose_shaping_dropped == 0) Fail("pose shaping", "no input drops a changed value");
        if (tally.migrated.count(tally.committed) == 0) Fail("first run", "no input migrated to the committed file");

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring lintDir(exe);
        lintDir = lintDir.substr(0, lintDir.find_last_of(L'\\'));
        lintDir = MakeFolder(lintDir, L"migrated");
        int n = 0;
        for (const std::string& file : tally.migrated) {
            WriteBytes(lintDir + L"\\" + std::to_wstring(n++) + L".ini", file);
        }

        const std::wstring global = root + L"\\global";
        EmptyFolder(global);
        RemoveDirectoryW(global.c_str());
        RemoveDirectoryW(root.c_str());
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    if (g_failures == 0) {
        std::printf("config differential: all passed\n");
        return 0;
    }
    std::printf("config differential: %d failure(s)\n", g_failures);
    return 1;
}
