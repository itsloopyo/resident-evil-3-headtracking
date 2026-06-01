#pragma once

namespace RE3HT {

// Called from plugin_main's pre-BeginRendering callback
void OnPreBeginRendering();

// Called from plugin_main's post-BeginRendering callback — restores clean matrix
// so game logic (aim, raycasts, physics) never sees head-tracked state.
void OnPostBeginRendering();

// on_pre_gui_draw_element callback for marker/crosshair compensation.
// Returns true to keep drawing the element, false to hide.
bool OnPreGuiDrawElement(void* element, void* context);

} // namespace RE3HT
