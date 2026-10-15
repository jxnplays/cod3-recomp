#include "settings.h"
#include "logic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <Windows.h>

namespace
{
    std::mutex g_mutex;
    Settings::Values g_values;
    std::filesystem::path g_file;

    const char* const ActionNames[Settings::ActionCount] = {
        "Fire", "Aim", "Jump", "Crouch", "Use", "Reload", "Weapon", "Melee", "Sprint",
        "Grenade", "Smoke", "Objectives", "Pause", "Forward", "Backward", "Left", "Right",
    };
    const char* const ActionKeys[Settings::ActionCount] = {
        "fire", "aim", "jump", "crouch", "use", "reload", "weapon", "melee", "sprint",
        "grenade", "smoke", "objectives", "pause", "forward", "backward", "left", "right",
    };

    std::string Trim(const std::string& text)
    {
        size_t a = 0, b = text.size();
        while (a < b && isspace((unsigned char)text[a])) a++;
        while (b > a && isspace((unsigned char)text[b - 1])) b--;
        return text.substr(a, b - a);
    }
}

const char* Settings::ActionName(Action action)
{
    return action >= 0 && action < ActionCount ? ActionNames[action] : "?";
}

const char* Settings::KeyName(int key)
{
    static char name[64];
    switch (key)
    {
    case 0: return "-";
    case VK_LBUTTON: return "Mouse 1";
    case VK_RBUTTON: return "Mouse 2";
    case VK_MBUTTON: return "Mouse 3";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_SHIFT: return "Shift";
    case VK_LSHIFT: return "Left Shift";
    case VK_RSHIFT: return "Right Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_LCONTROL: return "Left Ctrl";
    case VK_RCONTROL: return "Right Ctrl";
    case VK_MENU: return "Alt";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    default: break;
    }
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) { name[0] = char(key); name[1] = 0; return name; }
    if (key >= VK_F1 && key <= VK_F12) { snprintf(name, sizeof(name), "F%d", key - VK_F1 + 1); return name; }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) { snprintf(name, sizeof(name), "Num %d", key - VK_NUMPAD0); return name; }
    // Anything else by what the keyboard layout calls it.
    const UINT scan = MapVirtualKeyW(UINT(key), MAPVK_VK_TO_VSC);
    wchar_t wide[64] = {};
    if (scan != 0 && GetKeyNameTextW(LONG(scan << 16), wide, 63) > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), nullptr, nullptr);
        return name;
    }
    snprintf(name, sizeof(name), "Key %d", key);
    return name;
}

Settings::Values Settings::Defaults()
{
    Values v;
    v.keys[Fire] = VK_LBUTTON;
    v.keys[Aim] = VK_RBUTTON;
    v.keys[Jump] = VK_SPACE;
    v.keys[Crouch] = 'C';
    v.keys[Use] = 'E';
    v.keys[Reload] = 'R';
    v.keys[Weapon] = 'F';
    v.keys[Melee] = 'V';
    v.keys[Sprint] = VK_SHIFT;
    v.keys[Grenade] = 'G';
    v.keys[Smoke] = '4';
    v.keys[Objectives] = VK_TAB;
    v.keys[Pause] = VK_ESCAPE;
    v.keys[Forward] = 'W';
    v.keys[Backward] = 'S';
    v.keys[Left] = 'A';
    v.keys[Right] = 'D';
    return v;
}

Settings::Values Settings::Get()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values;
}

void Settings::Set(const Values& values)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_values = values;
    }
    Save();
}

void Settings::Load(const std::filesystem::path& exeDirectory)
{
    g_file = exeDirectory / "CoD3Recomp.ini";
    Values v = Defaults();
    if (FILE* file = _wfopen(g_file.c_str(), L"rb"))
    {
        char line[512];
        while (fgets(line, sizeof(line), file))
        {
            const std::string text = Trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[') continue;
            const size_t eq = text.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(text.substr(0, eq)), value = Trim(text.substr(eq + 1));
            if (key == "renderer") v.renderer = atoi(value.c_str());
            else if (key == "resolution")
            {
                // "1920x1080", or "desktop".
                int w = 0, h = 0;
                if (sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { v.resolutionWidth = w; v.resolutionHeight = h; }
            }
            else if (key == "resolution_scale") v.resolutionScale = atoi(value.c_str());
            else if (key == "texture_filter") v.textureFilter = atoi(value.c_str());
            else if (key == "anisotropy") v.anisotropy = atoi(value.c_str());
            else if (key == "vsync") v.vsync = atoi(value.c_str()) != 0;
            else if (key == "unlock_frame_rate") v.unlockFrameRate = atoi(value.c_str()) != 0;
            else if (key == "antialiasing") v.antialiasing = atoi(value.c_str());
            else if (key == "texture_quality") v.textureQuality = atoi(value.c_str());
            else if (key == "fov") v.fov = atoi(value.c_str());
            else if (key == "model_detail") v.modelDetail = atoi(value.c_str());
            else if (key == "aim_blur") v.aimBlur = atoi(value.c_str()) != 0;
            else if (key == "console_depth") v.consoleDepth = atoi(value.c_str()) != 0;
            else if (key == "native_commands") v.nativeCommands = atoi(value.c_str()) != 0;
            else if (key == "watch_writes") v.watchWrites = atoi(value.c_str()) != 0;
            else if (key == "window_mode") v.windowMode = atoi(value.c_str());
            else if (key == "fps_overlay") v.fpsOverlay = atoi(value.c_str()) != 0;
            else if (key == "stats_overlay") v.statsOverlay = atoi(value.c_str()) != 0;
            else if (key == "aim_assist") v.aimAssist = atoi(value.c_str()) != 0;
            else if (key == "controller") v.controller = atoi(value.c_str()) != 0;
        else if (key == "aspect_mode") v.aspectMode = atoi(value.c_str());
        else if (key == "control_layout") v.controlLayout = atoi(value.c_str());
        else if (key == "contrast") v.contrast = float(atof(value.c_str()));
        else if (key == "saturation") v.saturation = float(atof(value.c_str()));
        else if (key == "temperature") v.temperature = float(atof(value.c_str()));
        else if (key == "brightness") v.brightness = float(atof(value.c_str()));
        else if (key == "vignette") v.vignette = float(atof(value.c_str()));
        else if (key == "grain") v.grain = float(atof(value.c_str()));
        else if (key == "sharpen") v.sharpen = float(atof(value.c_str()));
        else if (key == "playstation_prompts") v.playstationPrompts = atoi(value.c_str()) != 0;
            else if (key == "stick_sensitivity") v.stickSensitivity = float(atof(value.c_str()));
        else if (key == "aim_assist_stick") v.aimAssistStick = atoi(value.c_str()) != 0;
        else if (key == "aim_assist_mouse") v.aimAssistMouse = atoi(value.c_str()) != 0;
        else if (key == "mouse_sensitivity") v.mouseSensitivity = float(atof(value.c_str()));
            else if (key == "raw_mouse") v.rawMouse = atoi(value.c_str()) != 0;
            else if (key.compare(0, 4, "key_") == 0)
            {
                for (int i = 0; i < ActionCount; i++)
                    if (key.compare(4, std::string::npos, ActionKeys[i]) == 0) v.keys[i] = atoi(value.c_str());
            }
        }
        fclose(file);
    }
    if (v.renderer < 0 || v.renderer > 2) v.renderer = 0;
    if (v.resolutionWidth < 320 || v.resolutionHeight < 240 || v.resolutionWidth > 16384 || v.resolutionHeight > 16384) { v.resolutionWidth = 0; v.resolutionHeight = 0; }
    if (v.resolutionScale < 25 || v.resolutionScale > 300) v.resolutionScale = 100;
    if (v.textureFilter < 0 || v.textureFilter > 3) v.textureFilter = 3;
    if (v.anisotropy < 2 || v.anisotropy > 16) v.anisotropy = 16;
    if (v.antialiasing < 0 || v.antialiasing > 5) v.antialiasing = 3;
    if (v.textureQuality < 0 || v.textureQuality > 2) v.textureQuality = 2;
    // 65 to 180, matching the slider. It stopped at 100. 180 goes on the title's
    // command buffer without complaint and without a fault, which is what the
    // range is allowed on; what it looks like is for someone to judge.
    if (v.fov < 65 || v.fov > 180) v.fov = 65;
    if (v.modelDetail < 0 || v.modelDetail > 1) v.modelDetail = 1;
    if (v.windowMode < 0 || v.windowMode > 1) v.windowMode = 0;
    // Up to 10, matching the slider. It used to stop at 3, which is low for raw
    // mouse: at 3 the gain already saturates the stick's range within about
    // eight pixels of movement, so there was no room left between "too slow"
    // and "clipped".
    // The floor is shared with the menu and with ReadMouse; it used to be 0.1,
    // which could not take the edge off a modern mouse at any setting.
    if (!(v.mouseSensitivity >= float(HostLogic::kMinMouseSensitivity) && v.mouseSensitivity <= 10.0f)) v.mouseSensitivity = 1.0f;
    if (!(v.stickSensitivity >= 0.1f && v.stickSensitivity <= 10.0f)) v.stickSensitivity = 1.0f;
    // Four layouts, 0 to 3. The ini is hand-editable, so an index out of
    // range has to be corrected here rather than trusted.
    if (v.controlLayout < 0 || v.controlLayout > 3) v.controlLayout = 0;
    if (v.aspectMode < 0 || v.aspectMode > 1) v.aspectMode = 0;
    // Grading ranges. The identity has to survive a round trip through the
    // ini exactly, so 1.0 and 0.0 are inside these and not clamped to a
    // neighbouring value.
    if (!(v.contrast >= 0.5f && v.contrast <= 2.0f)) v.contrast = 1.0f;
    if (!(v.saturation >= 0.0f && v.saturation <= 2.0f)) v.saturation = 1.0f;
    if (!(v.temperature >= -10.0f && v.temperature <= 10.0f)) v.temperature = 0.0f;
    if (!(v.brightness >= -0.5f && v.brightness <= 0.5f)) v.brightness = 0.0f;
    if (!(v.sharpen >= 0.0f && v.sharpen <= 1.0f)) v.sharpen = 0.0f;
    if (!(v.vignette >= 0.0f && v.vignette <= 1.0f)) v.vignette = 0.0f;
    if (!(v.grain >= 0.0f && v.grain <= 1.0f)) v.grain = 0.0f;
    // A checkbox: anything but 0 is "on", so there is no range to check.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_values = v;
}

void Settings::Save()
{
    Values v = Get();
    if (g_file.empty()) return;
    if (FILE* file = _wfopen(g_file.c_str(), L"wb"))
    {
        fprintf(file, "# Call of Duty 3 recompiled: the settings menu (F11) keeps its values here.\n");
        char resolution[32];
        if (v.resolutionWidth > 0 && v.resolutionHeight > 0) snprintf(resolution, sizeof(resolution), "%dx%d", v.resolutionWidth, v.resolutionHeight);
        else snprintf(resolution, sizeof(resolution), "desktop");
        fprintf(file, "[graphics]\nrenderer = %d\nresolution = %s\nresolution_scale = %d\ntexture_filter = %d\nanisotropy = %d\nvsync = %d\naspect_mode = %d\nunlock_frame_rate = %d\nantialiasing = %d\ntexture_quality = %d\nfov = %d\nmodel_detail = %d\naim_blur = %d\nconsole_depth = %d\nnative_commands = %d\nwatch_writes = %d\nwindow_mode = %d\nfps_overlay = %d\nstats_overlay = %d\ncontrast = %.3f\nsaturation = %.3f\ntemperature = %.3f\nbrightness = %.3f\nsharpen = %.3f\nvignette = %.3f\ngrain = %.3f\n",
            v.renderer, resolution, v.resolutionScale, v.textureFilter, v.anisotropy, v.vsync ? 1 : 0, v.aspectMode, v.unlockFrameRate ? 1 : 0, v.antialiasing, v.textureQuality, v.fov, v.modelDetail, v.aimBlur ? 1 : 0, v.consoleDepth ? 1 : 0, v.nativeCommands ? 1 : 0, v.watchWrites ? 1 : 0, v.windowMode, v.fpsOverlay ? 1 : 0, v.statsOverlay ? 1 : 0,
        v.contrast, v.saturation, v.temperature, v.brightness, v.sharpen, v.vignette, v.grain);
            fprintf(file, "[game]\naim_assist = %d\naim_assist_stick = %d\naim_assist_mouse = %d\nstick_sensitivity = %.2f\ncontroller = %d\ncontrol_layout = %d\nplaystation_prompts = %d\nmouse_sensitivity = %.2f\nraw_mouse = %d\n", v.aimAssist ? 1 : 0, v.aimAssistStick ? 1 : 0, v.aimAssistMouse ? 1 : 0, v.stickSensitivity, v.controller ? 1 : 0, v.controlLayout,
        v.playstationPrompts ? 1 : 0, v.mouseSensitivity, v.rawMouse ? 1 : 0);
        fprintf(file, "[keys]\n");
        for (int i = 0; i < ActionCount; i++) fprintf(file, "key_%s = %d\n", ActionKeys[i], v.keys[i]);
        fclose(file);
    }
}

void Settings::Resolution(const Values& values, int& width, int& height)
{
    if (values.resolutionWidth > 0 && values.resolutionHeight > 0)
    {
        width = values.resolutionWidth;
        height = values.resolutionHeight;
        return;
    }
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0) { width = 1280; height = 720; }
}

void Settings::DrawnSize(const Values& values, int& width, int& height)
{
    Resolution(values, width, height);
    const int percent = values.resolutionScale < 25 ? 25 : values.resolutionScale > 300 ? 300 : values.resolutionScale;
    width = std::max(320, width * percent / 100);
    height = std::max(240, height * percent / 100);
}

const std::vector<Settings::Mode>& Settings::DisplayModes()
{
    // Asked of the display once: every size it offers at 720p or better,
    // each once, smallest first, the desktop's among them.
    static const std::vector<Mode> modes = []() {
        std::vector<Mode> found;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); i++)
        {
            if (mode.dmPelsWidth < 1280 || mode.dmPelsHeight < 720) continue;
            bool known = false;
            for (const Mode& m : found) if (m.width == int(mode.dmPelsWidth) && m.height == int(mode.dmPelsHeight)) known = true;
            if (!known) found.push_back({ int(mode.dmPelsWidth), int(mode.dmPelsHeight) });
        }
        const Mode desktop{ GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        bool known = false;
        for (const Mode& m : found) if (m.width == desktop.width && m.height == desktop.height) known = true;
        if (!known && desktop.width > 0) found.push_back(desktop);
        std::sort(found.begin(), found.end(), [](const Mode& a, const Mode& b) { return a.width * a.height < b.width * b.height || (a.width * a.height == b.width * b.height && a.width < b.width); });
        return found;
    }();
    return modes;
}

// The model detail: COD3_MODELDETAIL=0|1 over the settings.
static int ModelDetail(const Settings::Values& v)
{
    static const int detailOverride = []() { const char* t = getenv("COD3_MODELDETAIL"); return t ? (t[0] == '0' ? 0 : 1) : -1; }();
    return detailOverride >= 0 ? detailOverride : v.modelDetail;
}

std::string Settings::TitleCommandsChanged()
{
    // What the title has, to begin with, is its own: the field of view 65,
    // the aim assist on, r_lodscale 1. So the first ask brings whatever the
    // settings change from that, and later ones what changed since. "set"
    // rather than the variable's name alone, because the first ask comes
    // before the title has registered most of them (the renderer's and the
    // game's come later): set makes the variable, and the title's own
    // registration then keeps the value it finds. These are commands on
    // the title's buffer, not lines in a config: the title never runs
    // default.cfg (it only checks the file is there), so lines added to
    // it did nothing.
    static int appliedFov = 65, appliedDetail = 0;
    static bool appliedAimAssist = true;
    const Values v = Get();
    std::string commands;

    // The button prompts. Sent once per run, not once per call: this function is
    // called every frame, so an unconditional append put "set ps2" on the
    // title's buffer hundreds of times a second. A static remembers it went.
    //
    // Once is enough, and is in fact the point - the prompts come out of the
    // title's own font data, and the font is loaded long before the settings
    // menu can be opened, so an early value is the only one likely to be
    // picked up. A change made from the menu is sent directly by the picker.
    //
    // ps2 is the title's own cvar: it is in its variable name table, sorted
    // between ProfileWriteData and r_clear. That it changes the on-screen
    // prompts has not been confirmed by looking at one, so this is the
    // mechanism believed to drive it rather than a verified result.
    static bool sentPrompts = false;
    if (!sentPrompts)
    {
        sentPrompts = true;
        commands += std::string("set ps2 ") + (v.playstationPrompts ? "1" : "0") + "\n";
    }
    if (v.fov != appliedFov)
    {
        appliedFov = v.fov;
        commands += "set cg_fov " + std::to_string(v.fov) + "\n";
    }
    // The model detail is the title's r_lodscale, which scales the distance
    // at which a model steps down to a coarser version: 1 is its own, 0
    // keeps the finest one at every distance.
    if (ModelDetail(v) != appliedDetail)
    {
        appliedDetail = ModelDetail(v);
        commands += std::string("set r_lodscale ") + (appliedDetail ? "0" : "1") + "\n";
    }
    // The title's own aim assist is three switches of its console.
    if (v.aimAssist != appliedAimAssist)
    {
        appliedAimAssist = v.aimAssist;
        const char* on = v.aimAssist ? "1" : "0";
        commands += std::string("set aim_slowdown_enabled ") + on + "\nset aim_lockon_enabled " + on + "\nset aim_autoaim_enabled " + on + "\n";
    }
    if (!commands.empty()) commands.pop_back();   // the last line's end: the queue adds its own
    return commands;
}
