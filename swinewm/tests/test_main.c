#include <stdio.h>
#include "log.h"
#include "test.h"

int test_failures = 0;
int test_checks = 0;

void test_common_all(void);
void test_config_all(void);
void test_tree_all(void);
void test_commands_all(void);
void test_bar_all(void);
void test_notify_all(void);
void test_criteria_all(void);
void test_marks_all(void);
void test_outputs_all(void);
void test_cursor_all(void);

int main(void) {
	swine_log_init(SWINE_SILENT);
	test_common_all();
	test_config_all();
	test_tree_all();
	test_commands_all();
	test_bar_all();
	test_notify_all();
	test_criteria_all();
	test_marks_all();
	test_outputs_all();
	test_cursor_all();
	printf("%d checks, %d failures\n", test_checks, test_failures);
	return test_failures ? 1 : 0;
}
