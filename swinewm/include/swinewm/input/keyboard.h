#ifndef _SWINE_KEYBOARD_H
#define _SWINE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

// Modifier bits use the same values as wlroots' enum wlr_keyboard_modifier.
enum swine_modifier {
	SWINE_MOD_SHIFT = 1 << 0,
	SWINE_MOD_CAPS = 1 << 1,
	SWINE_MOD_CTRL = 1 << 2,
	SWINE_MOD_ALT = 1 << 3,   // Mod1
	SWINE_MOD_MOD2 = 1 << 4,
	SWINE_MOD_MOD3 = 1 << 5,
	SWINE_MOD_LOGO = 1 << 6,  // Mod4, the Windows key
	SWINE_MOD_MOD5 = 1 << 7,
};

// Key codes are Windows virtual-key codes. They are plain integers, so the
// name tables and binding matching stay portable and unit-testable.
uint32_t get_modifier_mask_by_name(const char *name);
const char *get_modifier_name_by_mask(uint32_t mask);
// Accepts xkb keysym names as used in sway configs ("Return", "h", "Left",
// "XF86AudioMute", ...). Returns false for unknown names.
bool keyname_to_keycode(const char *name, uint32_t *keycode);
const char *keycode_to_name(uint32_t keycode);

struct swine_binding;
// Looks up a binding in the current mode. Returns NULL if none matches.
struct swine_binding *keyboard_find_binding(uint32_t modifiers,
		uint32_t keycode, bool release);

#endif
