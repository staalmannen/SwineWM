// Window decorations through DWM and window styles (sway's borders, title
// bars and opacity; see doc/research/glazewm.md for where the ideas come
// from):
//
// - the border is the window's own DWM border (DWMWA_BORDER_COLOR, Windows
//   11), in the client.* child_border colour, or none;
// - "border normal" keeps the window's own title bar, coloured with the
//   client.* background and text (DWMWA_CAPTION_COLOR, DWMWA_TEXT_COLOR);
//   "pixel" and "none" take WS_DLGFRAME away so there is no title bar;
// - tiled windows get square corners (DWMWA_WINDOW_CORNER_PREFERENCE);
// - "opacity" makes the window layered (WS_EX_LAYERED, LWA_ALPHA).
//
// Changing another application's styles sends it messages and waits, so a
// hung application would freeze swinewm: all of this happens on a worker
// thread. The worker remembers what it changed per window, so that exit,
// the crash handler and --recover (through the recovery file) undo it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "swinewm/platform.h"
#include "swinewm/tree/view.h"
#include "win32.h"
#include <dwmapi.h>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#define COLOR_DEFAULT 0xFFFFFFFF   // DWMWA_COLOR_DEFAULT
#define COLOR_NONE 0xFFFFFFFE      // DWMWA_COLOR_NONE
#define CORNER_DEFAULT 0           // DWMWCP_DEFAULT
#define CORNER_SQUARE 1            // DWMWCP_DONOTROUND

struct decor_entry {
	HWND hwnd;
	DWORD pid;
	unsigned flags;   // DECOR_*
};

struct decor_job {
	HWND hwnd;
	DWORD pid;
	bool restore;
	bool restore_all;
	unsigned restore_flags;   // for windows from the recovery file
	struct view_decoration deco;
};

static CRITICAL_SECTION lock;
static HANDLE wake = NULL;    // auto-reset: jobs were queued
static HANDLE idle = NULL;    // manual-reset: nothing queued or running
static HANDLE thread = NULL;
static struct decor_job *jobs;
static int job_count, job_cap;
static struct decor_entry *entries;
static int entry_count, entry_cap;
// Once restore-all is queued, shutdown must not accept later decorations.
static bool restoring_all;

static COLORREF to_colorref(uint32_t rgba) {
	return RGB((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
}

static void set_dwm(HWND hwnd, DWORD attribute, DWORD value) {
	// Fails on Windows 10 for the Windows 11 attributes: nothing to do then.
	DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
}

static void frame_changed(HWND hwnd) {
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
		SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool is_same_process(HWND hwnd, DWORD pid) {
	DWORD owner = 0;
	return pid && IsWindow(hwnd) &&
		GetWindowThreadProcessId(hwnd, &owner) && owner == pid;
}

// Runs on the worker. Returns the new flags.
static unsigned apply(HWND hwnd, DWORD pid, unsigned flags,
		const struct view_decoration *d) {
	if (!is_same_process(hwnd, pid)) {
		return 0;
	}
	set_dwm(hwnd, DWMWA_BORDER_COLOR, (d->border_color & 0xFF) ?
		to_colorref(d->border_color) : COLOR_NONE);
	bool colored_title = d->titlebar && d->title_color;
	set_dwm(hwnd, DWMWA_CAPTION_COLOR, colored_title ?
		to_colorref(d->title_color) : COLOR_DEFAULT);
	set_dwm(hwnd, DWMWA_TEXT_COLOR, colored_title ?
		to_colorref(d->title_text_color) : COLOR_DEFAULT);
	set_dwm(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
		d->square_corners ? CORNER_SQUARE : CORNER_DEFAULT);
	flags |= DECOR_DWM;

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if (!d->titlebar && !(flags & DECOR_CAPTION) &&
			(style & WS_CAPTION) == WS_CAPTION) {
		SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~(LONG_PTR)WS_DLGFRAME);
		frame_changed(hwnd);
		flags |= DECOR_CAPTION;
	} else if (d->titlebar && (flags & DECOR_CAPTION)) {
		SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_DLGFRAME);
		frame_changed(hwnd);
		flags &= ~(unsigned)DECOR_CAPTION;
	}

	LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (d->alpha < 1.0f) {
		if (!(ex & WS_EX_LAYERED)) {
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
			flags |= DECOR_LAYERED;
		}
		// A window that is layered by itself keeps its own transparency.
		if (flags & DECOR_LAYERED) {
			SetLayeredWindowAttributes(hwnd, 0,
				(BYTE)(d->alpha * 255.0f + 0.5f), LWA_ALPHA);
		}
	} else if (flags & DECOR_LAYERED) {
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_LAYERED);
		flags &= ~(unsigned)DECOR_LAYERED;
	}
	return flags;
}

static void restore(HWND hwnd, DWORD pid, unsigned flags) {
	if (!is_same_process(hwnd, pid)) {
		return;
	}
	if (flags & DECOR_DWM) {
		set_dwm(hwnd, DWMWA_BORDER_COLOR, COLOR_DEFAULT);
		set_dwm(hwnd, DWMWA_CAPTION_COLOR, COLOR_DEFAULT);
		set_dwm(hwnd, DWMWA_TEXT_COLOR, COLOR_DEFAULT);
		set_dwm(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, CORNER_DEFAULT);
	}
	if (flags & DECOR_CAPTION) {
		LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_DLGFRAME);
		frame_changed(hwnd);
	}
	if (flags & DECOR_LAYERED) {
		LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_LAYERED);
	}
}

// With the lock held.
static int find_entry(HWND hwnd, DWORD pid) {
	for (int i = 0; i < entry_count; ++i) {
		if (entries[i].hwnd == hwnd && entries[i].pid == pid) {
			return i;
		}
	}
	return -1;
}

static void restore_all_entries(void) {
	bool changed = false;
	for (;;) {
		EnterCriticalSection(&lock);
		if (entry_count == 0) {
			LeaveCriticalSection(&lock);
			break;
		}
		struct decor_entry entry = entries[entry_count - 1];
		LeaveCriticalSection(&lock);

		restore(entry.hwnd, entry.pid, entry.flags);

		EnterCriticalSection(&lock);
		int index = find_entry(entry.hwnd, entry.pid);
		if (index >= 0) {
			entries[index] = entries[--entry_count];
			changed = true;
		}
		LeaveCriticalSection(&lock);
	}
	if (changed && win32_shell_hwnd) {
		PostMessageW(win32_shell_hwnd, WM_SWINE_SAVE_HIDDEN, 0, 0);
	}
}

static DWORD WINAPI worker(LPVOID param) {
	(void)param;
	while (WaitForSingleObject(wake, INFINITE) == WAIT_OBJECT_0) {
		for (;;) {
			EnterCriticalSection(&lock);
			if (job_count == 0) {
				SetEvent(idle);
				LeaveCriticalSection(&lock);
				break;
			}
			struct decor_job job = jobs[0];
			memmove(jobs, jobs + 1, sizeof(*jobs) * (size_t)(job_count - 1));
			job_count--;
			if (job.restore_all) {
				LeaveCriticalSection(&lock);
				restore_all_entries();
				continue;
			}
			int index = find_entry(job.hwnd, job.pid);
			unsigned flags = index >= 0 ? entries[index].flags :
				job.restore_flags;
			LeaveCriticalSection(&lock);

			unsigned new_flags = 0;
			if (job.restore) {
				restore(job.hwnd, job.pid, flags);
			} else {
				new_flags = apply(job.hwnd, job.pid, flags, &job.deco);
			}

			EnterCriticalSection(&lock);
			index = find_entry(job.hwnd, job.pid);
			bool changed = (index >= 0 ? entries[index].flags : 0) != new_flags;
			if (new_flags && index < 0) {
				if (entry_count == entry_cap) {
					entry_cap = entry_cap ? entry_cap * 2 : 32;
					entries = realloc(entries, sizeof(*entries) * (size_t)entry_cap);
				}
				entries[entry_count++] = (struct decor_entry){
					job.hwnd, job.pid, new_flags};
			} else if (new_flags) {
				entries[index].flags = new_flags;
			} else if (index >= 0) {
				entries[index] = entries[--entry_count];
			}
			LeaveCriticalSection(&lock);
			if (changed && win32_shell_hwnd) {
				// What --recover has to undo changed.
				PostMessageW(win32_shell_hwnd, WM_SWINE_SAVE_HIDDEN, 0, 0);
			}
		}
	}
	return 0;
}

static bool ensure_worker(void) {
	if (thread) {
		return true;
	}
	InitializeCriticalSection(&lock);
	wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	idle = CreateEventW(NULL, TRUE, TRUE, NULL);
	thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread) {
		swine_log(SWINE_ERROR, "Unable to start the decoration thread");
		return false;
	}
	return true;
}

static void queue(const struct decor_job *job) {
	if (!ensure_worker()) {
		return;
	}
	EnterCriticalSection(&lock);
	if (restoring_all) {
		LeaveCriticalSection(&lock);
		return;
	}
	// The latest wish for a window replaces an older one still waiting.
	int i = 0;
	for (; i < job_count; ++i) {
		if (jobs[i].hwnd == job->hwnd && jobs[i].pid == job->pid) {
			break;
		}
	}
	if (i == job_count) {
		if (job_count == job_cap) {
			job_cap = job_cap ? job_cap * 2 : 32;
			jobs = realloc(jobs, sizeof(*jobs) * (size_t)job_cap);
		}
		job_count++;
	}
	jobs[i] = *job;
	ResetEvent(idle);
	LeaveCriticalSection(&lock);
	SetEvent(wake);
}

void platform_view_set_decoration(struct swine_view *view,
		const struct view_decoration *deco) {
	struct decor_job job = {0};
	job.hwnd = view->handle;
	job.pid = (DWORD)view->pid;
	job.deco = *deco;
	queue(&job);
}

void win32_decor_forget(HWND hwnd, DWORD pid) {
	if (!thread) {
		return;
	}
	struct decor_job job = {0};
	job.hwnd = hwnd;
	job.pid = pid;
	job.restore = true;
	queue(&job);
}

void win32_decor_restore(HWND hwnd, DWORD pid, unsigned flags) {
	struct decor_job job = {0};
	job.hwnd = hwnd;
	job.pid = pid;
	job.restore = true;
	job.restore_flags = flags;
	queue(&job);
}

void win32_decor_restore_all(DWORD timeout_ms) {
	if (!thread) {
		return;
	}
	EnterCriticalSection(&lock);
	// Let the worker finish any active job before restoring its recorded state.
	restoring_all = true;
	job_count = 0;
	if (job_count == job_cap) {
		job_cap = job_cap ? job_cap * 2 : 32;
		jobs = realloc(jobs, sizeof(*jobs) * (size_t)job_cap);
	}
	jobs[job_count++] = (struct decor_job){.restore_all = true};
	ResetEvent(idle);
	LeaveCriticalSection(&lock);
	SetEvent(wake);
	win32_decor_wait(timeout_ms);
}

void win32_decor_wait(DWORD timeout_ms) {
	if (thread) {
		WaitForSingleObject(idle, timeout_ms);
	}
}

void win32_decor_save(FILE *f) {
	if (!thread) {
		return;
	}
	EnterCriticalSection(&lock);
	for (int i = 0; i < entry_count; ++i) {
		fprintf(f, "%llx %lu d %x\n", (unsigned long long)(size_t)entries[i].hwnd,
			(unsigned long)entries[i].pid, entries[i].flags);
	}
	LeaveCriticalSection(&lock);
}
