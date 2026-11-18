/* Changing what a character package's model files hold (port/src/gs/charedit.c): the numbers of its tables, the
   names of its moves in the skill list, and its whole moveset taken from another character. Every change goes into
   every costume's model (m_<n>p.pak, m_<n>p_dmg.pak); each file's own version is kept once as <file>.orig, which
   Ce_Restore puts back. */
#ifndef PORT_PLAT_CHAREDIT_H
#define PORT_PLAT_CHAREDIT_H

#ifdef __cplusplus
extern "C" {
#endif

enum { CE_U8, CE_S8, CE_U16, CE_S16, CE_S32, CE_F32 };

typedef struct CeCol {
    const char *name;
    int off, type, step; /* step: how far the next row's value lies (tables kept as an array per field), else 0 */
    const char *hint;
} CeCol;

typedef struct CeTable {
    const char *name;
    int section, rows, stride;
    const CeCol *cols;
    int ncols;
    const char *note;
} CeTable;

int Ce_TableCount(void);
const CeTable *Ce_Table(int t);
void Ce_RowLabel(int t, int row, char *out, int size);

typedef struct CeModel CeModel;
/* The package at dir (a folder path) read: NULL with the reason when it has no model of its own. */
CeModel *Ce_Load(const char *dir, char *why, int whyLen);
void Ce_Free(CeModel *m);
int Ce_Get(const CeModel *m, int t, int row, int col, double *v); /* 0: the model has no such value */
int Ce_Set(CeModel *m, int t, int row, int col, double v);        /* kept until Ce_Save */
int Ce_MoveCount(const CeModel *m);
/* a move of the skill list: its cost as shown ("030") and name, UTF-8 */
int Ce_Move(const CeModel *m, int i, char *cost, char *name, int nameLen);
int Ce_MoveSet(CeModel *m, int i, const char *cost, const char *name);
int Ce_Dirty(const CeModel *m);
int Ce_Save(CeModel *m, char *why, int whyLen); /* into every costume; 1 done */

/* The moveset (tables, skill list, animations and effects) of another character: a package folder, or the disc's
   character `discId` (from gamedata) when donorDir is NULL. */
int Ce_TakeMoves(const char *dir, const char *donorDir, int discId, const char *gamedata, char *why, int whyLen);
/* Same, but only the parts in `parts` (CE_PART_*) come from the donor. Tables that pair with animations (melee, ki
   blasts, blasts) can point at moves the character's own animations lack: take them with CE_PART_ANIM. */
#define CE_PART_FIGHTER 1  /* fighter table (its forms and fusions stay) */
#define CE_PART_MELEE 2    /* melee attacks */
#define CE_PART_KI 4       /* ki blasts */
#define CE_PART_MOVE 8     /* movement */
#define CE_PART_BLASTS 16  /* blast 1, blast 2, ultimate and the other tables */
#define CE_PART_TEXT 32    /* skill list, every language */
#define CE_PART_ANIM 64    /* animations and effects */
#define CE_PART_ALL 127
int Ce_TakeParts(const char *dir, const char *donorDir, int discId, const char *gamedata, int parts, char *why, int whyLen);
/* One move of another character: 0, 1 Blast 1 (A, B); 2, 3 Blast 2 (A, B); 4 Ultimate. Its numbers and animations
   (not its effects). Several in a row add up. */
int Ce_TakeMove(const char *dir, const char *donorDir, int discId, const char *gamedata, int move, int srcMove, char *why, int whyLen);
/* `move` of this character gets the donor's `srcMove` (both Blast 1, or both Blast 2 / Ultimate). */
/* The names of the five moves (skill list), 5 strings of 48 bytes; for a package folder, or (dir NULL) a disc character. */
int Ce_SlotNamesOf(const char *dir, int discId, const char *gamedata, char names[5][48]);
int Ce_HasBackup(const char *dir);
int Ce_Restore(const char *dir); /* every .orig put back */
/* A package with no model of its own (one that only changes a disc character) gets that character's files. */
int Ce_BringDisc(const char *dir, int discId, const char *gamedata);

#ifdef __cplusplus
}
#endif
#endif
