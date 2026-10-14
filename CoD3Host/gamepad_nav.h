#pragma once

#include <cstdint>

namespace Input
{
    struct Pad;
    bool ReadPadDirect(Pad& pad);
}

namespace GamepadNav
{
// Tell ImGui a pad exists, and keep the state the menu's readout shows.
//
// Called once per frame BEFORE ImGui::NewFrame(), because that is when ImGui
// reads ImGuiBackendFlags_HasGamepad and every gamepad navigation test is
// gated on it.
//
// It deliberately does NOT translate the pad into navigation keys. ImGui's own
// Win32 backend already does that, correctly, for every button and both sticks,
// in ImGui_ImplWin32_NewFrame() - which runs earlier in the same frame. A
// second translator running after NewFrame is a no-op at best; at worst it
// wedges the key state, which is what the previous version of this did.
void AnnouncePad();

// Forget the last seen state, so reopening the menu does not still show a pad
// that has been unplugged.
void Reset();

// What the on-screen gamepad readout shows. This exists because "the pad does
// nothing in the menu" has three quite different causes - not seen, seen but
// not announced, announced but the keys are going nowhere - and without this
// they all look identical from the outside.
bool Connected();
uint16_t Buttons();
int16_t ThumbLX();
int16_t ThumbLY();
uint8_t Triggers();
}