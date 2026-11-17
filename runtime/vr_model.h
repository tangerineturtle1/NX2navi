// A model of the game's files for VR's hands (vr_model.c): an actor's mesh and its colour textures, read from the
// user's RomFS. Nothing is shipped.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VR_MODEL_PARTS = 8 };

// A part of the mesh drawn with one material: its triangles' indices (first, count) and its colour texture (RGBA8,
// sRGB-encoded, malloc'd; a 1x1 flat colour where the material has none)
typedef struct {
    uint32_t first, count;
    uint8_t *rgba;
    uint32_t tex_w, tex_h;
} VrModelPart;

// The mesh's vertices by 8 floats (position xyz in metres, normal xyz, texture uv) and its triangles' indices,
// malloc'd, its parts by material and the vertices' bounds
typedef struct {
    float *verts;
    uint32_t *index;
    uint32_t verts_n, index_n;
    VrModelPart part[VR_MODEL_PARTS];
    int parts;
    float lo[3], hi[3];
    float grip_z;   // a sword's (blade along +z): where the hand holds it, 5 cm behind the crossguard (further on a long handle)
    float head_z;   // along +z from the end at -z: where the shape first widens (a hammer's head), the far end if never
} VrModel;

// Model/<actor>.<actor>.bfres.zs's (or, for "<actor>.<model>", Model/<actor>.<model>.bfres.zs's) shapes at their finest detail and their materials' "_Alb" textures; 1 when read
int vr_model_load(const char *actor, VrModel *out);
// Link's open hand for his age, cut at the wrist from Model/Player_Adult's or Player_Child's, in the controller's grip's
// space (OpenXR's: x right, y up, z back): the palm toward the handle, the thumb's side forward; 1 when read
int vr_model_hand(int adult, int right, VrModel *out);
void vr_model_free(VrModel *m);

#ifdef __cplusplus
}
#endif
