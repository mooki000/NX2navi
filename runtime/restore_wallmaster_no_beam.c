// Restoration: a Wallmaster about to drop is not announced by a column of purple light (the original telegraphed it
// with the hand's shadow alone). This release counts down BeforeFallFrame (WallmasterParam:
// 180) in Wallmaster.action.fall.module (node 1, ExecuteGenericCounter) and an ExecuteBlackboardSetF32 (node 8,
// ExecuteOnce false) writes the count's PercentComplete into Wallmaster_EmitAppearSignEffectScale every frame: the
// scale of the sign effect (Enm_Wallmaster_AppearLight_00), a column of light that grows and turns red until the hand
// drops. With ExecuteOnce true the scale is written once, at the count's start, and the column does not show (seen in
// the game). Not changed, because the files offer no way: the coloured ring the same effect draws on the floor (its
// emitters are in ELink2/elink2.Product.110.belnk, not in a graph), and the hand's shadow, which this release does not
// draw while the hand waits out of sight (its model is faded out until it drops: OneShotWallmasterModelAlphaFade).
#include "restore.h"

static const RestoreEdit edits[] = {
    RESTORE_GRAPH_FIND("Pack/Actor/Enemy_Wallmaster.pack.zs", "AI/Wallmaster.action.fall.module.ainb", "sign",
                       "ExecuteBlackboardSetF32",
                       "BlackboardKey=Wallmaster_EmitAppearSignEffectScale;F32<-#1.PercentComplete"),
    RESTORE_GRAPH_SET("Pack/Actor/Enemy_Wallmaster.pack.zs", "AI/Wallmaster.action.fall.module.ainb", "sign",
                      "ExecuteOnce", "false", "true"),
};

RESTORATION(wallmaster_no_beam, "WALLMASTER_NO_BEAM", "Wallmaster without light beam",
            "A Wallmaster's coming drop is not lit by a growing column of purple light; the ring on the floor stays.",
            edits);
