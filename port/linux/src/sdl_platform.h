/*
SDL_PLATFORM.H

Window, OpenGL context and input state shared by the renderer and the
controller emulation (see sdl_platform.c).
*/

#ifndef __HALO_LINUX_SDL_PLATFORM_H
#define __HALO_LINUX_SDL_PLATFORM_H

#include <SDL3/SDL_scancode.h>
#include <stddef.h>

#define PLATFORM_MOUSE_BUTTON_COUNT 8

struct platform_input_state
{
	unsigned char keys[SDL_SCANCODE_COUNT];
	unsigned char mouse_buttons[PLATFORM_MOUSE_BUTTON_COUNT]; /* SDL_BUTTON_* */
	float mouse_dx, mouse_dy;
	float mouse_wheel;
	BOOL focused;
	BOOL mouse_released;
	/* the mouse drives the menus' pointer (platform_ui_pointer_set_active)
	instead of the controller */
	BOOL ui_pointer;
	/* a menu is up (platform_menus_set_active): the keys drive the first
	controller, to move about it, instead of the player's actions */
	BOOL menus;
};

/* an input of the keyboard and mouse's controls (xinput_sdl.c): a scancode,
a mouse button (INPUT_MOUSE + SDL_BUTTON_*), or the wheel */
#define INPUT_MOUSE SDL_SCANCODE_COUNT
#define INPUT_WHEEL (INPUT_MOUSE + PLATFORM_MOUSE_BUTTON_COUNT)
#define INPUT_WHEEL_UP (INPUT_WHEEL + 1)
#define INPUT_WHEEL_DOWN (INPUT_WHEEL + 2)
/* an input by its name in config.toml ("W", "Mouse Left"), -1 if none */
int halo_input_from_name(const char *name);
void halo_input_name(int input, char *name, size_t size);

/* whether a menu is up (halo_ui_pointer_update, every frame) */
void platform_menus_set_active(BOOL active);
/* rebinding a control (Settings > Controls Setup): from now the next key,
mouse button or wheel turn is taken, and none reaches the game or the
menus; nor does the keyboard after, until every key and button is up */
void platform_binding_capture_begin(void);
/* 0 while waiting; else the capture ends: 1 with the input, 2 for none
(Delete), 3 if cancelled (Escape) */
int platform_binding_capture_poll(int *input);

struct platform_keystroke
{
	BYTE virtual_key;
	CHAR ascii;
	BYTE flags;
};

/* where the game drew its picture, in the window's pixels (the touch
controls' overlay, d3d8_gl.c) */
struct platform_touch_rect
{
	int left, top;
	int width, height;
};

BOOL platform_sdl_initialize(void);
/* creates the window and makes its OpenGL context current on this thread */
BOOL platform_video_initialize(unsigned long width, unsigned long height);
#ifndef HALO_ANDROID
BOOL platform_screen_mode(long *width, long *height);
#endif
/* Video Setup's resolutions (port/linux/game/menu_tags.c), in pixels: the
display's modes the game can draw at, largest first, each once, but the
display's own (Native), and display.resolution's where it is none of them;
how many. None on Android. */
int platform_display_resolutions(long *widths, long *heights, int maximum);
/* Video Setup's window sizes: those of each shape (4:3, 16:10, 16:9, 21:9)
that fit the desktop, and display.window_size's; how many. None on
Android. */
int platform_window_sizes(long *widths, long *heights, int maximum);
void platform_video_drawable_size(int *width, int *height);
/* the window's mode and size and V-Sync, from config.toml as Settings has
just written it (the main thread's) */
void platform_display_apply(void);
void platform_video_swap(void);
/* frames between the 30 Hz ticks at the display's refresh rate, unless
display.interpolation is false (port/linux/game/render_interpolation.c) */
int halo_interpolation_enabled(void);
void platform_mouse_capture(BOOL capture);

/* main thread only; a no-op elsewhere */
void platform_pump_events(void);
/* a snapshot of the input state; consume_motion resets the mouse deltas */
void platform_input_read(struct platform_input_state *state, BOOL consume_motion);
/* the pointer in the menus (d3d8_gl.c, halo_ui_pointer_update) */
struct platform_ui_pointer
{
	/* the mouse, in the window's coordinates, as SDL reports them */
	float x, y;
	float click_x, click_y;
	/* a finger, as a fraction of the window (Android has no mouse, and its
	window fills the display) */
	float nx, ny, click_nx, click_ny;
	BOOL moved;
	int left_clicks, right_clicks;
	int wheel_steps;
};
void platform_ui_pointer_set_active(BOOL active);
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer);
#ifndef HALO_ANDROID
void platform_video_window_size(int *width, int *height);
#endif

#ifdef HALO_ANDROID
/* ---------- the touch controls (touch_sdl.c) */

/* a finger went down, moved or came up, at a fraction of the window with y
down (SDL_EVENT_FINGER_*, sdl_platform.c) */
void platform_touch_finger(Uint64 finger, BOOL down, float normalized_x, float normalized_y);
/* the window lost the focus: no finger is down any more */
void platform_touch_release_all(void);
/* what the game says (ui_widget.c, through halo_ui_pointer_update): a menu is
up, and the game's on-screen keyboard with it */
void platform_touch_menus(BOOL menus, BOOL keyboard);
/* what the game says (player_control.c): a game is in progress, so the
player's controls are up (an intro waits for the player to do something) */
void platform_touch_player(BOOL playing);
/* the controller the touch controls make up, merged into player 1's pad
(xinput_sdl.c) */
void platform_touch_gamepad(XINPUT_GAMEPAD *pad);
/* the view's turn since the last call, in heights of the picture with y
down (xinput_sdl.c's halo_linux_mouse_look) */
void platform_touch_look(float *x, float *y);
/* how far a finger's drag turns the view: the options panel's slider once it
has been opened, and input.touch_sensitivity until then (the panel writes it
into config.toml, so it is read again the next time the game starts) */
float platform_touch_sensitivity(void);
/* the controls, drawn over the picture at presentation (d3d8_gl.c) */
void platform_touch_draw(const struct platform_touch_rect *rect);
/* what the fingers did to a menu's pointer since the last call, put into
`pointer` (touch_sdl.c). The event pump calls this, holding the lock that
guards the pointer: the touch controls take no lock of their own, since they
run on the game's thread with everything else */
void platform_touch_pointer(struct platform_ui_pointer *pointer);
/* the device's vibration, 0 to 100: a press of a touch control, or the
rumble the game asks its controller for, which has none here (xinput_sdl.c) */
#define PLATFORM_HAPTIC_PRESS 45
void platform_haptic(int strength, int milliseconds);

/* port/android/host/host_sdl.c, over SDL3's haptics */
int host_sdl_haptic_pulse(unsigned int strength, unsigned int milliseconds);
#endif

BOOL platform_next_keystroke(struct platform_keystroke *keystroke);
/* the multiplayer scoreboard (game_engine.c) open or not: while it is, the
mouse wheel and Page Up/Down scroll it instead of switching weapons; how
far they moved it since the last call (notches and pages, down positive) */
void platform_scoreboard_scroll(int open, long *notches, long *pages);

#endif
