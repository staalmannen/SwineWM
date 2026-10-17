// Key bindings through a low-level keyboard hook (WH_KEYBOARD_LL).
//
// RegisterHotKey cannot claim most Win+key combinations (Explorer owns
// them), so like other Windows tiling WMs we look at every key press and
// swallow the ones that match a binding. The hook must return quickly or
// Windows silently removes it, so matching is a table lookup and the command
// itself runs later on the main thread via WM_SWINE_BINDING.
#include <stdlib.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "win32.h"

// Unassigned virtual-key code used to "break" a lone Win/Alt press.
#define VK_SWINE_DUMMY 0xE8

static HHOOK hook = NULL;
static bool key_down[256];
static bool swallowed[256];
static bool suppress_logo_menu = false;
static bool suppress_alt_menu = false;

static bool is_down(int vk) {
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static uint32_t modifier_for_vk(DWORD vk) {
	switch (vk) {
	case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
		return SWINE_MOD_SHIFT;
	case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
		return SWINE_MOD_CTRL;
	case VK_LMENU: case VK_RMENU: case VK_MENU:
		return SWINE_MOD_ALT;
	case VK_LWIN: case VK_RWIN:
		return SWINE_MOD_LOGO;
	default:
		return 0;
	}
}

// Asking the system each time (instead of tracking presses ourselves) keeps
// the state right even when a release was never delivered to us, e.g. after
// Win+L or Ctrl+Alt+Del switched to the secure desktop.
static uint32_t current_modifiers(void) {
	uint32_t mods = 0;
	if (is_down(VK_SHIFT)) mods |= SWINE_MOD_SHIFT;
	if (is_down(VK_CONTROL)) mods |= SWINE_MOD_CTRL;
	if (is_down(VK_MENU)) mods |= SWINE_MOD_ALT;
	if (is_down(VK_LWIN) || is_down(VK_RWIN)) mods |= SWINE_MOD_LOGO;
	return mods;
}

uint32_t win32_keyboard_modifiers(void) {
	return current_modifiers();
}

void win32_keyboard_used_modifiers(uint32_t mods) {
	if (mods & SWINE_MOD_LOGO) {
		suppress_logo_menu = true;
	}
	if (mods & SWINE_MOD_ALT) {
		suppress_alt_menu = true;
	}
}

static void send_dummy_key(void) {
	INPUT inputs[2] = {0};
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = VK_SWINE_DUMMY;
	inputs[1] = inputs[0];
	inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(2, inputs, sizeof(INPUT));
}

static void post_binding(struct swine_binding *binding) {
	char *command = swine_strdup(binding->command);
	if (!PostMessageW(win32_shell_hwnd, WM_SWINE_BINDING, 0, (LPARAM)command)) {
		free(command);
	}
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code != HC_ACTION) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lparam;
	DWORD vk = k->vkCode & 0xFF;
	bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;

	// Our own (and other programs') synthetic input is never a binding.
	if (k->flags & LLKHF_INJECTED) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	bool repeat = down && key_down[vk];
	key_down[vk] = down;

	uint32_t mod = modifier_for_vk(vk);
	if (mod) {
		if (!down && mod == SWINE_MOD_LOGO && suppress_logo_menu) {
			// Win was used for a binding: stop the Start menu from opening.
			suppress_logo_menu = false;
			send_dummy_key();
		} else if (!down && mod == SWINE_MOD_ALT && suppress_alt_menu) {
			// Same for Alt and the focused window's menu bar.
			suppress_alt_menu = false;
			send_dummy_key();
		}
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	uint32_t mods = current_modifiers();
	if (down) {
		struct swine_binding *binding = keyboard_find_binding(mods, vk, false);
		if (binding) {
			if (!(repeat && (binding->flags & BINDING_NOREPEAT))) {
				post_binding(binding);
			}
			swallowed[vk] = true;
			if (mods & SWINE_MOD_LOGO) {
				suppress_logo_menu = true;
			}
			if (mods & SWINE_MOD_ALT) {
				suppress_alt_menu = true;
			}
			return 1;
		}
	} else {
		struct swine_binding *binding = keyboard_find_binding(mods, vk, true);
		if (binding) {
			post_binding(binding);
			swallowed[vk] = false;
			return 1;
		}
		if (swallowed[vk]) {
			// Swallow the release of a key whose press we swallowed.
			swallowed[vk] = false;
			return 1;
		}
	}
	return CallNextHookEx(hook, code, wparam, lparam);
}

bool win32_keyboard_init(void) {
	hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc,
		GetModuleHandleW(NULL), 0);
	if (!hook) {
		char *msg = win32_error_string(GetLastError());
		swine_log(SWINE_ERROR, "Unable to install keyboard hook: %s", msg);
		free(msg);
		return false;
	}
	return true;
}

void win32_keyboard_finish(void) {
	if (hook) {
		UnhookWindowsHookEx(hook);
		hook = NULL;
	}
}
