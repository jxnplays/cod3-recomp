// The player's settings: what the F11 menu shows and changes, kept in
// CoD3Recomp.ini beside the executable. Read from any thread through a copy;
// written by the menu.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Settings
{
    // The keys the menu binds, each standing in for one of the pad's
    // controls. The order is the order the menu lists them in.
    enum Action
    {
        Fire, Aim, Jump, Crouch, Use, Reload, Weapon, Melee, Sprint, Grenade, Smoke,
        Objectives, Pause, Forward, Backward, Left, Right,
        ActionCount
    };

    const char* ActionName(Action action);   // "Fire", "Aim", ...

    // A key, as a Windows virtual key code, or 0 for none.
    const char* KeyName(int virtualKey);

    struct Values
    {
        int renderer = 0;             // 0 Direct3D 11; 1 and 2 are names only
        int resolutionWidth = 0;      // the resolution the frame is drawn at; 0 by 0 is the desktop's
        int resolutionHeight = 0;
        int resolutionScale = 100;    // per cent of it the frame is really drawn at, 25 .. 200
        int textureFilter = 3;        // 0 the title's own, 1 bilinear, 2 trilinear, 3 anisotropic
        int anisotropy = 16;          // 2..16, with textureFilter 3
        bool vsync = true;
        bool unlockFrameRate = true;  // a level played at what the machine can do; the menus and films keep the console's 60
        int antialiasing = 3;         // 0 none, 1 FXAA, 2 MSAA 2x, 3 MSAA 4x, 4 MSAA 8x, 5 MSAA 4x and FXAA
        int textureQuality = 2;       // 0 low (two mip levels dropped), 1 medium (one), 2 high (all)
        int fov = 65;                 // the title's cg_fov, 65 (its own) .. 100
        int modelDetail = 1;          // 0 the title's own level of detail (r_lodscale 1), 1 the most detailed models at every distance (r_lodscale 0)
        bool aimBlur = true;          // the title's depth of field while aiming down the sights
        bool consoleDepth = true;     // depth cut to the console's 24 bit float precision (no flickering surfaces; costs with MSAA)
        bool nativeCommands = false;  // the draws' state taken from the game's own D3D calls, the command stream run only for what the GPU still does (docs/native-d3d11-plan.md, N3; experimental, after a restart)
        bool watchWrites = false;     // still textures and vertex buffers known unchanged by their pages, not hashed every frame (write_watch.h; experimental, after a restart)
        int windowMode = 0;           // 0 windowed, 1 borderless full screen
        bool fpsOverlay = false;
        bool statsOverlay = false;    // the renderer's counters over the picture
        bool aimAssist = true;        // the title's own, through its config
        bool controller = true;       // read a pad when one is there
        float mouseSensitivity = 1.0f;   // both axes, 0.1 .. 3
        bool rawMouse = true;            // the mouse's own counts, not the pointer's travel
        int keys[ActionCount] = {};
    };

    Values Defaults();

    // The resolution chosen, resolved: the desktop's when the setting is
    // 0 by 0. In a window the window is made this size; over the whole
    // screen the picture is scaled to it.
    void Resolution(const Values& values, int& width, int& height);

    // The size the frame is really drawn at: the resolution times the scale.
    void DrawnSize(const Values& values, int& width, int& height);

    // The resolutions the display offers, largest last, for the menu.
    struct Mode { int width, height; };
    const std::vector<Mode>& DisplayModes();

    // The current values, and setting them (which also saves).
    Values Get();
    void Set(const Values& values);

    // Reads the file beside the executable, or starts from the defaults.
    void Load(const std::filesystem::path& exeDirectory);
    void Save();

    // The console commands the settings ask of the title (the field of
    // view, the model detail, the aim assist), for its command buffer: at
    // the first ask whatever differs from the title's own values, then
    // what changed since the last ask. Empty when there is nothing to say.
    std::string TitleCommandsChanged();
}
