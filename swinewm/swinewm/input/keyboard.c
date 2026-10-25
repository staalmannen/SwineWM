#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"

struct modifier_name {
	const char *name;
	uint32_t mask;
};

// First entry per mask is the canonical name used when printing.
static const struct modifier_name modifier_names[] = {
	{ "Shift", SWINE_MOD_SHIFT },
	{ "Lock", SWINE_MOD_CAPS },
	{ "Caps", SWINE_MOD_CAPS },
	{ "Ctrl", SWINE_MOD_CTRL },
	{ "Control", SWINE_MOD_CTRL },
	{ "Mod1", SWINE_MOD_ALT },
	{ "Alt", SWINE_MOD_ALT },
	{ "Mod2", SWINE_MOD_MOD2 },
	{ "Mod3", SWINE_MOD_MOD3 },
	{ "Mod4", SWINE_MOD_LOGO },
	{ "Super", SWINE_MOD_LOGO },
	{ "Logo", SWINE_MOD_LOGO },
	{ "Win", SWINE_MOD_LOGO },
	{ "Mod5", SWINE_MOD_MOD5 },
};

uint32_t get_modifier_mask_by_name(const char *name) {
	for (size_t i = 0; i < sizeof(modifier_names) / sizeof(modifier_names[0]); ++i) {
		if (swine_strcasecmp(modifier_names[i].name, name) == 0) {
			return modifier_names[i].mask;
		}
	}
	return 0;
}

const char *get_modifier_name_by_mask(uint32_t mask) {
	for (size_t i = 0; i < sizeof(modifier_names) / sizeof(modifier_names[0]); ++i) {
		if (modifier_names[i].mask == mask) {
			return modifier_names[i].name;
		}
	}
	return NULL;
}

struct key_name {
	const char *name;
	uint32_t vk;
};

// xkb keysym names -> Windows virtual-key codes. Punctuation keys use the
// VK_OEM_* codes, which follow the physical US layout; layout-aware lookup
// (VkKeyScanEx) is planned in the Win32 layer.
static const struct key_name key_names[] = {
	{ "BackSpace", 0x08 }, { "Tab", 0x09 }, { "Return", 0x0D },
	{ "KP_Enter", 0x0D }, { "Pause", 0x13 }, { "Caps_Lock", 0x14 },
	{ "Escape", 0x1B }, { "space", 0x20 }, { "Prior", 0x21 },
	{ "Page_Up", 0x21 }, { "Next", 0x22 }, { "Page_Down", 0x22 },
	{ "End", 0x23 }, { "Home", 0x24 }, { "Left", 0x25 }, { "Up", 0x26 },
	{ "Right", 0x27 }, { "Down", 0x28 }, { "Print", 0x2C },
	{ "Insert", 0x2D }, { "Delete", 0x2E }, { "Menu", 0x5D },
	{ "KP_0", 0x60 }, { "KP_1", 0x61 }, { "KP_2", 0x62 }, { "KP_3", 0x63 },
	{ "KP_4", 0x64 }, { "KP_5", 0x65 }, { "KP_6", 0x66 }, { "KP_7", 0x67 },
	{ "KP_8", 0x68 }, { "KP_9", 0x69 }, { "KP_Multiply", 0x6A },
	{ "KP_Add", 0x6B }, { "KP_Subtract", 0x6D }, { "KP_Decimal", 0x6E },
	{ "KP_Divide", 0x6F }, { "Num_Lock", 0x90 }, { "Scroll_Lock", 0x91 },
	{ "semicolon", 0xBA }, { "equal", 0xBB }, { "plus", 0xBB },
	{ "comma", 0xBC }, { "minus", 0xBD }, { "period", 0xBE },
	{ "slash", 0xBF }, { "grave", 0xC0 }, { "bracketleft", 0xDB },
	{ "backslash", 0xDC }, { "bracketright", 0xDD }, { "apostrophe", 0xDE },
	{ "XF86Back", 0xA6 }, { "XF86Forward", 0xA7 }, { "XF86Refresh", 0xA8 },
	{ "XF86Stop", 0xA9 }, { "XF86Search", 0xAA }, { "XF86Favorites", 0xAB },
	{ "XF86HomePage", 0xAC }, { "XF86AudioMute", 0xAD },
	{ "XF86AudioLowerVolume", 0xAE }, { "XF86AudioRaiseVolume", 0xAF },
	{ "XF86AudioNext", 0xB0 }, { "XF86AudioPrev", 0xB1 },
	{ "XF86AudioStop", 0xB2 }, { "XF86AudioPlay", 0xB3 },
	{ "XF86AudioPause", 0xB3 }, { "XF86Mail", 0xB4 },
	{ "XF86AudioMedia", 0xB5 }, { "XF86Calculator", 0xB7 },
};

#define KEY_NAME_COUNT (sizeof(key_names) / sizeof(key_names[0]))

bool keyname_to_keycode(const char *name, uint32_t *keycode) {
	size_t len = strlen(name);
	if (len == 1) {
		char c = name[0];
		if (c >= 'a' && c <= 'z') {
			*keycode = (uint32_t)(c - 'a' + 'A');
			return true;
		}
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			*keycode = (uint32_t)c;
			return true;
		}
	}
	if ((name[0] == 'F' || name[0] == 'f') && len >= 2 && len <= 3) {
		int n = 0;
		if (sscanf(name + 1, "%d", &n) == 1 && n >= 1 && n <= 24) {
			*keycode = 0x70 + (uint32_t)(n - 1);
			return true;
		}
	}
	for (size_t i = 0; i < KEY_NAME_COUNT; ++i) {
		if (swine_strcasecmp(key_names[i].name, name) == 0) {
			*keycode = key_names[i].vk;
			return true;
		}
	}
	return false;
}

const char *keycode_to_name(uint32_t keycode) {
	static char buf[8];
	if ((keycode >= 'A' && keycode <= 'Z')) {
		buf[0] = (char)(keycode - 'A' + 'a');
		buf[1] = '\0';
		return buf;
	}
	if (keycode >= '0' && keycode <= '9') {
		buf[0] = (char)keycode;
		buf[1] = '\0';
		return buf;
	}
	if (keycode >= 0x70 && keycode <= 0x87) {
		snprintf(buf, sizeof(buf), "F%u", keycode - 0x70 + 1);
		return buf;
	}
	for (size_t i = 0; i < KEY_NAME_COUNT; ++i) {
		if (key_names[i].vk == keycode) {
			return key_names[i].name;
		}
	}
	return NULL;
}

struct swine_binding *keyboard_find_binding(uint32_t modifiers,
		uint32_t keycode, bool release) {
	if (!config || !config->current_mode) {
		return NULL;
	}
	// Caps Lock and Num Lock state never prevent a match.
	modifiers &= ~(uint32_t)(SWINE_MOD_CAPS | SWINE_MOD_MOD2);
	list_t *bindings = config->current_mode->keysym_bindings;
	for (int i = 0; i < bindings->length; ++i) {
		struct swine_binding *binding = bindings->items[i];
		bool binding_release = (binding->flags & BINDING_RELEASE) != 0;
		if (binding->keycode == keycode && binding->modifiers == modifiers &&
				binding_release == release) {
			return binding;
		}
	}
	return NULL;
}

static struct {
	list_t *layouts;   // struct platform_keyboard_layout *
	int active;
	bool loaded;
} kb_layouts = { NULL, -1, false };

void keyboard_layouts_free(list_t *layouts) {
	if (!layouts) {
		return;
	}
	for (int i = 0; i < layouts->length; ++i) {
		struct platform_keyboard_layout *l = layouts->items[i];
		free(l->name);
		free(l->short_name);
		free(l->description);
		free(l->variant);
		free(l);
	}
	list_free(layouts);
}

static bool same_layouts(list_t *a, list_t *b) {
	if (!a || !b || a->length != b->length) {
		return false;
	}
	for (int i = 0; i < a->length; ++i) {
		struct platform_keyboard_layout *x = a->items[i], *y = b->items[i];
		if (strcmp(x->name, y->name) != 0) {
			return false;
		}
	}
	return true;
}

bool keyboard_layouts_refresh(void) {
	list_t *layouts = create_list();
	int active = platform_keyboard_layouts(layouts);
	if (active >= layouts->length) {
		active = -1;
	}
	bool changed = !kb_layouts.loaded || active != kb_layouts.active ||
		!same_layouts(layouts, kb_layouts.layouts);
	keyboard_layouts_free(kb_layouts.layouts);
	kb_layouts.layouts = layouts;
	kb_layouts.active = active;
	bool first = !kb_layouts.loaded;
	kb_layouts.loaded = true;
	if (changed && !first) {
		// sway's ipc_event_input(..., "xkb_layout")
		char *event = ipc_json_event_input("xkb_layout");
		platform_ipc_event((uint32_t)IPC_EVENT_INPUT, event);
		free(event);
		platform_ui_refresh();
	}
	return changed && !first;
}

list_t *keyboard_layouts_get(void) {
	if (!kb_layouts.loaded) {
		keyboard_layouts_refresh();
	}
	return kb_layouts.layouts;
}

int keyboard_layouts_active(void) {
	if (!kb_layouts.loaded) {
		keyboard_layouts_refresh();
	}
	return kb_layouts.active;
}

void keyboard_layouts_finish(void) {
	keyboard_layouts_free(kb_layouts.layouts);
	kb_layouts.layouts = NULL;
	kb_layouts.active = -1;
	kb_layouts.loaded = false;
}
