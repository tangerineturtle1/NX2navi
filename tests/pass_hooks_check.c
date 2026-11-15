// runtime/vk_pass_hooks.h: which pass begin runs the frame's pass hooks (vk_exec.c vk_exec_pass_begin). A parallel
// group's hooks run once, at its first chunk's first pass: a pass that chunk begins again later must not put its own
// target back after the hooks of later passes ran. Built and run by tests/pass_hooks_check.py; prints each failed check
// and exits 1 on any.
#include <stdio.h>

#include "vk_pass_hooks.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The hook as vk_dlaa_pass keeps the light target: the target of the last pass that ran it.
static int light;
// A group as vk_exec.c keeps it: whether its first chunk began a pass.
typedef struct { int hooks; } Grp;

// A pass begin of a thread that records no chunk (g NULL), or chunk idx of group g, into `target`.
static void pass_begin(Grp *g, int idx, int target) {
    if (pass_hooks_due(g != NULL, idx, g ? g->hooks : 0)) light = target;
    if (g && !idx) g->hooks = 1;
}

int main(void) {
    enum { EARLY = 1, SCENE = 2, OUTPUT = 3 };
    // ---- the rule itself
    CHECK(pass_hooks_due(0, 0, 0), "a thread outside a group does not run the hooks");
    CHECK(pass_hooks_due(0, 0, 1) && pass_hooks_due(0, 5, 1), "outside a group the other two values count");
    CHECK(pass_hooks_due(1, 0, 0), "a group's first chunk does not run the hooks at its first pass");
    CHECK(!pass_hooks_due(1, 0, 1), "a group's first chunk runs the hooks again at a pass it begins again");
    for (int idx = 1; idx < 17; idx++)
        CHECK(!pass_hooks_due(1, idx, 0) && !pass_hooks_due(1, idx, 1), "chunk %d of a group runs the hooks", idx);

    // ---- one thread: a pass begun twice changes nothing, the last pass of the stream keeps the light target
    light = 0;
    pass_begin(NULL, 0, EARLY);
    pass_begin(NULL, 0, EARLY);   // after a barrier
    pass_begin(NULL, 0, SCENE);
    pass_begin(NULL, 0, SCENE);
    CHECK(light == SCENE, "one thread: the light target is %d, not the last scene pass's", light);

    // ---- a group whose chunks the workers record while the submitting thread waits for it: its passes begin in any
    // order after the first chunk's, and the pass after the group comes after all of them
    Grp g = {0};
    light = 0;
    pass_begin(&g, 0, EARLY);
    pass_begin(&g, 2, EARLY);
    pass_begin(&g, 1, EARLY);
    pass_begin(&g, 0, EARLY);     // the first chunk's pass again, after a texture lookup waited for its turn
    pass_begin(NULL, 0, SCENE);
    CHECK(light == SCENE, "a group waited for: the light target is %d", light);

    // ---- the submitting thread goes on past the group: the scene pass after it begins once the group's first chunk
    // began its pass, and that chunk begins its pass again after that. The scene pass is later in the stream and keeps
    // the light target; the hooks running at the second begin gave the upscaler the earlier pass's image for a frame.
    g = (Grp){0};
    light = 0;
    pass_begin(&g, 0, EARLY);
    pass_begin(NULL, 0, SCENE);   // vk_exec_hooks_wait let it through: the group's hooks ran
    pass_begin(&g, 0, EARLY);     // late: after a lookup's turn, a barrier, a pass of the replay's own
    pass_begin(&g, 1, EARLY);
    CHECK(light == SCENE, "the submitting thread past a group: the light target is %d, the earlier pass's", light);

    // ---- two groups out and the pass after them: each first chunk begins its pass after the group before began
    // its own (chunk_record), and each begins it again out of order
    Grp a = {0}, b = {0};
    light = 0;
    pass_begin(&a, 0, EARLY);
    pass_begin(&b, 0, SCENE);
    pass_begin(&a, 0, EARLY);
    pass_begin(NULL, 0, OUTPUT);
    pass_begin(&b, 0, SCENE);
    pass_begin(&a, 0, EARLY);
    CHECK(light == OUTPUT, "two groups out: the light target is %d", light);

    // ---- a first chunk whose first pass begin waited (a deferred lookup returns before the hooks): the hooks run at
    // the begin that goes through, which is the one that marks the group
    g = (Grp){0};
    light = 0;
    if (pass_hooks_due(1, 0, g.hooks)) light = EARLY;   // the begin that goes through
    g.hooks = 1;
    CHECK(light == EARLY, "a group's hooks never ran");

    if (fails) printf("%d checks failed\n", fails);
    else printf("pass_hooks_check: ok\n");
    return fails != 0;
}
