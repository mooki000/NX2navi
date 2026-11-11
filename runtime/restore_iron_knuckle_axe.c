// Restoration: an Iron Knuckle's axe cuts past any shield. In the original a guarded axe swing still hurts; in this
// release Link blocks it with no loss of health. All four Iron Knuckles (Black, Silver, White, and Nabooru's) carry the
// same GameBalance/AttackParam/Enemy_IronKnuckle: one attack, a LargeWeapon of Strong intensity whose only damage
// attribute is GuardBreakStrong. The player's DamageParameters say his guard is OnlyAffectedByGuardBreakStrong: the axe
// knocks the shield aside and does no damage.
//
// The attributes are enum names hashed with murmur3 (GuardBreakStrong 2250389180). The one this edit gives the axe,
// 575954322, has no name in the game's files; it is taken for "cannot be guarded" from where it appears. It is on the
// attacks no shield stops (the cucco swarm, the grabs of Like Likes, ReDeads and Wallmasters, rolling boulders, walls
// of fire and spikes, Volvagia's breath) and on none of the swings a shield blocks (Stalfos, Lizalfos, Dinolfos,
// Wolfos, Gerudo thieves); on Ganon's sword swings it stands in place of GuardBreakStrong, never beside it, the swap
// this edit makes; and in the game's table of the attribute enum it sits between GuardBreak and NoKnockback. The axe
// keeps its damage (BaseAttackPower 12), its type and its intensity.
#include "restore.h"

#define MEMBER "GameBalance/AttackParam/Enemy_IronKnuckle.game__gamebalance__AttackParam.bgyml"
#define GUARD_BREAK_STRONG 2250389180.0   // murmur3("GuardBreakStrong")
#define UNGUARDABLE 575954322.0
#define AXE(pack) \
    RESTORE_SET_MEMBER(pack, MEMBER, "AttackParams/0/DamageAttributres/0", GUARD_BREAK_STRONG, UNGUARDABLE)

static const RestoreEdit edits[] = {
    AXE("Pack/Actor/Enemy_IronKnuckle_Black.pack.zs"),
    AXE("Pack/Actor/Enemy_IronKnuckle_Silver.pack.zs"),
    AXE("Pack/Actor/Enemy_IronKnuckle_White.pack.zs"),
    AXE("Pack/Actor/Enemy_MiddleBoss_IronKnuckle_Nabooru.pack.zs"),
};

RESTORATION(iron_knuckle_axe, "IRON_KNUCKLE_AXE", "Iron Knuckle's unblockable axe",
            "An Iron Knuckle's axe hurts Link through his shield, as it did in the original.", edits);
