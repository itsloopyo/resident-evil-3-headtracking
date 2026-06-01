#include "pch.h"
#include "camera_hook.h"
#include "camera_math.h"
#include "ref_utils.h"
#include "game_state_detector.h"
#include "core/mod.h"
#include "core/logger.h"

#include <reframework/API.hpp>
#include <cameraunlock/math/smoothing_utils.h>
#include <unordered_set>
#include <string>

namespace RE3HT {

constexpr int TX_WORLDMATRIX_OFFSET = 0x80;

// Half the 1920x1080 reference canvas the GUI compensation projects against.
// Focal lengths are expressed in pixels of that canvas.
constexpr float kHalfReferenceCanvasWidth  = 960.f;
constexpr float kHalfReferenceCanvasHeight = 540.f;

// Crosshair projection state, computed per frame in OnPreBeginRendering and
// consumed by the GUI draw callback to offset the reticle/HUD to the aim point.
struct CrosshairProjection {
    float tanRight = 0.0f;
    float tanUp = 0.0f;
    float fovDegrees = 75.0f;
    float rollDegrees = 0.0f;
    bool valid = false;
};

static CrosshairProjection g_crosshair;

// Saved game rotation — what the game INTENDED before we modified it
static struct {
    Matrix4x4f gameMatrix;     // The game's clean matrix (saved after game updates)
    bool hasGameMatrix = false;
} g_saved;

// Clean camera matrix saved at the start of each rendering frame
static struct {
    Matrix4x4f matrix;
    bool valid = false;
} g_cleanCameraMatrix;

static bool g_trackingAppliedThisFrame = false;

static struct {
    reframework::API::Method* getMainView = nullptr;
    reframework::API::Method* getPrimaryCamera = nullptr;
    reframework::API::Method* getGameObject = nullptr;
    reframework::API::Method* getTransform = nullptr;
    reframework::API::Method* getCameraFov = nullptr;
    reframework::API::Method* getCameraProjectionMatrix = nullptr;
    void* sceneManager = nullptr;
    bool initialized = false;
    bool failed = false;
} g_fn;

// GUI compensation method cache
static struct {
    reframework::API::Method* guiFindObjectsByType = nullptr;
    reframework::API::Method* transformSetPosition = nullptr;
    reframework::API::Method* transformGetGlobalPosition = nullptr;
    reframework::API::TypeDefinition* playObjectRuntimeType = nullptr;
    bool resolved = false;
    bool giveUp = false;
} g_guiCam;

// Per-frame transform cache
static struct {
    void* ptr = nullptr;
    ULONGLONG timeMs = 0;
} g_txCache;

// Bumped once per render frame at the top of OnPreBeginRendering. The GUI draw
// callbacks fire during that same frame's rendering, so this is a reliable
// per-frame key for caching values that are invariant within a frame.
static uint64_t g_frameEpoch = 0;

// Per-frame cache for the projection focal lengths. The camera's projection is
// fixed for the whole frame, but the GUI callback requests focal lengths once
// per matching element (reticle, bullet count, floating markers), each pulling
// the projection matrix across the managed VM. Compute once, reuse per frame.
static struct {
    uint64_t epoch = static_cast<uint64_t>(-1);
    bool ok = false;
    float fx = 0.f;
    float fy = 0.f;
} g_focalCache;

// Per-frame cache for the resolved primary-camera managed object. The active
// camera is invariant within a render frame, but both the FOV reader and the
// projection-matrix reader walk SceneManager -> MainView -> PrimaryCamera to
// find it. Resolve once per frame, reuse across both consumers. Only successful
// (non-null) resolves are cached, so a transient miss is retried, not poisoned.
static struct {
    uint64_t epoch = static_cast<uint64_t>(-1);
    reframework::API::ManagedObject* cam = nullptr;
} g_primaryCamCache;

static void* GetCameraTransform() {
    ULONGLONG now = GetTickCount64();
    if (g_txCache.ptr && now == g_txCache.timeMs) {
        return g_txCache.ptr;
    }

    if (!g_fn.sceneManager) {
        g_fn.sceneManager = reframework::API::get()->get_native_singleton("via.SceneManager");
        if (!g_fn.sceneManager) return nullptr;
    }
    auto mv = InvokePtr(g_fn.getMainView, g_fn.sceneManager);
    if (!mv) return nullptr;
    auto cam = InvokePtr(g_fn.getPrimaryCamera, mv);
    if (!cam) return nullptr;
    auto go = InvokePtr(g_fn.getGameObject, cam);
    if (!go) return nullptr;
    void* tx = InvokePtr(g_fn.getTransform, go);

    g_txCache.ptr = tx;
    g_txCache.timeMs = now;
    return tx;
}

// Resolve the active scene's primary camera as a managed object. Shared by the
// FOV and projection-matrix readers, which both walk SceneManager -> MainView
// -> PrimaryCamera before reading their respective property.
static reframework::API::ManagedObject* GetPrimaryCamera() {
    if (g_primaryCamCache.epoch == g_frameEpoch && g_primaryCamCache.cam) {
        return g_primaryCamCache.cam;
    }

    if (!g_fn.getMainView || !g_fn.getPrimaryCamera) return nullptr;

    void* sm = g_fn.sceneManager;
    if (!sm) sm = reframework::API::get()->get_native_singleton("via.SceneManager");
    if (!sm) return nullptr;

    auto mv = g_fn.getMainView->invoke(
        reinterpret_cast<reframework::API::ManagedObject*>(sm), EmptyArgs());
    if (mv.exception_thrown || !mv.ptr) return nullptr;

    auto cam = g_fn.getPrimaryCamera->invoke(
        reinterpret_cast<reframework::API::ManagedObject*>(mv.ptr), EmptyArgs());
    if (cam.exception_thrown || !cam.ptr) return nullptr;

    auto* result = reinterpret_cast<reframework::API::ManagedObject*>(cam.ptr);
    g_primaryCamCache.epoch = g_frameEpoch;
    g_primaryCamCache.cam = result;
    return result;
}

static float GetLivePrimaryCameraFov() {
    if (!g_fn.getCameraFov) return 0.f;

    auto cam = GetPrimaryCamera();
    if (!cam) return 0.f;

    auto fov = g_fn.getCameraFov->invoke(cam, EmptyArgs());
    if (fov.exception_thrown) return 0.f;

    float fovDeg = 0.f;
    if (fov.f >= 10.f && fov.f <= 170.f) fovDeg = fov.f;
    else { float fromD = static_cast<float>(fov.d); if (fromD >= 10.f && fromD <= 170.f) fovDeg = fromD; }
    return fovDeg;
}

static void ApplyHeadTracking(Matrix4x4f* worldMat) {
    float yaw, pitch, roll;
    if (!Mod::Instance().GetProcessedRotation(yaw, pitch, roll)) return;

    // Save the game's rotation axes BEFORE applying head rotation.
    Matrix4x4f preRotationAxes = *worldMat;

    if (Mod::Instance().IsRotationEnabled()) {
        float yr = -yaw * kDegToRad;
        float pr = pitch * kDegToRad;
        float rr = roll * kDegToRad;

        if (Mod::Instance().IsWorldSpaceYaw()) {
            float cy = cosf(yr), sy = -sinf(yr);
            for (int r = 0; r < 3; r++) {
                float x = worldMat->m[r][0];
                float z = worldMat->m[r][2];
                worldMat->m[r][0] = x * cy - z * sy;
                worldMat->m[r][2] = x * sy + z * cy;
            }

            float cp = cosf(pr), sp = sinf(pr);
            float cr = cosf(rr), sr = sinf(rr);
            float prRot[3][3] = {
                { cr,      sr,      0   },
                {-cp*sr,   cp*cr,   sp  },
                { sp*sr,  -sp*cr,   cp  }
            };

            ApplyRotation3x3ToColumns(*worldMat, prRot);
        } else {
            float hy = yr * 0.5f, hp = pr * 0.5f, hr = rr * 0.5f;
            REQuat qy = {0, sinf(hy), 0, cosf(hy)};
            REQuat qx = {sinf(hp), 0, 0, cosf(hp)};
            REQuat qz = {0, 0, sinf(hr), cosf(hr)};
            REQuat q = QuatNorm(QuatMul(QuatMul(qy, qx), qz));

            float headRot[3][3];
            QuatToMatrix3x3(q, headRot);

            ApplyRotation3x3ToColumns(*worldMat, headRot);
        }
    }

    // --- Position (6DOF) ---
    float px, py, pz;
    if (Mod::Instance().GetPositionOffset(px, py, pz)) {
        px = -px;
        const Matrix4x4f& gm = preRotationAxes;
        worldMat->m[3][0] += px * gm.m[0][0] + py * gm.m[1][0] + pz * gm.m[2][0];
        worldMat->m[3][1] += px * gm.m[0][1] + py * gm.m[1][1] + pz * gm.m[2][1];
        worldMat->m[3][2] += px * gm.m[0][2] + py * gm.m[1][2] + pz * gm.m[2][2];
    }
}

// --- Hook on camera controller update ---
static int CameraUpdatePreHook(int argc, void** argv, REFrameworkTypeDefinitionHandle* arg_tys, unsigned long long ret_addr) {
    if (!g_saved.hasGameMatrix || !Mod::Instance().IsEnabled()) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    void* transform = nullptr;
    __try { transform = GetCameraTransform(); } __except(EXCEPTION_EXECUTE_HANDLER) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (!transform) return REFRAMEWORK_HOOK_CALL_ORIGINAL;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + TX_WORLDMATRIX_OFFSET);
    __try {
        *worldMat = g_saved.gameMatrix;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

static void CameraUpdatePostHook(void** ret_val, REFrameworkTypeDefinitionHandle ret_ty, unsigned long long ret_addr) {
    void* transform = nullptr;
    __try { transform = GetCameraTransform(); } __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + TX_WORLDMATRIX_OFFSET);
    __try {
        g_saved.gameMatrix = *worldMat;
        g_saved.hasGameMatrix = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
        REQuat q = MatrixToQuat(g_saved.gameMatrix);
        Logger::Instance().Info("Hook save/restore active: gameQ=%.3f %.3f %.3f %.3f", q.x, q.y, q.z, q.w);
        s_loggedOnce = true;
    }
}

// --- GUI compensation ---

// Read focal lengths directly from the camera's projection matrix.
// P[0][0] = 1/tan(hFovX/2), P[1][1] = 1/tan(hFovY/2) in NDC.
// Multiply by half-canvas to get pixel focal lengths.
// No guessing about horizontal vs vertical FOV convention.
static bool GetFocalLengthsFromProjectionMatrix(float& fx, float& fy) {
    if (!g_fn.getCameraProjectionMatrix) return false;

    auto cam = GetPrimaryCamera();
    if (!cam) return false;

    // get_ProjectionMatrix is a property getter — no arguments, returns Matrix4x4
    auto ret = g_fn.getCameraProjectionMatrix->invoke(cam, EmptyArgs());
    if (ret.exception_thrown) return false;

    // Matrix4x4 (64 bytes) returned in ret.bytes — row-major [row][col]
    auto* retMat = reinterpret_cast<const float*>(ret.bytes.data());
    float p00 = retMat[0];   // m[0][0]
    float p11 = retMat[5];   // m[1][1]

    if (p00 < 0.1f || p00 > 20.f || p11 < 0.1f || p11 > 20.f) return false;

    fx = p00 * kHalfReferenceCanvasWidth;
    fy = p11 * kHalfReferenceCanvasHeight;

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        Logger::Instance().Info("Projection matrix focal lengths: P00=%.4f P11=%.4f fx=%.1f fy=%.1f", p00, p11, fx, fy);
    }
    return true;
}

static bool GetMarkerProjectionFocalLengths(float& fx, float& fy) {
    if (g_focalCache.epoch == g_frameEpoch) {
        if (!g_focalCache.ok) return false;
        fx = g_focalCache.fx;
        fy = g_focalCache.fy;
        return true;
    }
    g_focalCache.epoch = g_frameEpoch;
    g_focalCache.ok = false;

    // Prefer projection matrix — exact, no FOV convention guessing
    if (GetFocalLengthsFromProjectionMatrix(fx, fy)) {
        g_focalCache.ok = true;
        g_focalCache.fx = fx;
        g_focalCache.fy = fy;
        return true;
    }

    // Fallback to get_FOV (assume vertical for safety)
    constexpr float kAspect = kHalfReferenceCanvasWidth / kHalfReferenceCanvasHeight;
    float fov = GetLivePrimaryCameraFov();
    if (fov < 10.f || fov > 170.f) return false;
    float tanHFovY = tanf(fov * kDegToRad * 0.5f);
    fx = kHalfReferenceCanvasWidth / (tanHFovY * kAspect);
    fy = kHalfReferenceCanvasHeight / tanHFovY;
    g_focalCache.ok = true;
    g_focalCache.fx = fx;
    g_focalCache.fy = fy;
    return true;
}

static void InitGUICompensationMethods() {
    if (g_guiCam.resolved || g_guiCam.giveUp) return;
    g_guiCam.resolved = true;

    const auto& api = reframework::API::get();
    auto tdb = api->tdb();

    auto guiType = tdb->find_type("via.gui.GUI");
    auto transformType = tdb->find_type("via.gui.TransformObject");
    auto playObjType = tdb->find_type("via.gui.PlayObject");

    if (!guiType || !transformType || !playObjType) {
        Logger::Instance().Warning("GUI compensation: missing via.gui types");
        g_guiCam.giveUp = true;
        return;
    }

    g_guiCam.guiFindObjectsByType = guiType->find_method("findObjects(System.Type)");
    g_guiCam.transformSetPosition = transformType->find_method("set_Position");
    g_guiCam.transformGetGlobalPosition = transformType->find_method("get_GlobalPosition");

    auto runtimeType = playObjType->get_runtime_type();
    if (runtimeType) {
        g_guiCam.playObjectRuntimeType = reinterpret_cast<reframework::API::TypeDefinition*>(runtimeType);
    }

    bool ready = g_guiCam.guiFindObjectsByType && g_guiCam.transformSetPosition && g_guiCam.playObjectRuntimeType;
    if (!ready) {
        Logger::Instance().Warning("GUI compensation: some methods not found (findObjects=%p setPos=%p playObjRT=%p)",
            g_guiCam.guiFindObjectsByType, g_guiCam.transformSetPosition, g_guiCam.playObjectRuntimeType);
        g_guiCam.giveUp = true;
        return;
    }

    Logger::Instance().Info("GUI compensation methods resolved");
}

// --- OnPreGuiDrawElement callback ---
// Logs unique GUI element names for discovery, then applies marker/crosshair compensation.

// Read a managed string into a char buffer. Separated from the main callback
// to keep SEH (__try) out of functions with C++ destructors.
static bool ReadGuiElementName(void* guiMo, char* out, size_t outSize) {
    out[0] = 0;
    auto mo = reinterpret_cast<reframework::API::ManagedObject*>(guiMo);

    auto goRet = mo->invoke("get_GameObject", EmptyArgs());
    if (goRet.exception_thrown || !goRet.ptr) return false;

    auto goMo = reinterpret_cast<reframework::API::ManagedObject*>(goRet.ptr);
    auto nameRet = goMo->invoke("get_Name", EmptyArgs());
    if (nameRet.exception_thrown || !nameRet.ptr) return false;

    __try {
        auto* raw = reinterpret_cast<uint8_t*>(nameRet.ptr);
        uint32_t strLen = *reinterpret_cast<uint32_t*>(raw + 0x10);
        if (strLen > outSize - 1) strLen = static_cast<uint32_t>(outSize - 1);
        auto* chars = reinterpret_cast<uint16_t*>(raw + 0x14);
        for (uint32_t i = 0; i < strLen; i++) out[i] = (char)chars[i];
        out[strLen] = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }

    return out[0] != 0;
}

// Shift a GUI element's view to the head-tracked aim point so the reticle/HUD/
// world markers stay on the clean aim direction the game fires along. Shared by
// every compensated element; caller guarantees g_crosshair.valid, an installed
// transformSetPosition method, and active gameplay.
static void OffsetGuiElementToAimPoint(reframework::API::ManagedObject* mo) {
    float fx = 0.f, fy = 0.f;
    if (!GetMarkerProjectionFocalLengths(fx, fy)) return;

    float deltaX = -g_crosshair.tanRight * fx;
    float deltaY =  g_crosshair.tanUp * fy;

    auto viewRet = mo->invoke("get_View", EmptyArgs());
    if (viewRet.exception_thrown || !viewRet.ptr) return;

    auto view = reinterpret_cast<reframework::API::ManagedObject*>(viewRet.ptr);
    float pos[3] = { deltaX, deltaY, 0.f };
    void* setArgs[1] = { &pos[0] };
    g_guiCam.transformSetPosition->invoke(view, std::span<void*>(setArgs));

    static bool s_diagOnce = false;
    if (!s_diagOnce) {
        s_diagOnce = true;
        Logger::Instance().Info("Crosshair offset applied: fov=%.1f tanR=%.4f tanU=%.4f dX=%.1f dY=%.1f",
            g_crosshair.fovDegrees, g_crosshair.tanRight, g_crosshair.tanUp, deltaX, deltaY);
    }
}

// GUI elements whose view follows the aim point: world-anchored floating markers
// plus the reticle and bullet-count HUD. RE3 uses a simpler GUI hierarchy than
// requiem, so a single center-delta on the element's view covers all of them.
static bool IsAimCompensatedGuiElement(const char* name) {
    return strcmp(name, "GUI_FloatIcon") == 0
        || strcmp(name, "GUI_Purpose") == 0
        || strcmp(name, "GUI_Reticle") == 0
        || strcmp(name, "GUI_RemainingBullet") == 0;
}

bool OnPreGuiDrawElement(void* element, void* context) {
    if (!Mod::Instance().IsEnabled()) return true;

    // In this REFramework SDK version, 'element' is the GUI ManagedObject directly.
    if (!element) return true;

    char goName[128] = {};
    if (!ReadGuiElementName(element, goName, sizeof(goName))) return true;

    // Log unique GUI element names for discovery. This callback fires once per
    // GUI element per frame, so the lookup must not allocate in steady state:
    // assign into a reused buffer (no realloc once warmed) and only construct a
    // stored std::string when the name is genuinely new (a finite, small set).
    static std::unordered_set<std::string> s_loggedNames;
    static std::string s_nameQuery;
    if (s_loggedNames.size() < 100) {
        s_nameQuery.assign(goName);
        if (s_loggedNames.find(s_nameQuery) == s_loggedNames.end()) {
            s_loggedNames.insert(s_nameQuery);
            Logger::Instance().Info("GUI element: \"%s\"", goName);
        }
    }

    // Initialize GUI compensation methods on first element
    if (!g_guiCam.resolved && !g_guiCam.giveUp) {
        InitGUICompensationMethods();
    }

    auto mo = reinterpret_cast<reframework::API::ManagedObject*>(element);

    if (g_crosshair.valid && g_guiCam.transformSetPosition && IsAimCompensatedGuiElement(goName)) {
        if (!IsInGameplay()) return true;
        OffsetGuiElementToAimPoint(mo);
    }

    return true;
}

// --- Initialization ---

static bool InitCachedFunctions() {
    if (g_fn.initialized) return !g_fn.failed;
    g_fn.initialized = true;

    const auto& api = reframework::API::get();
    auto tdb = api->tdb();
    auto smType = tdb->find_type("via.SceneManager");
    auto svType = tdb->find_type("via.SceneView");
    auto camType = tdb->find_type("via.Camera");
    auto goType = tdb->find_type("via.GameObject");

    if (!smType || !svType || !camType || !goType) { g_fn.failed = true; return false; }

    g_fn.getMainView = smType->find_method("get_MainView");
    g_fn.getPrimaryCamera = svType->find_method("get_PrimaryCamera");
    g_fn.getGameObject = camType->find_method("get_GameObject");
    g_fn.getTransform = goType->find_method("get_Transform");
    g_fn.getCameraFov = camType->find_method("get_FOV");
    g_fn.getCameraProjectionMatrix = camType->find_method("get_ProjectionMatrix");
    if (!g_fn.getCameraProjectionMatrix)
        Logger::Instance().Warning("via.Camera.get_ProjectionMatrix not found — will fall back to get_FOV");

    if (!g_fn.getMainView || !g_fn.getPrimaryCamera || !g_fn.getGameObject || !g_fn.getTransform) {
        g_fn.failed = true;
        return false;
    }

    if (!g_fn.getCameraFov) {
        Logger::Instance().Warning("via.Camera.get_FOV not found — crosshair projection will use fallback FOV");
    }

    // Hook camera controller update for save/restore
    struct CameraHookCandidate {
        const char* typeName;
        const char* methodName;
    };

    CameraHookCandidate candidates[] = {
        // RE3 Remake uses the `offline` root namespace. updateCameraPosition is
        // the per-frame transform writer (matches REFramework's own camera hook).
        {"offline.camera.PlayerCameraController", "updateCameraPosition"},
        {"offline.camera.PlayerCameraController", "lateUpdate"},
        {"offline.camera.PlayerCameraController", "update"},
        {"offline.PlayerCameraController", "updateCameraPosition"},
        {"offline.PlayerCameraController", "update"},
        // RE2 Remake (app.ropeway) fallbacks — harmless when absent.
        {"app.ropeway.camera.PlayerCameraController", "updateCameraPosition"},
        {"app.ropeway.camera.CameraSystem", "update"},
        {"app.ropeway.PlayerCameraController", "update"},
    };

    bool hooked = false;
    for (const auto& candidate : candidates) {
        auto pccType = tdb->find_type(candidate.typeName);
        if (pccType) {
            auto method = pccType->find_method(candidate.methodName);
            if (method) {
                auto id = method->add_hook(CameraUpdatePreHook, CameraUpdatePostHook, false);
                Logger::Instance().Info("Hooked %s.%s (id=%u)", candidate.typeName, candidate.methodName, id);
                hooked = true;
                break;
            }
        }
    }

    // Namespace-agnostic fallback: scan the TDB for any PlayerCameraController and
    // hook its per-frame writer. Self-discovers the type if a future engine build
    // renames the namespace out from under the candidate list above.
    if (!hooked) {
        const char* methodNames[] = { "updateCameraPosition", "lateUpdate", "update" };
        auto numTypes = tdb->get_num_types();
        for (uint32_t i = 0; i < numTypes && !hooked; i++) {
            auto type = tdb->get_type(i);
            if (!type) continue;
            const char* name = type->get_name();
            if (!name || strcmp(name, "PlayerCameraController") != 0) continue;
            const char* ns = type->get_namespace();
            for (const char* mn : methodNames) {
                auto method = type->find_method(mn);
                if (method) {
                    auto id = method->add_hook(CameraUpdatePreHook, CameraUpdatePostHook, false);
                    Logger::Instance().Info("Hooked (auto) %s.%s.%s (id=%u)", ns ? ns : "", name, mn, id);
                    hooked = true;
                    break;
                }
            }
        }
    }

    if (!hooked) {
        Logger::Instance().Warning("Camera controller hook NOT installed — aim will couple to head view");
    }

    Logger::Instance().Info("Methods cached");
    return true;
}

// --- Pre-BeginRendering: apply head tracking for rendering ---
void OnPreBeginRendering() {
    g_frameEpoch++;
    if (!InitCachedFunctions()) return;
    if (!Mod::Instance().IsEnabled()) return;
    if (!IsInGameplay()) return;
    if (ShouldRecenter()) {
        Mod::Instance().Recenter();
    }

    void* transform = nullptr;
    __try { transform = GetCameraTransform(); } __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + TX_WORLDMATRIX_OFFSET);

    // Save the clean matrix before applying head tracking
    g_cleanCameraMatrix.matrix = *worldMat;
    g_cleanCameraMatrix.valid = true;

    ApplyHeadTracking(worldMat);
    g_trackingAppliedThisFrame = true;

    // Crosshair projection
    {
        const Matrix4x4f& clean = g_cleanCameraMatrix.matrix;
        const Matrix4x4f& head = *worldMat;

        constexpr float kAimDist = 50.0f;
        float aimPtX = clean.m[3][0] + kAimDist * clean.m[2][0];
        float aimPtY = clean.m[3][1] + kAimDist * clean.m[2][1];
        float aimPtZ = clean.m[3][2] + kAimDist * clean.m[2][2];

        float dx = aimPtX - head.m[3][0];
        float dy = aimPtY - head.m[3][1];
        float dz = aimPtZ - head.m[3][2];

        float vx = dx * head.m[0][0] + dy * head.m[0][1] + dz * head.m[0][2];
        float vy = dx * head.m[1][0] + dy * head.m[1][1] + dz * head.m[1][2];
        float vz = dx * head.m[2][0] + dy * head.m[2][1] + dz * head.m[2][2];

        if (vz > 1e-4f) {
            float rawTanRight = vx / vz;
            float rawTanUp = vy / vz;
            float liveFov = GetLivePrimaryCameraFov();
            float rawFov = (liveFov > 10.f) ? liveFov : g_crosshair.fovDegrees;

            // Smooth screen-space projection values to eliminate jitter from
            // perspective-division noise and per-frame FOV fluctuations.
            float dt = Mod::Instance().GetLastDeltaTime();
            constexpr float kCrosshairSmoothing = static_cast<float>(cameraunlock::math::kBaselineSmoothing);
            float t = cameraunlock::math::CalculateSmoothingFactor(kCrosshairSmoothing, dt);

            static float s_smoothedTanRight = 0.f;
            static float s_smoothedTanUp = 0.f;
            static float s_smoothedFov = 75.f;
            static bool s_initialized = false;

            if (!s_initialized) {
                s_smoothedTanRight = rawTanRight;
                s_smoothedTanUp = rawTanUp;
                s_smoothedFov = rawFov;
                s_initialized = true;
            } else {
                s_smoothedTanRight = cameraunlock::math::Lerp(s_smoothedTanRight, rawTanRight, t);
                s_smoothedTanUp = cameraunlock::math::Lerp(s_smoothedTanUp, rawTanUp, t);
                s_smoothedFov = cameraunlock::math::Lerp(s_smoothedFov, rawFov, t);
            }

            g_crosshair.tanRight = s_smoothedTanRight;
            g_crosshair.tanUp = s_smoothedTanUp;
            g_crosshair.fovDegrees = s_smoothedFov;
            g_crosshair.valid = g_crosshair.fovDegrees > 10.f;

            float roll = 0.f, yaw = 0.f, pitch = 0.f;
            Mod::Instance().GetProcessedRotation(yaw, pitch, roll);
            g_crosshair.rollDegrees = Mod::Instance().IsRotationEnabled() ? roll : 0.f;
        } else {
            g_crosshair.valid = false;
        }
    }
}

// Post-BeginRendering: restore clean ROTATION so aim direction follows the
// mouse, but keep head-tracked POSITION so the aim origin matches the lean.
// The GUI camera captures position during rendering, shifting the reticle.
// If we also restore clean position, bullets fire from a different origin
// than what the reticle shows → shots miss where the reticle points.
void OnPostBeginRendering() {
    if (!g_trackingAppliedThisFrame) return;
    g_trackingAppliedThisFrame = false;

    if (!g_cleanCameraMatrix.valid) return;

    void* transform = nullptr;
    __try { transform = GetCameraTransform(); } __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + TX_WORLDMATRIX_OFFSET);
    __try {
        // Save head-tracked position before restoring
        float hx = worldMat->m[3][0];
        float hy = worldMat->m[3][1];
        float hz = worldMat->m[3][2];

        // Restore clean rotation (3x3) + clean row 3 w component
        *worldMat = g_cleanCameraMatrix.matrix;

        // Re-apply head-tracked position so aim origin matches lean
        worldMat->m[3][0] = hx;
        worldMat->m[3][1] = hy;
        worldMat->m[3][2] = hz;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace RE3HT
