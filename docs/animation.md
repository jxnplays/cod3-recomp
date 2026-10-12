# Animation, asset banks and zones: what the title's code does

What was worked out of the title while finding the bind pose after a
restart (the memset, see [native_crt.cpp](../CoD3Host/native_crt.cpp)),
written down so the next fault in this part of the game starts from here.
Addresses are the executable's; each level DLL has its own copy of the C
runtime but not of these systems. `COD3_TRACEANIMHEAP=1` turns on the
traces in [anim_heap_trace.cpp](../CoD3Host/anim_heap_trace.cpp) that
look at everything below while the game runs.

## Asset banks

A level's assets arrive in place: each `.cod` archive holds a bank that is
used where it is loaded, not copied into other structures.

- **Bank header**: `"IIBS"` (0x49494253), a version (0x4000A3D7), the
  number of types (13), a pointer to the type records, a pointer to the
  bank's end. Each type record is 32 bytes: +0 type id, +4 the type's name
  (twelve characters), +16 a hash mask, +20 the hash table, +24 the number
  of entries, +28 the entries (8 bytes each; the resource pointer at +4).
  Names are looked up as hashes (sub_820CEBB8 hashes a string) in
  sub_820C0168(bank, type, 0, name).
- **The thirteen types**, the same in every bank:

  | id | type | id | type |
  |---|---|---|---|
  | 0 | TEXTURE | 7 | ANIMOFFSET |
  | 1 | FONT | 8 | SKELETON |
  | 2 | MESHFILE | 9 | EFFECT |
  | 3 | MESH | 10 | FX |
  | 4 | ANIMFILE | 11 | DISCTEX |
  | 5 | ANIM (a nalAnimClass) | 12 | DISCTEXSIZE |
  | 6 | SCNANIM | | |

- **The resource manager** at `*(0x82A2AB68)` keeps a bank per id at
  +(13 + id) * 4. sub_8245A0F8(manager, type, zone, name) and
  sub_8245A1D8 gather the banks to search (for ANIM and SKELETON every
  loaded bank, sub_82455248; otherwise one zone's, sub_82459E80, or every
  loaded one, sub_824598F0) and return the first match.
- **The bank set** at `*(0x82A2AB6C)` (an `InplaceAssetBankSet<ZoneBoundaryBank>`)
  keeps a zone object per id at +(11 + id) * 4 and three ids at +28, +32
  and +40: the anim bank, the global bank and the level's own bank.
  sub_82458668(set, id) finds a zone, with a cache at +(110 + id) * 4.
- **Zone objects** are 208 bytes and lie one after another: +0 / +4 next
  and previous, +12 the archive's path, +76 the zone's category, which its
  maker passes to the constructor (sub_8245B990): 0 for global.cod
  (sub_824631D0), 3 for the level's own archive (sub_8245CEE8), and for
  the rest what the level's zone descriptors say (sub_8245F710,
  sub_824636D0; Chambois: 2 anim, 4 the zn zones, 6 veh and defense2,
  7 char, 8 weapons, 9 fx), 10 marking one that cannot be used, +136 the
  zone's id, +140 its bank once resolved
  (sub_82456198), +200 its state (3 loaded; unloading goes 4, 5, 7 in
  sub_82462648 and ends with the script notify `"unloaded_<zone>"`).
- **Chambois' ids**: 0 global.cod, 1 anim.cod, 3 weapons, 4 char.cod,
  5 veh.cod, 6 fx.cod, 7 Chambois.cod, 8 zn01, 9 defense2, 10 zn02,
  11 zn03, 12 zn05. The zones are read once at the level's load; a restart
  after a death reads none of them again.
- **The stream zone manager** at 0x826E4C00 (its address in
  `*(0x82A2AB74)`; made once, sub_82457FE0 from sub_825372E8) handles only the zones streamed in during play: a
  slot per zone id from +4, two lists whose heads are at +436 and +444,
  linked through the zone's +68.

The animations of every level sit in anim.cod's bank; the loader for
loose animation files (sub_82173808, "Attempt to load already loaded
AnimFile %s") and its release (sub_82173AD8) never run in a level.

## Animation trees

The IW engine's XAnim, next to Treyarch's nal library.

- **Anim lists** (what a script's `#animtree` names): 12 bytes, name,
  count, entries. Chambois has 34, in a table at `*(0x82A2A204)`:
  generic_human (1216 animations), mg42 (13), opel_blitz (2), panzerIV (4),
  tankdeadguys (7), each weapon and its `_arms` (25 each), and
  drone_animtree (42).
- **An entry** is 28 bytes: +0 the name's hash, +4 the number of children
  (0 for an animation), +6 the parent's index, +8 the nal animation class
  once resolved, +12 the notetrack once built, +16 the generation of the
  last attempt to resolve it, +20 (a byte) the child a random node chose,
  +24 flags (0x20 a random node: setting it picks one child with rand()
  and plays that one; 0x4 a synced node, whose children restart
  together), +26 the first child's index.
- **A tree** (entity +552) is (count + 12) * 2 bytes: +0 / +4 next and
  previous in the list of live trees at 0x825F5190 (count, then the
  first), +8 its anim list, +12 the entity's +500 (the low half is the
  model), +16 the bank its entity's assets come from (entity +496, or the
  level's bank from the set's +40), +20 the number of info records it
  holds, then from +24 a 16-bit slot per animation: 0 when the animation
  is idle, otherwise the info record that plays it. A tree whose every
  slot is 0 leaves its model in the bind pose.
- **The info pool**: 4096 records of 52 bytes at 0x82A59DB0; record 0 is
  the sentinel, whose +12 heads the free list (+12 next, +10 previous).
  A record: +0 the leaf that carries the node (sub_824F0FC0's answer),
  +2 the notetrack cursor (-1 when none), +4 the notify to send, +8 a
  half-word cleared when the record is taken, +16 the time (0 to 1 over the
  animation), +20 the time at the last frame, +24 and +26 the note the
  cursor is at and was at, +28 how fast the weight moves, +32 the goal
  weight, +36 the weight, +40 the playback rate, +44 a nal instance
  released when the record goes. The update (sub_824F7D90) moves the
  weight toward the goal at its rate and gives a record back once weight
  and goal are both 0; the script's getanimtime reads +16. The pool is made once
  at start (sub_824F9498) and has no lock; one thread uses it.

The functions, by what they do:

| function | does |
|---|---|
| sub_824FD368 | makes a tree for an entity and list, puts it on the live list |
| sub_824F2238 | frees a tree (off the live list, back to its heap) |
| sub_824F3F50 | clears a tree, giving back every record |
| sub_824FCF68 | takes a record for a slot |
| sub_824F2348 | gives a record back (copies of it inline in sub_824F7D90 and sub_824F7F90) |
| sub_824FE120 | sets an animation's goal weight, the core of the rest |
| sub_82500A40 | "set anim": the weight, and every parent at 0 raised to 1 |
| sub_82500B80 | sets the weight and leaves the parents' weights alone |
| sub_824FE410 | "knob all": the weight, the siblings let go (sub_824F73D8), the parents up to a given one set to 1 |
| sub_824F0E38 | makes the missing parents of a node, at weight 0 |
| sub_82500C28 | "set anim knob": the siblings let go, then set anim |
| sub_824F73D8 | lets a node's siblings go (their goal weights to 0 over the blend time) |
| sub_824F7170 | restarts an animation (time and note cursor to 0) when asked or when it is not running; for a synced parent, its children with it |
| sub_824F1B60 | puts the notetrack cursor where the time is |
| sub_824F76A0 | clears an animation (the script's clearanim) |
| sub_824F70E8 | stops every child of a node at once |
| sub_824F0FC0 | the leaf that carries most weight under a node |
| sub_824F7D90 | the per-frame update's giving back of what has finished |
| sub_824FAF40 | resolves an entry's name to its animation class (type 5, all banks), once per generation at 0x825D19E8 |
| sub_824F9148 | builds an entry's notetrack from the class's `COD_Note` component on `fakeroot` or `tag_origin`: script string, hash, time as a fraction, then `end` |
| sub_824F7F90 | at a restart (`map_restart`, sub_824EB848; the death restart, sub_824EC048): every record every live tree holds given back, every entry unresolved |

The level scripts reach them through the built-in table described in
[scripts.md](scripts.md): offsets 0x674 to 0x6B0 are the animation calls
(setanim 0x68C, setanimknob 0x688, setanimknoball 0x694, their flagged
forms, clearanim 0x684, getanimtime 0x69C, getanimlength 0x67C,
animhasnotetrack 0x680, the frame count 0x678), and 0x6A0 / 0x6A4
useanimtree and clearanimtree ("cannot change the animtree of classname
'%s'").

Entity types seen with trees: 7 the drones (drone_animtree), 11 and 13
the actors and the player's (generic_human, weapons).

## The nal library's memory

- **AnimHeap**, four megabytes made once at start (sub_82400098,
  `*(0x82A2A144)`): allocate sub_82522CB8 refuses past four megabytes,
  free sub_82522CB0.
- Its one user is the **decoded animation cache** at 0x82914EE0 (the heap,
  then the least and the most recently used entry). An entry is the
  owner's slot that points back at it, next, previous, size, and its
  chunks. When the heap refuses, the cache drops its least recently used
  entry (sub_82174388) and tries again, so a full heap, which Chambois
  reaches in about a hundred seconds, is its normal state.
- The nal library's complaints go through sub_82144CA8, which the release
  build left empty; `COD3_TRACETITLE=1` prints them.

## The destructor that runs twice

Checked again with the host's memset: with its fix turned off
(`COD3_NODTORFIX=1`), a `spmap` from inside a level still destroys every
object of the teardown list twice (sub_823F0828 and the deleting
destructor sub_820C8478 both call sub_823F04B0), frees their blocks a
second time, and a malloc bin closes into a loop: the game stops. The fix
in title_fixes.cpp stays. A restart after a death does not tear the level
down this way at all.

The objects are `SpeedTreeWrapper`s, a level's trees. The destructor
releases their Direct3D resources (sub_822F1340, an atomic count down that
destroys at zero, so a second release only takes the count below zero,
unless the memory has gone to something else) and frees their arrays
through the title's free (sub_821276D0): 64-byte blocks of the fixed
small-block pool (sub_820CBF58 puts a block at the head of its size's list
with no check at all) and dlmalloc chunks (whose test takes a freed
chunk's header for one in use). Neither refuses a second free, so the
console frees the same blocks twice as well; that it carries on there is
down to what the heap holds at that moment, which cannot be looked at
without the console.

## Tools

- `COD3_MEMDUMP=hexaddress,hexsize,seconds,path[;...]` writes guest memory
  to files while the game runs (the physical heap is 0xA0000000, 0x20000000
  bytes; the running image 0x82000000, 0x1000000).
- `COD3_IMAGEDUMP=path` writes the image before the title starts.
- `scripts/xref.py`, `scripts/rtti.py`, `scripts/cut_tails.py`, and
  `scripts/builtins.py` for the level scripts' built-in table.
