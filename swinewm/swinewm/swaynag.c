#include <stdlib.h>
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/platform.h"
#include "swinewm/swaynag.h"

void swaynag_show_config_errors(void) {
	if (!config || config->validating) {
		return;
	}
	if (!config->errors->length) {
		platform_swaynag_show(NULL);
		return;
	}
	char *details = swine_strdup("");
	for (int i = 0; i < config->errors->length; ++i) {
		char *next = format_str("%s%s%s", details,
			i ? "\n" : "", (char *)config->errors->items[i]);
		free(details);
		details = next;
	}
	platform_swaynag_show(details);
	free(details);
}
