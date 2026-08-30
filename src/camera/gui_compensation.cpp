#include "pch.h"
#include "gui_compensation.h"
#include "game_state_detector.h"

#include <cameraunlock/reframework/camera_pipeline.h>
#include <cameraunlock/reframework/gui_elements.h>
#include <cameraunlock/reframework/log_callback.h>
#include <cameraunlock/reframework/managed_utils.h>
#include <cameraunlock/reframework/plugin_mod.h>
#include <cameraunlock/reframework/re_math.h>

#include <reframework/API.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

namespace RE3HT {

namespace ref = cameraunlock::reframework;

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

// Shift a GUI element's view to the head-tracked screen position of the clean
// aim point so the reticle and HUD stay locked to where the game is aiming
// while the head turns the view.
static void OffsetGuiElementToAimPoint(reframework::API::ManagedObject* mo, const char* name) {
    const auto& projection = ref::GetFrameProjection();

    float fx = 0.f, fy = 0.f;
    if (!ref::GetMarkerFocalLengths(fx, fy)) return;

    float deltaX = -projection.aimTanRight * fx;
    float deltaY =  projection.aimTanUp * fy;

    if (!ref::ShiftElementView(mo, deltaX, deltaY)) return;

    static std::unordered_set<std::string> s_diagNames;
    if (s_diagNames.size() < 8 && s_diagNames.insert(std::string(name)).second) {
        ref::LogInfo("Compensation applied to \"%s\": fov=%.1f tanR=%.4f tanU=%.4f dX=%.1f dY=%.1f",
            name, projection.fovDegrees, projection.aimTanRight, projection.aimTanUp, deltaX, deltaY);
    }
}

// Diagnostic: during gameplay, log every unique GUI element reaching the draw
// callback with its element type and descendant PlayObject count. RE Engine
// titles vary in whether the gameplay HUD/markers arrive as flat top-level
// elements (RE2: GUI_FloatIcon/GUI_Reticle) or as containers walked via
// findObjects (Requiem: Gui_ui2010 + children). This dump is what revealed
// RE3's real names and nesting, and is how a future build's changes would be
// caught.
static void DumpGuiStructure(reframework::API::ManagedObject* mo, const char* name) {
    static std::unordered_set<std::string> s_dumped;
    if (s_dumped.size() >= 80 || !s_dumped.insert(std::string(name)).second) return;

    const char* typeName = "?";
    auto td = mo->get_type_definition();
    if (td && td->get_name()) typeName = td->get_name();

    uint32_t descendants = 0;
    ref::FindPlayObjects(mo, descendants);

    ref::LogInfo("GUI gameplay element: \"%s\" type=%s descendants=%u", name, typeName, descendants);
}

// Pin a world-anchored marker (GUI_FloatIcon) to its world target. Read the
// marker's true anchor from child[1] "main" (a Panel) after zeroing the root
// View (so the read reflects the game's clean projection, not our prior offset,
// under the engine's several-draws-per-frame), reproject it through the head
// rotation, and shift the whole marker (the View) by the delta. Only the View
// and child[1] are touched - never the Text/Circle children whose
// get_GlobalPosition returns garbage and crashes the game.
//
// Rotation-only reprojection, with no lean term, and that is a known gap rather
// than a design choice.
//
// The post-render callback restores the clean camera in full, position row
// included, so the engine projects the anchor from the un-leaned eye while the
// frame was drawn from the leaned one. The rotation half of that difference is
// what this corrects, exactly, because the anchor's own ray is read rather than
// assumed. The translation half is lean/depth and needs the anchor's depth,
// which neither of RE3's two marker elements can supply: get_GlobalPosition is a
// canvas position, GUI_FloatIcon and GUI_Purpose have no near/far pair bounding
// their range, and correcting with an assumed depth d_a leaves
// f*lean*(1/d_true - 1/d_a), which only beats leaving it alone while
// d_a > d_true/2. The diagnostic below logs the live lean and the third
// component of the anchor read, which is where a real depth would have to come
// from.
static void OffsetWorldMarker(reframework::API::ManagedObject* mo, const char* name) {
    (void)name;
    const auto& projection = ref::GetFrameProjection();
    if (!projection.cleanToHeadValid) return;

    const auto& gui = ref::GetGuiMethods();
    if (!gui.ready || !gui.getGlobalPosition) return;

    float fx = 0.f, fy = 0.f;
    if (!ref::GetMarkerFocalLengths(fx, fy)) return;

    auto view = ref::GetElementView(mo);
    if (!view) return;

    // Zero our prior offset so the anchor read reflects the game's clean projection.
    ref::SetTransformPosition(view, 0.f, 0.f);

    uint32_t count = 0;
    auto arr = ref::FindPlayObjects(mo, count);
    if (!arr || count < 2) return;

    auto main = ref::ArrayGetValue(arr, 1);
    if (!main) return;
    float gx = 0.f, gy = 0.f;
    if (!ref::GetTransformGlobalPosition(main, gx, gy)) return;
    if (!std::isfinite(gx) || !std::isfinite(gy) || fabsf(gx) > 3000.f || fabsf(gy) > 2000.f) {
        return;
    }

    // Reproject the marker's clean-view anchor into the head-tracked view. The
    // sign convention (canvas X = -camera-right tangent, canvas Y = +camera-up
    // tangent) was derived from measured head-yaw/pitch sweeps, not guessed:
    // pure yaw produces near-zero vertical delta only with the -X mapping, and
    // pure pitch matches the verified centre-offset only with the +Y mapping.
    //
    // cleanToHead already carries head roll (it is R_head * R_clean^T), so the
    // roll argument is 0 - passing roll again would double-count it. The whole
    // thing round-trips to the anchor when the head is centred.
    float tcr = -(gx - ref::kHalfReferenceCanvasWidth)  / fx;
    float tcu =  (gy - ref::kHalfReferenceCanvasHeight) / fy;
    float guiX = 0.f, guiY = 0.f;
    if (!ref::ProjectCleanRayToHeadGui(projection.cleanToHead, 0.f, tcr, tcu, 1.f, fx, fy, guiX, guiY)) {
        return;
    }

    float deltaX = (ref::kHalfReferenceCanvasWidth  + guiX) - gx;
    float deltaY = (ref::kHalfReferenceCanvasHeight + guiY) - gy;
    ref::SetTransformPosition(view, deltaX, deltaY);

    // Capped: the 30-frame interval alone streams for the whole session,
    // which buries the startup chain a user is asked to send.
    static uint64_t s_lastLogFrame = 0;
    static int s_worldMarkerLogsLeft = 5;
    const uint64_t frame = ref::GetRenderFrame();
    if (s_worldMarkerLogsLeft > 0 && frame != s_lastLogFrame && (frame % 30) == 0) {
        s_lastLogFrame = frame;
        s_worldMarkerLogsLeft--;
        float yaw = 0.f, pitch = 0.f, roll = 0.f;
        ref::PluginMod::Instance().GetProcessedRotation(yaw, pitch, roll);
        float px = 0.f, py = 0.f, pz = 0.f;
        ref::PluginMod::Instance().GetPositionOffset(px, py, pz);
        // Third component of the anchor read. get_GlobalPosition returns a
        // vec3 and only x/y are used; if the engine writes the anchor's view
        // depth into z, that is the missing input for the lean term above, and
        // this line is what would say so.
        float anchorZ = 0.f;
        reframework::InvokeRet gpRet;
        if (ref::TryInvoke(gui.getGlobalPosition, main, gpRet)) {
            anchorZ = *reinterpret_cast<const float*>(&gpRet.bytes[8]);
        }
        const float* lean = projection.cleanLocalPositionDelta;
        ref::LogInfo("World marker yaw=%.1f pitch=%.1f posOff=(%.3f,%.3f,%.3f) "
            "lean=(%.3f,%.3f,%.3f) anchor=(%.0f,%.0f,%.3f) delta=(%.1f,%.1f)",
            yaw, pitch, px, py, pz, lean[0], lean[1], lean[2], gx, gy, anchorZ, deltaX, deltaY);
    }
}

bool OnPreGuiDrawElement(void* element, void* context) {
    (void)context;
    if (!ref::PluginMod::Instance().IsEnabled()) return true;

    // In this REFramework SDK version, 'element' is the GUI ManagedObject directly.
    if (!element) return true;

    auto mo = reinterpret_cast<reframework::API::ManagedObject*>(element);

    char goName[128] = {};
    if (!ref::ReadGuiElementName(mo, goName, sizeof(goName))) return true;
    ref::LogGuiElementNameOnce(goName);

    if (!IsInGameplay()) return true;
    DumpGuiStructure(mo, goName);

    if (ref::GetFrameProjection().aimValid) {
        if (IsWorldMarkerGuiElement(goName)) {
            OffsetWorldMarker(mo, goName);
        } else if (IsReticleGuiElement(goName)) {
            OffsetGuiElementToAimPoint(mo, goName);
        }
    }

    return true;
}

} // namespace RE3HT
