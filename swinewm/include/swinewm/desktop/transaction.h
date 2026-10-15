#ifndef _SWINE_TRANSACTION_H
#define _SWINE_TRANSACTION_H

// Arranges the tree and pushes every geometry/visibility/focus difference to
// the platform. Call after anything that changes the tree. sway batches this
// into atomic transactions with client acknowledgements; native windows have
// no such protocol, so this is a straight diff-and-apply.
void transaction_commit_dirty(void);

struct swine_view;
// The platform reports a focus change it observed (e.g. a mouse click), so
// the next commit does not re-request activation of the same window.
void transaction_note_activated(struct swine_view *view);
// Must be called before a view is freed.
void transaction_forget_view(struct swine_view *view);

#endif
