// Restoration: the bomb flower that brings the Shadow Temple's tower down. In the original Link sets off a bomb flower
// by the bird statue with an arrow, and the statue falls across the chasm as a bridge; this release leaves only Link's
// own bomb to carry to the tower's foot, and neither ShadowTemple_Static nor ShadowTemple_Dynamic places a bomb flower.
// The tower (Obj_ShadowTemple_FallDownPillar_A_01, room 17) falls when its crack (Obj_ShadowTemple_CrackWall_A_02 at
// (-127.58, 41.95, -43.16)) breaks, which a Bomb attack does (Life/DamageParameters/CrackStoneObject); a bomb flower's
// fruit explodes as a Bomb attack within 4.3 m (BombFruit_ExplodeBlast), and an arrow sets it off (BombFlower_Fruit
// takes Arrow). This copies the plant the game places everywhere else, Obj_BombFlower_A_03, from FireTemple_Dynamic,
// 1.3 m in front of the crack on the ledge's floor (42.0, the height of the ledge's pots), and moves the copy into room
// 17. The tower stands on Link's own ledge in this release, so the shot is taken from that ledge rather than across the
// chasm. Seen in the game: the flower grows at the tower's foot; setting it off with an arrow and the tower's fall were
// not tried.
#include "restore.h"

#define FLOWER 7960167791040293156ull   // "nx2navi$": a Hash the game's files do not use

static const RestoreEdit edits[] = {
    RESTORE_ADD_FROM("Banc/ShadowTemple/ShadowTemple_Dynamic.bcett.byml.zs",
                     "Banc/FireTemple/FireTemple_Dynamic.bcett.byml.zs", "Obj_BombFlower_A_03",
                     1515362312184767526ull, -62.01, 148.0, -0.867, FLOWER, -127.58f, 42.0f, -44.46f, 0, 0, 0),
    RESTORE_SET("Banc/ShadowTemple/ShadowTemple_Dynamic.bcett.byml.zs", "Obj_BombFlower_A_03", FLOWER, -127.58,
                42.0, -44.46, "Dynamic/DefaultDungeonRoomIndex", 24, 17),
};

RESTORATION(bomb_flower, "SHADOW_BOMB_FLOWER", "Shadow Temple bomb flower",
            "A bomb flower at the foot of the Shadow Temple's tower: shoot it with an arrow to bring the tower down.",
            edits);
