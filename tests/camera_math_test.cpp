// Characterization tests for the cameraunlock-core re_math.h functions that
// replaced this mod's local camera math (camera_math.h + inline blocks in
// camera_hook.cpp). Each test pins a core function against the ORIGINAL inline
// implementation copied verbatim from the pre-refactor code, so the refactor
// cannot have changed the rendered rotation, position, or crosshair projection.
//
// Build:  g++ -std=c++20 -O2 -I cameraunlock-core/cpp/include tests/camera_math_test.cpp -o build/camera_math_test
// Run:    build/camera_math_test

#include <cassert>
#include <cstdio>
#include <cmath>

#include <cameraunlock/reframework/re_math.h>

using cameraunlock::reframework::Matrix4x4f;
using cameraunlock::reframework::REQuat;
using cameraunlock::reframework::QuatToMatrix3x3;
using cameraunlock::reframework::PreMultiplyRotation3x3;
using cameraunlock::reframework::ApplyWorldSpaceHeadRotation;
using cameraunlock::reframework::ApplyCameraLocalHeadRotation;
using cameraunlock::reframework::ApplyViewSpacePositionOffset;
using cameraunlock::reframework::ProjectAimToViewTangents;

// ============================================================================
// Original implementations (pre-refactor), copied verbatim as the reference.
// ============================================================================

// The column-multiply loop from camera_math.h (ApplyRotation3x3ToColumns).
static void OriginalInlineColumnMultiply(Matrix4x4f& worldMat, const float r[3][3]) {
    for (int c = 0; c < 3; c++) {
        float c0 = worldMat.m[0][c];
        float c1 = worldMat.m[1][c];
        float c2 = worldMat.m[2][c];
        worldMat.m[0][c] = r[0][0]*c0 + r[0][1]*c1 + r[0][2]*c2;
        worldMat.m[1][c] = r[1][0]*c0 + r[1][1]*c1 + r[1][2]*c2;
        worldMat.m[2][c] = r[2][0]*c0 + r[2][1]*c1 + r[2][2]*c2;
    }
}

// QuatMul/QuatNorm from camera_math.h.
static REQuat OriginalQuatMul(const REQuat& a, const REQuat& b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

static REQuat OriginalQuatNorm(const REQuat& q) {
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l < 0.0001f) return {0,0,0,1};
    return {q.x/l, q.y/l, q.z/l, q.w/l};
}

// The world-space rotation block that lived inline in ApplyHeadTracking.
static void OriginalWorldSpaceRotation(Matrix4x4f& worldMat, float yr, float pr, float rr) {
    float cy = cosf(yr), sy = -sinf(yr);
    for (int r = 0; r < 3; r++) {
        float x = worldMat.m[r][0];
        float z = worldMat.m[r][2];
        worldMat.m[r][0] = x * cy - z * sy;
        worldMat.m[r][2] = x * sy + z * cy;
    }

    float cp = cosf(pr), sp = sinf(pr);
    float cr = cosf(rr), sr = sinf(rr);
    float prRot[3][3] = {
        { cr,      sr,      0   },
        {-cp*sr,   cp*cr,   sp  },
        { sp*sr,  -sp*cr,   cp  }
    };

    OriginalInlineColumnMultiply(worldMat, prRot);
}

// The camera-local quaternion rotation block that lived inline in ApplyHeadTracking.
static void OriginalCameraLocalRotation(Matrix4x4f& worldMat, float yr, float pr, float rr) {
    float hy = yr * 0.5f, hp = pr * 0.5f, hr = rr * 0.5f;
    REQuat qy = {0, sinf(hy), 0, cosf(hy)};
    REQuat qx = {sinf(hp), 0, 0, cosf(hp)};
    REQuat qz = {0, 0, sinf(hr), cosf(hr)};
    REQuat q = OriginalQuatNorm(OriginalQuatMul(OriginalQuatMul(qy, qx), qz));

    float headRot[3][3];
    QuatToMatrix3x3(q, headRot);

    OriginalInlineColumnMultiply(worldMat, headRot);
}

// The position-offset block that lived inline in ApplyHeadTracking.
static void OriginalPositionOffset(Matrix4x4f& worldMat, const Matrix4x4f& preRotationAxes,
                                   float px, float py, float pz) {
    px = -px;
    const Matrix4x4f& gm = preRotationAxes;
    worldMat.m[3][0] += px * gm.m[0][0] + py * gm.m[1][0] + pz * gm.m[2][0];
    worldMat.m[3][1] += px * gm.m[0][1] + py * gm.m[1][1] + pz * gm.m[2][1];
    worldMat.m[3][2] += px * gm.m[0][2] + py * gm.m[1][2] + pz * gm.m[2][2];
}

// The crosshair aim projection block that lived inline in OnPreBeginRendering.
static bool OriginalAimProjection(const Matrix4x4f& clean, const Matrix4x4f& head,
                                  float aimDist, float& tanRight, float& tanUp) {
    float aimPtX = clean.m[3][0] + aimDist * clean.m[2][0];
    float aimPtY = clean.m[3][1] + aimDist * clean.m[2][1];
    float aimPtZ = clean.m[3][2] + aimDist * clean.m[2][2];

    float dx = aimPtX - head.m[3][0];
    float dy = aimPtY - head.m[3][1];
    float dz = aimPtZ - head.m[3][2];

    float vx = dx * head.m[0][0] + dy * head.m[0][1] + dz * head.m[0][2];
    float vy = dx * head.m[1][0] + dy * head.m[1][1] + dz * head.m[1][2];
    float vz = dx * head.m[2][0] + dy * head.m[2][1] + dz * head.m[2][2];

    if (vz <= 1e-4f) return false;

    tanRight = vx / vz;
    tanUp = vy / vz;
    return true;
}

// ============================================================================
// Fixtures
// ============================================================================

static Matrix4x4f MakeMatrix(int seed) {
    Matrix4x4f m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m.m[i][j] = static_cast<float>(((seed * 31 + i * 7 + j * 13) % 19) - 9) * 0.5f;
    return m;
}

// Orthonormal basis from a quaternion so the rotation tests run on matrices
// shaped like real camera transforms.
static Matrix4x4f MakeRotationMatrix(int seed) {
    float a = 0.1f * seed, b = 0.07f * seed, c = 0.13f * seed;
    REQuat qy = {0, sinf(a), 0, cosf(a)};
    REQuat qx = {sinf(b), 0, 0, cosf(b)};
    REQuat qz = {0, 0, sinf(c), cosf(c)};
    REQuat q = OriginalQuatNorm(OriginalQuatMul(OriginalQuatMul(qy, qx), qz));
    float rot[3][3];
    QuatToMatrix3x3(q, rot);
    Matrix4x4f m{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = rot[i][j];
    m.m[3][0] = 1.5f * seed; m.m[3][1] = -0.5f * seed; m.m[3][2] = 2.0f * seed; m.m[3][3] = 1.f;
    return m;
}

static void AssertMatricesEqual(const Matrix4x4f& a, const Matrix4x4f& b, const char* what) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            assert(a.m[i][j] == b.m[i][j] && what);
}

static void AssertMatricesNear(const Matrix4x4f& a, const Matrix4x4f& b, float eps, const char* what) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            assert(fabsf(a.m[i][j] - b.m[i][j]) <= eps && what);
}

// ============================================================================
// Tests
// ============================================================================

static void TestColumnMultiplyMatchesInline() {
    const REQuat quats[] = {
        {0.f, 0.f, 0.f, 1.f},                 // identity
        {0.3826834f, 0.f, 0.f, 0.9238795f},   // 45deg pitch
        {0.f, 0.7071068f, 0.f, 0.7071068f},   // 90deg yaw
        {0.1830127f, 0.1830127f, 0.6830127f, 0.6830127f},
        {-0.5f, 0.5f, -0.5f, 0.5f},
    };

    int cases = 0;
    for (const auto& q : quats) {
        float rot[3][3];
        QuatToMatrix3x3(q, rot);
        for (int seed = 0; seed < 8; seed++) {
            Matrix4x4f viaCore = MakeMatrix(seed);
            Matrix4x4f viaInline = viaCore;

            PreMultiplyRotation3x3(viaCore, rot);
            OriginalInlineColumnMultiply(viaInline, rot);

            AssertMatricesEqual(viaCore, viaInline,
                "core PreMultiplyRotation3x3 must byte-match the original inline column-multiply");
            cases++;
        }
    }
    printf("  [column-multiply] %d (quat x matrix) cases byte-identical to inline\n", cases);
}

static void TestTranslationRowUntouched() {
    REQuat q = {0.f, 0.7071068f, 0.f, 0.7071068f};
    float rot[3][3];
    QuatToMatrix3x3(q, rot);

    Matrix4x4f m = MakeMatrix(3);
    float t0 = m.m[3][0], t1 = m.m[3][1], t2 = m.m[3][2], t3 = m.m[3][3];

    PreMultiplyRotation3x3(m, rot);

    assert(m.m[3][0] == t0 && m.m[3][1] == t1 && m.m[3][2] == t2 && m.m[3][3] == t3
           && "translation row must be untouched");
    printf("  [translation-row] preserved exactly through rotation apply\n");
}

static void TestWorldSpaceRotationMatchesInline() {
    const float angles[][3] = {
        {0.f, 0.f, 0.f},
        {0.5f, 0.f, 0.f},
        {0.f, -0.3f, 0.f},
        {0.f, 0.f, 0.2f},
        {0.7f, -0.4f, 0.15f},
        {-1.2f, 0.9f, -0.6f},
    };

    int cases = 0;
    for (const auto& a : angles) {
        for (int seed = 1; seed <= 6; seed++) {
            Matrix4x4f viaCore = MakeRotationMatrix(seed);
            Matrix4x4f viaInline = viaCore;

            ApplyWorldSpaceHeadRotation(viaCore, a[0], a[1], a[2]);
            OriginalWorldSpaceRotation(viaInline, a[0], a[1], a[2]);

            AssertMatricesEqual(viaCore, viaInline,
                "core ApplyWorldSpaceHeadRotation must byte-match the original inline block");
            cases++;
        }
    }
    printf("  [world-space] %d (angles x matrix) cases byte-identical to inline\n", cases);
}

static void TestCameraLocalRotationMatchesInline() {
    const float angles[][3] = {
        {0.f, 0.f, 0.f},
        {0.5f, 0.f, 0.f},
        {0.f, -0.3f, 0.f},
        {0.f, 0.f, 0.2f},
        {0.7f, -0.4f, 0.15f},
        {-1.2f, 0.9f, -0.6f},
    };

    // Core's QuatNorm uses multiply-by-reciprocal where the original divided by
    // length; results can differ in the last ULP, so this comparison is near,
    // not byte-exact.
    constexpr float kEps = 1e-5f;

    int cases = 0;
    for (const auto& a : angles) {
        for (int seed = 1; seed <= 6; seed++) {
            Matrix4x4f viaCore = MakeRotationMatrix(seed);
            Matrix4x4f viaInline = viaCore;

            ApplyCameraLocalHeadRotation(viaCore, a[0], a[1], a[2]);
            OriginalCameraLocalRotation(viaInline, a[0], a[1], a[2]);

            AssertMatricesNear(viaCore, viaInline, kEps,
                "core ApplyCameraLocalHeadRotation must match the original inline block");
            cases++;
        }
    }
    printf("  [camera-local] %d (angles x matrix) cases match inline within 1e-5\n", cases);
}

static void TestPositionOffsetMatchesInline() {
    const float offsets[][3] = {
        {0.f, 0.f, 0.f},
        {0.1f, -0.05f, 0.3f},
        {-0.25f, 0.2f, -0.1f},
    };

    int cases = 0;
    for (const auto& o : offsets) {
        for (int seed = 1; seed <= 6; seed++) {
            Matrix4x4f axes = MakeRotationMatrix(seed);
            Matrix4x4f viaCore = MakeRotationMatrix(seed + 10);
            Matrix4x4f viaInline = viaCore;

            ApplyViewSpacePositionOffset(viaCore, axes, o[0], o[1], o[2]);
            OriginalPositionOffset(viaInline, axes, o[0], o[1], o[2]);

            AssertMatricesEqual(viaCore, viaInline,
                "core ApplyViewSpacePositionOffset must byte-match the original inline block");
            cases++;
        }
    }
    printf("  [position-offset] %d (offset x matrix) cases byte-identical to inline\n", cases);
}

static void TestAimProjectionMatchesInline() {
    int cases = 0;
    for (int cleanSeed = 1; cleanSeed <= 5; cleanSeed++) {
        for (int headSeed = 1; headSeed <= 5; headSeed++) {
            Matrix4x4f clean = MakeRotationMatrix(cleanSeed);
            Matrix4x4f head = MakeRotationMatrix(headSeed);

            float coreTr = 0.f, coreTu = 0.f, origTr = 0.f, origTu = 0.f;
            bool coreOk = ProjectAimToViewTangents(clean, head, 50.f, coreTr, coreTu);
            bool origOk = OriginalAimProjection(clean, head, 50.f, origTr, origTu);

            assert(coreOk == origOk && "core projection validity must match original");
            if (coreOk) {
                assert(coreTr == origTr && coreTu == origTu &&
                       "core ProjectAimToViewTangents must byte-match the original inline block");
            }
            cases++;
        }
    }
    printf("  [aim-projection] %d (clean x head) cases byte-identical to inline\n", cases);
}

int main() {
    printf("Running camera math equivalence tests (core re_math.h vs original inline)...\n");
    TestColumnMultiplyMatchesInline();
    TestTranslationRowUntouched();
    TestWorldSpaceRotationMatchesInline();
    TestCameraLocalRotationMatchesInline();
    TestPositionOffsetMatchesInline();
    TestAimProjectionMatchesInline();
    printf("All camera math equivalence tests passed.\n");
    return 0;
}
