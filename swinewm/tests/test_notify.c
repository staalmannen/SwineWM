#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "ipc.h"
#include "swinewm/config.h"
#include "swinewm/ipc-server.h"
#include "stringop.h"
#include "swinewm/notify.h"
#include "test.h"

static struct swine_notification *newest(void) {
	list_t *active = notify_active();
	return active->length ? active->items[0] : NULL;
}

static void test_notify_send(void) {
	fake_setup();
	notify_parse_config("", "empty");
	CHECK(run("notify-send -a mail -u critical \"New mail\" \"From: Jens\""));
	struct swine_notification *n = newest();
	CHECK(n != NULL);
	if (n) {
		CHECK_STR(n->app_name, "mail");
		CHECK_STR(n->summary, "New mail");
		CHECK_STR(n->body, "From: Jens");
		CHECK_INT(n->urgency, NOTIFY_URGENCY_CRITICAL);
		CHECK_INT(n->expire_ms, 0);   // mako default-timeout is 0 (never)
		// mako defaults
		CHECK_INT(n->style.width, 300);
		CHECK_INT(n->style.background.r, 0x28);
		list_t *lines = notify_format_lines(n);
		CHECK_INT(lines->length, 2);
		struct notify_line *l0 = lines->items[0];
		struct notify_line *l1 = lines->items[1];
		CHECK_STR(l0->text, "New mail");
		CHECK(l0->bold);
		CHECK_STR(l1->text, "From: Jens");
		CHECK(!l1->bold);
		notify_free_lines(lines);
	}
	CHECK(!run("notify-send"));
	CHECK(!run("notify-send -u urgent x"));

	// exec in a binding goes to the built-in implementation
	CHECK(run("exec notify-send hello"));
	CHECK_INT(fake.spawn_calls, 0);
	CHECK_INT(notify_active()->length, 2);
	CHECK(run("exec makoctl dismiss --all"));
	CHECK_INT(notify_active()->length, 0);
	CHECK_INT(notify_history()->length, 2);
	CHECK(run("makoctl restore"));
	CHECK_INT(notify_active()->length, 1);
	CHECK(!run("makoctl frobnicate"));
	fake_teardown();
}

static void test_mako_config(void) {
	fake_setup();
	CHECK(notify_parse_config(
		"# mako config\n"
		"font=Fira Sans Bold 12\n"
		"background-color=#11223344\n"
		"default-timeout=5000\n"
		"max-visible=2\n"
		"max-history=1\n"
		"margin=5,10\n"
		"format=%a: %s\n"
		"\n"
		"[urgency=critical]\n"
		"border-color=#ff0000\n"
		"default-timeout=0\n"
		"\n"
		"[app-name=\"Spam Bot\"]\n"
		"invisible=1\n"
		"\n"
		"[mode=do-not-disturb]\n"
		"invisible=1\n"
		"\n"
		"[summary~=regex]\n"
		"text-color=#000000\n", "test"));

	fake.now_ms = 1000;
	CHECK(run("notify-send -a app one"));
	struct swine_notification *n = newest();
	CHECK(n && n->expire_ms == 6000);
	if (n) {
		CHECK_STR(n->style.font_family, "Fira Sans");
		CHECK(n->style.font_bold);
		CHECK((int)n->style.font_size == 12);
		CHECK_INT(n->style.background.a, 0x44);
		CHECK_INT(n->style.margin.right, 10);
		CHECK_INT(n->style.text.r, 0xFF);   // unsupported criteria ignored
		list_t *lines = notify_format_lines(n);
		CHECK_STR(((struct notify_line *)lines->items[0])->text, "app: one");
		notify_free_lines(lines);
	}
	CHECK(run("notify-send -u critical two"));
	n = newest();
	CHECK(n && n->expire_ms == 0 && n->style.border.r == 0xff);
	CHECK(run("notify-send -a \"Spam Bot\" three"));
	CHECK(run("notify-send -t 100 four"));

	// "Spam Bot" is invisible; max-visible=2 shows the newest two others
	// and a "(1 more)" placeholder for the rest.
	list_t *visible = notify_visible();
	CHECK_INT(visible->length, 3);
	CHECK_STR(((struct swine_notification *)visible->items[0])->summary, "four");
	CHECK_STR(((struct swine_notification *)visible->items[1])->summary, "two");
	struct swine_notification *more = visible->items[2];
	CHECK(more->hidden);
	if (more->hidden) {
		list_t *lines = notify_format_lines(more);
		CHECK_STR(((struct notify_line *)lines->items[0])->text, "(1 more)");
		notify_free_lines(lines);
	}
	notify_free_visible(visible);

	CHECK(notify_next_expiry() == 1100);
	CHECK(notify_tick(1100));
	CHECK_INT(notify_active()->length, 3);
	// "one" and the invisible "three" expire; critical "two" never does
	CHECK(notify_tick(6000));
	CHECK_INT(notify_active()->length, 1);
	CHECK_INT(notify_history()->length, 1);   // max-history=1

	CHECK(run("makoctl mode -a do-not-disturb"));
	visible = notify_visible();
	CHECK_INT(visible->length, 0);
	notify_free_visible(visible);
	CHECK(run("makoctl mode -t do-not-disturb"));
	visible = notify_visible();
	CHECK_INT(visible->length, 1);
	notify_free_visible(visible);

	uint32_t type;
	char *json = ipc_handle_request(SWINE_IPC_GET_NOTIFICATIONS, "", 0, &type);
	CHECK(strstr(json, "\"summary\":\"two\"") != NULL);
	CHECK(strstr(json, "\"urgency\":\"critical\"") != NULL);
	free(json);

	// Right click dismisses (mako default)
	n = newest();
	uint32_t id = n ? n->id : 0;
	notify_button(id, 3);
	CHECK(notify_by_id(id) == NULL);
	fake_teardown();
}


static struct swine_notification *nth(int i) {
	list_t *active = notify_active();
	return i < active->length ? active->items[i] : NULL;
}

static char *first_line(const struct swine_notification *n) {
	list_t *lines = notify_format_lines(n);
	char *text = swine_strdup(lines->length ?
		((struct notify_line *)lines->items[0])->text : "");
	notify_free_lines(lines);
	return text;
}

static void test_notify_send_options(void) {
	fake_setup();
	notify_parse_config("", "empty");
	CHECK(run("notify-send -i C:/icons/mail.png -h int:value:42 "
		"-h string:desktop-entry:thunderbird -A open=Open -A Later "
		"-c email Mail Body"));
	struct swine_notification *n = newest();
	CHECK(n != NULL);
	if (n) {
		CHECK_STR(n->app_icon, "C:/icons/mail.png");
		CHECK_INT(n->progress, 42);
		CHECK_STR(n->desktop_entry, "thunderbird");
		CHECK_STR(n->category, "email");
		CHECK_INT(n->actions->length, 2);
		struct notify_action_item *a0 = n->actions->items[0];
		struct notify_action_item *a1 = n->actions->items[1];
		CHECK_STR(a0->key, "open");
		CHECK_STR(a0->title, "Open");
		CHECK_STR(a1->key, "1");   // unnamed actions are numbered
		CHECK_STR(a1->title, "Later");
	}
	CHECK(run("notify-send -h int:value:150 -h byte:urgency:2 x"));
	n = newest();
	CHECK(n && n->progress == 100 && n->urgency == NOTIFY_URGENCY_CRITICAL);
	CHECK(run("notify-send plain"));
	CHECK(newest() && newest()->progress == -1);
	CHECK(!run("notify-send -h value:3 x"));
	CHECK(!run("notify-send -h float:value:3 x"));
	// image-path wins over the icon
	CHECK(run("notify-send -i a.png -h string:image-path:C:/b.png x"));
	CHECK(newest() && strcmp(newest()->app_icon, "C:/b.png") == 0);

	// Replacing keeps the notification's place (mako); so do tags.
	notify_dismiss_all(false);
	CHECK(run("notify-send -a vol -h string:x-dunst-stack-tag:volume "
		"\"Volume 10\""));
	uint32_t vol = newest()->id;
	CHECK(run("notify-send other"));
	CHECK(run("notify-send -a vol -h string:x-dunst-stack-tag:volume "
		"\"Volume 20\""));
	CHECK_INT(notify_active()->length, 2);
	CHECK(nth(1) && nth(1)->id == vol);
	CHECK_STR(nth(1)->summary, "Volume 20");
	// another app's tag is another notification
	CHECK(run("notify-send -a music -h string:x-dunst-stack-tag:volume x"));
	CHECK_INT(notify_active()->length, 3);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "notify-send -r %u \"Volume 30\"", vol);
	CHECK(run(cmd));
	CHECK_INT(notify_active()->length, 3);
	CHECK(nth(2) && nth(2)->id == vol);
	fake_teardown();
}

static void test_mako_criteria(void) {
	fake_setup();
	CHECK(notify_parse_config(
		"[summary~=\"^Build (passed|ok)\"]\n"
		"background-color=#00ff00\n"
		"[body~=error]\n"
		"border-color=#ff0000\n"
		"[app-icon=firefox]\n"
		"width=111\n"
		"[desktop-entry=org.gnome.Mail]\n"
		"width=222\n"
		"[actionable]\n"
		"height=50\n"
		"[actionable=false]\n"
		"height=60\n"
		"[!expiring]\n"
		"padding=1\n", "test"));
	CHECK(run("notify-send \"Build passed\" \"no error here\""));
	struct swine_notification *n = newest();
	CHECK(n && n->style.background.g == 0xff && n->style.border.r == 0xff);
	CHECK(n && n->style.height == 60);
	CHECK(run("notify-send \"Build failed\""));
	n = newest();
	CHECK(n && n->style.background.g == 0x55 && n->style.border.r == 0x4C);
	CHECK(run("notify-send -i firefox -A default=Open x"));
	n = newest();
	CHECK(n && n->style.width == 111 && n->style.height == 50);
	CHECK(run("notify-send -h string:desktop-entry:org.gnome.Mail x"));
	CHECK(newest() && newest()->style.width == 222);
	CHECK(run("notify-send -t 0 x"));
	CHECK(newest() && newest()->style.padding.top == 1);
	CHECK(run("notify-send -t 10 x"));
	CHECK(newest() && newest()->style.padding.top == 5);

	// Sections mako rejects are ignored, and the load reports failure.
	CHECK(!notify_parse_config("[frobnicate=1]\nwidth=1\n", "bad"));
	CHECK(!notify_parse_config("[summary~=\"(\"]\nwidth=1\n", "bad"));
	CHECK(!notify_parse_config("[grouped]\nanchor=top-left\n", "bad"));
	CHECK(!notify_parse_config("[app-name=x]\nmax-visible=1\n", "bad"));
	CHECK(!notify_parse_config("[summary=a summary~=b]\nwidth=1\n", "bad"));
	CHECK(!notify_parse_config("[app-name=x hidden]\nwidth=1\n", "bad"));
	CHECK(!notify_parse_config("width=10\nbackground-color=nope\n", "bad"));
	CHECK(!notify_parse_config("frobnicate=1\n", "bad"));
	CHECK(notify_parse_config("[anchor=top-left]\nmax-visible=1\n"
		"[output=DISPLAY1 hidden]\nformat=+%h\n", "good"));
	fake_teardown();
}

static void test_mako_output_anchor(void) {
	fake_setup();
	CHECK(notify_parse_config(
		"max-visible=1\n"
		"[app-name=left]\n"
		"anchor=bottom-left\n"
		"[anchor=bottom-left]\n"
		"text-color=#123456\n"
		"[output=DISPLAY1]\n"
		"border-size=7\n"
		"[output=DISPLAY1 anchor=bottom-left hidden]\n"
		"format=%h of %t hidden\n", "test"));
	CHECK(run("notify-send -a left one"));
	CHECK(run("notify-send -a left two"));
	CHECK(run("notify-send -a right three"));
	struct swine_notification *n = nth(1);   // "one" after "two"
	CHECK(n && n->style.anchor == NOTIFY_ANCHOR_BOTTOM_LEFT);
	CHECK(n && n->style.text.r == 0x12);       // anchor= matched
	CHECK(n && n->style.border_size == 7);     // the focused output
	CHECK(nth(0) && nth(0)->style.text.r == 0xFF);
	// One surface per anchor: each shows one, then a placeholder.
	list_t *visible = notify_visible();
	CHECK_INT(visible->length, 3);
	if (visible->length == 3) {
		struct swine_notification *v0 = visible->items[0];
		struct swine_notification *v1 = visible->items[1];
		struct swine_notification *v2 = visible->items[2];
		CHECK_STR(v0->summary, "three");
		CHECK_STR(v1->summary, "two");
		CHECK(v2->hidden && v2->style.anchor == NOTIFY_ANCHOR_BOTTOM_LEFT);
		char *text = first_line(v2);
		CHECK_STR(text, "1 of 2 hidden");
		free(text);
	}
	notify_free_visible(visible);
	fake_teardown();
}

static void test_mako_grouping(void) {
	fake_setup();
	CHECK(notify_parse_config(
		"group-by=app-name\n"
		"format=<b>%s</b>\\n%b\n", "test"));
	CHECK(run("notify-send -a mail one"));
	CHECK(run("notify-send -a chat hi"));
	CHECK(run("notify-send -a mail two"));
	// The group gathers where its first member was: mail two, mail one, hi
	CHECK(nth(0) && strcmp(nth(0)->summary, "two") == 0);
	CHECK(nth(1) && strcmp(nth(1)->summary, "one") == 0);
	CHECK(nth(2) && strcmp(nth(2)->summary, "hi") == 0);
	CHECK(nth(0) && nth(0)->group_index == 0 && nth(0)->group_count == 2);
	CHECK(nth(1) && nth(1)->group_index == 1);
	CHECK(nth(2) && nth(2)->group_index == -1);
	// The default [grouped] section: count in the format, members hidden.
	CHECK(nth(0) && !nth(0)->style.invisible);
	CHECK(nth(1) && nth(1)->style.invisible);
	char *text = first_line(nth(0));
	CHECK_STR(text, "(2) two");
	free(text);
	list_t *visible = notify_visible();
	CHECK_INT(visible->length, 2);
	notify_free_visible(visible);

	// Dismissing one ungroups the last of its group.
	CHECK(run("makoctl dismiss"));
	CHECK(nth(0) && strcmp(nth(0)->summary, "one") == 0 &&
		nth(0)->group_index == -1 && !nth(0)->style.invisible);
	text = first_line(nth(0));
	CHECK_STR(text, "one");
	free(text);

	CHECK(run("notify-send -a mail three"));
	CHECK(run("notify-send -a mail four"));
	CHECK_INT(notify_active()->length, 4);
	CHECK(run("makoctl dismiss -g"));
	CHECK_INT(notify_active()->length, 1);
	CHECK(nth(0) && strcmp(nth(0)->summary, "hi") == 0);
	CHECK(!run("makoctl dismiss -a -g"));
	CHECK(!run("makoctl dismiss -g -n 3"));

	// max-visible counts a group once.
	CHECK(notify_parse_config("max-visible=2\ngroup-by=app-name\n"
		"[grouped]\ninvisible=0\n", "test"));
	notify_dismiss_all(false);
	CHECK(run("notify-send -a a 1"));
	CHECK(run("notify-send -a b 2"));
	CHECK(run("notify-send -a b 3"));
	CHECK(run("notify-send -a b 4"));
	visible = notify_visible();
	CHECK_INT(visible->length, 4);   // b b b, a
	notify_free_visible(visible);
	CHECK(run("notify-send -a c 5"));
	visible = notify_visible();
	// c and the first b fill max-visible: as in mako, the rest of the b
	// group goes into the placeholder too.
	CHECK_INT(visible->length, 3);
	if (visible->length == 3) {
		struct swine_notification *more = visible->items[2];
		CHECK(more->hidden && more->hidden_count == 3);
	}
	notify_free_visible(visible);
	fake_teardown();
}

static void test_mako_sort(void) {
	fake_setup();
	CHECK(notify_parse_config("sort=+time\n", "test"));
	CHECK(run("notify-send a"));
	CHECK(run("notify-send b"));
	CHECK(nth(0) && strcmp(nth(0)->summary, "a") == 0);
	notify_dismiss_all(false);
	CHECK(notify_parse_config("sort=-priority\n", "test"));
	CHECK(run("notify-send -u low low1"));
	CHECK(run("notify-send -u critical crit1"));
	CHECK(run("notify-send normal1"));
	CHECK(run("notify-send -u low low2"));
	CHECK(run("notify-send -u critical crit2"));
	// critical first, newest first within each urgency
	const char *want[] = {"crit2", "crit1", "normal1", "low2", "low1"};
	for (int i = 0; i < 5; ++i) {
		CHECK(nth(i) && strcmp(nth(i)->summary, want[i]) == 0);
	}
	fake_teardown();
}

static void test_mako_actions(void) {
	fake_setup();
	CHECK(notify_parse_config(
		"on-button-middle=exec notify-send \"clicked $id\"\n"
		"[app-name=bell]\n"
		"on-notify=exec beep.exe --id ${id}\n", "test"));
	CHECK(run("notify-send -A default=Open -A later=Later mail"));
	uint32_t id = newest()->id;
	fake.ipc_event_count = 0;
	// makoctl invoke: the action goes out as an IPC event; with nobody
	// waiting for it the notification closes, as the sender would do.
	CHECK(run("makoctl invoke later"));
	CHECK(strstr(fake.ipc_last_event, "\"reason\":\"dismissed\"") != NULL);
	CHECK(strstr(fake.ipc_changes, "action,close,") != NULL);
	CHECK(notify_by_id(id) == NULL);
	CHECK_INT(notify_history()->length, 1);
	CHECK(!run("makoctl invoke"));   // nothing left

	// A waiting notify-send.exe closes it itself.
	uint32_t type;
	const char *args = "[\"-A\",\"yes=Yes\",\"Question?\"]";
	char *reply = ipc_handle_request(SWINE_IPC_NOTIFY_SEND, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "\"success\":true") != NULL);
	CHECK(strstr(reply, "\"id\":2") != NULL);
	free(reply);
	fake.ipc_changes[0] = '\0';
	CHECK(run("makoctl invoke -n 2 yes"));
	CHECK(strstr(fake.ipc_last_event, "\"action\":\"yes\"") != NULL);
	CHECK(notify_by_id(2) != NULL);
	CHECK(!run("makoctl invoke -n 2 no"));

	// Left click is invoke-default-action: it closes even without one.
	notify_button(2, 1);
	CHECK(notify_by_id(2) == NULL);

	// exec bindings get $id
	CHECK(run("notify-send hello"));
	id = newest()->id;
	notify_button(id, 2);
	char want[32];
	snprintf(want, sizeof(want), "clicked %u", id);
	CHECK(newest() && strcmp(newest()->summary, want) == 0);
	fake.spawn_calls = 0;
	CHECK(run("notify-send -a bell ding"));
	snprintf(want, sizeof(want), "beep.exe --id %u", newest()->id);
	CHECK_INT(fake.spawn_calls, 1);
	CHECK_STR(fake.last_spawn, want);

	// invoke-action <name>, dismiss --no-history
	CHECK(notify_parse_config("on-button-right=dismiss --no-history\n"
		"on-button-left=invoke-action open\n", "test"));
	int history = notify_history()->length;
	id = newest()->id;
	notify_button(id, 3);
	CHECK(notify_by_id(id) == NULL);
	CHECK_INT(notify_history()->length, history);
	CHECK(run("notify-send -A open=Open x"));
	id = newest()->id;
	fake.ipc_last_event[0] = '\0';
	fake.ipc_changes[0] = '\0';
	notify_button(id, 1);
	CHECK(strstr(fake.ipc_changes, "action,close,") != NULL);
	// actions=0 ignores them
	CHECK(notify_parse_config("actions=0\n", "test"));
	CHECK(run("notify-send -A default=Open x"));
	CHECK(!run("makoctl invoke"));
	fake_teardown();
}

static void test_makoctl_output(void) {
	fake_setup();
	notify_parse_config("", "empty");
	CHECK(run("notify-send -a Mail -c email -h string:desktop-entry:mail "
		"-A open=Open \"New mail\" Body"));
	CHECK(run("notify-send -u low \"\""));
	uint32_t type;
	const char *args = "[\"list\"]";
	char *reply = ipc_handle_request(SWINE_IPC_MAKOCTL, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "\"output\":\"Notification 2:\\n"
		"  Urgency: low\\n"
		"Notification 1: New mail\\n"
		"  App name: Mail\\n"
		"  Category: email\\n"
		"  Desktop entry: mail\\n"
		"  Urgency: normal\\n"
		"  Actions:\\n"
		"    open: Open\\n\"") != NULL);
	free(reply);
	args = "[\"list\",\"-j\"]";
	reply = ipc_handle_request(SWINE_IPC_MAKOCTL, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "\\\"app_icon\\\":null") != NULL);
	CHECK(strstr(reply, "\\\"actions\\\":{\\\"open\\\":\\\"Open\\\"}") != NULL);
	free(reply);
	args = "[\"mode\",\"-a\",\"dnd\"]";
	reply = ipc_handle_request(SWINE_IPC_MAKOCTL, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "\"output\":\"default\\ndnd\\n\"") != NULL);
	free(reply);
	args = "[\"frobnicate\"]";
	reply = ipc_handle_request(SWINE_IPC_MAKOCTL, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "\"success\":false") != NULL);
	free(reply);
	reply = ipc_handle_request(SWINE_IPC_MAKOCTL, "nope", 4, &type);
	CHECK(strstr(reply, "\"success\":false") != NULL);
	free(reply);
	CHECK(run("makoctl dismiss -a"));
	args = "[\"history\"]";
	reply = ipc_handle_request(SWINE_IPC_MAKOCTL, args,
		(uint32_t)strlen(args), &type);
	CHECK(strstr(reply, "Notification 1: New mail") != NULL);
	free(reply);
	// makoctl menu starts a program: exec runs it rather than the builtin
	fake.spawn_calls = 0;
	CHECK(run("exec makoctl menu wofi -d"));
	CHECK_INT(fake.spawn_calls, 1);
	fake_teardown();
}

static void test_notify_icons(void) {
	struct swine_notification n = {0};
	n.style.icon_path[0] = '\0';
	n.app_icon = "file:///C:/My%20Icons/a.png";
	list_t *c = notify_icon_candidates(&n);
	CHECK_INT(c->length, 1);
	CHECK_STR(c->items[0], "C:/My Icons/a.png");
	list_free_items_and_destroy(c);
	n.app_icon = "D:\\x.ico";
	c = notify_icon_candidates(&n);
	CHECK_INT(c->length, 1);
	CHECK_STR(c->items[0], "D:\\x.ico");
	list_free_items_and_destroy(c);
	n.app_icon = "mail";
	c = notify_icon_candidates(&n);
	CHECK_INT(c->length, 0);
	list_free_items_and_destroy(c);
	snprintf(n.style.icon_path, sizeof(n.style.icon_path),
		"C:\\icons;D:/more/:E:\\x");
	c = notify_icon_candidates(&n);
	CHECK_INT(c->length, 18);
	CHECK_STR(c->items[0], "C:\\icons\\mail.png");
	CHECK_STR(c->items[1], "C:\\icons\\mail.ico");
	CHECK_STR(c->items[6], "D:/more/mail.png");
	CHECK_STR(c->items[12], "E:\\x\\mail.png");
	list_free_items_and_destroy(c);
	n.app_icon = "";
	c = notify_icon_candidates(&n);
	CHECK_INT(c->length, 0);
	list_free_items_and_destroy(c);
}

void test_notify_all(void) {
	test_notify_send();
	test_mako_config();
	test_notify_send_options();
	test_mako_criteria();
	test_mako_output_anchor();
	test_mako_grouping();
	test_mako_sort();
	test_mako_actions();
	test_makoctl_output();
	test_notify_icons();
}
