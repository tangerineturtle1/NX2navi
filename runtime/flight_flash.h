// The flight recorder's flash trigger (flight.c flight_cells): whether a frame's picture is a one-frame flash, judged
// from the thumbnails of three frames in a row. A flash is a frame that differs strongly from the frame before and
// from the frame after while those two agree with each other: a texture that was blank for one frame, a draw that went
// missing, a picture from somewhere else shown once. Own header so tests/flight_flash_check.c checks it without a GPU.
//
// The pictures are compared as FLASH_CELLS_X x FLASH_CELLS_Y cells of mean brightness (flash_cells), which noise
// hardly changes and a moving camera changes slowly. A cell counts for the flash when
//   - the frame's mean is brighter than both neighbours' or darker than both, by more than `diff` (of 255) each, and
//   - the two neighbours are close to each other in that cell and in the eight cells around it: no further apart
//     than a quarter of what the frame differs by.
// The frame is a flash when at least `min_cells` cells count, and the neighbours agree over the picture as a whole:
// in no cell are they further apart than `diff`, and in at most FLASH_MOVING cells further than a quarter of it.
// What that leaves out:
//   - a camera cut: the frame agrees with the neighbour on its side of the cut;
//   - a fade: the frame lies between its neighbours;
//   - steady motion: a shape that crosses a cell makes that cell's mean rise and fall, and in the frame at the top
//     the cell is unlike both neighbours, which are alike. But what left the cell went into the next one, so the
//     neighbours differ there by as much: the cells around are what tells a shape passing from a flash in place.
//     A shape that jumps further than a cell a frame leaves its marks further away, where the frame before and
//     the frame after differ by as much as a flash does: such a frame is not judged;
//   - fast motion and anything else that changes much of the picture between the neighbours: not judged.
// It cannot tell a fault from a flash the game means (lightning, the white frame of a hit), and a flash while the
// camera turns fast goes unseen: the F8 dump is for those.
#pragma once
#include <stdint.h>
#include <string.h>

#define FLASH_CELLS_X 16
#define FLASH_CELLS_Y 9
#define FLASH_CELLS (FLASH_CELLS_X * FLASH_CELLS_Y)
#define FLASH_MOVING (FLASH_CELLS / 4)   // more cells than this in which the neighbours disagree: the picture moves
#define FLASH_STEP 2                     // flash_cells reads every FLASH_STEP-th pixel of every FLASH_STEP-th row

// The cell means of a picture of w x h pixels, four bytes a pixel with red or blue first (the weights are the same
// for both, so the thumbnail's byte order does not matter): (first + 2 * second + third) / 4 over the pixels read.
// A 640x360 thumbnail has cells of 40x40 pixels, of which this reads 400 each: 57600 pixels a picture.
static inline void flash_cells(const uint8_t *px, int w, int h, uint8_t out[FLASH_CELLS]) {
    for (int cy = 0; cy < FLASH_CELLS_Y; cy++) {
        int y0 = cy * h / FLASH_CELLS_Y, y1 = (cy + 1) * h / FLASH_CELLS_Y;
        uint32_t sum[FLASH_CELLS_X] = {0}, n[FLASH_CELLS_X] = {0};
        for (int y = y0; y < y1; y += FLASH_STEP) {
            const uint8_t *row = px + (size_t)y * (size_t)w * 4;
            for (int cx = 0; cx < FLASH_CELLS_X; cx++) {
                int x0 = cx * w / FLASH_CELLS_X, x1 = (cx + 1) * w / FLASH_CELLS_X;
                uint32_t s = 0;
                for (int x = x0; x < x1; x += FLASH_STEP) s += row[4 * x] + 2u * row[4 * x + 1] + row[4 * x + 2];
                sum[cx] += s;
                n[cx] += (uint32_t)(x1 - x0 + FLASH_STEP - 1) / FLASH_STEP;
            }
        }
        for (int cx = 0; cx < FLASH_CELLS_X; cx++)
            out[cy * FLASH_CELLS_X + cx] = n[cx] ? (uint8_t)(sum[cx] / (4 * n[cx])) : 0;
    }
}

// The test over the cell means of three frames in a row: the number of cells that count for a flash of `cur` when it
// is one (at least min_cells), else 0.
static inline int flash_test(const uint8_t prev[FLASH_CELLS], const uint8_t cur[FLASH_CELLS],
                             const uint8_t next[FLASH_CELLS], int diff, int min_cells) {
    uint8_t apart[FLASH_CELLS];   // how far apart the two neighbours are in each cell
    int moving = 0, agree = diff / 4;
    for (int i = 0; i < FLASH_CELLS; i++) {
        int d = (int)prev[i] - (int)next[i];
        apart[i] = (uint8_t)(d < 0 ? -d : d);
        moving += apart[i] > agree;
        if (apart[i] > diff) return 0;   // something crossed this cell between the two neighbours
    }
    if (moving > FLASH_MOVING) return 0;
    int n = 0;
    for (int y = 0; y < FLASH_CELLS_Y; y++)
        for (int x = 0; x < FLASH_CELLS_X; x++) {
            int i = y * FLASH_CELLS_X + x, a = (int)cur[i] - (int)prev[i], b = (int)cur[i] - (int)next[i];
            if ((a > 0) != (b > 0)) continue;   // between its neighbours
            if (a < 0) a = -a, b = -b;
            int by = a < b ? a : b;             // what the frame differs by from the nearer neighbour
            if (by <= diff) continue;
            int around = 0;                     // the neighbours' largest distance in this cell and those around it
            for (int v = y > 0 ? y - 1 : 0; v <= y + 1 && v < FLASH_CELLS_Y; v++)
                for (int u = x > 0 ? x - 1 : 0; u <= x + 1 && u < FLASH_CELLS_X; u++)
                    if (apart[v * FLASH_CELLS_X + u] > around) around = apart[v * FLASH_CELLS_X + u];
            n += around * 4 <= by;
        }
    return n >= min_cells ? n : 0;
}

// The cell means of the last thumbnails, by frame. A thumbnail completes a few presents after its frame and some
// frames have none (a present that was skipped), so each slot names the frame it holds; 0 is an empty slot (frames
// with a thumbnail start at 1).
#define FLASH_KEEP 4   // the frame judged and its two neighbours fall into three different slots
typedef struct {
    uint64_t frame[FLASH_KEEP];
    uint8_t cells[FLASH_KEEP][FLASH_CELLS];
} FlashTrack;

// Takes the cell means of `frame`'s thumbnail and judges the frame before it, the newest whose two neighbours can
// both be known. Returns flash_test's count when frame - 1 is a flash; 0 when it is none, and when it or frame - 2
// has no thumbnail here: a frame without both neighbours is not judged.
static inline int flash_note(FlashTrack *t, uint64_t frame, const uint8_t cells[FLASH_CELLS], int diff, int min_cells) {
    if (!frame) return 0;
    unsigned s = (unsigned)(frame % FLASH_KEEP);
    t->frame[s] = frame;
    memcpy(t->cells[s], cells, FLASH_CELLS);
    if (frame < 3) return 0;
    unsigned c = (unsigned)((frame - 1) % FLASH_KEEP), p = (unsigned)((frame - 2) % FLASH_KEEP);
    if (t->frame[c] != frame - 1 || t->frame[p] != frame - 2) return 0;
    return flash_test(t->cells[p], t->cells[c], t->cells[s], diff, min_cells);
}
