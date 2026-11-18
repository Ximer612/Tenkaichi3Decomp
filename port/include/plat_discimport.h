/* Taking characters, stages and songs out of a modded disc image (port/src/gs/discimport.c), and the stages and songs
   folders as lists that can be switched on and off, renamed, put in order and emptied. */
#ifndef PORT_PLAT_DISCIMPORT_H
#define PORT_PLAT_DISCIMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

enum { DI_CHARAS = 161, DI_STAGES = 35, DI_SONGS = 24 };
enum { DI_CHARA, DI_STAGE, DI_SONG };

typedef struct DiItem {
    int kind;            /* DI_CHARA, DI_STAGE, DI_SONG */
    int index;           /* the disc's slot, stage or song number */
    char name[96];       /* UTF-8: a character's name; a stage or song: the original game's name when its file is
                            the original's, else "Stage n" / "Song n" (its own name is only a picture: DiNamePicture) */
    char form[96];       /* a character's label ("Super Saiyan"), may be empty */
    int changed;         /* differs from the player's own game (1), the same (0), not known (-1) */
    int formOf;          /* a character another one transforms into (left, up, right) */
    int fusion;          /* a character a fusion makes */
    int hasForms;
    unsigned char chip[5504]; /* a character's chip of the roster, a stage's icon (texture files of the game) */
    int chipLen;
} DiItem;

typedef struct DiDisc DiDisc;

/* Opens a disc and reads what it holds (slow: it compares with the game's own files). NULL with the reason. progress
   (may be NULL) goes from 0 to 1000. */
DiDisc *Di_Open(const char *iso, const char *gamedata, char *why, int whyLen, volatile int *progress);
void Di_Close(DiDisc *d);
int Di_Count(const DiDisc *d);
const DiItem *Di_Item(const DiDisc *d, int i);
/* The picture of a stage's or a song's name as the disc has it: RGBA, *w x *h; free() it. NULL if there is none. */
unsigned char *Di_NamePicture(const DiDisc *d, int i, int *w, int *h);
/* Decodes a chip / icon texture file into RGBA (free() it); NULL if it is not one. */
unsigned char *Di_Picture(const unsigned char *file, int len, int *w, int *h);
/* The names of the game's own characters, from the player's data (gamedata): names[id], "" where none. */
/* A name's picture as eight 64 x 64 textures of a chip's kind, tile by tile; the file's length, 0 none. */
int Di_NameChip(const DiDisc *d, int i, int tile, unsigned char *out, int max);
int Di_StockNames(const char *gamedata, char (*names)[64], int max);

/* The picks of an import: for every item of the disc, 0 not taken, 1 taken; a character may replace a disc character
   (replaces[i] = its id, -1 adds it). names: what a taken stage or song is called (NULL entries: its item name). */
typedef struct DiPicks {
    unsigned char take[DI_CHARAS + DI_STAGES + DI_SONGS];
    int replaces[DI_CHARAS];
    const char *names[DI_CHARAS + DI_STAGES + DI_SONGS];
    int linkForms, linkFusions; /* also take what the characters' tables lead to, and link them */
    int transformCost;          /* 0..9 written for every transformation; -1 leaves the tables' own */
} DiPicks;

/* Writes the taken characters into `characters` (each package into characters/.import, then moved in whole), the
   stages into `stages` and the songs into `songs`. Returns how many things were made; notes gets what was left out. */
int Di_Import(DiDisc *d, const DiPicks *p, const char *characters, const char *stages, const char *songs, char *notes, int notesLen,
              volatile int *progress);
/* The characters a pick of i brings along through its table (forms, fusions): marks them in take[]. */
void Di_Closure(const DiDisc *d, DiPicks *p);

/* ---- the stages and songs folders ---- */
typedef struct ExEntry {
    char file[128];
    char name[64];
    long long size;
    int on; /* 0: switched off (kept in the folder's "off" folder, where the game does not look) */
} ExEntry;
enum { EX_STAGES, EX_SONGS };
/* Every stage or song, on and off, in the game's order (the list's, then the rest by name); fills up to max. */
int Ex_List(int which, const char *dir, ExEntry *out, int max);
int Ex_SetOn(int which, const char *dir, const char *file, int on);
int Ex_Rename(int which, const char *dir, const char *file, const char *name);
int Ex_Move(int which, const char *dir, const char *file, int by); /* up (-1) or down (1) in the order */
int Ex_Remove(int which, const char *dir, const char *file);      /* into the folder's ".removed" folder */
/* Copies a stage (with its .thumb / .icon beside it) or a song in, under name; 0 if it is already there or fails. */
int Ex_Add(int which, const char *dir, const char *source, const char *name);
/* The stages and songs in a folder (an unpacked archive), outside its packages: copied in with the names a list
   beside them gives. Returns how many. */
int Ex_AddLoose(const char *root, const char *stagesDir, const char *songsDir);

#ifdef __cplusplus
}
#endif
#endif
