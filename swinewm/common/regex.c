// A small backtracking regular-expression engine; see regex.h. Patterns
// are parsed into a tree of nodes and matched with explicit continuations,
// which keeps quantifiers, groups and alternation simple to backtrack.
// Matching is bounded (steps, depth, text length) so that a pathological
// pattern cannot hang the window manager.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "regex.h"
#include "stringop.h"

#define MAX_STEPS 200000
// Each level is up to three C stack frames: 1000 keeps even a debug build
// within half of Windows' 1 MB main-thread stack. A leading ".*" still
// covers 1000 characters of the (at most MAX_TEXT) text.
#define MAX_DEPTH 1000
#define MAX_TEXT 1024

enum node_type {
	N_CHAR,
	N_ANY,
	N_CLASS,
	N_BOL,
	N_EOL,
	N_WORDB,
	N_NWORDB,
	N_GROUP,
	N_REPEAT,
};

// "Perl" classes inside a character class
enum {
	PERL_DIGIT = 1 << 0,
	PERL_NDIGIT = 1 << 1,
	PERL_WORD = 1 << 2,
	PERL_NWORD = 1 << 3,
	PERL_SPACE = 1 << 4,
	PERL_NSPACE = 1 << 5,
};

struct range {
	uint32_t lo, hi;
};

struct char_class {
	struct range *ranges;
	int length, capacity;
	unsigned perl;
	bool negated;
};

struct node {
	enum node_type type;
	bool icase;
	uint32_t ch;                // N_CHAR
	struct char_class *cls;     // N_CLASS
	struct node **alts;         // N_GROUP: alternatives (sequences)
	int alt_count;
	struct node *child;         // N_REPEAT
	int min, max;               // max < 0: unbounded
	bool lazy;
	struct node *next;          // next node in the sequence
};

struct swine_regex {
	struct node *root;
};

// --- UTF-8 and character properties --------------------------------------

static int decode(const char *s, const char *end, uint32_t *cp) {
	if (s >= end || !*s) {
		return 0;
	}
	const unsigned char *u = (const unsigned char *)s;
	int len = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 :
		(u[0] & 0xF0) == 0xE0 ? 3 : (u[0] & 0xF8) == 0xF0 ? 4 : 1;
	if (s + len > end) {
		len = 1;
	}
	for (int i = 1; i < len; ++i) {
		if ((u[i] & 0xC0) != 0x80) {
			len = 1;
			break;
		}
	}
	if (len == 1) {
		*cp = u[0];
	} else if (len == 2) {
		*cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
	} else if (len == 3) {
		*cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
			(u[2] & 0x3F);
	} else {
		*cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
			((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
	}
	return len;
}

// Simple case mapping: ASCII and Latin-1.
static uint32_t to_lower(uint32_t c) {
	if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) {
		return c + 32;
	}
	return c;
}

static uint32_t to_upper(uint32_t c) {
	if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
		return c - 32;
	}
	return c;
}

static bool is_digit(uint32_t c) {
	return c >= '0' && c <= '9';
}

static bool is_space(uint32_t c) {
	return c == ' ' || (c >= '\t' && c <= '\r') || c == 0xA0 ||
		(c >= 0x2000 && c <= 0x200A) || c == 0x3000;
}

// With PCRE2_UCP (as sway compiles), \w covers letters in any script;
// approximated here by "non-ASCII and not punctuation or space".
static bool is_word(uint32_t c) {
	if (c < 0x80) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			is_digit(c) || c == '_';
	}
	return c >= 0xC0 && c != 0xD7 && c != 0xF7 &&
		!(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F);
}

// --- parsing -----------------------------------------------------------

struct parser {
	const char *p;
	const char *end;
	bool icase;
	char *error;
};

static void node_free(struct node *n);

static struct node *new_node(struct parser *ps, enum node_type type) {
	struct node *n = calloc(1, sizeof(*n));
	n->type = type;
	n->icase = ps->icase;
	return n;
}

static void class_add(struct char_class *cls, uint32_t lo, uint32_t hi) {
	if (cls->length == cls->capacity) {
		cls->capacity = cls->capacity ? cls->capacity * 2 : 8;
		cls->ranges = realloc(cls->ranges, sizeof(*cls->ranges) * cls->capacity);
	}
	cls->ranges[cls->length++] = (struct range){lo, hi};
}

static bool fail(struct parser *ps, const char *message) {
	if (!ps->error) {
		ps->error = swine_strdup(message);
	}
	return false;
}

static uint32_t next_cp(struct parser *ps) {
	uint32_t cp = 0;
	int len = decode(ps->p, ps->end, &cp);
	ps->p += len ? len : 0;
	return cp;
}

// After a backslash: a single-character escape (not a class or assertion).
// Returns false if the escape is not a single character.
static bool simple_escape(struct parser *ps, char c, uint32_t *out) {
	switch (c) {
	case 't': *out = '\t'; return true;
	case 'n': *out = '\n'; return true;
	case 'r': *out = '\r'; return true;
	case 'f': *out = '\f'; return true;
	case 'v': *out = '\v'; return true;
	case 'e': *out = 0x1B; return true;
	case 'a': *out = 0x07; return true;
	case 'x': {
		bool braces = *ps->p == '{';
		if (braces) {
			ps->p++;
		}
		uint32_t value = 0;
		int digits = 0;
		while (ps->p < ps->end && (braces || digits < 2)) {
			char h = *ps->p;
			int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ?
				h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
			if (v < 0) {
				break;
			}
			value = value * 16 + (uint32_t)v;
			ps->p++;
			digits++;
		}
		if (braces) {
			if (*ps->p != '}') {
				return fail(ps, "Missing } in \\x{...}");
			}
			ps->p++;
		}
		*out = value;
		return true;
	}
	default:
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9')) {
			return false;
		}
		*out = (unsigned char)c;
		return true;
	}
}

static unsigned perl_class(char c) {
	switch (c) {
	case 'd': return PERL_DIGIT;
	case 'D': return PERL_NDIGIT;
	case 'w': return PERL_WORD;
	case 'W': return PERL_NWORD;
	case 's': return PERL_SPACE;
	case 'S': return PERL_NSPACE;
	default: return 0;
	}
}

// [:alpha:] and friends, inside a class.
static bool posix_class(struct parser *ps, struct char_class *cls) {
	static const struct {
		const char *name;
		unsigned perl;
		uint32_t ranges[6];
	} names[] = {
		{"alpha", 0, {'a', 'z', 'A', 'Z', 0, 0}},
		{"digit", PERL_DIGIT, {0}},
		{"alnum", 0, {'a', 'z', 'A', 'Z', '0', '9'}},
		{"space", PERL_SPACE, {0}},
		{"upper", 0, {'A', 'Z', 0, 0, 0, 0}},
		{"lower", 0, {'a', 'z', 0, 0, 0, 0}},
		{"word", PERL_WORD, {0}},
		{"xdigit", 0, {'0', '9', 'a', 'f', 'A', 'F'}},
	};
	if (strncmp(ps->p, "[:", 2) != 0) {
		return false;
	}
	const char *close = strstr(ps->p + 2, ":]");
	if (!close) {
		return false;
	}
	size_t len = (size_t)(close - (ps->p + 2));
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		if (strlen(names[i].name) == len &&
				strncmp(names[i].name, ps->p + 2, len) == 0) {
			cls->perl |= names[i].perl;
			for (int r = 0; r < 6 && names[i].ranges[r]; r += 2) {
				class_add(cls, names[i].ranges[r], names[i].ranges[r + 1]);
			}
			ps->p = close + 2;
			return true;
		}
	}
	return false;
}

static struct node *parse_class(struct parser *ps) {
	struct node *n = new_node(ps, N_CLASS);
	n->cls = calloc(1, sizeof(*n->cls));
	if (*ps->p == '^') {
		n->cls->negated = true;
		ps->p++;
	}
	bool first = true;
	while (ps->p < ps->end && (*ps->p != ']' || first)) {
		first = false;
		if (posix_class(ps, n->cls)) {
			continue;
		}
		uint32_t lo;
		if (*ps->p == '\\') {
			ps->p++;
			char c = *ps->p++;
			unsigned perl = perl_class(c);
			if (perl) {
				n->cls->perl |= perl;
				continue;
			}
			if (c == 'b') {
				lo = '\b';
			} else if (!simple_escape(ps, c, &lo)) {
				fail(ps, "Unknown escape in character class");
				node_free(n);
				return NULL;
			}
		} else {
			lo = next_cp(ps);
		}
		uint32_t hi = lo;
		if (ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] != ']') {
			ps->p++;
			if (*ps->p == '\\') {
				ps->p++;
				char c = *ps->p++;
				if (perl_class(c) || !simple_escape(ps, c, &hi)) {
					fail(ps, "Invalid range in character class");
					node_free(n);
					return NULL;
				}
			} else {
				hi = next_cp(ps);
			}
			if (hi < lo) {
				fail(ps, "Range out of order in character class");
				node_free(n);
				return NULL;
			}
		}
		class_add(n->cls, lo, hi);
	}
	if (ps->p >= ps->end) {
		fail(ps, "Missing terminating ] for character class");
		node_free(n);
		return NULL;
	}
	ps->p++;   // ']'
	return n;
}

static struct node *parse_alternation(struct parser *ps);

// Parses a quantifier at ps->p into min/max; false if there is none.
static bool parse_quantifier(struct parser *ps, int *min, int *max,
		bool *lazy) {
	const char *start = ps->p;
	switch (*ps->p) {
	case '*': *min = 0; *max = -1; ps->p++; break;
	case '+': *min = 1; *max = -1; ps->p++; break;
	case '?': *min = 0; *max = 1; ps->p++; break;
	case '{': {
		const char *q = ps->p + 1;
		if (!is_digit((unsigned char)*q)) {
			return false;   // a literal "{", as in PCRE
		}
		long a = strtol(q, (char **)&q, 10), b = a;
		if (*q == ',') {
			q++;
			b = is_digit((unsigned char)*q) ? strtol(q, (char **)&q, 10) : -1;
		}
		if (*q != '}' || a > 1000 || b > 1000 || (b >= 0 && b < a)) {
			ps->p = start;
			return false;
		}
		*min = (int)a;
		*max = (int)b;
		ps->p = q + 1;
		break;
	}
	default:
		return false;
	}
	*lazy = *ps->p == '?';
	if (*lazy) {
		ps->p++;
	} else if (*ps->p == '+') {
		ps->p++;   // possessive: treated as greedy
	}
	return true;
}

// One atom; NULL with no error for "(?i)" and similar, which match nothing.
static struct node *parse_atom(struct parser *ps) {
	char c = *ps->p;
	switch (c) {
	case '(': {
		ps->p++;
		bool saved_icase = ps->icase;
		if (*ps->p == '?') {
			if (strncmp(ps->p, "?:", 2) == 0) {
				ps->p += 2;
			} else if (strncmp(ps->p, "?i)", 3) == 0 ||
					strncmp(ps->p, "?-i)", 4) == 0) {
				ps->icase = ps->p[1] == 'i';
				ps->p += ps->p[1] == 'i' ? 3 : 4;
				return NULL;
			} else if (strncmp(ps->p, "?i:", 3) == 0) {
				ps->icase = true;
				ps->p += 3;
			} else {
				fail(ps, "Unsupported group syntax");
				return NULL;
			}
		}
		struct node *inner = parse_alternation(ps);
		ps->icase = saved_icase;
		if (ps->error) {
			node_free(inner);
			return NULL;
		}
		if (*ps->p != ')') {
			fail(ps, "Missing closing parenthesis");
			node_free(inner);
			return NULL;
		}
		ps->p++;
		struct node *group = new_node(ps, N_GROUP);
		if (inner && inner->type == N_GROUP && !inner->next) {
			// An alternation: use its alternatives directly.
			group->alts = inner->alts;
			group->alt_count = inner->alt_count;
			inner->alts = NULL;
			inner->alt_count = 0;
			node_free(inner);
		} else {
			group->alts = calloc(1, sizeof(*group->alts));
			group->alts[0] = inner;
			group->alt_count = 1;
		}
		return group;
	}
	case '[':
		ps->p++;
		return parse_class(ps);
	case '.':
		ps->p++;
		return new_node(ps, N_ANY);
	case '^':
		ps->p++;
		return new_node(ps, N_BOL);
	case '$':
		ps->p++;
		return new_node(ps, N_EOL);
	case '*': case '+': case '?':
		fail(ps, "Quantifier does not follow a repeatable item");
		return NULL;
	case '\\': {
		ps->p++;
		char e = *ps->p++;
		if (e == '\0') {
			fail(ps, "Pattern ends with a backslash");
			return NULL;
		}
		unsigned perl = perl_class(e);
		if (perl) {
			struct node *n = new_node(ps, N_CLASS);
			n->cls = calloc(1, sizeof(*n->cls));
			n->cls->perl = perl;
			return n;
		}
		if (e == 'b' || e == 'B') {
			return new_node(ps, e == 'b' ? N_WORDB : N_NWORDB);
		}
		if (e >= '1' && e <= '9') {
			fail(ps, "Back-references are not supported");
			return NULL;
		}
		uint32_t cp;
		if (!simple_escape(ps, e, &cp)) {
			if (!ps->error) {
				fail(ps, "Unrecognized escape sequence");
			}
			return NULL;
		}
		struct node *n = new_node(ps, N_CHAR);
		n->ch = cp;
		return n;
	}
	default: {
		struct node *n = new_node(ps, N_CHAR);
		n->ch = next_cp(ps);
		return n;
	}
	}
}

static struct node *parse_sequence(struct parser *ps) {
	struct node *head = NULL, *tail = NULL;
	while (ps->p < ps->end && *ps->p != '|' && *ps->p != ')') {
		struct node *atom = parse_atom(ps);
		if (ps->error) {
			node_free(head);
			return NULL;
		}
		if (!atom) {
			continue;
		}
		int min, max;
		bool lazy;
		if (parse_quantifier(ps, &min, &max, &lazy)) {
			if (atom->type == N_BOL || atom->type == N_EOL ||
					atom->type == N_WORDB || atom->type == N_NWORDB) {
				node_free(atom);
				node_free(head);
				fail(ps, "Quantifier does not follow a repeatable item");
				return NULL;
			}
			struct node *rep = new_node(ps, N_REPEAT);
			rep->child = atom;
			rep->min = min;
			rep->max = max;
			rep->lazy = lazy;
			atom = rep;
		}
		if (tail) {
			tail->next = atom;
		} else {
			head = atom;
		}
		tail = atom;
	}
	return head;
}

static struct node *parse_alternation(struct parser *ps) {
	struct node *first = parse_sequence(ps);
	if (ps->error || *ps->p != '|') {
		return first;
	}
	struct node *group = new_node(ps, N_GROUP);
	group->alts = calloc(1, sizeof(*group->alts));
	group->alts[0] = first;
	group->alt_count = 1;
	while (*ps->p == '|') {
		ps->p++;
		struct node *alt = parse_sequence(ps);
		if (ps->error) {
			node_free(group);
			return NULL;
		}
		group->alts = realloc(group->alts,
			sizeof(*group->alts) * (size_t)(group->alt_count + 1));
		group->alts[group->alt_count++] = alt;
	}
	return group;
}

static void node_free(struct node *n) {
	while (n) {
		struct node *next = n->next;
		if (n->cls) {
			free(n->cls->ranges);
			free(n->cls);
		}
		for (int i = 0; i < n->alt_count; ++i) {
			node_free(n->alts[i]);
		}
		free(n->alts);
		node_free(n->child);
		free(n);
		n = next;
	}
}

struct swine_regex *swine_regex_compile(const char *pattern, bool icase,
		char **error) {
	struct parser ps = {pattern, pattern + strlen(pattern), icase, NULL};
	struct node *root = parse_alternation(&ps);
	if (!ps.error && ps.p < ps.end) {
		fail(&ps, "Unmatched closing parenthesis");
	}
	if (ps.error) {
		node_free(root);
		if (error) {
			*error = ps.error;
		} else {
			free(ps.error);
		}
		return NULL;
	}
	struct swine_regex *re = calloc(1, sizeof(*re));
	re->root = root;
	return re;
}

void swine_regex_free(struct swine_regex *re) {
	if (re) {
		node_free(re->root);
		free(re);
	}
}

// --- matching ----------------------------------------------------------

enum cont_kind {
	K_SEQ,      // continue with a sequence of nodes
	K_REPEAT,   // one iteration of a repeat has matched
};

struct cont {
	enum cont_kind kind;
	const struct node *node;
	int count;
	const char *start;
	const struct cont *next;
};

struct matcher {
	const char *text, *end;
	long steps;
	int depth;
};

static bool match(const struct node *n, const char *s, const struct cont *k,
		struct matcher *m);

static bool class_contains(const struct char_class *cls, uint32_t c) {
	for (int i = 0; i < cls->length; ++i) {
		if (c >= cls->ranges[i].lo && c <= cls->ranges[i].hi) {
			return true;
		}
	}
	unsigned p = cls->perl;
	return ((p & PERL_DIGIT) && is_digit(c)) ||
		((p & PERL_NDIGIT) && !is_digit(c)) ||
		((p & PERL_WORD) && is_word(c)) || ((p & PERL_NWORD) && !is_word(c)) ||
		((p & PERL_SPACE) && is_space(c)) || ((p & PERL_NSPACE) && !is_space(c));
}

static bool atom_matches(const struct node *n, uint32_t c) {
	switch (n->type) {
	case N_CHAR:
		return n->icase ? to_lower(c) == to_lower(n->ch) : c == n->ch;
	case N_ANY:
		return c != '\n';
	case N_CLASS: {
		bool in = class_contains(n->cls, c) || (n->icase &&
			(class_contains(n->cls, to_lower(c)) ||
			class_contains(n->cls, to_upper(c))));
		return in != n->cls->negated;
	}
	default:
		return false;
	}
}

static bool word_before(const struct matcher *m, const char *s) {
	if (s == m->text) {
		return false;
	}
	const char *p = s - 1;
	while (p > m->text && ((unsigned char)*p & 0xC0) == 0x80) {
		p--;
	}
	uint32_t c = 0;
	decode(p, m->end, &c);
	return is_word(c);
}

static bool word_at(const struct matcher *m, const char *s) {
	uint32_t c = 0;
	return decode(s, m->end, &c) && is_word(c);
}

static bool match_repeat(const struct node *r, const char *s, int count,
		const struct cont *after, struct matcher *m);

static bool match_cont(const struct cont *k, const char *s,
		struct matcher *m) {
	if (!k) {
		return true;
	}
	if (k->kind == K_SEQ) {
		return match(k->node, s, k->next, m);
	}
	// An iteration that matched nothing would loop forever.
	if (s == k->start && k->count > k->node->min) {
		return false;
	}
	return match_repeat(k->node, s, k->count, k->next, m);
}

static bool match_repeat(const struct node *r, const char *s, int count,
		const struct cont *after, struct matcher *m) {
	bool more = r->max < 0 || count < r->max;
	bool enough = count >= r->min;
	struct cont again = {K_REPEAT, r, count + 1, s, after};
	if (r->lazy) {
		if (enough && match_cont(after, s, m)) {
			return true;
		}
		return more && match(r->child, s, &again, m);
	}
	if (more && match(r->child, s, &again, m)) {
		return true;
	}
	return enough && match_cont(after, s, m);
}

static bool match(const struct node *n, const char *s, const struct cont *k,
		struct matcher *m) {
	if (++m->steps > MAX_STEPS || m->depth >= MAX_DEPTH) {
		return false;
	}
	if (!n) {
		return match_cont(k, s, m);
	}
	m->depth++;
	bool result = false;
	switch (n->type) {
	case N_CHAR:
	case N_ANY:
	case N_CLASS: {
		uint32_t c = 0;
		int len = decode(s, m->end, &c);
		result = len && atom_matches(n, c) && match(n->next, s + len, k, m);
		break;
	}
	case N_BOL:
		result = s == m->text && match(n->next, s, k, m);
		break;
	case N_EOL:
		result = (s == m->end || (*s == '\n' && s + 1 == m->end)) &&
			match(n->next, s, k, m);
		break;
	case N_WORDB:
	case N_NWORDB: {
		bool boundary = word_before(m, s) != word_at(m, s);
		result = boundary == (n->type == N_WORDB) && match(n->next, s, k, m);
		break;
	}
	case N_GROUP: {
		struct cont after = {K_SEQ, n->next, 0, NULL, k};
		for (int i = 0; i < n->alt_count && !result; ++i) {
			result = match(n->alts[i], s, &after, m);
		}
		break;
	}
	case N_REPEAT: {
		struct cont after = {K_SEQ, n->next, 0, NULL, k};
		result = match_repeat(n, s, 0, &after, m);
		break;
	}
	}
	m->depth--;
	return result;
}

bool swine_regex_search(const struct swine_regex *re, const char *text) {
	size_t len = strlen(text);
	struct matcher m = {text, text + (len > MAX_TEXT ? MAX_TEXT : len), 0, 0};
	const char *s = text;
	for (;;) {
		if (match(re->root, s, NULL, &m)) {
			return true;
		}
		uint32_t c;
		int step = decode(s, m.end, &c);
		if (!step || m.steps > MAX_STEPS) {
			return false;
		}
		s += step;
	}
}
