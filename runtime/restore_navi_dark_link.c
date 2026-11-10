// Restoration: Navi's line at the Dark Link fight. In the original, Navi's hint names the foe on one line and says
// "Conquer yourself!" on the next; this release's Navi says instead that it looks like Link, is full of evil power, and
// that monsters can mimic others. This release's lines are the three messages Enemy_MiddleBoss_DarkLink, _00 and _01 of
// EventFlowMsg/NaviHint_Enemy.msbt in each language's text archive (Mals/<language>.Product.110.sarc.zs); this gives
// all three the original's two lines. The original named the foe "Dark" and the player's name; this release has no
// player names (Link is Link), so the name line is "Dark Link". Languages: those of the original whose wording is
// known, English (USen, EUen) and Japanese (JPja; ダークリンク, 自分自身に打ち勝つのよ！！). The original's German and French lines are
// not, so EUde, EUfr and USfr keep the remake's. Each message keeps its leading tags (this release's Navi window and
// voice); the hashes name the messages as this release ships them (msbt_hash of their bytes).
#include "restore.h"

#define NAVI "EventFlowMsg/NaviHint_Enemy.msbt"
#define MALS(lang) "Mals/" lang ".Product.110.sarc.zs"
#define EN "Dark Link\nConquer yourself!"
#define JA "ダークリンク\n自分自身に打ち勝つのよ！！"

static const RestoreEdit edits[] = {
    RESTORE_SET_MESSAGE(MALS("USen"), NAVI, "Enemy_MiddleBoss_DarkLink", 0xf420144cf33d7c35ull, EN),
    RESTORE_SET_MESSAGE(MALS("USen"), NAVI, "Enemy_MiddleBoss_DarkLink_00", 0x73b9dbf519753b8full, EN),
    RESTORE_SET_MESSAGE(MALS("USen"), NAVI, "Enemy_MiddleBoss_DarkLink_01", 0x077386941e4ee902ull, EN),
    RESTORE_SET_MESSAGE(MALS("EUen"), NAVI, "Enemy_MiddleBoss_DarkLink", 0xf420144cf33d7c35ull, EN),
    RESTORE_SET_MESSAGE(MALS("EUen"), NAVI, "Enemy_MiddleBoss_DarkLink_00", 0x73b9dbf519753b8full, EN),
    RESTORE_SET_MESSAGE(MALS("EUen"), NAVI, "Enemy_MiddleBoss_DarkLink_01", 0x077386941e4ee902ull, EN),
    RESTORE_SET_MESSAGE(MALS("JPja"), NAVI, "Enemy_MiddleBoss_DarkLink", 0xa408ce0546a54a95ull, JA),
    RESTORE_SET_MESSAGE(MALS("JPja"), NAVI, "Enemy_MiddleBoss_DarkLink_00", 0xdd986cba80695590ull, JA),
    RESTORE_SET_MESSAGE(MALS("JPja"), NAVI, "Enemy_MiddleBoss_DarkLink_01", 0xb07fd910257a4a5bull, JA),
};

RESTORATION(navi_dark_link, "NAVI_DARK_LINK", "Navi's Dark Link line",
            "Navi's hint at Dark Link is the original's: \"Conquer yourself!\" (English and Japanese).", edits);
