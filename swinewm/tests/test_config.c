#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

#ifndef SWINEWM_SOURCE_DIR
#define SWINEWM_SOURCE_DIR "."
#endif

static bool read_inactive(const char *text) {
	config->active = false;
	config->reading = true;
	bool ok = read_config_text(text, "test");
	config->reading = false;
	config->active = true;
	return ok;
}

static void test_variables_and_bindings(void) {
	fake_setup();
	const char *text =
		"# comment\n"
		"set $mod Mod4\n"
		"set $mod_shift Mod4+Shift\n"
		"set $term wt.exe\n"
		"bindsym $mod+Return exec $term\n"
		"bindsym $mod_shift+q kill\n"
		"bindsym $mod+1 workspace \\\n"
		"    number 1\n";
	CHECK(read_inactive(text));
	struct swine_binding *b = keyboard_find_binding(SWINE_MOD_LOGO, 0x0D, false);
	CHECK(b != NULL);
	if (b) {
		CHECK_STR(b->command, "exec wt.exe");
	}
	b = keyboard_find_binding(SWINE_MOD_LOGO | SWINE_MOD_SHIFT, 'Q', false);
	CHECK(b != NULL);
	if (b) {
		CHECK_STR(b->command, "kill");
	}
	b = keyboard_find_binding(SWINE_MOD_LOGO, '1', false);
	CHECK(b != NULL);
	if (b) {
		CHECK_STR(b->command, "workspace number 1");
	}
	// Caps Lock does not prevent a match; extra modifiers do.
	CHECK(keyboard_find_binding(SWINE_MOD_LOGO | SWINE_MOD_CAPS, 0x0D, false));
	CHECK(!keyboard_find_binding(SWINE_MOD_LOGO | SWINE_MOD_CTRL, 0x0D, false));
	fake_teardown();
}

static void test_modes_and_blocks(void) {
	fake_setup();
	const char *text =
		"mode \"resize\" {\n"
		"    bindsym Left resize shrink width 10px\n"
		"    bindsym Return mode \"default\"\n"
		"}\n"
		"bindsym Mod4+r mode \"resize\"\n"
		"bar\n"
		"{\n"
		"    position top\n"
		"    colors {\n"
		"        statusline #ffffff\n"
		"    }\n"
		"}\n";
	CHECK(read_inactive(text));
	struct swine_mode *resize = config_find_mode("resize");
	CHECK(resize != NULL);
	if (resize) {
		CHECK_INT(resize->keysym_bindings->length, 2);
	}
	CHECK(keyboard_find_binding(0, 0x25, false) == NULL);
	CHECK(run("mode resize"));
	CHECK(config->current_mode == resize);
	struct swine_binding *b = keyboard_find_binding(0, 0x25, false);
	CHECK(b != NULL);
	if (b) {
		CHECK_STR(b->command, "resize shrink width 10px");
	}
	CHECK(run("mode default"));
	CHECK_STR(config->current_mode->name, "default");
	CHECK(!run("mode nosuchmode"));
	fake_teardown();
}

static void test_errors_and_defer(void) {
	fake_setup();
	CHECK(!read_inactive("frobnicate everything\n"));
	CHECK(!read_inactive("bindsym Mod4+NoSuchKey exec x\n"));
	CHECK(!read_inactive("mode \"x\" {\nbindsym a nop\n"));   // unclosed block
	CHECK(!read_inactive("}\n"));
	CHECK(!read_inactive("focus left\n"));  // runtime-only
	CHECK(!run("default_orientation vertical"));  // config-only

	CHECK(read_inactive("exec notepad.exe\nexec_always calc.exe\nworkspace 3\n"));
	CHECK_INT(config->cmd_queue->length, 3);
	CHECK_INT(fake.spawn_calls, 0);
	config_run_deferred();
	CHECK_INT(fake.spawn_calls, 2);
	CHECK_STR(fake.last_spawn, "calc.exe");
	CHECK_STR(seat_get_focused_workspace()->name, "3");
	fake_teardown();
}

static void test_settings(void) {
	fake_setup();
	CHECK(read_inactive(
		"gaps inner 10\n"
		"gaps outer 5\n"
		"default_orientation vertical\n"
		"workspace_layout tabbed\n"
		"focus_wrapping no\n"
		"workspace_auto_back_and_forth yes\n"
		"floating_modifier Mod4 normal\n"
		"font pango:monospace 10\n"
		"output * bg wallpaper.png fill\n"
		"input type:keyboard {\n xkb_layout se\n}\n"));
	CHECK_INT(config->gaps_inner, 10);
	CHECK_INT(config->gaps_outer.left, 5);
	CHECK_INT(config->default_orientation, L_VERT);
	CHECK_INT(config->default_layout, L_TABBED);
	CHECK_INT(config->focus_wrapping, WRAP_NO);
	CHECK(config->auto_back_and_forth);
	CHECK_INT(config->floating_mod, SWINE_MOD_LOGO);
	CHECK(config->ignored_count >= 3);
	fake_teardown();
}

static void test_default_config(void) {
	fake_setup();
	bool ok = load_main_config(SWINEWM_SOURCE_DIR "/config.in", false, true);
	CHECK(ok);
	CHECK(config->modes->length == 2);
	struct swine_binding *term = keyboard_find_binding(SWINE_MOD_LOGO, 0x0D, false);
	CHECK(term && strcmp(term->command, "exec alacritty.exe") == 0);
	struct swine_binding *menu = keyboard_find_binding(SWINE_MOD_LOGO, 'D', false);
	CHECK(menu && strcmp(menu->command,
		"exec %LOCALAPPDATA%\\FlowLauncher\\Flow.Launcher.exe") == 0);
	CHECK(config->bar_enabled);
	fake_teardown();
}

void test_config_all(void) {
	test_variables_and_bindings();
	test_modes_and_blocks();
	test_errors_and_defer();
	test_settings();
	test_default_config();
}
