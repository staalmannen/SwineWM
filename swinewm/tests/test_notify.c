#include <stdlib.h>
#include <string.h>
#include "fake_platform.h"
#include "ipc.h"
#include "swinewm/config.h"
#include "swinewm/ipc-server.h"
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

	// "Spam Bot" is invisible; max-visible=2 shows the newest two others.
	list_t *visible = notify_visible();
	CHECK_INT(visible->length, 2);
	CHECK_STR(((struct swine_notification *)visible->items[0])->summary, "four");
	CHECK_STR(((struct swine_notification *)visible->items[1])->summary, "two");
	list_free(visible);
	CHECK_INT(notify_hidden_count(), 1);

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
	list_free(visible);
	CHECK(run("makoctl mode -t do-not-disturb"));
	visible = notify_visible();
	CHECK_INT(visible->length, 1);
	list_free(visible);

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

void test_notify_all(void) {
	test_notify_send();
	test_mako_config();
}
