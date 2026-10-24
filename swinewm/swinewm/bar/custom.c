// The scripts of custom/* modules. Ported from Waybar's
// src/modules/custom.cpp (Copyright (c) 2018 Alexis Rouillard, MIT): the
// same three workers (an interval, a signal, a continuous script), the same
// output parsing. Waybar's worker threads sleep and run commands
// synchronously; here the platform runs them in the background and this
// part is called back on the main thread, so the tree never sees a thread.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "log.h"
#include "stringop.h"
#include "swinewm/bar/custom.h"
#include "swinewm/bar/format.h"
#include "swinewm/platform.h"

enum script_kind {
	SCRIPT_NONE,         // only exec-if, nothing to run
	SCRIPT_INTERVAL,     // Waybar's delayWorker; "once" never repeats
	SCRIPT_SIGNAL,       // waitingWorker: once, then on its signal
	SCRIPT_CONTINUOUS,   // continuousWorker: one update per line
};

enum script_step {
	STEP_IDLE,
	STEP_EXEC_IF,
	STEP_EXEC,
};

struct bar_script {
	char *exec;
	char *exec_if;
	char *on_update;
	char *output_name;
	bool json;
	bool escape;
	bool exec_on_event;
	enum script_kind kind;
	long long interval_ms;   // -1: "once"
	long long restart_ms;    // 0: do not restart
	int signal;
	long long next_run;      // platform_now_ms when due, 0 if not scheduled
	uint64_t job;            // the running process, or 0
	enum script_step step;
	char *pending;           // what a one-shot run printed so far
	size_t pending_len;
	char *output;            // the last complete output
	int exit_code;
	bool has_output;
};

static list_t *scripts = NULL;   // struct bar_script *

// Seconds as Waybar reads "interval" and "restart-interval": at least 1 ms.
static long long seconds_ms(const struct json_value *v) {
	if (!v || v->type != JSON_NUMBER || v->number <= 0) {
		return 0;
	}
	double ms = v->number * 1000.0;
	return ms < 1 ? 1 : (long long)ms;
}

struct bar_script *bar_script_create(const struct json_value *cfg,
		const char *output_name) {
	const char *exec = json_get_string(cfg, "exec", NULL);
	const char *exec_if = json_get_string(cfg, "exec-if", NULL);
	if (!exec && !exec_if) {
		return NULL;
	}
	struct bar_script *s = calloc(1, sizeof(*s));
	s->exec = exec ? swine_strdup(exec) : NULL;
	s->exec_if = exec_if ? swine_strdup(exec_if) : NULL;
	const char *on_update = json_get_string(cfg, "on-update", NULL);
	s->on_update = on_update ? swine_strdup(on_update) : NULL;
	s->output_name = output_name ? swine_strdup(output_name) : NULL;
	const char *type = json_get_string(cfg, "return-type", "");
	s->json = strcmp(type, "json") == 0;
	s->escape = json_get_bool(cfg, "escape", false);
	s->exec_on_event = json_get_bool(cfg, "exec-on-event", true);

	struct json_value *interval = json_object_get(cfg, "interval");
	if (interval && interval->type == JSON_STRING &&
			strcmp(interval->string, "once") == 0) {
		s->interval_ms = -1;
	} else {
		s->interval_ms = seconds_ms(interval);
	}
	s->restart_ms = seconds_ms(json_object_get(cfg, "restart-interval"));
	struct json_value *signal = json_object_get(cfg, "signal");
	s->signal = signal && signal->type == JSON_NUMBER ? (int)signal->number : 0;

	// As Waybar's constructor picks the worker.
	if (signal && !interval && !json_object_get(cfg, "restart-interval")) {
		s->kind = SCRIPT_SIGNAL;
	} else if (s->interval_ms != 0) {
		s->kind = SCRIPT_INTERVAL;
	} else if (s->exec) {
		s->kind = SCRIPT_CONTINUOUS;
	} else {
		s->kind = SCRIPT_NONE;
	}
	if (s->kind != SCRIPT_NONE) {
		s->next_run = platform_now_ms();
		if (s->next_run == 0) {
			s->next_run = 1;
		}
	}
	if (!scripts) {
		scripts = create_list();
	}
	list_add(scripts, s);
	return s;
}

void bar_script_destroy(struct bar_script *s) {
	if (!s) {
		return;
	}
	if (s->job) {
		platform_script_stop(s->job);
	}
	for (int i = 0; scripts && i < scripts->length; ++i) {
		if (scripts->items[i] == s) {
			list_del(scripts, i);
			break;
		}
	}
	if (scripts && scripts->length == 0) {
		list_free(scripts);
		scripts = NULL;
	}
	free(s->exec);
	free(s->exec_if);
	free(s->on_update);
	free(s->output_name);
	free(s->pending);
	free(s->output);
	free(s);
}

static struct bar_script *find_job(uint64_t id) {
	for (int i = 0; id && scripts && i < scripts->length; ++i) {
		struct bar_script *s = scripts->items[i];
		if (s->job == id) {
			return s;
		}
	}
	return NULL;
}

static void set_output(struct bar_script *s, const char *output, int exit_code) {
	free(s->output);
	s->output = swine_strdup(output ? output : "");
	s->exit_code = exit_code;
	s->has_output = true;
	if (s->on_update) {
		char *command = format_str("exec %s", s->on_update);
		platform_queue_command(command);
		free(command);
	}
	platform_ui_refresh();
}

static void script_ended(struct bar_script *s, int status);

static void start_step(struct bar_script *s, enum script_step step) {
	const char *cmd = step == STEP_EXEC_IF ? s->exec_if : s->exec;
	s->step = step;
	free(s->pending);
	s->pending = NULL;
	s->pending_len = 0;
	s->job = platform_script_start(cmd, s->output_name);
	if (!s->job) {
		swine_log(SWINE_ERROR, "Unable to run '%s'", cmd);
		script_ended(s, 127);   // as a shell reports a missing command
	}
}

static void schedule_next(struct bar_script *s, long long now) {
	s->next_run = 0;
	if (s->kind == SCRIPT_INTERVAL && s->interval_ms > 0) {
		s->next_run = now + s->interval_ms;
	} else if (s->kind == SCRIPT_CONTINUOUS && s->restart_ms > 0) {
		s->next_run = now + s->restart_ms;
	}
}

static void run_now(struct bar_script *s) {
	s->next_run = 0;
	if (s->exec_if && s->kind != SCRIPT_CONTINUOUS) {
		start_step(s, STEP_EXEC_IF);
	} else if (s->exec) {
		start_step(s, STEP_EXEC);
	}
}

static void script_ended(struct bar_script *s, int status) {
	long long now = platform_now_ms();
	enum script_step step = s->step;
	s->job = 0;
	s->step = STEP_IDLE;
	if (step == STEP_EXEC_IF) {
		if (status != 0) {
			// Waybar hides the module until exec-if succeeds again.
			set_output(s, "", status);
			schedule_next(s, now);
		} else if (s->exec) {
			start_step(s, STEP_EXEC);
		} else {
			schedule_next(s, now);
		}
		return;
	}
	if (s->kind == SCRIPT_CONTINUOUS) {
		if (status != 0) {
			swine_log(SWINE_ERROR, "Bar script '%s' stopped unexpectedly, "
				"is it endless?", s->exec);
			set_output(s, "", status);
		}
	} else {
		// The whole output of a one-shot run, without the last line break.
		char *out = s->pending ? s->pending : swine_strdup("");
		s->pending = NULL;
		s->pending_len = 0;
		size_t len = strlen(out);
		while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
			out[--len] = '\0';
		}
		set_output(s, out, status);
		free(out);
	}
	schedule_next(s, now);
	platform_ui_refresh();
}

void bar_script_line(uint64_t id, const char *line) {
	struct bar_script *s = find_job(id);
	if (!s || s->step != STEP_EXEC) {
		return;   // exec-if's output is ignored
	}
	if (s->kind == SCRIPT_CONTINUOUS) {
		set_output(s, line, 0);
		return;
	}
	size_t n = strlen(line);
	s->pending = realloc(s->pending, s->pending_len + n + 2);
	memcpy(s->pending + s->pending_len, line, n);
	s->pending_len += n;
	s->pending[s->pending_len++] = '\n';
	s->pending[s->pending_len] = '\0';
}

void bar_script_exited(uint64_t id, int status) {
	struct bar_script *s = find_job(id);
	if (s) {
		script_ended(s, status);
	}
}

void bar_scripts_tick(long long now_ms) {
	for (int i = 0; scripts && i < scripts->length; ++i) {
		struct bar_script *s = scripts->items[i];
		if (s->next_run && now_ms >= s->next_run && !s->job) {
			run_now(s);
		}
	}
}

long long bar_scripts_next_due(void) {
	long long next = 0;
	for (int i = 0; scripts && i < scripts->length; ++i) {
		struct bar_script *s = scripts->items[i];
		if (s->next_run && !s->job && (!next || s->next_run < next)) {
			next = s->next_run;
		}
	}
	return next;
}

static void wake(struct bar_script *s) {
	if (s->kind == SCRIPT_CONTINUOUS || s->kind == SCRIPT_NONE || s->job) {
		return;
	}
	long long now = platform_now_ms();
	s->next_run = now ? now : 1;
	bar_scripts_tick(s->next_run);
}

void bar_scripts_signal(int signal) {
	for (int i = 0; scripts && i < scripts->length; ++i) {
		struct bar_script *s = scripts->items[i];
		if (s->signal == signal && signal > 0) {
			wake(s);
		}
	}
}

void bar_script_event(struct bar_script *s) {
	if (s && s->exec_on_event) {
		wake(s);
	}
}

static char *maybe_escape(const char *text, bool escape) {
	return escape ? markup_escape(text) : swine_strdup(text ? text : "");
}

void bar_script_parse(const char *output, bool json, bool escape,
		struct bar_script_result *out) {
	memset(out, 0, sizeof(*out));
	out->classes = create_list();
	if (json) {
		// The first line holds one JSON object.
		size_t len = strcspn(output, "\n");
		char *line = swine_strndup(output, len);
		char *error = NULL;
		struct json_value *v = json_parse(line, &error);
		free(error);
		free(line);
		out->text = maybe_escape(json_get_string(v, "text", ""), escape);
		out->alt = maybe_escape(json_get_string(v, "alt", ""), escape);
		out->tooltip = maybe_escape(json_get_string(v, "tooltip", ""), escape);
		struct json_value *cls = json_object_get(v, "class");
		if (cls && cls->type == JSON_STRING) {
			list_add(out->classes, swine_strdup(cls->string));
		} else if (cls && cls->type == JSON_ARRAY) {
			for (int i = 0; i < cls->items->length; ++i) {
				struct json_value *c = cls->items->items[i];
				if (c->type == JSON_STRING) {
					list_add(out->classes, swine_strdup(c->string));
				}
			}
		}
		struct json_value *pct = json_object_get(v, "percentage");
		if (pct && pct->type == JSON_NUMBER) {
			out->percentage = (int)lround(pct->number);
		}
		json_value_free(v);
		return;
	}
	// i3blocks: the text, the tooltip, a class.
	const char *p = output;
	for (int i = 0; i < 3 && p; ++i) {
		size_t len = strcspn(p, "\n");
		char *line = swine_strndup(p, len);
		if (i == 0) {
			out->text = maybe_escape(line, escape);
			// Waybar keeps the first line unescaped as the tooltip.
			out->tooltip = swine_strdup(line);
		} else if (i == 1) {
			free(out->tooltip);
			out->tooltip = maybe_escape(line, escape);
		} else {
			list_add(out->classes, swine_strdup(line));
		}
		free(line);
		p = p[len] == '\n' ? p + len + 1 : NULL;
	}
	if (!out->text) {
		out->text = swine_strdup("");
	}
	if (!out->tooltip) {
		out->tooltip = swine_strdup("");
	}
	out->alt = swine_strdup("");
}

void bar_script_result(struct bar_script *s, struct bar_script_result *out) {
	if (!s->has_output || !s->output[0] || s->exit_code != 0) {
		memset(out, 0, sizeof(*out));
		out->hidden = true;
		out->classes = create_list();
		return;
	}
	bar_script_parse(s->output, s->json, s->escape, out);
}

void bar_script_result_finish(struct bar_script_result *r) {
	free(r->text);
	free(r->alt);
	free(r->tooltip);
	if (r->classes) {
		list_free_items_and_destroy(r->classes);
	}
	memset(r, 0, sizeof(*r));
}
