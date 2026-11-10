# Restorations

A restoration brings back something the original game did that this release changed. Each one has its own switch,
`restore_<key> = 1` in `nx2navi.ini` `[restorations]` (`NX2NAVI_RESTORE_<KEY>`) or its row on the *Restorations* tab of
the menu (F1). All are off by default, and a switch takes effect at the next start. No game file ships with the port:
a restoration is a list of edits, and the game applies them once at start to the player's own files.

| Key | What it restores |
|---|---|
| `WOLFOS_AMBUSH` | The two Wolfos in the Forest Temple's first room appear when Link is about 4 m away, not 12 m. |
| `FIRE_TRIAL_SINKING` | The long sinking platform of the Fire Trial in Ganon's Castle sinks at the original's pace, about 1.6 times as fast. |
| `TALON_DOOR` | The service door behind Hyrule Castle where Talon delivers milk: a town door that does not open, in front of the window where the path ends. |
| `DRAWBRIDGE_RUPEES` | A green rupee at the top of each drawbridge chain in front of the castle town, for a child who climbs it. |
| `SHADOW_BOMB_FLOWER` | A bomb flower at the foot of the Shadow Temple's tower, to shoot with an arrow and bring the tower down. Not yet seen in the game: the flower was, its shot and the tower's fall were not. |
| `IRON_KNUCKLE_AXE` | An Iron Knuckle's axe hurts Link through his shield, as it did in the original. Not yet seen in the game. |
| `LOST_WOODS_BY_EAR` | The Lost Woods without the floating notes and the trails of mist: Saria's song alone shows the way. Not yet seen in the game. |
| `NAVI_DARK_LINK` | Navi's hint at Dark Link in the Water Temple: "Dark Link" and "Conquer yourself!" (English; in Japanese the original's ダークリンク, 自分自身に打ち勝つのよ！！). The German and French texts keep this release's lines: the original's wording in those languages is not known to us. Not yet seen in the game. |

The tab's own texts are in the menu's four languages; a restoration's name and description are English.

## How it works

`runtime/romfs.c` `overlay_init` calls `restore_apply` (`runtime/restore.c`) once, at start, after the mods' files are
chosen. For each file the switched-on restorations edit, the engine (`runtime/restore_engine.c`) takes the file as the
game would read it (a mod's version, else the original), unpacks it (zstd, with the dictionary of its id from
`Pack/ZsDic.pack.zs`), adds the pack members it gains (`runtime/sarc.c`), parses the BYML (`runtime/byml.c`), or each
BYML member of a pack the edits name, applies the edits, writes the BYML back, reads it back to check it holds what was
meant, and packs it as the original was; a message file (MSBT, `runtime/msbt.c`) member of a text archive gets its
messages replaced instead, and a logic graph (AINB, `runtime/ainb.c`; a loose `Logic/` graph or an `AI/` member of a
pack) its structure changed. A copy from another file reads that file the same way. `romfs.c` serves the
result from `<profile>/restore/<folder>/` and raises the file's entry in the resource size table when it grew, gives an
added member an entry of its own when the table has none for its path, and raises the entry of a pack member the edits
made larger: the game sizes the member's buffer from it (without, a Heart Piece whose ActorParam grew by one text
crashed the game as the area loaded). Several restorations on one file all apply to it.

What a placement edit does in the game (seen behind Hyrule Castle): the area's `_Static` and `_Dynamic` files are both
read at its load, edited or not, and an object removed, moved or copied in either one is gone, moved or there; a copy
shows whatever it is (a crate, a door, a rupee taken from another area's file) and needs nothing but a Hash that
neither of the area's two files uses (the engine refuses one the other file has, and two declared copies with one Hash
stop the start). What a placement edit cannot change is the area's static compound collision
(`Phive/StaticCompoundBody/<folder>/<area>_X*_Z*.bphsc`): an object it holds keeps its collision where it was, so a
removed crate vanished but Link still stopped at its face. The engine refuses to remove an object the area's static
compound holds.

An edit applies only when what it names is there as declared: an object by its `Hash`, `Gyaml` and `Translate` (within
1 cm, in the file that places it), a value or text at what the game ships, a key it adds missing from a dict that is
there, a member it adds missing from the pack, a message whose bytes hash to the declared value. Anything else
(another game version, a mod that changed the file) skips
that edit, and the log says which and why (`restore: <KEY>: <file>: ...`); the other edits still apply. Each document,
the file or one pack member, is checked on its own: one that does not read back as edited, or a member that cannot be
put back into its pack, loses its own edits, and the file is served with the other members' edits. When the file
itself cannot be read, unpacked, packed again or served, none of its edits apply and the game reads it as it is. The
menu row says how many edits the start applied. With every switch off nothing is read or written, and the log has no
`restore:` line.

The BYML reader takes the little-endian files this release has, and refuses a dict whose keys are out of order or
repeated (the game looks them up sorted); a key an edit adds goes where that order puts it. It allocates at most 8 Mi
nodes (about 512 MiB) for one file, then gives up and the file is served as it is.

`nx2navi.exe --probe-restore` (`./ocarina --probe-restore` on Linux) is the dry run: it applies every declared
restoration, switched on or not, to the files as the game would read them, serves nothing, prints a line per edit
(applied, or skipped and why) and exits 1 when one was skipped. `python tests/restore_check.py` tests the engine, the
BYML and SARC readers and writers and `restore.c`'s switches on files of its own (no game files), and runs the readers
on every cut length of its files and on thousands of copies with bits flipped.

## Adding one

One file, `runtime/restore_<name>.c`; the build picks it up and the registry (a linker section, like `HOST_OVERRIDE`)
lists it. Nothing else changes but the docs: a line in `nx2navi.ini.example` `[restorations]` and in the table above.

```c
// Restoration: what the original did, what the remake does, and how the files show it (object names, hashes, why
// these values).
#include "restore.h"

static const RestoreEdit edits[] = {
    // set a number below a placed object: file, Gyaml, Hash, Translate, then a path of keys and array indices, the
    // value shipped, ours
    RESTORE_SET("Banc/X/X_Static.bcett.byml.zs", "Area", 3333ull, 0.0, 101.5, 61.5, "Scale/2", 7.06, 3.0),
    // set a number in a pack member
    RESTORE_SET_MEMBER("Pack/Actor/Obj_W.pack.zs", "GameParameter/WParam/Obj_W.game__object__WParam.bgyml",
                       "MoveSpeed", 0.15, 0.24),
    // remove a placed object: file, Gyaml, Hash, Translate
    RESTORE_REMOVE("Banc/X/X_Dynamic.bcett.byml.zs", "Obj_Y", 1234567890123456789ull, 1.0, 100.0, -2.5),
    // add a copy of a placed object: the original's Gyaml, Hash and Translate, then the copy's Hash, Translate, Rotate
    RESTORE_ADD("Banc/X/X_Static.bcett.byml.zs", "Obj_Z", 1111ull, 0.0, 100.0, 0.0,
                2222ull, 3.0, 100.0, -4.0, 0.0, 1.5708, 0.0),
    // the same with the object placed in another file
    RESTORE_ADD_FROM("Banc/X/X_Dynamic.bcett.byml.zs", "Banc/V/V_Dynamic.bcett.byml.zs", "Obj_Z", 4444ull,
                     1.0, 50.0, 2.0, 5555ull, 3.0, 100.0, -4.0, 0.0, 0.0, 0.0),
    // set a text (RESTORE_SET_TEXT below a placed object); NULL for the game's text adds the key
    RESTORE_SET_MEMBER_TEXT("Pack/Actor/Obj_W.pack.zs", "Actor/Obj_W.engine__actor__ActorParam.bgyml",
                            "Components/LiftableRef", "", "?Component/LiftableParam/Obj_W.game__component__LiftableParam.bgyml"),
    // add a key the file lacks, a number or bool of the type given (RESTORE_SET_MEMBER_NEW in a pack member)
    RESTORE_SET_NEW("Banc/X/X_Static.bcett.byml.zs", "Area", 3333ull, 0.0, 101.5, 61.5, "Dynamic/Extra", BYML_FLOAT, 2.5),
    // add a pack member: a copy of another pack's (NULL: this pack's), or bytes declared in an array
    RESTORE_COPY_MEMBER("Pack/Actor/Obj_W.pack.zs", "Component/LiftableParam/Obj_V.game__component__LiftableParam.bgyml",
                        "Pack/Actor/Obj_V.pack.zs", "Component/LiftableParam/Obj_V.game__component__LiftableParam.bgyml"),
    // replace a message of a text archive's MSBT: its label, the msbt_hash of the game's message, ours
    RESTORE_SET_MESSAGE("Mals/USen.Product.110.sarc.zs", "EventFlowMsg/NaviHint_Enemy.msbt", "Enemy_X",
                        0x0123456789abcdefull, "Name\nHint"),
    // a logic graph: name the node of a type whose values match, add a copy of a node, set a value, connect
    RESTORE_GRAPH_FIND("Pack/Actor/Obj_W.pack.zs", "AI/Obj_W.module.ainb", "sel", "BoolSelector", "Input<-#13.Bool"),
    RESTORE_GRAPH_ADD("Pack/Actor/Obj_W.pack.zs", "AI/Obj_W.module.ainb", "less", "QueryMathLessOrEqualF32", "#131",
                      NULL, NULL),
    RESTORE_GRAPH_SET("Pack/Actor/Obj_W.pack.zs", "AI/Obj_W.module.ainb", "less", "F32Base", "0", "0.3"),
    RESTORE_GRAPH_LINK("Pack/Actor/Obj_W.pack.zs", "AI/Obj_W.module.ainb", "less", "Bool", "sel", "Input", "#13.Bool"),
};

RESTORATION(name, "KEY", "Menu label", "One-line description for the menu.", edits);
```

- The key is upper case, digits and `_`; the setting becomes `restore_<key>`.
- Hashes and positions come from the player's files of the game version the port builds (`Banc/<Area>/<Area>_*.bcett.byml.zs`,
  `Pack/Actor/<Name>.pack.zs`); the edit list holds facts about the game, never its files.
- A copy (`RESTORE_ADD`, `RESTORE_ADD_FROM`) gets the new Hash in `Phive/Placement/ID` too and leaves out `Links` and
  `SRTHash`; pick a Hash the area's files do not use. Edits on one file apply in their order, so a `RESTORE_SET` after
  the copy (by its new Hash and Translate) changes the copy, and a `RESTORE_SET_MEMBER` can change a member just added.
- A new key goes into a dict that is already there (`Dynamic/Extra` needs `Dynamic`); the engine adds no dicts or
  array items and changes no value's type. The BYML writer rebuilds the key and string tables, so new keys and texts
  need nothing else.
- An added member's bytes come from the player's files (a copy) or from the restoration's declaration; declared bytes
  must be our own, never data taken from the game.
- A message (`RESTORE_SET_MESSAGE`) is named by its label and the 64-bit FNV-1a hash of its bytes as the game ships
  them (`msbt_hash`, `runtime/msbt.h`): the restoration holds no game text. The message keeps its leading control tags
  (the window, Navi's voice) and the rest becomes the declared text, UTF-8 with a control tag written
  `{group.type:hex parameters}` and a closing one `{/group.type}`; the file's encoding (UTF-8 or UTF-16) is kept. The
  probe prints the hash it found when it skips an edit for another message.
- A logic graph (`RESTORE_GRAPH_*`, `runtime/ainb.h`) is changed through names its edits give nodes: `FIND` names the
  one node of a type whose values match (`"InstanceName=Enemy_X;Input<-#13.Bool;#8"`: a value it holds, an input
  connected to an output, its index; no node or two is a skip), `ADD` names a new node, a copy of one of this graph or
  of another (its values; its inputs keep their values and lose their connections; a node with children, a module
  call, an expression or attachments is not copied), `SET` changes a property or an input that holds a value of its
  own from the game's value to ours, `LINK` connects an output of one named node to an input of another and names
  what the input held (`"#13.Bool"`, `"@name.Out"`, `"bb:Name"`, `"=value"`; `"+#13.Bool"` adds the output to those a
  multi-input such as `OperateAND`'s `BoolMulti` reads). The graph's edits apply all or none:
  half a change to a behaviour is worse than none. New nodes, entries and strings go at the ends of their arrays; the
  rest of the file keeps its bytes. `python tests/restore_check.py --game <game copy>` reads every graph of the
  player's files and writes it back (unchanged, and with a node added) to check the reader and writer.
- Run `--probe-restore` on a game build: every edit of the new restoration should say what it did.

Not yet possible: removing a key or a pack member, adding dicts or array items, removing graph nodes or changing a
node's children, adding blackboard entries, editing binary files (models, textures, collision).
