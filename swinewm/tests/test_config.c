#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/input/seat.h"
#include "swinewm/notify.h"
#include "swinewm/swaynag.h"
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

	// Store apps: the backslash and "!" of shell:AppsFolder\<AUMID> reach
	// the platform unchanged, from the config and at runtime.
	CHECK(read_inactive("exec shell:AppsFolder\\AnthropicPBC.Claude_fnn82j28hfe8t!Claude\n"));
	config_run_deferred();
	CHECK_STR(fake.last_spawn,
		"shell:AppsFolder\\AnthropicPBC.Claude_fnn82j28hfe8t!Claude");
	CHECK(run("exec shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"));
	CHECK_STR(fake.last_spawn,
		"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
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
	CHECK(config->ignored_count >= 2);   // font, input
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
	CHECK(menu && strcmp(menu->command, "exec swinemenu") == 0);
	CHECK(config->bar_enabled);
	fake_teardown();
}

static void test_output_bg(void) {
	fake_setup();
	config->active = false;
	config->reading = true;
	CHECK(read_config_text(
		"output * bg %SWINEWM_DATA%\\wallpapers\\pig.png fill\n"
		"output DISPLAY2 resolution 1920x1080 position 1920,0\n"
		"output DISPLAY2 background pictures/a.jpg fit #112233\n"
		"output DISPLAY3 bg #445566 solid_color\n"
		"output HDMI-A-1 {\n  mode 1920x1080@60Hz\n  bg c:/w.png center\n}\n",
		"C:/Users/me/.swinewm/config"));
	CHECK(!read_config_text("output * bg file.png sideways\n", "x"));
	CHECK(!read_config_text("output * bg\n", "x"));
	CHECK(!read_config_text("output * bg red solid_color\n", "x"));
	config->reading = false;
	config->active = true;

	struct output_config *all = output_config_for_name("DISPLAY1");
	CHECK(all && strcmp(all->name, "*") == 0);
	if (all) {
		CHECK_STR(all->background,
			"C:\\Program Files\\SwineWM\\wallpapers\\pig.png");
		CHECK_STR(all->background_mode, "fill");
		CHECK(all->background_fallback == NULL);
	}
	struct output_config *d2 = output_config_for_name("display2");
	CHECK(d2 && d2 != all);
	if (d2) {
		// relative to the directory of the config file
		CHECK_STR(d2->background, "C:/Users/me/.swinewm/pictures/a.jpg");
		CHECK_STR(d2->background_mode, "fit");
		CHECK_STR(d2->background_fallback, "#112233");
	}
	struct output_config *d3 = output_config_for_name("DISPLAY3");
	CHECK(d3 && strcmp(d3->background_mode, "solid_color") == 0 &&
		strcmp(d3->background, "#445566") == 0);
	struct output_config *hdmi = output_config_for_name("HDMI-A-1");
	CHECK(hdmi && strcmp(hdmi->background, "c:/w.png") == 0);

	// At runtime, e.g. from swinemsg
	CHECK(run("output DISPLAY3 bg #000000 solid_color"));
	CHECK_STR(output_config_for_name("DISPLAY3")->background, "#000000");
	fake_teardown();
}

static void test_include(void) {
	fake_setup();
	CHECK(glob_match("*.conf", "10-vars.CONF"));
	CHECK(glob_match("[0-9]?-*", "30-layout"));
	CHECK(!glob_match("[!0-9]*", "30-layout"));
	CHECK(glob_match("a*b*c", "aXbYbZc"));
	CHECK(!glob_match("a*b", "aXbY"));
	CHECK(!glob_match("[ab", "a"));

	CHECK(load_main_config(SWINEWM_SOURCE_DIR "/tests/data/include/config",
		false, true));
	// Sorted: 10-vars.conf, "20 gaps.conf", 30-layout; each file once
	// (the second glob, the quoted name and the loop back to the main
	// config are skipped), hidden files are not matched, a missing file is
	// no error.
	CHECK_INT(config->config_chain->length, 4);
	CHECK_INT(config->gaps_inner, 20);
	CHECK_INT(config->gaps_outer.top, 0);
	CHECK(config->default_layout == L_TABBED);
	fake_teardown();
}

static int count_swaynag(void) {
	int count = 0;
	list_t *active = notify_active();
	for (int i = 0; i < active->length; ++i) {
		struct swine_notification *n = active->items[i];
		if (strcmp(n->category, "swaynag") == 0) {
			count++;
		}
	}
	return count;
}

static void test_swaynag(void) {
	fake_setup();
	config->active = false;
	config->reading = true;
	CHECK(!read_config_text("frobnicate\ngaps inner\nmode \"x\" {\n", "cfg"));
	config->reading = false;
	config->active = true;
	CHECK_INT(config->errors->length, 3);
	swaynag_show_config_errors();
	CHECK_INT(count_swaynag(), 1);
	struct swine_notification *n = notify_active()->items[0];
	CHECK(n->urgency == NOTIFY_URGENCY_CRITICAL);
	CHECK(n->expire_ms == 0);
	CHECK(strstr(n->body, "Error on line 1 (cfg) 'frobnicate'") != NULL);
	CHECK(strstr(n->body, "Unclosed block") != NULL);
	// Showing again replaces the message; a clean config removes it.
	swaynag_show_config_errors();
	CHECK_INT(count_swaynag(), 1);
	list_free_items_and_destroy(config->errors);
	config->errors = create_list();
	swaynag_show_config_errors();
	CHECK_INT(count_swaynag(), 0);
	notify_dismiss_all(false);
	fake_teardown();
}

static void test_shipped_defaults(void) {
	// The files installed next to swinewm.exe must load without errors.
	fake_setup();
	char *text = read_file(SWINEWM_SOURCE_DIR "/resources/mako/config");
	CHECK(text != NULL);
	if (text) {
		CHECK(notify_parse_config(text, "mako/config"));
		CHECK(run("notify-send hello"));
		CHECK(run("notify-send -u critical alarm"));
		list_t *active = notify_active();
		struct swine_notification *critical = active->items[0];
		struct swine_notification *normal = active->items[1];
		CHECK_INT(normal->style.default_timeout, 10000);
		CHECK_INT(critical->style.default_timeout, 0);
		free(text);
	}
	fake_teardown();
}

void test_config_all(void) {
	test_output_bg();
	test_include();
	test_swaynag();
	test_shipped_defaults();
	test_variables_and_bindings();
	test_modes_and_blocks();
	test_errors_and_defer();
	test_settings();
	test_default_config();
}
