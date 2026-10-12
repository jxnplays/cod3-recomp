"""A catalogue of the engine functions the level scripts call.

    python scripts/builtins.py <image.bin> [--level chambois] [--markdown out.md]

CoD3's level scripts are C++ ("Broc", c:\\cod\\code\\script\\include\\*.inl)
compiled into each level's DLL. They reach the engine through one table of
function pointers, which the level owns: sub_824BB360 loads the level's DLL
and calls its first export with &*(0x82A2A2A0) and an engine block at
0x82A909C0, the level puts its table's address there, and sub_824BB028 then
fills it: from two lists of (byte offset, function) pairs in the executable's
data (0x825F5638, 265 pairs; 0x825F5E80, 222 pairs), and one slot at a time
in sub_824B9D70 and six other functions (another 517), and the three the
level copies from the engine block: 1007 in all. The level fills
0xAE4 to 0xB74 with functions of its own that the engine calls back (those
are in docs/scripts.md, not here). The level code calls
`lwz rX,offset(table) / mtctr rX / bctrl` with the arguments already in
registers, so the names of these built-ins are nowhere in the game: only
their offsets and what they do.

For each entry this prints the offset, the function, where it came from, how
many times the level's code and all the levels' call through that offset
(the table's register followed through each function), the argument
registers the function reads before it writes them, the notable functions
it calls, the text it refers to, and its name (NAMES below).
The image is COD3_IMAGEDUMP's (0x82000000, sixteen megabytes).
"""
import collections
import glob
import os
import re
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
image = open(sys.argv[1], 'rb').read()
level = sys.argv[sys.argv.index('--level') + 1] if '--level' in sys.argv else 'chambois'
markdown = sys.argv[sys.argv.index('--markdown') + 1] if '--markdown' in sys.argv else None


def word(address):
    return struct.unpack_from('>I', image, address - 0x82000000)[0]


def text(address):
    if not 0x82000000 <= address < 0x82000000 + len(image):
        return None
    start = address - 0x82000000
    end = image.find(b'\0', start, start + 200)
    raw = image[start:end]
    if len(raw) >= 3 and all(32 <= c < 127 for c in raw):
        return raw.decode()
    return None


# What is known of the functions the built-ins call.
KNOWN = {
    0x824534B0: 'va',
    0x820CEBB8: 'hash a name',
    0x824FE120: 'set goal weight',
    0x824FE410: 'set knob all',
    0x82500A40: 'set anim',
    0x82500B80: 'set anim limited',
    0x824F3F50: 'clear tree',
    0x82554D60: 'update model/tree',
    0x82555B98: 'set entity tree',
    0x823D0950: 'actor tree',
    0x82538A28: 'anim time',
    0x825389E8: 'anim weight',
    0x821276D0: 'free',
    0x82127A20: 'alloc',
    0x8234EBA0: 'memset',
    0x82144CA8: '(empty print)',
    0x82539518: '(empty print)',
}

# Names worked out by hand, by offset.
NAMES = {
    # animation (an anim argument is (anim list << 16) | entry)
    0x674: 'setflaggedanimknoball (sub_824FE410, with a notify)',
    0x678: 'the anim\'s frame count (class +68)',
    0x67C: 'getanimlength (class +56; a random node\'s chosen child)',
    0x680: 'animhasnotetrack (the entry\'s notetrack, by hash)',
    0x684: 'clearanim (anim, blend time)',
    0x688: 'setanimknob (siblings let go, then set anim)',
    0x68C: 'setanim (sub_82500A40)',
    0x690: 'setflaggedanimknob',
    0x694: 'setanimknoball (sub_824FE410)',
    0x698: 'setflaggedanim',
    0x69C: 'getanimtime (info record +16)',
    0x6A0: 'useanimtree ("cannot change the animtree of classname")',
    0x6A4: 'clearanimtree (tree dropped, freed at the frame end)',
    0x6B0: 'stops every child of a node at once',
    # the rest by what they do
    0x4: 'println (the print is empty in this build)',
    0x8: 'nothing (an empty function)',
    0x118: 'isalive (actor +784, or the entity\'s health +776)',
    0x11C: 'isdefined for an entity reference',
    0x140: 'randomint (a linear congruential generator)',
    0x144: 'randomfloat',
    0x14C: 'randomfloatrange',
    0x42C: 'gettime (the level\'s time, ms)',
    0x4E0: 'the system clock',
    0xAC4: 'delete (the script heap, else the general one)',
    0xAC8: 'new (the script heap)',
    0xACC: 'free (the script heap)',
    0x5DC: 'plays a sound alias (sub_824927C0)',
    0xF8: 'plays a sound alias or a spoken line (sub_824B3FF0)',
    0x5C: 'whether the event system exists',
    0x1C: 'sprintf (the C runtime\'s)',
    0x124: 'isai (the entity has an actor at +532)',
    0x148: 'randomintrange',
    0x590: 'linkto',
    0x5A0: 'delete (sub_82540D78 frees the entity)',
    0xAAC: 'fadeovertime (a HUD element)',
    0xAC0: 'new, from the fixed pools first',
    0x0: 'print (formatted; the print is empty in this build)',
    0xC: 'nothing (an empty function)',
    0x10: 'nothing (an empty function)',
    0x14: 'nothing (an empty function)',
    0x18: 'nothing (an empty function)',
    0x20: "strcpy (the C runtime's)",
    0x24: "strncpy (the C runtime's)",
    0x48: 'nothing (an empty function)',
    0x64: 'nothing (an empty function)',
    0x68: 'nothing (an empty function)',
    0x94: 'getent / getentarray (value, key: classname, targetname, target, groupname)',
    0x98: "the entity's model (+500)",
    0xA0: "sets or clears flag 2 on the entity model's record (sub_8251B798)",
    0xA4: 'getnodearray (path nodes by value and key, sub_823C9948)',
    0xA8: 'the path nodes nearest a point within a radius (sub_823C3688)',
    0xAC: 'getnode (sub_823C96B0)',
    0xB0: 'the number of clients in use',
    0xB4: 'nothing (an empty function)',
    0xB8: 'playloopsound on an entity, unless it has one already (+904)',
    0xBC: 'playsoundatposition (sub_82535788)',
    0xC0: 'playsound on an entity, with a notify (sub_82535460)',
    0xC4: 'playloopsound on an entity, stopping the one it had (+904)',
    0xC8: 'playsound on an entity (sub_82535568)',
    0xCC: 'stopsound (a sound handle)',
    0xD0: 'whether a sound handle still plays (sub_82528E38)',
    0xD4: 'a switch of the sound system (sub_8252D5B8, flag in r4)',
    0xD8: "sets a sound handle's volume (sub_82523350)",
    0xDC: "plays one of a vehicle's eleven sounds (+528, sub_82534ED0)",
    0xEC: "a zone by name hash: sub_82452308 on the bank set's +400 list",
    0xF0: "a zone by name hash: sub_82452270 on the bank set's +400 list",
    0xF4: 'sets the global flag byte at 0x82A2A262',
    0xFC: 'plays a sound alias or a spoken line, without waiting (sub_824B3FF0)',
    0x100: 'whether a sound alias exists (sub_82114B80)',
    0x104: 'sets the float at 0x825CC3E0, no lower than a floor',
    0x108: 'sets the float at 0x825CC3E4, no lower than a floor',
    0x10C: 'clears the three words at 0x82A2A30C',
    0x110: "a trace from the entity's origin (sub_8247EA20)",
    0x114: 'sets the level\'s "tosser" settings by name (sub_824814C8)',
    0x120: 'isvehicle (+528)',
    0x128: 'isplayer (+536)',
    0x12C: 'isturret (+540)',
    0x130: 'the turret is of type 2',
    0x134: 'the actor is in state 11',
    0x138: 'a handle is set (not 0, not -1)',
    0x13C: 'a handle is not -1',
    0x174: 'abs',
    0x184: 'distance',
    0x188: 'distancesquared',
    0x190: 'lengthsquared',
    0x198: 'vectordot',
    0x38: "starts the level's first script thread (a 56-byte thread object: function r6, self r7; run at once if r8)",
    0x44: "sub_82499EA8 on the script VM's +48 list with a table of the level's data",
    0x60: 'isdefined for an entity reference (a second copy)',
    0x6C: "the running thread's handle (+20), made on first use",
    0x70: 'kills a thread by its handle (flags 4, 8 and 64)',
    0x88: "sub_8252A4A8 on the entity's +512 object",
    0x194: 'closer: whether a is nearer b than c',
    0x19C: 'vectornormalize (out, in)',
    0x1B4: "the screen's size as two floats (sub_820E9210, sub_820E8C58)",
    0x1B8: 'sets the float at 0x825D0F70',
    0x1BC: "sets a flag (+1452) on the local player's vehicle, and the byte at 0x82A2A0CB",
    0x1C0: 'nothing (an empty function)',
    0x1C4: 'nothing (an empty function)',
    0x1C8: 'adds an entity to, or takes it from, a list of 32 at 0x82A92390 (entity +636 flag 128)',
    0x24C: 'getdvar as a string',
    0x250: "getdvarint (the dvar's integer at +32)",
    0x254: "getdvarfloat (the dvar's float at +28)",
    0x258: 'setdvar (a string)',
    0x25C: 'setdvar (an integer, "%d")',
    0x260: 'setdvar (a float, "%f")',
    0x264: 'spawn (classname, origin, spawn flags)',
    0x268: 'spawn, with a model (sub_82543568)',
    0x26C: 'spawn a trigger with a box (mins, maxs)',
    0x270: 'spawn a trigger box by name hash',
    0x274: 'spawn a character (a "cdChar" model)',
    0x278: 'spawnturret ("leftarc", "rightarc")',
    0x27C: 'spawnturret, with a model',
    0x280: 'musicplay (sub_824897A0 on the music player at *(0x82A2A2CC))',
    0x284: 'musicstop',
    0x288: 'musicplay with a fade time (sub_824895C0)',
    0x28C: "the music's volume over a time (player +20, +24, +28)",
    0x290: 'plays a 2D sound by alias, at a volume (sub_82487DC8 on the mixer at *(0x82A2A2C4))',
    0x294: 'stops a 2D sound by its handle (sub_82478A58)',
    0x298: 'sets a sound mix, second slot only (sub_82484EC8)',
    0x29C: 'sets a sound mix over a time (sub_82484EC8)',
    0x2A0: 'sets a sound mix, both slots (sub_82484EC8)',
    0x2A4: 'a sound alias by name hash: sub_82119A18',
    0x2A8: "sets a named channel's volume over a time (sub_82465E90)",
    0x2AC: 'fades a named channel (sub_82465F08)',
    0x2B0: 'a sound alias by name hash: sub_82119A98',
    0x2B4: 'a sound alias by name hash: sub_82119AC8',
    0x2B8: 'a sound alias by name hash: sub_82119B18',
    0x2BC: 'a sound alias by name hash: sub_82119B88',
    0x2C0: 'setreverb ("Preset_Alley" and the rest, sub_824661F0)',
    0x2C4: 'sets the global flag byte at 0x82A2A0D3',
    0x2C8: 'loads a sound bank (.wbk; sub_82494E90 on *(0x82A2A2C0))',
    0x2CC: 'unloads a sound bank (sub_82494C70)',
    0x2D4: "a path node's +4, by node reference",
    0x2D8: "sets the player's integer at +1920",
    0x2DC: "sets the player's float at +1352",
    0x2F4: 'autosave (a checkpoint by name, e.g. "post_igc"; sub_82476B20 on the game state at *(0x82A2A300))',
    0x2F8: 'a saved game variable as a vector, by name hash',
    0x2FC: 'a saved game variable as an integer, by name hash (0xFEFEFEFE when absent)',
    0x300: 'a saved game variable as a float, by name hash',
    0x304: 'sets a saved game variable to a vector',
    0x308: 'sets a saved game variable to an integer',
    0x30C: 'sets a saved game variable to a float',
    0x328: 'nothing (an empty function)',
    0x32C: 'a game statistic (sub_8251B100, when statistics are on)',
    0x330: 'a game statistic counter (0x829C2910 table), optionally plus its base',
    0x334: 'sets a game statistic counter',
    0x338: 'adds one to a game statistic counter',
    0x33C: 'takes one from a game statistic counter, not below 0',
    0x340: 'sets a bit in the game statistics bit field',
    0x344: 'sub_82519410 on the game statistics',
    0x348: 'a game statistic update (sub_8251AF08)',
    0x34C: 'starts a timed shake at an origin (strength, time, radius; a pool at 0x82A9F280)',
    0x350: "moves a timed shake's origin",
    0x354: 'stops a timed shake',
    0x358: "sets this slot's byte in a table of 14 at 0x82A8E4F8",
    0x35C: 'sets three render floats at 0x825A2338',
    0x360: 'sets the render float at 0x825CC5A4',
    0x364: 'sets the render floats at 0x825A2344 and 0x825CCE94',
    0x368: 'sets the render float at 0x825A2318',
    0x36C: 'sets a render block at 0x825FBEF0: on, 0, then five floats (a fog)',
    0x370: 'moves the render value at 0x825CDA9C to f2 over f1 seconds (at once when f1 is 0)',
    0x374: 'sets the render float at 0x825CDABC',
    0x378: 'sets an entry (r3, r4) of a table of 100 at 0x82A11D90 to f1 (sub_82416260, sub_82415468)',
    0x37C: 'sets an entry (r3, r4) of a table of 500 at 0x82A158F8 (sub_82415140, sub_82414F68)',
    0x380: 'sets the render float at 0x825CDBE0',
    0x384: 'sets a render ramp at 0x825CDB38 (five floats: from, to, over, and a second pair)',
    0x388: 'sub_82415BB8 with the table at 0x82A11D4C',
    0x38C: 'sets a render block at 0x82909D70: on, r10, seven floats',
    0x390: 'sets two floats of the render block at 0x82909D70 (+64, +68)',
    0x394: "sets a float of the world's render settings (*(0x82A2AD7C) +128 -> +20)",
    0x398: "sub_823F6758 on the world's render settings, and sets 0x825A2158",
    0x39C: "sets the entity's render flag 1 (+644)",
    0x3A0: "sets or clears the entity's render flag 4 (+644)",
    0x3A4: 'sets the render values at 0x825CC590 (an integer and three floats)',
    0x3A8: "sets the entity's render flag 8 (+644)",
    0x3AC: 'nothing (an empty function)',
    0x3B0: "sets or clears the entity's render flag 16 (+644)",
    0x3B8: 'sub_82147098 with a flag and three floats kept at 0x82A2A0C8',
    0x3C0: 'nothing (an empty function)',
    0x3C4: 'sets the render float at 0x825CDBA8',
    0x3C8: 'nothing (an empty function)',
    0x3CC: "sub_820A9BF0 on the entity's model instance (+508)",
    0x3D0: "sets the entity's flags 0x02000000 (+632) and 8 (+228)",
    0x3D4: "sets two half-words of the level's settings (level +20 -> +4, +8)",
    0x3D8: "sets two words of the level's settings (level +20 -> +4, +12)",
    0x3DC: "whether the entity's model instance has bit 0 of +196 clear",
    0x3E0: 'sets a float and a word at 0x82A565EC',
    0x3E4: 'sets the state of a HUD slot (3 or 0; sub_824D1C10, sub_824CB000)',
    0x3E8: 'a HUD element by name hash (sub_824D9670)',
    0x3EC: "sets three of the entity's flags (+632: 0x10000000 and two more)",
    0x3F0: "sets or clears the entity's flags 0x04100000 (+632)",
    0x3F4: "sets or clears the entity's flag 0x20000000 (+632) and a half-word of its +548 object",
    0x3F8: 'applies an integer and a float to the matching entry of every loaded bank (sub_82404F38)',
    0x400: 'a 36-byte "ANFY" record with two name hashes (sub_820CE818)',
    0x404: 'three words of a named record (sub_8251A370 on *(0x82A2A198), by name hash)',
    0x414: "sets a float (+232) of the entity's +548 object",
    0x418: 'sub_8246F4D0 on two entities',
    0x41C: 'sub_820A1838 with seven floats (a physics push?)',
    0x420: 'sub_820A1838 at two positions with forces',
    0x424: "adds an id to, or takes it from, the game state's list of 256 (+856, count +1368)",
    0x428: "whether an id is in the game state's list of 256",
    0x430: 'the float at 0x825F4E14',
    0x434: 'a string from the table at 0x825D0F60 by the index at 0x829B1B7C',
    0x45C: 'nothing (an empty function)',
    0x460: 'nothing (an empty function)',
    0x464: 'the entity and origin of the record at *(0x82A2ACE4)',
    0x468: 'getstartorigin (origin, angles, anim; sub_824FE5B0)',
    0x46C: 'getstartangles (origin, angles, anim; sub_824FE5B0)',
    0x470: "an anim's start relative to an origin (sub_8252E838, sub_824FE5B0)",
    0x474: "getmovedelta (an anim's translation; sub_824FE698)",
    0x478: "getangledelta (an anim's rotation; sub_824FE698, sub_82531A00)",
    0x47C: "the local player's float at +5712 (a table of 5840-byte entries)",
    0x484: 'a trace between two points (mask 0x0280E033, or 0x00802033 with the flag; sub_8247EA20)',
    0x488: 'a trace with more options (sub_82485D38)',
    0x48C: 'nothing (an empty function)',
    0x490: 'nothing (an empty function)',
    0x494: 'a message of category 9 (sub_82513C98)',
    0x498: 'radiusdamage (origin, range, max, min; sub_82567510)',
    0x49C: 'radiusdamage with an attacker entity',
    0x4A0: "sets the game state's word at +3104 (0x82A4F3A0)",
    0x4B4: 'earthquake (scale, duration, origin, radius; sub_82439058)',
    0x4B8: 'a timed shake with its defaults (the pool at 0x82A9F280)',
    0x4BC: 'sets the global flag byte at 0x82A2ABEA',
    0x4C0: 'sets the script string at 0x82A55B50',
    0x4C4: "sets the game state's word at +3096",
    0x4C8: 'sub_825411F0 (with 12)',
    0x4CC: "sets the game state's float at +3084 (scaled)",
    0x4D0: 'sub_82438AB8 with two positions',
    0x4D8: 'nothing (an empty function)',
    0x4DC: 'nothing (an empty function)',
    0x4E4: 'a null entry (no function)',
    0x4E8: "a registry entry by name for the level's bank (sub_8242F4A8 on *(0x82A2ABB8))",
    0x4EC: "a registry entry by name for the level's bank, with its values (sub_8242F4A8)",
    0x504: 'a path node lookup by name (sub_823CB668, sub_823D0230)',
    0x510: 'takes a free slot of five kinds in a table of 124-byte entries at 0x829B01B8',
    0x514: "sub_823CC598 on the level's path nodes",
    0x518: 'a zone by name (sub_82459D20 on the bank set)',
    0x51C: "the zone of an entity's bank (+496, or the level's)",
    0x520: 'the zone that holds a point (sub_8241C0F8)',
    0x524: 'whether a zone is active (its +196 is 0)',
    0x528: 'sets a float (+68) of an object and counts the change at 0x825CE6BC',
    0x40: "starts a script thread waiting for an entity's notify (an AeThreadEntityNotifyState)",
    0x4C: 'wait (an AeThreadWaitState of f1 seconds, then the thread yields)',
    0x50: 'waits a number of frames (an AeThreadWaitFramesState)',
    0x54: 'waits for a pak (a zone or bank) to be ready, with a time limit (an AeThreadPakNotifyState)',
    0x52C: 'resets a float (+68) of an object to its default (1)',
    0x530: 'activates a zone (sub_82462FB0)',
    0x538: 'nothing (an empty function)',
    0x53C: 'nothing (an empty function)',
    0x540: 'nothing (an empty function)',
    0x544: 'nothing (an empty function)',
    0x548: 'nothing (an empty function)',
    0x54C: '0x550 with a string argument held on the thread',
    0x550: 'plays an animation on the entity with a notify (sub_824B22C0)',
    0x554: "ends the actor's scripted state (state 15; sub_823C8D40, entity +924)",
    0x558: 'puts the actor in state 8 (sub_823C7760, sub_823C8DA8)',
    0x55C: 'takes the actor from state 8 to 2',
    0x560: 'attach (model, tag, with a bank; sub_825585E8)',
    0x564: 'attach (model, tag)',
    0x568: 'attach (model, no tag)',
    0x56C: 'detach (model, tag)',
    0x570: 'detach (model, no tag)',
    0x574: 'detachall (the seven attachments at +932; sub_82558598)',
    0x578: 'getattachsize (the seven slots at +928)',
    0x57C: 'getattachmodelname',
    0x580: 'getattachtagname',
    0x584: "whether bit r4 of the entity's +621 is set (a hidden part)",
    0x588: 'linkto with a tag and offsets',
    0x58C: 'linkto with a tag',
    0x594: 'puts the entity on a vehicle, copying its origin (sub_8254A928)',
    0x598: 'puts the entity on a vehicle by tag (sub_8254A928)',
    0x59C: 'puts the entity on a vehicle, untagged (sub_8254A928)',
    0x10D8: "registers an object on the running thread's stack with its destructor (a BrocDtor<T>, e.g. for Broc::string), so a killed thread still destroys it (sub_824A6040)",
    0x10DC: "unregisters an object from the running thread's list (sub_82499D38)",
    0x3C: "thread: starts a script thread at a function of the level's code (with the .bro file, line and function name for debugging)",
    0x9C: 'registers a level function for a named AI behaviour state ("anim_main_init", "anim_main_combat"...; sub_8251BBC8 on *(0x82A2A1A0))',
    0x3FC: 'registers a level function for a notetrack name ("end", "finished", "footstep"...)',
    0x408: 'starts drones along a spline (name, counts, spacing; sub_8251E520 on the drone system at *(0x82A2A1A0))',
    0x40C: 'a drone spline by name ("spline_event1_center"...; sub_8251E7A0)',
    0x410: 'a drone group by name ("ev7_ally_drn_grp_1"...; sub_8251E6C0)',
    0x534: "prof_begin (a stub that returns 0; the argument is the function's name)",
    0xAD4: 'assert (disabled: file, line, message such as "out of bounds")',
    0xAD8: 'a script warning (disabled: file, line, message)',
    0xADC: 'a script error (disabled: file, line, message such as "Cannot wait for undefined time")',
    0x150: 'sine and cosine of an angle in degrees (sin to *r4, cos to *r5)',
    0x154: 'sin (radians; checked by running it)',
    0x158: 'cos (radians; checked by running it)',
    0x15C: 'tan (radians; checked by running it)',
    0x160: 'asin (checked by running it)',
    0x164: 'acos (checked by running it)',
    0x168: 'atan (checked by running it)',
    0x170: 'log (natural; checked by running it)',
    0x180: "sub_8234C850 (the C runtime's)",
    0x1A0: 'vectortoangles (out, in; checked by running it)',
    0x1A4: 'an up vector from pitch and roll, the yaw left out (checked by running it)',
    0x1A8: 'anglestoright (out, angles; checked by running it)',
    0x1AC: 'anglestoforward (out, angles; checked by running it)',
    0x1B0: 'anglevectors: forward, right and up from angles (checked by running it)',
    0x2D0: 'whether a weapon exists, by name (sub_82464FD0 looks it up in the weapon table *(0x82A2A2E0))',
    0x3BC: 'a grenade-type projectile at a position by weapon name ("grenade" handled apart; sub_823F79C0)',
    0x438: 'getaiarray (a team; "getai")',
    0x440: 'getaiarray (every team)',
    0x444: 'getspawnerteamarray ("getspawnerteamarray")',
    0x448: 'a weapon\'s class, checked ("unknown weapon \'%s\' in getWeaponClassname")',
    0x44C: "a weapon's display name (its definition's +1432)",
    0x450: 'getweaponclassname',
    0x454: 'getweaponclassname for a second weapon slot',
    0x458: 'two names for one (sub_82509840), appended to a string',
    0x4A4: 'missionsuccess / changelevel: the next level\'s name to the game state\'s +2780, the next mission unlocked unless it is "creditsdone", +2756 set for "gamefinished"',
    0x4A8: "missionfailed (reason): the failure message to the game state's +2768, +2764 set, +2744 the time plus 500 ms",
    0x4AC: "the mission's end with a time only (the game state's +2776)",
    0x4B0: "the mission's end with a text (copied to the game state's +2780; +2752 and +2760 set)",
    0x4D4: 'magicbullet (weapon, start, end; "MagicBullet called with unknown weapon name")',
    0x4F0: "a weapon's float by name (its definition in the weapon table)",
    0x4F4: "a weapon's value by name",
    0x4F8: "a weapon's integer by name",
    0x4FC: "a weapon's second integer by name",
    0x500: "a weapon's string by name",
    0x508: 'badplace_cylinder (name, time, origin, radius, height; "badplace_cylinder")',
    0x50C: 'badplace (a second shape)',
    0x5A4: 'whether the entity is a live trigger_multiple',
    0x614: 'disconnectpaths ("cannot disconnect paths. Make sure you have a collmap")',
    0x618: 'connectpaths ("cannot connect paths. Make sure you have a collmap")',
    0x5A8: 'dospawn with a flag (sub_824B92A8: "dospawn can only be called on actor spawners")',
    0x5AC: 'dospawn with a flag, the name held on the thread',
    0x5B0: 'dospawn (sub_824B9118)',
    0x5B4: 'dospawn, the name held on the thread (562 calls in the levels)',
    0x5B8: "a point of the entity's bounds (+236, +272..+296)",
    0x5BC: 'geteye ("BIP01 HEAD"; the player\'s or actor\'s eye, sub_823D5308)',
    0x5C0: "sub_824D1500 on the level's HUD (level +20 -> +4)",
    0x5C4: "sub_824C9B50 on the level's HUD (level +20 -> +4)",
    0x5C8: 'sub_820A0840 on two entities',
    0x5CC: "istouching (the other entity's bounds; sub_82516268)",
    0x5D0: "whether the entity was hit this frame (+568 against the level's time)",
    0x5D4: 'whether the entity was hit this frame (a second test)',
    0x5D8: 'whether the entity was hit this or the last frame, and its +884',
    0x5E0: 'sub_824721C8 on the entity, with a flag',
    0x5E4: 'whether a registry entry exists for a name in a bank (sub_8242F4A8)',
    0x5E8: "a registry entry for the entity's bank by name (sub_8242F4A8)",
    0x5EC: 'setmodel (a "cdChar" model; the team from "allies", "axis", "neutral")',
    0x5F0: "sets the byte +252 of the entity's sub_824721C8 record",
    0x5F4: "the entity's health as a fraction (a vehicle's of its +1448)",
    0x5F8: 'setnormalhealth ("setNormalHealth must be greater than 0")',
    0x5FC: 'moves a vehicle toward a point at a speed (sub_82564888; "player_tank")',
    0x600: "sets the entity's +624 (takedamage)",
    0x604: 'sub_8246BFB0 on the entity with a flag',
    0x608: 'show (clears flag 0x400 of +632)',
    0x60C: 'hide (sets flag 0x400 of +632)',
    0x610: 'setcontents (the old +324 returned; sub_82513198)',
    0x61C: 'a turret method (+544)',
    0x620: 'a turret method (+544)',
    0x624: 'a turret method with a target entity (sub_8256BE08)',
    0x62C: 'a turret method returning a value (+544, +619)',
    0x630: "the entity's +432",
    0x634: "sets the entity's +432 from another entity",
    0x638: 'a turret method with a second entity',
    0x63C: 'a turret method (+544)',
    0x640: 'a turret method (+544)',
    0x644: 'setturretteam ("axis", "allies")',
    0x648: 'a turret method (+544)',
    0x64C: 'a turret method (+544)',
    0x650: "sets a turret's float (+544)",
    0x654: 'a turret method (+544)',
    0x658: 'a turret method returning a float (+544)',
    0x65C: "a turret method with the entity's +500 (+544)",
    0x660: 'setcursorhint ("HINT_INHERIT", "List of valid hint type strings")',
    0x664: "the use hint's text by index (+444, +2668)",
    0x668: 'sethintstring ("Too many different hintstring values")',
    0x66C: 'sethintstring by a localized name hash',
    0x670: 'a localized line\'s key ("MOVIE", "%s_%s"; sub_824DEC18)',
    0x6A8: 'nothing (an empty function)',
    0x6AC: 'the level\'s script animtree ("script_animtree", "script_name")',
    0x6B4: 'animscripted (an anim from an origin and angles; sub_824F8F70, sub_8254B398)',
    0x6B8: 'takes an actor off its vehicle (sub_82541430; actor +2532)',
    0x6BC: 'puts an actor on a vehicle seat by tag ("tag_passenger1"; sub_82541530)',
    0x6C0: 'whether the actor is on a vehicle (+2536)',
    0x6C4: "lets go of an actor's vehicle (+2537; sub_82540D78)",
    0x6C8: 'takes an actor off its vehicle, checked',
    0x6CC: 'puts an actor on a vehicle seat, checked ("tag_driver", "tag_gunner", "tag_passenger1")',
    0x6D0: 'an AI state change ("ignoring AI state transition"; sub_8254BD78, sub_823C8DA8)',
    0x6D4: 'an AI state change (sub_823C8DA8)',
    0x6D8: "sets up a turret's arcs and speeds (three floats; a record for +540, sub_82547EC8)",
    0x6DC: 'sub_82547EC8 on the turret (+540)',
    0x6E0: "the actor's +440",
    0x6E4: "the vehicle's name as a script string (+528)",
    0x6E8: 'magicgrenade ("MagicGrenade: None of the sScrMethods worked")',
    0x6EC: "magicgrenade, from the level's player",
    0x6F0: 'throws a grenade from an actor ("grenadethrow"; f1 the fuse)',
    0x6F4: "throws a grenade, from the level's player",
    0x6F8: 'fires a rifle grenade ("m1garand_RG", with g_gravity)',
    0x6FC: 'a turret method (+544)',
    0x700: 'the actor\'s grenade toss ("aiTosser")',
    0x704: 'gettagorigin (by tag; sub_824AF658)',
    0x708: 'gettagorigin by tag name',
    0x70C: 'gettagangles (by tag; sub_82532898)',
    0x710: 'gettagangles by tag name',
    0x714: 'shellshock (a type such as "death", "pain", "default"; "duration %g should be >= 0 and <= 60")',
    0x718: "stopshellshock (clears the player's +1288..+1296)",
    0x71C: 'viewkick ("viewkick: damage %g < 0")',
    0x720: 'nothing (an empty function)',
    0x724: 'nothing (an empty function)',
    0x728: 'a small entity method (no fields beyond the lookup)',
    0x72C: 'a small entity method (no fields beyond the lookup)',
    0x730: "a level-time test of the entity's +616",
    0x734: 'a direction from the entity to a point (sub_82530EA8)',
    0x738: 'sets the entity\'s +500 (called with "prone")',
    0x73C: "sets one of the entity's +632 flags",
    0x740: "sets one of the entity's +632 flags",
    0x744: "sets one of the entity's +632 flags",
    0x748: "sets one of the entity's +632 flags",
    0x74C: "sets the entity's contents and bounds (+228..+264, +324; sub_82513198)",
    0x750: 'setteam ("unknown team \'%s\', should be axis, allies, or neutral"; +884)',
    0x754: 'sets a turret float (+544)',
    0x758: 'sets a turret float (+544)',
    0x75C: 'sets a turret arc (+544)',
    0x760: 'sets a turret arc (+544)',
    0x764: 'a turret method (+544)',
    0x768: 'a turret method (+544)',
    0x76C: 'setmodel (a wrapper of 0x5EC)',
    0x770: 'loads an animation by name (".anim", "generic_human"; sub_824FA748)',
    0x774: 'a localized string by name hash ("_XBOX360", "STRING MISSING"; sub_8252A638)',
    0x310: 'a player profile entry by name, off (sub_823FA7D8 on *(0x825CCD7C): achievements and progress)',
    0x314: 'a player profile entry by name, on (sub_823FA7D8)',
    0x318: 'writes the player profile (sub_823FA528)',
    0x31C: 'campaign progress ("PROGRESS_COMPLETE", "HARDCORE_COMPLETE", "PROGRESS_US"...; sub_823FA8D8)',
    0x320: 'rich presence ("RICH PRESENCE: Setting player on controller %d"; sub_823FB088)',
    0x324: 'a player profile entry by name (sub_823FBB18)',
    0x43C: 'getaiarray, the team held on the thread',
    0x628: 'setmode for a turret ("manual", "auto_ai", "manual_ai", "auto_nonai")',
    0xA88: 'a HUD element\'s text ("localized string")',
    0xA8C: 'setshader on a HUD element ("width %i < 0", "height %i < 0")',
    0xA90: 'settimer ("setTimer")',
    0xA94: 'settimerup ("setTimerUp")',
    0xA98: 'settenthstimer ("setTenthsTimer")',
    0xA9C: 'settenthstimerup ("setTenthsTimerUp")',
    0xAA0: 'setclock ("setClock")',
    0xAA4: 'setclockup ("setClockUp")',
    0xAB0: 'scaleovertime ("scale time %g <= 0")',
    0xAB4: 'moveovertime ("move time %g <= 0")',
    0xABC: "a HUD element's config string (sub_824AABA8)",
    0xAD0: "memset (the title's, also in the table)",
    0x178: 'sqrt',
    0x17C: 'the remainder of an integer division',
    0xA84: "sets the level settings' bytes +63 and +64 to the opposite of a flag",
    0xAA8: "sets a HUD element's value (state 2, the value at +100; the 124-byte entries at 0x829B01B8)",
    0xAB8: 'resets a HUD element to its defaults (state 1)',
    0xB88: "atof (the C runtime's, sub_8234C240)",
    0xB8C: 'atoi (strtol base 10)',
    0xB90: 'the name hash of a string (sub_820CEBB8)',
    0xB94: "the entity's +1020 object, made on first use through slot 0xAE4",
    0xB98: "the player entity's +500",
    0xB9C: "the local client's entity's +500",
    0xBA0: 'adds a word to the list at 0x82A90988',
    0xBA4: "copies a string from slot 0xB50's function into a buffer",
    0xBB4: 'sets two fog floats (0x82A2ABAC, 0x825CCE6C)',
    0xBB8: "sets the fog's start and end distance (0x82A2ABA8, 0x825CCE68; e.g. 420, 9500)",
    0xBBC: "sets the fog's colour from three bytes",
    0xBC0: "sets the fog's colour (0x825CCE80..88)",
    0xBC4: 'sets the render float at 0x825A088C',
    0xBC8: 'sets the render float at 0x825A0890',
    0xBCC: 'sets the render float at 0x825A0894',
    0xBD0: 'sets the byte at 0x82909E6C',
    0xBD4: 'sets the render word at 0x825A0898',
    0xBDC: 'looks an id up in the map at 0x82A90994 (sub_820B33A0)',
    0xBE0: 'the name hash of a string (sub_820A0118)',
    0xBE4: "starts a scene animation by name from the current stream zone's bank (sub_82176F68)",
    0xBE8: 'sets a scene\'s localized line ("MOVIE", "%s_%s"; sub_824DEC18)',
    0xBEC: 'sub_824F1DC8 on an object of the live tree list',
    0xBF4: "freezes an entity's animation: flag 0x04000000 of +632 set, its model's trees cleared",
    0xBF8: "unfreezes an entity's animation (flag 0x04000000 of +632 cleared)",
    0x10AC: 'sets field +0 of entry r3 in the table of 208-byte records at 0x82909990',
    0x10B0: 'sets fields +4 and +8 of an entry of the table at 0x82909990',
    0x10B4: 'sets one of three pairs (+48..+68) of an entry of the table at 0x82909990',
    0x10B8: 'sets one of three pairs (+16..+36) of an entry of the table at 0x82909990',
    0x10BC: 'sets one of four pairs (+80..) of an entry of the table at 0x82909990',
    0x10C0: 'sets a float (+112 block) of an entry of the table at 0x82909990',
    0x10C4: 'sets a float (+128 block) of an entry of the table at 0x82909990',
    0x10C8: 'sets a float (+144 block) of an entry of the table at 0x82909990',
    0x10CC: 'sets a float (+160 block) of an entry of the table at 0x82909990',
    0x10D0: 'sets a float (+176 block) of an entry of the table at 0x82909990',
    0x10D4: 'sets a float (+192 block) of an entry of the table at 0x82909990',
    0x10E0: 'ends the running thread and goes back to the scheduler (sub_824A2BD0)',
    0x10E4: 'sets the word at 0x82A90A64',
    0x28: "stricmp (the C runtime's)",
    0x2C: "strncmp (the C runtime's)",
    0x30: "strstr (the C runtime's)",
    0x74: "notify (entity, event name hash): a 20-byte event on the script VM's list (+40) for the waiting threads, and the entity's handlers (0x8C) run at once",
    0x78: 'notify with an integer argument (a 16-byte parameter object)',
    0x7C: 'notify with a string argument (a Broc::string parameter)',
    0x80: 'notify with a float and an integer argument',
    0x84: 'notify with a name hash argument ("end")',
    0xE0: 'sub_82488108 with an entity and a string (or ""), through the table at *(0x82A2A2FC)',
    0xE4: 'sub_82468218 on *(0x82A2AB38) with two arguments',
    0xE8: 'sub_82468180 on *(0x82A2AB38) with two arguments',
    0x16C: 'atan2 (f1, f2)',
    0x18C: 'length (of a vector)',
    0x1CC: 'objective_add without a string (index, state; config string 16 + index)',
    0x1D0: 'objective_add with the string given as a number (checked as a localized string reference)',
    0x1D4: 'objective_add (index, state, string; config string 16 + index with "state", "str", "order")',
    0x1D8: 'objective_add on an entity, the string given as a number',
    0x1DC: 'objective_add on an entity (index, state, string, entity, height: "ent", "height")',
    0x1E0: 'objective_add with a localized string by hash (sub_8252A638, "STRING MISSING")',
    0x1E4: 'objective_add on an entity with a localized string by hash',
    0x1E8: 'objective_delete (the config string emptied, its child objectives removed)',
    0x1EC: 'objective_state (index, state; "state")',
    0x1F0: 'objective_string (index, string; "str", "Objective strings is too long")',
    0x1F4: 'objective_string with a localized string by hash',
    0x1F8: 'objective_string without the on-screen message',
    0x1FC: 'objective_position (index, vector; "org", as "%i %i %i")',
    0x200: 'sets an objective\'s "wstate"',
    0x204: 'objective_current: the one objective in state 4, any other current one back to 1',
    0x208: 'objective_ring: turns an objective\'s "ring" over',
    0x20C: 'removes an objective\'s child objectives ("pobj")',
    0x210: 'adds a child objective (parent, display index) without a string',
    0x214: 'adds a child objective on an entity, the string given as a number',
    0x218: 'adds a child objective (parent, display index, state, string; config string 32 + n with "pobj", "order")',
    0x21C: 'adds a child objective with the string given as a number',
    0x220: 'adds a child objective on an entity',
    0x224: 'adds a child objective with a localized string by hash',
    0x228: 'adds a child objective on an entity with a localized string by hash',
    0x22C: 'deletes a child objective (parent, display index)',
    0x230: 'a child objective\'s state ("state")',
    0x234: "a child objective's string",
    0x238: "a child objective's string, a localized string by hash",
    0x23C: "a child objective's string without the on-screen message",
    0x240: 'a child objective\'s position ("org")',
    0x244: 'makes a child objective the current one of its parent (state 4)',
    0x248: 'turns a child objective\'s "ring" over',
    0x2E0: 'nothing (an empty function)',
    0x2E4: 'nothing (an empty function)',
    0x2E8: 'nothing (an empty function)',
    0x2EC: 'nothing (an empty function)',
    0x2F0: 'checkpoint: sub_82496AA8 on *(0x82A2A300) with a name ("Checkpoint <%s> already reached")',
    0x778: "sets or clears the actor's entity flag 0x08000000 (actor +0 -> +16)",
    0x77C: "sets or clears the actor's entity flag 0x10000000",
    0x780: 'setgoalpos with a float (the sentient at entity +536, sub_823D1998)',
    0x784: 'setgoalpos (a vector, checked finite; the sentient at entity +536, sub_823D1998)',
    0x788: "a path node's field from its 16-bit reference (the 132-byte nodes at *(*(0x82A2ADB0) + 4) + 20)",
    0x78C: 'setgoalnode (a path node reference; sub_823CF898)',
    0x790: 'setgoalentity (sub_823E0A98)',
    0x794: "a float from the sentient's path (sub_823C11F8, sub_823C91F8)",
    0x798: "a float from the sentient's path towards an entity (sub_823DAC18)",
    0x79C: "whether the actor's position (+336) is on its path (sub_823C11F8)",
    0x7A0: 'the animation call of sub_824B22C0 with both blend times 0',
    0x7A4: 'shoot with a float (the actor at entity +532, sub_823ED9B8)',
    0x7A8: 'shoot (sub_823ED9B8: "Attempt for same actor to shoot/melee more than once in a frame")',
    0x7AC: 'melee or shoot from the eye (sub_823ED8B0, the eye from BIP01 HEAD)',
    0x7B0: "runs the handler of the actor's current state (actor +36; 40-byte entries at 0x8206277C)",
    0x7B4: 'sets six words of the actor (+404 to +424)',
    0x7B8: "a float from the actor's eye (sub_823D5308) and a point",
    0x7BC: 'a time in seconds (x1000) to sub_82551C50 on the actor',
    0x7C0: 'a time in seconds (x1000) to sub_825399D0 on the actor',
    0x7C4: "sets the actor's +428 to +436 and the reciprocals of two angles (-45, 45) at +456 and +460",
    0x7C8: 'an actor animation (sub_82500A40; f1 to f3, "%.1f(%.2f, %.2f,%.2f)")',
    0x7CC: 'sub_823C1DD0 on the actor with one float twice',
    0x7D0: "sets the actor's +588 and +608 when positive (1000 or 0), clears +552",
    0x7D4: "resets the actor's vector at +576 to a constant",
    0x7D8: "sets the actor's vector at +576",
    0x7DC: "resets the actor's vector at +596 to a constant",
    0x7E0: "sets the actor's vector at +596",
    0x7E4: "fires the actor's weapon from tag_flash along a line (two vectors, a float: 12 or 8)",
    0x7E8: 'a sight test of the actor (sub_823D2868)',
    0x7EC: "a sight test from the actor's tag_flash to a point (sub_823D13B0, sub_823D2868)",
    0x7F0: '0x7EC with a zero offset',
    0x7F4: "a sight test between the actor's eye and an entity's (sub_823D5308, sub_823D2868)",
    0x7F8: 'a float for shooting at an entity (sub_823D9888)',
    0x7FC: "0x7F8 with the actor's +2232 as a time",
    0x800: "sub_823DAC18 on the actor's path with a time in seconds",
    0x804: "sub_823DAC18 on the actor's path with its +2232",
    0x808: 'sub_823E4C38 on the actor unless it is in state 1 (+8, +184)',
    0x80C: 'sub_8251EA58 on *(0x82A2A1A8) with an entity and an argument',
    0x810: "sub_8251DD00 on *(0x82A2A1A8) with the entity's origin raised by its height (+264)",
    0x814: 'sub_8251DD00 on *(0x82A2A1A8) with a point',
    0x818: 'a path node near the actor (sub_823D1C58), its 16-bit reference',
    0x81C: 'dropweapon (a weapon name, tag_weapon_left or tag_weapon_right; "unknown weapon \'%s\' in dropWeapon")',
    0x820: "a trace from the actor's origin to a point (sub_82516E28, sub_823D2E00)",
    0x824: 'teleport (point, angles; refused if the player can see the goal or the actor, unless forced)',
    0x828: 'enters the AI critical section ("enterCriticalSection: AI is already in the critical section")',
    0x82C: "leaves the AI critical section (state 14 off the actor's stack, sub_823C8D40)",
    0x830: 'whether the actor is in the critical section (state 14 on its stack at +184)',
    0x834: "a float from the actor's +1024 (sub_823C1760)",
    0x838: "whether the actor's first entry at +1980 has bit 0 (when +1920 is positive)",
    0x83C: 'allowedstances ("stand", "crouch", "prone"; up to three)',
    0x840: 'isstanceallowed ("invalid stance in isStanceAllowed()")',
    0x844: "whether any of the actor's four 24-byte entries at +2400 is active",
    0x848: "how many of the actor's four 24-byte entries at +2400 are active",
    0x84C: "sub_823DA5E8 with the actor's origin and a string, when it has grenades (+2576)",
    0x850: 'checkgrenadethrow ("min energy", "min time", "max time"; tag_weapon_right, "grenadethrow")',
    0x854: 'magicgrenade-like: a grenade of a weapon by name from an entity (sub_82556688, "grenadethrow")',
    0x858: "sub_823DB798 (tag_weapon_right) on the actor's +2504",
    0x85C: 'whether the actor and an entity pass sub_823DD068',
    0x860: "clears the actor's +2648 after sub_823DD0C0",
    0x864: 'whether the actor and an entity pass sub_8255DC08',
    0x868: 'traversemode ("gravity", "nogravity", "noclip"; actor +684)',
    0x86C: 'animmode ("nophysics", "gravity", "nogravity", "angle deltas"...; actor +708)',
    0x870: 'orientmode ("face angle", "face current", "face enemy"...; with an angle; actor +372)',
    0x874: "a yaw from the actor's +1948 (sub_82531C00)",
    0x878: "sets the actor's +2652",
    0x87C: "the entity number of the actor's +2648 (after sub_823C67D0)",
    0x880: 'sub_820CB568 (the C runtime, when the flag at 0x82A07401 is set)',
    0x884: 'sub_820CB568 (the C runtime, when the flag at 0x82A07401 is set)',
    0x888: "sets the actor entity's +352 vector and its packed form",
    0x88C: 'puts the actor in state 4 (+700) and runs sub_823E54C8, sub_823DF998',
    0x890: 'sub_823E5460 and sub_82561F10 on the actor, with a flag',
    0x894: "sub_824A0F90 on the actor's +988",
    0x898: "a yaw from the actor's +1000 and +1004, negated (sub_82531C00)",
    0x89C: "sub_824A0F90 on the actor's +752",
    0x8A0: "the actor's byte +1009",
    0x8A4: 'nothing (an empty function)',
    0x8A8: 'nothing (an empty function)',
    0x8AC: 'nothing (an empty function)',
    0x8B0: 'a path node near the actor (sub_823D1B48, then sub_823C35D8)',
    0x8B4: 'a path node near the actor (sub_823D1B48) kept at +2132, its 16-bit reference',
    0x8B8: "a path node's number from its 16-bit reference, -1 if none",
    0x8BC: "sets a path node's byte +39 from its 16-bit reference",
    0x8C0: "the actor's current path point (+1020 entries of 28 bytes) or its origin",
    0x8C4: 'a trace for a stance at a point ("prone"; sub_82487468)',
    0x8C8: "sets or clears the actor's flag 0x02000000 (+848)",
    0x8CC: "sub_82501F38 for an entity with a string, on the current local player's record",
    0x8D0: "sub_82501BA8 for an entity with a string, on the current local player's record",
    0x8D4: "sets flag 16 of the current local player's record (0x82A59DA0 table)",
    0x8D8: 'sets the words at 0x82A2A1DC to 0x82A2A1F0 (a "row" hash, floats 1.5 and infinity)',
    0x8DC: 'sub_823CCD48 on the path nodes (targetname, target, script_noteworthy...) with a flag',
    0x8E0: 'moveto (point, time, acceleration and deceleration times; script_brushmodel, script_model or script_origin only)',
    0x8E4: 'movex (distance, time, acceleration, deceleration; sub_824AA0E0 on axis 0)',
    0x8E8: 'movey (sub_824AA0E0 on axis 1)',
    0x8EC: 'movez (sub_824AA0E0 on axis 2)',
    0x8F0: 'rotatevelocity (a vector and a time; sub_824A6AA0)',
    0x8F4: 'rotateto (angles, time, acceleration, deceleration; sub_824A6A38)',
    0x8F8: 'rotatepitch (angle, time, acceleration, deceleration; sub_824A9FE8 on axis 0)',
    0x8FC: 'rotateyaw (sub_824A9FE8 on axis 1)',
    0x900: 'rotateroll (sub_824A9FE8 on axis 2)',
    0x904: 'movegravity (a velocity and times; sub_824A69D0)',
    0x908: 'solid (+324 set, flag 2 cleared, relinked; "cannot use the solid/notsolid commands on a script_origin entity")',
    0x90C: 'notsolid (+324 cleared, flag 2 set, relinked)',
    0x910: 'giveweapon (a weapon by name; sub_824A9AC0, sub_8253FC18)',
    0x914: 'gives a player a weapon by name if it has none of it (sub_82471188, sub_8253FC18)',
    0x918: 'takeweapon (a weapon by name; sub_82469C28)',
    0x91C: 'takeallweapons (the current weapon +132 cleared, each weapon taken)',
    0x920: 'getcurrentweapon (the name of the weapon at the player\'s +132, "none")',
    0x924: 'hasweapon (by name)',
    0x928: "whether a weapon by name is among the player's (+1460, +1468 bits)",
    0x92C: 'givemaxammo (by name; "gauge_fill" weapons, sub_824694B0)',
    0x930: '0 (a stub that tests the player and returns nothing useful)',
    0x934: 'gives a weapon by name (sub_8253FC18), a third variant',
    0x938: 'gives a weapon by name (sub_8253FC18), a fourth variant',
    0x93C: 'a float for a weapon by name of the player',
    0x940: 'a float for a weapon by name of the player, a second one',
    0x944: 'sub_8253D020 on the player',
    0x948: "sets the player state's vector at +16",
    0x94C: 'setplayerangles (sub_82540168)',
    0x950: "the player state's vector at +172 (the view angles)",
    0x954: "whether the player's +1468 has bits 0x60",
    0x958: "the player's +1468 bit 0",
    0x95C: "the player's +1468 bit 4",
    0x960: "the player's +44 bit 5",
    0x964: "whether the player's +100 is set",
    0x968: "a registry entry by name for the entity's zone (+496; sub_8242F4A8)",
    0x96C: 'sets or clears the entity flag 0x100000 (+632) of a player',
    0x970: "sets or clears the player's flag 0x100000 (+44)",
    0x974: "sets or clears the player's flag 0x200000 (+44)",
    0x978: "sets or clears the player's flag 0x400000 (+44)",
    0x97C: "sets or clears the player's flag 0x800000 (+44)",
    0x980: "sets or clears the player's flag 0x1000000 (+44)",
    0x984: "one of the player's +44 flags by number",
    0x988: '0 (a stub)',
    0x98C: '0 (a stub)',
    0x990: '0 (a stub)',
    0x994: 'closes the popup menu (the client command "popupclose")',
    0x998: 'nothing (an empty function)',
    0x99C: 'nothing (an empty function)',
    0x9A0: 'nothing (an empty function)',
    0x9A4: 'nothing (an empty function)',
    0x9A8: 'nothing (an empty function)',
    0x9AC: 'nothing (an empty function)',
    0x9B4: "sets the player's +1460 (a flag)",
    0x9B8: 'sets the byte at 0x829EB1B8',
    0x9BC: 'noclip for the local player (turned over; GAME_NOCLIPON, GAME_NOCLIPOFF)',
    0x9C0: 'reverb (the client command \'reverb "%s" %g %g\')',
    0x9C4: 'a test of a player against an entity',
    0x9C8: 'playlocalsound (the client command "ls %i"; "unknown sound alias \'%s\'")',
    0x9CC: "sets the player's +1624 to the opposite of a flag",
    0x9D0: "a weapon by name's string for the player (sub_824A8E00)",
    0x9D4: 'giveweapon into a slot ("Weapon %s goes in the %s weaponslot, not the %s weaponslot")',
    0x9D8: 'the ammo of a weapon by name (+1816, +1460, +1468)',
    0x9DC: 'sets the ammo of a weapon by name',
    0x9E0: 'the clip ammo of a weapon by name',
    0x9E4: 'sets the clip ammo of a weapon by name',
    0x9E8: "the player's +1476 for a weapon by name",
    0x9EC: 'attachpath (a vehicle path node; the vehicle at entity +540; "Vehicle is invalid on path after it\'s been used")',
    0x9F0: 'startpath ("Can\'t start path on a vehicle that hasn\'t been attached"; +619 = 1)',
    0x9F4: 'takes the vehicle off its path (+619 = 0; sub_82400AF8)',
    0x9F8: 'setswitchnode (two path nodes; sub_8253D618)',
    0x9FC: 'setwaitnode (a path node)',
    0xA00: "sets the vehicle's speed at once (mph, +380)",
    0xA04: 'setspeed (speed, acceleration, deceleration in mph; +824 = 1)',
    0xA08: 'resumespeed (acceleration; +824 = 2)',
    0xA0C: 'joltbody (a point and three floats; sub_82561208)',
    0xA10: 'turns the vehicle into a corpse (state 15, "script_vehicle_corpse")',
    0xA14: 'getwheelsurface ("Vehicle type [%s] has no wheels", "default", "none")',
    0xA18: 'getspeedmph (+716 scaled)',
    0xA1C: "the vehicle's entity at +432 (its owner or driver)",
    0xA20: "a vehicle test against the local player's vehicle (sub_82401FC0)",
    0xA24: "a vehicle setting with the local player's vehicle (sub_82401FC0), a flag",
    0xA28: "a vehicle setting with the local player's vehicle (sub_82401FC0)",
    0xA2C: "a vehicle setting with the local player's vehicle (sub_82401FC0), a second one",
    0xA30: 'a vehicle point with two floats (120, 0.3)',
    0xA34: "a vehicle aim from the local player's eye (sub_823D4778, sub_823C1DD0)",
    0xA38: "sets the vehicle's +928",
    0xA3C: "clears the vehicle's +928",
    0xA40: 'a vehicle type setting by number (the types at 0x829E1950)',
    0xA44: "sets the entity's +628 bit 0 and +324 flag 0x200000, relinked",
    0xA48: "clears the entity's +628 bit 0 and +324 flag 0x200000, relinked",
    0xA4C: 'ejects the vehicle\'s driver ("No driver to eject", "cl_stance")',
    0xA50: "sets the vehicle's +404 and the flag at +408",
    0xA54: "clears the vehicle's +404",
    0xA58: "sets the vehicle type's turretRotRate (+368: 18, 90, 5)",
    0xA5C: 'setturrettargetent ("Vehicle must have health to control the turret")',
    0xA60: 'setturrettargetvec ("Vehicle must have health to control the turret")',
    0xA64: 'clearturrettarget (with a flag)',
    0xA68: 'fireweapon ("No weapon specified for [%s]", "No tag_barrel for [%s]", "tag_gunner_flash")',
    0xA6C: 'whether a player\'s vehicle\'s +384 is not positive ("Must be called on a player controlled vehicle")',
    0xA70: "one of the vehicle's vectors (+1120 or +1104)",
    0xA74: "the vehicle's string at +940",
    0xA78: 'radiusdamage (point, radius, damages; sub_82410B20)',
    0xA7C: 'sub_82410A78 on an entity',
    0xA80: 'sets the byte at 0x825F3733',
    0xAE0: "nothing (an empty function; the engine block's +0)",
    0xB78: "clears the table of 70 name entries at 0x829EAF88 (the engine block's +152, copied into the level's table)",
    0xB7C: "adds a name (hashed) and a value to the 70-entry table at 0x829EAF88 (the engine block's +156)",
    0xBA8: 'nothing (an empty function)',
    0xBAC: 'nothing (an empty function)',
    0xBB0: 'nothing (an empty function)',
    0xBD8: 'formats two strings of up to 31 characters and passes them to sub_820B2A48',
    0xBF0: 'sub_820CE938 on the list at 0x825F51A0',
    0xBFC: "sets a HUD element's x (+4; the 124-byte entries at 0x829B01B8)",
    0xC00: "a HUD element's x (+4)",
    0xC04: "sets a HUD element's y (+8)",
    0xC08: "a HUD element's y (+8)",
    0xC0C: "sets a HUD element's alignx (+20)",
    0xC10: "a HUD element's alignx (+20)",
    0xC14: "sets a HUD element's aligny (+24)",
    0xC18: "a HUD element's aligny (+24)",
    0xC1C: "sets a HUD element's sort (+108, a float)",
    0xC20: "a HUD element's sort (+108)",
    0xC24: "sets a HUD element's fontscale (+12)",
    0xC28: "a HUD element's fontscale (+12)",
    0xC2C: "sets a HUD element's alpha (+31, 0 to 255)",
    0xC30: "a HUD element's alpha (+31)",
    0xC34: "sets a HUD element's red (+28)",
    0xC38: "a HUD element's red (+28)",
    0xC3C: "sets a HUD element's green (+29)",
    0xC40: "a HUD element's green (+29)",
    0xC44: "sets a HUD element's blue (+30)",
    0xC48: "a HUD element's blue (+30)",
    0xC4C: "a path node's targetname (+48; nodes by 16-bit reference, 132 bytes each)",
    0xC50: "sets a path node's targetname (+48)",
    0xC54: "a path node's on_goal (+60)",
    0xC58: "sets a path node's on_goal (+60)",
    0xC5C: "a path node's reservename (+64)",
    0xC60: "sets a path node's reservename (+64)",
    0xC64: "a path node's target (+56)",
    0xC68: "sets a path node's target (+56)",
    0xC6C: "a path node's animscript (+68)",
    0xC70: "sets a path node's animscript (+68)",
    0xC74: "a path node's script_noteworthy (+52)",
    0xC78: "sets a path node's script_noteworthy (+52)",
    0xC7C: "a path node's origin (+76)",
    0xC80: "sets a path node's origin (+76)",
    0xC84: "a path node's angles (+88)",
    0xC88: "sets a path node's angles (+88)",
    0xC8C: "a path node's radius (+92)",
    0xC90: "sets a path node's radius (+92)",
    0xC94: "a path node's spawnflags (+44)",
    0xC98: "sets a path node's spawnflags (+44)",
    0xC9C: "a path node's type (+40)",
    0xCA0: "sets a path node's type (+40)",
    0xCA4: "a vehicle node's targetname (+0; the table at 0x829B8648)",
    0xCA8: "sets a vehicle node's targetname (+0)",
    0xCBC: "a vehicle node's target (+4)",
    0xCC0: "sets a vehicle node's target (+4)",
    0xCC4: "a vehicle node's origin (+20)",
    0xCC8: "sets a vehicle node's origin (+20)",
    0xCCC: "a vehicle node's angles (+44)",
    0xCD0: "sets a vehicle node's angles (+44)",
    0xCD4: "a vehicle node's speed (+8)",
    0xCD8: "sets a vehicle node's speed (+8)",
    0xCDC: "a vehicle node's lookahead (+12)",
    0xCE0: "sets a vehicle node's lookahead (+12)",
    0xCE4: "a vehicle node's script_noteworthy (+16)",
    0xCE8: "sets a vehicle node's script_noteworthy (+16)",
    0xCEC: "the entity's classname (+564)",
    0xCF0: "sets the entity's classname (+564)",
    0xCF4: "the entity's origin (+336)",
    0xCF8: "sets the entity's origin (+336)",
    0xCFC: "the entity's model (+556)",
    0xD00: "sets the entity's model (+556)",
    0xD04: "the entity's modelscale (+560)",
    0xD08: "sets the entity's modelscale (+560)",
    0xD0C: "the entity's spawnflags (+628)",
    0xD10: "sets the entity's spawnflags (+628)",
    0xD14: "the entity's speed (+716)",
    0xD18: "sets the entity's speed (+716)",
    0xD1C: "the entity's closespeed (+720)",
    0xD20: "sets the entity's closespeed (+720)",
    0xD24: "the entity's target (+580)",
    0xD28: "sets the entity's target (+580)",
    0xD2C: "the entity's targetname (+572)",
    0xD30: "sets the entity's targetname (+572)",
    0xD4C: "the entity's teamname (+712)",
    0xD50: "sets the entity's teamname (+712)",
    0xD54: "the entity's wait (+828)",
    0xD58: "sets the entity's wait (+828)",
    0xD5C: "the entity's random (+832)",
    0xD60: "sets the entity's random (+832)",
    0xD64: "the entity's count (+808)",
    0xD68: "sets the entity's count (+808)",
    0xD6C: "the entity's health (+776)",
    0xD70: "sets the entity's health (+776)",
    0xD74: "the entity's dmg (+784)",
    0xD78: "sets the entity's dmg (+784)",
    0xD7C: "the entity's angles (+352)",
    0xD80: "sets the entity's angles (+352)",
    0xD8C: "the entity's rotate (+848)",
    0xD90: "sets the entity's rotate (+848)",
    0xD94: "the entity's degrees (+708)",
    0xD98: "sets the entity's degrees (+708)",
    0xD9C: "the entity's speed (+716)",
    0xDA0: "sets the entity's speed (+716)",
    0xDB4: "the entity's key (+884)",
    0xDB8: "sets the entity's key (+884)",
    0xDBC: "the entity's delay (+836)",
    0xDC0: "sets the entity's delay (+836)",
    0xDDC: "the entity's count (+808)",
    0xDE0: "sets the entity's count (+808)",
    0xDE4: "the entity's spawnitem (+888)",
    0xDE8: "sets the entity's spawnitem (+888)",
    0xDF4: "the entity's groupname (+588)",
    0xDF8: "sets the entity's groupname (+588)",
    0xE04: "the entity's script_noteworthy (+596)",
    0xE08: "sets the entity's script_noteworthy (+596)",
    0xE0C: "the entity's maxhealth (+780)",
    0xE10: "sets the entity's maxhealth (+780)",
    0xE1C: "the entity's animname (+604)",
    0xE20: "sets the entity's animname (+604)",
    0xE24: "the entity's persistent_index (+894)",
    0xE28: "sets the entity's persistent_index (+894)",
    0xE2C: "the entity's takedamage (+624)",
    0xE30: "sets the entity's takedamage (+624)",
    0xE34: "the actor's float at +236: probably accuracy",
    0xE38: "sets the actor's float at +236: probably accuracy",
    0xE3C: "the actor's float at +240",
    0xE40: "sets the actor's float at +240",
    0xE44: "the actor's float at +244",
    0xE48: "sets the actor's float at +244",
    0xE4C: "the actor's float at +248",
    0xE50: "sets the actor's float at +248",
    0xE54: "the actor's float at +252",
    0xE58: "sets the actor's float at +252",
    0xE5C: "the actor's word at +316",
    0xE60: "sets the actor's value at +316",
    0xE64: "the actor's word at +328",
    0xE68: "sets the actor's value at +328",
    0xE6C: "the actor's word at +340",
    0xE70: "sets the actor's value at +340",
    0xE74: "the actor's float at +2224",
    0xE78: "sets the actor's float at +2224",
    0xE7C: "the actor's float at +2228: maxsightdistsqrd",
    0xE80: "sets the actor's float at +2228: maxsightdistsqrd",
    0xE84: "the actor's float at +2220",
    0xE88: "sets the actor's float at +2220",
    0xE8C: "the actor's float at +2396",
    0xE90: "sets the actor's float at +2396",
    0xE94: "the actor's word at +2232",
    0xE98: "sets the actor's word at +2232",
    0xE9C: "the actor's word at +2084",
    0xEA0: "sets the actor's word at +2084",
    0xEA4: "the actor's word at +2088",
    0xEA8: "sets the actor's word at +2088",
    0xEAC: "the actor's half-word at +2128",
    0xEB0: "sets the actor's half-word at +2128",
    0xEB4: "the actor's float at +2092",
    0xEB8: "sets the actor's float at +2092",
    0xEBC: "the actor's float at +2096",
    0xEC0: "sets the actor's float at +2096",
    0xEC4: "the actor's word at +624",
    0xEC8: "sets the actor's word at +624",
    0xECC: "the actor's word at +628",
    0xED0: "sets the actor's word at +628",
    0xED4: "the actor's word at +636",
    0xED8: "sets the actor's value at +636",
    0xEDC: "the actor's word at +632",
    0xEE0: "sets the actor's word at +632",
    0xEE4: "the actor's word at +648",
    0xEE8: "sets the actor's word at +648",
    0xEEC: "the actor's word at +468",
    0xEF0: "sets the actor's word at +468",
    0xEF4: "the actor's float at +2072",
    0xEF8: "sets the actor's float at +2072",
    0xEFC: "the actor's float at +392",
    0xF00: "sets the actor's float at +392",
    0xF04: "the actor's float at +2148",
    0xF08: "sets the actor's float at +2148",
    0xF0C: "the actor's word at +2144",
    0xF10: "sets the actor's word at +2144",
    0xF14: "the actor's word at +2152",
    0xF18: "sets the actor's word at +2152",
    0xF1C: "the actor's word at +2156",
    0xF20: "sets the actor's word at +2156",
    0xF24: "the actor's word at +2496",
    0xF28: "sets the actor's word at +2496",
    0xF2C: "the actor's string at +284",
    0xF30: "sets the actor's string at +284",
    0xF34: 'the actor\'s word at +288: the weapon, a name hash ("shotgun", "bazooka")',
    0xF38: 'sets the actor\'s word at +288: the weapon, a name hash ("shotgun", "bazooka")',
    0xF3C: "the actor's word at +292",
    0xF40: "sets the actor's word at +292",
    0xF44: "the actor's string at +296",
    0xF48: "sets the actor's string at +296",
    0xF4C: "the actor's word at +2124",
    0xF50: "sets the actor's word at +2124",
    0xF54: "the actor's float at +2500: probably grenadeawareness",
    0xF58: "sets the actor's float at +2500: probably grenadeawareness",
    0xF5C: "the actor's word at +2504: the grenade weapon",
    0xF60: "sets the actor's word at +2504: the grenade weapon",
    0xF64: "the actor's string at +2564",
    0xF68: "sets the actor's string at +2564",
    0xF6C: "the actor's word at +2576: grenadeammo",
    0xF70: "sets the actor's word at +2576: grenadeammo",
    0xF74: "the actor's word at +2388",
    0xF78: "sets the actor's word at +2388",
    0xF7C: "the actor's word at +224",
    0xF80: "sets the actor's word at +224",
    0xF84: "the actor's byte at +2656",
    0xF88: "sets the actor's byte at +2656",
    0xF8C: "the actor's byte at +2657",
    0xF90: "sets the actor's byte at +2657",
    0xF94: "the actor's byte at +2130",
    0xF98: "sets the actor's byte at +2130",
    0xF9C: "the actor's word at +2660",
    0xFA0: "sets the actor's word at +2660",
    0xFA4: "the actor's word at +2664",
    0xFA8: "sets the actor's word at +2664",
    0xFAC: "the actor's word at +2792",
    0xFB0: "sets the actor's word at +2792",
    0xFB4: "the actor's string at +2672",
    0xFB8: "sets the actor's string at +2672",
    0xFBC: "the actor's string at +2676",
    0xFC0: "sets the actor's string at +2676",
    0xFC4: "the actor's string at +2680",
    0xFC8: "sets the actor's string at +2680",
    0xFCC: "the actor's string at +768",
    0xFD0: "sets the actor's string at +768",
    0xFD4: 'the actor\'s word at +448: the pose, a name hash ("stand", "crouch", "prone", "back")',
    0xFD8: 'sets the actor\'s word at +448: the pose, a name hash ("stand", "crouch", "prone", "back")',
    0xFDC: "the actor's word at +452",
    0xFE0: "sets the actor's word at +452",
    0xFE4: "the actor's word at +2516",
    0xFE8: "sets the actor's word at +2516",
    0xFEC: "the actor's word at +2520",
    0xFF0: "sets the actor's word at +2520",
    0xFF4: "the actor's word at +2524",
    0xFF8: "sets the actor's word at +2524",
    0xFFC: "the actor's word at +2528",
    0x1000: "sets the actor's word at +2528",
    0x1004: "the sentient's string at +4",
    0x1008: "sets the sentient's string at +4",
    0x100C: "the sentient's word at +48",
    0x1010: "sets the sentient's word at +48",
    0x1014: "the sentient's float at +52",
    0x1018: "sets the sentient's float at +52",
    0x101C: "the sentient's word at +116: probably the enemy",
    0x1020: "sets the sentient's word at +116: probably the enemy",
    0x1024: "the sentient's word at +132",
    0x1028: "sets the sentient's word at +132",
    0x102C: "the sentient's float at +24: goalradius",
    0x1030: "sets the sentient's float at +24: goalradius",
    0x1034: "the sentient's float at +36",
    0x1038: "sets the sentient's float at +36",
    0x103C: "the sentient's word at +104",
    0x1040: "sets the sentient's word at +104",
    0x1044: "the sentient's word at +56",
    0x1048: "sets the sentient's word at +56",
    0x104C: "the sentient's word at +64",
    0x1050: "sets the sentient's word at +64",
    0x1054: "the sentient's float at +76",
    0x1058: "sets the sentient's float at +76",
    0x105C: "the sentient's word at +80",
    0x1060: "sets the sentient's word at +80",
    0x1064: "the sentient's word at +84",
    0x1068: "sets the sentient's word at +84",
    0x107C: "the sentient's word at +140",
    0x1080: "sets the sentient's word at +140",
    0x1084: "the sentient's word at +144",
    0x1088: "sets the sentient's word at +144",
    0x108C: "the sentient's float at +148",
    0x1090: "sets the sentient's float at +148",
    0x1094: "the sentient's float at +88",
    0x1098: "sets the sentient's value at +88",
    0x109C: "the sentient's float at +100",
    0x10A0: "sets the sentient's float at +100",
    0x10A4: "the sentient's word at +68",
    0x10A8: "sets the sentient's word at +68",
    0x58: "endon (entity, event name hash): a 32-byte record of type 3 on the running thread's list (+24); the thread ends when the entity is notified with it",
    0x8C: "adds an event handler to an entity: (event name hash, level function hash) in its +516 list of 68-byte blocks, seven pairs each; notify (0x74) runs it through the level's slot 0xB60",
    0x90: "removes an event handler (event name hash, level function hash) from the entity's +516 list",
}


def pairs(start, count):
    return [(word(start + 8 * i), word(start + 8 * i + 4)) for i in range(count)]



# The generated code: each function's instructions (as the comments give them).
func_re = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
insn_re = re.compile(r'^\s*// (\S+)\s*(.*)$')
code = {}
for path in sorted(glob.glob(os.path.join(ROOT, 'CoD3RecompLib', 'ppc', 'ppc_recomp.*.cpp'))):
    current = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func_re.match(line)
        if m:
            current = int(m.group(1), 16)
            code[current] = []
            continue
        m = insn_re.match(line)
        if m and current is not None:
            code[current].append((m.group(1), m.group(2)))

# The table: the two lists, then the slots the fill functions store one at a
# time (a register loaded from 0x82A2A2A0, a function's address built with
# lis + addi and stored through it; the fill function is the "table" column),
# then the three the level copies from the engine block sub_824BB360 hands it
# (+0, +152, +156 of 0x82A909C0).
listed = {}
for name, start, count in (('A', 0x825F5638, 265), ('B', 0x825F5E80, 222)):
    for o, f in pairs(start, count):
        listed[o] = (name, f)


def direct_stores():
    found = {}
    for filler, body in code.items():
        values, table = {}, set()
        for op, operands in body:
            regs = operands.replace(' ', '').split(',')
            if op == 'lis':
                values[regs[0]] = (int(regs[1]) << 16) & 0xFFFFFFFF
                table.discard(regs[0])
            elif op == 'addi' and regs[1] in values:
                values[regs[0]] = (values[regs[1]] + int(regs[2])) & 0xFFFFFFFF
                table.discard(regs[0])
            elif op == 'lwz' and '(' in regs[-1]:
                offset, base = regs[-1].rstrip(')').split('(')
                if base in values and (values[base] + int(offset)) & 0xFFFFFFFF == 0x82A2A2A0:
                    table.add(regs[0])
                else:
                    table.discard(regs[0])
                values.pop(regs[0], None)
            elif op == 'stw' and '(' in regs[-1]:
                offset, base = regs[-1].rstrip(')').split('(')
                if base in table and 0x82000000 <= values.get(regs[0], 0) < 0x83000000:
                    found[int(offset)] = (filler, values[regs[0]])
            elif op == 'mr':
                if regs[1] in values:
                    values[regs[0]] = values[regs[1]]
                else:
                    values.pop(regs[0], None)
                if regs[1] in table:
                    table.add(regs[0])
                else:
                    table.discard(regs[0])
            elif not op.startswith(('st', 'cmp', 'fcmp', 'b', 'mt', 'tw', 'dcb')) and regs and regs[0]:
                values.pop(regs[0], None)
                table.discard(regs[0])
    return found


for o, (filler, f) in direct_stores().items():
    listed.setdefault(o, ('%08X' % filler, f))
for o, f in ((0xAE0, 0x82497BF8), (0xB78, 0x824978F8), (0xB7C, 0x82498DE0)):
    listed[o] = ('824BB360', f)
entries = [(t, o, f) for o, (t, f) in sorted(listed.items())]

STORES = re.compile(r'^(st[bhwd]|stf[sd]|stw|stmw|stv|stb|sth)')
ALL_SOURCES = re.compile(r'^(cmp|fcmp|mtctr|mtlr|mtcrf|tw|td|dcb)')
reg_re = re.compile(r'\b([rf])(\d+)\b')


def arguments(instructions):
    """The argument registers read before they are written (a linear pass)."""
    written, read = set(), []
    for op, operands in instructions:
        regs = reg_re.findall(operands)
        base = op.rstrip('.+-')
        if base in ('bl', 'blr', 'bctr', 'bctrl', 'b'):
            if base == 'bl':
                written.update({('r', str(i)) for i in range(3, 13)} | {('f', str(i)) for i in range(0, 14)})
            continue
        if STORES.match(base) or ALL_SOURCES.match(base):
            sources, dests = regs, []
        else:
            dests, sources = regs[:1], regs[1:]
        for kind, number in sources:
            n = int(number)
            key = (kind, number)
            if key not in written and ((kind == 'r' and 3 <= n <= 10) or (kind == 'f' and 1 <= n <= 8)) and key not in read:
                read.append(key)
        for key in dests:
            written.add(key)
    ints = sorted(int(n) for k, n in read if k == 'r')
    floats = sorted(int(n) for k, n in read if k == 'f')
    return ' '.join(['r%d' % n for n in ints] + ['f%d' % n for n in floats])


def calls(instructions):
    found = []
    for op, operands in instructions:
        if op == 'bl':
            target = int(operands.split()[0], 16)
            if target in KNOWN and KNOWN[target] not in found:
                found.append(KNOWN[target])
    return ', '.join(found)


def strings(instructions):
    high, found = {}, []
    for op, operands in instructions:
        m = re.match(r'r(\d+),(-?\d+)$', operands.replace(' ', ''))
        if op == 'lis' and m:
            high[int(m.group(1))] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = re.match(r'r(\d+),r(\d+),(-?\d+)$', operands.replace(' ', ''))
        if op == 'addi' and m and int(m.group(2)) in high:
            t = text((high[int(m.group(2))] + int(m.group(3))) & 0xFFFFFFFF)
            if t and t not in found:
                found.append(t)
    return found


# How often each level's code calls through each offset. A level keeps its
# copy of the table in its own data (lis + addi, 0x89005308 in Chambois) and
# holds that address in a register; a call is `lwz rX,offset(that register)`,
# then `mtctr rX` and `bctrl` a few instructions later. The register's value is
# followed through each function in one straight pass (lis, addi, mr; a call
# clears the volatile registers), and the table is the address most calls
# load through, so a virtual call (`lwz r11,8(r11)`) is not counted.
insn_line = re.compile(r'^\s*// (\S+)\s*(.*)$')
level_func = re.compile(r'^PPC_FUNC_IMPL\(')
level_tables = {}   # each level's table address


def indirect_calls(folder):
    found = []      # (base address, offset)
    for path in sorted(glob.glob(os.path.join(folder, 'ppc_recomp.*.cpp'))):
        values, loads, window = {}, {}, []
        for line in open(path, encoding='utf-8', errors='replace'):
            if level_func.match(line):
                values, loads, window = {}, {}, []
                continue
            m = insn_line.match(line)
            if not m:
                continue
            op, operands = m.group(1), m.group(2).replace(' ', '')
            regs = operands.split(',')
            if op == 'bctrl':
                for target in window[::-1]:
                    if target in loads:
                        found.append(loads[target])
                        break
                window = []
            elif op == 'mtctr':
                window.append(regs[0])
            if op in ('bl', 'bctrl'):
                for n in [0] + list(range(3, 13)):
                    values.pop('r%d' % n, None)
                continue
            if op == 'lis':
                values[regs[0]] = (int(regs[1]) << 16) & 0xFFFFFFFF
            elif op == 'addi' and regs[1] in values:
                values[regs[0]] = (values[regs[1]] + int(regs[2])) & 0xFFFFFFFF
            elif op == 'mr' and regs[1] in values:
                values[regs[0]] = values[regs[1]]
            elif op == 'lwz' and len(regs) == 2 and '(' in regs[1]:
                offset, base = regs[1].rstrip(')').split('(')
                if base in values:
                    loads[regs[0]] = (values[base], int(offset))
                else:
                    loads.pop(regs[0], None)
                values.pop(regs[0], None)
            elif op.startswith(('st', 'cmp', 'fcmp', 'b', 'mt', 'tw', 'dcb')):
                pass
            elif regs and regs[0] in values:
                values.pop(regs[0])
                loads.pop(regs[0], None)
            elif regs:
                loads.pop(regs[0], None)
    table = collections.Counter(base for base, offset in found).most_common(1)
    level_tables[os.path.basename(folder)] = table[0][0] if table else None
    return collections.Counter(offset for base, offset in found if table and base == table[0][0])


level_folders = sorted(f for f in glob.glob(os.path.join(ROOT, 'CoD3RecompLib', 'levels', '*')) if os.path.isdir(f))
calls_by_level = {os.path.basename(f): indirect_calls(f) for f in level_folders}
level_calls = calls_by_level.get(level, collections.Counter())
all_calls = sum(calls_by_level.values(), collections.Counter())

rows = []
for table, offset, function in entries:
    body = code.get(function, [])
    texts = strings(body)
    rows.append((offset, function, table, len(body), level_calls[offset], all_calls[offset], arguments(body), calls(body),
                 '; '.join(texts[:2])[:90], NAMES.get(offset, '')))
rows.sort(key=lambda r: r[0])

out = []
out.append('| offset | function | table | size | %s calls | all %d levels | reads | calls | text | name |' % (level, len(calls_by_level)))
out.append('|---|---|---|---|---|---|---|---|---|---|')
for r in rows:
    out.append('| 0x%X | sub_%08X | %s | %d | %d | %d | %s | %s | %s | %s |' % (
        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8].replace('|', '/'), r[9]))
result = '\n'.join(out)
if markdown:
    header = ('# The level scripts\' built-ins\n\n'
              'Generated by `scripts/builtins.py` from the executable and the %s level; what the '
              'table is and how it was read is in [scripts.md](scripts.md). "table" is the list '
              '(A, B) or the function that put the entry in the table. The two "calls" columns '
              'count the calls through each offset in that level\'s code and in all the levels\'; '
              '"reads" are the argument registers the function reads before writing them.\n\n' % level)
    open(markdown, 'w', encoding='utf-8', newline='\n').write(header + result + '\n')
    print('%d entries written to %s' % (len(rows), markdown))
else:
    print(result)
