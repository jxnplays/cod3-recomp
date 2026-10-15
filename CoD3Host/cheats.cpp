#include "cheats.h"

#include "kernel.h"

namespace
{
    Cheats::State g_state;
    bool g_mutexHeld = false;

    // The gate, once per level and only when something wants it.
    void OpenGate()
    {
        if (g_state.gateSent) return;
        g_state.gateSent = true;
        Kernel::QueueConsoleCommand("sv_cheats 1");
    }

    void Toggle(bool& flag, const char* on, const char* off, bool on_)
    {
        if (flag == on_) return;      // no command for a state we are already in
        if (on_) OpenGate();
        flag = on_;
        Kernel::QueueConsoleCommand(on_ ? on : off);
    }
}

Cheats::State& Cheats::Get()
{
    return g_state;
}

void Cheats::SetGod(bool on)
{
    Toggle(g_state.god, "GAME_GODMODEON", "GAME_GODMODEOFF", on);
}

void Cheats::SetNoTarget(bool on)
{
    Toggle(g_state.noTarget, "GAME_NOTARGETON", "GAME_NOTARGETOFF", on);
}

void Cheats::SetNoClip(bool on)
{
    Toggle(g_state.noClip, "GAME_NOCLIPON", "GAME_NOCLIPOFF", on);
}

void Cheats::SetFogOff(bool on)
{
    // The fog command is not one of the gated debug commands - it is a level
    // author's control and it was measured working without sv_cheats - so it
    // does not open the gate.
    if (g_state.fogOff == on) return;
    g_state.fogOff = on;
    Kernel::QueueConsoleCommand(on ? "fog off" : "fog on");
}

void Cheats::GiveAmmo()
{
    OpenGate();
    Kernel::QueueConsoleCommand("give ammo");
}

void Cheats::GiveAll()
{
    OpenGate();
    Kernel::QueueConsoleCommand("give all");
}

void Cheats::LevelStarted()
{
    // The state first, then the commands: if one of these faults or the level
    // is not ready yet, the menu still shows the truth rather than claiming a
    // cheat that is not on.
    const bool god = g_state.god, noTarget = g_state.noTarget, noClip = g_state.noClip, fogOff = g_state.fogOff;
    g_state = State();
    if (god) Kernel::QueueConsoleCommand("GAME_GODMODEOFF");
    if (noTarget) Kernel::QueueConsoleCommand("GAME_NOTARGETOFF");
    if (noClip) Kernel::QueueConsoleCommand("GAME_NOCLIPOFF");
    if (fogOff) Kernel::QueueConsoleCommand("fog on");
}
