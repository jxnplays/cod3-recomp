#include "overlay.h"
#include "kernel.h"
#include "settings.h"
#include "render_stats.h"
#include "render.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace
{
    // The window thread hands messages in and the command thread draws:
    // Dear ImGui is for one thread at a time, so both take this. Recursive,
    // because the render sends the window a message of its own (the input
    // method's window, placed for the console's text field), and the
    // window procedure runs on the rendering thread, inside the lock.
    std::recursive_mutex g_mutex;

    std::atomic<bool> g_open{ false };
    std::atomic<bool> g_console{ false };
    std::atomic<bool> g_screenshotWanted{ false };

    // The console: what was typed and what was said back, under the
    // overlay's lock. The title's own console was stripped from this build
    // (its "toggleconsole" and its Com_Printf are gone), so this is the
    // host's: the commands go to the title's command buffer, which still
    // runs them, and the answers are only what this side can say.
    std::deque<std::string> g_consoleLines;
    std::vector<std::string> g_consoleHistory;
    int g_consoleHistoryAt = -1;
    char g_consoleInput[1024] = {};
    bool g_consoleFocus = false;
    bool g_consoleScroll = false;

    void ConsoleLine(const std::string& line)
    {
        g_consoleLines.push_back(line);
        if (g_consoleLines.size() > 400) g_consoleLines.pop_front();
        g_consoleScroll = true;
    }

    int ConsoleInputCallback(ImGuiInputTextCallbackData* data)
    {
        // Up and down through the history.
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_consoleHistory.empty()) return 0;
        const int count = int(g_consoleHistory.size());
        if (data->EventKey == ImGuiKey_UpArrow) g_consoleHistoryAt = g_consoleHistoryAt < 0 ? count - 1 : std::max(0, g_consoleHistoryAt - 1);
        else if (data->EventKey == ImGuiKey_DownArrow) g_consoleHistoryAt = g_consoleHistoryAt < 0 ? -1 : (g_consoleHistoryAt + 1 >= count ? -1 : g_consoleHistoryAt + 1);
        data->DeleteChars(0, data->BufTextLen);
        if (g_consoleHistoryAt >= 0) data->InsertChars(0, g_consoleHistory[size_t(g_consoleHistoryAt)].c_str());
        return 0;
    }

    void RunConsoleLine(const std::string& typed)
    {
        std::string line = typed;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) first++;
        line = line.substr(first);
        if (line.empty()) return;
        ConsoleLine("] " + line);
        if (g_consoleHistory.empty() || g_consoleHistory.back() != line) g_consoleHistory.push_back(line);
        if (g_consoleHistory.size() > 100) g_consoleHistory.erase(g_consoleHistory.begin());
        g_consoleHistoryAt = -1;
        if (line == "help" || line == "?")
        {
            ConsoleLine("Commands go on the game's own command buffer, the way the chapter select runs \"spmap\".");
            ConsoleLine("Examples: spmap forest   map_restart   cg_fov 80   timescale 0.5   god   noclip   give all   seta com_maxfps 60");
            ConsoleLine("The game's replies (Com_Printf) were compiled out of this build, so only this side's lines appear here.");
            ConsoleLine("clear: empties the log.   ` or ~ or Esc: closes the console.");
            return;
        }
        if (line == "clear") { g_consoleLines.clear(); return; }
        Kernel::QueueConsoleCommand(line);
    }

    void DrawConsole(int width, int height)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(float(width), float(height) * 0.45f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (!ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End();
            return;
        }
        const float inputHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
        if (ImGui::BeginChild("log", ImVec2(0, -inputHeight), false, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (g_consoleLines.empty()) ImGui::TextDisabled("Console. Type \"help\" for examples.");
            for (const std::string& line : g_consoleLines) ImGui::TextUnformatted(line.c_str());
            if (g_consoleScroll) { ImGui::SetScrollHereY(1.0f); g_consoleScroll = false; }
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        if (g_consoleFocus) { ImGui::SetKeyboardFocusHere(); g_consoleFocus = false; }
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
        if (ImGui::InputText("##input", g_consoleInput, sizeof(g_consoleInput), flags, ConsoleInputCallback))
        {
            RunConsoleLine(g_consoleInput);
            g_consoleInput[0] = 0;
            g_consoleFocus = true;
        }
        ImGui::End();
    }

    std::atomic<bool> g_ready{ false };
    HWND g_hwnd = nullptr;
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    int g_capturing = -1;   // the action whose key is being pressed next, or -1
    Settings::Values g_edit;   // the values the menu shows and changes
    bool g_editLoaded = false;
    float g_fontScale = 1.0f;

    // The frame counter: swaps in the last second.
    std::atomic<uint64_t> g_frames{ 0 };
    uint64_t g_framesAtTick = 0;
    double g_fps = 0.0;
    std::chrono::steady_clock::time_point g_tick;

    // A key the player pressed while a binding waited for one.
    void Capture(int key)
    {
        if (g_capturing < 0 || g_capturing >= Settings::ActionCount) return;
        if (key == VK_ESCAPE) { g_capturing = -1; return; }
        g_edit.keys[g_capturing] = key;
        g_capturing = -1;
    }

    void SaveScreenshot(ID3D11Texture2D* back, int width, int height)
    {
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) return;
        g_context->CopyResource(staging.Get(), back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

        // Beside the executable, named by the moment.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path folder = std::filesystem::path(exe).parent_path() / "screenshots";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char name[64];
        strftime(name, sizeof(name), "CoD3-%Y%m%d-%H%M%S.png", &local);
        const std::filesystem::path file = folder / name;

        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICStream> stream;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(UINT(width), UINT(height))))
        {
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(frame->SetPixelFormat(&format)))
            {
                // The back buffer is B8G8R8A8; its alpha is whatever the
                // present left, so it is made opaque on the way out.
                std::vector<uint8_t> rows(size_t(width) * height * 4);
                for (int y = 0; y < height; y++)
                {
                    const uint8_t* from = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    uint8_t* to = rows.data() + size_t(y) * width * 4;
                    memcpy(to, from, size_t(width) * 4);
                    for (int x = 0; x < width; x++) to[x * 4 + 3] = 255;
                }
                ok = SUCCEEDED(frame->WritePixels(UINT(height), UINT(width) * 4, UINT(rows.size()), rows.data())) &&
                     SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
            }
        }
        g_context->Unmap(staging.Get(), 0);
        printf("screenshot: %s%s\n", file.string().c_str(), ok ? "" : " could not be written");
        fflush(stdout);
    }

    void DrawMenu(int width, int height)
    {
        if (!g_editLoaded) { g_edit = Settings::Get(); g_editLoaded = true; }
        bool changed = false;   // something differs from what is applied
        const Settings::Values applied = Settings::Get();
        ImGui::SetNextWindowSize(ImVec2(620 * g_fontScale, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        bool open = true;
        if (ImGui::Begin("Settings (F11)", &open, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("tabs"))
            {
                if (ImGui::BeginTabItem("Graphics"))
                {
                    const char* renderers[] = { "DirectX 11", "DirectX 12 (not yet)", "Vulkan (not yet)" };
                    int renderer = g_edit.renderer;
                    if (ImGui::Combo("Renderer", &renderer, renderers, 3))
                    {
                        // Only the first exists; the others stay names for now.
                        g_edit.renderer = 0;
                    }
                    if (renderer != 0) ImGui::TextDisabled("Only DirectX 11 is available for now.");
                    const char* modes[] = { "Windowed", "Borderless full screen" };
                    ImGui::Combo("Window mode", &g_edit.windowMode, modes, 2);
                    ImGui::TextDisabled("Alt+Enter switches it too.");
                    {
                        // The display's sizes, and "Desktop" for whatever it is now.
                        const std::vector<Settings::Mode>& modes = Settings::DisplayModes();
                        std::vector<std::string> names;
                        names.push_back("Desktop");
                        int current = 0;
                        for (size_t i = 0; i < modes.size(); i++)
                        {
                            char name[32];
                            snprintf(name, sizeof(name), "%dx%d", modes[i].width, modes[i].height);
                            names.push_back(name);
                            if (g_edit.resolutionWidth == modes[i].width && g_edit.resolutionHeight == modes[i].height) current = int(i) + 1;
                        }
                        if (g_edit.resolutionWidth > 0 && current == 0)
                        {
                            char name[32];
                            snprintf(name, sizeof(name), "%dx%d", g_edit.resolutionWidth, g_edit.resolutionHeight);
                            names.push_back(name);
                            current = int(names.size()) - 1;
                        }
                        std::vector<const char*> items;
                        for (const std::string& name : names) items.push_back(name.c_str());
                        if (ImGui::Combo("Resolution", &current, items.data(), int(items.size())))
                        {
                            if (current == 0) { g_edit.resolutionWidth = 0; g_edit.resolutionHeight = 0; }
                            else if (current <= int(modes.size())) { g_edit.resolutionWidth = modes[size_t(current) - 1].width; g_edit.resolutionHeight = modes[size_t(current) - 1].height; }
                        }
                        ImGui::SliderInt("Resolution scale", &g_edit.resolutionScale, 25, 200, "%d%%");
                        int width, height, drawnWidth, drawnHeight;
                        Settings::Resolution(g_edit, width, height);
                        Settings::DrawnSize(g_edit, drawnWidth, drawnHeight);
                        if (g_edit.resolutionScale == 100)
                            ImGui::TextDisabled("The game is drawn at %dx%d; the window is that size, full screen scales it.", width, height);
                        else
                            ImGui::TextDisabled("The game is drawn at %dx%d and scaled to %dx%d; the window is that size, full screen scales it.", drawnWidth, drawnHeight, width, height);
                    }
                    const char* filters[] = { "The game's own", "Bilinear", "Trilinear", "Anisotropic" };
                    ImGui::Combo("Texture filtering", &g_edit.textureFilter, filters, 4);
                    if (g_edit.textureFilter == 3)
                    {
                        const char* levels[] = { "2x", "4x", "8x", "16x" };
                        int level = g_edit.anisotropy >= 16 ? 3 : g_edit.anisotropy >= 8 ? 2 : g_edit.anisotropy >= 4 ? 1 : 0;
                        if (ImGui::Combo("Anisotropy", &level, levels, 4)) g_edit.anisotropy = 2 << level;
                    }
                    const char* aa[] = { "None", "FXAA", "MSAA 2x", "MSAA 4x", "MSAA 8x", "MSAA 4x + FXAA" };
                    ImGui::Combo("Anti-aliasing", &g_edit.antialiasing, aa, 6);
                    const char* qualities[] = { "Low", "Medium", "High" };
                    ImGui::Combo("Texture quality", &g_edit.textureQuality, qualities, 3);
                    ImGui::SliderInt("Field of view", &g_edit.fov, 65, 100, g_edit.fov == 65 ? "65 (the game's own)" : "%d");
                    const char* details[] = { "The game's own", "Maximum" };
                    ImGui::Combo("Model detail", &g_edit.modelDetail, details, 2);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Maximum: the finest version of every model at any distance)");
                    ImGui::Checkbox("Blur while aiming", &g_edit.aimBlur);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the game's depth of field down the sights)");
                    ImGui::Checkbox("Console depth precision", &g_edit.consoleDepth);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(no flickering surfaces; costs frame rate with MSAA; after a restart)");
                    ImGui::Checkbox("Native command lists (experimental)", &g_edit.nativeCommands);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the draws' state straight from the game's own calls: faster; after a restart)");
                    ImGui::Checkbox("Watch memory writes (experimental)", &g_edit.watchWrites);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(still textures and buffers not looked at again until written: faster; after a restart)");
                    ImGui::Checkbox("Vertical sync (VSync)", &g_edit.vsync);
                    ImGui::Checkbox("Unlimited frame rate in levels", &g_edit.unlockFrameRate);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the menus and films keep the console's 60)");
                    ImGui::Checkbox("Show FPS", &g_edit.fpsOverlay);
                    ImGui::Checkbox("Renderer statistics", &g_edit.statsOverlay);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls"))
                {
                    ImGui::SliderFloat("Mouse sensitivity", &g_edit.mouseSensitivity, 0.1f, 3.0f, "%.2f");
                    ImGui::TextDisabled("Both axes alike.");
                    ImGui::Checkbox("Raw mouse input", &g_edit.rawMouse);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the mouse's own movement, without Windows' pointer speed and acceleration)");
                    ImGui::Checkbox("Aim assist", &g_edit.aimAssist);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the game's own; takes effect after a restart)");
                    ImGui::Checkbox("Controller support", &g_edit.controller);
                    ImGui::Separator();
                    ImGui::TextUnformatted("Keys");
                    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        for (int i = 0; i < Settings::ActionCount; i++)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(Settings::ActionName(Settings::Action(i)));
                            ImGui::TableNextColumn();
                            ImGui::PushID(i);
                            const bool waiting = g_capturing == i;
                            if (ImGui::Button(waiting ? "Press a key... (Esc: cancel)" : Settings::KeyName(g_edit.keys[i]), ImVec2(-1, 0)))
                                g_capturing = waiting ? -1 : i;
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("Default keys"))
                    {
                        const Settings::Values defaults = Settings::Defaults();
                        for (int i = 0; i < Settings::ActionCount; i++) g_edit.keys[i] = defaults.keys[i];
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            // Apply writes and uses the values; Cancel drops the edits.
            const bool dirty = memcmp(&g_edit, &applied, sizeof(Settings::Values)) != 0;
            ImGui::BeginDisabled(!dirty);
            if (ImGui::Button("Apply", ImVec2(140 * g_fontScale, 0)))
            {
                Settings::Set(g_edit);
                g_capturing = -1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140 * g_fontScale, 0)))
            {
                g_edit = applied;
                g_capturing = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(140 * g_fontScale, 0))) open = false;
            if (dirty) { ImGui::SameLine(); ImGui::TextDisabled("(unapplied changes)"); }
            ImGui::TextDisabled("F11: this menu   F12: screenshot   ` (left of 1): console   Esc: back to the game");
        }
        ImGui::End();
        (void)changed;
        if (!open) { g_open.store(false); g_capturing = -1; }
    }
}

void Overlay::Initialize(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_ready.load()) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_hwnd = static_cast<HWND>(hwnd);
    g_device = device;
    g_context = context;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // A readable size on any screen: the font grows with the resolution
    // the picture is drawn at, which is the window's size or the screen's.
    int width = 0, height = 0;
    Settings::Resolution(Settings::Get(), width, height);
    g_fontScale = std::min(2.0f, std::max(1.0f, float(height) / 900.0f));
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0 };   // Latin with the Hungarian letters
    ImFontConfig config;
    config.OversampleH = 2;
    if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f * g_fontScale, &config, ranges) == nullptr)
        io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    {
        // The title's own front end: dark sepia panels, tan text, the
        // chosen entry in a lighter gold, thin brown rules.
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* c = style.Colors;
        const ImVec4 tan(0.86f, 0.80f, 0.64f, 1.00f), gold(0.95f, 0.84f, 0.52f, 1.00f), dim(0.58f, 0.52f, 0.40f, 1.00f);
        const ImVec4 panel(0.10f, 0.085f, 0.065f, 0.94f), panelLight(0.16f, 0.13f, 0.095f, 1.00f);
        const ImVec4 frame(0.22f, 0.18f, 0.13f, 0.95f), frameHover(0.33f, 0.27f, 0.18f, 1.00f), frameActive(0.44f, 0.36f, 0.22f, 1.00f);
        const ImVec4 rule(0.45f, 0.38f, 0.24f, 0.70f);
        c[ImGuiCol_Text] = tan;
        c[ImGuiCol_TextDisabled] = dim;
        c[ImGuiCol_WindowBg] = panel;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.10f, 0.075f, 0.97f);
        c[ImGuiCol_Border] = rule;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = frame;
        c[ImGuiCol_FrameBgHovered] = frameHover;
        c[ImGuiCol_FrameBgActive] = frameActive;
        c[ImGuiCol_TitleBg] = panelLight;
        c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.18f, 0.12f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed] = panel;
        c[ImGuiCol_MenuBarBg] = panelLight;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.07f, 0.05f, 0.8f);
        c[ImGuiCol_ScrollbarGrab] = frameHover;
        c[ImGuiCol_ScrollbarGrabHovered] = frameActive;
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.60f, 0.50f, 0.30f, 1.00f);
        c[ImGuiCol_CheckMark] = gold;
        c[ImGuiCol_SliderGrab] = ImVec4(0.75f, 0.63f, 0.38f, 1.00f);
        c[ImGuiCol_SliderGrabActive] = gold;
        c[ImGuiCol_Button] = ImVec4(0.30f, 0.25f, 0.16f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.46f, 0.38f, 0.23f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.62f, 0.50f, 0.28f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.30f, 0.25f, 0.16f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.46f, 0.38f, 0.23f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.62f, 0.50f, 0.28f, 1.00f);
        c[ImGuiCol_Separator] = rule;
        c[ImGuiCol_SeparatorHovered] = gold;
        c[ImGuiCol_SeparatorActive] = gold;
        c[ImGuiCol_ResizeGrip] = rule;
        c[ImGuiCol_ResizeGripHovered] = gold;
        c[ImGuiCol_ResizeGripActive] = gold;
        c[ImGuiCol_Tab] = ImVec4(0.20f, 0.16f, 0.11f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.46f, 0.38f, 0.23f, 1.00f);
        c[ImGuiCol_TabActive] = ImVec4(0.36f, 0.29f, 0.18f, 1.00f);
        c[ImGuiCol_TabUnfocused] = panelLight;
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.28f, 0.23f, 0.15f, 1.00f);
        c[ImGuiCol_TableHeaderBg] = panelLight;
        c[ImGuiCol_TableBorderStrong] = rule;
        c[ImGuiCol_TableBorderLight] = ImVec4(0.30f, 0.25f, 0.16f, 0.5f);
        c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 0.9f, 0.7f, 0.04f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.62f, 0.50f, 0.28f, 0.45f);
        c[ImGuiCol_NavHighlight] = gold;
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.04f, 0.03f, 0.6f);
        style.WindowRounding = 2.0f;
        style.FrameRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.TabRounding = 2.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
    }
    ImGui::GetStyle().ScaleAllSizes(g_fontScale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(device, context);
    g_tick = std::chrono::steady_clock::now();
    g_ready.store(true);
}

namespace
{
    // The front end's pages seen this frame and the last, as bits.
    std::atomic<uint32_t> g_frontEndNow{ 0 };
    uint32_t g_frontEndLast = 0;
}

void Overlay::NoteFrame()
{
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const uint32_t now = g_frontEndNow.exchange(0);
    // The options page arriving from the main menu: the player pressed
    // OPTIONS, and the settings come up over it. Not when it comes back
    // from one of its own sub pages, and not while the menu is up already.
    const bool wasMain = (g_frontEndLast & uint32_t(FrontEndText::MainMenu)) != 0;
    const bool isOptions = (now & uint32_t(FrontEndText::OptionsMenu)) != 0;
    const bool wasOptions = (g_frontEndLast & uint32_t(FrontEndText::OptionsMenu)) != 0;
    if (isOptions && !wasOptions && wasMain && !g_open.load() && !g_console.load())
    {
        g_open.store(true);
        g_capturing = -1;
        g_editLoaded = false;
    }
    g_frontEndLast = now;
}

void Overlay::NoteFrontEndText(FrontEndText text)
{
    g_frontEndNow.fetch_or(uint32_t(text), std::memory_order_relaxed);
}

bool Overlay::IsOpen() { return g_open.load() || g_console.load(); }

void Overlay::Toggle()
{
    g_open.store(!g_open.load());
    g_capturing = -1;
    g_editLoaded = false;
}

void Overlay::ToggleConsole()
{
    const bool open = !g_console.load();
    g_console.store(open);
    if (open) { g_consoleFocus = true; g_consoleInput[0] = 0; }
}

void Overlay::ConsolePrint(const std::string& line)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ConsoleLine(line);
}

void Overlay::RequestScreenshot() { g_screenshotWanted.store(true); }

bool Overlay::HandleMessage(void* hwnd, uint32_t message, uint64_t wParam, int64_t lParam)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
    {
        if (wParam == VK_F11) { Toggle(); return true; }
        // F12: the picture, and the next frame's draws to the log, so a
        // fault seen while playing can be read afterwards.
        if (wParam == VK_F12) { RequestScreenshot(); Render::LogNextFrame(); return true; }
        // The key left of 1 (scan code 0x29: ` and ~ on a US keyboard, 0
        // on a Hungarian one): the console, as the title's own configs
        // bind it. By scan code, so it is that key on every layout.
        if (((lParam >> 16) & 0xFF) == 0x29 && !g_open.load()) { ToggleConsole(); return true; }
        if (wParam == VK_ESCAPE && g_console.load() && !g_open.load()) { g_console.store(false); return true; }
    }
    // The character that key makes must not land in the console's input.
    if (message == WM_CHAR && ((lParam >> 16) & 0xFF) == 0x29 && g_console.load()) return true;
    if (!g_ready.load() || (!g_open.load() && !g_console.load())) return false;
    // A binding waiting for a key takes the next key or mouse button.
    if (g_capturing >= 0)
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) { Capture(int(wParam)); return true; }
        if (message == WM_LBUTTONDOWN) { Capture(VK_LBUTTON); return true; }
        if (message == WM_RBUTTONDOWN) { Capture(VK_RBUTTON); return true; }
        if (message == WM_MBUTTONDOWN) { Capture(VK_MBUTTON); return true; }
        if (message == WM_XBUTTONDOWN) { Capture(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2); return true; }
    }
    else if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
    {
        g_open.store(false);
        return true;
    }
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), message, WPARAM(wParam), LPARAM(lParam));
    // While the menu is up the keys and the mouse are its.
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
           message == WM_CHAR || message == WM_MOUSEWHEEL || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
           message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP;
}

void Overlay::Render(ID3D11RenderTargetView* back, ID3D11Texture2D* backTexture, int width, int height)
{
    if (!g_ready.load()) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // The counter, once a second.
    {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - g_tick).count();
        if (seconds >= 1.0)
        {
            const uint64_t frames = g_frames.load(std::memory_order_relaxed);
            g_fps = double(frames - g_framesAtTick) / seconds;
            g_framesAtTick = frames;
            g_tick = now;
        }
    }
    // The screenshot first, of the picture alone.
    if (g_screenshotWanted.exchange(false) && backTexture != nullptr) SaveScreenshot(backTexture, width, height);

    const Settings::Values values = Settings::Get();
    const bool open = g_open.load();
    const bool console = g_console.load();
    if (!open && !console && !values.fpsOverlay && !values.statsOverlay) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (values.fpsOverlay || values.statsOverlay)
    {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
        {
            ImGui::Text("%.0f fps", g_fps);
            if (values.statsOverlay)
            {
                // The renderer's own counters, averaged over the last second.
                const RenderStats::Snapshot s = RenderStats::Current();
                ImGui::Text("frame %.2f ms  cpu %.2f ms  gpu %s", s.frameMilliseconds, s.cpuMilliseconds,
                    s.gpuProfiled ? "" : "(COD3_GPU_PROFILE=1)");
                if (s.gpuProfiled) { ImGui::SameLine(); ImGui::Text("%.2f ms", s.gpuMilliseconds); }
                ImGui::Text("%.0f draws  %.0f k triangles  %.0f resolves  %.0f skipped", s.draws, s.triangles / 1000.0f, s.resolves, s.skipped);
                ImGui::Text("switches: %.0f programs  %.0f textures  %.0f states  %.0f targets", s.shaderSwitches, s.textureSwitches, s.pipelineSwitches, s.targetSwitches);
                ImGui::Text("uploads: %.0f textures  %.0f buffers  %.0f KB;  streamed %.0f KB", s.textureUploads, s.bufferUploads, s.uploadKilobytes, s.streamKilobytes);
            }
        }
        ImGui::End();
    }
    if (open) DrawMenu(width, height);
    else if (console) DrawConsole(width, height);
    ImGui::Render();
    ID3D11RenderTargetView* targets[1] = { back };
    g_context->OMSetRenderTargets(1, targets, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
