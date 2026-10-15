#pragma once
// The title's own debug commands, put in the menu.
//
// See docs/DEV_COMMANDS.md for how these were found, because the first
// answer about them was wrong twice. The short names - god, noclip, notarget,
// setviewpos - are the ones the first search found, and testing them showed
// they did nothing. They were the wrong names. The commands are
// GAME_GODMODEON and GAME_GODMODEOFF, GAME_NOTARGETON and GAME_NOTARGETOFF,
// GAME_NOCLIPON and GAME_NOCLIPOFF, GAME_UFOON and GAME_UFOOFF, and they sit
// at 0x82065CB8..0x82065D30, beside a "fog %s" command and the string
// "GAME_CHEATSNOTENABLED".
//
// "GAME_CHEATSNOTENABLED" is what the title prints when they are refused, so
// something has to turn them on first. It is sv_cheats, which is in the
// title's variable table at 0x82061F48. Nothing here assumes that is the
// only gate; it is simply the one that was found.
namespace Cheats
{
    // The state the menu shows. Every one of these starts off, on purpose:
    // a cheat that is on when you did not ask for it is worse than one that
    // is missing.
    struct State
    {
        bool gateSent = false;    // sv_cheats 1 has gone out this level
        bool god = false;
        bool noTarget = false;
        bool noClip = false;
        bool fogOff = false;
    };
    State& Get();

    // Turn one on or off. Sending on for the first time also opens the gate,
    // without which the title answers with GAME_CHEATSNOTENABLED and does
    // nothing at all - which is exactly what "the button did nothing" looks
    // like from the player's side, and is why this is worth doing in one
    // place rather than at four call sites.
    void SetGod(bool on);
    void SetNoTarget(bool on);
    void SetNoClip(bool on);
    void SetFogOff(bool on);

    // "give ammo" and "give all", which need the gate and change nothing
    // themselves. Nothing here claims they work: see the note in
    // SetGod about what was and was not measured.
    void GiveAmmo();
    void GiveAll();

    // A level starting turns every one of them off and forgets the gate.
    // Called from the one place a level is loaded, so it cannot be missed.
    // Without this, god mode in one level is god mode in the next one, which
    // is not a thing anyone asked for.
    void LevelStarted();
}
