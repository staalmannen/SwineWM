#ifndef _SWINE_CURSOR_H
#define _SWINE_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

struct swine_container;

// Mouse buttons use Linux event codes, as in sway, so that "bindsym
// button3" and "bindsym BTN_RIGHT" mean the same button.
// Scrolling is reported as the extra codes sway uses for buttons 4-7.
#define SWINE_BTN_LEFT 0x110
#define SWINE_BTN_RIGHT 0x111
#define SWINE_BTN_MIDDLE 0x112
#define SWINE_BTN_SIDE 0x113      // "back" (XBUTTON1 on Windows)
#define SWINE_BTN_EXTRA 0x114     // "forward" (XBUTTON2)
#define SWINE_BTN_FORWARD 0x115
#define SWINE_BTN_BACK 0x116
#define SWINE_BTN_TASK 0x117
#define SWINE_SCROLL_UP 0x300     // KEY_MAX + 1, as SWAY_SCROLL_UP
#define SWINE_SCROLL_DOWN 0x301
#define SWINE_SCROLL_LEFT 0x302
#define SWINE_SCROLL_RIGHT 0x303

// What the platform found under the cursor.
enum cursor_hit {
	HIT_NONE,       // a window swinewm does not manage, or swinewm's bar
	HIT_WORKSPACE,  // the desktop: an empty workspace or the gaps
	HIT_CONTENTS,   // a managed window (con is set)
	HIT_TITLEBAR,   // a title bar or tab drawn by swinewm (con is set)
};

// "button1".."button9", "BTN_LEFT", ... Returns 0 if name is not a mouse
// button; *error (caller frees) is set if it looks like one but is invalid.
uint32_t get_mouse_button(const char *name, char **error);
const char *get_mouse_button_name(uint32_t button);

// Pointer input, ported from sway's seatop_default.c and the seatop_move_*
// and seatop_resize_* operations. Coordinates are virtual-desktop pixels.
// The platform reports every event; for a press, true means swinewm used
// it (a binding, or the start of a move or resize), and the platform must
// keep it, and its release, from the application.
void cursor_motion(int x, int y, enum cursor_hit hit,
		struct swine_container *con);
bool cursor_button(int x, int y, enum cursor_hit hit,
		struct swine_container *con, uint32_t button, bool pressed,
		uint32_t modifiers);
// One step of the wheel (SWINE_SCROLL_*). True if a binding used it.
bool cursor_axis(int x, int y, enum cursor_hit hit,
		struct swine_container *con, uint32_t button, uint32_t modifiers);
// True while a move or resize started by the mouse is in progress.
bool cursor_in_operation(void);
// A container is going away: ends an operation that uses it.
void cursor_container_destroyed(struct swine_container *con);
// Forgets everything (tests, config reload).
void cursor_reset(void);

// mouse_warping: moves the pointer to the focused container or output after
// a focus command. Requested by seat_consider_warp_to_focus and done by the
// next transaction_commit_dirty, once the layout is final.
void cursor_request_warp(bool force_container);
void cursor_apply_warp(void);

#endif
