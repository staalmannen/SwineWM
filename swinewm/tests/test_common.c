#include <stdlib.h>
#include "json.h"
#include "list.h"
#include "stringop.h"
#include "swinewm/input/keyboard.h"
#include "test.h"

static void test_list(void) {
	list_t *list = create_list();
	int a = 1, b = 2, c = 3;
	for (int i = 0; i < 25; ++i) {
		list_add(list, &a);
	}
	CHECK_INT(list->length, 25);
	list_insert(list, 0, &b);
	list_insert(list, list->length, &c);
	CHECK(list->items[0] == &b);
	CHECK(list->items[list->length - 1] == &c);
	CHECK_INT(list_find(list, &c), 26);
	list_del(list, 0);
	CHECK_INT(list_find(list, &b), -1);
	list_move_to_end(list, &a);
	CHECK(list->items[list->length - 1] == &a);
	list_free(list);
}

static void test_split_args(void) {
	int argc;
	char **argv = split_args("bindsym $mod+Return exec 'my term' --flag", &argc);
	CHECK_INT(argc, 5);
	CHECK_STR(argv[0], "bindsym");
	CHECK_STR(argv[3], "'my term'");
	strip_quotes(argv[3]);
	CHECK_STR(argv[3], "my term");
	char *joined = join_args(argv, argc);
	CHECK_STR(joined, "bindsym $mod+Return exec my term --flag");
	free(joined);
	free_argv(argc, argv);

	argv = split_args("   ", &argc);
	CHECK_INT(argc, 0);
	free_argv(argc, argv);
}

static void test_argsep(void) {
	char buf[] = "focus left; exec \"a;b\", kill";
	char *head = buf;
	char matched;
	CHECK_STR(argsep(&head, ";,", &matched), "focus left");
	CHECK(matched == ';');
	CHECK_STR(argsep(&head, ";,", &matched), " exec \"a;b\"");
	CHECK(matched == ',');
	CHECK_STR(argsep(&head, ";,", &matched), " kill");
	CHECK(head == NULL);
}

static void test_strings(void) {
	char s[] = "  \t hello world \n";
	strip_whitespace(s);
	CHECK_STR(s, "hello world");
	CHECK(swine_strcasecmp("Return", "return") == 0);
	CHECK(swine_strcasecmp("a", "b") < 0);
	CHECK(parse_boolean("yes", false));
	CHECK(!parse_boolean("off", true));
	CHECK(parse_boolean("toggle", false));
	char *f = format_str("%s-%d", "ws", 3);
	CHECK_STR(f, "ws-3");
	free(f);
	list_t *parts = split_string("Mod4++Shift+q", "+");
	CHECK_INT(parts->length, 3);
	list_free_items_and_destroy(parts);
}

static void test_json(void) {
	struct json_writer w;
	json_init(&w);
	json_begin_object(&w);
	json_key(&w, "a");
	json_int(&w, 1);
	json_key(&w, "list");
	json_begin_array(&w);
	json_string(&w, "x\"y");
	json_bool(&w, true);
	json_null(&w);
	json_end_array(&w);
	json_key(&w, "o");
	json_begin_object(&w);
	json_end_object(&w);
	json_end_object(&w);
	char *out = json_finish(&w);
	CHECK_STR(out, "{\"a\":1,\"list\":[\"x\\\"y\",true,null],\"o\":{}}");
	free(out);
}

static void test_keys(void) {
	uint32_t code = 0;
	CHECK(keyname_to_keycode("Return", &code) && code == 0x0D);
	CHECK(keyname_to_keycode("q", &code) && code == 'Q');
	CHECK(keyname_to_keycode("Q", &code) && code == 'Q');
	CHECK(keyname_to_keycode("1", &code) && code == '1');
	CHECK(keyname_to_keycode("F12", &code) && code == 0x7B);
	CHECK(keyname_to_keycode("minus", &code) && code == 0xBD);
	CHECK(keyname_to_keycode("XF86AudioMute", &code) && code == 0xAD);
	CHECK(!keyname_to_keycode("NoSuchKey", &code));
	CHECK_STR(keycode_to_name('H'), "h");
	CHECK_INT(get_modifier_mask_by_name("Mod4"), SWINE_MOD_LOGO);
	CHECK_INT(get_modifier_mask_by_name("mod1"), SWINE_MOD_ALT);
	CHECK_INT(get_modifier_mask_by_name("Control"), SWINE_MOD_CTRL);
	CHECK_INT(get_modifier_mask_by_name("q"), 0);
}

void test_common_all(void) {
	test_list();
	test_split_args();
	test_argsep();
	test_strings();
	test_json();
	test_keys();
}
