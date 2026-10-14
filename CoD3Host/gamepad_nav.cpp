// Feeding the controller to ImGui.
//
// The settings panel and the mod menu are ImGui, and ImGui's Win32/DX11
// backends only ever hand it a mouse and a keyboard. A player holding a pad
// therefore cannot move the selection anywhere in this menu - which is not a
// small thing, because the menu is the one piece of UI this port owns.
//
// Navigation is ImGui's own (NavEnableKeyboard is on, and gamepad nav is
// what uses this), not a hand-rolled cursor: ImGui already knows how to get
// from one control to the next, handles wrap, scroll regions and popups, and
// gets that right far more cheaply than this file could.
//
// What this does is translate the pad's button mask into the keys ImGui's
// navigation listens for, once per frame, as edge-triggered presses. Edge
// triggering matters: ImGui advances one item per key event, so a held button
// would run the selection down the list at sixty a second.

#include "gamepad_nav.h"
#include "input.h"

#include "../thirdparty/imgui/imgui.h"

// The Xbox 360 pad mask, as XINPUT defines it. Input::State() reports
// buttons in this shape already - the same shape the title reads - so this is
// a translation, not a conversion.
namespace
{
    constexpr uint16_t kDPadUp    = 0x0001;
    constexpr uint16_t kDPadDown  = 0x0002;
    constexpr uint16_t kDPadLeft  = 0x0004;
    constexpr uint16_t kDPadRight = 0x0008;
    constexpr uint16_t kStart     = 0x0010;
    constexpr uint16_t kBack      = 0x0020;
    constexpr uint16_t kLeftThumb = 0x0040;
    constexpr uint16_t kRightThumb= 0x0080;
    constexpr uint16_t kLeftShoulder = 0x0100;
    constexpr uint16_t kRightShoulder= 0x0200;
    constexpr uint16_t kA = 0x1000;
    constexpr uint16_t kB = 0x2000;
    constexpr uint16_t kX = 0x4000;
    constexpr uint16_t kY = 0x8000;
}

namespace GamepadNav
{
    namespace
    {
        uint16_t g_previous = 0;

        // Press once on the way down only. ImGui's navigation is event based,
        // so a held button would repeat sixty times a second.
        bool Pressed(uint16_t now, uint16_t mask)
        {
            return (now & mask) != 0 && (g_previous & mask) == 0;
        }
    }

    // Read the pad straight from XInput, rather than through Input::State() or
    // Input::Buttons(), and that is deliberate.
    //
    // Both of those call Refresh(), and Refresh() is where the overlay's hold
    // on input is enforced: while the menu is up the title must not see the
    // pad, or pressing A jumps the player as well as activating whatever was
    // selected. Reading the overlay's state from there would mean it sees the
    // same empty pad the title does, and the menu could not be driven at all -
    // one holds the pad, the other does not.
    //
    // So the overlay asks XInput itself. One pad, read twice per frame by
    // whoever needs it, and the two disagree on purpose.
    void Feed()
{
    ImGuiIO& io = ImGui::GetIO();
    Input::Pad pad{};
    Input::ReadPadDirect(pad);
    const uint16_t now = pad.buttons;

        // The stick, as direction only. This ImGui has no analogue gamepad axis to
        // report into - AddGamepadAxis is not in this version - so the stick is
        // read as a direction and deadzoned. A held stick then advances one
        // item, like a held button, which is worse than a real analogue axis:
        // the repeat rate is not proportional to the deflection. Said here
        // rather than hidden; upgrading imgui is what would fix it properly.
        constexpr int16_t kStickDeadzone = 12000;
        auto stickDir = [kStickDeadzone](int16_t value) -> int
        {
            if (value > kStickDeadzone) return 1;
            if (value < -kStickDeadzone) return -1;
            return 0;
        };
        const int stickX = stickDir(pad.thumbLX);
        const int stickY = stickDir(pad.thumbLY);

        if (Pressed(now, kDPadUp) || stickY > 0)          io.AddKeyEvent(ImGuiKey_GamepadDpadUp, true);
        if (Pressed(now, kDPadDown) || stickY < 0)        io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
        if (Pressed(now, kDPadLeft) || stickX < 0)        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, true);
        if (Pressed(now, kDPadRight) || stickX > 0)       io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true);

        // A confirms, B backs out, the bumpers page between the tabs, and the
        // triggers move the sliders - the same shape as the console menus, so
        // the pad behaves the way a player expects from this game.
        //
        // The names in this ImGui are FaceDown and FaceLeft, not A and B, and
        // the bumpers are L1/R1 rather than L/R. FaceDown is A and FaceRight
        // is B on an Xbox pad, which is what this mask is.
        if (Pressed(now, kA))              io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
        if (Pressed(now, kB))              io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        if (Pressed(now, kX))              io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
        if (Pressed(now, kY))              io.AddKeyEvent(ImGuiKey_GamepadFaceUp, true);
        if (Pressed(now, kLeftShoulder))   io.AddKeyEvent(ImGuiKey_GamepadL1, true);
        if (Pressed(now, kRightShoulder))  io.AddKeyEvent(ImGuiKey_GamepadR1, true);
        if (Pressed(now, kLeftThumb))      io.AddKeyEvent(ImGuiKey_GamepadL3, true);
        if (Pressed(now, kRightThumb))     io.AddKeyEvent(ImGuiKey_GamepadR3, true);
        if (Pressed(now, kStart))          io.AddKeyEvent(ImGuiKey_GamepadStart, true);
        if (Pressed(now, kBack))           io.AddKeyEvent(ImGuiKey_GamepadBack, true);

        const uint16_t pressed = uint16_t(now & ~g_previous);
        if (pad.leftTrigger  > 128) io.AddKeyEvent(ImGuiKey_GamepadL2, true);
        if (pad.rightTrigger > 128) io.AddKeyEvent(ImGuiKey_GamepadR2, true);
        (void)pressed;

        // Release everything that is not held now. Doing it this way rather
        // than pairing each press with its release means a button that was
        // down last frame and is not down now always comes up, which is the
        // case that would otherwise leave ImGui stuck thinking it is held.
        static uint16_t g_held = 0;
        const uint16_t stillHeld = now & (kDPadUp | kDPadDown | kDPadLeft | kDPadRight | kA | kB | kX | kY |
                                          kLeftShoulder | kRightShoulder | kLeftThumb | kRightThumb | kStart | kBack);
        const uint16_t letGo = uint16_t(g_held & ~stillHeld);
        if (letGo & kDPadUp)        io.AddKeyEvent(ImGuiKey_GamepadDpadUp, false);
        if (letGo & kDPadDown)      io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
        if (letGo & kDPadLeft)      io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
        if (letGo & kDPadRight)     io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
        if (letGo & kA)             io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
        if (letGo & kB)             io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        if (letGo & kX)             io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
        if (letGo & kY)             io.AddKeyEvent(ImGuiKey_GamepadFaceUp, false);
        if (letGo & kLeftShoulder)  io.AddKeyEvent(ImGuiKey_GamepadL1, false);
        if (letGo & kRightShoulder) io.AddKeyEvent(ImGuiKey_GamepadR1, false);
        if (letGo & kLeftThumb)     io.AddKeyEvent(ImGuiKey_GamepadL3, false);
        if (letGo & kRightThumb)    io.AddKeyEvent(ImGuiKey_GamepadR3, false);
        if (letGo & kStart)         io.AddKeyEvent(ImGuiKey_GamepadStart, false);
        if (letGo & kBack)          io.AddKeyEvent(ImGuiKey_GamepadBack, false);
        if (pad.leftTrigger  <= 128) io.AddKeyEvent(ImGuiKey_GamepadL2, false);
        if (pad.rightTrigger <= 128) io.AddKeyEvent(ImGuiKey_GamepadR2, false);

        g_held = stillHeld;
        g_previous = now;
    }
}