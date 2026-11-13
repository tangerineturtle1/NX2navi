// HD rumble for a pair of Joy-Con (input_hd_pair.c): the rumble output report written to each Joy-Con's HID device,
// next to SDL, which reads the two as one gamepad and takes no rumble data for it. input.c calls it (hd_tick).
#pragma once
#include <stdint.h>

#include "rumble_hd.h"

// Looks for a left and a right Joy-Con among the HID devices and keeps their paths for hd_pair_write. 1 when both are
// there. Opens nothing. Any thread; takes the time of a HID device listing.
int hd_pair_find(void);
// Writes one rumble report to each Joy-Con: the left half of data to the left one, the right half to the right one.
// The first call after hd_pair_find opens the two devices. 0 when a device could not be opened or written (both are
// closed then, and SDL's error text says why), and after hd_pair_end. Blocks for as long as the writes take: the HD
// rumble thread only.
int hd_pair_write(const uint8_t data[RUMBLE_HD_DATA]);
// 1 while the two devices are open.
int hd_pair_is_open(void);
// Writes the neutral value to both Joy-Con, when neutral is set, and closes them. wait_ms: how long to wait for a
// write that is still running on another thread before giving up (the process is ending); 0 waits for it.
void hd_pair_close(int neutral, unsigned wait_ms);
// The process is ending (input.c input_rumble_hd_stop): hd_pair_close(1, wait_ms), and hd_pair_write writes nothing
// from here on, so a report the HD thread chose before the stop cannot open the two devices again after it.
void hd_pair_end(unsigned wait_ms);
