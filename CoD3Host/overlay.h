// The settings menu (F11), the frame counter and the screenshot key (F12),
// drawn over the picture with Dear ImGui on the window's thread.
#pragma once

#include <cstdint>
#include <string>


struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace Overlay
{
    // On the window's thread, once the swap chain exists.
    void Initialize(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* context);

    // Before the window's Present: the menu and the counter into the back
    // buffer, and the screenshot of it when one was asked for.
    void Render(ID3D11RenderTargetView* back, ID3D11Texture2D* backTexture, int width, int height);

    // The window procedure hands every message here first; true when it
    // was the menu's.
    bool HandleMessage(void* hwnd, uint32_t message, uint64_t wParam, int64_t lParam);

// The mod menu tab: the title's own console variables. Fire-and-forget,
// because this build cannot read a value back (Com_Printf is compiled out of
// it), so a tick means the menu set it rather than that the game confirmed
// it. What is not in the list, and why, is at the top of mods.cpp.
bool DrawModsTab();

    // The settings menu or the console is up: the keys and the mouse are
    // theirs, not the title's.
    bool IsOpen();
    void Toggle();
    void ToggleConsole();

    // A line for the console's log, from anywhere.
    void ConsolePrint(const std::string& line);
    void RequestScreenshot();

// Whether the frame now being drawn is one where a scripted capture
// (COD3_SHOT_AT / COD3_SHOT_EVERY) is using the COD3_SHOT_ALT values instead
// of the player's settings.
//
// The present path asks this so that two captures one frame apart can differ
// only by the feature under test. Comparing two separate runs cannot do that,
// because the camera is somewhere different each time.
bool CaptureUsingForcedGrade();

    // Frames the title finished, for the counter; called at every swap.
    void NoteFrame();

    // What the front end drew this frame (title_trace.cpp): the main
    // menu, or its options page. The settings menu opens itself when the
    // options page follows the main menu, which is the OPTIONS entry.
    enum class FrontEndText { MainMenu = 1, OptionsMenu = 2 };
    void NoteFrontEndText(FrontEndText text);
}
