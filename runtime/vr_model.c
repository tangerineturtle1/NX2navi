// A model of the game's files for VR's hands (vr_model.h).
//
// BFRES (version 10): the header's +0x28 points at the models, the buffer section's offset is at the memory pool info
// the header's +0xb0 points at (+8). A model (FMDL) points at its vertex sets (+0x20) and shapes (+0x28) and counts
// them (+0x68, +0x6a). A vertex set (FVTX, 0x60 bytes) points at its attributes (+0x08: 0x10 each, the name's string,
// the format, the offset in the vertex) and strides (+0x38), keeps its data's offset in the buffer section (+0x48), its
// attribute count (+0x4c) and its vertex count (+0x50). A shape (FSHP, 0x60 bytes) names its vertex set (+0x10) and
// meshes (+0x18), its levels of detail, the finest first, and its material's index (+0x52): a mesh (0x38 bytes) has its
// indices' offset in the buffer section (+0x20), primitive (+0x24, 3 triangles), index size (+0x28, 1 16 bits, 2 32
// bits), count (+0x2c) and first vertex (+0x30). The model's materials (FMAT, 0xb0 bytes, from +0x38) name their
// textures (+0x20, strings' offsets; +0xa2 their count). Strings are a 16-bit length and the bytes. Textures are
// apart, Tex/<name>.bntx.zs, the name whole (a variant's ".00" kept).
// The skeleton (FSKL, the model's +0x18) holds its bones (+0x10, 0x58 bytes each: +0x22 the parent, +0x2c flags, +0x30
// scale, +0x3c rotation, Euler angles XYZ by the flags' 0x1000 or else a quaternion, +0x4c translation), its matrices'
// bones (+0x18, u16 each) and its bone count (+0x38). A shape whose vertices name no bone (+0x5a, 0) sits in its own
// bone's place (+0x54); one bone each (_i0, a matrix's index), in that bone's; two or more already in the model's. The
// skeleton's rest pose puts them together as the game shows the model before it animates it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "menu_assets.h"
#include "vr_model.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static float half(uint16_t h) {
    const int e = h >> 10 & 31, m = h & 1023;
    const float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 1024), e - 25);
    return h & 0x8000 ? -v : v;
}

// an attribute's first three (or two) components as floats; 0 for a format not known
static int attr(const uint8_t *p, uint32_t fmt, float out[3]) {
    switch (fmt) {
    case 0x1505:   // 16_16_16_16 float
    case 0x1205:   // 16_16 float
        for (int i = 0; i < (fmt == 0x1505 ? 3 : 2); i++) out[i] = half(le16(p + 2 * i));
        return 1;
    case 0x1805:   // 32_32_32 float
    case 0x1d05:   // 32_32_32_32 float
        memcpy(out, p, 12);
        return 1;
    case 0x1201:   // 16_16 unorm
        out[0] = le16(p) / 65535.0f, out[1] = le16(p + 2) / 65535.0f;
        return 1;
    case 0x1202:   // 16_16 snorm
        out[0] = (int16_t)le16(p) / 32767.0f, out[1] = (int16_t)le16(p + 2) / 32767.0f;
        return 1;
    case 0x0e02: {   // 10_10_10_2 snorm
        const uint32_t v = le32(p);
        for (int i = 0; i < 3; i++) {
            int c = (int)(v >> (10 * i) & 1023);
            if (c & 512) c -= 1024;
            out[i] = fmaxf(-1.0f, c / 511.0f);
        }
        return 1;
    }
    default:
        return 0;
    }
}

// the material's colour texture's name (its texture ending "_Alb", or "_Alb.NN", a variant; without one, its glow's,
// "_Emc", as a boomerang's gem has) into name; 0 when none
static int albedo_of(const uint8_t *b, size_t n, uint64_t mat, char *name, size_t cap) {
    if (mat + 0xb0 > n || memcmp(b + mat, "FMAT", 4)) return 0;
    const uint64_t list = le64(b + mat + 0x20);
    const uint8_t count = b[mat + 0xa2];
    for (int pass = 0; pass < 2; pass++)
    for (uint8_t i = 0; i < count && list + 8ull * (i + 1) <= n; i++) {
        const uint64_t so = le64(b + list + 8ull * i);
        if (so + 2 > n) continue;
        uint16_t len = le16(b + so);
        if (!len || so + 2 + len > n || len >= cap) continue;
        memcpy(name, b + so + 2, len), name[len] = 0;
        const char *dot = strchr(name, '.');
        const size_t stem = dot ? (size_t)(dot - name) : len;
        if (stem > 4 && !memcmp(name + stem - 4, pass ? "_Emc" : "_Alb", 4)) return 1;
    }
    return 0;
}

// a part's texture: its material's colour texture, else a flat colour by the material's name (the game colours such
// parts in its shaders: a wrap of cloth or leather dark blue, other bare metal a violet-grey, the rest a light grey)
static void part_texture(const uint8_t *b, size_t n, uint64_t mat, const char *actor, VrModelPart *p) {
    char tex[96];
    if (albedo_of(b, n, mat, tex, sizeof tex)) {
        char tpath[160];
        snprintf(tpath, sizeof tpath, "Tex/%s.bntx.zs", tex);
        size_t tn = 0;
        uint8_t *t = menu_asset_load(tpath, &tn);
        if (!t || !menu_asset_texture(t, tn, tex, &p->rgba, &p->tex_w, &p->tex_h))
            rt_log("vr: hands: %s's texture %s not read", actor, tpath);
        free(t);
        if (p->rgba) return;
    }
    uint8_t c[4] = {170, 170, 170, 255};
    if (mat + 0x10 <= n) {
        const uint64_t no = le64(b + mat + 8);
        const char *nm = no + 2 < n ? (const char *)b + no + 2 : "";
        const uint16_t len = no + 2 < n ? le16(b + no) : 0;
        char name[96] = "";
        if (len < sizeof name && no + 2 + len <= n) memcpy(name, nm, len), name[len] = 0;
        if (strstr(name, "Coat") || strstr(name, "Cloth") || strstr(name, "Leather")) c[0] = 48, c[1] = 52, c[2] = 110;
        else if (strstr(name, "Metal") || strstr(name, "Sword")) c[0] = 105, c[1] = 95, c[2] = 150;
    }
    p->rgba = malloc(4), p->tex_w = p->tex_h = 1;
    if (p->rgba) memcpy(p->rgba, c, 4);
}

// the bones' places in the model at rest (3x4 by rows), from the skeleton's; their count, 0 without a skeleton
enum { MAX_BONES = 256 };
static int bones_at_rest(const uint8_t *b, size_t n, uint64_t model, float world[][12]) {
    const uint64_t sk = le64(b + model + 0x18);
    if (sk + 0x40 > n || memcmp(b + sk, "FSKL", 4)) return 0;
    const uint64_t bones = le64(b + sk + 0x10);
    int count = le16(b + sk + 0x38);
    if (count > MAX_BONES) count = MAX_BONES;
    for (int i = 0; i < count; i++) {
        const uint64_t o = bones + 0x58ull * i;
        if (o + 0x58 > n) return i;
        float f[10], r[9], local[12];
        memcpy(f, b + o + 0x30, 40);
        const float *sc = f, *ro = f + 3, *tr = f + 7;
        if (le32(b + o + 0x2c) & 0x1000) {   // Euler angles, X then Y then Z: Rz * Ry * Rx
            const float cx = cosf(ro[0]), sx = sinf(ro[0]), cy = cosf(ro[1]), sy = sinf(ro[1]), cz = cosf(ro[2]), sz = sinf(ro[2]);
            const float m[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                                sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                                -sy, cy * sx, cy * cx};
            memcpy(r, m, sizeof r);
        } else {   // a quaternion x, y, z, w
            const float x = ro[0], y = ro[1], z = ro[2], w = ro[3];
            const float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
            memcpy(r, m, sizeof r);
        }
        for (int row = 0; row < 3; row++) {
            for (int c = 0; c < 3; c++) local[4 * row + c] = r[3 * row + c] * sc[c];
            local[4 * row + 3] = tr[row];
        }
        const int parent = (int16_t)le16(b + o + 0x22);
        if (parent < 0 || parent >= i) {
            memcpy(world[i], local, sizeof local);
            continue;
        }
        for (int row = 0; row < 3; row++)
            for (int c = 0; c < 4; c++)
                world[i][4 * row + c] = world[parent][4 * row] * local[c] + world[parent][4 * row + 1] * local[4 + c] +
                                        world[parent][4 * row + 2] * local[8 + c] + (c == 3 ? world[parent][4 * row + 3] : 0);
    }
    return count;
}

// the bone named `name`'s index, -1 none
static int bone_named(const uint8_t *b, size_t n, uint64_t model, const char *name) {
    const uint64_t sk = le64(b + model + 0x18);
    if (sk + 0x40 > n || memcmp(b + sk, "FSKL", 4)) return -1;
    const uint64_t bones = le64(b + sk + 0x10);
    const size_t len = strlen(name);
    for (int i = 0, count = le16(b + sk + 0x38); i < count && i < MAX_BONES; i++) {
        const uint64_t o = bones + 0x58ull * i, nm = o + 8 <= n ? le64(b + o) : n;
        if (nm + 2 + len <= n && le16(b + nm) == len && !memcmp(b + nm + 2, name, len)) return i;
    }
    return -1;
}

// Link's hand (vr_model_hand): the shapes it is cut from, its side, and from the left hand's bones (the right's mirrored
// across x) the wrist, the way the fingers point, the way across them from the little finger to the index, and the palm
typedef struct {
    const char *const *shapes;
    float side;                        // +1 the left hand (+x), -1 the right
    float wrist[3], fingers[3], across[3], palm[3], reach;
} HandCut;

void vr_model_free(VrModel *m) {
    free(m->verts), free(m->index);
    for (int i = 0; i < m->parts; i++) free(m->part[i].rgba);
    memset(m, 0, sizeof *m);
}

static int model_load(const char *actor, VrModel *out, HandCut *cut) {
    memset(out, 0, sizeof *out);
    char path[160];
    if (strchr(actor, '.')) snprintf(path, sizeof path, "Model/%s.bfres.zs", actor);   // "<actor>.<model>"
    else snprintf(path, sizeof path, "Model/%s.%s.bfres.zs", actor, actor);
    size_t n = 0;
    uint8_t *b = menu_asset_load(path, &n);
    if (!b || n < 0x100 || memcmp(b, "FRES", 4)) {
        rt_log("vr: hands: %s not read", path);
        free(b);
        return 0;
    }
#define IN(o, k) ((uint64_t)(o) + (uint64_t)(k) <= n)
    int ok = 0;
    float (*world)[12] = NULL;
    const uint64_t pool = le64(b + 0xb0), model = le64(b + 0x28);
    if (!IN(pool, 16) || !IN(model, 0x70) || memcmp(b + model, "FMDL", 4)) goto done;
    const uint64_t buffers = le64(b + pool + 8), shapes = le64(b + model + 0x28), mats = le64(b + model + 0x38);
    const uint16_t shape_n = le16(b + model + 0x6a), mat_n = le16(b + model + 0x6c);
    world = malloc(sizeof(float) * 12 * MAX_BONES);   // the bones at rest (models are read on workers of their own)
    const int bone_n = world ? bones_at_rest(b, n, model, world) : 0;
    const uint64_t sk = le64(b + model + 0x18), mat_bones = sk + 0x20 <= n ? le64(b + sk + 0x18) : 0;
    if (cut) {   // the hand's frame from the left hand's bones, mirrored for the right
        const int wr = bone_named(b, n, model, "Wrist_L"), mid = bone_named(b, n, model, "Finger_C_1_L"),
                  idx = bone_named(b, n, model, "Finger_B_1_L"), lit = bone_named(b, n, model, "Finger_E_1_L"),
                  pal = bone_named(b, n, model, "Weapon_L");
        if (wr < 0 || mid < 0 || idx < 0 || lit < 0 || pal < 0 || wr >= bone_n || mid >= bone_n || idx >= bone_n ||
            lit >= bone_n || pal >= bone_n) {
            rt_log("vr: hands: %s has no hand bones", actor);
            goto done;
        }
        float f[3], a[3], fl = 0, al = 0, d = 0;
        for (int c = 0; c < 3; c++) {
            const float m = c == 0 ? cut->side : 1.0f;
            cut->wrist[c] = m * world[wr][4 * c + 3], cut->palm[c] = m * world[pal][4 * c + 3];
            f[c] = m * (world[mid][4 * c + 3] - world[wr][4 * c + 3]), fl += f[c] * f[c];
            a[c] = m * (world[idx][4 * c + 3] - world[lit][4 * c + 3]);
        }
        fl = sqrtf(fl);
        for (int c = 0; c < 3; c++) f[c] /= fl, d += a[c] * f[c];
        for (int c = 0; c < 3; c++) a[c] -= d * f[c], al += a[c] * a[c];
        al = sqrtf(al);
        for (int c = 0; c < 3; c++) cut->fingers[c] = f[c], cut->across[c] = a[c] / al;
    }
    // two passes over the shapes: their sizes, then their data
    for (int pass = 0; pass < 2; pass++) {
        uint32_t nv = 0, ni = 0;
        for (uint16_t si = 0; si < shape_n; si++) {
            const uint64_t s = shapes + 0x60ull * si;
            if (!IN(s, 0x60) || memcmp(b + s, "FSHP", 4)) goto done;
            const uint64_t vt = le64(b + s + 0x10), mesh = le64(b + s + 0x18);
            if (!IN(vt, 0x60) || !IN(mesh, 0x38)) goto done;
            const uint32_t count = le32(b + vt + 0x50), stride = IN(le64(b + vt + 0x38), 4) ? le32(b + le64(b + vt + 0x38)) : 0;
            const uint64_t data = buffers + le32(b + vt + 0x48);
            const uint32_t prim = le32(b + mesh + 0x24), isize = le32(b + mesh + 0x28) == 2 ? 4 : 2;
            const uint32_t icount = le32(b + mesh + 0x2c), first = le32(b + mesh + 0x30);
            const uint64_t ib = buffers + le32(b + mesh + 0x20);
            if (prim != 3 || !stride || !IN(data, (uint64_t)count * stride) || !IN(ib, (uint64_t)icount * isize)) continue;
            {   // the slingshot's seeds (shown only as it shoots) left out; a hand's own shapes alone
                const uint64_t sn = le64(b + s + 8);
                if (strstr(actor, "SlingShot") && IN(sn, 10) && !memcmp(b + sn + 2, "DekuSeed", 8)) continue;
                if (cut) {
                    int keep = 0;
                    for (const char *const *k = cut->shapes; *k && !keep; k++)
                        keep = IN(sn, 2 + strlen(*k)) && !memcmp(b + sn + 2, *k, strlen(*k));
                    if (!keep) continue;
                }
            }
            if (pass == 1) {
                uint32_t pos_fmt = 0, nrm_fmt = 0, uv_fmt = 0, pos_at = 0, nrm_at = 0, uv_at = 0, idx_at = UINT32_MAX;
                const int skin = b[s + 0x5a], own_bone = le16(b + s + 0x54);
                const uint64_t attrs = le64(b + vt + 0x08);
                for (uint8_t a = 0; a < b[vt + 0x4c]; a++) {
                    const uint64_t at = attrs + 0x10ull * a;
                    if (!IN(at, 0x10)) break;
                    const uint64_t no = le64(b + at);
                    if (!IN(no, 5)) continue;
                    const char *nm = (const char *)b + no + 2;
                    const uint32_t f = le32(b + at + 8), off = le16(b + at + 12);
                    if (!memcmp(nm, "_p0", 3)) pos_fmt = f, pos_at = off;
                    else if (!memcmp(nm, "_n0", 3)) nrm_fmt = f, nrm_at = off;
                    else if (!memcmp(nm, "_u0", 3)) uv_fmt = f, uv_at = off;
                    else if (!memcmp(nm, "_i0", 3)) idx_at = off;
                }
                for (uint32_t v = 0; v < count; v++) {
                    const uint8_t *src = b + data + (uint64_t)v * stride;
                    float *d = out->verts + 8ull * (nv + v), p[3] = {0}, q[3] = {0, 1, 0}, u[3] = {0};
                    if (!attr(src + pos_at, pos_fmt, p)) {
                        rt_log("vr: hands: %s has positions of format %#x", path, pos_fmt);
                        goto done;
                    }
                    if (nrm_fmt) attr(src + nrm_at, nrm_fmt, q);
                    if (uv_fmt) attr(src + uv_at, uv_fmt, u);
                    int bone = -1;   // the bone it sits in (none: already in the model's place)
                    if (skin == 0) bone = own_bone;
                    else if (skin == 1 && idx_at != UINT32_MAX) {
                        const uint64_t mb = mat_bones + 2ull * src[idx_at];
                        if (mat_bones && mb + 2 <= n) bone = le16(b + mb);
                    }
                    if (bone >= 0 && bone < bone_n) {
                        const float *w = world[bone];
                        float pp[3], qq[3];
                        for (int r = 0; r < 3; r++) {
                            pp[r] = w[4 * r] * p[0] + w[4 * r + 1] * p[1] + w[4 * r + 2] * p[2] + w[4 * r + 3];
                            qq[r] = w[4 * r] * q[0] + w[4 * r + 1] * q[1] + w[4 * r + 2] * q[2];
                        }
                        memcpy(p, pp, sizeof pp), memcpy(q, qq, sizeof qq);
                    }
                    d[0] = p[0], d[1] = p[1], d[2] = p[2], d[3] = q[0], d[4] = q[1], d[5] = q[2], d[6] = u[0], d[7] = u[1];
                }
                uint32_t kept = 0;
                for (uint32_t i = 0; i + 2 < icount; i += 3) {
                    uint32_t t[3];
                    int in = 1;
                    for (int j = 0; j < 3; j++) {
                        const uint32_t k = isize == 4 ? le32(b + ib + 4ull * (i + j)) : le16(b + ib + 2ull * (i + j));
                        t[j] = first + k < count ? nv + first + k : nv;
                        if (cut) {   // a hand: on its side, past the wrist across the arm and near it
                            const float *v = out->verts + 8ull * t[j];
                            float along = 0, far = 0;
                            for (int c = 0; c < 3; c++) {
                                const float e = v[c] - cut->wrist[c];
                                along += e * cut->fingers[c], far += e * e;
                            }
                            in = in && v[0] * cut->side > 0 && along > 0 && far < cut->reach * cut->reach;
                        }
                    }
                    if (!in) continue;
                    memcpy(out->index + ni + kept, t, sizeof t), kept += 3;
                }
                if (kept && out->parts < VR_MODEL_PARTS) {
                    VrModelPart *p = &out->part[out->parts++];
                    const uint16_t mi = le16(b + s + 0x52);
                    p->first = ni, p->count = kept;
                    part_texture(b, n, mi < mat_n ? mats + 0xb0ull * mi : n, actor, p);
                }
                nv += count, ni += kept;
                continue;
            }
            nv += count, ni += icount - icount % 3;
        }
        if (pass == 0) {
            if (!nv || !ni) goto done;
            out->verts = malloc(sizeof(float) * 8 * nv), out->index = malloc(sizeof(uint32_t) * ni);
            if (!out->verts || !out->index) goto done;
        } else {
            out->verts_n = nv, out->index_n = ni;
            for (int c = 0; c < 3; c++) out->lo[c] = 1e9f, out->hi[c] = -1e9f;
            for (uint32_t v = 0; v < nv; v++)
                for (int c = 0; c < 3; c++)
                    out->lo[c] = fminf(out->lo[c], out->verts[8 * v + c]), out->hi[c] = fmaxf(out->hi[c], out->verts[8 * v + c]);
            // the crossguard: along z in 64 slices, from the pommel, the first slice past the grip's own that is more
            // than 2.5 times as wide (across x) as the grip's widest (slices 4 to 8)
            enum { SLICES = 64 };
            float w[SLICES] = {0};
            const float span = out->hi[2] - out->lo[2];
            out->grip_z = out->lo[2] + 0.11f;
            if (span > 0.05f) {
                for (uint32_t v = 0; v < nv; v++) {
                    int i = (int)((out->verts[8 * v + 2] - out->lo[2]) / span * SLICES);
                    i = i < 0 ? 0 : i >= SLICES ? SLICES - 1 : i;
                    w[i] = fmaxf(w[i], fabsf(out->verts[8 * v]));
                }
                float grip = 0;
                for (int i = 4; i <= 8; i++) grip = fmaxf(grip, w[i]);
                out->head_z = out->hi[2];
                for (int i = 9; i < SLICES; i++)
                    if (grip > 0 && w[i] > 2.5f * grip) {
                        out->head_z = out->lo[2] + span * i / SLICES;
                        break;
                    }
                for (int i = 9; i < SLICES / 2; i++)
                    if (grip > 0 && w[i] > 2.5f * grip) {
                        const float guard = out->lo[2] + span * i / SLICES, handle = guard - out->lo[2];
                        out->grip_z = guard - 0.05f;
                        // a long handle (a sword for both hands): halfway on toward the handle's middle
                        if (handle > 0.30f) out->grip_z = 0.5f * (out->grip_z + (out->lo[2] + 0.5f * handle));
                        break;
                    }
            }
        }
    }
    ok = out->parts > 0;
    for (int i = 0; i < out->parts; i++) ok = ok && out->part[i].rgba;
    if (ok) rt_log("vr: hands: %s: %u vertices, %u triangles, %d parts", actor, out->verts_n, out->index_n / 3, out->parts);
done:
    if (ok && cut) {   // a hand: its vertices put in the grip's space (x right, y up, z back, metres)
        // the grip's: the palm toward the handle (the left's to +x, the right's to -x), the index's side forward (-z),
        // the fingers down and tilted 45 degrees forward; the palm's middle 2 cm from the handle's
        const float t = 45.0f * 3.14159265f / 180.0f;
        const float gf[3] = {0, -cosf(t), -sinf(t)}, ga[3] = {0, sinf(t), -cosf(t)};
        const float gc[3] = {gf[1] * ga[2] - gf[2] * ga[1], gf[2] * ga[0] - gf[0] * ga[2], gf[0] * ga[1] - gf[1] * ga[0]};
        const float *f = cut->fingers, *a = cut->across;
        const float c3[3] = {f[1] * a[2] - f[2] * a[1], f[2] * a[0] - f[0] * a[2], f[0] * a[1] - f[1] * a[0]};
        for (uint32_t v = 0; v < out->verts_n; v++) {
            float *p = out->verts + 8ull * v, e[3], q[3];
            for (int c = 0; c < 3; c++) e[c] = p[c] - cut->palm[c], q[c] = p[3 + c];
            const float u[3] = {e[0] * f[0] + e[1] * f[1] + e[2] * f[2], e[0] * a[0] + e[1] * a[1] + e[2] * a[2],
                                e[0] * c3[0] + e[1] * c3[1] + e[2] * c3[2]};
            const float w[3] = {q[0] * f[0] + q[1] * f[1] + q[2] * f[2], q[0] * a[0] + q[1] * a[1] + q[2] * a[2],
                                q[0] * c3[0] + q[1] * c3[1] + q[2] * c3[2]};
            for (int c = 0; c < 3; c++) {
                p[c] = u[0] * gf[c] + u[1] * ga[c] + u[2] * gc[c] - (c == 0 ? cut->side * 0.02f : 0.0f);
                p[3 + c] = w[0] * gf[c] + w[1] * ga[c] + w[2] * gc[c];
            }
        }
    }
#undef IN
    free(world);
    free(b);
    if (!ok) vr_model_free(out);
    return ok;
}

int vr_model_load(const char *actor, VrModel *out) { return model_load(actor, out, NULL); }

int vr_model_hand(int adult, int right, VrModel *out) {
    static const char *const adult_shapes[] = {"Hand_low", "Glove_Arm_B_low", "Glove_Arm_Metal_A_low", NULL};
    static const char *const child_shapes[] = {"Skin_Body", NULL};
    HandCut cut = {adult ? adult_shapes : child_shapes, right ? -1.0f : 1.0f, {0}, {0}, {0}, {0}, adult ? 0.22f : 0.16f};
    return model_load(adult ? "Player_Adult" : "Player_Child", out, &cut);
}
