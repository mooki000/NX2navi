// Restoration: a pair of Stalfos in the Forest Temple must fall together. In the original, a Stalfos that falls while
// its partner still stands lies there as bones and gets up again unless the partner falls in time. In this release a
// fallen Stalfos stays down. The pairs are placed in ForestTemple_Static: Stalfos_A and B
// (room 4, Enemy_Stalfos_Weak 1639162365107625161 and 10526761513561859643) and Stalfos_D and E (room 17, above it);
// each room's logic graph holds one ActorLogicSimpleEnemy per Stalfos (InstanceName Enemy_Stalfos_Weak_<id>), whose
// output Dead the room reads (both Dead open the room's reward) and whose input RespawnTrigger (Logic/NodeDefinition)
// is false. This adds to each graph, for each Stalfos of a pair:
//   Bool of an OperateInvertBool fed the partner's Dead  (the partner stands)
//   True of an OperateAND fed this one's Dead and that   (this one is down alone)
//   Bool of a HoldGeneralDelayBool, Frame = RETURN_FRAMES (down alone that long)  -> this one's RespawnTrigger
// The new nodes are copies of nodes of other logic graphs of the game (named below by graph and index), unconnected
// before the edits connect them. The Stalfos comes back as this release brings any Stalfos in, dropping down; the
// original's pile of bones has no model or animation in this release. RETURN_FRAMES: 10 s of the logic's frames.
#include "restore.h"

#define RETURN_FRAMES "300"
#define ROOM4 "Logic/ForestTemple_Level_Room_06_Prefab_61dc.logic.module.ainb"
#define ROOM17 "Logic/ForestTemple_Level_Room_06_Prefab_6b8c.logic.module.ainb"
#define INVERT_FROM "Logic/EnemyPoeBigSet_15d3.logic.module.ainb"   // node 5, an OperateInvertBool
#define AND_FROM "Logic/InvBool.module.ainb"                        // node 0, an OperateAND
#define DELAY_FROM "Logic/MorphaStage_0120.logic.root.ainb"         // node 10, a HoldGeneralDelayBool

// Stalfos `me` (named by an edit before) gets up when it has been down for RETURN_FRAMES while `other` stands.
#define RETURN(graph, me, other, up, alone, wait)                                                                \
    RESTORE_GRAPH_ADD(graph, NULL, up, "OperateInvertBool", "#5", INVERT_FROM, NULL),                             \
    RESTORE_GRAPH_ADD(graph, NULL, alone, "OperateAND", "#0", AND_FROM, NULL),                                    \
    RESTORE_GRAPH_ADD(graph, NULL, wait, "HoldGeneralDelayBool", "#10", DELAY_FROM, NULL),                        \
    RESTORE_GRAPH_SET(graph, NULL, wait, "Frame", "1", RETURN_FRAMES),                                            \
    RESTORE_GRAPH_LINK(graph, NULL, other, "Dead", up, "Bool", "=false"),                                         \
    RESTORE_GRAPH_LINK(graph, NULL, me, "Dead", alone, "BoolMulti", "=false"),                                    \
    RESTORE_GRAPH_LINK(graph, NULL, up, "Bool", alone, "BoolMulti", "+@" me ".Dead"),                             \
    RESTORE_GRAPH_LINK(graph, NULL, alone, "True", wait, "Bool", "=false"),                                       \
    RESTORE_GRAPH_LINK(graph, NULL, wait, "Bool", me, "RespawnTrigger", "=false")

static const RestoreEdit edits[] = {
    RESTORE_GRAPH_FIND(ROOM4, NULL, "a", "ActorLogicSimpleEnemy", "InstanceName=Enemy_Stalfos_Weak_58a6"),
    RESTORE_GRAPH_FIND(ROOM4, NULL, "b", "ActorLogicSimpleEnemy", "InstanceName=Enemy_Stalfos_Weak_dcb8"),
    RETURN(ROOM4, "a", "b", "b_stands", "a_alone", "a_waits"),
    RETURN(ROOM4, "b", "a", "a_stands", "b_alone", "b_waits"),
    RESTORE_GRAPH_FIND(ROOM17, NULL, "d", "ActorLogicSimpleEnemy", "InstanceName=Enemy_Stalfos_Weak_c051"),
    RESTORE_GRAPH_FIND(ROOM17, NULL, "e", "ActorLogicSimpleEnemy", "InstanceName=Enemy_Stalfos_Weak_50d1"),
    RETURN(ROOM17, "d", "e", "e_stands", "d_alone", "d_waits"),
    RETURN(ROOM17, "e", "d", "d_stands", "e_alone", "e_waits"),
};

RESTORATION(stalfos_pair, "STALFOS_PAIR", "Stalfos pairs fall together",
            "In the Forest Temple, a Stalfos that falls gets up again 10 s later unless its partner falls too.", edits);
