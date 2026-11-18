/* What the Mod Manager's Stages, Songs, Importer and character screens (src/menu/port_modmgr3.c) ask of the host: the game's
 * code has 4-byte pointers, so everything here goes through its own buffers and plain numbers. Behind these are
 * discimport.c (a disc image; the stages and songs folders) and charedit.c (a package's model files). */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/plat_charas.h"
#include "../../include/plat_charedit.h"
#include "../../include/plat_discimport.h"

typedef int s32;

extern const char *Port_ExtrasDir(int songs); /* modmgr.c */
extern int Port_ModDirOf(int i, char *out, int size);
extern const char *Port_FileRoot(void);       /* plat_file.c */
extern void PortStages_Reload(void);          /* plat_stages.c */
extern void PortSongs_Reload(void);           /* plat_songs.c */

/* The main menu's line for the Mod Manager (the disc has none: the item is the PC's), in the menus' language. */
extern const char *MenuTranslation_Locale(void);
void Port_ModManagerLine(char *out, s32 size) {
    static const struct {
        const char *locale, *line;
    } kLines[] = {
        {"it", "Aggiungi e gestisci personaggi, scenari e musiche: importa i mod, attivali o modificali."},
        {"es", "A\xC3\xB1" "ade y gestiona personajes, escenarios y m\xC3\xBAsica: importa mods, act\xC3\xADvalos o ed\xC3\xADtalos."},
        {"fr", "G\xC3\xA9rez personnages, d\xC3\xA9" "cors et musiques : importez des mods, activez-les ou modifiez-les."},
        {"de", "Charaktere, Arenen und Musik verwalten: Mods importieren, ein- und ausschalten, bearbeiten."},
    };
    const char *locale = MenuTranslation_Locale();
    size_t i;
    for (i = 0; locale != NULL && i < sizeof(kLines) / sizeof(kLines[0]); i++) {
        if (strncmp(locale, kLines[i].locale, 2) == 0) {
            snprintf(out, (size_t)size, "%s", kLines[i].line);
            return;
        }
    }
    snprintf(out, (size_t)size, "%s", "Add and manage characters, stages and songs: import mods, switch them on or off, edit them.");
}

/* ---- stages and songs ---- */
typedef struct PortExInfo {
    char file[128];
    char name[64];
    s32 tenthMb;
    s32 on;
} PortExInfo;

static ExEntry sEx[2][256];
static int sExN[2];
static int sExDirty;

s32 Port_ExList(s32 which) {
    sExN[which != 0] = Ex_List(which != 0 ? EX_SONGS : EX_STAGES, Port_ExtrasDir(which != 0), sEx[which != 0], 256);
    return sExN[which != 0];
}

void Port_ExGet(s32 which, s32 i, PortExInfo *out) {
    const ExEntry *e = &sEx[which != 0][i];
    snprintf(out->file, sizeof(out->file), "%s", e->file);
    snprintf(out->name, sizeof(out->name), "%s", e->name);
    out->tenthMb = (s32)(e->size * 10 / 1048576);
    out->on = e->on;
}

s32 Port_ExLimit(s32 which) { return which != 0 ? 40 : 26; }

s32 Port_ExToggle(s32 which, s32 i) {
    int k = which != 0, on = 0, j, ok;
    for (j = 0; j < sExN[k]; j++) {
        on += sEx[k][j].on;
    }
    if (!sEx[k][i].on && on >= Port_ExLimit(which)) {
        return 0;
    }
    ok = Ex_SetOn(k ? EX_SONGS : EX_STAGES, Port_ExtrasDir(k), sEx[k][i].file, !sEx[k][i].on);
    sExDirty |= ok;
    return ok;
}

s32 Port_ExRename(s32 which, s32 i, char *name) {
    int ok = Ex_Rename(which != 0 ? EX_SONGS : EX_STAGES, Port_ExtrasDir(which != 0), sEx[which != 0][i].file, name);
    sExDirty |= ok;
    return ok;
}

s32 Port_ExOrder(s32 which, s32 i, s32 by) {
    int ok = Ex_Move(which != 0 ? EX_SONGS : EX_STAGES, Port_ExtrasDir(which != 0), sEx[which != 0][i].file, by);
    sExDirty |= ok;
    return ok;
}

s32 Port_ExDrop(s32 which, s32 i) {
    int ok = Ex_Remove(which != 0 ? EX_SONGS : EX_STAGES, Port_ExtrasDir(which != 0), sEx[which != 0][i].file);
    sExDirty |= ok;
    return ok;
}

/* a stage's icon (its .icon beside it) into out; its length, 0 none */
s32 Port_ExIcon(s32 i, void *out, s32 max) {
    const ExEntry *e = &sEx[0][i];
    const char *dot = strrchr(e->file, '.');
    char path[700];
    FILE *f;
    long n;
    snprintf(path, sizeof(path), "%s%s/%.*s.icon", Port_ExtrasDir(0), e->on ? "" : "/off", (int)(dot != NULL ? dot - e->file : (long)strlen(e->file)), e->file);
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    n = (long)fread(out, 1, (size_t)max, f);
    fclose(f);
    return (s32)n;
}

void Port_ExTouched(void) {
    sExDirty = 1;
}

/* what was changed is used from now on (when the manager is left, and the menus read their lists afresh) */
void Port_ExApply(void) {
    if (sExDirty) {
        sExDirty = 0;
        PortStages_Reload();
        PortSongs_Reload();
    }
}

/* ---- a disc image ---- */
typedef struct PortDiscInfo {
    s32 kind, index, changed, formOf, fusion, hasForms, chipLen;
    char name[96], form[96];
} PortDiscInfo;

enum { DS_NONE, DS_READING, DS_OPEN, DS_FAILED, DS_WRITING, DS_WRITTEN };
static SDL_AtomicInt sDs;
static volatile int sDsProg;
static SDL_Thread *sDsThread;
static DiDisc *sDisc;
static DiPicks sPicks;
static char sDsPath[700], sDsWhy[300], sDsNotes[4096], sDsNames[DI_CHARAS + DI_STAGES + DI_SONGS][64];
static int sDsMade;

static void ds_join(void) {
    if (sDsThread != NULL) {
        SDL_WaitThread(sDsThread, NULL);
        sDsThread = NULL;
    }
}

static int ds_read(void *unused) {
    (void)unused;
    sDisc = Di_Open(sDsPath, Port_FileRoot(), sDsWhy, sizeof(sDsWhy), &sDsProg);
    SDL_SetAtomicInt(&sDs, sDisc != NULL ? DS_OPEN : DS_FAILED);
    return 0;
}

static int ds_write(void *unused) {
    char chars[300];
    int i;
    (void)unused;
    if (!PortCharas_Root(chars, sizeof(chars))) {
        snprintf(chars, sizeof(chars), "characters");
        SDL_CreateDirectory(chars);
    }
    for (i = 0; i < DI_CHARAS + DI_STAGES + DI_SONGS; i++) {
        sPicks.names[i] = sDsNames[i][0] != '\0' ? sDsNames[i] : NULL;
    }
    sDsMade = Di_Import(sDisc, &sPicks, chars, Port_ExtrasDir(0), Port_ExtrasDir(1), sDsNotes, sizeof(sDsNotes), &sDsProg);
    sExDirty = 1;
    SDL_SetAtomicInt(&sDs, DS_WRITTEN);
    return 0;
}

void Port_DiscClose(void) {
    ds_join();
    Di_Close(sDisc);
    sDisc = NULL;
    SDL_SetAtomicInt(&sDs, DS_NONE);
}

s32 Port_DiscOpen(char *path) {
    int i;
    Port_DiscClose();
    snprintf(sDsPath, sizeof(sDsPath), "%s", path);
    memset(&sPicks, 0, sizeof(sPicks));
    memset(sDsNames, 0, sizeof(sDsNames));
    for (i = 0; i < DI_CHARAS; i++) {
        sPicks.replaces[i] = -1;
    }
    sPicks.linkForms = sPicks.linkFusions = 1;
    sPicks.transformCost = 1;
    sDsProg = 0;
    sDsWhy[0] = '\0';
    SDL_SetAtomicInt(&sDs, DS_READING);
    sDsThread = SDL_CreateThread(ds_read, "bt3 disc", NULL);
    if (sDsThread == NULL) {
        ds_read(NULL);
    }
    return 1;
}

/* 0 no disc, 1 reading, 2 open, 3 it failed (why: 200 bytes), 4 writing, 5 written (once; then open again; why: the notes) */
s32 Port_DiscPoll(s32 *progress, char *why) {
    int s = SDL_GetAtomicInt(&sDs);
    if (progress != NULL) {
        *progress = sDsProg;
    }
    if (s == DS_FAILED) {
        ds_join();
        snprintf(why, 200, "%s", sDsWhy);
        SDL_SetAtomicInt(&sDs, DS_NONE);
    } else if (s == DS_WRITTEN) {
        ds_join();
        snprintf(why, 200, "%s", sDsNotes);
        SDL_SetAtomicInt(&sDs, DS_OPEN);
    } else if (s == DS_OPEN) {
        ds_join();
    }
    return s;
}

s32 Port_DiscMade(void) { return sDsMade; }
s32 Port_DiscCount(void) { return Di_Count(sDisc); }

void Port_DiscGet(s32 i, PortDiscInfo *out) {
    const DiItem *it = Di_Item(sDisc, i);
    memset(out, 0, sizeof(*out));
    if (it != NULL) {
        out->kind = it->kind;
        out->index = it->index;
        out->changed = it->changed;
        out->formOf = it->formOf;
        out->fusion = it->fusion;
        out->hasForms = it->hasForms;
        out->chipLen = it->chipLen;
        snprintf(out->name, sizeof(out->name), "%s", it->kind != DI_CHARA && sDsNames[i][0] != '\0' ? sDsNames[i] : it->name);
        snprintf(out->form, sizeof(out->form), "%s", it->form);
    }
}

s32 Port_DiscChip(s32 i, void *out, s32 max) {
    const DiItem *it = Di_Item(sDisc, i);
    if (it == NULL || it->chipLen == 0 || it->chipLen > max) {
        return 0;
    }
    memcpy(out, it->chip, (size_t)it->chipLen);
    return it->chipLen;
}

s32 Port_DiscNameChip(s32 i, s32 tile, void *out, s32 max) { return Di_NameChip(sDisc, i, tile, out, max); }
s32 Port_DiscTaken(s32 i) { return i >= 0 && i < DI_CHARAS + DI_STAGES + DI_SONGS ? sPicks.take[i] : 0; }

void Port_DiscTake(s32 i, s32 on) {
    if (i >= 0 && i < DI_CHARAS + DI_STAGES + DI_SONGS) {
        sPicks.take[i] = on != 0;
        if (on) {
            Di_Closure(sDisc, &sPicks);
        }
    }
}

void Port_DiscSetName(s32 i, char *name) { snprintf(sDsNames[i], sizeof(sDsNames[i]), "%s", name); }

/* a character taken: -1 added as new, else the game's character it replaces */
s32 Port_DiscReplaces(s32 slot) { return slot >= 0 && slot < DI_CHARAS ? sPicks.replaces[slot] : -1; }
void Port_DiscSetReplaces(s32 slot, s32 id) {
    if (slot >= 0 && slot < DI_CHARAS) {
        sPicks.replaces[slot] = id;
    }
}
/* what is brought along, what transformations cost (-1 the disc's own) */
void Port_DiscOptions(s32 links, s32 cost) {
    sPicks.linkForms = sPicks.linkFusions = links;
    sPicks.transformCost = cost;
}

void Port_DiscImport(void) {
    sDsProg = 0;
    sDsNotes[0] = '\0';
    SDL_SetAtomicInt(&sDs, DS_WRITING);
    sDsThread = SDL_CreateThread(ds_write, "bt3 disc", NULL);
    if (sDsThread == NULL) {
        ds_write(NULL);
    }
}

/* ---- a package's model files ---- */
static CeModel *sCe;
static char sCeDir[600];

/* mod i's model read; 0 with the reason (why: 200 bytes). A package with no model of its own gets its fighter's files. */
s32 Port_CeOpen(s32 mod, s32 chara, char *why) {
    char reason[300] = "";
    Ce_Free(sCe);
    sCe = NULL;
    if (!Port_ModDirOf(mod, sCeDir, sizeof(sCeDir))) {
        snprintf(why, 200, "NO FOLDER");
        return 0;
    }
    sCe = Ce_Load(sCeDir, reason, sizeof(reason));
    if (sCe == NULL && chara >= 0 && chara < DI_CHARAS && Ce_BringDisc(sCeDir, chara, Port_FileRoot()) > 0) {
        sCe = Ce_Load(sCeDir, reason, sizeof(reason));
    }
    if (sCe == NULL) {
        snprintf(why, 200, "%s", reason);
    }
    return sCe != NULL;
}

void Port_CeClose(void) {
    Ce_Free(sCe);
    sCe = NULL;
}

s32 Port_CeTables(void) { return Ce_TableCount(); }

void Port_CeTable(s32 t, char *name, s32 *rows, s32 *cols, char *note) {
    const CeTable *tb = Ce_Table(t);
    snprintf(name, 48, "%s", tb->name);
    snprintf(note, 160, "%s", tb->note);
    *rows = tb->rows;
    *cols = tb->ncols;
}

void Port_CeRow(s32 t, s32 row, char *out) { Ce_RowLabel(t, row, out, 48); }

void Port_CeCol(s32 t, s32 c, char *name, char *hint) {
    const CeCol *col = &Ce_Table(t)->cols[c];
    snprintf(name, 48, "%s", col->name);
    snprintf(hint, 48, "%s", col->hint);
}

s32 Port_CeValue(s32 t, s32 row, s32 c, char *out) {
    double v;
    if (sCe == NULL || !Ce_Get(sCe, t, row, c, &v)) {
        snprintf(out, 32, "-");
        return 0;
    }
    if (Ce_Table(t)->cols[c].type == CE_F32) {
        snprintf(out, 32, "%.3f", v);
    } else {
        snprintf(out, 32, "%d", (int)v);
    }
    return 1;
}

s32 Port_CeStep(s32 t, s32 row, s32 c, s32 dir, s32 big) {
    double v, step;
    if (sCe == NULL || !Ce_Get(sCe, t, row, c, &v)) {
        return 0;
    }
    if (Ce_Table(t)->cols[c].type == CE_F32) {
        step = big ? 1.0 : 0.1;
    } else {
        step = big ? floor(fabs(v) / 10.0) : 1.0;
        step = step < 1.0 ? 1.0 : step;
    }
    return Ce_Set(sCe, t, row, c, v + step * dir);
}

s32 Port_CeSetText(s32 t, s32 row, s32 c, char *text) {
    char *end;
    double v = strtod(text, &end);
    return sCe != NULL && end != text && Ce_Set(sCe, t, row, c, v);
}

s32 Port_CeMoves(void) { return Ce_MoveCount(sCe); }
s32 Port_CeMove(s32 i, char *cost, char *name) { return Ce_Move(sCe, i, cost, name, 96); }
s32 Port_CeMoveSet(s32 i, char *cost, char *name) { return Ce_MoveSet(sCe, i, cost, name); }
s32 Port_CeDirty(void) { return Ce_Dirty(sCe); }

s32 Port_CeSave(char *why) {
    char reason[300] = "";
    int ok = sCe != NULL && Ce_Save(sCe, reason, sizeof(reason));
    if (!ok) {
        snprintf(why, 200, "%s", reason);
    }
    return ok;
}

/* the moveset of the game's character `chara`, or (chara < 0) of mod `from` */
s32 Port_CeTakeParts(s32 mod, s32 chara, s32 from, s32 parts, char *why);
s32 Port_CeTake(s32 mod, s32 chara, s32 from, char *why) {
    return Port_CeTakeParts(mod, chara, from, CE_PART_ALL, why);
}

/* the same, only the CE_PART_* in `parts` (customize moveset) */
s32 Port_CeTakeParts(s32 mod, s32 chara, s32 from, s32 parts, char *why) {
    char dir[600], donor[600], reason[300] = "";
    int ok;
    Port_CeClose();
    if (!Port_ModDirOf(mod, dir, sizeof(dir)) || (chara < 0 && !Port_ModDirOf(from, donor, sizeof(donor)))) {
        snprintf(why, 200, "NO FOLDER");
        return 0;
    }
    ok = Ce_TakeParts(dir, chara < 0 ? donor : NULL, chara, Port_FileRoot(), parts, reason, sizeof(reason));
    if (!ok) {
        snprintf(why, 200, "%s", reason);
    }
    return ok;
}

/* one move (0..4, see Ce_TakeMove) of the game's character `chara`, or (chara < 0) of mod `from` */
s32 Port_CeTakeMove(s32 mod, s32 chara, s32 from, s32 move, s32 srcMove, char *why) {
    char dir[600], donor[600], reason[300] = "";
    int ok;
    Port_CeClose();
    if (!Port_ModDirOf(mod, dir, sizeof(dir)) || (chara < 0 && !Port_ModDirOf(from, donor, sizeof(donor)))) {
        snprintf(why, 200, "NO FOLDER");
        return 0;
    }
    ok = Ce_TakeMove(dir, chara < 0 ? donor : NULL, chara, Port_FileRoot(), move, srcMove, reason, sizeof(reason));
    if (!ok) {
        snprintf(why, 200, "%s", reason);
    }
    return ok;
}

/* the five moves' names of mod `mod` (chara < 0) or of the game's character `chara`: 5 strings of 48 bytes; 1 when found */
s32 Port_CeSlotNames(s32 mod, s32 chara, char *out) {
    char dir[600];
    char names[5][48];
    int k, ok;
    if (chara < 0 && !Port_ModDirOf(mod, dir, sizeof(dir))) {
        memset(out, 0, 5 * 48);
        return 0;
    }
    ok = Ce_SlotNamesOf(chara < 0 ? dir : NULL, chara, Port_FileRoot(), names);
    for (k = 0; k < 5; k++) {
        memcpy(out + k * 48, names[k], 48);
    }
    return ok;
}

s32 Port_CeHasBackup(s32 mod) {
    char dir[600];
    return Port_ModDirOf(mod, dir, sizeof(dir)) && Ce_HasBackup(dir);
}

s32 Port_CeRestore(s32 mod) {
    char dir[600];
    Port_CeClose();
    return Port_ModDirOf(mod, dir, sizeof(dir)) ? Ce_Restore(dir) : 0;
}

/* BT3_GS_VERBOSE: how long the Mod Manager's opening takes, step by step */
void Port_MmStep(char *what) {
    static uint64_t was;
    uint64_t now = SDL_GetTicksNS();
    if (getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "mm: %s %.1f ms\n", what, (double)(now - was) / 1e6);
    }
    was = now;
}
