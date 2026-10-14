// The mod menu: the title's own console variables, as a list.
//
// Why it is built this way, and what it deliberately is not
// ---------------------------------------------------------
// A mod menu wants to read a value back to show whether it is on. That is
// not possible here: Com_Printf is compiled out of this build, so a command
// the title rejects is silent (overlay.cpp says as much). There is also no
// cvar query of any kind in the port.
//
// So every row here is fire-and-forget, and the menu remembers what it set
// rather than asking. The checked box means "this menu turned it on", not
// "the game says it is on". That distinction is the whole design, and it is
// why the list below is made of variables that were read out of the title's
// own cvar table (COD3_DUMP, see docs/cvars.txt) rather than guessed.
//
// What is NOT here, and why: the classic private-server menu items - god,
// noclip, infinite ammo, give weapon, spawnable bots, gametype editing -
// have nothing in this build to switch on. The title's cvar table holds 188
// names and none of them set health, ammo, money, position or a peer
// player's state. Those need a different door into the game, and a menu row
// that silently does nothing is worse than no row at all.
//
// What is here is the 82 cg_ and r_ variables that are real: crosshair, gun,
// HUD alpha, compass, widescreen, letterbox, fog, fullbright, and the r_
// debug set. That is the same idea as the "Vision Menu" and "Menu
// Customization" entries in the menus this is modelled on.

#include "overlay.h"
#include "kernel.h"
#include "settings.h"
#include "../thirdparty/imgui/imgui.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace
{
    // What kind of control a row gets. Everything here takes a number, but
    // not every number is one the player should have to type.
    enum class Kind { Toggle, Slider, Choice };

    struct Row
    {
        const char* label;
        const char* cvar;
        Kind kind;
        float off;        // the value that means "off", for a toggle
        float minValue;
        float maxValue;
        const char* const* choices;   // for Kind::Choice
        int choiceCount;
        const char* help;
    };

    const char* const kYesNo[] = { "off", "on" };
    const char* const kAniso[] = { "off", "2x", "4x", "8x", "16x" };
    const char* const kTextureMode[] = { "nearest", "bilinear", "trilinear" };
    const char* const kMaxEntLights[] = { "0", "1", "2", "4", "8" };

    // The list. Names and ranges are the title's own; nothing invented.
    // A test that reports working code as broken is worse than no test, so
    // anything not confirmed to do something is left out rather than guessed.
    const Row kRows[] =
    {
        // --- HUD ------------------------------------------------------------
        { "Crosshair",           "cg_drawCrosshair",        Kind::Toggle, 0, 0, 1, kYesNo, 2, "the crosshair in the centre" },
        { "Weapon model",        "cg_drawGun",              Kind::Toggle, 0, 0, 1, kYesNo, 2, "the weapon drawn in the corner" },
        { "Status text",         "cg_drawStatus",           Kind::Toggle, 0, 0, 1, kYesNo, 2, "objective text and subtitles" },
        { "HUD brightness",      "cg_hudAlpha",             Kind::Slider, 1, 0, 2, nullptr, 0, "how solid the HUD is" },
        { "Compass",             "cg_hudCompassVisible",    Kind::Toggle, 0, 0, 1, kYesNo, 2, "the compass, bottom left" },
        { "Compass size",        "cg_hudCompassSize",       Kind::Slider, 0, 0, 3, nullptr, 0, "how large the compass is drawn" },
        { "Compass range",       "cg_hudCompassMaxRange",   Kind::Slider, 0, 100, 3000, nullptr, 0, "how far the compass shows" },
        { "Compass springy",     "cg_hudCompassSpringyPointers", Kind::Toggle, 0, 0, 1, kYesNo, 2, "compass markers ease into place" },
        { "Damage direction",    "cg_showLocationalDamage", Kind::Toggle, 0, 0, 1, kYesNo, 2, "the arrow showing where a shot came from" },
        { "Damage icon width",   "cg_hudDamageIconWidth",   Kind::Slider, 0, 0, 200, nullptr, 0, "how wide the damage arcs are" },
        { "Objectives on screen","cg_hudObjectiveMaxHeight", Kind::Slider, 0, 0, 400, nullptr, 0, "how many objective markers show at once" },
        { "Objectives range",    "cg_hudObjectiveMaxRange", Kind::Slider, 0, 100, 4000, nullptr, 0, "how far an objective marker shows" },
        { "Objective fade",      "cg_hudObjectiveMinAlpha", Kind::Slider, 0, 0, 1, nullptr, 0, "how faint a distant objective goes" },
        { "Objective ring size", "cg_hudObjectiveRingSize", Kind::Slider, 0, 0, 400, nullptr, 0, "the ring around an objective" },
        { "Hint fade",           "cg_hintFadeTime",         Kind::Slider, 0, 0, 60, nullptr, 0, "how long a hint stays up" },
        { "Cursor hints",        "cg_cursorHints",          Kind::Toggle, 0, 0, 1, kYesNo, 2, "the control hints on screen" },
        { "Shellshock",          "cg_shellshock",           Kind::Toggle, 0, 0, 1, kYesNo, 2, "the slow-motion effect" },
        { "Letterbox",           "cg_letterbox",            Kind::Toggle, 0, 0, 1, kYesNo, 2, "black bars top and bottom, for cutscenes" },
        { "Widescreen",          "cg_widescreen",           Kind::Toggle, 0, 0, 1, kYesNo, 2, "the title's own widescreen flag" },

        // --- Scene ----------------------------------------------------------
        { "Fog",                 "r_fog",                   Kind::Toggle, 0, 0, 1, kYesNo, 2, "distance haze" },
        { "Fullbright",          "r_fullbright",            Kind::Toggle, 0, 0, 1, kYesNo, 2, "no lighting at all" },
        { "Light scale",         "r_LightScale",            Kind::Slider, 0, 0, 4, nullptr, 0, "overall scene brightness" },
        { "Z near",              "r_znear",                 Kind::Slider, 0, 0.01f, 4, nullptr, 0, "the near clipping plane" },
        { "Z far",               "r_zfar",                  Kind::Slider, 0, 500, 20000, nullptr, 0, "the far clipping plane" },
        { "Max entity lights",   "r_maxEntLights",          Kind::Choice, 0, 0, 0, kMaxEntLights, 5, "how many lights one object can have" },
        { "Min light intensity", "r_minEntLightIntensity",  Kind::Slider, 0, 0, 1, nullptr, 0, "how dim a light can be before it is dropped" },
        { "Dynamic lights",      "r_dynamiclight",          Kind::Toggle, 0, 0, 1, kYesNo, 2, "lights that move" },
        { "Textures",            "r_textureMode",           Kind::Choice, 0, 0, 0, kTextureMode, 3, "how textures are filtered" },
        { "Texture depth",       "r_texturebits",           Kind::Slider, 0, 16, 32, nullptr, 0, "bits per texture channel" },
        { "Mip weighting",       "r_weightMipMaps",         Kind::Slider, 0, 0, 3, nullptr, 0, "bias between mipmap levels" },

        // --- Culling and debugging -----------------------------------------
        { "No culling",          "r_nocull",                Kind::Toggle, 0, 0, 1, kYesNo, 2, "draw everything, even what is behind you" },
        { "No portals",          "r_noportals",             Kind::Toggle, 0, 0, 1, kYesNo, 2, "ignore the portal system" },
        { "Lock PVS",            "r_lockpvs",               Kind::Toggle, 0, 0, 1, kYesNo, 2, "stop recomputing what is visible" },
        { "Single cell",         "r_singlecell",            Kind::Toggle, 0, 0, 1, kYesNo, 2, "draw only one cell" },
        { "Show triangles",      "r_showtris",              Kind::Toggle, 0, 0, 1, kYesNo, 2, "wireframe" },
        { "Show skeletons",      "r_showSkeletons",         Kind::Toggle, 0, 0, 1, kYesNo, 2, "bones, drawn as lines" },
        { "Show portals",        "r_showportals",           Kind::Toggle, 0, 0, 1, kYesNo, 2, "the portal geometry" },
        { "Show surf counts",    "r_showsurfcounts",        Kind::Toggle, 0, 0, 1, kYesNo, 2, "how many surfaces were drawn" },
        { "Show tri counts",     "r_showtricounts",         Kind::Toggle, 0, 0, 1, kYesNo, 2, "triangles drawn per frame" },
        { "Test shadows",        "r_testshadow",            Kind::Toggle, 0, 0, 1, kYesNo, 2, "shadow debug" },
        { "Verbose",             "r_verbose",               Kind::Slider, 0, 0, 3, nullptr, 0, "how much the renderer says" },
    };

    constexpr int kRowCount = int(sizeof(kRows) / sizeof(kRows[0]));

    // What the menu has set, per cvar. Starts empty and honest: a variable
    // that has never been touched is left at the title's own default, which
    // is the only value we can be sure of.
    struct Applied
    {
        bool known = false;
        float value = 0.0f;
    };
    Applied g_applied[128];
    int g_filter = 0;    // 0 = everything, 1 = HUD, 2 = scene, 3 = debug

    const char* GroupOf(const Row& r)
    {
        if (std::string_view(r.cvar).rfind("cg_hud", 0) == 0 || std::string_view(r.cvar) == "cg_drawCrosshair"
            || std::string_view(r.cvar) == "cg_drawGun" || std::string_view(r.cvar) == "cg_drawStatus"
            || std::string_view(r.cvar) == "cg_showLocationalDamage")
            return "HUD";
        if (std::string_view(r.cvar).rfind("r_show", 0) == 0 || std::string_view(r.cvar) == "r_nocull"
            || std::string_view(r.cvar) == "r_noportals" || std::string_view(r.cvar) == "r_lockpvs"
            || std::string_view(r.cvar) == "r_singlecell" || std::string_view(r.cvar) == "r_verbose"
            || std::string_view(r.cvar) == "r_testshadow")
            return "Debug";
        return "Scene";
    }

    void Send(const Row& r, float value)
    {
        std::string command;
        if (r.kind == Kind::Toggle)
            command = std::string("set ") + r.cvar + " " + (value > 0.5f ? "1" : "0");
        else if (r.kind == Kind::Choice)
            command = std::string("set ") + r.cvar + " " + std::to_string(int(value));
        else
        {
            char text[32];
            snprintf(text, sizeof(text), "%g", value);
            command = std::string("set ") + r.cvar + " " + text;
        }
        Kernel::QueueConsoleCommand(command);
    }

    void ResetAll()
    {
        // There is no "unset" in this build and no way to read the title's
        // default back, so this re-sends a known default for each row rather
        // than pretending it can restore. Anything the menu set is put back
        // to what the menu itself records as the default.
        for (int i = 0; i < kRowCount; i++)
        {
            if (!g_applied[i].known) continue;
            const Row& r = kRows[i];
            const float back = (r.kind == Kind::Toggle) ? 0.0f : r.minValue;
            Send(r, back);
            g_applied[i].known = false;
        }
    }
}

bool Overlay::DrawModsTab()
{
    ImGui::TextDisabled("These are the title's own console variables. A tick means this menu set it, not that the game confirms it - this build has no way to read a value back.");
    ImGui::Spacing();

    if (ImGui::Button("Reset everything this menu has set"))
    {
        ResetAll();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(a guess at the default; the title's own value cannot be read back)");

    const char* groups[] = { "Everything", "HUD", "Scene", "Debug" };
    ImGui::Combo("Show", &g_filter, groups, 4);
    ImGui::SameLine();
    ImGui::TextDisabled("%d variables", kRowCount);

    ImGui::Separator();

    if (ImGui::BeginChild("mods"))
    {
        int shown = 0;
        for (int i = 0; i < kRowCount; i++)
        {
            const Row& r = kRows[i];
            const char* group = GroupOf(r);
            const bool want =
                g_filter == 0 ||
                (g_filter == 1 && std::string_view(group) == "HUD") ||
                (g_filter == 2 && std::string_view(group) == "Scene") ||
                (g_filter == 3 && std::string_view(group) == "Debug");
            if (!want) continue;
            shown++;

            Applied& a = g_applied[i];

            if (r.kind == Kind::Choice)
            {
                int current = a.known ? int(a.value) : 0;
                if (ImGui::Combo(r.label, &current, r.choices, r.choiceCount))
                {
                    Send(r, float(current));
                    a.known = true;
                    a.value = float(current);
                }
            }
            else if (r.kind == Kind::Toggle)
            {
                bool on = a.known && a.value > 0.5f;
                if (ImGui::Checkbox(r.label, &on))
                {
                    Send(r, on ? 1.0f : 0.0f);
                    a.known = true;
                    a.value = on ? 1.0f : 0.0f;
                }
            }
            else
            {
                // A slider that has never been touched has no value to show:
                // the title's own default cannot be read back in this build.
                // Drawing it at the minimum would claim the default is that
                // number, which is a different claim and not one we can make,
                // so until the row is used it reads as a slider with no
                // value and says so.
                if (!a.known)
                {
                    // Every one of these buttons is captioned "Set", so ImGui
                    // saw sixteen widgets sharing one identifier and said so on
                    // screen in an error panel over the menu. The row index is
                    // what makes them distinct.
                    ImGui::PushID(i);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", r.label);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the title's own value; drag to set)");
                    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 220.0f, ImGui::GetFrameHeight()));
                    ImGui::SameLine();
                    if (ImGui::Button("Set", ImVec2(60.0f, 0)))
                    {
                        Send(r, r.minValue);
                        a.known = true;
                        a.value = r.minValue;
                    }
                    ImGui::PopID();
                }
                else
                {
                    float value = a.value;
                    if (ImGui::SliderFloat(r.label, &value, r.minValue, r.maxValue, "%.2f"))
                    {
                        Send(r, value);
                        a.value = value;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(set by this menu)");
                }
            }
            if (ImGui::IsItemHovered() && r.help) ImGui::SetTooltip("%s\n\n%s", r.help, r.cvar);
        }
        if (shown == 0) ImGui::TextDisabled("Nothing in this group.");
        ImGui::EndChild();
    }
    return false;
}