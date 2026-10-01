/*
PORT_CONFIG.H

The native ports' settings, read from config.toml (port_config.c): next to
the executable on the desktop, in the data folder (the one holding maps/)
on Android. A missing file is written with the defaults. Each setting can
also be set for one run with its HALO_* environment variable, which wins
over the file (the tools and the Android app pass settings that way).

Settings are named "section.key", as in the file: "display.vsync".
*/

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);
/* sets a boolean setting, and writes it into config.toml (only its line
changes); 1 on success */
int config_write_boolean(const char *name, int value);
/* the same for a real setting, which is what a slider in the game offers
while it runs (the touch controls' options panel, touch_sdl.c) */
int config_write_real(const char *name, double value);

#endif
