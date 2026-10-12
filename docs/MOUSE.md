# Mouse look: what was measured, and what is still wrong

Worked on 12 October 2026. The mouse works, and four real bugs were fixed along
the way, but the feel is still not what a PC shooter gives and this note is
what is known, so the next attempt starts here rather than from nothing.

## Fixed, and verified

**The mouse died whenever the window lost focus.** It was released and nothing
gave it back: the only ways to take it were F1, the middle button, and the first
time in a level. So alt-tabbing away and back left the look doing nothing until
the player clicked, which is indistinguishable from a broken mouse. Given back
now when the focus returns or the settings menu closes. `Escape` still releases
it deliberately, which is how the pointer is freed for the taskbar.

**The settings menu's sliders needed a click on Apply to take effect.** A
sensitivity dragged to 12 that then needed a second click somewhere else is
indistinguishable from a setting that does nothing - and the value was written
to disk, so the next run started hot, which made the next attempt wrong too.
Sensitivity and DPI now apply as they are moved.

**Input delivery.** The counts are read from a running total instead of being
emptied on each read, so whether a frame's movement has been delivered is no
longer a race against the message pump. A frame's movement is published on the
first ask of that frame, so both asks see the same deflection as a console
gives them, and it is published when asked for rather than at the frame
boundary, which had put every movement a whole frame behind the hand.

**The clamp no longer carries.** Overflow used to be paid out over the
following frames so that fast movement was not lost. That made the view keep
turning after the hand stopped - a drag behind every flick. Dropped.

## Measured

    message pump          540 a second, every 1.85 ms
    total input latency   ~1.85 ms + up to one frame (~7 ms at 144 fps)
    aim mapping           MAP_LINEAR, config/ts_def.cfg
    stick saturates       62 counts, 0.2 cm of hand at 850 DPI

The frame-sized part of the latency is structural: the title samples input once
a frame and no host change removes it. CS at 144 fps has the same.

**The aim mapping is linear**, `bindaxis A_RSTICK_X VA_YAW MAP_LINEAR` in
`config/ts_def.cfg`. There is no response curve to compensate for. Several
attempts assumed there was, and every one of them was wrong.

**COD3's own Horizontal and Vertical Sensitivity is controller-side.** It does
not scale what the host sends for the mouse. It was mistaken for the mouse's
speed lever for most of one session, which cost time.

## What is still wrong

The feel. Reports have been "floaty", "dragging", "slides", "not synced to the
hand", and "cannot do a 360". These were partly conflated all night:

- **Lag** - now about 9 ms, which should not feel floaty. Not confirmed fixed
  by the player after the change that got it there.
- **Rotation rate** - the stick saturates almost immediately, so the title is
  handed full deflection and still turns slowly. This is unexplained.

Note the distinction: *floaty* and *lagging behind* are latency; *rotating too
little* is a rate. They were treated as one problem and should not be again.

## The leads that were not exhausted

**The title's maximum turn rate is the ceiling, and it is a constant.** The
mapping is linear, so the rate is `stick * sensitivity * something`. The stick
cannot exceed full deflection, so the fastest the view can turn is set by that
constant. A controller reaches the same ceiling, and a controller feels right -
which is the strongest clue that the mouse should be able to reach it too, and
currently does not feel like it does.

**The host reports every profile setting as unset.** `XamUserReadProfileSettings`
in `kernel_xam.cpp` answers `type: 0` for all of them, so the title falls back
to its own defaults for everything, and nothing it writes back persists. The
eight setting ids it asks for are:

    10040018 10040015 10040002 10040003 4064000F 63E83FFF 63E83FFE 63E83FFD

Returning a real value instead of "unset" is a way to change settings the
title's own menu cannot reach, and the ids above are where to start. Which one
is horizontal sensitivity has not been established.

**How the look is measured.** `scripts/mouse_measure.py` drives the mouse with
SendInput and correlates the screen before and after to find how far a known
number of counts actually moves the picture. It should have been written first
and several hours of guessing would have been avoided. It is unreliable when
something is moving in the picture - a walking player, a moving soldier - and
`scripts/mouse_symmetry.py` uses it to check that left and right are the same,
which is worth doing: the title loads the stick with `lhz`, which zero-extends,
so a negative value arrives as a large positive one.

**The player's actual input** is in `COD3_MOUSELOG=1`, which reports counts,
peak stick deflection and how often the clamp was reached. That peak is the
number to reason from. It is what showed a real hand reaching seven per cent of
deflection, which was why the look felt dead.

## How this went wrong, for the next time

A measurement tool existed in the idea the whole time and was not written until
the end. Instead: a turn rate at full stick was invented, gains were fitted to
it, and constants were changed on the strength of how the game felt. Three
consecutive gains were wrong by 4x, 20x and 41x, one of them killed the mouse
outright, and one of them made the message pump spin and killed the game at 58
seconds. Each of those would have been one run of a log line.

The rule this project already had - measure, do not infer - was broken for most
of a session by exactly the person who wrote it down.
