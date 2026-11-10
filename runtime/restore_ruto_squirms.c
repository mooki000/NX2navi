// Restoration: Princess Ruto, carried by Link in Jabu-Jabu's belly, gets impatient when he stands still for a few
// seconds. This release has the impatient reaction (SameRoomPlayer in her
// GameParameter/NpcZoraRuto: animation Act_LiftRest_LookAround_B and a "getting bored" line) but plays it only after
// she has been carried in one room for SameRoomPlayerWaitTime = 18000 frames: her graph
// AI/NpcZoraRutoChild.utility.ControlActTogetherReactionCondition.module (both of her packs) counts with node 14
// (SelectorGenericFrameCounter, CountFrames bound to that wait) while node 8 (BoolSelector) reads node 13, a
// comparison of her room and Link's; nothing looks at whether Link moves. This makes node 8 read whether Link's speed
// is at most 0.3 m/s instead (the threshold the game's own graphs use with a linear velocity for "still"):
//   F32 of a QueryMathGetVectorLength fed Velocity of node 5 (NpcCommon.QueryGetPerceptionPlayerInfo, in m/s: the
//   game compares its length with GameParameter thresholds named ...Mps)
//   Bool of a QueryMathLessOrEqualF32 of that and 0.3   -> Input of node 8
// and shortens the wait to IDLE_FRAMES, so the counter runs while Link stands and the reaction comes after a few
// seconds; when he moves, node 8 leaves the counter, which starts again at his next stop. The new nodes are copies of
// nodes of other graphs of the game (named below by graph and index), unconnected before the edits connect them.
// IDLE_FRAMES: 4 s of the AI's frames.
#include "restore.h"

#define IDLE_FRAMES 120.0
#define COND "AI/NpcZoraRutoChild.utility.ControlActTogetherReactionCondition.module.ainb"
#define PARAM "GameParameter/NpcZoraRuto/Npc_Zora_Ruto_Child_001.game__npc__NpcZoraRuto.bgyml"

#define IDLE(pack)                                                                                                   \
    RESTORE_GRAPH_FIND(pack, COND, "player", "NpcCommon.QueryGetPerceptionPlayerInfo.module", "#5"),                \
    RESTORE_GRAPH_FIND(pack, COND, "stays", "BoolSelector", "Input<-#13.Bool;#8"),                                  \
    RESTORE_GRAPH_ADD(pack, COND, "speed", "QueryMathGetVectorLength", "#134", "Pack/Actor/Npc_Navi.pack.zs",      \
                      "AI/Navi.action.Wait.module.ainb"),                                                           \
    RESTORE_GRAPH_ADD(pack, COND, "still", "QueryMathLessOrEqualF32", "#1",                                         \
                      "AI/DungeonBossVolvagia.action.flymove.module.ainb", NULL),                                   \
    RESTORE_GRAPH_SET(pack, COND, "still", "F32Base", "0", "0.3"),                                                  \
    RESTORE_GRAPH_LINK(pack, COND, "player", "Velocity", "speed", "Vector", "=0,0,0"),                             \
    RESTORE_GRAPH_LINK(pack, COND, "speed", "F32", "still", "F32", "=0"),                                          \
    RESTORE_GRAPH_LINK(pack, COND, "still", "Bool", "stays", "Input", "#13.Bool"),                                 \
    RESTORE_SET_MEMBER(pack, PARAM, "SameRoomPlayerWaitTime", 18000.0, IDLE_FRAMES)

static const RestoreEdit edits[] = {
    IDLE("Pack/Actor/Npc_Zora_Ruto_Child_001.pack.zs"),
    IDLE("Pack/Actor/Npc_Zora_Ruto_Child_002.pack.zs"),
};

RESTORATION(ruto_squirms, "RUTO_SQUIRMS", "Ruto squirms when idle",
            "Carried in Jabu-Jabu's belly, Ruto gets impatient when Link stands still for a few seconds.", edits);
