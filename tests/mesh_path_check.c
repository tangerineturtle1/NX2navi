// runtime/vk_mesh_path.h: how the game's task and mesh programs are drawn (vk_device.c create_device): natively, through
// the mesh fallback, the mesh emulation or not at all, for each device kind and setting. Built and run by
// tests/mesh_path_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "vk_mesh_path.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const names[] = {"native", "fallback", "emulation", "none"};

static void expect_nv(int has_mesh, int amd_sc, int nvidia, const char *mf, const char *me, int want, const char *what) {
    const char *why = NULL;
    int got = mesh_path_decide(has_mesh, amd_sc, nvidia, mf, me, &why);
    CHECK(got == want, "%s (mesh shaders %d, AMD RDNA 1/2 driver %d, mesh_fallback %s, mesh_emulation %s): %s, not %s",
          what, has_mesh, amd_sc, mf, me, names[got], names[want]);
    CHECK(why && *why, "%s: no reason for the boot line", what);
}

static void expect(int has_mesh, int amd_sc, const char *mf, const char *me, int want, const char *what) {
    expect_nv(has_mesh, amd_sc, 0, mf, me, want, what);
}

int main(void) {
    // ---- the default (auto, emulation off) by device
    expect(1, 0, "auto", "0", MESH_PATH_NATIVE, "NVIDIA, Intel, Mesa or AMD's RDNA 3/4 driver with mesh shaders");
    expect(1, 1, "auto", "0", MESH_PATH_FALLBACK, "an RX 6000 card on AMD's RDNA 1/2 driver");
    expect(0, 1, "auto", "0", MESH_PATH_FALLBACK, "an RX 5000 card on AMD's RDNA 1/2 driver (no mesh shaders)");
    expect(0, 0, "auto", "0", MESH_PATH_FALLBACK, "a GTX 10 card (no mesh shaders)");
    // ---- mesh_fallback = 0 gives an RX 6000 card native mesh pipelines back, as 0.10.1 drew them
    expect(1, 1, "0", "0", MESH_PATH_NATIVE, "an RX 6000 card with mesh_fallback = 0");
    expect(1, 0, "0", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = 0");
    expect(0, 0, "0", "0", MESH_PATH_NONE, "a card without mesh shaders and mesh_fallback = 0");
    // ---- mesh_fallback = 1 on every device
    expect(1, 0, "1", "0", MESH_PATH_FALLBACK, "an NVIDIA card with mesh_fallback = 1");
    expect(1, 1, "1", "0", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_fallback = 1");
    // ---- the mesh emulation acts only without mesh shaders, and then before the fallback
    expect(0, 0, "auto", "1", MESH_PATH_EMULATION, "no mesh shaders, mesh_emulation = 1");
    expect(0, 1, "1", "1", MESH_PATH_EMULATION, "no mesh shaders, both set");
    expect(1, 1, "auto", "1", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_emulation = 1");
    expect(1, 0, "auto", "1", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_emulation = 1");
    // ---- an NVIDIA card never takes the emulation, which ended the game inside its driver (GTX 10 cards in the
    // low-end GPU mode of 0.10.1, which set mesh_emulation = 1)
    expect_nv(0, 0, 1, "auto", "1", MESH_PATH_FALLBACK, "a GTX 10 card with mesh_emulation = 1");
    expect_nv(0, 0, 1, "1", "1", MESH_PATH_FALLBACK, "a GTX 10 card with both set");
    expect_nv(0, 0, 1, "0", "1", MESH_PATH_NONE, "a GTX 10 card with mesh_emulation = 1 and mesh_fallback = 0");
    expect_nv(0, 0, 1, "auto", "0", MESH_PATH_FALLBACK, "a GTX 10 card by default");
    expect_nv(1, 0, 1, "auto", "1", MESH_PATH_NATIVE, "an RTX card with mesh_emulation = 1");
    expect_nv(1, 0, 1, "auto", "0", MESH_PATH_NATIVE, "an RTX card by default");
    // ---- a value the settings table does not list reads as auto
    expect(1, 1, "", "0", MESH_PATH_FALLBACK, "an RX 6000 card with an empty mesh_fallback");
    expect(1, 0, "yes", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = yes");
    // ---- the reason names the driver default, and how to turn it off
    const char *why = NULL;
    mesh_path_decide(1, 1, 0, "auto", "0", &why);
    CHECK(why && strstr(why, "mesh_fallback = 0"), "the RX 6000 default's reason does not say how to turn it off: %s", why ? why : "(none)");

    mesh_path_decide(0, 0, 1, "auto", "1", &why);
    CHECK(why && strstr(why, "mesh_emulation"), "a GTX 10 card with mesh_emulation = 1: the reason does not name it: %s",
          why ? why : "(none)");

    printf("mesh_path_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
