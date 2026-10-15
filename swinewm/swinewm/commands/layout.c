#include <string.h>
#include "stringop.h"
#include "swinewm/commands.h"
#include "swinewm/config.h"
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"
#include "swinewm/tree/root.h"
#include "swinewm/tree/workspace.h"

static enum swine_container_layout parse_layout_string(const char *s) {
	if (swine_strcasecmp(s, "splith") == 0) {
		return L_HORIZ;
	} else if (swine_strcasecmp(s, "splitv") == 0) {
		return L_VERT;
	} else if (swine_strcasecmp(s, "tabbed") == 0) {
		return L_TABBED;
	} else if (swine_strcasecmp(s, "stacking") == 0) {
		return L_STACKED;
	}
	return L_NONE;
}

static const char expected_syntax[] =
	"Expected 'layout default|tabbed|stacking|splitv|splith' or "
	"'layout toggle [split|all]' or "
	"'layout toggle [split|tabbed|stacking|splitv|splith] "
	"[split|tabbed|stacking|splitv|splith]...'";

static enum swine_container_layout toggle_split_layout(
		enum swine_container_layout layout,
		enum swine_container_layout prev_split_layout,
		struct swine_output *output) {
	if (layout == L_HORIZ) {
		return L_VERT;
	} else if (layout == L_VERT) {
		return L_HORIZ;
	} else if (prev_split_layout != L_NONE) {
		return prev_split_layout;
	}
	return output_get_default_layout(output);
}

static enum swine_container_layout get_layout_toggle(int argc, char **argv,
		enum swine_container_layout layout,
		enum swine_container_layout prev_split_layout,
		struct swine_output *output) {
	if (argc == 1) {
		return toggle_split_layout(layout, prev_split_layout, output);
	}
	if (argc == 2) {
		if (swine_strcasecmp(argv[1], "split") == 0) {
			return toggle_split_layout(layout, prev_split_layout, output);
		}
		if (swine_strcasecmp(argv[1], "all") == 0) {
			return layout == L_HORIZ ? L_VERT :
				layout == L_VERT ? L_STACKED :
				layout == L_STACKED ? L_TABBED : L_HORIZ;
		}
		return L_NONE;
	}
	int curr = 1;
	for (; curr < argc; curr++) {
		enum swine_container_layout parsed = parse_layout_string(argv[curr]);
		if (parsed == layout || (strcmp(argv[curr], "split") == 0 &&
				(layout == L_VERT || layout == L_HORIZ))) {
			break;
		}
	}
	for (int i = curr + 1; i != curr; ++i) {
		// cycle round to find next valid layout
		if (i >= argc) {
			i = 1;
		}
		enum swine_container_layout parsed = parse_layout_string(argv[i]);
		if (parsed != L_NONE) {
			return parsed;
		}
		if (strcmp(argv[i], "split") == 0) {
			return toggle_split_layout(layout, prev_split_layout, output);
		}
	}
	return L_NONE;
}

static enum swine_container_layout get_layout(int argc, char **argv,
		enum swine_container_layout layout,
		enum swine_container_layout prev_split_layout,
		struct swine_output *output) {
	enum swine_container_layout parsed = parse_layout_string(argv[0]);
	if (parsed != L_NONE) {
		return parsed;
	}
	if (swine_strcasecmp(argv[0], "default") == 0) {
		// sway returns prev_split_layout, which is "none" until the layout
		// has been changed once; fall back to the default orientation.
		return prev_split_layout != L_NONE ? prev_split_layout :
			output_get_default_layout(output);
	}
	if (swine_strcasecmp(argv[0], "toggle") == 0) {
		return get_layout_toggle(argc, argv, layout, prev_split_layout, output);
	}
	return L_NONE;
}

struct cmd_results *cmd_layout(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "layout", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	struct swine_container *container = config->handler_context.container;
	struct swine_workspace *workspace = config->handler_context.workspace;
	if (!workspace) {
		return cmd_results_new(CMD_FAILURE, "No workspace to change");
	}
	if (container && container_is_floating(container)) {
		return cmd_results_new(CMD_FAILURE,
			"Unable to change layout of floating windows");
	}

	// Operate on parent container, like i3.
	if (container) {
		container = container->parent;
		// If parent has only a single child operate on its parent and
		// flatten once, like i3
		if (container && container->children->length == 1) {
			struct swine_container *parent = container->parent;
			if (parent && parent->children->length == 1) {
				struct swine_container *child = container->children->items[0];
				container_replace(container, child);
				container_destroy(container);
				container = parent;
			}
		}
	}

	enum swine_container_layout new_layout = L_NONE;
	enum swine_container_layout old_layout = L_NONE;
	if (container) {
		old_layout = container->layout;
		new_layout = get_layout(argc, argv, container->layout,
			container->prev_split_layout, workspace->output);
	} else {
		old_layout = workspace->layout;
		new_layout = get_layout(argc, argv, workspace->layout,
			workspace->prev_split_layout, workspace->output);
	}
	if (new_layout == L_NONE) {
		return cmd_results_new(CMD_INVALID, "%s", expected_syntax);
	}
	if (new_layout != old_layout) {
		if (container) {
			if (old_layout != L_TABBED && old_layout != L_STACKED) {
				container->prev_split_layout = old_layout;
			}
			container->layout = new_layout;
		} else if (config->handler_context.container) {
			// i3 avoids changing workspace layouts with a new container
			container = workspace_wrap_children(workspace);
			container->layout = new_layout;
		} else {
			if (old_layout != L_TABBED && old_layout != L_STACKED) {
				workspace->prev_split_layout = old_layout;
			}
			workspace->layout = new_layout;
		}
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
