#include "input.h"
#include "kernel.h"
#include "window.h"
#include "overlay.h"
#include "render_internal.h"
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
    //
    // What the title reads is one number per frame, not one per read. It asks
    // for its input twice a frame - COD3_PADRATE counts the asks, and it is
    // two for every frame it draws - and on a console both asks return the
    // same pad, because the pad is sampled once a frame and held. Here the
    // first ask used to take the whole frame's movement and the second got
    // nothing, so the stick the title steered by was zero half the time.
    //
    // So movement is collected and published per frame rather than taken per
    // read. The counts are read from a running total, the difference from the
    // last one taken is this read's share, and the share is held until the
    // title's next swap: both asks in a frame then see the same deflection,
    // and no movement is dropped or applied a frame early.
    void ReadMouse(int16_t& outX, int16_t& outY)
    {
        outX = 0;
        outY = 0;

        // What has been read out of the mouse's total so far, and what has
        // been given to the title. What is given is the whole of a frame's
        // movement, published once, and both asks in that frame read it.
        static long seenX = 0, seenY = 0;         // of the mouse's running total
        static long givenX = 0, givenY = 0;       // published to the title
        static uint64_t lastSwap = uint64_t(-1);

        // The mouse's own movement is read whether it is used or not, so
        // that what piled up while the menu was open is not a jump the
        // moment the window takes the mouse back.
        long totalX = 0, totalY = 0;
        const bool raw = Window::RawMouseTotal(totalX, totalY) && Settings::Get().rawMouse;
        if (!g_mouseHeld.load())
        {
            // Not steering: keep the total's place up to date so the movement
            // from the menu is never handed over as one lurch on entry.
            seenX = totalX;
            seenY = totalY;
            givenX = givenY = 0;
            return;
        }

        long dx = 0, dy = 0;
        if (raw)
        {
            // What has arrived since this share of the total was last read.
            dx = totalX - seenX;
            dy = totalY - seenY;
            seenX = totalX;
            seenY = totalY;
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

        // This read's share, waiting to be published.
        static long pendingX = 0, pendingY = 0;
        pendingX += dx;
        pendingY += dy;

        // Published on the title's frame boundary, so both asks in one frame
        // read the same deflection and a whole frame's movement is applied
        // together rather than split between two asks at two moments.
        const uint64_t swap = RenderInternal::Swaps();
        if (swap != lastSwap)
        {
            lastSwap = swap;
            givenX = pendingX;
            givenY = pendingY;
            pendingX = pendingY = 0;
        }
        dx = givenX;
        dy = givenY;

        // A stick's deflection is a speed: the title turns by it times its
        // frame's time. What is published is the movement of one whole frame,
        // so it is put on the scale of a sixtieth of a second - the rate the
        // gain was set at - and a sweep of the hand is the same turn whether
        // the frame it landed in was a sixtieth of a second long or a
        // two-hundredth. This matters more than it looks: the title runs at
        // 60 frames a second in the menus and at 200 and more in a level, so
        // a gain that was right for one is three times too slow in the other.
        //
        // The frame's length is measured where the swap count changes and
        // nowhere else. Measuring it between reads does not work, because the
        // title asks twice a frame and half of those gaps are the width of a
        // moment: a scale built from them swings by the length of a frame.
        double scale = 1.0;
        {
            static double secondsPerFrame = 1.0 / 60.0;
            static uint64_t timedSwap = uint64_t(-1);
            // Timed once per frame, at the boundary, and then reused by every
            // read in that frame. The first frame after a long gap - a stall,
            // a pause, the game not running at all - leaves the last good rate
            // alone rather than dividing by the gap.
            if (swap != timedSwap)
            {
                static auto lastFrame = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(now - lastFrame).count();
                lastFrame = now;
                timedSwap = swap;
                // Under half a millisecond is not a frame, and over a
                // fiftieth of a second is a stall rather than a slow frame.
                // Either way the rate from before is the better guess.
                if (seconds >= 0.0005 && seconds <= 0.05)
                {
                    // Smoothed, so one short frame in a run of long ones does
                    // not move the gain and then move it back.
                    secondsPerFrame += (seconds - secondsPerFrame) * 0.25;
                }
            }
            scale = (1.0 / 60.0) / secondsPerFrame;
        }

        // The settings menu's sensitivity scales the gain, both axes alike.
        //
        // The gain is set so a hard flick reaches full deflection and an
        // ordinary movement does not. It used to be 1300, which pinned the
        // stick after 57 counts. A mouse reports one to three counts per
        // millimetre, so 57 counts is under two millimetres of travel, and
        // every movement past that arrived at the title as the same maximum -
        // a twitch and a fast sweep indistinguishable, with no fine aim
        // anywhere between them. COD3_MOUSELOG counted it: every movement in
        // a second arriving pinned at 32767.
        //
        // 80 puts full deflection at about 410 counts, a flick of a few
        // centimetres, which leaves a slow deliberate aim at around a quarter
        // of the stick and a fast sweep below the clamp. The sensitivity
        // setting moves it from there.
        const double gain = 80.0 * Settings::Get().mouseSensitivity * scale;
        const long x = long(std::clamp(double(dx) * gain, -32767.0, 32767.0));
        // Screen coordinates grow downwards and a stick grows upwards.
        const long y = long(std::clamp(-double(dy) * gain, -32767.0, 32767.0));
        outX = int16_t(x);
        outY = int16_t(y);

        // COD3_MOUSELOG=1: what the look took, once a second. Both the counts
        // the mouse's device reported and the stick the title was given,
        // because the gain between them is where a mouse that feels wrong is
        // wrong: a gain far too high puts ordinary movement into the clamp,
        // where a twitch and a fast sweep both arrive as full deflection and
        // neither can be aimed with. The frame time is here too, since it
        // scales the movement and the title runs at sixty frames a second in
        // the menus and two hundred in a level.
        static const bool log = getenv("COD3_MOUSELOG") != nullptr;
        if (log)
        {
            static uint64_t second = 0, counts = 0, reads = 0, peak = 0, pinned = 0, frames = 0;
            static uint64_t lastLoggedSwap = uint64_t(-1);
            if (dx != 0 || dy != 0)
            {
                counts += uint64_t(std::abs(dx) + std::abs(dy));
                reads++;
                const long biggest = std::abs(x) > std::abs(y) ? std::abs(x) : std::abs(y);
                if (uint64_t(biggest) > peak) peak = uint64_t(biggest);
                if (biggest >= 32767) pinned++;
            }
            const uint64_t nowSwap = RenderInternal::Swaps();
            if (nowSwap != lastLoggedSwap) { lastLoggedSwap = nowSwap; frames++; }

            const uint64_t now = GetTickCount64() / 1000;
            if (now != second)
            {
                second = now;
                printf("input: %s, %llu counts, %llu reads, %llu frames, peak stick %llu of 32767, %llu pinned, frame %.2f ms\n",
                    raw ? "the mouse's own movement" : "the pointer's travel",
                    (unsigned long long)counts, (unsigned long long)reads, (unsigned long long)frames,
                    (unsigned long long)peak, (unsigned long long)pinned, scale * 1000.0 / 60.0);
                fflush(stdout);
                counts = 0; reads = 0; peak = 0; pinned = 0; frames = 0;
            }
        }
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
