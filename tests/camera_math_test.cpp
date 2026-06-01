// Behaviour-locking tests for camera_math.h helpers.
//
// ApplyRotation3x3ToColumns was extracted from two byte-identical inline loops
// in ApplyHeadTracking (world-space pitch/roll branch and camera-local quaternion
// branch). This test asserts the extracted helper reproduces the ORIGINAL inline
// loop exactly, so the refactor cannot have changed the rendered rotation.
//
// Build:  g++ -std=c++20 -O2 tests/camera_math_test.cpp -o build/camera_math_test
// Run:    build/camera_math_test

#include <cassert>
#include <cstdio>
#include <cmath>

#include "../src/camera/camera_math.h"

using RE3HT::Matrix4x4f;
using RE3HT::REQuat;
using RE3HT::QuatToMatrix3x3;
using RE3HT::ApplyRotation3x3ToColumns;

// The exact loop that lived inline in ApplyHeadTracking before extraction.
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

static Matrix4x4f MakeMatrix(int seed) {
    Matrix4x4f m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m.m[i][j] = static_cast<float>(((seed * 31 + i * 7 + j * 13) % 19) - 9) * 0.5f;
    return m;
}

static void AssertMatricesEqual(const Matrix4x4f& a, const Matrix4x4f& b, const char* what) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            assert(a.m[i][j] == b.m[i][j] && what);
}

static void TestColumnMultiplyMatchesInline() {
    // A spread of rotation matrices (built from quaternions, as the real code does)
    // applied to a spread of world matrices. Byte-equality is the contract.
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
            Matrix4x4f viaHelper = MakeMatrix(seed);
            Matrix4x4f viaInline = viaHelper;  // identical starting state

            ApplyRotation3x3ToColumns(viaHelper, rot);
            OriginalInlineColumnMultiply(viaInline, rot);

            AssertMatricesEqual(viaHelper, viaInline,
                "extracted helper must byte-match the original inline column-multiply");
            cases++;
        }
    }
    printf("  [column-multiply] %d (quat x matrix) cases byte-identical to inline\n", cases);
}

static void TestTranslationRowUntouched() {
    // The helper must only touch the 3x3 rotation block, never the translation row.
    REQuat q = {0.f, 0.7071068f, 0.f, 0.7071068f};
    float rot[3][3];
    QuatToMatrix3x3(q, rot);

    Matrix4x4f m = MakeMatrix(3);
    float t0 = m.m[3][0], t1 = m.m[3][1], t2 = m.m[3][2], t3 = m.m[3][3];

    ApplyRotation3x3ToColumns(m, rot);

    assert(m.m[3][0] == t0 && m.m[3][1] == t1 && m.m[3][2] == t2 && m.m[3][3] == t3
           && "translation row must be untouched");
    printf("  [translation-row] preserved exactly through rotation apply\n");
}

int main() {
    printf("Running camera_math equivalence tests...\n");
    TestColumnMultiplyMatchesInline();
    TestTranslationRowUntouched();
    printf("All camera_math equivalence tests passed.\n");
    return 0;
}
