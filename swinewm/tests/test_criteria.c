#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/criteria.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/input/seat.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/view.h"
#include "swinewm/tree/workspace.h"
#include "test.h"

static struct swine_view *map(const char *title, const char *app_id,
		const char *class_name) {
	static int next_handle = 0x5000;
	struct swine_view *view = view_create((void *)(size_t)next_handle++);
	view->title = swine_strdup(title);
	view->app_id = swine_strdup(app_id);
	view->class_name = swine_strdup(class_name);
	view_map(view);
	transaction_commit_dirty();
	return view;
}

// Config-only commands (assign) as if read from the config file.
static bool config_lines(const char *text) {
	config->active = false;
	config->reading = true;
	bool ok = read_config_text(text, "test");
	config->reading = false;
	config->active = true;
	return ok;
}

static bool parse_fails(const char *raw, const char *expected_error) {
	char *error = NULL;
	struct criteria *criteria = criteria_parse(raw, &error);
	bool failed = criteria == NULL && error &&
		(!expected_error || strstr(error, expected_error));
	if (criteria) {
		criteria_destroy(criteria);
	}
	free(error);
	return failed;
}

static void test_parse(void) {
	fake_setup();
	CHECK(parse_fails("[frobnicate=1]", "not recognized"));
	CHECK(parse_fails("[app_id]", "requires a value"));
	CHECK(parse_fails("[title=\"abc]", "Quote mismatch"));
	CHECK(parse_fails("[]", "empty"));
	CHECK(parse_fails("[title=\"(\"]", "Regex compilation"));
	CHECK(parse_fails("[urgent=sometimes]", "urgent"));
	CHECK(parse_fails("[con_id=abc]", "numeric"));
	CHECK(parse_fails("title=x", "No criteria"));
	char *error = NULL;
	struct criteria *c = criteria_parse(
		"[app_id=\"firefox\" title=\"a \\\"quoted\\\" title\" floating] rest",
		&error);
	CHECK(c != NULL);
	if (c) {
		CHECK_STR(c->raw,
			"[app_id=\"firefox\" title=\"a \\\"quoted\\\" title\" floating]");
		CHECK(c->floating && c->app_id && c->title);
		criteria_destroy(c);
	}
	free(error);
	fake_teardown();
}

static void test_runtime_criteria(void) {
	fake_setup();
	struct swine_view *firefox = map("GitHub - Mozilla Firefox", "firefox.exe",
		"MozillaWindowClass");
	struct swine_view *discord = map("Friends", "Discord.exe",
		"Chrome_WidgetWin_1");
	struct swine_view *term = map("~/src", "alacritty.exe", "Window Class");

	// app_id ignores case, like Windows file names.
	CHECK(run("[app_id=\"discord\"] floating enable"));
	CHECK(container_is_floating(discord->container));
	CHECK(!container_is_floating(firefox->container));
	// title and class are case-sensitive regexes
	CHECK(run("[title=\"Mozilla Firefox$\" class=\"^Mozilla\"] move to workspace 3"));
	CHECK_STR(firefox->container->workspace->name, "3");
	CHECK(!run("[title=\"mozilla firefox\"] kill"));   // no matching node
	CHECK(run("[instance=\"^alacritty$\"] kill"));
	CHECK(fake.last_closed == term);
	// floating / tiling / workspace tokens
	CHECK(run("[floating] floating disable"));
	CHECK(!container_is_floating(discord->container));
	CHECK(run("[workspace=\"^3$\" tiling] floating enable"));
	CHECK(container_is_floating(firefox->container));
	// several commands, separated by "," apply to the same criteria
	CHECK(run("[app_id=firefox] floating disable, move to workspace 4"));
	CHECK_STR(firefox->container->workspace->name, "4");
	CHECK(!container_is_floating(firefox->container));
	// __focused__
	CHECK(run("workspace 1"));
	seat_set_focus_container(discord->container);
	struct swine_view *discord2 = map("Voice", "Discord.exe", "Chrome_WidgetWin_1");
	CHECK(run("[app_id=__focused__] move to workspace 7"));
	CHECK_STR(discord->container->workspace->name, "7");
	CHECK_STR(discord2->container->workspace->name, "7");
	CHECK_STR(term->container->workspace->name, "1");
	// con_id
	char line[64];
	snprintf(line, sizeof(line), "[con_id=%u] move to workspace 8",
		(unsigned)term->container->node.id);
	CHECK(run(line));
	CHECK_STR(term->container->workspace->name, "8");
	fake_teardown();
}

static void test_urgent_criteria(void) {
	fake_setup();
	struct swine_view *a = map("a", "a.exe", "A");
	struct swine_view *b = map("b", "b.exe", "B");
	struct swine_view *c = map("c", "c.exe", "C");
	(void)c;
	fake.now_ms = 1000;
	view_set_urgent(b, true);
	fake.now_ms = 2000;
	view_set_urgent(a, true);
	CHECK(run("[urgent=oldest] focus"));
	CHECK(seat_get_focused_container() == b->container);
	CHECK(run("[urgent=latest] focus"));
	CHECK(seat_get_focused_container() == a->container);
	CHECK(!run("[urgent=latest] focus"));   // focusing cleared the urgency
	fake_teardown();
}

static void test_for_window(void) {
	fake_setup();
	CHECK(run("for_window [app_id=\"(?i)^pavucontrol\"] floating enable"));
	// Several tokens: the criteria stays one argument (split_args).
	CHECK(config_lines("for_window [app_id=\"notepad\" title=\"Untitled\"] "
		"floating enable\n"));
	struct swine_view *pad = map("Untitled - Notepad", "notepad.exe", "Notepad");
	CHECK(container_is_floating(pad->container));
	// In the config the whole line is the rule; at runtime (as in sway)
	// "," would end the for_window command.
	CHECK(config_lines("for_window [title=\"Picture-in-Picture\"] "
		"floating enable, move to workspace 9\n"));
	// The same rule twice is kept once.
	CHECK(run("for_window [app_id=\"(?i)^pavucontrol\"] floating enable"));
	CHECK_INT(config->criteria->length, 3);

	struct swine_view *mixer = map("Volume", "Pavucontrol.exe", "Qt");
	CHECK(container_is_floating(mixer->container));

	// Rules are checked again when the title changes, and run once.
	struct swine_view *video = map("Firefox", "firefox.exe", "Mozilla");
	CHECK(!container_is_floating(video->container));
	CHECK(view_set_title(video, "Picture-in-Picture"));
	transaction_commit_dirty();
	CHECK(container_is_floating(video->container));
	CHECK_STR(video->container->workspace->name, "9");
	CHECK(!view_set_title(video, "Picture-in-Picture"));   // already ran
	fake_teardown();

	// A reload frees the rules: the views must forget them, or a new rule
	// at a freed one's address would count as run.
	fake_setup();
	CHECK(load_main_config(SWINEWM_SOURCE_DIR "/tests/data/reload/config",
		false, false));
	struct swine_view *pip = map("PiP", "firefox.exe", "Mozilla");
	CHECK(container_is_floating(pip->container));
	CHECK_INT(pip->executed_criteria->length, 1);
	CHECK(load_main_config(SWINEWM_SOURCE_DIR "/tests/data/reload/config",
		false, false));
	CHECK_INT(pip->executed_criteria->length, 0);
	// as in sway, the new rules run on the next title change
	CHECK(view_set_title(pip, "PiP"));
	CHECK_INT(pip->executed_criteria->length, 1);
	fake_teardown();
}

static void test_assign_and_no_focus(void) {
	fake_setup_two_outputs();
	CHECK(config_lines(
		"assign [app_id=\"discord\"] 3\n"
		"assign [class=\"^Steam$\"] \xE2\x86\x92 workspace number 5\n"
		"assign [app_id=\"spotify\"] output DISPLAY2\n"
		"no_focus [title=\"^popup$\"]\n"));
	struct swine_view *term = map("term", "alacritty.exe", "Window Class");
	CHECK(seat_get_focused_container() == term->container);

	struct swine_view *discord = map("Discord", "Discord.exe", "Chrome");
	CHECK_STR(discord->container->workspace->name, "3");
	CHECK(!discord->pending.visible);
	CHECK(seat_get_focused_container() == term->container);   // stays

	struct swine_view *steam = map("Steam", "steam.exe", "Steam");
	CHECK_STR(steam->container->workspace->name, "5");

	struct swine_view *spotify = map("Spotify", "Spotify.exe", "Chrome");
	CHECK_STR(spotify->container->workspace->output->name, "DISPLAY2");

	struct swine_view *popup = map("popup", "app.exe", "Popup");
	CHECK_STR(popup->container->workspace->name,
		term->container->workspace->name);
	CHECK(popup->pending.visible);
	CHECK(seat_get_focused_container() == term->container);   // no_focus

	// for_window and assign are config state: errors are reported.
	CHECK(!config_lines("assign [app_id=x]\n"));
	CHECK(!run("assign [app_id=x] 4"));   // config only
	fake_teardown();
}

void test_criteria_all(void) {
	test_parse();
	test_runtime_criteria();
	test_urgent_criteria();
	test_for_window();
	test_assign_and_no_focus();
}
