#include <stdlib.h>
#include "ipc-client.h"
#include "json.h"
#include "list.h"
#include "log.h"
#include "regex.h"
#include "stringop.h"
#include "swinewm/input/keyboard.h"
#include "menu.h"
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

static bool rx(const char *pattern, const char *text) {
	char *error = NULL;
	struct swine_regex *re = swine_regex_compile(pattern, false, &error);
	if (!re) {
		free(error);
		return false;
	}
	bool result = swine_regex_search(re, text);
	swine_regex_free(re);
	return result;
}

static bool rx_invalid(const char *pattern) {
	char *error = NULL;
	struct swine_regex *re = swine_regex_compile(pattern, false, &error);
	swine_regex_free(re);
	bool invalid = re == NULL && error != NULL;
	free(error);
	return invalid;
}

static void test_regex(void) {
	// Unanchored search, as pcre2_match
	CHECK(rx("fox", "firefox.exe"));
	CHECK(!rx("Fox", "firefox.exe"));
	CHECK(rx("(?i)FIREFOX", "firefox.exe"));
	CHECK(rx("^firefox\\.exe$", "firefox.exe"));
	CHECK(!rx("^fire$", "firefox"));
	// Typical sway criteria
	CHECK(rx("^(Firefox|Chromium)$", "Chromium"));
	CHECK(!rx("^(Firefox|Chromium)$", "Chromium-browser"));
	CHECK(rx("Picture-in-Picture", "Picture-in-Picture"));
	CHECK(rx(".*- Mozilla Firefox$", "GitHub - Mozilla Firefox"));
	CHECK(rx("^Steam$|^Friends", "Friends List"));
	CHECK(rx("(?i)^pavucontrol", "Pavucontrol"));
	CHECK(rx("^(?!x)", "a") == false);   // lookahead: unsupported, invalid
	// Classes and escapes
	CHECK(rx("^[A-Z][a-z]+ [0-9]{2,3}$", "Window 42"));
	CHECK(!rx("^[A-Z][a-z]+ [0-9]{2,3}$", "Window 4"));
	CHECK(rx("^\\d+%$", "75%"));
	CHECK(rx("\\bWord\\b", "a Word here"));
	CHECK(!rx("\\bWord\\b", "Wordy"));
	CHECK(rx("^[^.]+$", "noext"));
	CHECK(!rx("^[^.]+$", "has.ext"));
	CHECK(rx("[[:digit:]]", "abc1"));
	CHECK(rx("^a.c$", "a\xC3\xA4" "c"));            // "." is one code point
	CHECK(rx("(?i)\xC3\xA4", "\xC3\x84"));     // ä matches Ä
	CHECK(rx("a{,2}", "a{,2}"));                // not a quantifier: literal
	// Quantifiers and backtracking
	CHECK(rx("^a*a$", "aaaa"));
	CHECK(rx("^(ab)+$", "ababab"));
	CHECK(!rx("^(ab)+$", "ababa"));
	CHECK(rx("^a.*?b$", "axxbxxb"));
	CHECK(rx("^(a*)*$", "aaa"));                // empty iterations end
	CHECK(rx("^(|a)b", "b"));
	CHECK(!rx("^(a+)+$", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
	// Errors
	CHECK(rx_invalid("(abc"));
	CHECK(rx_invalid("abc)"));
	CHECK(rx_invalid("[abc"));
	CHECK(rx_invalid("*a"));
	CHECK(rx_invalid("a\\1"));
	CHECK(rx_invalid("\\q"));
	CHECK(rx_invalid("[z-a]"));
}

static void test_log_levels(void) {
	enum swine_log_importance level = SWINE_ERROR;
	CHECK(swine_log_parse_importance("debug", &level));
	CHECK_INT(level, SWINE_DEBUG);
	CHECK(swine_log_parse_importance("info", &level));
	CHECK_INT(level, SWINE_INFO);
	CHECK(!swine_log_parse_importance("loud", &level));
	CHECK(!swine_log_parse_importance(NULL, &level));
	CHECK_INT(level, SWINE_INFO);
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

// swinemsg -t subscribe: JSON arrays pass unchanged; plain names and what
// Windows PowerShell before 7.3 makes of '["window","workspace"]' become one.
static void test_subscribe_payload(void) {
	const char *inputs[] = {
		"[window,workspace]",
		"window workspace",
		"window,workspace",
		"[\"window\",\"workspace\"]",
		"[ \"window\", \"workspace\" ]",
	};
	const char *expected[] = {
		"[\"window\",\"workspace\"]",
		"[\"window\",\"workspace\"]",
		"[\"window\",\"workspace\"]",
		"[\"window\",\"workspace\"]",
		"[ \"window\", \"workspace\" ]",
	};
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
		char *out = ipc_subscribe_payload(inputs[i]);
		CHECK_STR(out, expected[i]);
		free(out);
	}
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

static void test_menu(void) {
	CHECK(menu_match_score("Firefox", "") == 0);
	CHECK(menu_match_score("Firefox", "fire") > 0);
	CHECK(menu_match_score("Firefox", "FIRE") > 0);       // case
	CHECK(menu_match_score("Firefox", "fox") > 0);
	CHECK(menu_match_score("Firefox", "chrome") < 0);
	// every word must match, in any order
	CHECK(menu_match_score("Visual Studio Code", "code vis") > 0);
	CHECK(menu_match_score("Visual Studio Code", "code vim") < 0);
	// prefix beats word start beats inside a word
	CHECK(menu_match_score("Terminal", "term") > menu_match_score("Windows Terminal", "term"));
	CHECK(menu_match_score("Windows Terminal", "term") > menu_match_score("Determined", "term"));

	list_t *entries = create_list();
	list_add(entries, menu_entry_create("Windows Terminal", "wt.exe"));
	list_add(entries, menu_entry_create("Terminal Server Client", NULL));
	list_add(entries, menu_entry_create("Notepad", "notepad.exe"));
	list_add(entries, menu_entry_create("Terminal", "alacritty.exe"));
	list_t *found = menu_filter(entries, "term");
	CHECK_INT(found->length, 3);
	// prefix matches first, the shorter name first among them
	CHECK((size_t)found->items[0] == 3);
	CHECK((size_t)found->items[1] == 1);
	CHECK((size_t)found->items[2] == 0);
	list_free(found);

	// History: chosen entries move up.
	const char *path = "swinemenu-test-history.txt";
	remove(path);
	menu_history_record(path, "Windows Terminal");
	menu_history_record(path, "Windows Terminal");
	menu_history_record(path, "Notepad");
	menu_history_load(entries, path);
	CHECK_INT(((struct menu_entry *)entries->items[0])->uses, 2);
	CHECK_INT(((struct menu_entry *)entries->items[2])->uses, 1);
	found = menu_filter(entries, "term");
	CHECK((size_t)found->items[0] == 0);
	list_free(found);
	found = menu_filter(entries, "");
	CHECK_INT(found->length, 4);
	CHECK((size_t)found->items[0] == 0);
	CHECK((size_t)found->items[1] == 2);
	list_free(found);
	remove(path);
	for (int i = 0; i < entries->length; ++i) {
		menu_entry_free(entries->items[i]);
	}
	list_free(entries);
}

void test_common_all(void) {
	test_menu();
	test_list();
	test_split_args();
	test_argsep();
	test_strings();
	test_json();
	test_subscribe_payload();
	test_log_levels();
	test_regex();
	test_keys();
}
