#ifndef _SWINE_ARRANGE_H
#define _SWINE_ARRANGE_H

// Recomputes geometry and visibility for the whole tree. Cheap enough at
// desktop scale that the draft always arranges everything.
void arrange_root(void);

#endif
