#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "json.h"
#include "swinewm/bar/bar.h"
#include "swinewm/bar/css.h"
#include "swinewm/bar/custom.h"
#include "swinewm/bar/format.h"
#include "swinewm/bar/tray.h"
#include "swinewm/config.h"
#include "swinewm/input/keyboard.h"
#include "swinewm/ipc-json.h"
#include "swinewm/tree/root.h"
#include "test.h"

static void test_json_parse(void) {
	char *error = NULL;
	struct json_value *v = json_parse(
		"// comment\n{\n  \"a\": 1.5, /* block */ \"s\": \"x\\\"\\u00e9\",\n"
		"  \"list\": [true, null, \"y\",],\n  \"o\": {\"k\": false},\n}\n",
		&error);
	CHECK(v != NULL);
	CHECK(error == NULL);
	if (v) {
		CHECK(json_get_number(v, "a", 0) == 1.5);
		CHECK_STR(json_get_string(v, "s", NULL), "x\"\xC3\xA9");
		struct json_value *list = json_object_get(v, "list");
		CHECK(list && list->type == JSON_ARRAY && list->items->length == 3);
		CHECK(!json_get_bool(json_object_get(v, "o"), "k", true));
		CHECK(json_get_string(v, "missing", NULL) == NULL);
	}
	json_value_free(v);

	v = json_parse("{\"a\": }", &error);
	CHECK(v == NULL);
	CHECK(error != NULL && strstr(error, "line 1") != NULL);
	free(error);
	v = json_parse("[1, 2] x", &error);
	CHECK(v == NULL);
	free(error);
}

static void test_css(void) {
	struct css_stylesheet *sheet = css_parse(
		"@define-color accent #ff0000;\n"
		"* { font-family: \"Fira Sans\", sans-serif; font-size: 12pt; }\n"
		"window#waybar { background-color: rgba(0, 0, 0, 0.5); color: #eee; }\n"
		"#workspaces button { padding: 0 5px; color: white; }\n"
		"#workspaces button.focused { background: @accent;"
		" box-shadow: inset 0 -3px #ffffff; }\n"
		"#workspaces button:hover { background: #123456; }\n"
		"#clock, #battery { padding: 2px 10px 4px; margin: 0 4px; }\n"
		"#battery.critical { color: alpha(@accent, 0.5); font-weight: bold; }\n"
		"@keyframes blink { to { color: #000; } }\n"
		"#mode { border-bottom: 3px solid #00ff00; }\n");
	struct swine_style win, box, button, focused, clock, battery, mode;
	struct css_element win_el = {"window", "waybar", NULL, NULL};
	css_compute(sheet, &win_el, NULL, &win);
	CHECK_INT(win.background.a, 128);
	CHECK_INT(win.color.r, 0xee);
	CHECK_STR(win.font_family, "Fira Sans, sans-serif");
	CHECK_INT((int)win.font_size, 16);

	struct css_element box_el = {"box", "workspaces", NULL, &win_el};
	css_compute(sheet, &box_el, &win, &box);
	struct css_element btn_el = {"button", NULL, NULL, &box_el};
	css_compute(sheet, &btn_el, &box, &button);
	CHECK_INT(button.padding.left, 5);
	CHECK_INT(button.padding.top, 0);
	CHECK_INT(button.background.a, 0);   // :hover never applies
	CHECK_INT(button.color.r, 255);

	const char *classes[] = {"focused", NULL};
	struct css_element focused_el = {"button", NULL, classes, &box_el};
	css_compute(sheet, &focused_el, &box, &focused);
	CHECK_INT(focused.background.r, 255);
	CHECK_INT(focused.background.g, 0);
	CHECK_INT(focused.inset_bottom, 3);

	struct css_element clock_el = {"label", "clock", NULL, &win_el};
	css_compute(sheet, &clock_el, &win, &clock);
	CHECK_INT(clock.padding.top, 2);
	CHECK_INT(clock.padding.right, 10);
	CHECK_INT(clock.padding.bottom, 4);
	CHECK_INT(clock.padding.left, 10);
	CHECK_INT(clock.margin.left, 4);
	CHECK_INT(clock.color.r, 0xee);   // inherited

	const char *crit[] = {"critical", NULL};
	struct css_element bat_el = {"label", "battery", crit, &win_el};
	css_compute(sheet, &bat_el, &win, &battery);
	CHECK_INT(battery.color.r, 255);
	CHECK_INT(battery.color.a, 127);
	CHECK(battery.bold);

	struct css_element mode_el = {"label", "mode", NULL, &win_el};
	css_compute(sheet, &mode_el, &win, &mode);
	CHECK_INT(mode.border_bottom.width, 3);
	CHECK_INT(mode.border_bottom.color.g, 255);
	css_free(sheet);

	struct swine_color c;
	CHECK(css_parse_color(NULL, "#abc", &c) && c.r == 0xaa && c.a == 255);
	CHECK(css_parse_color(NULL, "#11223344", &c) && c.a == 0x44);
	CHECK(!css_parse_color(NULL, "@undefined", &c));
	struct swine_color blended = color_blend(
		(struct swine_color){255, 255, 255, 128}, (struct swine_color){0, 0, 0, 255});
	CHECK_INT(blended.r, 128);
}

static void test_format(void) {
	struct tm tm = {0};
	tm.tm_year = 126;
	tm.tm_mon = 9;
	tm.tm_mday = 15;
	tm.tm_hour = 9;
	tm.tm_min = 5;
	char *s = format_waybar("{:%H:%M} {:%Y-%m-%d}", NULL, 0, &tm);
	CHECK_STR(s, "09:05 2026-10-15");
	free(s);
	struct fmt_arg args[] = {
		{"used", true, NULL, 3.14159},
		{"name", false, "web", 0},
	};
	s = format_waybar("{used:0.1f}G {name} {{x}} {missing}|{name:>5}", args, 2, &tm);
	CHECK_STR(s, "3.1G web {x} |  web");
	free(s);
	s = format_waybar("{:%-d.%-m %k %Q|%}", NULL, 0, &tm);
	CHECK_STR(s, "15.10 09 |");
	free(s);
	s = format_waybar("{}%", (struct fmt_arg[]){{NULL, true, NULL, 42}}, 1, NULL);
	CHECK_STR(s, "42%");
	free(s);
	s = markup_strip("<span style=\"italic\">resize</span> &amp; &lt;go&gt;");
	CHECK_STR(s, "resize & <go>");
	free(s);
}

static struct bar_module_view *find_view(struct bar_model *model, int index) {
	for (int i = 0; i < model->modules->length; ++i) {
		struct bar_module_view *v = model->modules->items[i];
		if (v->module_index == index) {
			return v;
		}
	}
	return NULL;
}

static const char *item_text(struct bar_model *model, int index) {
	struct bar_module_view *v = find_view(model, index);
	if (!v || !v->items->length) {
		return NULL;
	}
	return ((struct bar_item *)v->items->items[0])->text;
}

static void test_bar_model(void) {
	fake_setup();
	config->bar_enabled = true;
	bar_load_from_strings(
		"{\n"
		"  \"height\": 24,\n"
		"  \"modules-left\": [\"sway/workspaces\", \"sway/mode\", \"pulseaudio\"],\n"
		"  \"modules-center\": [\"sway/window\"],\n"
		"  \"modules-right\": [\"cpu\", \"battery#bat0\", \"clock\", \"custom/hello\"],\n"
		"  \"sway/window\": { \"max-length\": 5 },\n"
		"  \"cpu\": { \"format\": \"{usage}%\", \"states\": {\"busy\": 50} },\n"
		"  \"battery#bat0\": { \"states\": { \"warning\": 30, \"critical\": 15 },\n"
		"     \"format\": \"{capacity}% {icon}\", \"format-icons\": [\"E\", \"H\", \"F\"] },\n"
		"  \"clock\": { \"format\": \"T\", \"format-alt\": \"D\" },\n"
		"  \"custom/hello\": { \"format\": \"hi\", \"on-click\": \"notepad.exe\" },\n"
		"}\n", NULL);
	CHECK(bar_enabled());
	CHECK_INT(bar_height(), 24);

	fake_map_view("A long title");
	struct swine_output *output = root->outputs->items[0];
	// The bar's space is reserved at the top.
	CHECK_INT(output->reserved.top, 24);
	struct swine_box rect;
	CHECK(bar_get_rect(output, &rect));
	CHECK_INT(rect.y, 0);
	CHECK_INT(rect.height, 24);

	run("workspace 2");
	fake.cpu_usage = 70;
	struct bar_model *model = bar_build(output, 0);
	// "pulseaudio" is unsupported, so the indexes are 0 ws, 1 mode, 2 window, ...
	struct bar_module_view *ws = find_view(model, 0);
	CHECK(ws && ws->is_box && ws->items->length == 2);
	if (ws && ws->items->length == 2) {
		struct bar_item *b1 = ws->items->items[0];
		struct bar_item *b2 = ws->items->items[1];
		CHECK_STR(b1->text, "1");
		CHECK_STR(b2->text, "2");
		CHECK_STR(b1->on_click, "workspace --no-auto-back-and-forth \"1\"");
		// default style: focused button has #64727D background
		CHECK_INT(b2->style.background.r, 0x64);
		CHECK_INT(b1->style.background.a, 0);
		CHECK_STR(b2->on_scroll_down, "workspace next_on_output");
	}
	CHECK(find_view(model, 1) == NULL);   // mode hidden in "default"
	CHECK(find_view(model, 2) == NULL);   // empty workspace: no window
	CHECK_STR(item_text(model, 3), "70%");
	CHECK(find_view(model, 4) == NULL);   // no battery
	CHECK_STR(item_text(model, 5), "T");
	struct bar_module_view *custom = find_view(model, 6);
	CHECK(custom != NULL);
	if (custom) {
		struct bar_item *item = custom->items->items[0];
		CHECK_STR(item->text, "hi");
		CHECK_STR(item->on_click, "exec notepad.exe");
	}
	bar_model_free(model);

	run("workspace 1");
	fake.has_battery = true;
	fake.battery_capacity = 10;
	bar_module_clicked(5);
	config->current_mode = config_add_mode("resize");
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 1), "resize");
	CHECK_STR(item_text(model, 2), "A lon\xE2\x80\xA6");
	CHECK_STR(item_text(model, 4), "10% E");
	CHECK_STR(item_text(model, 5), "D");
	struct bar_module_view *bat = find_view(model, 4);
	if (bat) {
		// default style: #battery.critical:not(.charging) never matches
		// (unsupported pseudo-class), #battery is white
		struct bar_item *item = bat->items->items[0];
		CHECK_INT(item->style.background.r, 255);
	}
	bar_model_free(model);

	// "bar mode invisible" hides the bar and frees the space.
	CHECK(run("bar mode invisible"));
	CHECK_INT(output->reserved.top, 0);
	CHECK(!bar_get_rect(output, &rect));
	CHECK(run("bar mode dock"));
	CHECK(bar_get_rect(output, &rect));
	output->external_fullscreen = true;   // e.g. a game
	CHECK(!bar_get_rect(output, &rect));
	output->external_fullscreen = false;
	CHECK(run("fullscreen"));
	CHECK(!bar_get_rect(output, &rect));
	fake_teardown();
}

static void test_bar_config_enabling(void) {
	fake_setup();
	config->active = false;
	config->reading = true;
	CHECK(read_config_text("bar {\n}\n", "test"));
	CHECK(config->bar_enabled);
	config->bar_enabled = false;
	CHECK(read_config_text("bar bar-0 {\n position bottom\n status_command date\n}\n",
		"test"));
	CHECK(config->bar_enabled);
	CHECK_INT(config->bar_position, 2);
	config->bar_enabled = false;
	CHECK(read_config_text("exec waybar\n", "test"));
	config->reading = false;
	config->active = true;
	config_run_deferred();
	CHECK(config->bar_enabled);
	CHECK_INT(fake.spawn_calls, 0);
	bar_load_from_strings(NULL, NULL);
	CHECK(!bar_at_top());
	fake_teardown();
}

static uint64_t tray_id_at(struct bar_module_view *view, int i) {
	return view && i < view->items->length ?
		((struct bar_item *)view->items->items[i])->tray_id : 0;
}

static void test_tray(void) {
	fake_setup();
	config->bar_enabled = true;
	bar_load_from_strings(
		"{ \"modules-right\": [\"clock\", \"tray\"],\n"
		"  \"tray\": { \"spacing\": 7, \"icon-size\": 20,\n"
		"    \"icons\": { \"spotify\": false, \"Steam.exe\": \"C:/steam.ico\" },\n"
		"    \"orders\": { \"discord\": 5 },\n"
		"    \"ignore-list\": [\"Updater\"] } }\n",
		"#tray > .passive { color: #ff0000; }\n"
		"#tray > .active { color: #00ff00; }\n");
	CHECK(bar_has_tray());
	struct swine_output *output = root->outputs->items[0];

	// An empty tray is hidden, as in Waybar.
	struct bar_model *model = bar_build(output, 0);
	CHECK(find_view(model, 1) == NULL);
	bar_model_free(model);

	tray_item_set(1, "Discord.exe", "Discord", false);
	tray_item_set(2, "Spotify.exe", "Spotify", false);
	tray_item_set(3, "Steam.exe", "Steam", false);
	tray_item_set(4, "OneDrive.exe", "OneDrive Updater", false);
	tray_item_set(5, "Teams.exe", "Teams", true);   // hidden by its program
	model = bar_build(output, 0);
	struct bar_module_view *tray = find_view(model, 1);
	CHECK(tray && tray->is_box && tray->items->length == 2);
	if (tray && tray->items->length == 2) {
		CHECK_INT(tray->item_spacing, 7);
		// Steam (order 0) before Discord (order 5); Spotify hidden by
		// "icons", OneDrive by "ignore-list", Teams is passive.
		CHECK_INT((int)tray_id_at(tray, 0), 3);
		CHECK_INT((int)tray_id_at(tray, 1), 1);
		struct bar_item *steam = tray->items->items[0];
		CHECK_INT(steam->icon_size, 20);
		CHECK_STR(steam->icon_path, "C:/steam.ico");
		CHECK_INT(steam->style.color.g, 255);
		struct bar_item *discord = tray->items->items[1];
		CHECK(discord->icon_path == NULL);
		CHECK_STR(discord->text, "");
	}
	bar_model_free(model);

	// show-passive-items and reverse-direction
	bar_load_from_strings(
		"{ \"modules-right\": [\"tray\"],\n"
		"  \"tray\": { \"show-passive-items\": true,"
		" \"reverse-direction\": true } }\n",
		"#tray > .passive { color: #ff0000; }\n");
	tray_item_set(1, "Discord.exe", "Discord (3)", false);   // update in place
	CHECK(tray_item_remove(4));
	CHECK(!tray_item_remove(42));
	model = bar_build(output, 0);
	tray = find_view(model, 0);
	CHECK(tray && tray->items->length == 4);
	if (tray && tray->items->length == 4) {
		// newest first: Teams, Steam, Spotify, Discord
		CHECK_INT((int)tray_id_at(tray, 0), 5);
		CHECK_INT((int)tray_id_at(tray, 3), 1);
		struct bar_item *teams = tray->items->items[0];
		CHECK_INT(teams->style.color.r, 255);
		CHECK_INT(teams->style.color.g, 0);
	}
	bar_model_free(model);
	struct tray_item *first = tray_get_items()->items[0];
	CHECK_STR(first->title, "Discord (3)");

	bar_load_from_strings("{ \"modules-right\": [\"clock\"] }", NULL);
	CHECK(!bar_has_tray());
	tray_clear();
	CHECK_INT(tray_get_items()->length, 0);
	fake_teardown();
}

static void test_bar_config_ipc(void) {
	fake_setup();
	config->bar_enabled = true;
	bar_load_from_strings("{ \"position\": \"bottom\" }",
		"* { font-family: \"Fira Sans\"; font-size: 16px; }\n"
		"window#waybar { background-color: #102030; color: #e0e0e0; }\n"
		"#workspaces button.focused { background-color: #405060;\n"
		"    box-shadow: inset 0 -3px #ffcc00; }\n"
		"#workspaces button.urgent { background-color: #eb4d4b; }\n");
	char *ids = ipc_json_get_bar_config(NULL);
	CHECK_STR(ids, "[\"bar-0\"]");
	free(ids);
	char *json = ipc_json_get_bar_config("bar-0");
	CHECK(strstr(json, "\"position\":\"bottom\"") != NULL);
	CHECK(strstr(json, "\"mode\":\"dock\"") != NULL);
	CHECK(strstr(json, "\"font\":\"pango:Fira Sans 12\"") != NULL);   // 16px = 12pt
	CHECK(strstr(json, "\"background\":\"#102030ff\"") != NULL);
	CHECK(strstr(json, "\"statusline\":\"#e0e0e0ff\"") != NULL);
	CHECK(strstr(json, "\"focused_workspace_bg\":\"#405060ff\"") != NULL);
	CHECK(strstr(json, "\"focused_workspace_border\":\"#ffcc00ff\"") != NULL);
	// A transparent button shows the bar's background.
	CHECK(strstr(json, "\"inactive_workspace_bg\":\"#102030ff\"") != NULL);
	CHECK(strstr(json, "\"urgent_workspace_bg\":\"#eb4d4bff\"") != NULL);
	free(json);
	json = ipc_json_get_bar_config("bar-9");
	CHECK(strstr(json, "\"success\":false") != NULL);
	free(json);
	fake_teardown();
}

// Waybar's sway/language: the active keyboard layout.
static void test_language(void) {
	fake_setup();
	config->bar_enabled = true;
	fake.kb_layout_count = 3;
	fake.kb_active = 1;
	bar_load_from_strings(
		"{ \"modules-right\": [\"sway/language\", \"sway/language#long\","
		"   \"sway/language#flag\"],\n"
		"  \"sway/language#long\": { \"format\": \"{long} {shortDescription}\","
		"     \"on-click\": \"swaymsg input type:keyboard xkb_switch_layout next\" },\n"
		"  \"sway/language#flag\": { \"format\": \"{flag}{variant}\" } }", NULL);
	struct swine_output *output = root->outputs->items[0];
	struct bar_model *model = bar_build(output, 0);
	CHECK_STR(item_text(model, 0), "se");
	CHECK_STR(item_text(model, 1), "Swedish (Sweden) sv");
	// U+1F1F8 U+1F1EA: the Swedish flag
	CHECK_STR(item_text(model, 2), "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAA");
	struct bar_module_view *v = find_view(model, 1);
	if (v && v->items->length) {
		struct bar_item *item = v->items->items[0];
		CHECK_STR(item->on_click,
			"exec swaymsg input type:keyboard xkb_switch_layout next");
		// Clicking it runs the command in swinewm (no swaymsg process).
		CHECK(run(item->on_click));
		CHECK_INT(fake.kb_active, 2);
		CHECK_INT(fake.spawn_calls, 0);
	}
	bar_model_free(model);
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 2), "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8" "Dvorak");
	bar_model_free(model);

	// hide-single-layout
	bar_load_from_strings("{ \"modules-right\": [\"sway/language\"],"
		" \"sway/language\": { \"hide-single-layout\": true } }", NULL);
	fake.kb_layout_count = 1;
	fake.kb_active = 0;
	keyboard_layouts_refresh();
	model = bar_build(output, 0);
	CHECK(find_view(model, 0) == NULL);
	bar_model_free(model);
	fake_teardown();
}

static void test_script_parse(void) {
	struct bar_script_result r;
	bar_script_parse("hello\ntip\nwarn\nignored", false, false, &r);
	CHECK_STR(r.text, "hello");
	CHECK_STR(r.tooltip, "tip");
	CHECK_INT(r.classes->length, 1);
	CHECK_STR(r.classes->items[0], "warn");
	bar_script_result_finish(&r);
	bar_script_parse("a & <b>", false, true, &r);
	CHECK_STR(r.text, "a &amp; &lt;b&gt;");
	bar_script_result_finish(&r);
	bar_script_parse("{\"text\": \"5\", \"alt\": \"muted\", \"tooltip\": \"t\","
		" \"class\": [\"x\", \"y\"], \"percentage\": 42.6}\n{\"text\": \"6\"}",
		true, false, &r);
	CHECK_STR(r.text, "5");
	CHECK_STR(r.alt, "muted");
	CHECK_STR(r.tooltip, "t");
	CHECK_INT(r.classes->length, 2);
	CHECK_INT(r.percentage, 43);
	bar_script_result_finish(&r);
	bar_script_parse("not json", true, false, &r);
	CHECK_STR(r.text, "");
	bar_script_result_finish(&r);
}

static struct bar_item *first_item(struct bar_model *model, int index) {
	struct bar_module_view *v = find_view(model, index);
	return v && v->items->length ? v->items->items[0] : NULL;
}

// Waybar's custom/* scripts: when they run, and what the module shows.
static void test_custom_scripts(void) {
	fake_setup();
	config->bar_enabled = true;
	fake.now_ms = 1000;
	bar_load_from_strings(
		"{ \"modules-right\": [\"custom/interval\", \"custom/if\","
		"   \"custom/loop\", \"custom/signal\", \"custom/json\"],\n"
		"  \"custom/interval\": { \"exec\": \"date.bat\", \"interval\": 5,"
		"     \"format\": \"[{}]\", \"on-click-right\": \"notepad.exe\" },\n"
		"  \"custom/if\": { \"exec-if\": \"check.bat\", \"exec\": \"run.bat\","
		"     \"interval\": \"once\" },\n"
		"  \"custom/loop\": { \"exec\": \"loop.bat\", \"restart-interval\": 2 },\n"
		"  \"custom/signal\": { \"exec\": \"count.bat\", \"signal\": 8,"
		"     \"hide-empty-text\": true },\n"
		"  \"custom/json\": { \"exec\": \"vol.ps1\", \"interval\": 1,"
		"     \"return-type\": \"json\", \"format\": \"{icon} {text} {percentage}\","
		"     \"format-icons\": { \"muted\": \"M\", \"default\": [\"lo\", \"hi\"] } } }",
		NULL);
	struct swine_output *output = root->outputs->items[0];
	// Nothing has run yet: every script module is hidden.
	struct bar_model *model = bar_build(output, 0);
	CHECK_INT(model->modules->length, 0);
	bar_model_free(model);
	CHECK(bar_scripts_next_due() == 1000);
	bar_scripts_tick(1000);
	// interval, exec-if, loop, signal and json start; exec-if first.
	CHECK_INT(fake.script_starts, 5);
	CHECK_STR(fake.last_script_output, "DISPLAY1");
	CHECK(bar_scripts_next_due() == 0);   // all running

	// 1: interval. A run's whole output, then again after the interval.
	bar_script_line(1, "12:00");
	bar_script_line(1, "tooltip");
	bar_script_exited(1, 0);
	CHECK(bar_scripts_next_due() == 6000);
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 0), "[12:00]");
	struct bar_item *item = first_item(model, 0);
	CHECK(item && item->on_click_right &&
		strcmp(item->on_click_right, "exec notepad.exe") == 0);
	bar_model_free(model);

	// 2: exec-if fails: hidden, and "once" does not try again.
	bar_script_exited(2, 1);
	model = bar_build(output, 0);
	CHECK(find_view(model, 1) == NULL);
	bar_model_free(model);

	// 3: a continuous script: each line is an update.
	bar_script_line(3, "first");
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 2), "first");
	bar_model_free(model);
	bar_script_line(3, "second");
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 2), "second");
	bar_model_free(model);
	fake.now_ms = 1500;
	bar_script_exited(3, 1);   // it stopped: hidden, restarted in 2 s
	model = bar_build(output, 0);
	CHECK(find_view(model, 2) == NULL);
	bar_model_free(model);
	CHECK(bar_scripts_next_due() == 3500);

	// 4: signal. Once at startup, then on "pkill -RTMIN+8 waybar".
	bar_script_line(4, "");
	bar_script_exited(4, 0);
	model = bar_build(output, 0);
	CHECK(find_view(model, 3) == NULL);   // empty output
	bar_model_free(model);
	int starts = fake.script_starts;
	CHECK(run("exec pkill -RTMIN+8 waybar"));
	CHECK_INT(fake.script_starts, starts + 1);
	CHECK_STR(fake.last_script, "count.bat");
	uint64_t id = fake.last_script_id;
	CHECK(run("exec pkill -SIGRTMIN+8 waybar"));   // still running: no second
	CHECK_INT(fake.script_starts, starts + 1);
	bar_script_line(id, "3");
	bar_script_exited(id, 0);
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 3), "3");
	bar_model_free(model);
	CHECK(run("exec killall -s RTMIN+8 waybar"));
	CHECK_INT(fake.script_starts, starts + 2);
	bar_script_exited(fake.last_script_id, 0);
	CHECK_INT(fake.spawn_calls, 0);
	CHECK(run("exec pkill -RTMIN+8 notepad"));   // not waybar: a program
	CHECK_INT(fake.spawn_calls, 1);

	// 5: JSON with format-icons by alt, then by percentage.
	bar_script_line(5, "{\"text\": \"50%\", \"percentage\": 80, \"class\": \"loud\"}");
	bar_script_exited(5, 0);
	model = bar_build(output, 0);
	CHECK_STR(item_text(model, 4), "hi 50% 80");
	bar_model_free(model);

	// A click runs an idle interval script again (exec-on-event).
	fake.now_ms = 2000;
	starts = fake.script_starts;
	bar_module_clicked(0);
	CHECK_INT(fake.script_starts, starts + 1);
	CHECK_STR(fake.last_script, "date.bat");
	bar_module_event(0);   // already running
	CHECK_INT(fake.script_starts, starts + 1);

	// exec-if succeeds: exec runs after it.
	bar_module_event(1);
	CHECK_STR(fake.last_script, "check.bat");
	bar_script_exited(fake.last_script_id, 0);
	CHECK_STR(fake.last_script, "run.bat");

	// A reload stops what is running.
	int stops = fake.script_stops;
	bar_load_from_strings("{ \"modules-right\": [\"clock\"] }", NULL);
	CHECK(fake.script_stops >= stops + 2);
	CHECK(bar_scripts_next_due() == 0);

	// A script that cannot start is hidden and tried again later.
	fake.script_start_fails = true;
	bar_load_from_strings("{ \"modules-right\": [\"custom/x\"],"
		" \"custom/x\": { \"exec\": \"missing\", \"interval\": 3 } }", NULL);
	bar_scripts_tick(2000);
	CHECK(bar_scripts_next_due() == 5000);
	model = bar_build(output, 0);
	CHECK_INT(model->modules->length, 0);
	bar_model_free(model);
	fake.script_start_fails = false;
	fake_teardown();
}

void test_bar_all(void) {
	test_bar_config_ipc();
	test_json_parse();
	test_css();
	test_format();
	test_bar_model();
	test_bar_config_enabling();
	test_tray();
	test_language();
	test_script_parse();
	test_custom_scripts();
}
