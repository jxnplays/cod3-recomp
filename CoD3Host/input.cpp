#include "input.h"
#include "kernel.h"
#include "window.h"
#include "overlay.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

namespace
{
    // The Xbox button masks, which are also the XInput ones.
    constexpr uint16_t PadUp        = 0x0001;
    constexpr uint16_t PadDown      = 0x0002;
    constexpr uint16_t PadLeft      = 0x0004;
    constexpr uint16_t PadRight     = 0x0008;
    constexpr uint16_t PadStart     = 0x0010;
    constexpr uint16_t PadBack      = 0x0020;
    constexpr uint16_t PadLeftThumb  = 0x0040;
    constexpr uint16_t PadRightThumb = 0x0080;
    constexpr uint16_t PadLeftShoulder  = 0x0100;
    constexpr uint16_t PadRightShoulder = 0x0200;
    constexpr uint16_t PadA         = 0x1000;
    constexpr uint16_t PadB         = 0x2000;
    constexpr uint16_t PadX         = 0x4000;
    constexpr uint16_t PadY         = 0x8000;

    constexpr int16_t StickFull = 32767;

    struct XInputGamepad
    {
        uint16_t buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        int16_t thumbLX, thumbLY, thumbRX, thumbRY;
    };
    struct XInputState
    {
        uint32_t packetNumber;
        XInputGamepad gamepad;
    };

    using GetStateFunction = uint32_t (WINAPI*)(uint32_t, XInputState*);
    GetStateFunction g_getState = nullptr;
    bool g_looked = false;

    void FindXInput()
    {
        if (g_looked) return;
        g_looked = true;

        // Whichever version this machine has. Loading it by name keeps the
        // build from needing the library at all.
        static const wchar_t* const names[] = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
        for (const wchar_t* name : names)
        {
            HMODULE library = LoadLibraryW(name);
            if (library == nullptr) continue;
            g_getState = reinterpret_cast<GetStateFunction>(
                reinterpret_cast<void*>(GetProcAddress(library, "XInputGetState")));
            if (g_getState != nullptr) return;
        }
    }

    bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

    // The mouse, held by the window.
    //
    // A first person game steers with the right stick, and a mouse is a
    // relative device, so the two only meet if the pointer is pinned: the
    // cursor goes back to the middle of the window after every read and the
    // distance it travelled becomes the stick's deflection. It is taken by
    // clicking in the window and given back with Escape, because a window that
    // swallows the pointer without asking cannot be left.
    std::atomic<bool> g_mouseHeld{ false };
    POINT g_centre{};

    void SetHold(bool hold)
    {
        if (g_mouseHeld.load() == hold) return;

        const HWND window = static_cast<HWND>(Window::Handle());
        if (window == nullptr) return;

        if (hold)
        {
            RECT client{};
            GetClientRect(window, &client);
            POINT topLeft{ client.left, client.top };
            POINT bottomRight{ client.right, client.bottom };
            ClientToScreen(window, &topLeft);
            ClientToScreen(window, &bottomRight);

            g_centre.x = (topLeft.x + bottomRight.x) / 2;
            g_centre.y = (topLeft.y + bottomRight.y) / 2;

            RECT screen{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
            ClipCursor(&screen);
            SetCursorPos(g_centre.x, g_centre.y);
            printf("input: the mouse is held by the window and steers; "
                   "Escape gives it back\n");
        }
        else
        {
            ClipCursor(nullptr);
            printf("input: the mouse is free; click in the window to steer "
                   "with it\n");
        }
        fflush(stdout);

        Window::SetCursorHidden(hold);
        g_mouseHeld.store(hold);
    }

    // How far the pointer moved since the last look, in stick units. Full
    // deflection at about twenty five pixels in a sixtieth of a second, a
    // brisk turn rather than a twitch.
    void ReadMouse(int16_t& outX, int16_t& outY)
    {
        outX = 0;
        outY = 0;

        // The mouse's own movement is taken whether it is used or not, so
        // that what piled up while the menu was open is not a jump the
        // moment the window takes the mouse back.
        long rawX = 0, rawY = 0;
        const bool raw = Window::TakeRawMouse(rawX, rawY) && Settings::Get().rawMouse;
        if (!g_mouseHeld.load()) return;

        long dx = 0, dy = 0;
        if (raw)
        {
            dx = rawX;
            dy = rawY;
            // The pointer is still put back in the middle: it is hidden and
            // nothing reads it, but a click has to land in the window and it
            // must never come to rest against an edge of the screen.
            POINT now{};
            if (GetCursorPos(&now) && (now.x != g_centre.x || now.y != g_centre.y))
                SetCursorPos(g_centre.x, g_centre.y);
        }
        else
        {
            POINT now{};
            if (!GetCursorPos(&now)) return;
            dx = now.x - g_centre.x;
            dy = now.y - g_centre.y;
            if (dx != 0 || dy != 0) SetCursorPos(g_centre.x, g_centre.y);
        }

        // COD3_MOUSELOG=1: what the look took, once a second, to see that
        // the mouse's own counts arrive and how many a turn is worth.
        static const bool log = getenv("COD3_MOUSELOG") != nullptr;
        if (log && (dx != 0 || dy != 0))
        {
            static uint64_t second = 0, counts = 0, reads = 0;
            counts += uint64_t(std::abs(dx) + std::abs(dy));
            reads++;
            const uint64_t now = GetTickCount64() / 1000;
            if (now != second)
            {
                second = now;
                printf("input: %s, %llu counts over %llu reads\n", raw ? "the mouse's own movement" : "the pointer's travel",
                    (unsigned long long)counts, (unsigned long long)reads);
                fflush(stdout);
                counts = 0; reads = 0;
            }
        }

        // A stick's deflection is a speed: the title turns by it times its
        // frame's time, and reads the pad once a frame (twice, the second
        // read a moment after the first). The distance the mouse went since
        // the read before is a speed only at one frame rate, so taken as it
        // is, the turn would slow down as the frame rate went up (at 100 a
        // second to 60 per cent of the turn at 60). It is scaled to the
        // distance over a sixtieth of a second, the rate the gain was set
        // at, so a sweep of the hand is the same turn at any frame rate.
        // The read that comes straight after another is left as it is: what
        // it carries is the crumb in between.
        double scale = 1.0;
        {
            static auto lastRead = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double interval = std::chrono::duration<double>(now - lastRead).count();
            lastRead = now;
            if (interval >= 0.002) scale = (1.0 / 60.0) / std::min(interval, 0.05);
        }

        // The settings menu's sensitivity scales the gain, both axes alike.
        const double gain = 1300.0 * Settings::Get().mouseSensitivity * scale;
        const long x = long(std::clamp(double(dx) * gain, -32767.0, 32767.0));
        // Screen coordinates grow downwards and a stick grows upwards.
        const long y = long(std::clamp(-double(dy) * gain, -32767.0, 32767.0));
        outX = int16_t(x);
        outY = int16_t(y);
    }

    // One step of the menu highlight for each stretch of mouse movement.
    //
    // A menu reads the pad's cross as presses, not as a position, so the
    // movement is turned into presses: travel builds up, and each time enough
    // of it has gone by in one direction the step is reported once and the
    // total is cleared. The gap keeps a single flick from running down the
    // whole list, and the press is held for two reads so the title cannot miss
    // it between polls.
    uint16_t MenuStep()
    {
        static POINT last{};
        static bool have = false;
        static long travel = 0;
        static uint16_t holding = 0;
        static int holdReads = 0;

        POINT now{};
        if (!GetCursorPos(&now)) return 0;

        if (!have) { last = now; have = true; return 0; }
        const long dy = now.y - last.y;
        last = now;

        if (holdReads > 0)
        {
            holdReads--;
            return holding;
        }
        holding = 0;

        // Movement the other way cancels what had built up, so a wobble does
        // not eventually add up to a step.
        if ((dy > 0) != (travel > 0)) travel = 0;
        travel += dy;

        constexpr long Step = 28;   // pixels of travel to one press
        if (travel >= Step)  { travel = 0; holding = PadDown; holdReads = 2; }
        if (travel <= -Step) { travel = 0; holding = PadUp;   holdReads = 2; }
        return holding;
    }

    // Presses from a script, for driving the title without anyone at the
    // keyboard: COD3_PAD="5:start 8:a 9:down 10:a" presses each button at
    // that many seconds from the start. Names are start back a b x y up
    // down left right ls rs; a plus joins several ("10:down+a"), and the
    // seconds can have a decimal. "40:lt/6" and "40:rt/6" hold the left
    // or right trigger for that many seconds (aiming, firing), and
    // "40:ly+/5" or "40:rx-50/2" hold a stick (lx ly rx ry, + or -, an
    // optional per cent): walking and looking round without a hand on it.
    //
    // A press lasts two of the title's own reads of the pad and no longer.
    // It used to last a quarter of a second, and a menu that opened while
    // the button was still down took it as pressed again: one Start on the
    // title screen fell through the main menu, the single player menu and
    // a dialogue and started a game, or landed anywhere depending on how
    // fast the screens came.
    struct ScriptedPress { double at; uint16_t buttons; int readsLeft; bool begun; };
    std::vector<ScriptedPress> g_script;
    // A held trigger or stick: which (0 and 1 the left and right trigger,
    // 2 to 5 the left stick's x and y and the right stick's x and y) and how
    // far, as a fraction of full, negative for left and down.
    struct ScriptedHold { double at, until; int control; float amount; };
    std::vector<ScriptedHold> g_holds;
    bool g_scriptParsed = false;
    std::chrono::steady_clock::time_point g_scriptStart;

    uint16_t ButtonNamed(const std::string& name)
    {
        static const std::pair<const char*, uint16_t> names[] = {
            { "start", PadStart }, { "back", PadBack }, { "a", PadA }, { "b", PadB },
            { "x", PadX }, { "y", PadY }, { "up", PadUp }, { "down", PadDown },
            { "left", PadLeft }, { "right", PadRight },
            { "ls", PadLeftShoulder }, { "rs", PadRightShoulder },
        };
        for (const auto& entry : names) if (name == entry.first) return entry.second;
        return 0;
    }

    uint16_t ScriptedButtons()
    {
        if (!g_scriptParsed)
        {
            g_scriptParsed = true;
            g_scriptStart = std::chrono::steady_clock::now();
            if (const char* text = getenv("COD3_PAD"))
            {
                std::string all(text);
                size_t at = 0;
                while (at < all.size())
                {
                    const size_t end = all.find(' ', at);
                    const std::string item = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    at = end == std::string::npos ? all.size() : end + 1;
                    const size_t colon = item.find(':');
                    if (colon == std::string::npos) continue;
                    ScriptedPress press{ atof(item.substr(0, colon).c_str()), 0, 2, false };
                    std::string rest = item.substr(colon + 1);
                    if (rest.compare(0, 3, "lt/") == 0 || rest.compare(0, 3, "rt/") == 0)
                    {
                        g_holds.push_back({ press.at, press.at + atof(rest.c_str() + 3), rest[0] == 'l' ? 0 : 1, 1.0f });
                        continue;
                    }
                    // A stick: "rx+/3" holds the right stick fully right for
                    // three seconds, "ly+60/2" the left stick six tenths up
                    // for two (walking forward). lx ly rx ry, + or -, an
                    // optional per cent, then the seconds.
                    if (rest.size() >= 4 && (rest[0] == 'l' || rest[0] == 'r') && (rest[1] == 'x' || rest[1] == 'y') &&
                        (rest[2] == '+' || rest[2] == '-'))
                    {
                        const size_t slash = rest.find('/');
                        if (slash == std::string::npos) continue;
                        const std::string percent = rest.substr(3, slash - 3);
                        float amount = percent.empty() ? 1.0f : float(atof(percent.c_str())) / 100.0f;
                        if (rest[2] == '-') amount = -amount;
                        const int control = 2 + (rest[0] == 'r' ? 2 : 0) + (rest[1] == 'y' ? 1 : 0);
                        g_holds.push_back({ press.at, press.at + atof(rest.c_str() + slash + 1), control, amount });
                        continue;
                    }
                    size_t from = 0;
                    while (from <= rest.size())
                    {
                        const size_t plus = rest.find('+', from);
                        press.buttons |= ButtonNamed(rest.substr(from, plus == std::string::npos ? std::string::npos : plus - from));
                        if (plus == std::string::npos) break;
                        from = plus + 1;
                    }
                    g_script.push_back(press);
                }
                printf("input: %zu scripted presses and %zu holds from COD3_PAD\n", g_script.size(), g_holds.size());
            }
        }
        if (g_script.empty()) return 0;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_scriptStart).count();
        uint16_t buttons = 0;
        for (ScriptedPress& press : g_script)
        {
            if (seconds < press.at || press.readsLeft <= 0) continue;
            if (!press.begun)
            {
                press.begun = true;
                // A press is where something is about to happen, so the
                // kernel trace, if one was asked for, starts here.
                Kernel::StartKernelTrace();
            }
            buttons |= press.buttons;
        }
        return buttons;
    }

    // The triggers and sticks a script holds now.
    void ScriptedHolds(Input::Pad& pad)
    {
        if (g_holds.empty()) return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_scriptStart).count();
        for (const ScriptedHold& hold : g_holds)
        {
            if (seconds < hold.at || seconds >= hold.until) continue;
            const int16_t stick = int16_t(std::clamp(hold.amount, -1.0f, 1.0f) * 32767.0f);
            switch (hold.control)
            {
            case 0: pad.leftTrigger = 255; break;
            case 1: pad.rightTrigger = 255; break;
            case 2: pad.thumbLX = stick; break;
            case 3: pad.thumbLY = stick; break;
            case 4: pad.thumbRX = stick; break;
            case 5: pad.thumbRY = stick; break;
            }
        }
    }

    // The title has read the pad: the presses it saw count down.
    void ScriptedRead(uint16_t buttons)
    {
        for (ScriptedPress& press : g_script)
            if (press.begun && press.readsLeft > 0 && (buttons & press.buttons) == press.buttons)
                press.readsLeft--;
    }

    // The keyboard and mouse stand in for a pad. Only while the title's own
    // window is in front, so typing anywhere else is not taken as input.
    Input::Pad ReadKeyboard()
    {
        Input::Pad pad{};
        // COD3_NOKEYBOARD=1 ignores the keyboard and mouse entirely, for
        // runs driven by a script while someone is using the machine: the
        // key state is global, and typing anywhere walked the title's menus.
        static const bool ignored = []() {
            const char* text = getenv("COD3_NOKEYBOARD");
            if (text == nullptr) text = getenv("COD3_BACKGROUND");   // a run nobody is playing
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        if (ignored) return pad;
        if (!Window::HasFocus() || Overlay::IsOpen())
        {
            // Out of focus, or the settings menu is up and the keys and the
            // mouse are its.
            if (g_mouseHeld.load()) SetHold(false);
            return pad;
        }

        // Taking and giving back the pointer.
        //
        // In a level the window takes the mouse by itself, the moment it is in
        // front, and a click in it takes the mouse back after Escape: looking
        // around is what a mouse is for in a shooter, and asking for a key
        // first was asking every player to read the console. In the menus the
        // left button used to take it, which made the mouse useless for the
        // one thing a menu wants it for: clicking; F1 and the middle button
        // take it there. Escape always gives it back, and in a level it is
        // also Start, so the pause menu opens with the pointer free to leave.
        const bool inLevel = Kernel::Stats().filesOpened.load(std::memory_order_relaxed) >= 40;
        const bool escapeDown = Down(VK_ESCAPE);
        // Start is held for a few reads after Escape frees the pointer, so
        // the title's own poll, which is slower than these reads, sees it.
        static int startReadsLeft = 0;
        if (g_mouseHeld.load() && escapeDown) { SetHold(false); startReadsLeft = 10; }
        else if (!g_mouseHeld.load())
        {
            static bool takenForLevel = false;
            if (!inLevel) takenForLevel = false;
            const bool firstTime = inLevel && !takenForLevel;
            if (Down(VK_F1) || Down(VK_MBUTTON) || firstTime || (inLevel && Down(VK_LBUTTON) && !escapeDown))
            {
                SetHold(true);
                if (inLevel) takenForLevel = true;
            }
        }

        const bool held = g_mouseHeld.load();

        // The keys, where the console's buttons are on a keyboard, from the
        // settings menu: jump is A, crouch B, use and reload X, weapon Y,
        // melee the right stick pressed, sprint the left, the grenade the
        // right bumper and the smoke the left, objectives left on the cross,
        // pause Start. The arrows and Enter always walk the menus.
        const Settings::Values keys = Settings::Get();
        struct Mapping { int key; uint16_t button; };
        const Mapping mappings[] = {
            { VK_RETURN, PadStart },
            { VK_UP, PadUp }, { VK_DOWN, PadDown },
            { VK_LEFT, PadLeft }, { VK_RIGHT, PadRight },
            { keys.keys[Settings::Jump], PadA },
            { keys.keys[Settings::Crouch], PadB },
            { keys.keys[Settings::Use], PadX }, { keys.keys[Settings::Reload], PadX },
            { keys.keys[Settings::Weapon], PadY },
            { keys.keys[Settings::Melee], PadRightThumb },
            { keys.keys[Settings::Sprint], PadLeftThumb },
            { keys.keys[Settings::Grenade], PadRightShoulder },
            { keys.keys[Settings::Smoke], PadLeftShoulder },
            { keys.keys[Settings::Objectives], PadLeft },
        };

        for (const Mapping& mapping : mappings)
            if (mapping.key != 0 && Down(mapping.key)) pad.buttons |= mapping.button;
        // Pause, when it is not the Escape that frees the mouse (below).
        if (inLevel && keys.keys[Settings::Pause] != 0 && keys.keys[Settings::Pause] != VK_ESCAPE && Down(keys.keys[Settings::Pause]))
            pad.buttons |= PadStart;

        // The wheel changes weapon, either way: the console has one button
        // for it, which cycles, so up and down both press it, held for a
        // few reads so the title's own poll sees the press.
        {
            static int wheelReadsLeft = 0;
            const int wheel = Window::TakeWheel();
            if (wheel != 0) wheelReadsLeft = 8;
            if (wheelReadsLeft > 0) { pad.buttons |= PadY; wheelReadsLeft--; }
        }

        // The title's first screen waits for Start, and asking a player to know
        // that is asking them to guess. While the mouse is free, which is every
        // menu, Space is Start as well as A.
        if (!held && Down(VK_SPACE)) pad.buttons |= PadStart;
        if (!held && Down(VK_ESCAPE) && !inLevel) pad.buttons |= PadBack;
        if (inLevel && startReadsLeft > 0) { pad.buttons |= PadStart; startReadsLeft--; }

        // Driving a menu with the mouse.
        //
        // The menus are built for a pad: a highlight that moves up and down and
        // a button that takes it. There is no way to ask the title what is
        // under the pointer, so pointing at an entry cannot select it. What can
        // be done is to make the mouse do what the pad does. Moving it up or
        // down steps the highlight, one step per movement rather than one per
        // pixel, and the left button takes the entry. It is not pointing and
        // clicking, but it is a menu driven with the mouse.
        if (!held && !inLevel)
        {
            if (Down(VK_LBUTTON)) pad.buttons |= PadA;
            if (Down(VK_RBUTTON)) pad.buttons |= PadBack;
            pad.buttons |= MenuStep();
        }

        // Moving. WASD is the left stick so it is analogue, and the arrows stay
        // on the pad's cross so menus that only read that still work.
        int16_t x = 0, y = 0;
        if (keys.keys[Settings::Left] && Down(keys.keys[Settings::Left])) x -= StickFull;
        if (keys.keys[Settings::Right] && Down(keys.keys[Settings::Right])) x += StickFull;
        if (keys.keys[Settings::Backward] && Down(keys.keys[Settings::Backward])) y -= StickFull;
        if (keys.keys[Settings::Forward] && Down(keys.keys[Settings::Forward])) y += StickFull;
        pad.thumbLX = x;
        pad.thumbLY = y;

        ReadMouse(pad.thumbRX, pad.thumbRY);

        // Firing and aiming, where a shooter expects them: while the mouse
        // is held, or for keys that are not mouse buttons at any time.
        const int fire = keys.keys[Settings::Fire], aim = keys.keys[Settings::Aim];
        const auto mouseButton = [](int key) { return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2; };
        if (fire != 0 && (held || !mouseButton(fire)) && Down(fire)) pad.rightTrigger = 255;
        if (aim != 0 && (held || !mouseButton(aim)) && Down(aim)) pad.leftTrigger = 255;

        return pad;
    }

    std::mutex g_mutex;
    Input::Pad g_pad{};
    uint32_t g_packet = 1;
    std::chrono::steady_clock::time_point g_lastRead;

    bool Same(const Input::Pad& a, const Input::Pad& b)
    {
        return a.buttons == b.buttons &&
               a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger &&
               a.thumbLX == b.thumbLX && a.thumbLY == b.thumbLY &&
               a.thumbRX == b.thumbRX && a.thumbRY == b.thumbRY;
    }

    // Asking Windows on every call would cost more than the title's own frame.
    // Once every four milliseconds is finer than any frame it draws.
    void Refresh()
    {
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(g_mutex);
        if (now - g_lastRead < std::chrono::milliseconds(4) && g_script.empty() && g_holds.empty()) return;
        g_lastRead = now;

        FindXInput();

        // A pad and the keyboard together, not one or the other. A pad that is
        // plugged in and untouched used to silence the keyboard completely,
        // which looks exactly like input not working at all.
        Input::Pad pad = ReadKeyboard();
        const uint16_t fromKeyboard = pad.buttons;
        const uint16_t fromScript = ScriptedButtons();
        uint16_t fromPad = 0;
        pad.buttons |= fromScript;
        ScriptedHolds(pad);
        // COD3_NOPAD=1 ignores a physical pad the same way, for the same
        // reason: one lying on the desk with a button pressed walked the
        // title's menus in place of the script.
        static const bool padIgnored = []() {
            const char* text = getenv("COD3_NOPAD");
            return text != nullptr && text[0] != 0 && text[0] != '0';
        }();
        if (g_getState != nullptr && !padIgnored && Settings::Get().controller)
        {
            XInputState state{};
            if (g_getState(0, &state) == ERROR_SUCCESS)
            {
                fromPad = state.gamepad.buttons;
                pad.buttons |= state.gamepad.buttons;
                pad.leftTrigger = std::max(pad.leftTrigger, state.gamepad.leftTrigger);
                pad.rightTrigger = std::max(pad.rightTrigger, state.gamepad.rightTrigger);
                if (state.gamepad.thumbLX != 0 || state.gamepad.thumbLY != 0)
                {
                    pad.thumbLX = state.gamepad.thumbLX;
                    pad.thumbLY = state.gamepad.thumbLY;
                }
                if (state.gamepad.thumbRX != 0 || state.gamepad.thumbRY != 0)
                {
                    pad.thumbRX = state.gamepad.thumbRX;
                    pad.thumbRY = state.gamepad.thumbRY;
                }
            }
        }

        if (!Same(pad, g_pad))
        {
            // Where a button came from, the first few times it changes:
            // the title sometimes walks its own menus with no one pressing
            // anything, and this says whether anything was pressed at all.
            static int announced = 0;
            if (pad.buttons != g_pad.buttons && announced++ < 40)
            {
                printf("input: buttons 0x%04X (keyboard 0x%04X, script 0x%04X, pad 0x%04X, focus %d)\n",
                    pad.buttons, fromKeyboard, fromScript, fromPad, Window::HasFocus() ? 1 : 0);
                fflush(stdout);
            }
            g_pad = pad;
            g_packet++;
        }
    }
}

Input::Pad Input::State()
{
    Refresh();
    std::lock_guard<std::mutex> lock(g_mutex);
    ScriptedRead(g_pad.buttons);
    return g_pad;
}

uint16_t Input::Buttons()
{
    return State().buttons;
}

bool Input::MouseHeld()
{
    return g_mouseHeld.load();
}

uint32_t Input::PacketNumber()
{
    Refresh();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_packet;
}

const char* Input::KeyboardHelp()
{
    return "Enter or Space start, mouse moves the menu and clicks to choose, "
           "F1 steers with it, Escape frees it";
}
