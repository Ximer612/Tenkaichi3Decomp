/* A 3D model file (FBX, OBJ, glTF / GLB) made into a fighter's model (port/src/gs/modelimport.c): the mesh is bound
   to the skeleton of the fighter model already in the package folder, and its textures become that model's. */
#ifndef PORT_PLAT_MODELIMPORT_H
#define PORT_PLAT_MODELIMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the owner can say about a model; 0 everywhere = decide it all from the file (the old behaviour). */
typedef struct {
    int up;       /* 0 auto; else the file axis that points up: 1 +Y, 2 +Z, 3 +X, 4 -Y, 5 -Z, 6 -X */
    int facing;   /* 0 auto; else the turn about the up axis that puts the front towards the camera: 1 none, 2 90, 3 180, 4 270 degrees */
    int mirror;   /* 1: left and right swapped (a file made left-handed) */
    int size;     /* 0 or 100: as measured; else percent of the measured size */
    int arms;     /* 0 auto; else 1 + the angle of the model's arms below horizontal in 5 degree steps (1 = straight out, 19 = straight down) */
    int shoulder; /* 0 auto; else the shoulder line's height in percent of the model's height */
    int waist;    /* 0 auto; else the waist (pelvis) height in percent */
    int neck;     /* 0 auto; else the neck's height in percent */
    int tex;      /* 0 smooth colours; 1 dithered */
    int rig;      /* 0 automatic joints for a file with no skeleton; 1 the old binding by distance to the fighter's rest pose */
} MiOpts;

/* On success, `why` ends with this and the names of the pictures the file names but that were not found (the
   materials then have their colour only): "...; textures not found: Image_0, Image_2". */
#define MI_MISSING "; textures not found: "

/* 1 when the file's name ends as a model file this reads */
int Mi_IsModel(const char *path);
/* dir: a package folder that holds a fighter's m_1p.pak (the skeleton and tables to keep). That file is replaced.
   fitSkeleton 0: the mesh is moved onto the fighter's skeleton; 1: the skeleton takes the mesh's proportions
   (only for a file that has a humanoid skeleton of its own). 1 done; else 0 and the reason. */
int Mi_Build(const char *file, const char *dir, int fitSkeleton, char *why, int whyLen);
/* The same with the owner's choices (o NULL: none). On success `why` says what was done and `found` (when not NULL) what
   automatic choices were made, as words: "UP +Y  FRONT +Z  ARMS 0  SHOULDER 80  WAIST 53  NECK 85". */
int Mi_BuildEx(const char *file, const char *dir, int fitSkeleton, const MiOpts *o, char *why, int whyLen, char *found, int foundLen);

#ifdef __cplusplus
}
#endif
#endif
