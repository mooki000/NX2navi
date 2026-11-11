// Restoration: rupees at the top of the drawbridge chains in front of the castle town. In the original a child who
// climbs a drawbridge chain up to the castle wall finds rupees there; this release lets him climb the chains and has
// nothing at the top. The lowered bridge (Land_HrlField_DrawBridge_A_01, HyruleField_Child_Static at (0, 150.35,
// 34.02), shown while IsHyruleFieldBridgeClose is off) carries the chains in its model and in its collision, which
// reaches 11.58 above the deck and 2.41 behind its hinge: one chain at each side, 3.73 from the bridge's middle, from
// the deck's outer end up into the wall above the gate. No air wall is placed on their way (the gate's
// FldObj_AirWall_Player_GenerateNavMesh stops 156.2 high, where the chains are 3.7 m above it).
//
// The model's chain links put the upper chain on the line y = 150.35 + 8.50 + (34.13 - z) x 1.19: it meets the gate's
// face (z about 32.7) at y 160.5. One green rupee per chain, a copy of the field's own Obj_Rupee_Green, 1.8 m along the
// chain before the wall (of the chain's 17) and 0.5 above it, where a climbing child's body passes: z 33.87, y 159.66.
// Their number and colour are our choice.
#include "restore.h"

#define FIELD "Banc/HyruleField/HyruleField_Child_Static.bcett.byml.zs"
#define TOP_Y 159.66f
#define TOP_Z 33.87f
// The copied rupee: the green one HyruleField_Child_Static places in the field's south-west.
#define GREEN(new_hash, x)                                                                                            \
    RESTORE_ADD(FIELD, "Obj_Rupee_Green", 8976284932613239030ull, -147.51, 125.38, 456.08, new_hash, x, TOP_Y, TOP_Z, \
                0.0f, 0.0f, 0.0f)

static const RestoreEdit edits[] = {
    GREEN(7960167791040293130ull, -3.73f),   // "nx2navi\x0a": a Hash the game's files do not use
    GREEN(7960167791040293131ull, 3.73f),    // "nx2navi\x0b"
};

RESTORATION(drawbridge_rupees, "DRAWBRIDGE_RUPEES", "Drawbridge chain rupees",
            "A rupee waits at the top of each drawbridge chain for a child who climbs it.", edits);
