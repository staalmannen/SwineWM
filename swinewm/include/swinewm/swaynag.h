#ifndef _SWINE_SWAYNAG_H
#define _SWINE_SWAYNAG_H

// sway shows config errors with swaynag, a bar at the top of the screen.
// swinewm shows them as a critical notification of its built-in mako,
// which stays until it is dismissed or the config is loaded without
// errors.

// Shows the errors collected while reading the config (config->errors),
// replacing the previous message; removes it when there are none.
void swaynag_show_config_errors(void);

#endif
