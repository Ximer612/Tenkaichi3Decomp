#include "common.h"
#include "menu/menu_e.h"
#include "sys/pad.h"
#include "sys/save.h"

#ifdef PORT
/*
 * PC only: the Mod Manager's Stages and Songs tabs, the disc image picker of its Importer tab (the tab itself is the file
 * browser, src/menu/port_modmgr2.c), and the pages of a character's menu: its moves' names, the numbers of its tables,
 * another character's moveset. What they ask for is answered by port/src/gs/modtools.c.
 */
#include "menu/port_modui.h"
#include "menu/tbo_art.h"

extern void Heap_Free(void *ptr);
extern void Res_RelocateOffsets(void *out, void *base, void *hdr);

/* ---- what the host answers (port/src/gs/modtools.c) ---- */
typedef struct PortExInfo {
    char file[128];
    char name[64];
    s32 tenthMb;
    s32 on;
} PortExInfo;
typedef struct PortDiscInfo {
    s32 kind, index, changed, formOf, fusion, hasForms, chipLen;
    char name[96], form[96];
} PortDiscInfo;
extern s32 Port_ExList(s32 which);
extern void Port_ExGet(s32 which, s32 i, PortExInfo *out);
extern s32 Port_ExLimit(s32 which);
extern s32 Port_ExToggle(s32 which, s32 i);
extern s32 Port_ExRename(s32 which, s32 i, char *name);
extern s32 Port_ExOrder(s32 which, s32 i, s32 by);
extern s32 Port_ExDrop(s32 which, s32 i);
extern s32 Port_ExIcon(s32 i, void *out, s32 max);
extern void Port_ExApply(void);
extern void Port_DiscClose(void);
extern s32 Port_DiscOpen(char *path);
extern s32 Port_DiscPoll(s32 *progress, char *why);
extern s32 Port_DiscMade(void);
extern s32 Port_DiscCount(void);
extern void Port_DiscGet(s32 i, PortDiscInfo *out);
extern s32 Port_DiscChip(s32 i, void *out, s32 max);
extern s32 Port_DiscNameChip(s32 i, s32 tile, void *out, s32 max);
extern s32 Port_DiscTaken(s32 i);
extern void Port_DiscTake(s32 i, s32 on);
extern void Port_DiscSetName(s32 i, char *name);
extern s32 Port_DiscReplaces(s32 slot);
extern void Port_DiscSetReplaces(s32 slot, s32 id);
extern void Port_DiscOptions(s32 links, s32 cost);
extern void Port_DiscImport(void);
extern s32 Port_CeOpen(s32 mod, s32 chara, char *why);
extern void Port_CeClose(void);
extern s32 Port_CeTables(void);
extern void Port_CeTable(s32 t, char *name, s32 *rows, s32 *cols, char *note);
extern void Port_CeRow(s32 t, s32 row, char *out);
extern void Port_CeCol(s32 t, s32 c, char *name, char *hint);
extern s32 Port_CeValue(s32 t, s32 row, s32 c, char *out);
extern s32 Port_CeStep(s32 t, s32 row, s32 c, s32 dir, s32 big);
extern s32 Port_CeSetText(s32 t, s32 row, s32 c, char *text);
extern s32 Port_CeMoves(void);
extern s32 Port_CeMove(s32 i, char *cost, char *name);
extern s32 Port_CeMoveSet(s32 i, char *cost, char *name);
extern s32 Port_CeDirty(void);
extern s32 Port_CeSave(char *why);
extern s32 Port_CeTake(s32 mod, s32 chara, s32 from, char *why);
extern s32 Port_CeTakeMove(s32 mod, s32 chara, s32 from, s32 move, s32 srcMove, char *why);
extern s32 Port_CeSlotNames(s32 mod, s32 chara, char *out);
extern s32 Port_CeHasBackup(s32 mod);
extern s32 Port_CeRestore(s32 mod);
extern void Port_ModRescan(void);

#define RES_SIZE 0x1600
#define KB_EX 5   /* what the keyboard is typing (sMm->keyWhich): a stage's or song's name, */
#define KB_DISC 6 /* a name on the disc picker, */
#define KB_MOVE 7 /* a move's name, */
#define KB_NUM 8  /* a number */

static char sWhy[200];
static char sLine[96];

static void Tabs_Move(void); /* L1 / R1 between the manager's four tabs */

/* A chip-like file (64 x 64, the roster's kind of picture) in a block of its own; NULL if it was none. */
static MmRes *Res_Read(s32 got, MmRes *res) {
    if (got <= 0) {
        Heap_Free(res);
        return NULL;
    }
    Res_RelocateOffsets((void *)&res, (u8 *)res, (void *)res);
    return res;
}

/* ================================================================================================= stages and songs */
#define EX_ROWS 7
static s32 sExCount[2] = {-1, -1};
static s32 sExCur[2];
static s32 sExTop[2];
static PortExInfo sExShown;
static MmRes *sExIcon[256];
static u8 sExIconTried[256];

static void Icons_Free(void) {
    s32 i;

    for (i = 0; i < 256; i++) {
        if (sExIcon[i] != NULL) {
            Heap_Free(sExIcon[i]);
            sExIcon[i] = NULL;
        }
        sExIconTried[i] = 0;
    }
}

static void Ex_Read(s32 which) {
    sExCount[which] = Port_ExList(which);
    if (sExCur[which] >= sExCount[which]) {
        sExCur[which] = sExCount[which] > 0 ? sExCount[which] - 1 : 0;
    }
    if (which == 0) {
        Icons_Free();
    }
}

static MmTex *Ex_IconOf(s32 i) {
    if (i < 0 || i >= 256) {
        return NULL;
    }
    if (!sExIconTried[i]) {
        MmRes *res = Heap_Alloc(RES_SIZE, 0x40, 0, 2);

        sExIconTried[i] = 1;
        if (res != NULL) {
            sExIcon[i] = Res_Read(Port_ExIcon(i, res, RES_SIZE), res);
        }
    }
    return sExIcon[i] != NULL ? sExIcon[i]->tex : NULL;
}

void Mm3_OpenExtras(s32 which) {
    Ex_Read(which);
    Mm_SetMode(which ? MM_SONGS : MM_STAGES);
}

static void Ex_Update(s32 live, s32 which) {
    Pad *pad = &gPad[0];
    s32 n = sExCount[which];
    s32 *cur = &sExCur[which];
    s32 *top = &sExTop[which];
    s32 was;

    sMm->wantKey = -1;
    if (n > 0) {
        Mm_Prompt(0, "ON / OFF");
        Mm_Prompt(1, "RENAME");
        Mm_Prompt(10, "ORDER");
        Mm_Prompt(4, "REMOVE");
    }
    Mm_Prompt(14, "TABS");
    Mm_Prompt(2, "BACK");
    if (!live) {
        return;
    }
    if (pad->gamePressed & PADG_TRIANGLE) {
        Snd_PlaySe(1, 2);
        Mm_SetMode(MM_LIBRARY);
        return;
    }
    Tabs_Move();
    if (sMm->mode != (which ? MM_SONGS : MM_STAGES) || n <= 0) {
        return;
    }
    was = *cur;
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        *cur = (*cur + 1) % n;
    } else if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        *cur = (*cur + n - 1) % n;
    } else if (pad->gameRepeat & (PADG_RIGHT | PADG_RIGHT2)) {
        *cur = *cur + EX_ROWS < n ? *cur + EX_ROWS : n - 1;
    } else if (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2)) {
        *cur = *cur >= EX_ROWS ? *cur - EX_ROWS : 0;
    }
    if (*cur != was) {
        Snd_PlaySe(1, 0);
    }
    if (*cur < *top) {
        *top = *cur;
    }
    if (*cur >= *top + EX_ROWS) {
        *top = *cur - EX_ROWS + 1;
    }
    Port_ExGet(which, *cur, &sExShown);
    if (pad->gamePressed & PADG_CROSS) {
        if (Port_ExToggle(which, *cur)) {
            Snd_PlaySe(1, 1);
            Ex_Read(which);
        } else {
            Mm_Note(sExShown.on ? "COULD NOT SWITCH IT OFF" : "THE MOST THE GAME TAKES ARE ON: SWITCH ONE OFF");
            Snd_PlaySe(1, 2);
        }
    } else if (pad->gamePressed & PADG_SQUARE) {
        Snd_PlaySe(1, 1);
        Mm2_TextEntry(KB_EX, which ? MM_SONGS : MM_STAGES, sExShown.name, 40);
    } else if (pad->gamePressed & PADG_CIRCLE) {
        if (Port_ExDrop(which, *cur)) {
            Mm_Note("MOVED TO THE FOLDER'S .REMOVED FOLDER");
            Snd_PlaySe(1, 1);
            Ex_Read(which);
        } else {
            Snd_PlaySe(1, 2);
        }
    } else if (pad->gamePressed & (PADG_L2 | PADG_R2)) {
        s32 by = (pad->gamePressed & PADG_L2) ? -1 : 1;

        if (sExShown.on && Port_ExOrder(which, *cur, by)) {
            Snd_PlaySe(1, 0);
            *cur += by;
            Ex_Read(which);
        } else {
            Snd_PlaySe(1, 2);
        }
    }
}

static void OnOff(s32 on, f32 cx, f32 cy) {
    Mx_Box(on ? MM_TAB_SEL : MM_BAR_GREY, cx - 40.0f, cy - 18.0f, cx + 40.0f, cy + 18.0f, 14.0f, 0x80);
    Mx_Text(cx, cy, on ? "ON" : "OFF", 22.0f, 70.0f, on ? MX_WHITE : MX_DIM, 1);
}

static void Ex_Draw(s32 which) {
    static PortExInfo e;
    s32 n = sExCount[which];
    s32 row;
    s32 on = 0;
    s32 i;

    for (i = 0; i < n; i++) {
        Port_ExGet(which, i, &e);
        on += e.on;
    }
    Mx_Box(MM_FIELD, 60.0f, 132.0f, 780.0f, 178.0f, 23.0f, 0x80);
    Mx_Text(85.0f, 155.0f, which ? "ADDED SONGS" : "ADDED STAGES", 28.0f, 0.0f, MX_GOLD, 0);
    sprintf(sLine, "%d ON, AT MOST %d", (int)on, (int)Port_ExLimit(which));
    Mx_Text(755.0f, 155.0f, sLine, 24.0f, 0.0f, MX_GOLD, 2);
    for (row = 0; row < EX_ROWS && sExTop[which] + row < n; row++) {
        f32 y0 = 192.0f + 70.0f * (f32)row;
        s32 at = sExTop[which] + row;
        s32 sel = at == sExCur[which];

        Port_ExGet(which, at, &e);
        Mx_Row(60.0f, y0, 780.0f, y0 + 60.0f, sel, 0x80);
        if (which == 0) {
            MmTex *t = Ex_IconOf(at);

            if (t != NULL) {
                Mx_Tex(t, 84.0f, y0 + 6.0f, 148.0f, y0 + 54.0f, 0, 0, 64, 64, 0x80, 0x80);
            }
        }
        Mx_Text(which == 0 ? 164.0f : 92.0f, y0 + 30.0f, e.name, 28.0f, which == 0 ? 500.0f : 580.0f, e.on ? MX_LAV : MX_DIM, 0);
        OnOff(e.on, 725.0f, y0 + 30.0f);
    }
    if (sExTop[which] > 0) {
        Mx_Fit(MM_UP, 420.0f, 184.0f, 20.0f, 0x80);
    }
    if (sExTop[which] + EX_ROWS < n) {
        Mx_Fit(MM_DOWN, 420.0f, 682.0f, 20.0f, 0x80);
    }
    Mx_Window(805.0f, 130.0f, 1340.0f, 686.0f);
    if (n == 0) {
        Mx_Text(1072.0f, 300.0f, which ? "NO ADDED SONGS" : "NO ADDED STAGES", 32.0f, 0.0f, MX_GOLD, 1);
        Mx_Say(1072.0f, 350.0f, "The Importer tab brings them", 24.0f, 480.0f, MX_WHITE, 1);
        Mx_Say(1072.0f, 382.0f, "from a disc image or an archive.", 24.0f, 480.0f, MX_WHITE, 1);
        return;
    }
    Port_ExGet(which, sExCur[which], &e);
    if (which == 0) {
        MmTex *t = Ex_IconOf(sExCur[which]);

        if (t != NULL) {
            Mx_Tex(t, 952.0f, 160.0f, 1192.0f, 340.0f, 0, 0, 64, 64, 0x80, 0x80);
        }
    }
    Mx_Text(1072.0f, 380.0f, e.name, 34.0f, 490.0f, MX_WHITE, 1);
    Mx_Say(1072.0f, 424.0f, e.file, 22.0f, 490.0f, MX_CYAN, 1);
    sprintf(sLine, "%d.%d MB", (int)(e.tenthMb / 10), (int)(e.tenthMb % 10));
    Mx_Text(1072.0f, 462.0f, sLine, 24.0f, 0.0f, MX_LAV, 1);
    Mx_Say(1072.0f, 520.0f, e.on ? "It is in the game's menus." : "Switched off: kept in the folder's off folder.", 24.0f, 490.0f, MX_WHITE, 1);
    Mx_Say(1072.0f, 556.0f, "The order is the menus' order.", 24.0f, 490.0f, MX_WHITE, 1);
    Mx_Rect(813.0f, 604.0f, 1332.0f, 678.0f, Mx_Rgba(0x06, 0x1A, 0x24, 0x70), Mx_Rgba(0x06, 0x14, 0x1C, 0x78));
    Mx_Say(1072.0f, 641.0f, "Used as soon as you leave the Mod Manager.", 23.0f, 490.0f, MX_WHITE, 1);
}

/* ==================================================================================================== a disc image */
#define DI_ROWS 7
#define DI_MAX 220
static s32 sDiPhase;  /* 0 closed, 1 reading, 2 the list, 3 writing */
static s32 sDiProg;
static s32 sDiCur;
static s32 sDiTop;
static s32 sDiFilter; /* 0 all, 1 characters, 2 stages, 3 songs */
static s32 sDiOnlyNew = 1;
static s32 sDiCost = 1; /* what transformations will cost; -1 the disc's own */
static s32 sDiLinks = 1;
static s32 sDiCount;
static s32 sDiView[DI_MAX];
static s32 sDiViewCount;
static PortDiscInfo sDiItems[DI_MAX];
static MmRes *sDiChip[DI_MAX];
static u8 sDiChipTried[DI_MAX];
static MmRes *sDiName[8];
static s32 sDiNameOf = -1;
static s32 sDiPicking; /* the item a picker is choosing for */
static char *const sDiFilterName[4] = {"EVERYTHING", "CHARACTERS", "STAGES", "SONGS"};

static void Di_FreePics(void) {
    s32 i;

    for (i = 0; i < DI_MAX; i++) {
        if (sDiChip[i] != NULL) {
            Heap_Free(sDiChip[i]);
            sDiChip[i] = NULL;
        }
        sDiChipTried[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        if (sDiName[i] != NULL) {
            Heap_Free(sDiName[i]);
            sDiName[i] = NULL;
        }
    }
    sDiNameOf = -1;
}

static void Di_BuildView(void) {
    s32 i;

    sDiViewCount = 0;
    for (i = 0; i < sDiCount; i++) {
        PortDiscInfo *it = &sDiItems[i];

        if ((sDiFilter == 0 || it->kind == sDiFilter - 1) && (!sDiOnlyNew || it->changed != 0)) {
            sDiView[sDiViewCount++] = i;
        }
    }
    if (sDiCur >= sDiViewCount) {
        sDiCur = sDiViewCount > 0 ? sDiViewCount - 1 : 0;
    }
    if (sDiTop > sDiCur) {
        sDiTop = sDiCur;
    }
}

/* a file of the Importer that is a disc image */
s32 Mm3_IsDisc(char *path) {
    s32 n = 0;

    while (path[n] != 0) {
        n++;
    }
    return n > 4 && path[n - 4] == '.' && (path[n - 3] | 32) == 'i' && (path[n - 2] | 32) == 's' && (path[n - 1] | 32) == 'o';
}

void Mm3_OpenDisc(char *path) {
    Di_FreePics();
    sDiPhase = 1;
    sDiProg = 0;
    sDiCur = 0;
    sDiTop = 0;
    Port_DiscOpen(path);
    Mm_SetMode(MM_DISC);
}

static void Di_Leave(void) {
    Di_FreePics();
    Port_DiscClose();
    sDiPhase = 0;
    Mm2_OpenBrowser();
}

static MmTex *Di_ChipOf(s32 i) {
    if (!sDiChipTried[i]) {
        MmRes *res = sDiItems[i].chipLen > 0 ? Heap_Alloc(RES_SIZE, 0x40, 0, 2) : NULL;

        sDiChipTried[i] = 1;
        if (res != NULL) {
            sDiChip[i] = Res_Read(Port_DiscChip(i, res, RES_SIZE), res);
        }
    }
    return sDiChip[i] != NULL ? sDiChip[i]->tex : NULL;
}

/* the disc's picture of the name under the cursor, as eight tiles */
static void Di_NameRead(s32 i) {
    s32 t;

    if (sDiNameOf == i) {
        return;
    }
    sDiNameOf = i;
    for (t = 0; t < 8; t++) {
        if (sDiName[t] != NULL) {
            Heap_Free(sDiName[t]);
            sDiName[t] = NULL;
        }
        if (sDiItems[i].kind != 0) {
            MmRes *res = Heap_Alloc(RES_SIZE, 0x40, 0, 2);

            if (res != NULL) {
                sDiName[t] = Res_Read(Port_DiscNameChip(i, t, res, RES_SIZE), res);
            }
        }
    }
}

static void Di_Update(s32 live) {
    Pad *pad = &gPad[0];
    s32 poll;
    s32 was;
    s32 picked = 0;
    s32 i;

    sMm->wantKey = -1;
    poll = Port_DiscPoll(&sDiProg, sWhy);
    if (sDiPhase == 1 || sDiPhase == 3) {
        if (poll == 3) { /* it could not be read */
            static char note[64];

            sprintf(note, "%.60s", sWhy);
            Mm_Note(note);
            Snd_PlaySe(1, 2);
            sDiPhase = 0;
            Mm2_OpenBrowser();
        } else if (poll == 2 && sDiPhase == 1) {
            sDiCount = Port_DiscCount();
            sDiCount = sDiCount > DI_MAX ? DI_MAX : sDiCount;
            for (i = 0; i < sDiCount; i++) {
                Port_DiscGet(i, &sDiItems[i]);
            }
            Di_BuildView();
            sDiPhase = 2;
            Snd_PlaySe(1, 1);
        } else if (poll == 5) {
            static char note[64];

            sprintf(note, "ADDED %d. THEY ARE IN THEIR TABS NOW", (int)Port_DiscMade());
            Mm_Note(note);
            Snd_PlaySe(1, 1);
            sDiPhase = 2;
            Port_ModRescan();
            Mm_FreeFaces();
            Mm_Refresh();
            for (i = 0; i < sDiCount; i++) {
                Port_DiscTake(i, 0);
            }
        }
        return;
    }
    if (sDiViewCount > 0) {
        s32 kind = sDiItems[sDiView[sDiCur]].kind;

        Mm_Prompt(0, "PICK");
        Mm_Prompt(1, kind == 0 ? "ADD / REPLACE" : "NAME IT");
    }
    Mm_Prompt(4, "ALL SHOWN");
    Mm_Prompt(10, "SHOW");
    Mm_Prompt(13, "NEW ONLY");
    Mm_Prompt(14, "OPTIONS");
    Mm_Prompt(7, "ADD PICKED");
    Mm_Prompt(2, "CLOSE");
    if (!live) {
        return;
    }
    if (pad->gamePressed & PADG_TRIANGLE) {
        Snd_PlaySe(1, 2);
        Di_Leave();
        return;
    }
    if (pad->gamePressed & (PADG_L2 | PADG_R2)) {
        sDiFilter = (sDiFilter + ((pad->gamePressed & PADG_R2) ? 1 : 3)) % 4;
        sDiCur = 0;
        sDiTop = 0;
        Di_BuildView();
        Snd_PlaySe(1, 0);
        return;
    }
    if (pad->gamePressed & PADG_SELECT) {
        sDiOnlyNew ^= 1;
        sDiCur = 0;
        sDiTop = 0;
        Di_BuildView();
        Snd_PlaySe(1, 0);
        return;
    }
    if (pad->gamePressed & PADG_L1) { /* what transformations will cost: 0..9, the disc's own */
        sDiCost = sDiCost >= 9 ? -1 : sDiCost + 1;
        Port_DiscOptions(sDiLinks, sDiCost);
        Snd_PlaySe(1, 0);
    }
    if (pad->gamePressed & PADG_R1) { /* their forms and fusions come along */
        sDiLinks ^= 1;
        Port_DiscOptions(sDiLinks, sDiCost);
        Snd_PlaySe(1, 0);
    }
    if (pad->gamePressed & PADG_CIRCLE) {
        for (i = 0; i < sDiViewCount; i++) {
            Port_DiscTake(sDiView[i], 1);
        }
        Snd_PlaySe(1, 1);
        return;
    }
    if (pad->gamePressed & PADG_START) {
        for (i = 0; i < sDiCount; i++) {
            picked += Port_DiscTaken(i);
        }
        if (picked == 0) {
            Mm_Note("PICK SOMETHING FIRST");
            Snd_PlaySe(1, 2);
        } else {
            Snd_PlaySe(1, 1);
            sDiPhase = 3;
            sDiProg = 0;
            Port_DiscImport();
        }
        return;
    }
    if (sDiViewCount == 0) {
        return;
    }
    was = sDiCur;
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        sDiCur = (sDiCur + 1) % sDiViewCount;
    } else if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        sDiCur = (sDiCur + sDiViewCount - 1) % sDiViewCount;
    } else if (pad->gameRepeat & (PADG_RIGHT | PADG_RIGHT2)) {
        sDiCur = sDiCur + DI_ROWS < sDiViewCount ? sDiCur + DI_ROWS : sDiViewCount - 1;
    } else if (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2)) {
        sDiCur = sDiCur >= DI_ROWS ? sDiCur - DI_ROWS : 0;
    }
    if (sDiCur != was) {
        Snd_PlaySe(1, 0);
    }
    if (sDiCur < sDiTop) {
        sDiTop = sDiCur;
    }
    if (sDiCur >= sDiTop + DI_ROWS) {
        sDiTop = sDiCur - DI_ROWS + 1;
    }
    i = sDiView[sDiCur];
    Di_NameRead(i);
    if (pad->gamePressed & PADG_CROSS) {
        Port_DiscTake(i, !Port_DiscTaken(i));
        Snd_PlaySe(1, 1);
    } else if (pad->gamePressed & PADG_SQUARE) {
        Snd_PlaySe(1, 1);
        sDiPicking = i;
        if (sDiItems[i].kind == 0) {
            Mm2_OpenPicker(5, MM_DISC, Port_DiscReplaces(sDiItems[i].index));
        } else {
            Mm2_TextEntry(KB_DISC, MM_DISC, sDiItems[i].name, 40);
        }
    }
}

static void Di_DrawWait(char *title) {
    f32 x0 = 334.0f;
    f32 x1 = 1043.0f;
    f32 k = (f32)sDiProg / 1000.0f;

    Mx_Window(294.0f, 208.0f, 1082.0f, 572.0f);
    Mx_Text(688.0f, 253.0f, title, 42.0f, 700.0f, MX_GOLD, 1);
    Mx_Box(MM_FIELD, x0, 294.0f, x1, 352.0f, 29.0f, 0x80);
    Mx_Box(MM_TAB_SEL, x0 + 6.0f, 300.0f, x0 + 40.0f + (x1 - x0 - 46.0f) * (k > 1.0f ? 1.0f : k), 346.0f, 23.0f, 0x80);
    sprintf(sLine, "%d%%", (int)(sDiProg / 10));
    Mx_Text(688.0f, 400.0f, sLine, 30.0f, 0.0f, MX_LAV, 1);
    Mx_Say(688.0f, 460.0f, "The disc image itself is only read.", 23.0f, 700.0f, MX_CYAN, 1);
}

static void Di_Draw(void) {
    PortDiscInfo *it;
    s32 row;
    s32 picked = 0;
    s32 i;

    if (sDiPhase == 1) {
        Di_DrawWait("READING THE DISC...");
        return;
    }
    if (sDiPhase == 3) {
        Di_DrawWait("ADDING THEM...");
        return;
    }
    for (i = 0; i < sDiCount; i++) {
        picked += Port_DiscTaken(i);
    }
    Mx_Box(MM_FIELD, 40.0f, 132.0f, 780.0f, 178.0f, 23.0f, 0x80);
    sprintf(sLine, "SHOW: %s%s", sDiFilterName[sDiFilter], sDiOnlyNew ? ", NEW ONLY" : "");
    Mx_Text(65.0f, 155.0f, sLine, 25.0f, 480.0f, MX_GOLD, 0);
    sprintf(sLine, "%d PICKED", (int)picked);
    Mx_Text(755.0f, 155.0f, sLine, 25.0f, 0.0f, MX_GOLD, 2);
    for (row = 0; row < DI_ROWS && sDiTop + row < sDiViewCount; row++) {
        f32 y0 = 192.0f + 70.0f * (f32)row;
        s32 at = sDiView[sDiTop + row];
        s32 sel = sDiTop + row == sDiCur;
        s32 took = Port_DiscTaken(at);
        MmTex *t = Di_ChipOf(at);

        it = &sDiItems[at];
        Mx_Row(40.0f, y0, 780.0f, y0 + 60.0f, sel, 0x80);
        if (t != NULL) {
            Mx_Tex(t, 64.0f, y0 + 4.0f, 116.0f, y0 + 56.0f, 0, 0, 64, 64, 0x80, 0x80);
        }
        Mx_Text(130.0f, y0 + (it->form[0] != 0 ? 19.0f : 30.0f), it->name, it->form[0] != 0 ? 23.0f : 27.0f, 510.0f, took ? MX_WHITE : MX_LAV, 0);
        if (it->form[0] != 0) {
            Mx_Text(130.0f, y0 + 43.0f, it->form, 20.0f, 510.0f, MX_CYAN, 0);
        }
        OnOff(took, 722.0f, y0 + 30.0f);
    }
    if (sDiViewCount == 0) {
        Mx_Say(410.0f, 300.0f, "Nothing here: this disc has no such files, or they are as in your game.", 24.0f, 700.0f, MX_WHITE, 1);
    }
    if (sDiTop > 0) {
        Mx_Fit(MM_UP, 410.0f, 184.0f, 20.0f, 0x80);
    }
    if (sDiTop + DI_ROWS < sDiViewCount) {
        Mx_Fit(MM_DOWN, 410.0f, 682.0f, 20.0f, 0x80);
    }
    Mx_Window(805.0f, 130.0f, 1340.0f, 686.0f);
    if (sDiViewCount > 0) {
        it = &sDiItems[sDiView[sDiCur]];
        if (it->kind == 0) {
            MmTex *t = Di_ChipOf(sDiView[sDiCur]);
            s32 r = Port_DiscReplaces(it->index);

            if (t != NULL) {
                Mx_Tex(t, 980.0f, 150.0f, 1164.0f, 330.0f, 0, 0, 64, 64, 0x80, 0x80);
            }
            Mx_Text(1072.0f, 360.0f, it->name, 34.0f, 490.0f, MX_WHITE, 1);
            Mx_Text(1072.0f, 398.0f, it->form, 25.0f, 490.0f, MX_GOLD, 1);
            sprintf(sLine, "SLOT %d%s%s%s", (int)it->index, it->formOf ? "  A FORM" : "", it->fusion ? "  A FUSION" : "", it->hasForms ? "  HAS FORMS" : "");
            Mx_Text(1072.0f, 434.0f, sLine, 22.0f, 490.0f, MX_CYAN, 1);
            if (r < 0) {
                Mx_Text(1072.0f, 472.0f, "ADDED AS A NEW FIGHTER", 24.0f, 490.0f, MX_LAV, 1);
            } else {
                sprintf(sLine, "REPLACES %.60s", Mm_RefName(r));
                Mx_Text(1072.0f, 472.0f, sLine, 24.0f, 490.0f, MX_ORANGE, 1);
            }
        } else {
            MmTex *t = Di_ChipOf(sDiView[sDiCur]);
            s32 h = it->kind == 1 ? 64 : 32;

            if (t != NULL) {
                Mx_Tex(t, 952.0f, 146.0f, 1192.0f, 300.0f, 0, 0, 64, 64, 0x80, 0x80);
            }
            Mx_Say(1072.0f, 318.0f, "What the disc calls it:", 22.0f, 490.0f, MX_CYAN, 1);
            if (sDiNameOf == sDiView[sDiCur]) {
                for (i = 0; i < 8; i++) {
                    if (sDiName[i] != NULL) {
                        Mx_Tex(sDiName[i]->tex, 828.0f + 61.0f * (f32)i, 338.0f, 889.0f + 61.0f * (f32)i, 338.0f + (f32)h * 0.95f, 0, 0, 64, h, 0x80, 0x80);
                    }
                }
            }
            Mx_Say(1072.0f, 416.0f, "It will be called:", 22.0f, 490.0f, MX_CYAN, 1);
            Mx_Text(1072.0f, 452.0f, it->name, 32.0f, 490.0f, MX_WHITE, 1);
        }
    }
    if (sDiLinks) {
        Mx_Text(1072.0f, 524.0f, "R1: THEIR FORMS AND FUSIONS COME ALONG", 21.0f, 500.0f, MX_LAV, 1);
    } else {
        Mx_Text(1072.0f, 524.0f, "R1: ONLY WHAT IS PICKED", 21.0f, 500.0f, MX_DIM, 1);
    }
    if (sDiCost < 0) {
        Mx_Text(1072.0f, 556.0f, "L1: TRANSFORMATIONS COST THE DISC'S OWN", 21.0f, 500.0f, MX_LAV, 1);
    } else {
        sprintf(sLine, "L1: TRANSFORMATIONS COST %d BLAST STOCKS", (int)sDiCost);
        Mx_Text(1072.0f, 556.0f, sLine, 21.0f, 500.0f, MX_LAV, 1);
    }
    Mx_Rect(813.0f, 604.0f, 1332.0f, 678.0f, Mx_Rgba(0x06, 0x1A, 0x24, 0x70), Mx_Rgba(0x06, 0x14, 0x1C, 0x78));
    Mx_Say(1072.0f, 641.0f, "Square names a stage or a song, or says what a character replaces.", 22.0f, 510.0f, MX_WHITE, 1);
}

/* ================================================================================================ a character's pages */
#define NU_ROWS 8
static s32 sNuTable;
static s32 sNuRow;
static s32 sNuCur;
static s32 sNuTop;
static char sNuTableName[48];
static s32 sNuRows;
static s32 sNuCols;
static char sNuNote[160];
static s32 sMvCur;
static s32 sMvTop;
static s32 sMsCur;
/* The customize page's rows: the five moves one by one (Port_CeTakeMove: 0, 1 Blast 1, 2, 3 Blast 2, 4 Ultimate), then a
   whole moveset, then the character's own files back. */
#define MS_MOVES 5
#define MS_WHOLE MS_MOVES
#define MS_BACK (MS_MOVES + 1)
static s32 sMsMove = -1;       /* the row whose fighter is being picked */
static char sMsFrom[MS_MOVES][48]; /* what each row took in this visit: "Move name (fighter)" */
static char sMsOwn[MS_MOVES * 48]; /* the character's own five moves' names, from its skill list */
static s32 sMsStage;               /* 0 the rows; 1 choosing which of the picked fighter's moves to take */
static s32 sMsRef;                 /* the picked fighter (a game character, or PORT_REF_MOD + mod) */
static char sMsDon[MS_MOVES * 48]; /* that fighter's five moves' names */
static s32 sMsDonCur;              /* the one chosen among them */
static char *sMsMoveName[MS_MOVES] = {"BLAST 1 (A)", "BLAST 1 (B)", "BLAST 2 (A)", "BLAST 2 (B)", "ULTIMATE"};

static void Nu_Table(void) {
    Port_CeTable(sNuTable, sNuTableName, &sNuRows, &sNuCols, sNuNote);
    if (sNuRow >= sNuRows) {
        sNuRow = 0;
    }
    if (sNuCur >= sNuCols) {
        sNuCur = 0;
        sNuTop = 0;
    }
}

/* its model read for the page; without one, a note and the Library */
static s32 Page_Open(s32 mode) {
    PortModInfo *m = &sMm->info[sMm->cur];

    if (!Port_CeOpen(sMm->cur, m->chara, sWhy)) {
        static char note[64];

        sprintf(note, "%.60s", sWhy);
        Mm_Note(note);
        Snd_PlaySe(1, 2);
        return 0;
    }
    Snd_PlaySe(1, 1);
    Mm_SetMode(mode);
    return 1;
}

void Mm3_OpenNumbers(void) {
    if (Page_Open(MM_NUMBERS)) {
        Nu_Table();
    }
}

void Mm3_OpenMoves(void) {
    sMvCur = 0;
    sMvTop = 0;
    Page_Open(MM_MOVES);
}

void Mm3_OpenMoveset(void) {
    s32 k;

    sMsCur = 0;
    sMsStage = 0;
    for (k = 0; k < MS_MOVES; k++) {
        sMsFrom[k][0] = 0;
    }
    Port_CeSlotNames(sMm->cur, -1, sMsOwn);
    Snd_PlaySe(1, 1);
    Mm_SetMode(MM_MOVESET);
}

/* back to the Library, what was changed saved into every costume */
static void Page_Leave(void) {
    if (Port_CeDirty()) {
        Mm_ViewRelease();
        if (Port_CeSave(sWhy)) {
            Mm_Note("SAVED INTO EVERY COSTUME");
            Snd_PlaySe(1, 1);
        } else {
            static char note[64];

            sprintf(note, "%.60s", sWhy);
            Mm_Note(note);
            Snd_PlaySe(1, 2);
        }
    } else {
        Snd_PlaySe(1, 2);
    }
    Port_CeClose();
    Mm_SetMode(MM_LIBRARY);
}

static void Nu_Update(s32 live) {
    Pad *pad = &gPad[0];
    s32 was = sNuCur;

    sMm->wantKey = -1;
    Mm_Prompt(3, "CHANGE");
    Mm_Prompt(0, "TYPE IT");
    Mm_Prompt(14, "TABLE");
    if (sNuRows > 1) {
        Mm_Prompt(10, "WHICH");
    }
    Mm_Prompt(1, "BIG STEP");
    Mm_Prompt(2, "SAVE, BACK");
    if (!live) {
        return;
    }
    if (pad->gamePressed & PADG_TRIANGLE) {
        Page_Leave();
        return;
    }
    if (pad->gamePressed & (PADG_L1 | PADG_R1)) {
        sNuTable = (sNuTable + ((pad->gamePressed & PADG_R1) ? 1 : Port_CeTables() - 1)) % Port_CeTables();
        sNuRow = 0;
        sNuCur = 0;
        sNuTop = 0;
        Nu_Table();
        Snd_PlaySe(1, 0);
        return;
    }
    if (sNuRows > 1 && (pad->gameRepeat & (PADG_L2 | PADG_R2))) {
        sNuRow = (sNuRow + ((pad->gameRepeat & PADG_R2) ? 1 : sNuRows - 1)) % sNuRows;
        Snd_PlaySe(1, 0);
        return;
    }
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        sNuCur = (sNuCur + 1) % sNuCols;
    } else if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        sNuCur = (sNuCur + sNuCols - 1) % sNuCols;
    }
    if (sNuCur != was) {
        Snd_PlaySe(1, 0);
    }
    if (sNuCur < sNuTop) {
        sNuTop = sNuCur;
    }
    if (sNuCur >= sNuTop + NU_ROWS) {
        sNuTop = sNuCur - NU_ROWS + 1;
    }
    if (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2 | PADG_RIGHT | PADG_RIGHT2)) {
        s32 dir = (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2)) ? -1 : 1;

        if (Port_CeStep(sNuTable, sNuRow, sNuCur, dir, (pad->gameHeld & PADG_SQUARE) != 0)) {
            Snd_PlaySe(1, 0);
        } else {
            Snd_PlaySe(1, 2);
        }
    } else if (pad->gamePressed & PADG_CROSS) {
        static char text[40];

        Port_CeValue(sNuTable, sNuRow, sNuCur, text);
        Snd_PlaySe(1, 1);
        Mm2_TextEntry(KB_NUM, MM_NUMBERS, text, 20);
    }
}

static void Nu_Draw(void) {
    static char name[48];
    static char hint[48];
    static char value[40];
    static char lab[48];
    s32 k;

    Mx_Box(MM_FIELD, 60.0f, 132.0f, 1316.0f, 178.0f, 23.0f, 0x80);
    Mx_Text(85.0f, 155.0f, sNuTableName, 28.0f, 500.0f, MX_GOLD, 0);
    if (sNuRows > 1) {
        Port_CeRow(sNuTable, sNuRow, lab);
        sprintf(sLine, "%d / %d: %.40s", (int)(sNuRow + 1), (int)sNuRows, lab);
        Mx_Text(1291.0f, 155.0f, sLine, 25.0f, 640.0f, MX_CYAN, 2);
    }
    for (k = 0; k < NU_ROWS && sNuTop + k < sNuCols; k++) {
        f32 y0 = 190.0f + 58.0f * (f32)k;
        s32 at = sNuTop + k;
        s32 sel = at == sNuCur;

        Port_CeCol(sNuTable, at, name, hint);
        Port_CeValue(sNuTable, sNuRow, at, value);
        Mx_Row(60.0f, y0, 1316.0f, y0 + 50.0f, sel, 0x80);
        Mx_Text(88.0f, y0 + 25.0f, name, 25.0f, 500.0f, MX_LAV, 0);
        Mx_Text(800.0f, y0 + 25.0f, value, 27.0f, 200.0f, sel ? MX_WHITE : MX_GOLD, 1);
        if (sel) {
            Mx_Fit(MM_LEFT, 680.0f, y0 + 25.0f, 24.0f, 0x80);
            Mx_Fit(MM_RIGHT, 920.0f, y0 + 25.0f, 24.0f, 0x80);
        }
        Mx_Say(1290.0f, y0 + 25.0f, hint, 20.0f, 340.0f, MX_CYAN, 2);
    }
    if (sNuTop > 0) {
        Mx_Fit(MM_UP, 688.0f, 183.0f, 16.0f, 0x80);
    }
    if (sNuTop + NU_ROWS < sNuCols) {
        Mx_Fit(MM_DOWN, 688.0f, 655.0f, 16.0f, 0x80);
    }
    Mx_Say(688.0f, 672.0f, sNuNote, 21.0f, 1240.0f, MX_DARK, 1);
}

static void Mv_Update(s32 live) {
    Pad *pad = &gPad[0];
    s32 n = Port_CeMoves();
    s32 was = sMvCur;
    static char cost[8];
    static char name[96];

    sMm->wantKey = -1;
    if (n > 0) {
        Mm_Prompt(0, "RENAME");
        Mm_Prompt(3, "SHOWN COST");
    }
    Mm_Prompt(2, "SAVE, BACK");
    if (!live) {
        return;
    }
    if (pad->gamePressed & PADG_TRIANGLE) {
        Page_Leave();
        return;
    }
    if (n <= 0) {
        return;
    }
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        sMvCur = (sMvCur + 1) % n;
    } else if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        sMvCur = (sMvCur + n - 1) % n;
    }
    if (sMvCur != was) {
        Snd_PlaySe(1, 0);
    }
    if (sMvCur < sMvTop) {
        sMvTop = sMvCur;
    }
    if (sMvCur >= sMvTop + NU_ROWS) {
        sMvTop = sMvCur - NU_ROWS + 1;
    }
    Port_CeMove(sMvCur, cost, name);
    if (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2 | PADG_RIGHT | PADG_RIGHT2)) {
        s32 v = (cost[0] - '0') * 100 + (cost[1] - '0') * 10 + (cost[2] - '0');

        v += (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2)) ? -10 : 10;
        v = v < 0 ? 0 : v > 999 ? 999 : v;
        sprintf(cost, "%03d", (int)v);
        Port_CeMoveSet(sMvCur, cost, name);
        Snd_PlaySe(1, 0);
    } else if (pad->gamePressed & PADG_CROSS) {
        Snd_PlaySe(1, 1);
        Mm2_TextEntry(KB_MOVE, MM_MOVES, name, 60);
    }
}

static void Mv_Draw(void) {
    static char cost[8];
    static char name[96];
    s32 n = Port_CeMoves();
    s32 k;

    Mx_Box(MM_FIELD, 60.0f, 132.0f, 1316.0f, 178.0f, 23.0f, 0x80);
    Mx_Text(85.0f, 155.0f, "THE SKILL LIST'S MOVES", 28.0f, 0.0f, MX_GOLD, 0);
    sprintf(sLine, "%.60s", sMm->info[sMm->cur].name);
    Mx_Text(1291.0f, 155.0f, sLine, 25.0f, 600.0f, MX_CYAN, 2);
    for (k = 0; k < NU_ROWS && sMvTop + k < n; k++) {
        f32 y0 = 190.0f + 58.0f * (f32)k;
        s32 at = sMvTop + k;

        Port_CeMove(at, cost, name);
        Mx_Row(60.0f, y0, 1316.0f, y0 + 50.0f, at == sMvCur, 0x80);
        Mx_Text(88.0f, y0 + 25.0f, cost, 26.0f, 80.0f, MX_GOLD, 0);
        Mx_Text(190.0f, y0 + 25.0f, name, 26.0f, 1090.0f, at == sMvCur ? MX_WHITE : MX_LAV, 0);
    }
    if (n == 0) {
        Mx_Say(688.0f, 300.0f, "This model's skill list names no moves.", 26.0f, 900.0f, MX_WHITE, 1);
    }
    if (sMvTop > 0) {
        Mx_Fit(MM_UP, 688.0f, 183.0f, 16.0f, 0x80);
    }
    if (sMvTop + NU_ROWS < n) {
        Mx_Fit(MM_DOWN, 688.0f, 655.0f, 16.0f, 0x80);
    }
    Mx_Say(688.0f, 672.0f, "A name is changed in every language of the list. The number is what the list shows; costs are in the Numbers page.", 21.0f, 1240.0f, MX_DARK, 1);
}

/* the moveset page: take one, or put its own files back */
static void Ms_Update(s32 live) {
    Pad *pad = &gPad[0];
    s32 back = Port_CeHasBackup(sMm->cur);

    sMm->wantKey = -1;
    Mm_Prompt(0, "CHOOSE");
    Mm_Prompt(2, "BACK");
    if (!live) {
        return;
    }
    if (sMsStage == 1) { /* which of the picked fighter's moves: the same kind only (Blast 1 for Blast 1; Blast 2 and Ultimate) */
        s32 lo = sMsMove < 2 ? 0 : 2, hi = sMsMove < 2 ? 1 : 4;

        if (pad->gamePressed & (PADG_TRIANGLE | PADG_CIRCLE)) {
            sMsStage = 0;
            Snd_PlaySe(1, 2);
        } else if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
            sMsDonCur = sMsDonCur >= hi ? lo : sMsDonCur + 1;
            Snd_PlaySe(1, 0);
        } else if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
            sMsDonCur = sMsDonCur <= lo ? hi : sMsDonCur - 1;
            Snd_PlaySe(1, 0);
        } else if (pad->gamePressed & PADG_CROSS) {
            s32 ok = sMsRef >= PORT_REF_MOD ? sMsRef - PORT_REF_MOD != sMm->cur && Port_CeTakeMove(sMm->cur, -1, sMsRef - PORT_REF_MOD, sMsMove, sMsDonCur, sWhy)
                                           : Port_CeTakeMove(sMm->cur, sMsRef, -1, sMsMove, sMsDonCur, sWhy);

            if (ok) {
                sprintf(sMsFrom[sMsMove], "%.26s (%.18s)", sMsDon[sMsDonCur * 48] != 0 ? &sMsDon[sMsDonCur * 48] : "?", Mm_RefName(sMsRef));
                sprintf(sLine, "%.28s TAKEN INTO %s", &sMsDon[sMsDonCur * 48], sMsMoveName[sMsMove]);
                Mm_Note(sLine);
                sMsStage = 0;
                Snd_PlaySe(1, 1);
            } else {
                static char note[64];

                sprintf(note, "%.60s", sWhy[0] != 0 ? sWhy : "THAT FIGHTER'S FILES CANNOT BE USED");
                Mm_Note(note);
                Snd_PlaySe(1, 2);
            }
        }
        return;
    }
    if (pad->gamePressed & (PADG_TRIANGLE | PADG_CIRCLE)) {
        Snd_PlaySe(1, 2);
        Mm_SetMode(MM_LIBRARY);
        return;
    }
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        sMsCur = (sMsCur + 1) % (MS_BACK + 1);
        Snd_PlaySe(1, 0);
    }
    if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        sMsCur = (sMsCur + MS_BACK) % (MS_BACK + 1);
        Snd_PlaySe(1, 0);
    }
    if (pad->gamePressed & PADG_CROSS) {
        if (sMsCur <= MS_WHOLE) {
            sMsMove = sMsCur < MS_MOVES ? sMsCur : -1;
            Snd_PlaySe(1, 1);
            Mm2_OpenPicker(6, MM_MOVESET, 0);
        } else if (back) {
            s32 k;

            Port_CeRestore(sMm->cur);
            for (k = 0; k < MS_MOVES; k++) {
                sMsFrom[k][0] = 0;
            }
            Port_CeSlotNames(sMm->cur, -1, sMsOwn);
            Mm_Note("ITS OWN FILES ARE BACK");
            Snd_PlaySe(1, 1);
        } else {
            Snd_PlaySe(1, 2);
        }
    }
}

static void Ms_Draw(void) {
    s32 back = Port_CeHasBackup(sMm->cur);
    s32 k;

    Mx_Window(240.0f, 150.0f, 1136.0f, 660.0f);
    sprintf(sLine, "%.60s", sMm->info[sMm->cur].name);
    Mx_Text(688.0f, 196.0f, sLine, 36.0f, 820.0f, MX_GOLD, 1);
    if (sMsStage == 1) {
        s32 lo = sMsMove < 2 ? 0 : 2, hi = sMsMove < 2 ? 1 : 4;

        sprintf(sLine, "%s: TAKE WHICH MOVE OF %.40s?", sMsMoveName[sMsMove], Mm_RefName(sMsRef));
        Mx_Say(688.0f, 250.0f, sLine, 24.0f, 960.0f, MX_WHITE, 1);
        for (k = lo; k <= hi; k++) {
            f32 y0 = 310.0f + 60.0f * (f32)(k - lo);

            Mx_Row(360.0f, y0, 1016.0f, y0 + 50.0f, k == sMsDonCur, 0x80);
            sprintf(sLine, "%s: %.40s", sMsMoveName[k], sMsDon[k * 48] != 0 ? &sMsDon[k * 48] : "(NO NAME)");
            Mx_TextA(688.0f, y0 + 25.0f, sLine, 26.0f, 600.0f, k == sMsDonCur ? MX_WHITE : MX_LAV, 1, 0x80);
        }
        Mx_Say(688.0f, 560.0f, "Its numbers and animations replace this move's. Triangle goes back.", 22.0f, 1000.0f, MX_CYAN, 1);
        return;
    }
    Mx_Say(688.0f, 240.0f, "Pick a move, then the fighter to take that one move from: its numbers and animations.", 22.0f, 960.0f, MX_WHITE, 1);
    Mx_Say(688.0f, 268.0f, "The effects (beam and ball pictures) stay its own. Own files are kept, so they can be put back.", 22.0f, 1000.0f, MX_CYAN, 1);
    for (k = 0; k <= MS_BACK; k++) {
        f32 y0 = 304.0f + 54.0f * (f32)k;
        s32 off = k == MS_BACK && !back;

        Mx_Row(360.0f, y0, 1016.0f, y0 + 46.0f, k == sMsCur, 0x80);
        if (k < MS_MOVES) {
            sprintf(sLine, "%s: %.44s", sMsMoveName[k], sMsFrom[k][0] != 0 ? sMsFrom[k] : sMsOwn[k * 48] != 0 ? &sMsOwn[k * 48] : "(NO NAME)");
        } else {
            sprintf(sLine, "%s", k == MS_WHOLE ? "ALL OF A MOVESET FROM..." : "PUT ITS OWN FILES BACK");
        }
        Mx_TextA(688.0f, y0 + 23.0f, sLine, 25.0f, 600.0f, k == sMsCur ? MX_WHITE : MX_LAV, 1, off ? 0x60 : 0x80);
    }
    if (back) {
        Mx_Say(688.0f, 750.0f, "Changed: the last line undoes numbers, names and moveset.", 21.0f, 1000.0f, MX_DARK, 1);
    }
}

/* ================================================================================================== a 3D model file */
/* The model's page: the choices at the left (the fighter it builds on, the fit, which way is up and where it faces, the
   arms' angle, the heights of shoulder, waist and neck, the textures, the rig), the model built from them at the right
   in the viewer. It is built again when a choice is left alone for a moment, so the page can be worked until the model
   stands as it should; "Make the fighter" adds it as a mod. 0 on a choice = decided from the file. */
enum { MO_BASE, MO_FIT, MO_UP, MO_TURN, MO_MIRROR, MO_SIZE, MO_ARMS, MO_SHO, MO_WAIST, MO_NECK, MO_TEX, MO_RIG, MO_TARGET, MO_TRIS, MO_TEXSZ, MO_TEST, MO_MAKE, MO_ROWS };
/* Port_MiOpt: 0 up, 1 facing, 2 mirror, 3 size, 4 arms, 5 shoulder, 6 waist, 7 neck, 8 textures, 9 rig, 10 target (this
   game or a PS2 disc), 11 triangles (MI_TRI_STEPS of plat_modelimport.h; 8 all), 12 texture size */
static const s32 sMoOptOf[MO_ROWS] = {-1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -1, -1};
#define MO_OPTS 13

static char sMoPath[512];
static s32 sMoCur, sMoBase, sMoFit, sMoGo, sMoBack, sMoFrom;
static s32 sMoIdle;       /* frames since a choice changed */
static s32 sMoDirty;      /* a choice changed since the last build started */
static s32 sMoState;      /* 0 nothing built, 1 building, 2 built, 3 failed */
static s32 sMoSeq;        /* the build the viewer shows */
static s32 sMoChara;
static s32 sMoWant;       /* the viewer is asked for the staging package */
static char sMoFound[220], sMoWhy[410], sMoMiss[170];
static s32 sMoSeen[4];    /* what the last build chose: arms (degrees), shoulder, waist, neck; -1 unknown */

s32 Mm3_IsModel(char *path) {
    return !sMoGo && Port_MiIsModel(path);
}

static s32 Mo_Num(char *s, char *key) { /* the number after `key` in s, -1 none */
    s32 i, k;

    for (i = 0; s[i] != 0; i++) {
        for (k = 0; key[k] != 0 && s[i + k] == key[k]; k++) {
        }
        if (key[k] == 0) {
            s32 v = 0;
            s32 any = 0;

            i += k;
            while (s[i] == ' ') {
                i++;
            }
            while (s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + (s[i] - '0');
                i++;
                any = 1;
            }
            return any ? v : -1;
        }
    }
    return -1;
}

/* The importer ends its report with "; textures not found: a, b" (MI_MISSING in plat_modelimport.h): those names go to
   sMoMiss, for the page to ask the owner for the pictures. */
static void Mo_SplitMissing(void) {
    static const char mark[] = "; textures not found: ";
    s32 i, k;

    sMoMiss[0] = 0;
    for (i = 0; sMoWhy[i] != 0; i++) {
        for (k = 0; mark[k] != 0 && sMoWhy[i + k] == mark[k]; k++) {
        }
        if (mark[k] == 0) {
            s32 n;

            for (n = 0; sMoWhy[i + k + n] != 0 && n < 168; n++) {
                sMoMiss[n] = sMoWhy[i + k + n];
            }
            sMoMiss[n] = 0;
            sMoWhy[i] = 0;
            return;
        }
    }
}

static void Mo_Start(void) {
    Port_MiSet(sMoBase, sMoFit);
    if (Port_MiTestStart(sMoPath)) {
        sMoState = 1;
        sMoDirty = 0;
    }
}

void Mm3_OpenModel(char *path, s32 from) {
    s32 n;

    for (n = 0; n < 511 && path[n] != 0; n++) {
        sMoPath[n] = path[n];
    }
    sMoPath[n] = 0;
    sMoCur = MO_UP;
    sMoFrom = from;
    sMoBack = sMm->mode;
    for (n = 0; n < MO_OPTS; n++) {
        Port_MiOpt(n, 0);
    }
    sMoState = 0;
    sMoWant = 0;
    sMoFound[0] = 0;
    sMoWhy[0] = 0;
    sMoMiss[0] = 0;
    for (n = 0; n < 4; n++) {
        sMoSeen[n] = -1;
    }
    sMoIdle = 0;
    Mm_Hand(0);
    Mm_SetMode(MM_MODEL);
    Mo_Start(); /* (so that the first look is already building) */
}

/* the value of a choice as a line */
static void Mo_Value(s32 row, char *out) {
    static const char *const up[7] = {"AUTO", "+Y", "+Z", "+X", "-Y", "-Z", "-X"};
    static const char *const turn[5] = {"AUTO", "0", "90", "180", "270"};
    s32 o = sMoOptOf[row] >= 0 ? Port_MiOptGet(sMoOptOf[row]) : 0;
    s32 seen = row == MO_ARMS ? sMoSeen[0] : row == MO_SHO ? sMoSeen[1] : row == MO_WAIST ? sMoSeen[2] : row == MO_NECK ? sMoSeen[3] : -1;

    switch (row) {
    case MO_UP:
        sprintf(out, "%s", up[o < 0 || o > 6 ? 0 : o]);
        break;
    case MO_TURN:
        sprintf(out, "%s", turn[o < 0 || o > 4 ? 0 : o]);
        break;
    case MO_MIRROR:
    case MO_TEX:
    case MO_RIG:
        sprintf(out, "%s", row == MO_MIRROR ? (o ? "ON" : "OFF") : row == MO_TEX ? (o ? "DITHERED" : "SMOOTH") : (o ? "BY DISTANCE (OLD)" : "AUTOMATIC JOINTS"));
        break;
    case MO_SIZE:
        sprintf(out, "%d%%", o > 0 ? o : 100);
        break;
    case MO_TARGET:
        sprintf(out, "%s", o ? "PS2 DISC (SMALL)" : "THIS GAME (UP TO 8 MB)");
        break;
    case MO_TRIS: {
        static const char *const tris[9] = {"AUTO (7,000)", "2,000", "4,000", "7,000", "10,000", "15,000", "25,000", "40,000", "ALL (AS THE FILE)"};

        sprintf(out, "%s", tris[o < 0 || o > 8 ? 0 : o]);
        break;
    }
    case MO_TEXSZ: {
        static const char *const side[4] = {"AUTO", "64 X 64", "128 X 128", "256 X 256 (BEST)"};

        sprintf(out, "%s", side[o < 0 || o > 3 ? 0 : o]);
        break;
    }
    case MO_ARMS:
        if (o > 0) {
            sprintf(out, "%d DEG", (o - 1) * 5);
        } else if (seen >= 0) {
            sprintf(out, "AUTO (%d DEG)", seen);
        } else {
            sprintf(out, "AUTO");
        }
        break;
    default: /* heights in percent */
        if (o > 0) {
            sprintf(out, "%d%%", o);
        } else if (seen >= 0) {
            sprintf(out, "AUTO (%d%%)", seen);
        } else {
            sprintf(out, "AUTO");
        }
        break;
    }
}

/* a choice moved one step; 1 when it changed */
static s32 Mo_Step(s32 row, s32 dir) {
    s32 which = sMoOptOf[row];
    s32 o;
    s32 seen;

    if (which < 0) {
        return 0;
    }
    o = Port_MiOptGet(which);
    switch (row) {
    case MO_UP:
        o = (o + dir + 7) % 7;
        break;
    case MO_TURN:
        o = (o + dir + 5) % 5;
        break;
    case MO_MIRROR:
    case MO_TEX:
    case MO_RIG:
        o ^= 1;
        break;
    case MO_TARGET:
        o ^= 1;
        break;
    case MO_TRIS:
        o = (o + dir + 9) % 9;
        break;
    case MO_TEXSZ:
        o = (o + dir + 4) % 4;
        break;
    case MO_SIZE:
        o = (o > 0 ? o : 100) + dir * 5;
        o = o < 50 ? 50 : o > 150 ? 150 : o;
        o = o == 100 ? 0 : o;
        break;
    case MO_ARMS:
        seen = sMoSeen[0] >= 0 ? sMoSeen[0] / 5 + 1 : 1;
        o = (o > 0 ? o : seen) + dir;
        o = o < 1 ? 1 : o > 19 ? 19 : o;
        break;
    default:
        seen = row == MO_SHO ? sMoSeen[1] : row == MO_WAIST ? sMoSeen[2] : sMoSeen[3];
        o = (o > 0 ? o : seen > 0 ? seen : 50) + dir;
        o = o < 20 ? 20 : o > 99 ? 99 : o;
        break;
    }
    Port_MiOpt(which, o);
    return 1;
}

static void Mo_Leave(void) {
    Mm_Hand(0);
    Port_MiTestEnd();
    sMoWant = 0;
}

static void Mo_Update(s32 live) {
    Pad *pad = &gPad[0];
    static char info[650];
    s32 st;

    sMm->wantKey = -1;
    if (sMoWant) { /* the model built, in the viewer */
        sMm->wantKey = 0x30000 + sMoSeq;
        sMm->wantChara = sMoChara;
        sMm->wantCostume = 0;
        sMm->wantPreview = -2;
    }
    Mm_SetModel(1130.0f, 650.0f, 430.0f);
    Mm_Prompt(0, sMoCur == MO_TEST ? "BUILD" : sMoCur == MO_MAKE ? "MAKE" : "CHOOSE");
    Mm_Prompt(2, "BACK");
    st = Port_MiTestPoll(info, 650);
    if (sMoState == 1 && st >= 2) {
        if (st == 2) {
            s32 i = 0;

            while (info[i] != 0 && info[i] != '|') {
                i++;
            }
            sMoFound[0] = 0;
            sMoWhy[0] = 0;
            {
                s32 k;

                for (k = 0; k < i && k < 218; k++) {
                    sMoFound[k] = info[k];
                }
                sMoFound[k] = 0;
                for (k = 0; info[i] != 0 && k < 408; k++) {
                    i++;
                    sMoWhy[k] = info[i];
                }
                sMoWhy[k] = 0;
                Mo_SplitMissing();
            }
            sMoSeen[0] = Mo_Num(sMoFound, "ARMS");
            sMoSeen[1] = Mo_Num(sMoFound, "SHOULDER");
            sMoSeen[2] = Mo_Num(sMoFound, "WAIST");
            sMoSeen[3] = Mo_Num(sMoFound, "NECK");
            if (Port_MiTestLook(&sMoChara)) {
                sMoSeq++;
                sMoWant = 1;
                sMoState = 2;
            } else {
                sMoState = 3;
                sprintf(sMoWhy, "The test could not be shown");
            }
        } else {
            sMoState = 3;
            sMoFound[0] = 0;
            {
                s32 k;

                for (k = 0; info[k] != 0 && k < 408; k++) {
                    sMoWhy[k] = info[k];
                }
                sMoWhy[k] = 0;
            }
        }
    }
    if (sMoDirty) {
        sMoIdle++;
        if (sMoIdle > 24 && sMoState != 1) {
            Mo_Start();
        }
    }
    if (!live) {
        return;
    }
    if (pad->gamePressed & (PADG_TRIANGLE | PADG_CIRCLE)) {
        Snd_PlaySe(1, 2);
        Mo_Leave();
        Mm_SetMode(sMoBack);
        return;
    }
    if (pad->gameRepeat & PADG_L2) {
        Mm_TurnBy(-0.07f);
    }
    if (pad->gameRepeat & PADG_R2) {
        Mm_TurnBy(0.07f);
    }
    if (pad->gamePressed & PADG_SQUARE) {
        Mm_Hand(0);
        Snd_PlaySe(1, 0);
    }
    if (pad->gameRepeat & (PADG_DOWN | PADG_DOWN2)) {
        sMoCur = (sMoCur + 1) % MO_ROWS;
        Snd_PlaySe(1, 0);
    }
    if (pad->gameRepeat & (PADG_UP | PADG_UP2)) {
        sMoCur = (sMoCur + MO_ROWS - 1) % MO_ROWS;
        Snd_PlaySe(1, 0);
    }
    if (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2 | PADG_RIGHT | PADG_RIGHT2)) {
        s32 dir = (pad->gameRepeat & (PADG_LEFT | PADG_LEFT2)) ? -1 : 1;

        if (sMoCur == MO_FIT) {
            sMoFit ^= 1;
            sMoDirty = 1;
            sMoIdle = 0;
            Snd_PlaySe(1, 0);
        } else if (Mo_Step(sMoCur, dir)) {
            sMoDirty = 1;
            sMoIdle = 0;
            Snd_PlaySe(1, 0);
        }
    }
    if (pad->gamePressed & PADG_CROSS) {
        Snd_PlaySe(1, 1);
        if (sMoCur == MO_BASE) {
            Mm2_OpenPicker(7, MM_MODEL, sMoBase);
        } else if (sMoCur == MO_FIT) {
            sMoFit ^= 1;
            sMoDirty = 1;
            sMoIdle = 0;
        } else if (sMoCur == MO_TEST) {
            sMoIdle = 99;
            sMoDirty = 1;
        } else if (sMoCur == MO_MAKE) {
            Mo_Leave();
            Port_MiSet(sMoBase, sMoFit);
            Mm_SetMode(sMoBack);
            sMoGo = 1;
            Mm_StartImport(sMoPath, 0, sMoFrom);
            sMoGo = 0;
        } else if (Mo_Step(sMoCur, 1)) {
            sMoDirty = 1;
            sMoIdle = 0;
        }
    }
}

static void Mo_Draw(void) {
    s32 k;
    s32 n = 0;
    s32 from = 0;
    static const char *const names[MO_ROWS] = {"BASE FIGHTER", "FIT", "UP", "TURN", "MIRROR", "SIZE", "ARMS", "SHOULDERS", "WAIST", "NECK", "TEXTURES", "RIG", "FOR", "TRIANGLES", "TEXTURE SIZE", "BUILD AGAIN", "MAKE THE FIGHTER"};

    while (sMoPath[n] != 0) { /* the file's name without its folders */
        if (sMoPath[n] == '/' || sMoPath[n] == 0x5C) {
            from = n + 1;
        }
        n++;
    }
    Mx_Window(40.0f, 140.0f, 860.0f, 712.0f);
    sprintf(sLine, "%.50s", sMoPath + from);
    Mx_Text(450.0f, 168.0f, sLine, 27.0f, 760.0f, MX_GOLD, 1);
    for (k = 0; k < MO_ROWS; k++) {
        f32 y0 = 186.0f + 27.0f * (f32)k;
        static char val[48];

        Mx_Row(70.0f, y0, 830.0f, y0 + 25.0f, k == sMoCur, 0x80);
        Mx_TextA(94.0f, y0 + 12.5f, (char *)names[k], 20.0f, 300.0f, k == sMoCur ? MX_WHITE : MX_LAV, 0, 0x80);
        if (k == MO_BASE) {
            sprintf(val, "%.34s", Mm_RefName(sMoBase));
        } else if (k == MO_FIT) {
            sprintf(val, "%s", sMoFit ? "THE SKELETON TAKES THE SHAPE" : "THE MODEL GOES ONTO THE SKELETON");
        } else if (k < MO_TEST) {
            Mo_Value(k, val);
        } else {
            val[0] = 0;
        }
        if (val[0] != 0) {
            Mx_TextA(806.0f, y0 + 12.5f, val, 20.0f, 470.0f, k == sMoCur ? MX_WHITE : MX_LAV, 2, 0x80);
        }
    }
    if (sMoState == 2 && !sMoDirty && sMoMiss[0] != 0) { /* the file names pictures that are not there: ask for them */
        sprintf(sLine, "TEXTURES NOT FOUND: %.150s", sMoMiss);
        Mx_Say(450.0f, 664.0f, sLine, 19.0f, 780.0f, MX_GOLD, 1);
        Mx_Say(450.0f, 690.0f, "Put them beside the model (PNG, JPG, TGA or BMP, same name), then BUILD AGAIN", 19.0f, 780.0f, MX_WHITE, 1);
    } else {
        sprintf(sLine, "%s", sMoState == 1 || sMoDirty ? "BUILDING THE TEST..." : sMoState == 2 ? sMoFound : sMoState == 3 ? "THE TEST FAILED" : "");
        Mx_Say(450.0f, 664.0f, sLine, 19.0f, 780.0f, MX_CYAN, 1);
        sprintf(sLine, "%s", sMoState == 3 ? sMoWhy : sMoState == 2 ? sMoWhy : "L2 / R2 turn the model, Square turns it by itself again. Left / right change a choice");
        Mx_Say(450.0f, 690.0f, sLine, 19.0f, 780.0f, MX_WHITE, 1);
    }
    if (!sMoWant || sMoState == 3) {
        Mx_Window(900.0f, 250.0f, 1340.0f, 450.0f);
        Mx_Text(1120.0f, 310.0f, sMoState == 3 ? "NO MODEL" : "BUILDING", 30.0f, 380.0f, MX_GOLD, 1);
        Mx_Say(1120.0f, 360.0f, sMoState == 3 ? sMoWhy : "The model comes here when it is built", 21.0f, 400.0f, MX_WHITE, 1);
    }
}

/* ========================================================================================================= the hooks */
/* A picker's answer for these screens: 5 what a disc's character replaces, 6 whose moveset. */
void Mm3_Picked(s32 what, s32 ref) {
    if (what == 7) { /* the base fighter of a 3D model */
        if (ref >= 0 && ref < MM_CHARA_COUNT) {
            sMoBase = ref;
            sMoDirty = 1;
            sMoIdle = 0;
        }
        return;
    }
    if (what == 5) {
        Port_DiscSetReplaces(sDiItems[sDiPicking].index, ref >= 0 && ref < MM_CHARA_COUNT ? ref : -1);
    } else if (what == 6) {
        s32 ok;

        if (sMsMove >= 0) { /* one move: first which of the fighter's, by name (the page's second stage) */
            if (ref >= 0 && (ref >= PORT_REF_MOD ? ref - PORT_REF_MOD != sMm->cur : ref < MM_CHARA_COUNT)) {
                sMsRef = ref;
                sMsStage = 1;
                sMsDonCur = sMsMove;
                Port_CeSlotNames(ref >= PORT_REF_MOD ? ref - PORT_REF_MOD : -1, ref >= PORT_REF_MOD ? -1 : ref, sMsDon);
            }
            return;
        }
        Mm_ViewRelease();
        if (ref >= PORT_REF_MOD) {
            ok = ref - PORT_REF_MOD != sMm->cur && Port_CeTake(sMm->cur, -1, ref - PORT_REF_MOD, sWhy);
        } else {
            ok = ref >= 0 && Port_CeTake(sMm->cur, ref, -1, sWhy);
        }
        if (ok) {
            s32 k;

            for (k = 0; k < MS_MOVES; k++) {
                sMsFrom[k][0] = 0; /* (the skill list came along: its own names are the donor's now) */
            }
            Port_CeSlotNames(sMm->cur, -1, sMsOwn);
            sprintf(sLine, "MOVESET OF %.60s TAKEN", Mm_RefName(ref));
            Mm_Note(sLine);
        } else {
            static char note[64];

            sprintf(note, "%.60s", sWhy);
            Mm_Note(note);
            Snd_PlaySe(1, 2);
        }
    }
}

/* What the keyboard typed, for these screens. */
void Mm3_Typed(s32 which, char *text) {
    switch (which) {
    case KB_EX: {
        s32 k = sMm->back == MM_SONGS;

        if (text[0] != 0) {
            Port_ExRename(k, sExCur[k], text);
            Ex_Read(k);
        }
        break;
    }
    case KB_DISC:
        Port_DiscSetName(sDiPicking, text);
        Port_DiscGet(sDiPicking, &sDiItems[sDiPicking]);
        break;
    case KB_MOVE: {
        static char cost[8];
        static char name[96];

        Port_CeMove(sMvCur, cost, name);
        Port_CeMoveSet(sMvCur, cost, text);
        break;
    }
    case KB_NUM:
        if (!Port_CeSetText(sNuTable, sNuRow, sNuCur, text)) {
            Mm_Note("THAT VALUE DOES NOT FIT THERE");
        }
        break;
    }
}

/* L1 / R1 between the tabs: Characters, Stages, Songs, Importer */
static void Tabs_Move(void) {
    Pad *pad = &gPad[0];
    s32 now;
    s32 to;

    if (!(pad->gamePressed & (PADG_L1 | PADG_R1))) {
        return;
    }
    now = sMm->mode == MM_STAGES ? 1 : sMm->mode == MM_SONGS ? 2 : sMm->mode == MM_BROWSER ? 3 : 0;
    to = (now + ((pad->gamePressed & PADG_R1) ? 1 : 3)) % 4;
    Snd_PlaySe(1, 0);
    switch (to) {
    case 0:
        Mm_SetMode(MM_LIBRARY);
        break;
    case 1:
        Mm3_OpenExtras(0);
        break;
    case 2:
        Mm3_OpenExtras(1);
        break;
    default:
        Mm2_OpenBrowser();
        break;
    }
}

void Mm3_TabsMove(void) {
    Tabs_Move();
}

void Mm3_Update(s32 live) {
    switch (sMm->mode) {
    case MM_STAGES:
        Ex_Update(live, 0);
        break;
    case MM_SONGS:
        Ex_Update(live, 1);
        break;
    case MM_DISC:
        Di_Update(live);
        break;
    case MM_NUMBERS:
        Nu_Update(live);
        break;
    case MM_MOVES:
        Mv_Update(live);
        break;
    case MM_MOVESET:
        Ms_Update(live);
        break;
    case MM_MODEL:
        Mo_Update(live);
        break;
    }
}

void Mm3_Draw(void) {
    switch (sMm->mode) {
    case MM_STAGES:
        Ex_Draw(0);
        break;
    case MM_SONGS:
        Ex_Draw(1);
        break;
    case MM_DISC:
        Di_Draw();
        break;
    case MM_NUMBERS:
        Nu_Draw();
        break;
    case MM_MOVES:
        Mv_Draw();
        break;
    case MM_MOVESET:
        Ms_Draw();
        break;
    case MM_MODEL:
        Mo_Draw();
        break;
    }
}

/* the manager is left: nothing of these screens stays; what changed is used from now on */
void Mm3_Term(void) {
    Port_MiTestEnd();
    Icons_Free();
    Di_FreePics();
    Port_DiscClose();
    Port_CeClose();
    Port_ExApply();
    sDiPhase = 0;
}
#endif
