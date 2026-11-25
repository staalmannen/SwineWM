// Key bindings through a low-level keyboard hook (WH_KEYBOARD_LL).
//
// RegisterHotKey cannot claim most Win+key combinations (Explorer owns
// them), so like other Windows tiling WMs we look at every key press and
// swallow the ones that match a binding. The hook must return quickly or
// Windows silently removes it, so matching is a table lookup and the command
// itself runs later on the main thread via WM_SWINE_BINDING.
//
// The hook runs on a thread of its own: the main thread can be busy for a
// while (a game changing the display mode makes it rearrange everything),
// and a hook that misses Windows' timeout a few times is removed without
// notice, after which no binding works. The config the hook reads is
// guarded by a lock the main thread holds while it runs commands or loads
// the config (platform_config_write_begin); the hook only tries to take it
// and lets the key through if it cannot in time. It is also installed
// again now and then, and after display changes, in case it was removed
// all the same.
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

static HHOOK hook = NULL;          // the hook thread's
static HANDLE hook_thread = NULL;
static DWORD hook_thread_id = 0;
static SRWLOCK config_lock = SRWLOCK_INIT;
static int config_write_depth = 0;  // main thread

// Installs the hook again (the thread's message): Windows may have removed
// it after timeouts.
#define WM_SWINE_REHOOK (WM_APP + 1)
#define REHOOK_INTERVAL_MS 60000
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

void platform_config_write_begin(void) {
	if (config_write_depth++ == 0) {
		AcquireSRWLockExclusive(&config_lock);
	}
}

void platform_config_write_end(void) {
	if (--config_write_depth == 0) {
		ReleaseSRWLockExclusive(&config_lock);
	}
}

// The command and the IPC event go to the main thread, which publishes the
// event and runs the command (win32_keyboard_binding_message).
struct binding_message {
	char *command;
	char *event;
};

static void post_binding(struct swine_binding *binding) {
	struct binding_message *m = calloc(1, sizeof(*m));
	m->event = ipc_json_event_binding(binding, "keyboard");
	m->command = swine_strdup(binding->command);
	if (!PostMessageW(win32_shell_hwnd, WM_SWINE_BINDING, 0, (LPARAM)m)) {
		free(m->event);
		free(m->command);
		free(m);
	}
}

void win32_keyboard_binding_message(LPARAM lparam) {
	struct binding_message *m = (struct binding_message *)lparam;
	platform_ipc_event((uint32_t)IPC_EVENT_BINDING, m->event);
	win32_run_command(m->command);
	free(m->event);
	free(m->command);
	free(m);
}

// The config for reading, or false if the main thread keeps it longer than
// the hook may wait (well inside Windows' timeout).
static bool lock_config(void) {
	for (int i = 0; i < 100; ++i) {
		if (TryAcquireSRWLockShared(&config_lock)) {
			return true;
		}
		Sleep(2);
	}
	return false;
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

// The bindings' part of the hook, with the config locked: 1 swallows the
// key, 0 passes it on.
static LRESULT handle_key(DWORD vk, bool down, bool repeat, int lock) {
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
		return 0;
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
	return 0;
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

	if (!lock_config()) {
		return CallNextHookEx(hook, code, wparam, lparam);
	}
	LRESULT result = handle_key(vk, down, repeat, lock);
	ReleaseSRWLockShared(&config_lock);
	return result ? 1 : CallNextHookEx(hook, code, wparam, lparam);
}

static HHOOK install_hook(void) {
	return SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc,
		GetModuleHandleW(NULL), 0);
}

struct hook_start {
	HANDLE ready;
	DWORD error;   // 0: installed
};

static DWORD WINAPI hook_thread_main(LPVOID param) {
	struct hook_start *start = param;
	// A message queue before anyone posts to it.
	MSG msg;
	PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
	// Keys wait on this thread: let it run before others.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	hook = install_hook();
	start->error = hook ? 0 : GetLastError();
	SetEvent(start->ready);   // start is gone after this
	if (!hook) {
		return 1;
	}
	UINT_PTR timer = SetTimer(NULL, 0, REHOOK_INTERVAL_MS, NULL);
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_SWINE_REHOOK || (msg.message == WM_TIMER &&
				msg.wParam == timer)) {
			// Installed again it is first in the chain; a hook Windows
			// removed after timeouts works again.
			HHOOK fresh = install_hook();
			if (fresh) {
				UnhookWindowsHookEx(hook);
				hook = fresh;
			}
			continue;
		}
		DispatchMessageW(&msg);
	}
	KillTimer(NULL, timer);
	UnhookWindowsHookEx(hook);
	hook = NULL;
	return 0;
}

bool win32_keyboard_init(void) {
	locks[0] = (GetKeyState(VK_CAPITAL) & 1) != 0;
	locks[1] = (GetKeyState(VK_NUMLOCK) & 1) != 0;
	locks[2] = (GetKeyState(VK_SCROLL) & 1) != 0;
	struct hook_start start = {CreateEventW(NULL, TRUE, FALSE, NULL), 0};
	hook_thread = start.ready ? CreateThread(NULL, 0, hook_thread_main, &start,
		0, &hook_thread_id) : NULL;
	if (hook_thread) {
		WaitForSingleObject(start.ready, INFINITE);
	} else {
		start.error = GetLastError();
	}
	if (start.ready) {
		CloseHandle(start.ready);
	}
	if (!hook_thread || start.error) {
		char *msg = win32_error_string(start.error);
		swine_log(SWINE_ERROR, "Unable to install keyboard hook: %s", msg);
		free(msg);
		if (hook_thread) {
			WaitForSingleObject(hook_thread, INFINITE);
			CloseHandle(hook_thread);
			hook_thread = NULL;
		}
		return false;
	}
	return true;
}

void win32_keyboard_rehook(void) {
	if (hook_thread) {
		PostThreadMessageW(hook_thread_id, WM_SWINE_REHOOK, 0, 0);
	}
}

void win32_keyboard_finish(void) {
	if (hook_thread) {
		PostThreadMessageW(hook_thread_id, WM_QUIT, 0, 0);
		WaitForSingleObject(hook_thread, 2000);
		CloseHandle(hook_thread);
		hook_thread = NULL;
	}
}
