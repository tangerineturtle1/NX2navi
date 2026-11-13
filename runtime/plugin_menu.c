// A plugin's menu page in the game's own settings screen (plugin_api.h open_menu, docs/plugins.md "A menu in the
// game's style"). Nothing is drawn by the runtime: the page is the game's screen UISystemOptionScreen, opened by the
// game's own settings sequence, with the game's row parts, cursor, scrolling, sounds and transitions; only the rows
// and their texts are the plugin's.
//
// How (game 1.1.1; every address and offset is in plugin_game_ui.c). The settings screen is made anew at every
// opening and builds its rows from a table in memory: a count and an array of 0x28-byte items, each with an option
// type, the kind of its row (0 toggle, 1 select, 2 button, 3 separator), the number of its choices and the names of
// the game data it reads and writes. So:
//   - open_menu, called from a title menu button's on_select, arms the page; the title's hook (plugins.c
//     title_decide_hook) then hands the sequence the name of the game's own settings entry, and the game opens its
//     settings screen as if the player had chosen that;
//   - before the screen builds its rows (hook of its build function) the table's count and array are replaced by ours:
//     items with option types the game does not know (0x100 and up) and no game data, so that nothing the player does
//     on the page reaches a setting of the game; when the screen is destroyed the table is put back;
//   - every text the game then sets on the page comes through one function, and its hook hands over the plugin's text
//     instead, telling the calls apart by where they come from (the title, a row's label, a select row's value, a
//     button's text, the caption under the list);
//   - after the screen opened, each row gets its state (a toggle's check, a select's choice, a button's look) with the
//     game's own functions, and the cursor goes to the first row;
//   - the row functions' hooks turn what the player does into the plugin's events; "back" is the game's own, and the
//     save the settings sequence makes on the way out is left out.
// What the page cannot be: more than one at a time, opened from anywhere but a title menu button, or changed in its
// number and kinds of rows while it is open (update_menu takes new texts and values for the same rows).
//
// A page opened while a page is open replaces it: the open page is closed with the game's "back", and when its screen
// is gone the title menu's entry is pressed for the player (the A button, in plugin_menu_poll), with the new rows
// armed, so the game opens its settings screen again with them. The plugin's on_select is not called for that press.
//
// Where the page's game calls are made. The game's layout animator (main+0x2d1380 in 1.1.1) runs as a job on the
// game's worker threads, also while the finished picture is presented, which is when the plugins' frame callbacks run
// (on the presenting thread). A row's update stops and restarts animations (anim_stop_at), which changes the lists
// the animator walks: done from a frame callback it now and then left the animator walking a list without an end,
// and the game made no more frames (seen when a page was updated at every typed character). So update_menu only
// keeps the new rows, and they are put on the screen, the cursor is moved and a text row's typing is shown in a hook
// of the frame step (PluginGameUi.frame_step), which the game calls on its calc thread once the frame's jobs have
// been waited on: measured in the running game, the animator ran during 253 of 3664 presents and during none of
// 3664 frame steps. Should the frame step not be called (it is, on the title screen too), the frame callback does
// the work as before after half a second.
//
// A text row the player types into (Nx2MenuRow.text_max): choosing it takes the input from the game (capture_input)
// and the characters and keys of the window's thread are queued; a frame callback, on the game's thread, puts them
// into the text, shows it on the row's button with a cursor mark, and ends the entry at Enter or Escape.
// A gamepad types as well, with the buttons alone (edit_on_pad): the row then shows the character that is being chosen
// in brackets after the text; up and down go through the characters of a group (held: again and again), X goes to the
// next group (capitals, small letters, digits and marks), right takes the character, left or Y takes the last one
// away, A or + ends the entry and B leaves it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "plugin_game_ui.h"
#include "plugins.h"

enum { MAX_ROWS = 48, MAX_CHOICES = 16, TYPE0 = 0x100, CHOICE0 = 0x1000, K_TOGGLE = 0, K_SELECT = 1, K_ACTION = 2, K_SEP = 3 };
typedef struct { const char *text; int32_t bytes, pad; int64_t message; } Ref;   // what the game's text setter takes
typedef struct { uint32_t type, kind, r8, rc, nchoice, r14; uint64_t choices; uint32_t data, cond; } Item;   // 0x28 bytes
typedef struct {
    int kind, game_kind, cur, disabled, nchoice, text_max;
    Ref label, desc, value, choice[MAX_CHOICES];
    Ref value0;   // the value the row came with, which is the one to free: a text entry puts texts of the set's extras into value
} Row;
typedef struct Extra { struct Extra *next; } Extra;   // a text made after the set, freed with it
// One set of rows with its texts: the game keeps pointers into it while the screen lives, so a set is freed only when
// the next page opens.
typedef struct Set { struct Set *older; Extra *extra; int n, focus; Ref title, none; Row row[MAX_ROWS]; } Set;

static const Nx2PluginApi *api;
static const PluginGameUi *G;
static uint64_t B;
static Set *set;                  // the rows of the page that is armed or open
static Nx2MenuFn on_event;
static void *event_user;
static uint64_t armed_at;         // plat_now_ns when open_menu armed the page, 0 not armed
static int active;                // the settings screen that exists was built from our rows
static int hooked, skip_save, close_req, in_event;
static uint64_t screen, table, saved_items, overlay;
static uint32_t saved_count, caption_type;
static int caption_known;      // the open screen has set a caption: caption_type is its row's option type
static char overlay_id[48];
static Item items[MAX_ROWS];
static uint8_t shown_disabled[MAX_ROWS];   // what each row's control was last told
static Set *next_set;             // the page that replaces the open one, until that one is gone
static Nx2MenuFn next_event;
static void *next_user;
static int reopen;                // 1 the open page closes for next_set; 2 it is gone and the title entry is pressed
// A text row's entry: the row (-1 none), its text so far, and what the window's thread typed since the last frame.
enum { EDIT_MAX = 255, EDIT_QUEUE = 64, EDIT_ENTER = 0x0d, EDIT_ESCAPE = 0x1b, EDIT_BACK = 0x08,
       EDIT_PAD_UP = 0x110000, EDIT_PAD_DOWN, EDIT_PAD_TAKE, EDIT_PAD_GROUP };   // above every character
// The characters a gamepad chooses from, group after group.
static const char pad_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz" "0123456789.:-_/@ ";
static const int pad_group[] = {0, 26, 52, 69};   // where each group starts, and the end
static int pad_at, pad_mode;      // the character being chosen; 1 once the gamepad was used in this entry
static int pad_held;              // the direction held on the gamepad (EDIT_PAD_UP or _DOWN), 0 none
static uint64_t pad_next;         // plat_now_ns when the held direction counts again
static int edit_row = -1, edit_len, edit_cap;
static char edit_buf[EDIT_MAX + 1];
static Ref edit_old;
static PlatMutex edit_lock = PLAT_MUTEX_INIT;
static uint32_t edit_queue[EDIT_QUEUE];
static int edit_queued;
static Set *pending_set;          // update_menu's rows that the frame step has not put on the screen yet
static PlatMutex pending_lock = PLAT_MUTEX_INIT;
static uint64_t step_seen;        // plat_now_ns of the last frame step (h_step), 0 none yet
static unsigned step_applied, late_applied;   // sets put on the screen at the frame step / in the frame callback
#define STEP_LATE_NS 500000000ull // no frame step for this long: the frame callback does the page's work
static int focus_pending;         // 1 + the row the cursor is to go to (open_menu's and update_menu's focus_row, or off a
                                  // disabled row), taken between two frames once the page's cursor is live
static int last_focus;            // the row the cursor was on last, of those that can be used
static int cursor_live;           // the open page's cursor has come to a row: it can be sent to another
static int cursor_row = -1;       // the row the cursor came to last, disabled or not
static int focus_tries;           // times the pending move was asked of the screen
static uint64_t focus_asked;      // plat_now_ns of the last of them
// A move asked for while the game still answers a row's choice (update_menu inside on_event) is dropped by the screen,
// so it is asked again every FOCUS_RETRY_NS until the cursor is on the row, FOCUS_TRIES times at most.
#define FOCUS_RETRY_NS 400000000ull
enum { FOCUS_TRIES = 8 };

static void focus_ask(int row) {
    focus_tries = 0, focus_asked = 0;
    __atomic_store_n(&focus_pending, row + 1, __ATOMIC_RELEASE);
}
static int first_row;             // the row the page's first cursor is to be on, -1 none
static uint32_t choice_vals[MAX_CHOICES];
static uint64_t row_layout[MAX_ROWS], row_obj[MAX_ROWS], row_ctl[MAX_ROWS];
#define ARM_NS 10000000000ull     // an armed page that did not open within this is given up

static uint64_t r64(uint64_t a) { uint64_t v = 0; api->read(a, &v, 8); return v; }
static uint32_t r32(uint64_t a) { uint32_t v = 0; api->read(a, &v, 4); return v; }
static uint64_t call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t a[5] = {a0, a1, a2, a3, 0};
    return api->guest_call(fn, a, 5);
}
static void anim_at(uint64_t anim, float frame) {
    Nx2GuestRegs r = {.size = sizeof r};
    r.x[0] = anim;
    memcpy(&r.v[0][0], &frame, 4);
    if (anim) api->guest_call_regs(B + G->anim_stop_at, &r);
}

// A copy of a plugin's text as the game's text setter takes it. percent under 100: with the game's tag for the size of
// what follows in front (the byte 0x0E, then 16-bit words: group 0, type 2, 2 bytes of parameters, the size).
static void ref_set_sized(Ref *r, const char *text, int percent) {
    size_t n = text ? strlen(text) : 0, tag = percent < 100 ? 9 : 0;
    char *copy = malloc(tag + n + 1);
    if (!copy) {
        *r = (Ref){.text = "", .message = -1};
        return;
    }
    if (tag) memcpy(copy, (const char[9]){0x0e, 0, 0, 2, 0, 2, 0, (char)percent, 0}, 9);
    memcpy(copy + tag, text ? text : "", n + 1);
    for (size_t i = tag; i < tag + n; i++)
        if ((unsigned char)copy[i] < 0x20 && copy[i] != 0x0a) copy[i] = ' ';   // would read as a tag of the game's text format; a line break stays
    *r = (Ref){.text = copy, .bytes = (int32_t)(tag + n), .message = -1};
}
static void ref_set(Ref *r, const char *text) { ref_set_sized(r, text, 100); }

// The size a button row's label is written at: the game's heading font fits about ACTION_FIT characters beside the
// button at full size, and a longer label is written smaller to stay clear of it.
enum { ACTION_FIT = 11 };
static int action_percent(const char *label) {
    int chars = 0;
    for (const char *p = label ? label : ""; *p; p++) chars += (*p & 0xc0) != 0x80;
    int percent = chars > ACTION_FIT ? 100 * ACTION_FIT / chars : 100;
    return percent < 45 ? 45 : percent;
}
static void ref_free(Ref *r) { if (r->text && *r->text) free((char *)(uintptr_t)r->text); }

static void set_free(Set *s) {
    while (s) {
        Set *older = s->older;
        ref_free(&s->title);
        for (int i = 0; i < s->n; i++) {
            ref_free(&s->row[i].label), ref_free(&s->row[i].desc), ref_free(&s->row[i].value0);
            for (int k = 0; k < s->row[i].nchoice; k++) ref_free(&s->row[i].choice[k]);
        }
        for (Extra *e = s->extra, *n; e; e = n) n = e->next, free(e);
        free(s);
        s = older;
    }
}

// A copy of the plugin's menu; NULL when it is not one the page can show (the reason is logged).
static Set *set_make(const Nx2Menu *m) {
    if (!m || m->size < offsetof(Nx2Menu, focus_row) || !m->rows || !m->row_count || m->row_count > MAX_ROWS) {
        rt_log("plugins: menu refused: it needs 1 to %d rows", MAX_ROWS);
        return NULL;
    }
    Set *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int buttons = 0, percent = 100;   // one size for the labels of all button rows of the page: the longest one's
    for (uint32_t i = 0; i < m->row_count; i++) {
        const Nx2MenuRow *in = (const Nx2MenuRow *)((const char *)m->rows + (size_t)i * m->rows->size);
        int p = in->kind == NX2_ROW_ACTION || in->kind == NX2_ROW_TEXT ? action_percent(in->label) : 100;
        if (p < percent) percent = p;
    }
    ref_set(&s->title, m->title);
    s->none = (Ref){.text = "", .message = -1};
    s->focus = NX2_HAS(m, Nx2Menu, focus_row) && m->focus_row <= m->row_count ? (int)m->focus_row : 0;
    for (uint32_t i = 0; i < m->row_count; i++) {
        const Nx2MenuRow *in = (const Nx2MenuRow *)((const char *)m->rows + (size_t)i * m->rows->size);
        Row *r = &s->row[s->n++];
        r->kind = (int)in->kind, r->disabled = in->disabled != 0, r->cur = in->current;
        r->text_max = in->kind == NX2_ROW_TEXT && in->size >= sizeof *in ? (int)(in->text_max < EDIT_MAX ? in->text_max : EDIT_MAX) : 0;
        r->game_kind = in->kind == NX2_ROW_TOGGLE ? K_TOGGLE : in->kind == NX2_ROW_SELECT ? K_SELECT :
                       in->kind == NX2_ROW_SEPARATOR ? K_SEP : K_ACTION;
        buttons += r->game_kind == K_ACTION;
        ref_set_sized(&r->label, in->label, r->game_kind == K_ACTION ? percent : 100);
        ref_set(&r->desc, in->description), ref_set(&r->value, in->value);
        r->value0 = r->value;
        if (r->game_kind == K_SELECT) {
            r->nchoice = in->choices ? (int)(in->choice_count < MAX_CHOICES ? in->choice_count : MAX_CHOICES) : 0;
            for (int k = 0; k < r->nchoice; k++) ref_set(&r->choice[k], in->choices[k]);
            if (!r->nchoice || in->size < sizeof *in) {
                rt_log("plugins: menu refused: select row %u has no choices", i);
                set_free(s);
                return NULL;
            }
            if (r->cur < 0 || r->cur >= r->nchoice) r->cur = 0;
        }
    }
    if (!buttons) {   // the screen puts its first cursor on its first button row
        rt_log("plugins: menu refused: it needs at least one action or text row");
        set_free(s);
        return NULL;
    }
    return s;
}

static void emit_text(uint32_t kind, int row, int value, const char *text) {
    if (!on_event) return;
    Nx2MenuEvent e = {.size = sizeof e, .kind = kind, .row = (uint32_t)row, .value = value, .text = text};
    in_event++;
    on_event(&e, event_user);
    in_event--;
}

static void emit(uint32_t kind, int row, int value) {
    if (!on_event) return;
    Nx2MenuEvent e = {.size = sizeof e, .kind = kind, .row = (uint32_t)row, .value = value};
    in_event++;
    on_event(&e, event_user);
    in_event--;
}

static int row_of(const uint64_t *of, uint64_t what) {
    for (int i = 0; set && what && i < set->n; i++)
        if (of[i] == what) return i;
    return -1;
}

// The game's row objects of the screen, in the order of its row list, which is the order of our items.
static void map_rows(void) {
    uint64_t list = r64(screen + G->os_rows);
    uint32_t n = r32(screen + G->os_rows_n);
    memset(row_layout, 0, sizeof row_layout), memset(row_obj, 0, sizeof row_obj), memset(row_ctl, 0, sizeof row_ctl);
    for (uint32_t i = 0; i < n && i < (uint32_t)set->n; i++) {
        uint32_t kind = r32(list + 8 * i), index = r32(list + 8 * i + 4);
        if (kind > 2 || (int)kind != set->row[i].game_kind || index >= r32(screen + G->os_kind_count[kind])) continue;
        uint64_t obj = r64(r64(screen + G->os_kind_count[kind] + 8) + 8 * index);
        row_obj[i] = obj, row_layout[i] = r64(obj + G->row_layout);
        row_ctl[i] = call(B + G->find_control, r64(r64(obj + G->row_screen) + G->os_controls_box), row_layout[i], 0, 0);
    }
}

static void pane_text(uint64_t layout, const char *pane, const Ref *ref) {
    uint64_t p = call(B + G->layout_pane, layout, (uint64_t)(uintptr_t)pane, 0, 0);
    if (p && (r32(p + G->pane_flags) & G->pane_text_mask) == G->pane_text_kind)
        call(B + G->pane_set_text, p, (uint64_t)(uintptr_t)ref, 0, 0);
}

// One row's state on the screen, from the set.
static void apply_row(int i) {
    const Row *r = &set->row[i];
    uint64_t obj = row_obj[i], ctl = row_ctl[i];
    if (!obj || !ctl) return;
    if (r->game_kind == K_TOGGLE) {
        uint8_t on = r->cur != 0;
        call(B + G->control_set_check, ctl, on, 0, 0);
        api->write(obj + G->tog_original, &on, 1);
        anim_at(r64(obj + G->tog_anim), (float)((on ? 0 : 1) + (r->disabled ? 2 : 0)));
    } else if (r->game_kind == K_SELECT) {
        uint32_t v = (uint32_t)r->cur;
        api->write(obj + G->sel_index, &v, 4), api->write(obj + G->sel_original, &v, 4);
        call(B + G->sel_text, obj, 0, 0, 0);
    } else {
        anim_at(r64(obj + G->act_anim), 1);
        call(B + G->act_refresh, obj, 0, 0, 0);
        pane_text(row_layout[i], "T_BtnTitle_00", &r->label);
        pane_text(row_layout[i], "T_AppIcon_00", &set->none);
    }
    if (ctl && (uint8_t)r->disabled != shown_disabled[i]) {   // as the game shows a setting that another one rules out
        uint8_t enabled = !r->disabled;
        call(r64(r64(ctl) + G->ctl_disable_slot), ctl, (uint64_t)r->disabled, 0, 0);
        api->write(ctl + G->ctl_enabled, &enabled, 1);
        shown_disabled[i] = (uint8_t)r->disabled;
    }
}

// The cursor goes to a row of the open page: the screen's own "focus this control" (the function the game's screens
// place their cursor with) hands the row's control to the cursor, which glides there in its next updates; the screen's
// focus handling (h_focus) then runs as for a move the player made, about a third of a second later. It is called
// between two frames (menu_on_frame) and with nothing else written: tried in the running game, the cursor then goes
// to the row every time, whichever row it is on. (An earlier way, inside the screen's own update or a row's choice and
// with the cursor manager's item and the screen's "row to focus" field written first, left the cursor where it was;
// which of those differences decides was not looked into.)
static void focus_row(int i) {
    if (row_ctl[i]) call(B + G->screen_focus, screen, row_ctl[i], 0, 0);
}

// ---- a text row's entry

// A text of the open set that did not come with it (the entry's text as it grows): freed with the set.
static void ref_extra(Ref *r, const char *text, int percent) {
    size_t n = strlen(text);
    Extra *e = malloc(sizeof *e + 9 + n + 1);
    if (!e) return;
    char *copy = (char *)(e + 1);
    size_t tag = percent < 100 ? 9 : 0;
    if (tag) memcpy(copy, (const char[9]){0x0e, 0, 0, 2, 0, 2, 0, (char)percent, 0}, 9);
    memcpy(copy + tag, text, n + 1);
    e->next = set->extra, set->extra = e;
    *r = (Ref){.text = copy, .bytes = (int32_t)(tag + n), .message = -1};
}

static void edit_show(int cursor) {
    char shown[EDIT_MAX + 8];
    if (cursor && pad_mode) snprintf(shown, sizeof shown, "%s[%c]", edit_buf, pad_chars[pad_at]);
    else snprintf(shown, sizeof shown, "%s%s", edit_buf, cursor ? "_" : "");
    ref_extra(&set->row[edit_row].value, shown, 100);
    apply_row(edit_row);
}

static void edit_end(int keep) {
    int row = edit_row;
    if (row < 0) return;
    edit_row = -1;
    api->capture_input(0);
    if (!set || !active || row >= set->n) return;
    if (keep) {
        edit_row = row, edit_show(0), edit_row = -1;
        emit_text(NX2_MENU_TEXT, row, edit_len, edit_buf);
    } else {
        set->row[row].value = edit_old;
        apply_row(row);
    }
}

static void edit_begin(int row) {
    const Row *r = &set->row[row];
    const char *text = r->value.text ? r->value.text : "";
    edit_old = r->value;
    edit_cap = r->text_max;
    edit_len = (int)strlen(text) < edit_cap ? (int)strlen(text) : edit_cap;
    while (edit_len > 0 && ((unsigned char)text[edit_len] & 0xc0) == 0x80) edit_len--;   // not inside a character
    memcpy(edit_buf, text, (size_t)edit_len), edit_buf[edit_len] = 0;
    plat_mutex_lock(&edit_lock);
    edit_queued = 0;
    plat_mutex_unlock(&edit_lock);
    // the gamepad's first character: a digit after a text that ends like a number or an address, else a capital
    char last = edit_len ? edit_buf[edit_len - 1] : 0;
    pad_at = (last >= '0' && last <= '9') || last == '.' || last == ':' ? pad_group[2] : 0;
    pad_mode = 0;
    __atomic_store_n(&pad_held, 0, __ATOMIC_RELAXED);
    edit_row = row;
    api->capture_input(1);
    edit_show(1);
}

static void edit_push(uint32_t what) {     // the window's thread, or the thread that reads the controls
    plat_mutex_lock(&edit_lock);
    if (edit_queued < EDIT_QUEUE) edit_queue[edit_queued++] = what;
    plat_mutex_unlock(&edit_lock);
}

static void edit_on_text(uint32_t cp, void *u) {
    (void)u;
    if (__atomic_load_n(&edit_row, __ATOMIC_RELAXED) >= 0 && cp >= 0x20 && cp != 0x7f) edit_push(cp);
}

static int edit_on_key(const Nx2KeyEvent *e, void *u) {
    (void)u;
    if (__atomic_load_n(&edit_row, __ATOMIC_RELAXED) < 0) return 0;
    if (e->down && (e->key == NX2_KEY_ENTER || e->key == NX2_KEY_ESCAPE || e->key == NX2_KEY_BACKSPACE))
        edit_push(e->key == NX2_KEY_ENTER ? EDIT_ENTER : e->key == NX2_KEY_ESCAPE ? EDIT_ESCAPE : EDIT_BACK);
    return 1;   // no key reaches the runtime's own menu keys while the player types
}

static void edit_on_pad(const Nx2PadEvent *e, void *u) {
    (void)u;
    if (__atomic_load_n(&edit_row, __ATOMIC_RELAXED) < 0) return;
    int dir = e->button == NX2_PAD_UP ? EDIT_PAD_UP : e->button == NX2_PAD_DOWN ? EDIT_PAD_DOWN : 0;
    if (!e->down) {
        if (dir && __atomic_load_n(&pad_held, __ATOMIC_RELAXED) == dir) __atomic_store_n(&pad_held, 0, __ATOMIC_RELAXED);
        return;
    }
    if (e->keyboard) return;   // a key bound to a button is a letter here, or a key the window's thread hands over
    if (dir) {
        __atomic_store_n(&pad_next, plat_now_ns() + 400000000ull, __ATOMIC_RELAXED);
        __atomic_store_n(&pad_held, dir, __ATOMIC_RELAXED);
        edit_push((uint32_t)dir);
    } else if (e->button == NX2_PAD_A || e->button == NX2_PAD_PLUS) edit_push(EDIT_ENTER);
    else if (e->button == NX2_PAD_B) edit_push(EDIT_ESCAPE);
    else if (e->button == NX2_PAD_RIGHT) edit_push(EDIT_PAD_TAKE);
    else if (e->button == NX2_PAD_LEFT || e->button == NX2_PAD_Y) edit_push(EDIT_BACK);
    else if (e->button == NX2_PAD_X) edit_push(EDIT_PAD_GROUP);
}

static void apply_set(Set *s);

// The game's calc thread, at the frame step (h_step): the rows update_menu left, the cursor's move that focus_row or a
// disabled row asked for, and what was typed into a text row.
static void menu_work(int at_step) {
    plat_mutex_lock(&pending_lock);
    Set *p = pending_set;
    pending_set = NULL;
    plat_mutex_unlock(&pending_lock);
    if (p) {
        if (active && set && p->n == set->n && !__atomic_load_n(&reopen, __ATOMIC_ACQUIRE)) {
            apply_set(p);
            if (at_step) step_applied++;
            else if (!late_applied++) rt_log("plugins: menu: no frame step for half a second: the page is updated in the frame callback");
        } else set_free(p);   // the page it was for has gone
    }
    int want = __atomic_load_n(&focus_pending, __ATOMIC_ACQUIRE);
    if (want && active && set && cursor_live && edit_row < 0) {
        uint64_t now = plat_now_ns();
        if (want > set->n || !row_ctl[want - 1] || cursor_row == want - 1 || focus_tries >= FOCUS_TRIES) {
            __atomic_compare_exchange_n(&focus_pending, &want, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);   // there, or given up
        } else if (now - focus_asked >= FOCUS_RETRY_NS) {
            focus_asked = now, focus_tries++;
            focus_row(want - 1);
            if (!set->row[want - 1].disabled) last_focus = want - 1;
        }
    }
    if (edit_row < 0) return;
    if (!active || !set) { edit_end(0); return; }
    uint32_t q[EDIT_QUEUE];
    plat_mutex_lock(&edit_lock);
    int n = edit_queued, changed = 0;
    memcpy(q, edit_queue, sizeof q[0] * (size_t)n);
    edit_queued = 0;
    plat_mutex_unlock(&edit_lock);
    int held = __atomic_load_n(&pad_held, __ATOMIC_RELAXED);   // a direction held on the gamepad goes on by itself
    if (held && n < EDIT_QUEUE && plat_now_ns() >= __atomic_load_n(&pad_next, __ATOMIC_RELAXED)) {
        q[n++] = (uint32_t)held;
        __atomic_store_n(&pad_next, plat_now_ns() + 90000000ull, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < n && edit_row >= 0; i++) {
        uint32_t c = q[i];
        if (c == EDIT_ENTER || c == EDIT_ESCAPE) {
            edit_end(c == EDIT_ENTER);
        } else if (c == EDIT_BACK) {
            while (edit_len > 0 && ((unsigned char)edit_buf[--edit_len] & 0xc0) == 0x80) {}
            edit_buf[edit_len] = 0, changed = 1;
        } else if (c == EDIT_PAD_UP || c == EDIT_PAD_DOWN || c == EDIT_PAD_GROUP) {
            int g = 0;
            while (pad_at >= pad_group[g + 1]) g++;
            int lo = pad_group[g], count = pad_group[g + 1] - lo;
            if (c == EDIT_PAD_GROUP) pad_at = pad_group[(g + 1) % 3];
            else if (pad_mode) pad_at = lo + (pad_at - lo + (c == EDIT_PAD_UP ? 1 : count - 1)) % count;
            pad_mode = 1, changed = 1;   // the first press only shows the character
        } else if (c == EDIT_PAD_TAKE) {
            if (pad_mode && edit_len + 1 <= edit_cap) edit_buf[edit_len++] = pad_chars[pad_at], edit_buf[edit_len] = 0;
            pad_mode = 1, changed = 1;
        } else {
            char utf[4];
            int k = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
            if (k == 1) utf[0] = (char)c;
            else if (k == 2) utf[0] = (char)(0xc0 | c >> 6), utf[1] = (char)(0x80 | (c & 0x3f));
            else if (k == 3) utf[0] = (char)(0xe0 | c >> 12), utf[1] = (char)(0x80 | (c >> 6 & 0x3f)), utf[2] = (char)(0x80 | (c & 0x3f));
            else utf[0] = (char)(0xf0 | c >> 18), utf[1] = (char)(0x80 | (c >> 12 & 0x3f)), utf[2] = (char)(0x80 | (c >> 6 & 0x3f)),
                 utf[3] = (char)(0x80 | (c & 0x3f));
            if (c > 0x10ffff || edit_len + k > edit_cap) continue;
            memcpy(edit_buf + edit_len, utf, (size_t)k), edit_len += k, edit_buf[edit_len] = 0, changed = 1;
        }
    }
    if (changed && edit_row >= 0) edit_show(1);
}

// The frame step, on the game's calc thread, before the function itself runs.
static void h_step(Nx2HookCall *c, void *u) {
    (void)u;
    uint64_t ra = c->lr - B;
    if (ra != G->frame_step_ra[0] && ra != G->frame_step_ra[1]) return;
    __atomic_store_n(&step_seen, plat_now_ns(), __ATOMIC_RELEASE);
    if (active || __atomic_load_n(&focus_pending, __ATOMIC_ACQUIRE) || edit_row >= 0) menu_work(1);
}

// The presenting thread, between two frames: only when the frame step does not come.
static void edit_on_frame(const Nx2Frame *f, void *u) {
    (void)f, (void)u;
    uint64_t seen = __atomic_load_n(&step_seen, __ATOMIC_ACQUIRE);
    if (seen && plat_now_ns() - seen < STEP_LATE_NS) return;
    menu_work(0);
}

// ---- the hooks

static void h_build(Nx2HookCall *c, void *u) {
    (void)u;
    uint64_t now = plat_now_ns(), at = __atomic_exchange_n(&armed_at, 0, __ATOMIC_ACQ_REL);
    active = skip_save = 0;
    __atomic_store_n(&reopen, 0, __ATOMIC_RELEASE);
    memset(shown_disabled, 0, sizeof shown_disabled);
    if (!at || now - at > ARM_NS || !set) return;   // the game's own settings
    table = r64(r64(B + G->opt_table_global) + G->opt_table_field);
    if (!table || r64(c->x[0]) != B + G->opt_vtable) return;
    screen = c->x[0];
    saved_count = r32(table), saved_items = r64(table + 8);
    for (int i = 0; i < set->n; i++)
        items[i] = (Item){.type = (uint32_t)(TYPE0 + i), .kind = (uint32_t)set->row[i].game_kind,
                          .nchoice = (uint32_t)set->row[i].nchoice, .choices = (uint64_t)(uintptr_t)choice_vals};
    uint32_t n = (uint32_t)set->n;
    uint64_t p = (uint64_t)(uintptr_t)items;
    api->write(table, &n, 4), api->write(table + 8, &p, 8);
    active = skip_save = 1, close_req = 0, overlay = 0, caption_known = 0;
    api->call_original(c);
    map_rows();
    rt_log("plugins: menu: the game's settings screen opens with %d rows of a plugin", set->n);
}

static void h_open(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active || c->x[0] != screen) return;
    api->call_original(c);
    map_rows();   // the rows' controls exist from here on
    int first = -1;
    for (int i = 0; i < set->n; i++) {
        apply_row(i);
        if (first < 0 && row_ctl[i] && !set->row[i].disabled) first = i;
    }
    // The layout holds one row part of each kind as the pattern its rows are made from; the part of a kind the page has
    // no row of stays where the layout put it, an empty row under the list. Those are taken out of the picture.
    static const char *const pattern[3] = {"Pa_ToggleBtn_00", "Pa_SelectBtn_00", "Pa_Separator_00"};
    static const int pattern_kind[3] = {K_TOGGLE, K_SELECT, K_SEP};
    for (int k = 0; k < 3; k++) {
        int used = 0;
        for (int i = 0; i < set->n; i++) used |= set->row[i].game_kind == pattern_kind[k];
        uint64_t pane = used ? 0 : call(B + G->layout_pane, r64(screen + G->os_layout), (uint64_t)(uintptr_t)pattern[k], 0, 0);
        uint8_t flags = 0;
        if (pane && api->read(pane + G->pane_visible, &flags, 1)) flags &= (uint8_t)~1u, api->write(pane + G->pane_visible, &flags, 1);
    }
    // The first cursor. The screen has just noted its first button row as the row to focus (os_focus) and places its
    // cursor there when the page has come in; the note is changed to the row asked for (focus_row), or to the first
    // row that can be used. Should the cursor still come up elsewhere, h_focus sends it on.
    if (set->focus && row_ctl[set->focus - 1] && !set->row[set->focus - 1].disabled) first = set->focus - 1;
    cursor_live = 0, cursor_row = -1, first_row = first;
    __atomic_store_n(&focus_pending, 0, __ATOMIC_RELEASE);
    if (first >= 0) api->write(screen + G->os_focus, &row_ctl[first], 8), last_focus = first;
}

static void h_dtor(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active || c->x[0] != screen) return;
    api->write(table, &saved_count, 4), api->write(table + 8, &saved_items, 8);
    active = 0, screen = 0, close_req = 0;
    if (edit_row >= 0) edit_end(0);
    plat_mutex_lock(&pending_lock);
    Set *p = pending_set;
    pending_set = NULL;
    plat_mutex_unlock(&pending_lock);
    set_free(p);
    if (step_applied || late_applied)
        rt_log("plugins: menu: the page's rows were updated %u times at the frame step, %u times in the frame callback",
               step_applied, late_applied);
    step_applied = late_applied = 0;
    if (__atomic_load_n(&reopen, __ATOMIC_ACQUIRE) == 1 && next_set) {   // the page that replaces this one
        next_set->older = set;   // freed with it: the screen that is going still points into the old texts
        set = next_set, on_event = next_event, event_user = next_user, next_set = NULL;
        __atomic_store_n(&armed_at, plat_now_ns() | 1, __ATOMIC_RELEASE);
        __atomic_store_n(&reopen, 2, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&reopen, 0, __ATOMIC_RELEASE);
    emit(NX2_MENU_CLOSED, 0, 0);
}

static void h_caption(Nx2HookCall *c, void *u) {
    (void)u;
    if (active && c->x[0] == screen) caption_type = (uint32_t)c->x[1], caption_known = 1;
}

// Every text the game sets: on our page, its own calls get the plugin's texts.
static void h_text(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active || !set) return;
    uint64_t ra = c->lr - B;
    int row = row_of(row_layout, c->x[0]);
    const Ref *t = NULL;
    if (ra == G->ra_title) {
        t = &set->title, overlay = c->x[0];
        api->read(c->x[1], overlay_id, sizeof overlay_id - 1);
    } else if (ra == G->ra_guide) {
        char id[32] = "";
        api->read(c->x[1], id, sizeof id - 1);
        if (!strcmp(id, "Pa_Guide_00/T_Guide_00")) t = &set->none;   // "reset to defaults": not on a plugin's page
    } else if (ra == G->ra_caption) {
        uint32_t i = caption_type - TYPE0;
        t = i < (uint32_t)set->n ? &set->row[i].desc : &set->none;
    } else if (row < 0) {
        return;
    } else if (ra == G->ra_label[0] || ra == G->ra_label[1]) {
        t = &set->row[row].label;
    } else if (ra == G->ra_select_value) {
        uint32_t k = r32(row_obj[row] + G->sel_index);
        t = k < (uint32_t)set->row[row].nchoice ? &set->row[row].choice[k] : &set->none;
    } else if (ra == G->ra_action_text[0] || ra == G->ra_action_text[1] || ra == G->ra_action_text[2]) {
        t = &set->row[row].value;
    }
    if (t) c->x[2] = (uint64_t)(uintptr_t)t;
}

static void h_focus(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active || c->x[0] != screen) return;
    uint64_t ctl = c->x[1];
    api->call_original(c);
    int row = row_of(row_ctl, ctl);
    if (row < 0) return;
    cursor_row = row;
    if (!cursor_live) {            // the page's first cursor
        cursor_live = 1;
        if (first_row >= 0 && row != first_row && !__atomic_load_n(&focus_pending, __ATOMIC_ACQUIRE)) {
            focus_ask(first_row);
            return;
        }
    }
    if (set->row[row].disabled) {
        // The cursor came to a row that is disabled (the game itself passes over a disabled button row, not over a
        // switch or a choice): it goes on in the direction it came, to the next row that can be used, between this
        // frame and the next; when there is none that way (the list's end, or the cursor wrapped round), the nearest
        // one the other way. The player sees the cursor touch the disabled row on its way.
        int dir = row > last_focus ? 1 : -1, to = -1;
        for (int pass = 0; pass < 2 && to < 0; pass++, dir = -dir)
            for (int i = row + dir; i >= 0 && i < set->n && to < 0; i += dir)
                if (row_ctl[i] && !set->row[i].disabled) to = i;
        if (to >= 0 && !__atomic_load_n(&focus_pending, __ATOMIC_ACQUIRE)) focus_ask(to);   // not on the way to a row asked for
        return;
    }
    last_focus = row;
    emit(NX2_MENU_FOCUS, row, set->row[row].cur);
}

static void h_decide(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active || c->x[0] != screen) return;
    int row = row_of(row_ctl, c->x[1]);
    if (row < 0) return;
    Row *r = &set->row[row];
    if (r->disabled || r->game_kind == K_ACTION) {   // a button's own action is the game's: left out
        c->skip = 1;
        if (r->disabled || edit_row >= 0) return;
        if (r->kind == NX2_ROW_TEXT && r->text_max > 0) edit_begin(row);
        else emit(NX2_MENU_ACTIVATE, row, 0);
        return;
    }
    api->call_original(c);
    uint8_t on = 0;
    api->read(c->x[1] + G->ctl_check, &on, 1);
    int v = r->game_kind == K_TOGGLE ? on != 0 : (int)r32(row_obj[row] + G->sel_index);
    if (v != r->cur) r->cur = v, emit(NX2_MENU_CHANGE, row, v);
}

static void h_left_right(Nx2HookCall *c, void *u) {
    (void)u;
    int row = active ? row_of(row_obj, c->x[0]) : -1;
    if (row < 0) return;
    Row *r = &set->row[row];
    if (r->disabled) { c->skip = 1; return; }
    api->call_original(c);
    int v = (int)r32(row_obj[row] + G->sel_index);
    if (v != r->cur) r->cur = v, emit(NX2_MENU_CHANGE, row, v);
}

// The settings sequence saves the game's settings when its screen closes: not after a plugin's page.
static void h_save(Nx2HookCall *c, void *u) {
    (void)u;
    if (!skip_save) return;
    uint32_t done = 3;
    if (r32(c->x[0] + G->save_node_state) != 0) return;
    api->write(c->x[0] + G->save_node_state, &done, 4);
    skip_save = 0;
}

// The sequence's test of the buttons pressed: on a plugin's page only "back" (bit 1) reaches it.
static void h_trig(Nx2HookCall *c, void *u) {
    (void)u;
    if (!active) return;
    uint64_t p = r64(r64(c->x[0] + G->trig_node_mask));
    uint8_t m = 0, only_back;
    if (!p || !api->read(p, &m, 1) || !(m & ~2u)) return;
    only_back = m & 2;            // for this one test; the node's own mask is put back for the game's settings
    api->write(p, &only_back, 1);
    api->call_original(c);
    api->write(p, &m, 1);
}

// ---- the interface

static int ready(void) {
    if (!api) api = nx2navi_plugin_api(NX2_PLUGIN_API_VERSION), G = plugin_game_ui(), B = api->game_info()->main_base;
    if (!G) return 0;
    if (hooked) return hooked > 0;
    hooked = api->hook_function(B + G->opt_build, h_build, NULL) && api->hook_function(B + G->opt_open, h_open, NULL) &&
             api->hook_function(B + G->opt_dtor, h_dtor, NULL) && api->hook_function(B + G->opt_caption, h_caption, NULL) &&
             api->hook_function(B + G->layout_set_text, h_text, NULL) && api->hook_function(B + G->opt_focus, h_focus, NULL) &&
             api->hook_function(B + G->opt_decide, h_decide, NULL) &&
             api->hook_function(B + G->sel_left_right, h_left_right, NULL) &&
             api->hook_function(B + G->save_node_update, h_save, NULL) &&
             api->hook_function(B + G->trig_node_update, h_trig, NULL) ? 1 : -1;
    // without it the frame callback does the work (edit_on_frame), as before the page's calls moved to the frame step
    if (hooked > 0 && !api->hook_function(B + G->frame_step, h_step, NULL))
        rt_log("plugins: menu: the frame step could not be hooked: the page is updated in the frame callback");
    for (int k = 0; k < MAX_CHOICES; k++) choice_vals[k] = (uint32_t)(CHOICE0 + k);
    if (hooked > 0)
        api->on_text(edit_on_text, NULL), api->on_key(edit_on_key, NULL), api->on_pad(edit_on_pad, NULL),
        api->on_frame(edit_on_frame, NULL);
    if (hooked < 0) rt_log("plugins: menu: a function of the settings screen could not be hooked");
    return hooked > 0;
}

int plugin_menu_open(const Nx2Menu *menu) {
    if (active && hooked > 0) {   // a page is open: this one replaces it once the game has closed that one
        Set *s = set_make(menu);
        if (!s) return 0;
        // (a text row's entry ends with the page: h_dtor, on the game's thread)
        set_free(next_set);
        next_set = s, next_event = menu->on_event, next_user = menu->user;
        __atomic_store_n(&reopen, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&close_req, 1, __ATOMIC_RELEASE);
        return 1;
    }
    if (!plugins_title_choosing()) {
        rt_log("plugins: menu refused: open_menu works inside a title menu button's on_select, with the button shown as "
               "the game's own entry");
        return 0;
    }
    if (!ready() || active) return 0;
    Set *s = set_make(menu);
    if (!s) return 0;
    set_free(set);
    set = s, on_event = menu->on_event, event_user = menu->user;
    __atomic_store_n(&armed_at, plat_now_ns() | 1, __ATOMIC_RELEASE);
    return 1;
}

// New texts and values for the rows of the open page, which must be of the same kinds. Any thread of a plugin's
// callbacks: the rows are kept, and the frame step puts them on the screen (menu_work); rows that were not shown yet
// when the next ones come are dropped for them.
int plugin_menu_update(const Nx2Menu *menu) {
    if (!active || !set || !menu || menu->row_count != (uint32_t)set->n || __atomic_load_n(&reopen, __ATOMIC_ACQUIRE)) return 0;
    Set *s = set_make(menu);
    if (!s) return 0;
    for (int i = 0; i < s->n; i++)
        if (s->row[i].game_kind != set->row[i].game_kind || s->row[i].nchoice != set->row[i].nchoice) {
            set_free(s);
            return 0;
        }
    plat_mutex_lock(&pending_lock);
    Set *old = pending_set;
    if (old && !s->focus) s->focus = old->focus;   // the cursor's move that came with the rows dropped still counts
    pending_set = s;
    plat_mutex_unlock(&pending_lock);
    set_free(old);
    return 1;
}

// The game's calc thread: the kept rows go on the screen.
static void apply_set(Set *s) {
    if (edit_row >= 0) edit_end(0);
    s->older = set, set = s;   // the game still points into the older texts until it has taken the new ones
    for (int i = 0; i < s->n; i++) {
        apply_row(i);
        if (row_layout[i] && s->row[i].game_kind != K_ACTION)
            call(B + G->layout_set_text, row_layout[i], (uint64_t)(uintptr_t)"T_Option_00", (uint64_t)(uintptr_t)&s->row[i].label, 1);
    }
    if (overlay) call(B + G->layout_set_text, overlay, (uint64_t)(uintptr_t)overlay_id, (uint64_t)(uintptr_t)&s->title, 1);
    // The caption under the list is the description of the row the cursor is on, which the game sets only when the
    // cursor comes to a row: the screen's own caption function is called again with the type it was last given, and
    // its text call gets the new description (h_text).
    if (caption_known) call(B + G->opt_caption, screen, caption_type, 0, 0);
    // The cursor moves at a later frame step, not inside the game's handling of a row's choice, where on_event runs.
    // A row that is disabled now is not a place for it.
    if (s->focus && row_ctl[s->focus - 1] && !s->row[s->focus - 1].disabled) focus_ask(s->focus - 1);
}

int plugin_menu_reopening(void) { return __atomic_load_n(&reopen, __ATOMIC_ACQUIRE) == 2; }

void plugin_menu_close(void) {
    __atomic_store_n(&armed_at, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&reopen, 0, __ATOMIC_RELEASE);
    if (active) __atomic_store_n(&close_req, 1, __ATOMIC_RELEASE);
}

int plugin_menu_armed(void) {
    uint64_t at = __atomic_load_n(&armed_at, __ATOMIC_ACQUIRE);
    return at && plat_now_ns() - at < ARM_NS;
}

// input.c's poll, through plugins_title_poll: close_menu is the game's own "back" on the page.
void plugin_menu_poll(uint64_t *buttons) {
    static unsigned polls;
    if (__atomic_load_n(&reopen, __ATOMIC_ACQUIRE) == 2) {
        // the page that replaces the closed one: the title menu's entry is pressed, on and off, until the game builds
        // its settings screen (h_build) or the armed page is given up
        static unsigned presses;
        if (!plugin_menu_armed()) {
            __atomic_store_n(&reopen, 0, __ATOMIC_RELEASE);
            emit(NX2_MENU_CLOSED, 0, 0);
        } else {
            *buttons &= ~(1ull << 1);
            if (presses++ / 8 % 2 == 1) *buttons |= 1ull;     // A
        }
        polls = 0;
        return;
    }
    if (!__atomic_load_n(&close_req, __ATOMIC_ACQUIRE) || !active) {
        polls = 0;
        return;
    }
    // "back" pressed for some reads and let go for as many, again and again until the page is gone: the sequence does
    // not look at the buttons in every frame (not while it answers the press that led to close_menu), and a button
    // held all along is pressed only once for it
    *buttons &= ~1ull;                                    // not A, which may still be held from that press
    if (polls++ / 8 % 2 == 0) *buttons |= 1ull << 1;      // B
}
