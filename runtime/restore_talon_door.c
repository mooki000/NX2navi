// Restoration: the service door behind Hyrule Castle that Talon delivers milk to. In the original a plain wooden door
// in the castle wall ends the path where Talon sleeps beside his milk crates; this release keeps the crates (the two
// Obj_LonLonRanch_MovableBox_A_01 at x 31.62, z -145.13 and -146.63) and the path ends at a wall with a barred window
// and a lantern.
//
// The door is a copy of a town door that does not open, Obj_Door_A_05 "RestrictedDoor_Common_20" of the castle town
// (Market_Child_Dynamic at (-60.55, 100.99, 10.46)): a wooden leaf 1.45 m wide and 2.59 m tall, its hinge at the
// origin, the leaf along -x and 0.06 m either side of z = 0 (its collision shape, TwnObj_DoorWood_A_01__Collision). It
// stands in front of the window, which is 1.87 m wide (x 31.66 to 33.53: Land_HrlCastle_Tower_F_01, origin (30.11,
// 116.74, -151.75), puts its glass at local x 1.55 to 3.42), centred on it, on the path's ground (y 117.04), with its
// back 0.05 m before the wall's face: Link walking into the wall there stops with his centre at z -148.69 (the player's
// capsule), so the face is about -148.95. Seen in the game: a copy of the castle's own steel door at z -148.95 further
// left was hidden in the wall's rock; a copy of any placed object shows from either placement file.
#include "restore.h"
static const RestoreEdit edits[] = {
    RESTORE_ADD_FROM("Banc/HyruleCastle/HyruleCastle_Dynamic.bcett.byml.zs",
                     "Banc/Market/Market_Child_Dynamic.bcett.byml.zs",
                     "Obj_Door_A_05", 3646315241077774126ull, -60.549, 100.994, 10.464,
                     7960167791040293129ull,   // "nx2navi\x09": a Hash the game's files do not use
                     33.32f, 117.04f, -148.84f, 0.0f, 0.0f, 0.0f),
};
RESTORATION(talon_door, "TALON_DOOR", "Talon's milk door",
            "The service door behind Hyrule Castle is back where Talon leaves the milk.", edits);
