#include <stdlib.h>
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/notify.h"
#include "swinewm/platform.h"
#include "swinewm/swaynag.h"

#define MAX_LISTED 5

static uint32_t config_errors_id;

void swaynag_show_config_errors(void) {
	if (!config || config->validating) {
		return;
	}
	if (!config->errors->length) {
		if (config_errors_id) {
			notify_dismiss(config_errors_id, false);
			config_errors_id = 0;
			platform_ui_refresh();
		}
		return;
	}
	// The details, as swaynag's "-l": the first few errors.
	int listed = config->errors->length < MAX_LISTED ?
		config->errors->length : MAX_LISTED;
	char *body = swine_strdup("");
	for (int i = 0; i < listed; ++i) {
		char *next = format_str("%s%s\n", body,
			(char *)config->errors->items[i]);
		free(body);
		body = next;
	}
	char *more = config->errors->length > listed ?
		format_str("... and %d more; ", config->errors->length - listed) :
		swine_strdup("");
	char *next = format_str("%s%ssee swinewm.log", body, more);
	free(more);
	free(body);
	body = next;
	struct notify_params params = {
		.app_name = "swinewm",
		.summary = "There are errors in your config file",
		.body = body,
		.category = "swaynag",
		.urgency = NOTIFY_URGENCY_CRITICAL,
		.expire_timeout = 0,
		.replaces_id = config_errors_id,
	};
	config_errors_id = notify_add(&params, platform_now_ms());
	free(body);
	platform_ui_refresh();
}
