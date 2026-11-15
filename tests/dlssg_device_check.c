// runtime/vk_dlssg_device.h: which card counts as usable while DLSS Frame Generation's interposer is in use, and how
// the device is made when the interposer cannot make it (vk_device.c pick_device, vk_dlssg.c vk_dlssg_create_device).
// The interposer and the loader are stand-ins that answer as the real ones did in a player's log. Built and run by
// tests/dlssg_device_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>

#include "vk_dlssg_device.h"

#define VENDOR_INTEL 0x8086u
#define VENDOR_AMD 0x1002u
#define EXT_NOT_PRESENT (-7)   // VK_ERROR_EXTENSION_NOT_PRESENT, what the interposer answered on Intel graphics

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// What a run did: the calls each stand-in got, and what each answers.
typedef struct { int interposer_calls, loader_calls, interposer_result, loader_result; } Run;
static int via_interposer(void *u) {
    Run *r = u;
    r->interposer_calls++;
    return r->interposer_result;
}
static int via_loader(void *u) {
    Run *r = u;
    r->loader_calls++;
    return r->loader_result;
}

// 0.10.1's creation, for comparison: the interposer alone whenever it is in use.
static int create_0101(int interposed, Run *r) { return interposed ? via_interposer(r) : via_loader(r); }

static const char *const hows[] = {"the loader", "the interposer", "the loader (not an NVIDIA card)",
                                   "the loader (the interposer refused)"};

static void expect(const char *what, int interposed, unsigned vendor, int interposer_result, int loader_result,
                   int want_result, int want_how, int want_interposer_calls, int want_loader_calls) {
    Run r = {0, 0, interposer_result, loader_result};
    int how = -1, refused = 0;
    int got = dlssg_device_create(interposed, vendor, via_interposer, via_loader, &r, &how, &refused);
    CHECK(got == want_result, "%s: result %d, not %d", what, got, want_result);
    CHECK(how == want_how, "%s: made by %s, not by %s", what, how >= 0 && how < 4 ? hows[how] : "?", hows[want_how]);
    CHECK(r.interposer_calls == want_interposer_calls, "%s: the interposer was asked %d times, not %d", what,
          r.interposer_calls, want_interposer_calls);
    CHECK(r.loader_calls == want_loader_calls, "%s: the loader was asked %d times, not %d", what, r.loader_calls,
          want_loader_calls);
    CHECK((how == DLSSG_DEV_REFUSED) == (refused != 0), "%s: the interposer's result is %d", what, refused);
    printf("  %s: %s, result %d\n", what, how >= 0 && how < 4 ? hows[how] : "?", got);
}

int main(void) {
    // ---- the laptop of the report: an RTX 3050 (gpu 0) and Intel Iris Xe (gpu 1), framegen = dlss, gpu_index = 1
    {
        Run old = {0, 0, EXT_NOT_PRESENT, 0};
        int r = create_0101(1, &old);
        CHECK(r == EXT_NOT_PRESENT && !old.loader_calls, "0.10.1 on the Intel card: %d", r);
        printf("  0.10.1, the Intel card through the interposer: result %d and nothing else tried (exit code 70)\n", r);
    }
    CHECK(dlssg_card_skipped(1, 1, VENDOR_INTEL), "the chosen Intel card is not skipped with the interposer in use");
    CHECK(!dlssg_card_skipped(1, 1, DLSSG_VENDOR_NVIDIA), "the chosen NVIDIA card is skipped");
    CHECK(!dlssg_card_skipped(1, 0, DLSSG_VENDOR_NVIDIA), "the NVIDIA card is skipped in the pass without a choice");
    printf("  pick: the chosen Intel card is skipped, so the pass without a choice takes the NVIDIA card\n");
    expect("the NVIDIA card, interposer in use", 1, DLSSG_VENDOR_NVIDIA, 0, 0, 0, DLSSG_DEV_INTERPOSER, 1, 0);

    // ---- no NVIDIA card at all (framegen = dlss left in nx2navi.ini): the device is made without frame generation
    CHECK(!dlssg_card_skipped(1, 0, VENDOR_AMD), "a card is skipped in the pass without a choice");
    CHECK(!dlssg_card_skipped(1, 0, VENDOR_INTEL), "a card is skipped in the pass without a choice");
    expect("an AMD card, interposer in use", 1, VENDOR_AMD, EXT_NOT_PRESENT, 0, 0, DLSSG_DEV_NOT_NVIDIA, 0, 1);
    expect("Intel graphics, interposer in use", 1, VENDOR_INTEL, EXT_NOT_PRESENT, 0, 0, DLSSG_DEV_NOT_NVIDIA, 0, 1);

    // ---- an NVIDIA card the interposer refuses (an old driver): once through it, then the loader
    expect("an NVIDIA card the interposer refuses", 1, DLSSG_VENDOR_NVIDIA, EXT_NOT_PRESENT, 0, 0, DLSSG_DEV_REFUSED, 1, 1);
    // ... and when the loader fails too, its result is the one reported
    expect("an NVIDIA card neither can make a device on", 1, DLSSG_VENDOR_NVIDIA, EXT_NOT_PRESENT, -3, -3,
           DLSSG_DEV_REFUSED, 1, 1);

    // ---- frame generation off: the loader, whatever the card, and a chosen card of any vendor stands
    expect("an NVIDIA card, frame generation off", 0, DLSSG_VENDOR_NVIDIA, 0, 0, 0, DLSSG_DEV_LOADER, 0, 1);
    expect("Intel graphics, frame generation off", 0, VENDOR_INTEL, EXT_NOT_PRESENT, 0, 0, DLSSG_DEV_LOADER, 0, 1);
    CHECK(!dlssg_card_skipped(0, 1, VENDOR_INTEL), "a chosen Intel card is skipped with frame generation off");
    CHECK(!dlssg_card_skipped(0, 1, VENDOR_AMD), "a chosen AMD card is skipped with frame generation off");

    printf("dlssg_device_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
