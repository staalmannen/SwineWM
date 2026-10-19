#ifndef _SWINE_SWAYNAG_H
#define _SWINE_SWAYNAG_H

// Shows config errors in SwineWM's persistent native nag window.

// Shows the errors collected while reading the config (config->errors),
// replacing the previous message; removes it when there are none.
void swaynag_show_config_errors(void);

#endif
