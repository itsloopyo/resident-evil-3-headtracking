#include "pch.h"
#include "camera_hook.h"
#include "math_types.h"
#include "game_state_detector.h"
#include "core/mod.h"
#include "core/logger.h"

#include <reframework/API.hpp>
#include <cameraunlock/math/smoothing_utils.h>
#include <cameraunlock/reframework/camera_chain.h>
#include <cameraunlock/reframework/camera_controller_hook.h>
#include <cameraunlock/reframework/managed_utils.h>
#include <cameraunlock/rendering/gui_marker_compensation.h>
#include <unordered_set>
#include <string>
#include <cmath>

namespace RE3HT {

namespace ref = cameraunlock::reframework;

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

// Head-tracked camera matrix for the same frame, captured right after head
// tracking is applied. World-anchored markers reproject their own clean-view
// screen anchor through the clean -> head rotation to stay pinned in yaw.
static struct {
    Matrix4x4f matrix;
    bool valid = false;
} g_headCameraMatrix;

static bool g_trackingAppliedThisFrame = false;

static ref::CameraTransformResolver g_cameraResolver;

// via.Camera.get_ProjectionMatrix — not part of the standard chain, resolved
// separately for exact focal-length reads.
static reframework::API::Method* g_getProjectionMatrix = nullptr;

// Per-frame camera/transform cache. Invalidated together at the
// camera-controller update pre-hook and at the end of OnPostBeginRendering,
// so within a single render frame they hold the live primary camera and its
// transform without re-walking the SceneManager chain.
static void* g_cachedTransform = nullptr;
static void* g_cachedCamera = nullptr;

static void* GetCameraTransformCached() {
    if (g_cachedTransform) return g_cachedTransform;
    g_cachedTransform = g_cameraResolver.ResolveTransform(&g_cachedCamera);
    return g_cachedTransform;
}

// GUI compensation method cache
static struct {
    reframework::API::Method* guiFindObjectsByType = nullptr;
    reframework::API::Method* transformSetPosition = nullptr;
    reframework::API::Method* transformGetGlobalPosition = nullptr;
    reframework::API::TypeDefinition* playObjectRuntimeType = nullptr;
    bool resolved = false;
    bool giveUp = false;

    // Instance getters invoked per GUI element per frame, resolved once via
    // InvokeCached. via.gui elements share these on their common base type, so
    // a Method* resolved from one element dispatches correctly on every element.
    reframework::API::Method* elemGetGameObject = nullptr;  // <element>.get_GameObject
    reframework::API::Method* getGameObjectName = nullptr;  // via.GameObject.get_Name
    reframework::API::Method* elemGetView = nullptr;        // <element>.get_View
} g_guiCam;

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

static void ApplyHeadTracking(Matrix4x4f* worldMat) {
    float yaw, pitch, roll;
    if (!Mod::Instance().GetProcessedRotation(yaw, pitch, roll)) return;

    // Save the game's rotation axes BEFORE applying head rotation.
    Matrix4x4f preRotationAxes = *worldMat;

    float yr = -yaw * kDegToRad;
    float pr = pitch * kDegToRad;
    float rr = roll * kDegToRad;

    if (Mod::Instance().IsWorldSpaceYaw()) {
        ApplyWorldSpaceHeadRotation(*worldMat, yr, pr, rr);
    } else {
        ApplyCameraLocalHeadRotation(*worldMat, yr, pr, rr);
    }

    float px, py, pz;
    if (Mod::Instance().GetPositionOffset(px, py, pz)) {
        ApplyViewSpacePositionOffset(*worldMat, preRotationAxes, px, py, pz);
    }
}

// --- Hook on camera controller update ---
static int CameraUpdatePreHook(int argc, void** argv, REFrameworkTypeDefinitionHandle* arg_tys, unsigned long long ret_addr) {
    g_cachedTransform = nullptr;
    g_cachedCamera = nullptr;

    if (!g_saved.hasGameMatrix || !Mod::Instance().IsEnabled()) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    void* transform = GetCameraTransformCached();
    if (!transform) return REFRAMEWORK_HOOK_CALL_ORIGINAL;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + ref::kTransformWorldMatrixOffset);
    __try {
        *worldMat = g_saved.gameMatrix;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

static void CameraUpdatePostHook(void** ret_val, REFrameworkTypeDefinitionHandle ret_ty, unsigned long long ret_addr) {
    void* transform = GetCameraTransformCached();
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + ref::kTransformWorldMatrixOffset);
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

// --- Camera controller discovery ---

// RE3 Remake's game code lives under the `offline` root namespace.
// updateCameraPosition is the per-frame transform writer (matches REFramework's
// own camera hook); the hooker's shared method-name list tries it first for
// these types. Namespace changes and unknown controllers fall back to the
// hooker's TDB short-name scan and parent-chain walk.
static const char* const kControllerTypeCandidates[] = {
    "offline.camera.PlayerCameraController",
    "offline.PlayerCameraController",
};

static ref::CameraControllerHooker g_controllerHooker{
    kControllerTypeCandidates,
    static_cast<int>(std::size(kControllerTypeCandidates)),
    CameraUpdatePreHook,
    CameraUpdatePostHook};

// Retry camera-controller discovery each gameplay frame until it succeeds.
// The candidate fast path normally hooks at plugin init (see
// InitCachedFunctions); this adds the parent-chain walk, which needs a live
// gameplay camera rig to inspect.
static void EnsureCameraControllerHooked() {
    if (g_controllerHooker.IsHooked()) return;
    if (g_controllerHooker.TryHook(GetCameraTransformCached())) return;

    int attempts = g_controllerHooker.AttemptCount();
    if (attempts == 1 || (attempts % 300) == 0) {
        Logger::Instance().Warning(
            "Camera controller hook not yet found (attempt %d) - head tracking "
            "still active via the BeginRendering restore path", attempts);
    }
}

// --- GUI compensation ---

// Read focal lengths directly from the camera's projection matrix.
// P[0][0] = 1/tan(hFovX/2), P[1][1] = 1/tan(hFovY/2) in NDC.
// Multiply by half-canvas to get pixel focal lengths.
// No guessing about horizontal vs vertical FOV convention.
static bool GetFocalLengthsFromProjectionMatrix(float& fx, float& fy) {
    if (!g_getProjectionMatrix) return false;

    void* cam = g_cachedCamera ? g_cachedCamera : g_cameraResolver.ResolveCamera();
    if (!cam) return false;

    // get_ProjectionMatrix is a property getter — no arguments, returns Matrix4x4
    auto ret = g_getProjectionMatrix->invoke(
        reinterpret_cast<reframework::API::ManagedObject*>(cam), ref::EmptyArgs());
    if (ret.exception_thrown) return false;

    // Matrix4x4 (64 bytes) returned in ret.bytes — row-major [row][col]
    auto* retMat = reinterpret_cast<const float*>(ret.bytes.data());
    float p00 = retMat[0];   // m[0][0]
    float p11 = retMat[5];   // m[1][1]

    if (!cameraunlock::rendering::FocalLengthsFromProjection(
            p00, p11, kHalfReferenceCanvasWidth, kHalfReferenceCanvasHeight, fx, fy)) {
        return false;
    }

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
    float fov = g_cameraResolver.ResolveFovDegrees(g_cachedCamera);
    if (!cameraunlock::rendering::FocalLengthsFromVerticalFov(
            fov, kHalfReferenceCanvasWidth, kHalfReferenceCanvasHeight, fx, fy)) {
        return false;
    }
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

// Resolve a GUI element's GameObject name into a char buffer.
static bool ReadGuiElementName(reframework::API::ManagedObject* mo, char* out, size_t outSize) {
    out[0] = 0;

    auto goRet = ref::InvokeCached(mo, g_guiCam.elemGetGameObject, "get_GameObject", ref::EmptyArgs());
    if (goRet.exception_thrown || !goRet.ptr) return false;

    auto goMo = reinterpret_cast<reframework::API::ManagedObject*>(goRet.ptr);
    auto nameRet = ref::InvokeCached(goMo, g_guiCam.getGameObjectName, "get_Name", ref::EmptyArgs());
    if (nameRet.exception_thrown || !nameRet.ptr) return false;

    ref::ReadManagedString(nameRet.ptr, out, outSize);
    return out[0] != 0;
}

// Shift a GUI element's view to the head-tracked screen position of the clean
// aim point so the reticle, HUD, and world-anchored markers stay locked to where
// the game is aiming while the head turns the view. Shared by every compensated
// element; caller guarantees g_crosshair.valid, an installed transformSetPosition
// method, and active gameplay.
static void OffsetGuiElementToAimPoint(reframework::API::ManagedObject* mo, const char* name) {
    float fx = 0.f, fy = 0.f;
    if (!GetMarkerProjectionFocalLengths(fx, fy)) return;

    float deltaX = -g_crosshair.tanRight * fx;
    float deltaY =  g_crosshair.tanUp * fy;

    auto viewRet = ref::InvokeCached(mo, g_guiCam.elemGetView, "get_View", ref::EmptyArgs());
    if (viewRet.exception_thrown || !viewRet.ptr) return;

    auto view = reinterpret_cast<reframework::API::ManagedObject*>(viewRet.ptr);
    float pos[3] = { deltaX, deltaY, 0.f };
    ref::InvokeMethodWithArg(g_guiCam.transformSetPosition, view, &pos[0]);

    static std::unordered_set<std::string> s_diagNames;
    if (s_diagNames.size() < 8 && s_diagNames.insert(std::string(name)).second) {
        Logger::Instance().Info("Compensation applied to \"%s\": fov=%.1f tanR=%.4f tanU=%.4f dX=%.1f dY=%.1f",
            name, g_crosshair.fovDegrees, g_crosshair.tanRight, g_crosshair.tanUp, deltaX, deltaY);
    }
}

// Diagnostic: during gameplay, log every unique GUI element reaching the draw
// callback with its element type and descendant PlayObject count. RE Engine
// titles vary in whether the gameplay HUD/markers arrive as flat top-level
// elements (RE2: GUI_FloatIcon/GUI_Reticle) or as containers walked via
// findObjects (RE9: Gui_ui2010 + children). RE3's structure is unknown until a
// session runs with the HUD on screen; this dump reveals the real names and
// nesting so compensation can target the correct elements.
static void DumpGuiStructure(reframework::API::ManagedObject* mo, const char* name) {
    static std::unordered_set<std::string> s_dumped;
    if (s_dumped.size() >= 80 || !s_dumped.insert(std::string(name)).second) return;

    const char* typeName = "?";
    auto td = mo->get_type_definition();
    if (td && td->get_name()) typeName = td->get_name();

    uint32_t descendants = 0;
    if (g_guiCam.guiFindObjectsByType && g_guiCam.playObjectRuntimeType) {
        std::vector<void*> findArgs = { (void*)g_guiCam.playObjectRuntimeType };
        auto arrRet = g_guiCam.guiFindObjectsByType->invoke(mo, findArgs);
        if (!arrRet.exception_thrown && arrRet.ptr) {
            auto arr = reinterpret_cast<reframework::API::ManagedObject*>(arrRet.ptr);
            auto lenRet = arr->invoke("get_Length", ref::EmptyArgs());
            if (!lenRet.exception_thrown) descendants = lenRet.dword;
        }
    }

    Logger::Instance().Info("GUI gameplay element: \"%s\" type=%s descendants=%u", name, typeName, descendants);
}

// Pin a world-anchored marker (GUI_FloatIcon) to its world target. Read the
// marker's true anchor from child[1] "main" (a Panel) after zeroing the root View
// (so the read reflects the game's clean projection, not our prior offset, under
// the engine's several-draws-per-frame), reproject it through the head rotation,
// and shift the whole marker (the View) by the delta. Only the View and child[1]
// are touched - never the Text/Circle children whose get_GlobalPosition returns
// garbage and crashes the game.
static void OffsetWorldMarker(reframework::API::ManagedObject* mo, const char* name) {
    if (!g_headCameraMatrix.valid || !g_cleanCameraMatrix.valid) return;
    if (!g_guiCam.transformSetPosition || !g_guiCam.transformGetGlobalPosition
        || !g_guiCam.guiFindObjectsByType || !g_guiCam.playObjectRuntimeType) {
        return;
    }

    float fx = 0.f, fy = 0.f;
    if (!GetMarkerProjectionFocalLengths(fx, fy)) return;

    // Square pixels: the pixel focal length is identical on both axes. RE3's
    // projection matrix reports P00 inconsistent with P11 (fx ends up half of fy,
    // a non-16:9 FOV ratio), which under-compensated yaw by 2x and drifted the
    // marker horizontally while pitch (which uses fy) stayed glued. fy is the
    // verified-correct value, so use it for both axes.
    fx = fy;

    auto viewRet = ref::InvokeCached(mo, g_guiCam.elemGetView, "get_View", ref::EmptyArgs());
    if (viewRet.exception_thrown || !viewRet.ptr) return;
    auto view = reinterpret_cast<reframework::API::ManagedObject*>(viewRet.ptr);

    // Zero our prior offset so the anchor read reflects the game's clean projection.
    float zero[3] = { 0.f, 0.f, 0.f };
    ref::InvokeMethodWithArg(g_guiCam.transformSetPosition, view, &zero[0]);

    std::vector<void*> findArgs = { (void*)g_guiCam.playObjectRuntimeType };
    auto arrRet = g_guiCam.guiFindObjectsByType->invoke(mo, findArgs);
    if (arrRet.exception_thrown || !arrRet.ptr) return;
    auto arr = reinterpret_cast<reframework::API::ManagedObject*>(arrRet.ptr);
    auto lenRet = arr->invoke("get_Length", ref::EmptyArgs());
    if (lenRet.exception_thrown || lenRet.dword < 2) return;

    auto main = ref::ArrayGetValue(arr, 1);
    if (!main) return;
    auto gpRet = g_guiCam.transformGetGlobalPosition->invoke(main, ref::EmptyArgs());
    if (gpRet.exception_thrown) return;
    float gx = *reinterpret_cast<const float*>(&gpRet.bytes[0]);
    float gy = *reinterpret_cast<const float*>(&gpRet.bytes[4]);
    if (!std::isfinite(gx) || !std::isfinite(gy) || fabsf(gx) > 3000.f || fabsf(gy) > 2000.f) {
        return;
    }

    // Reproject the marker's clean-view anchor into the head-tracked view. The
    // sign convention (canvas X = -camera-right tangent, canvas Y = +camera-up
    // tangent) was derived from measured head-yaw/pitch sweeps, not guessed:
    // pure yaw produces near-zero vertical delta only with the -X mapping, and
    // pure pitch matches the verified centre-offset only with the +Y mapping.
    const Matrix4x4f& clean = g_cleanCameraMatrix.matrix;
    const Matrix4x4f& head = g_headCameraMatrix.matrix;

    float tcr = -(gx - kHalfReferenceCanvasWidth)  / fx;
    float tcu =  (gy - kHalfReferenceCanvasHeight) / fy;

    float wx = clean.m[2][0] + tcr * clean.m[0][0] + tcu * clean.m[1][0];
    float wy = clean.m[2][1] + tcr * clean.m[0][1] + tcu * clean.m[1][1];
    float wz = clean.m[2][2] + tcr * clean.m[0][2] + tcu * clean.m[1][2];

    float vx = wx * head.m[0][0] + wy * head.m[0][1] + wz * head.m[0][2];
    float vy = wx * head.m[1][0] + wy * head.m[1][1] + wz * head.m[1][2];
    float vz = wx * head.m[2][0] + wy * head.m[2][1] + wz * head.m[2][2];
    if (vz <= 1e-4f) return;

    float newCanvasX = kHalfReferenceCanvasWidth  - (vx / vz) * fx;
    float newCanvasY = kHalfReferenceCanvasHeight + (vy / vz) * fy;

    float delta[3] = { newCanvasX - gx, newCanvasY - gy, 0.f };
    ref::InvokeMethodWithArg(g_guiCam.transformSetPosition, view, &delta[0]);

    static uint64_t s_lastLogEpoch = 0;
    if (g_frameEpoch != s_lastLogEpoch && (g_frameEpoch % 30) == 0) {
        s_lastLogEpoch = g_frameEpoch;
        float yaw = 0.f, pitch = 0.f, roll = 0.f;
        Mod::Instance().GetProcessedRotation(yaw, pitch, roll);
        Logger::Instance().Info("World marker yaw=%.1f pitch=%.1f anchor=(%.0f,%.0f) delta=(%.1f,%.1f)",
            yaw, pitch, gx, gy, delta[0], delta[1]);
    }
}

// The reticle and bullet-count HUD sit at screen centre on the clean aim
// direction, so a single centre delta is exactly right for them.
static bool IsReticleGuiElement(const char* name) {
    return strcmp(name, "GUI_Reticle") == 0
        || strcmp(name, "GUI_RemainingBullet") == 0;
}

// World-anchored floating markers are off centre, so they reproject their own
// screen anchor through the head rotation rather than taking the centre delta.
static bool IsWorldMarkerGuiElement(const char* name) {
    return strcmp(name, "GUI_FloatIcon") == 0
        || strcmp(name, "GUI_Purpose") == 0;
}

bool OnPreGuiDrawElement(void* element, void* context) {
    if (!Mod::Instance().IsEnabled()) return true;

    // In this REFramework SDK version, 'element' is the GUI ManagedObject directly.
    if (!element) return true;

    auto mo = reinterpret_cast<reframework::API::ManagedObject*>(element);

    char goName[128] = {};
    if (!ReadGuiElementName(mo, goName, sizeof(goName))) return true;

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

    if (IsInGameplay()) {
        DumpGuiStructure(mo, goName);
    }

    if (g_crosshair.valid && g_guiCam.transformSetPosition && IsInGameplay()) {
        if (IsWorldMarkerGuiElement(goName)) {
            OffsetWorldMarker(mo, goName);
        } else if (IsReticleGuiElement(goName)) {
            OffsetGuiElementToAimPoint(mo, goName);
        }
    }

    return true;
}

// --- Initialization ---

static bool InitCachedFunctions() {
    static bool s_attempted = false;
    if (s_attempted) return !g_cameraResolver.HasFailed();
    s_attempted = true;

    if (!g_cameraResolver.Initialize()) return false;

    auto tdb = reframework::API::get()->tdb();
    auto camType = tdb->find_type("via.Camera");
    g_getProjectionMatrix = camType ? camType->find_method("get_ProjectionMatrix") : nullptr;
    if (!g_getProjectionMatrix) {
        Logger::Instance().Warning("via.Camera.get_ProjectionMatrix not found - will fall back to get_FOV");
    }

    // Hook the camera controller from the candidate / TDB short-name fast
    // paths right away (the offline.* types exist in the TDB before gameplay
    // starts). The parent-chain walk needs a live camera rig, so it runs from
    // EnsureCameraControllerHooked during gameplay if this misses.
    if (!g_controllerHooker.TryHook(nullptr)) {
        Logger::Instance().Warning("Camera controller hook not installed at init - retrying during gameplay");
    }

    Logger::Instance().Info("Methods cached");
    return true;
}

// Project the clean aim direction into the head-tracked view to derive the
// screen-space tangents (and live FOV) the GUI compensation reads. The smoothed
// state persists across frames to suppress perspective-division and per-frame
// FOV jitter.
static void UpdateCrosshairProjection(const Matrix4x4f& clean, const Matrix4x4f& head) {
    constexpr float kAimDist = 50.0f;
    float rawTanRight = 0.f, rawTanUp = 0.f;
    if (!ProjectAimToViewTangents(clean, head, kAimDist, rawTanRight, rawTanUp)) {
        g_crosshair.valid = false;
        return;
    }

    float rawFov = g_cameraResolver.ResolveFovDegrees(g_cachedCamera);
    if (rawFov <= 10.f) rawFov = g_crosshair.fovDegrees;

    // Smooth screen-space projection values to eliminate jitter from
    // perspective-division noise and per-frame FOV fluctuations.
    float dt = Mod::Instance().GetLastDeltaTime();
    constexpr float kCrosshairSmoothing = static_cast<float>(cameraunlock::math::kBaselineSmoothing);

    static cameraunlock::math::SmoothedFloat s_tanRight;
    static cameraunlock::math::SmoothedFloat s_tanUp;
    static cameraunlock::math::SmoothedFloat s_fov;

    g_crosshair.tanRight = s_tanRight.Update(rawTanRight, kCrosshairSmoothing, dt);
    g_crosshair.tanUp = s_tanUp.Update(rawTanUp, kCrosshairSmoothing, dt);
    g_crosshair.fovDegrees = s_fov.Update(rawFov, kCrosshairSmoothing, dt);
    g_crosshair.valid = g_crosshair.fovDegrees > 10.f;
}

// --- Pre-BeginRendering: apply head tracking for rendering ---
void OnPreBeginRendering() {
    // Drain hotkey requests on the render thread so recenter / mode-cycle
    // never mutate session state concurrently with the pipeline tick below.
    Mod::Instance().ProcessDeferredActions();

    if (!InitCachedFunctions()) return;
    if (!Mod::Instance().IsEnabled()) return;
    if (!IsInGameplay()) return;
    EnsureCameraControllerHooked();
    g_frameEpoch++;
    if (ShouldRecenter()) {
        Mod::Instance().Recenter();
    }

    // Advance interpolation + smoothing once per render frame. Every
    // downstream consumer (ApplyHeadTracking, crosshair projection, GUI
    // compensation) reads cached values from this tick.
    Mod::Instance().TickFrame();

    void* transform = GetCameraTransformCached();
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + ref::kTransformWorldMatrixOffset);

    // Save the clean matrix before applying head tracking
    g_cleanCameraMatrix.matrix = *worldMat;
    g_cleanCameraMatrix.valid = true;

    ApplyHeadTracking(worldMat);
    g_trackingAppliedThisFrame = true;

    g_headCameraMatrix.matrix = *worldMat;
    g_headCameraMatrix.valid = true;

    UpdateCrosshairProjection(g_cleanCameraMatrix.matrix, *worldMat);
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

    // OnPreBeginRendering populated the per-frame transform cache this frame
    // (g_trackingAppliedThisFrame is only set after that succeeded), so reuse
    // it rather than re-walking the SceneManager chain.
    void* transform = GetCameraTransformCached();
    if (!transform) return;

    Matrix4x4f* worldMat = reinterpret_cast<Matrix4x4f*>(
        reinterpret_cast<uint8_t*>(transform) + ref::kTransformWorldMatrixOffset);
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

    g_cachedTransform = nullptr;
    g_cachedCamera = nullptr;
}

} // namespace RE3HT
