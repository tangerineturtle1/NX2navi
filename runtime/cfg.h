// Settings: the NX2NAVI_* environment variables, with nx2navi.ini next to nx2navi.exe behind them.
#pragma once
#include <stddef.h>

// getenv for a setting: the environment variable when set, else the nx2navi.ini key (the name without "NX2NAVI_", any
// case), else the device profile's default (cfg.c: the Steam Deck), else NULL. Same string semantics as getenv; the pointer stays valid for the process (a value cfg_set replaces
// is kept). A name without the NX2NAVI_ prefix goes to getenv unchanged. Safe from any thread once cfg_init has returned.
const char *cfg_get(const char *name);

// Reads nx2navi.ini once, from mem_init after the address space is claimed and before any setting is read. Logs the file, its key count, unknown keys and environment overrides. Keeps the restart-only settings' values (cfg_pending).
void cfg_init(void);

// Replaces the loaded keys with those of the file at path (tests). 1 when the file was read, 0 when missing.
// Not thread-safe: nothing else may call cfg_get meanwhile.
int cfg_load(const char *path);

// Live settings (the in-game menu). cfg_set stores value as the nx2navi.ini value of name (NX2NAVI_ prefixed), marks it for
// cfg_save and bumps cfg_version. cfg_get returns it unless the environment sets name. Any thread.
void cfg_set(const char *name, const char *value);
// Bumped by every cfg_set. A consumer that keeps a parsed value compares this with the number it last saw (one atomic
// load) and parses again when it changed.
unsigned cfg_version(void);
// plat_setenv for an NX2NAVI_ variable the process reads back: also bumps cfg_version, so lookups see the new value.
int cfg_setenv(const char *name, const char *value);
// 1 when the environment sets name: cfg_set cannot change what cfg_get returns (the menu greys the setting out).
int cfg_from_env(const char *name);
// The value of key, as nx2navi.ini spells it ("coop_accept"), in nx2navi.ini as the file is now: the file is read again,
// for the few settings that act while the game runs without the menu (co-op's acceptance and the room maker's
// remedies; everything else is read at start). Read as at start: a BOM is skipped, a repeated key's later line wins,
// quotes go and "key =" sets nothing. 1 and the value in out when the file sets it, 0 when it does not, -1 when the
// file cannot be read now or is empty (an editor saving it, a lock): the caller skips that reading.
int cfg_ini_now(const char *key, char *out, size_t cap);
int cfg_ini_read(const char *path, const char *key, char *out, size_t cap);   // the same, of the file at path
// 1 when the player set name (the environment or nx2navi.ini), 0 when cfg_get gives a device profile's default or NULL.
int cfg_is_set(const char *name);
// The keys of the file's lines that are no settings of this version, lower-case with ", " between them ("" when there
// are none; a list too long to keep ends early), and how many there are. Such a line does nothing: the log says so at
// start and the settings menu shows it (ui/menu.cpp), so that nobody takes one for the cause of what they see.
const char *cfg_unknown_keys(int *count);
// The device profile in use, "deck" or "desktop" (NX2NAVI_DEVICE_PROFILE, else what the machine is).
const char *cfg_device_profile(void);
// Writes the keys cfg_set changed since the last save into nx2navi.ini next to nx2navi.exe (cfg_save_to: the file at path).
// Only those lines change: the last line of a changed key gets the new value, a missing key goes under its section
// (after the commented example line of the key when the section has one; the section is added at the end of the file
// when missing), comments and every other line stay byte for byte. 1 on success or with nothing to write.
int cfg_save(void);
int cfg_save_to(const char *path);

// Reset (the menu's "Reset all settings to defaults"). Removes the active line of each key (names without NX2NAVI_, any
// case) from nx2navi.ini at the next cfg_save and its value from memory, so the consumer's own default applies again; the
// commented example lines stay. Keys the environment sets are skipped. Bumps cfg_version. Returns how many it reset.
int cfg_reset(const char *const *keys, int n);
// 0 when the last cfg_save / cfg_save_to failed to write (the values stay set in memory, the next save tries again).
int cfg_last_save_ok(void);
// Every setting with a value, for a bug report ("Copy system info"): the nx2navi.ini and cfg_set keys (name without
// NX2NAVI_, upper case; from_env 0), then the known NX2NAVI_ variables the environment sets (from_env 1).
void cfg_each_active(void (*cb)(const char *key, const char *value, int from_env, void *ctx), void *ctx);

// What the menu knows about a setting. key: without NX2NAVI_, upper case. def: what the consumer uses when the key is
// unset ("" = no fixed default, the consumer picks per machine). values: "0|1", "mailbox|fifo|immediate", a range
// "0..300", or NULL for free text. live: applies at once; the others are read at start.
typedef struct { const char *key, *def, *values; int live; } CfgMeta;
// The row of key (prefix and case ignored); NULL for a key the menu does not show.
const CfgMeta *cfg_meta(const char *key);
// The i-th row (0-based, table order); NULL past the last.
const CfgMeta *cfg_meta_at(int i);
// What key reads when the player has not set it: the device profile's value (the Steam Deck's), else the row's default;
// NULL for a key without a row. The menu's "default" mark follows it.
const char *cfg_default(const char *key);
// A restart-only setting (live 0) whose value differs from the one cfg_init read at start, which the running game still
// uses: 1 while a restart would change it, 0 otherwise and for a live key or one without a row (the menu's "after
// restart" note). cfg_pending_count: how many restart-only settings differ.
int cfg_pending(const char *key);
int cfg_pending_count(void);

// The setting's value for consumers: the environment or nx2navi.ini value, else the CfgMeta default. name has the NX2NAVI_
// prefix, like cfg_get; a name without a row is a bug (rt_fatal). A row with an empty default has no fixed value: its
// consumer reads cfg_get and keeps its own "unset" branch. An empty value counts as unset.
const char *cfg_value(const char *name);
// cfg_value parsed. A set value that does not parse (int: whole string; double: strtod) gives the default's number.
// cfg_bool: "0" is false, anything else true.
int cfg_int(const char *name);
double cfg_double(const char *name);
int cfg_bool(const char *name);
