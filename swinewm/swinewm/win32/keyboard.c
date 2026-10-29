// Key bindings through a low-level keyboard hook (WH_KEYBOARD_LL).
//
// RegisterHotKey cannot claim most Win+key combinations (Explorer owns
// them), so like other Windows tiling WMs we look at every key press and
// swallow the ones that match a binding. The hook must return quickly or
// Windows silently removes it, so matching is a table lookup and the command
// itself runs later on the main thread via WM_SWINE_BINDING.
#include <stdlib.h>
#include "ipc.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/ipc-json.h"
#include "swinewm/platform.h"
#include "swinewm/sysinfo.h"
#include "win32.h"

// Unassigned virtual-key code used to "break" a lone Win/Alt press.
#define VK_SWINE_DUMMY 0xE8

static HHOOK hook = NULL;
static bool key_down[256];
static bool swallowed[256];
static bool suppress_logo_menu = false;
// A --release binding whose keys are down, run at the next key release
// (sway's held_binding): letting go of $mod before the key still runs it.
static struct swine_binding *held_release = NULL;
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
	char *event = ipc_json_event_binding(binding, "keyboard");
	platform_ipc_event((uint32_t)IPC_EVENT_BINDING, event);
	free(event);
	char *command = swine_strdup(binding->command);
	if (!PostMessageW(win32_shell_hwnd, WM_SWINE_BINDING, 0, (LPARAM)command)) {
		free(command);
	}
}

// Caps, Num and Scroll Lock (keyboard-state). GetKeyState only follows the
// keyboard of a thread that reads key messages, which swinewm's does not:
// read them once at the start, then toggle on each press the hook sees.
static bool locks[3];        // VK_CAPITAL, VK_NUMLOCK, VK_SCROLL
static bool lock_pressed[3];

static int lock_index(DWORD vk) {
	return vk == VK_CAPITAL ? 0 : vk == VK_NUMLOCK ? 1 : vk == VK_SCROLL ? 2 : -1;
}

bool sysinfo_keyboard_locks(bool *caps, bool *num, bool *scroll) {
	*caps = locks[0];
	*num = locks[1];
	*scroll = locks[2];
	return true;
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code != HC_ACTION) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lparam;
	DWORD vk = k->vkCode & 0xFF;
	bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
	int lock = lock_index(vk);
	if (lock >= 0) {
		if (down && !lock_pressed[lock]) {
			locks[lock] = !locks[lock];
		}
		lock_pressed[lock] = down;
	}

	// Our own (and other programs') synthetic input is never a binding.
	if (k->flags & LLKHF_INJECTED) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}

	bool repeat = down && key_down[vk];
	key_down[vk] = down;

	// While slurp's selection is shown it gets Escape, Space and Shift,
	// whichever window has the keyboard.
	if (win32_slurp_running && (vk == VK_ESCAPE || vk == VK_SPACE ||
			vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT)) {
		PostMessageW(win32_shell_hwnd, WM_SWINE_SLURP, vk, down);
		if (vk == VK_ESCAPE || vk == VK_SPACE) {
			return 1;
		}
	}

	if (!down && held_release) {
		struct swine_binding *binding = held_release;
		held_release = NULL;
		// It may have gone with a reloaded config.
		if (config && config->current_mode &&
				list_find(config->current_mode->keysym_bindings, binding) != -1) {
			post_binding(binding);
		}
	}

	uint32_t mod = modifier_for_vk(vk);
	if (mod && down && !repeat) {
		held_release = NULL;   // other modifiers: no longer that binding
	}
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
		struct swine_binding *release = keyboard_find_binding(mods, vk, true);
		held_release = release;   // another key cancels a held one
		if (release) {
			// Windows' own shortcuts (Win+X, ...) would act on the press.
			swallowed[vk] = true;
			if (lock >= 0 && !repeat) {
				locks[lock] = !locks[lock];   // swallowed: it does not toggle
			}
			if (mods & SWINE_MOD_LOGO) {
				suppress_logo_menu = true;
			}
			if (mods & SWINE_MOD_ALT) {
				suppress_alt_menu = true;
			}
			return 1;
		}
		struct swine_binding *binding = keyboard_find_binding(mods, vk, false);
		if (binding) {
			if (!(repeat && (binding->flags & BINDING_NOREPEAT))) {
				post_binding(binding);
			}
			swallowed[vk] = true;
			if (lock >= 0 && !repeat) {
				locks[lock] = !locks[lock];   // swallowed: it does not toggle
			}
			if (mods & SWINE_MOD_LOGO) {
				suppress_logo_menu = true;
			}
			if (mods & SWINE_MOD_ALT) {
				suppress_alt_menu = true;
			}
			return 1;
		}
	} else {
		if (swallowed[vk]) {
			// Swallow the release of a key whose press we swallowed.
			swallowed[vk] = false;
			return 1;
		}
	}
	return CallNextHookEx(hook, code, wparam, lparam);
}

bool win32_keyboard_init(void) {
	locks[0] = (GetKeyState(VK_CAPITAL) & 1) != 0;
	locks[1] = (GetKeyState(VK_NUMLOCK) & 1) != 0;
	locks[2] = (GetKeyState(VK_SCROLL) & 1) != 0;
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
