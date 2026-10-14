// Controller button icons, and which set to draw them from. See
// button_icons.cpp for why PS5 is the default, why the two sets share
// logical names, and why the PNGs are decoded through WIC.
#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace ButtonIcons
{
    // How many sets there are, and what to call them in the picker.
    int Count();
    const char* SetName(int set);

    // PS5. Most players of a PC build today hold a PlayStation pad, and an A
    // on a button that says Cross on their hardware reads as sloppy.
    int DefaultSet();

    // Draws the icon for a logical button ('a', 'l2', 'start', ...). Returns
    // false when there is no icon, so the caller can fall back to text rather
    // than drawing nothing.
    bool Draw(int set, const char* logical, float size);

    // Hands over the device the icons are made with. Called once, when the
    // overlay has one to draw with. Not taken from ImGui internally: that
    // struct is private in this version of imgui and would break on the next
    // upgrade, and the overlay already holds the device.
    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context);

    // True when not a single icon could be loaded. Used to say so once,
    // instead of silently showing no glyphs.
    bool Missing();
}