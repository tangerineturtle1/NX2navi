// Amiibo loader and state management subsystem (540-byte NTAG215 binary dumps).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_DUMP_SIZE 540
#define AMIIBO_APP_AREA_SIZE 0xD8   // bytes of game data a figure holds

// Device state corresponding to nn::nfp / nn::nfc DeviceState
typedef enum {
    AMIIBO_DEV_STATE_INITIALIZED = 0,
    AMIIBO_DEV_STATE_SEARCHING   = 1,
    AMIIBO_DEV_STATE_TAG_FOUND   = 2,
    AMIIBO_DEV_STATE_TAG_REMOVED = 3,
    AMIIBO_DEV_STATE_TAG_MOUNTED = 4,
    AMIIBO_DEV_STATE_UNAVAILABLE = 5
} AmiiboDeviceState;

// Amiibo identification info
typedef struct {
    uint8_t uuid[10];
    uint8_t uuid_len;
    uint8_t character_id[3];
    uint8_t series_id;
    uint16_t numbering_id;
    uint8_t nfp_type;
    uint8_t version;
    char path[512];
    char filename[128];
} AmiiboInfo;

// Subsystem lifecycle
void amiibo_init(void);

// Hardware / device emulation status
void amiibo_set_device_present(bool present);
bool amiibo_is_device_present(void);

// Tag detection & lifecycle
void amiibo_start_detection(void);
void amiibo_stop_detection(void);
bool amiibo_is_detecting(void);

// Tag mounting
bool amiibo_mount(void);
void amiibo_unmount(void);
bool amiibo_is_mounted(void);

// State query
AmiiboDeviceState amiibo_get_device_state(void);
// The state the menu shows: searching also within a second after a stop, since the game restarts detection many times
// a second while it waits for a tag. The game reads amiibo_get_device_state.
AmiiboDeviceState amiibo_get_reader_state(void);

// File loading & triggering actions
bool amiibo_load_file(const char *path);
bool amiibo_scan_and_tap(const char *path);
void amiibo_tap_tag(void);
void amiibo_remove_tag(void);

// Timer tick called during menu / frame updates
void amiibo_tick(void);

// Options
void amiibo_set_auto_remove(bool enable);
bool amiibo_get_auto_remove(void);
void amiibo_set_auto_remove_delay_s(float seconds);
float amiibo_get_auto_remove_delay_s(void);

// Tag inspection
bool amiibo_has_tag(void);
const AmiiboInfo *amiibo_get_current_info(void);
const uint8_t *amiibo_get_raw_data(void);

// Horizon OS buffers extraction (called by NFP IPC/override services)
void amiibo_get_tag_info(void *out_tag_info);
void amiibo_get_model_info(void *out_model_info);
void amiibo_get_common_info(void *out_common_info);
void amiibo_get_register_info(void *out_register_info);
int amiibo_get_application_area(void *out_buf, uint32_t max_size);

// ---- The figure's own data: owner, nickname, write counts and the game's application area.
// Kept per figure UID in <profile>/amiibo/<uid>.dat; seeded from the dump when the keys decrypt it, and written back
// into the dump file (encrypted and signed again) when NX2NAVI_AMIIBO_WRITEBACK is on and the keys read it.
typedef enum {
    AMIIBO_SOURCE_NEW = 0,     // nothing stored yet: defaults (registered, nickname from the file name, no area)
    AMIIBO_SOURCE_SAVED = 1,   // <profile>/amiibo/<uid>.dat
    AMIIBO_SOURCE_DUMP = 2     // the decrypted dump
} AmiiboDataSource;

typedef struct {
    AmiiboDataSource source;
    bool decrypted;               // the keys decrypted and verified this dump
    bool registered;              // owner and nickname set
    char nickname[41];            // UTF-8, at most 10 characters
    uint16_t write_counter;
    uint16_t setup_year;
    uint8_t setup_month, setup_day;
    uint16_t write_year;
    uint8_t write_month, write_day;
    bool has_app_area;
    uint32_t access_id;
    uint32_t app_area_size;       // bytes the game last wrote, at most AMIIBO_APP_AREA_SIZE
    uint8_t app_area[AMIIBO_APP_AREA_SIZE];
} AmiiboFigureData;

// A copy of the loaded figure's data. False without a loaded dump.
bool amiibo_get_figure_data(AmiiboFigureData *out);
// The menu's "erase game data": drops the application area and saves the figure. False without a loaded dump.
bool amiibo_erase_app_area(void);
// Loads (or reloads) the keys from NX2NAVI_AMIIBO_KEYS, or <amiibo folder>/key_retail.bin when that is unset, and
// decrypts the loaded dump again. True when keys are loaded afterwards.
bool amiibo_reload_keys(void);

// ---- nn::nfp application area calls (ov_system.c overrides, nfp.c IUser). Each returns an nn::nfp result: 0 or one
// of these. Without a mounted figure every call returns AMIIBO_R_DEVICE_NOT_FOUND.
#define AMIIBO_R_DEVICE_NOT_FOUND 0x8073u    // no figure on the reader (module 115, description 64)
#define AMIIBO_R_AREA_NOT_CREATED 0x10073u   // the figure has no application area yet (description 128)
#define AMIIBO_R_WRONG_ACCESS_ID 0x13073u    // the application area belongs to another access id (description 152)
#define AMIIBO_R_AREA_EXISTS 0x15073u        // CreateApplicationArea on a figure that has one (description 168)

uint32_t amiibo_open_app_area(uint32_t access_id);
// The application area cut to cap bytes into out; *out_size the bytes copied.
uint32_t amiibo_get_app_area(void *out, uint64_t cap, uint64_t *out_size);
// Changes the application area in memory only; amiibo_flush_figure stores it.
uint32_t amiibo_set_app_area(const void *data, uint64_t size);
// Create (recreate false: AMIIBO_R_AREA_EXISTS when there is one) or recreate an application area for access_id with
// size bytes of initial data, then stores the figure as amiibo_flush_figure does.
uint32_t amiibo_create_app_area(uint32_t access_id, const void *data, uint64_t size, bool recreate);
// Counts and dates a write, saves the figure's data and, with NX2NAVI_AMIIBO_WRITEBACK on and a decrypted dump, writes
// it back into the dump file (encrypted and signed again; <dump>.bak first, once).
uint32_t amiibo_flush_figure(void);

#ifdef __cplusplus
}
#endif
