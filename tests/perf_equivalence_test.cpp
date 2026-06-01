// Behaviour-locking tests for the two performance changes in camera_hook.cpp.
//
// These replicate the ORIGINAL and OPTIMIZED logic for the two hot-path edits
// and assert they produce identical observable results, while measuring the
// allocation difference. The REFramework managed-invoke surface is unchanged by
// the edits, so it is out of scope here; what these lock is the pure logic that
// changed (the GUI-name dedup set, and the per-frame focal-length cache).
//
// Build:  g++ -std=c++20 -O2 tests/perf_equivalence_test.cpp -o build/perf_test
// Run:    build/perf_test

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <atomic>

// --- allocation counter (counts heap allocations from operator new) ---
static std::atomic<uint64_t> g_allocCount{0};
void* operator new(std::size_t n) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ============================================================================
// Change 1: GUI element-name dedup set (log each unique name once, up to 100)
// ============================================================================

// ORIGINAL: constructs a std::string for every call (heap alloc per call).
static std::vector<std::string> RunOriginalDedup(const std::vector<std::string>& stream) {
    std::vector<std::string> logged;
    std::unordered_set<std::string> seen;
    for (const auto& name : stream) {
        if (seen.size() < 100 && seen.insert(std::string(name)).second) {
            logged.push_back(name);
        }
    }
    return logged;
}

// OPTIMIZED: reused query buffer + find-then-insert; only allocates for genuinely
// new names. Mirrors the shipped edit in OnPreGuiDrawElement.
static std::vector<std::string> RunOptimizedDedup(const std::vector<std::string>& stream,
                                                  uint64_t* allocsDuringSteadyState) {
    std::vector<std::string> logged;
    std::unordered_set<std::string> seen;
    std::string query;
    bool steadyState = false;
    uint64_t startAllocs = 0;
    for (size_t i = 0; i < stream.size(); ++i) {
        // Mark "steady state" as the second half of the stream (all names already
        // seen by then in our test fixtures), and measure allocations there.
        if (!steadyState && i == stream.size() / 2) {
            steadyState = true;
            startAllocs = g_allocCount.load();
        }
        const std::string& name = stream[i];
        if (seen.size() < 100) {
            query.assign(name);
            if (seen.find(query) == seen.end()) {
                seen.insert(query);
                logged.push_back(name);
            }
        }
    }
    if (allocsDuringSteadyState) *allocsDuringSteadyState = g_allocCount.load() - startAllocs;
    return logged;
}

static void TestDedupEquivalence() {
    // A realistic GUI stream: a handful of distinct element names, repeated every
    // frame for many frames (steady state), interleaved.
    const char* names[] = {
        "GUI_Reticle", "GUI_RemainingBullet", "GUI_FloatIcon", "GUI_Purpose",
        "GUI_MenuMain", "GUI_Pause", "GUI_TitleMenu", "GUI_HealthBar",
        "GUI_Subtitle", "GUI_Map", "GUI_Inventory", "GUI_Compass"
    };
    std::vector<std::string> stream;
    for (int frame = 0; frame < 500; ++frame)
        for (const char* n : names) stream.emplace_back(n);

    auto orig = RunOriginalDedup(stream);
    uint64_t steadyAllocs = 0;
    auto opt = RunOptimizedDedup(stream, &steadyAllocs);

    assert(orig == opt && "Optimized dedup must log the exact same unique names in the same order");
    // Once every distinct name has been seen (well before the halfway point of a
    // 500-frame stream), the optimized path must do zero heap allocations.
    assert(steadyAllocs == 0 && "Optimized dedup must not allocate in steady state");

    printf("  [dedup] unique names logged: orig=%zu opt=%zu (identical), steady-state allocs=%llu\n",
           orig.size(), opt.size(), (unsigned long long)steadyAllocs);
}

static void TestDedupCapAt100() {
    // 150 distinct names: both must cap logging at exactly 100.
    std::vector<std::string> stream;
    for (int i = 0; i < 150; ++i) stream.push_back("Elem_" + std::to_string(i));
    // repeat to simulate multiple frames
    auto base = stream;
    for (int f = 0; f < 5; ++f) for (auto& s : base) stream.push_back(s);

    auto orig = RunOriginalDedup(stream);
    uint64_t dummy = 0;
    auto opt = RunOptimizedDedup(stream, &dummy);
    assert(orig == opt);
    assert(orig.size() == 100 && "must cap at 100 distinct names");
    printf("  [dedup-cap] both capped at %zu names (identical)\n", orig.size());
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
    printf("Running performance-equivalence tests...\n");
    TestDedupEquivalence();
    TestDedupCapAt100();
    TestFocalCacheEquivalence();
    printf("All equivalence tests passed.\n");
    return 0;
}
