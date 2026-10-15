#ifndef _SWINE_COMMANDS_UTIL_H
#define _SWINE_COMMANDS_UTIL_H

#include <stdbool.h>
#include "swinewm/tree/container.h"
#include "swinewm/tree/output.h"

// Helpers shared by focus.c and move.c.
bool parse_direction(const char *name, enum swine_direction *out);
// True if moving in dir travels along the axis of layout.
bool layout_parallel_to_direction(enum swine_container_layout layout,
		enum swine_direction dir);
// Resolves an output from a name or a direction relative to `from`.
struct swine_output *output_by_name_or_direction(const char *name,
		struct swine_output *from);

#endif
