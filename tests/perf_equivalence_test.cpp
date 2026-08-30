// Behaviour-locking tests for the two hot-path performance changes.
//
// The GUI-name dedup is driven through the shipped
// cameraunlock::reframework::LogGuiElementNameOnce, so a change to the shipped
// dedup or to kMaxLoggedGuiNames fails here. The per-frame focal-length cache
// is still modelled, because core's GetMarkerFocalLengths keys on a render
// frame counter this host build has no way to advance.
//
// Run: pixi run test

#include <cameraunlock/reframework/gui_elements.h>
#include <cameraunlock/reframework/log_callback.h>

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <atomic>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

// --- allocation counter (counts heap allocations from operator new) ---
static std::atomic<uint64_t> g_allocCount{0};
void* operator new(std::size_t n) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ============================================================================
// Change 1: GUI element-name dedup set
// ============================================================================

namespace ref = cameraunlock::reframework;

// The log callback is the shipped function's only output, so it is where the
// names it let through are read back from.
static std::vector<std::string> g_captured;

static void CaptureLog(ref::LogLevel level, const char* message) {
    if (level != ref::LogLevel::Info) return;
    g_captured.emplace_back(message);
}

static std::string NameFromLogLine(const std::string& line) {
    auto open = line.find('"');
    auto close = line.rfind('"');
    assert(open != std::string::npos && close > open && "log line must quote the element name");
    return line.substr(open + 1, close - open - 1);
}

// Drive the shipped dedup over a stream and return the names it logged.
static std::vector<std::string> RunShippedDedup(const std::vector<std::string>& stream,
                                                uint64_t* allocsDuringSteadyState) {
    g_captured.clear();
    ref::SetLogCallback(&CaptureLog);

    uint64_t startAllocs = 0;
    for (size_t i = 0; i < stream.size(); ++i) {
        // Steady state is the second half of the stream: every name in the
        // fixtures below has been seen by then, so nothing new is inserted.
        if (i == stream.size() / 2) startAllocs = g_allocCount.load();
        ref::LogGuiElementNameOnce(stream[i].c_str());
    }
    if (allocsDuringSteadyState) *allocsDuringSteadyState = g_allocCount.load() - startAllocs;

    ref::SetLogCallback(nullptr);

    std::vector<std::string> logged;
    for (const auto& line : g_captured) logged.push_back(NameFromLogLine(line));
    return logged;
}

// What the shipped dedup has to agree with: each distinct name the first time
// it is seen, in order, and nothing past the cap.
static std::vector<std::string> RunReferenceDedup(const std::vector<std::string>& stream,
                                                  size_t alreadyLogged) {
    std::vector<std::string> logged;
    std::unordered_set<std::string> seen;
    for (const auto& name : stream) {
        if (alreadyLogged + seen.size() >= ref::kMaxLoggedGuiNames) break;
        if (seen.insert(name).second) logged.push_back(name);
    }
    return logged;
}

// The dedup set lives in a function-local static, so it carries across every
// call in this process. Both tests below feed one continuous stream and track
// the running total against the cap.
static size_t g_totalLogged = 0;

static void TestDedupMatchesShippedBehaviour() {
    // A realistic GUI stream: a handful of distinct element names, repeated
    // every frame for many frames.
    const char* names[] = {
        "GUI_Reticle", "GUI_RemainingBullet", "GUI_FloatIcon", "GUI_Purpose",
        "GUI_MenuMain", "GUI_Pause", "GUI_TitleMenu", "GUI_HealthBar",
        "GUI_Subtitle", "GUI_Map", "GUI_Inventory", "GUI_Compass"
    };
    std::vector<std::string> stream;
    for (int frame = 0; frame < 500; ++frame)
        for (const char* n : names) stream.emplace_back(n);

    auto expected = RunReferenceDedup(stream, g_totalLogged);
    uint64_t steadyAllocs = 0;
    auto logged = RunShippedDedup(stream, &steadyAllocs);
    g_totalLogged += logged.size();

    assert(logged == expected &&
           "shipped dedup must log each distinct name once, in first-seen order");
    // Once every distinct name has been seen (well before the halfway point of
    // a 500-frame stream), the shipped path must do zero heap allocations.
    assert(steadyAllocs == 0 && "shipped dedup must not allocate in steady state");

    printf("  [dedup] unique names logged: %zu, steady-state allocs=%llu\n",
           logged.size(), (unsigned long long)steadyAllocs);
}

static void TestDedupCapsAtShippedLimit() {
    // Enough further distinct names to carry the process-wide set past the cap.
    std::vector<std::string> stream;
    for (size_t i = 0; i < ref::kMaxLoggedGuiNames + 50; ++i)
        stream.push_back("Elem_" + std::to_string(i));
    auto base = stream;
    for (int f = 0; f < 5; ++f) for (auto& s : base) stream.push_back(s);

    auto expected = RunReferenceDedup(stream, g_totalLogged);
    auto logged = RunShippedDedup(stream, nullptr);
    g_totalLogged += logged.size();

    assert(logged == expected && "shipped dedup must stop logging at the cap");
    assert(g_totalLogged == ref::kMaxLoggedGuiNames &&
           "shipped dedup must cap at kMaxLoggedGuiNames distinct names");
    printf("  [dedup-cap] capped at %zu distinct names (kMaxLoggedGuiNames=%zu)\n",
           g_totalLogged, ref::kMaxLoggedGuiNames);
}

// ============================================================================
// Change 2: per-frame focal-length cache
// ============================================================================
// Models the cache mechanics: compute-once-per-epoch, identical values for
// repeated calls within an epoch, recompute on epoch change.

struct FocalCache {
    uint64_t epoch = (uint64_t)-1;
    bool ok = false;
    float fx = 0.f, fy = 0.f;
};

static int g_computeCalls = 0;

// Returns the same value the uncached path would, but counts real computes.
static bool ComputeFocal(uint64_t frame, float& fx, float& fy) {
    g_computeCalls++;
    // Deterministic "projection-derived" focal lengths that vary per frame so we
    // can prove the cache returns the right frame's values.
    fx = 960.f + (float)(frame % 7);
    fy = 540.f + (float)(frame % 5);
    return true;
}

static bool GetFocalCached(FocalCache& c, uint64_t frame, float& fx, float& fy) {
    if (c.epoch == frame) {
        if (!c.ok) return false;
        fx = c.fx; fy = c.fy; return true;
    }
    c.epoch = frame;
    c.ok = false;
    if (ComputeFocal(frame, fx, fy)) {
        c.ok = true; c.fx = fx; c.fy = fy; return true;
    }
    return false;
}

static void TestFocalCacheEquivalence() {
    FocalCache cache;
    int frames = 300;
    int callsPerFrame = 4; // up to 4 matching GUI elements per frame

    g_computeCalls = 0;
    for (uint64_t f = 1; f <= (uint64_t)frames; ++f) {
        float refFx, refFy;
        bool refOk = ComputeFocal(f, refFx, refFy); // ground truth (uncached)
        for (int k = 0; k < callsPerFrame; ++k) {
            float fx, fy;
            bool ok = GetFocalCached(cache, f, fx, fy);
            assert(ok == refOk);
            assert(fx == refFx && fy == refFy &&
                   "cached focal lengths must byte-match the uncached compute for that frame");
        }
    }

    // Uncached would compute frames*callsPerFrame times; cached computes exactly
    // once per frame (the ground-truth ComputeFocal call above is the +frames).
    int cachedComputes = g_computeCalls - frames; // subtract the ground-truth calls
    assert(cachedComputes == frames &&
           "cache must collapse callsPerFrame computes down to one per frame");
    printf("  [focal-cache] %d frames x %d calls: uncached computes=%d, cached computes=%d (%.1fx fewer VM crossings)\n",
           frames, callsPerFrame, frames * callsPerFrame, cachedComputes,
           (double)(frames * callsPerFrame) / cachedComputes);
}

int main() {
#ifdef _MSC_VER
    // A failing assert in a Debug build otherwise opens a modal report box,
    // which blocks ctest on a CI runner rather than failing it. Reported to
    // stderr instead, so the process aborts with the message.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    printf("Running performance-equivalence tests...\n");
    TestDedupMatchesShippedBehaviour();
    TestDedupCapsAtShippedLimit();
    TestFocalCacheEquivalence();
    printf("All equivalence tests passed.\n");
    return 0;
}
