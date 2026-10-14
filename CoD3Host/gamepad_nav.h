// Translating the controller into ImGui's navigation keys, so the settings
// panel and the mod menu can be driven with a pad. See gamepad_nav.cpp for
// why this exists and why it uses ImGui's own navigation rather than a cursor.
#pragma once

namespace GamepadNav
{
    // Called once per frame, after ImGui::NewFrame() and before any panel is
    // drawn. Reads the pad and raises the navigation key events.
    void Feed();
}