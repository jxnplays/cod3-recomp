// Feeding the controller to ImGui.
//
// The settings panel and the mod menu are ImGui, and ImGui's Win32/DX11
// backends only ever hand it a mouse and a keyboard. A player holding a pad
// therefore could not move the selection anywhere in this menu - which is not
// a small thing, because the menu is the one piece of UI this port owns.
//
// WHAT THIS FILE DOES NOT DO, and why it matters.
//
// It used to re-implement the whole pad: every button, both sticks, the
// triggers, with its own edge detection and its own release bookkeeping. All
// of it was dead weight. ImGui's own Win32 backend already maps every XINPUT
// button and all eight analog axes, with correct edge handling, in
// ImGui_ImplWin32_NewFrame() - and that runs BEFORE ImGui::NewFrame(), so its
// events are the ones ImGui actually processes. This file ran afterwards, so
// everything it emitted was queued for the next frame and then superseded by
// the backend's. It was not a second way in; it was a no-op.
//
// Worse, the one part that was not redundant was broken. The analog stick was
// read as a level-triggered direction (`stickY > 0`) and fired
// AddKeyEvent(DpadUp, true) EVERY FRAME while deflected - but the release path
// only ever tracked the physical button mask, never the stick. So a single
// nudge of the stick left ImGui believing D-pad-Up was held forever, which
// ImGui's nav auto-repeat then ran down the list at its repeat rate.
//
// So what is left here is only the two things the backend genuinely does not
// do: announce that a pad exists at all, and tell the player what they are
// looking at.

#include "gamepad_nav.h"
#include "input.h"

#include "../thirdparty/imgui/imgui.h"

namespace GamepadNav
{
    namespace
    {
        bool s_connected = false;
        uint16_t s_buttons = 0;
        int16_t s_thumbLX = 0;
        int16_t s_thumbLY = 0;
        uint8_t s_triggers = 0;
    }

    // A pad is present, and ImGui has been told.
    //
    // ImGui gates ALL gamepad navigation on ImGuiBackendFlags_HasGamepad, and
    // clears every gamepad key each frame when it is absent. The backend sets
    // that flag itself, but it decides whether a pad exists exactly once - at
    // init and on WM_DEVICECHANGE - and the overlay used to swallow
    // WM_DEVICECHANGE while the menu was closed. Announcing it here means the
    // answer is re-derived every frame from a live read, and cannot go stale.
    //
    // Must be called BEFORE ImGui::NewFrame(), because that is when the flag is
    // read.
    void AnnouncePad()
    {
        ImGuiIO& io = ImGui::GetIO();
        Input::Pad pad{};
        if (Input::ReadPadDirect(pad))
        {
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            s_connected = true;
            s_buttons = pad.buttons;
            s_thumbLX = pad.thumbLX;
            s_thumbLY = pad.thumbLY;
            s_triggers = uint8_t(pad.leftTrigger > 128 ? 1 : 0) | uint8_t(pad.rightTrigger > 128 ? 2 : 0);
        }
        else
        {
            // Not cleared. The backend's own UpdateGamepads runs later in the
            // same frame and will clear it if it also finds nothing, so this
            // leaves the decision to one place rather than two that can
            // disagree.
            s_connected = false;
        }
    }

    void Reset()
    {
        s_connected = false;
        s_buttons = 0;
        s_thumbLX = 0;
        s_thumbLY = 0;
        s_triggers = 0;
    }

    // For the on-screen readout. Without it, "the pad does not work in the
    // menu" is a symptom with no way to find out whether the pad is not being
    // seen, the flag is not set, or the keys are arriving and going nowhere.
    bool Connected() { return s_connected; }
    uint16_t Buttons() { return s_buttons; }
    int16_t ThumbLX() { return s_thumbLX; }
    int16_t ThumbLY() { return s_thumbLY; }
    uint8_t Triggers() { return s_triggers; }
}