// The newest value each of the replay's timelines may reach, and the test a value read from one of them must pass
// (vk_sync.c "a lost device in the timelines' values"). Header only, so that tests/sync_hw_check.c checks it without a
// device.
//
// A timeline semaphore changes only through signal operations, and each of them carries a value the program chose:
// for the replay's two timelines, the queue hand-overs of its submissions (vk_sync.c hold_flush_locked) and the host's
// own signals (vk_sync.c vk_sync_start's restart, the lost-device probe, vk_device.c vk_selftest). A live device never
// reports a value above the newest of them. Each such value is noted here before its signal can happen, so a value read
// above the note comes from a device that answers for work it never ran: after a timeout reset, Windows releases every
// waiter of a timeline, and the reports of AMD's RX 6000 cards (0.9.1, 0.9.2) ran the completion callbacks of
// submissions the GPU had not finished ("occlusion query slot ... not available after its fence") before any call
// returned VK_ERROR_DEVICE_LOST.
#pragma once
#include <stdint.h>

// Raises the note *hw to v; never lowers it. Any thread.
static inline void sync_hw_raise(volatile uint64_t *hw, uint64_t v) {
    uint64_t o = __atomic_load_n(hw, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(hw, &o, v, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}

// 1 when raw, read from a timeline whose signals were all noted in hw (read after raw), is above every one of them.
static inline int sync_hw_beyond(uint64_t raw, uint64_t hw) { return raw > hw; }
