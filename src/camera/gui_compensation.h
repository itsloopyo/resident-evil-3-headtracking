#pragma once

namespace RE3HT {

// on_pre_gui_draw_element callback for reticle/marker compensation.
// Returns true to keep drawing the element, false to hide.
bool OnPreGuiDrawElement(void* element, void* context);

} // namespace RE3HT
