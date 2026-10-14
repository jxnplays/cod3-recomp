// Controller button icons, and choosing which set to draw them from.
//
// Why images at all
// -----------------
// The prompts this draws - the glyph beside a menu row, the button letter
// next to a keybind - are what a player notices first in a port and what they
// judge it by. Text approximations ("[A]") are prototype work. These are
// proper artwork: Zacksly's Xbox Series and PlayStation button sets, used
// under their own terms (assets/buttons/README.md).
//
// Both sets are extracted under the SAME logical names - a, b, x, y, l1, l2,
// start and so on - so nothing outside this file asks which set it is drawing.
// That is what makes the choice a setting rather than a code path: the
// drawing code asks for 'the confirm button' and the selected set decides
// whether that is a Cross or an A.
//
// Why PlayStation is the default
// ------------------------------
// Because most people playing a PC build of this today are holding a
// PlayStation controller, through an adapter or because that is the pad they
// own. Showing an A on a button that says Cross on their hardware reads as
// sloppy. It is a default, not a ruling: Xbox is one click away in F11.
//
// Decoding
// --------
// Through WIC, which is Windows' own PNG decoder and ships with the OS. The
// alternative - vendoring stb_image - would put someone else's permissive
// licence and a megabyte of header into a repository that currently has no
// image decoder at all and no business acquiring one for a menu.

#include "button_icons.h"

#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../thirdparty/imgui/imgui.h"

namespace
{
    const char* const kLogical[] =
    {
        "a", "b", "x", "y",
        "l1", "r1", "l2", "r2", "l3", "r3",
        "up", "down", "left", "right",
        "start", "back", "home",
        "lstick", "rstick",
    };
    constexpr int kLogicalCount = int(sizeof(kLogical) / sizeof(kLogical[0]));

    // The device the icons are made with, handed in by the overlay. See
    // ButtonIcons::SetDevice for why this is not taken from ImGui's own backend
    // data, which is private in this version of imgui.
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;

    struct Icon
    {
        ImTextureID texture = 0;
        int width = 0;
        int height = 0;
        bool attempted = false;
    };

    // [set][button]. Small and fixed, so no allocation.
    Icon g_icons[2][kLogicalCount];
    bool g_wicReady = false;

    int IndexOf(const char* logical)
    {
        for (int i = 0; i < kLogicalCount; i++)
            if (std::string(kLogical[i]) == logical) return i;
        return -1;
    }

    std::wstring IconPath(int set, const char* logical)
    {
        wchar_t exe[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir;
        if (n > 0)
        {
            dir.assign(exe, n);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) dir.resize(slash);
        }
        dir += L"\\buttons\\";
        dir += (set == 1 ? L"ps5" : L"xbox");
        dir += L"\\";
        for (const char* p = logical; *p; p++) dir += wchar_t(*p);
        dir += L".png";
        return dir;
    }

    bool EnsureWic()
    {
        if (g_wicReady) return true;
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // S_FALSE means it was already initialised on this thread, which is
        // fine; RPC_E_CHANGED_MODE would not be, and is reported as a failure.
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
        g_wicReady = true;
        return true;
    }

    // Decode one PNG and hand it to ImGui's device. Returns false on any
    // failure; the icon is then simply not drawn and the caller falls back to
    // text, which is why every path here is allowed to fail quietly.
    bool LoadPng(const std::wstring& path, ImTextureID& outTexture, int& outWidth, int& outHeight)
    {
        if (!EnsureWic()) return false;

        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) || factory == nullptr)
            return false;

        bool ok = false;
        const char* stage = "decoder open";
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        // COD3_ICONLOG=1 says why a load failed. The project rule is measure
        // rather than infer: a silent false from a five-call chain is exactly
        // the kind of thing that gets guessed at for an hour.
        static const bool verbose = getenv("COD3_ICONLOG") != nullptr;

        auto fail = [&]()
        {
            if (verbose)
            {
                // The path is wide, and this repository lives under a path with
                // a space in it. Printing it with a narrow %s truncated it at
                // the space, so the log said "X failed" - which is worse than
                // useless, because it looks like a bad file name.
                WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
                const int needed = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string narrow(size_t(needed > 0 ? needed - 1 : 0), '\0');
                if (needed > 0)
                    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow.data(), needed, nullptr, nullptr);
                printf("icons: %s failed at %s\n", narrow.c_str(), stage);
                fflush(stdout);
            }
            if (converter) converter->Release();
            if (frame) frame->Release();
            if (decoder) decoder->Release();
            factory->Release();
            return false;
        };

        stage = "open";
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder))) return fail();
        stage = "frame";
        if (FAILED(decoder->GetFrame(0, &frame))) return fail();
        stage = "converter";
        if (FAILED(factory->CreateFormatConverter(&converter))) return fail();
        // The size is taken from the frame, not the converter: this build of
        // WIC has no GetWidth on IWICFormatConverter, and the two are the same
        // image anyway.
        stage = "initialize";
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPRGBA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom))) return fail();

        UINT imageWidth = 0, imageHeight = 0;
        stage = "size";
        if (FAILED(frame->GetSize(&imageWidth, &imageHeight))) return fail();
        const UINT width = UINT(imageWidth);
        const UINT height = UINT(imageHeight);
        stage = "size sanity";
        if (width == 0 || height == 0 || width > 1024 || height > 1024) return fail();

        const UINT stride = width * 4;
        std::vector<uint8_t> pixels(size_t(stride) * height);
        stage = "pixels";
        if (FAILED(converter->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data()))) return fail();

        // ImGui owns textures through the renderer backend, so the texture is
        // made with the same device it draws with. ImGui_ImplDX11 is reached
        // The device is handed in by the overlay (see SetDevice below), not
        // taken from ImGui's backend data, which is private in this version.
        stage = "no device";
        if (g_device == nullptr || g_context == nullptr) return fail();

        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ID3D11Device* device = g_device;
        stage = "texture";
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) || texture == nullptr)
            return fail();

        stage = "map";
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
            for (UINT row = 0; row < height; row++)
                memcpy(dst + size_t(row) * mapped.RowPitch, pixels.data() + size_t(row) * stride, stride);
            g_context->Unmap(texture, 0);

            ID3D11ShaderResourceView* view = nullptr;
            if (SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &view)) && view != nullptr)
            {
                // ImGui keeps the texture in the DX11 backend's own registry,
                // which frees it with the backend. Nothing to release here.
                outTexture = (ImTextureID)view;
                outWidth = int(width);
                outHeight = int(height);
                ok = true;
            }
            else if (view) view->Release();
        }
        texture->Release();
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return ok;
    }
}

namespace ButtonIcons
{
    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        g_device = device;
        g_context = context;
    }

    int Count() { return 2; }

    const char* SetName(int set) { return set == 1 ? "PlayStation (DualSense)" : "Xbox (Xbox Series)"; }

    int DefaultSet() { return 1; }

    bool Draw(int set, const char* logical, float size)
    {
        if (set < 0 || set > 1) set = 0;
        const int index = IndexOf(logical);
        if (index < 0) return false;

        Icon& icon = g_icons[set][index];
        if (icon.texture == 0 && !icon.attempted)
        {
            icon.attempted = true;   // once, so a missing file is not retried every frame
            LoadPng(IconPath(set, logical), icon.texture, icon.width, icon.height);
        }
        if (icon.texture == 0) return false;

        // Square icons; scaled to the requested size, which the caller passes in
        // the same units as the font so they sit on the text's baseline.
        const float scale = size / float(icon.width > icon.height ? icon.width : icon.height);
        ImGui::Image(icon.texture, ImVec2(float(icon.width) * scale, float(icon.height) * scale));
        return true;
    }

    bool Missing()
    {
        // True when nothing at all could be loaded, which the caller uses to
        // say so rather than silently drawing no icon.
        for (int set = 0; set < 2; set++)
            for (int i = 0; i < kLogicalCount; i++)
                if (g_icons[set][i].texture != 0) return false;
        return true;
    }
}