# Button icons

Controller button artwork for the settings menu and the mod menu: the glyph
beside a menu row, the button letter next to a keybind.

## What is here

Two sets, extracted from the free button icon packs by Zacksly and stored
under **the same logical names**:

    assets/buttons/xbox/a.png     assets/buttons/ps5/a.png
    assets/buttons/xbox/b.png     assets/buttons/ps5/b.png
    ...                          ...

`a`, `b`, `x`, `y`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `up`, `down`,
`left`, `right`, `start`, `back`, `home`, `lstick`, `rstick`.

That is the whole point of the layout: **nothing outside `button_icons.cpp`
asks which set it is drawing.** The drawing code asks for "the confirm
button" and the selected set decides whether that is a Cross or an A. Adding
a Nintendo set later means extracting one more folder and adding one line to
the table, not touching any calling code.

## PlayStation is the default

Most people playing a PC build of this today are holding a PlayStation
controller, through an adapter or because that is the pad they own. Drawing
an A on a button that says Cross on their hardware reads as sloppy. It is a
default, not a ruling — Xbox is one click away in F11, and so is a
letters-only mode that uses no images at all.

## Where they go at run time

`CoD3.exe` looks for a `buttons\` folder **beside itself**, not beside the
repository. Copy `assets\buttons` to `build\CoD3Host\buttons` after building,
or the menu says *"No icons found"* and falls back to text. It says so rather
than drawing nothing, which is the point.

## Decoding

Through **WIC**, which is Windows' own PNG decoder and ships with the OS. The
alternative — vendoring stb_image — would put someone else's permissive
licence and a megabyte of header into a repository that has no business
acquiring an image decoder for a menu. The images are decoded once, on first
use, and cached.

`COD3_ICONLOG=1` says which stage a load failed at. A silent `false` from a
five-call chain is exactly the kind of thing that gets guessed at for an hour
instead of measured.

## Licences and a caveat worth reading

Both source packs are free to use and redistribute:

- **Zacksly's Xbox Series and PlayStation button sets** —
  <https://www.patreon.com/zacksly>
- The `Gamepad-Asset-Pack` by AL2009man is MIT and covers a wider range of
  controllers if more sets are wanted; its own README notes that most of its
  assets are "ripped straight from the official source", and that is worth
  reading before pulling anything else in.

The caveat: **these are Microsoft's, Sony's and Nintendo's trademarks.** They
are used here to label which physical button a player should press, which is
the nominative use the marks exist for, and the port is not being sold. It is
still worth knowing what is in the tree, because a commercial release would
need a different answer. See `docs/GAPS.md`.