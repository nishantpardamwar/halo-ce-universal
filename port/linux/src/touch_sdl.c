/*
TOUCH_SDL.C

The screen's own controls, on Android: the player's fingers drive the game.

The game only knows controllers, so the touch controls are a controller this
file makes up and hands to the gamepad emulation (xinput_sdl.c), which merges
it into player 1's pad: a stick to walk with, buttons for what the Xbox
controller's face buttons and triggers do, and a part of the screen a finger
drags to turn the view (the same direct aim the mouse gets, in
halo_linux_mouse_look). The menus take taps as the mouse takes them on the
desktop (halo_ui_pointer.h, halo_ui_pointer_update), and the game's own
on-screen keyboard, which is made for a controller too, gets a d-pad and two
buttons.

Which of the three layouts is up follows the game: the touch controls of a
game in progress, a menu's, or the keyboard's. The game says so
(platform_touch_menus, from ui_widget.c, and platform_touch_player, from
player_control.c). A signal that stops arriving (a loading screen) takes the
controls away, so they cannot press buttons into a game that is not
listening; a game in progress keeps them up from the start of a level, since
an intro waits for the player to do something.

The overlay is drawn by d3d8_gl.c over the picture at presentation, from one
program: a rounded box, a circle when the corner's radius is half its size, a
hole in the middle, and a glyph of a 5x7 font for the labels. It is the only
thing drawn after the game, and it changes no state the renderer relies on:
the renderer forgets its GL state after the swap (xgpu_gl_state_invalidate)
and sets it again for the next frame, and the overlay has no vertex array,
buffer or texture of its own.

The layout is in heights of the picture, so the controls are the same shape
on every display and the same drag turns the view the same way. A press
between two of the game's polls is remembered until the next one, so a tap is
never lost. The device vibrates for a press and for the rumble the game asks
its controller for (xinput_sdl.c), if it has a motor.

The settings (config.toml's [input]) are input.touch, input.touch_sensitivity,
input.touch_invert, input.touch_size and input.touch_left.

Everything here runs on the game's thread: the events are pumped by
platform_pump_events (which only runs on the thread that owns the window), the
controller is read by the game's input update, and the overlay is drawn where
the game presents. So none of this needs a lock.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"

#ifdef HALO_ANDROID

#include "gl.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

/* ---------- the labels' font

Five columns of seven rows a glyph, a column's lowest bit its top row, so a
label is drawn one quad a character with the same program as the buttons. */

#define GLYPH_COLUMNS 5
#define GLYPH_ROWS 7
#define NUMBER_OF_GLYPHS 31 /* A to Z, then ^, v, <, > and the space */

static const unsigned char touch_font[NUMBER_OF_GLYPHS][GLYPH_COLUMNS] =
{
	{ 0x7e, 0x09, 0x09, 0x09, 0x7e },	/* A */
	{ 0x7f, 0x49, 0x49, 0x49, 0x36 },	/* B */
	{ 0x3e, 0x41, 0x41, 0x41, 0x22 },	/* C */
	{ 0x7f, 0x41, 0x41, 0x41, 0x3e },	/* D */
	{ 0x7f, 0x49, 0x49, 0x49, 0x41 },	/* E */
	{ 0x7f, 0x09, 0x09, 0x09, 0x01 },	/* F */
	{ 0x3e, 0x41, 0x49, 0x49, 0x3a },	/* G */
	{ 0x7f, 0x08, 0x08, 0x08, 0x7f },	/* H */
	{ 0x41, 0x41, 0x7f, 0x41, 0x41 },	/* I */
	{ 0x20, 0x40, 0x40, 0x41, 0x3f },	/* J */
	{ 0x7f, 0x08, 0x14, 0x22, 0x41 },	/* K */
	{ 0x7f, 0x40, 0x40, 0x40, 0x40 },	/* L */
	{ 0x7f, 0x02, 0x0c, 0x02, 0x7f },	/* M */
	{ 0x7f, 0x02, 0x04, 0x08, 0x7f },	/* N */
	{ 0x3e, 0x41, 0x41, 0x41, 0x3e },	/* O */
	{ 0x7f, 0x09, 0x09, 0x09, 0x06 },	/* P */
	{ 0x3e, 0x41, 0x51, 0x21, 0x5e },	/* Q */
	{ 0x7f, 0x09, 0x19, 0x29, 0x46 },	/* R */
	{ 0x46, 0x49, 0x49, 0x49, 0x31 },	/* S */
	{ 0x01, 0x01, 0x7f, 0x01, 0x01 },	/* T */
	{ 0x3f, 0x40, 0x40, 0x40, 0x3f },	/* U */
	{ 0x1f, 0x20, 0x40, 0x20, 0x1f },	/* V */
	{ 0x7f, 0x20, 0x18, 0x20, 0x7f },	/* W */
	{ 0x63, 0x14, 0x08, 0x14, 0x63 },	/* X */
	{ 0x03, 0x04, 0x78, 0x04, 0x03 },	/* Y */
	{ 0x61, 0x51, 0x49, 0x45, 0x43 },	/* Z */
	{ 0x08, 0x1c, 0x7f, 0x1c, 0x08 },	/* ^ */
	{ 0x10, 0x0c, 0x0f, 0x0c, 0x10 },	/* v */
	{ 0x08, 0x14, 0x22, 0x41, 0x00 },	/* < */
	{ 0x00, 0x41, 0x22, 0x14, 0x08 },	/* > */
	{ 0x00, 0x00, 0x00, 0x00, 0x00 },	/* the space */
};

static int glyph_index(char character)
{
	if (character >= 'A' && character <= 'Z')
		return character - 'A';
	switch (character)
	{
	case '^': return 26;
	case 'v': return 27;
	case '<': return 28;
	case '>': return 29;
	case ' ': return 30;
	default: return -1;
	}
}

/* ---------- the layouts */

/* what a control does with the finger on it */
enum touch_kind
{
	_touch_kind_button,	/* a button of the pad, while the finger is on it */
	_touch_kind_toggle,	/* a button that stays on until it is pressed again */
	_touch_kind_back	/* the menus' and the keyboard's way back */
};

/* which edge of the picture a control is placed from: the buttons' side, or
the top corner of it (input.touch_left puts them on the left) */
enum touch_side
{
	_touch_side_pad,
	_touch_side_corner
};

/* a control of a layout, in heights of the picture from the edge named: the
same shape on every display, and fitted to the width where the picture is
too narrow for it */
struct touch_entry
{
	enum touch_kind kind;
	const char *label;
	float edge, bottom;
	float radius, hit_radius;
	short analog;		/* bAnalogButtons, or TOUCH_NO_ANALOG */
	unsigned short mask;	/* wButtons, or 0 */
	enum touch_side side;
};

#define TOUCH_NO_ANALOG ((short)-1)

/* the game: the Xbox controller's buttons, on one side, and the whole of the
other half for the stick. The labels are the buttons' names, as on the
controller, and short, to fit a small button. The places are the ones the
mobile shooters use: fire under the thumb, everything else on the arc around
it, and nothing in the way of the view. */
static const struct touch_entry touch_game_entries[] =
{
	{ _touch_kind_button, "FIRE", 0.325f, 0.250f, 0.105f, 0.125f, XINPUT_GAMEPAD_RIGHT_TRIGGER, 0, _touch_side_pad },
	{ _touch_kind_toggle, "ZOOM", 0.173f, 0.500f, 0.062f, 0.080f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_RIGHT_THUMB, _touch_side_pad },
	{ _touch_kind_button, "LITE", 0.607f, 0.480f, 0.054f, 0.072f, XINPUT_GAMEPAD_WHITE, 0, _touch_side_pad },
	{ _touch_kind_button, "A", 0.173f, 0.350f, 0.058f, 0.075f, XINPUT_GAMEPAD_A, 0, _touch_side_pad },
	{ _touch_kind_button, "X", 0.498f, 0.180f, 0.056f, 0.075f, XINPUT_GAMEPAD_X, 0, _touch_side_pad },
	{ _touch_kind_button, "B", 0.607f, 0.320f, 0.056f, 0.072f, XINPUT_GAMEPAD_B, 0, _touch_side_pad },
	{ _touch_kind_button, "DUCK", 0.347f, 0.080f, 0.058f, 0.075f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_LEFT_THUMB, _touch_side_pad },
	{ _touch_kind_button, "Y", 0.325f, 0.650f, 0.054f, 0.072f, XINPUT_GAMEPAD_Y, 0, _touch_side_pad },
	{ _touch_kind_button, "MENU", 0.075f, 0.075f, 0.042f, 0.070f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_START, _touch_side_corner },
};

/* the game's on-screen keyboard: what a controller's buttons do to it */
static const struct touch_entry touch_keyboard_entries[] =
{
	{ _touch_kind_button, "OK", 0.185f, 0.270f, 0.105f, 0.125f, XINPUT_GAMEPAD_A, 0, _touch_side_pad },
	{ _touch_kind_back, "BACK", 0.415f, 0.240f, 0.080f, 0.095f, XINPUT_GAMEPAD_B, 0, _touch_side_pad },
	{ _touch_kind_button, "^", 0.660f, 0.485f, 0.070f, 0.098f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_DPAD_UP, _touch_side_pad },
	{ _touch_kind_button, "v", 0.660f, 0.275f, 0.070f, 0.098f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_DPAD_DOWN, _touch_side_pad },
	{ _touch_kind_button, "<", 0.765f, 0.380f, 0.070f, 0.098f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_DPAD_LEFT, _touch_side_pad },
	{ _touch_kind_button, ">", 0.555f, 0.380f, 0.070f, 0.098f, TOUCH_NO_ANALOG, XINPUT_GAMEPAD_DPAD_RIGHT, _touch_side_pad },
};

/* a menu: the pointer picks what is tapped, and a button for the way back */
static const struct touch_entry touch_menu_entries[] =
{
	{ _touch_kind_back, "BACK", 0.085f, 0.085f, 0.062f, 0.090f, XINPUT_GAMEPAD_B, 0, _touch_side_corner },
};

/* a control once it is placed in the picture */
struct touch_control
{
	enum touch_kind kind;
	const char *label;
	float x, y;		/* the picture's pixels, y down from its top */
	float radius, hit_radius;
	short analog;
	unsigned short mask;
};

/* what a finger holds when it holds no control */
#define TOUCH_CONTROL_NONE (-1)
#define TOUCH_CONTROL_LOOK (-2)	/* the view follows the finger */
#define TOUCH_CONTROL_STICK (-3)	/* the stick, wherever the finger went down */
#define TOUCH_CONTROL_POINT (-4)	/* a menu's pointer */
#define TOUCH_CONTROL_FROZEN (-5)	/* committed to something under the old
									   layout: inert until the finger lifts */

/* how far the floating stick's knob may go from its base, in heights of the
picture */
#define TOUCH_STICK_RADIUS 0.090f

#define TOUCH_FINGER_COUNT 10
#define TOUCH_MAXIMUM_CONTROLS 16

/* how far a finger may travel and still count as a tap, in heights of the
picture, and how long it may be held to be a long press instead */
#define TOUCH_TAP_SLOP 0.030f
#define TOUCH_LONG_PRESS_MS 550

/* how many polls the view may turn for without the game taking the motion
before it is dropped, as the mouse's is (xinput_sdl.c) */
#define TOUCH_LOOK_POLLS 4

/* how long the grenade button is held to change the grenade, which has no
button of its own (the Xbox's black button) */
#define TOUCH_GRENADE_HOLD_MS 400

/* a signal from the game that stops arriving (a loading screen, a film, the
time before the player is in the level) takes the controls away */
#define TOUCH_SIGNAL_TIMEOUT_MS 700

enum touch_layout
{
	_touch_layout_none,
	_touch_layout_game,
	_touch_layout_keyboard,
	_touch_layout_menu
};

struct touch_finger
{
	BOOL down;		/* a finger of ours is on the screen */
	int control;		/* what it holds: a control, or one of the above */
	float x, y;		/* the picture's pixels, y down */
	float start_x, start_y;	/* where it went down */
	float normalized_x, normalized_y;	/* the window's, as SDL reports them */
	float start_normalized_x, start_normalized_y;
	Uint64 start_ms;
	BOOL moved;
};

/* the finger each slot is following, 0 for a slot with none (SDL's own
finger numbers, which are not ours to assume) */
static Uint64 touch_finger_ids[TOUCH_FINGER_COUNT];
static const struct touch_entry *touch_entries;
static int touch_entry_count;
static struct touch_control touch_controls[TOUCH_MAXIMUM_CONTROLS];
static int touch_control_count;
static struct touch_finger touch_fingers[TOUCH_FINGER_COUNT];
static enum touch_layout touch_layout = _touch_layout_none;
static struct platform_touch_rect touch_picture_rect;
static BOOL touch_picture_known = FALSE;
static int touch_picture_width, touch_picture_height;
static int touch_unit;		/* the layout's unit: a height of the picture */
static float touch_stick_x, touch_stick_y;	/* the floating stick's base */
static BOOL touch_mirrored = FALSE;
/* the buttons that stay on until they are pressed again, one bit a control */
static unsigned int touch_latched;
/* the buttons pressed since the last poll, so a tap between two polls is not
lost (as a key press is not, sdl_platform.c's keys_pressed) */
static unsigned int touch_pressed;
/* what the fingers did to a menu's pointer, for the event pump to take
(platform_touch_pointer): the pointer is the platform layer's, guarded by its
lock, so this only notes it down */
static struct platform_ui_pointer touch_pointer;
static BOOL touch_pointer_moved;
static int touch_pointer_clicks, touch_pointer_right_clicks;
/* whether the finger on the grenade button has already changed the grenade */
static BOOL touch_grenade_switched;
/* the view's turn since the game last read it, in heights of the picture */
static float touch_look_x, touch_look_y;
static int touch_look_polls;
/* what the game said, and when (a signal that stops is no signal): a menu (or
the on-screen keyboard) is up, and a game is in progress */
static BOOL touch_menus = FALSE;
static BOOL touch_keyboard = FALSE;
static Uint64 touch_menus_ms = 0;
static BOOL touch_player = FALSE;	/* a game is in progress */
static Uint64 touch_player_ms = 0;

static const char *touch_layout_name(enum touch_layout layout)
{
	switch (layout)
	{
	case _touch_layout_game: return "game";
	case _touch_layout_menu: return "menu";
	case _touch_layout_keyboard: return "keyboard";
	default: return "none";
	}
}

/* ---------- the settings */

static BOOL touch_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = config_boolean("input.touch");
	return enabled != 0;
}

static BOOL touch_left_handed(void)
{
	static int left = -1;

	if (left < 0)
		left = config_boolean("input.touch_left");
	return left != 0;
}

static float touch_size(void)
{
	static float size = 0.0f;

	if (size == 0.0f)
	{
		size = (float)config_real("input.touch_size");
		if (size < 0.6f)
			size = 0.6f;
		if (size > 1.5f)
			size = 1.5f;
	}
	return size;
}

/* ---------- what the game says */

void platform_touch_menus(BOOL menus, BOOL keyboard)
{
	touch_menus = menus;
	touch_keyboard = keyboard;
	touch_menus_ms = SDL_GetTicks();
}

void platform_touch_player(BOOL player)
{
	touch_player = player;
	touch_player_ms = SDL_GetTicks();
}

/* the layout the game has asked for */
static enum touch_layout touch_wanted(void)
{
	Uint64 now;

	if (!touch_enabled())
		return _touch_layout_none;
	now = SDL_GetTicks();
	if (!touch_menus_ms || now - touch_menus_ms > TOUCH_SIGNAL_TIMEOUT_MS)
		return _touch_layout_none;
	if (touch_keyboard)
		return _touch_layout_keyboard;
	if (touch_menus)
		return _touch_layout_menu;
	if (!touch_player || now - touch_player_ms > TOUCH_SIGNAL_TIMEOUT_MS)
		return _touch_layout_none;
	return _touch_layout_game;
}

/* ---------- the picture the controls are placed in */

/* where the game last drew, and before the first frame the whole window */
static void touch_picture(void)
{
	int width = 0, height = 0;

	if (touch_picture_known)
		return;
	platform_video_drawable_size(&width, &height);
	if (width <= 0 || height <= 0)
		return; /* no window yet: platform_touch_draw brings the first one */
	touch_picture_rect.left = 0;
	touch_picture_rect.top = 0;
	touch_picture_rect.width = width;
	touch_picture_rect.height = height;
	touch_picture_known = TRUE;
}

static void touch_measure(void)
{
	touch_picture();
	touch_picture_width = touch_picture_rect.width;
	touch_picture_height = touch_picture_rect.height;
	/* a height of the picture, unless the picture is too narrow for the
	layout, which is then fitted to the width instead */
	touch_unit = (int)((float)touch_picture_height * touch_size());
	if (touch_unit > touch_picture_width * 576 / 1000)
		touch_unit = touch_picture_width * 576 / 1000;
	touch_mirrored = touch_left_handed();
}

/* places the current layout's entries in the picture's pixels */
static void touch_place(void)
{
	int index;

	switch (touch_layout)
	{
	case _touch_layout_game:
		touch_entries = touch_game_entries;
		touch_entry_count = (int)(sizeof(touch_game_entries) / sizeof(touch_game_entries[0]));
		break;
	case _touch_layout_keyboard:
		touch_entries = touch_keyboard_entries;
		touch_entry_count = (int)(sizeof(touch_keyboard_entries) / sizeof(touch_keyboard_entries[0]));
		break;
	case _touch_layout_menu:
		touch_entries = touch_menu_entries;
		touch_entry_count = (int)(sizeof(touch_menu_entries) / sizeof(touch_menu_entries[0]));
		break;
	default:
		touch_entries = NULL;
		touch_entry_count = 0;
		break;
	}
	touch_control_count = touch_entry_count;
	if (touch_control_count > TOUCH_MAXIMUM_CONTROLS)
		touch_control_count = TOUCH_MAXIMUM_CONTROLS;
	for (index = 0; index < touch_control_count; index++)
	{
		const struct touch_entry *entry = &touch_entries[index];
		struct touch_control *control = &touch_controls[index];
		int edge = (int)(entry->edge * touch_unit);
		int bottom = (int)(entry->bottom * touch_unit);

		control->kind = entry->kind;
		control->label = entry->label;
		control->radius = entry->radius * touch_unit;
		control->hit_radius = entry->hit_radius * touch_unit;
		control->analog = entry->analog;
		control->mask = entry->mask;
		/* the xdk names the face buttons by their place in bAnalogButtons and
		the rest by their bit in wButtons, so a mask put in one column lands
		outright outside the other's array: say so rather than press nothing */
		if (entry->analog != TOUCH_NO_ANALOG && (entry->analog < 0 || entry->analog >= 14))
		{
			platform_log("touch: %s: %.0x is a wButtons mask, not a bAnalogButtons index",
				entry->label, (float)entry->analog);
			control->analog = TOUCH_NO_ANALOG;
		}
		if (entry->analog == TOUCH_NO_ANALOG && !entry->mask)
			platform_log("touch: %s: no button", entry->label);
		if (entry->side == _touch_side_corner)
		{
			control->x = touch_mirrored ? (float)edge : (float)(touch_picture_width - edge);
			control->y = (float)edge;
		}
		else
		{
			control->x = touch_mirrored ? (float)edge : (float)(touch_picture_width - edge);
			control->y = (float)(touch_picture_height - bottom);
		}
	}
	if (touch_layout != _touch_layout_game)
		touch_latched = 0;
}

/* the buttons' half of the picture, where the view turns */
static BOOL touch_on_pad_side(float x)
{
	return touch_mirrored ? x < touch_picture_width * 0.5f :
		x > touch_picture_width * 0.5f;
}

/* the control under a point, or what else a touch there does */
static int touch_control_at(float x, float y)
{
	const struct touch_control *nearest = NULL;
	float best = 0.0f;
	int index;

	for (index = 0; index < touch_control_count; index++)
	{
		const struct touch_control *control = &touch_controls[index];
		float dx = x - control->x, dy = y - control->y;
		float distance = sqrtf(dx * dx + dy * dy);

		if (distance <= control->hit_radius && (nearest == NULL || distance < best))
		{
			nearest = control;
			best = distance;
		}
	}
	if (nearest)
		return (int)(nearest - touch_controls);
	if (touch_layout == _touch_layout_game)
	{
		/* the buttons' half turns the view, the other half walks: the stick
		floats, so a thumb lands where it is comfortable rather than on a
		fixed base */
		if (touch_on_pad_side(x))
			return TOUCH_CONTROL_LOOK;
		return TOUCH_CONTROL_STICK;
	}
	if (touch_layout == _touch_layout_menu)
		return TOUCH_CONTROL_POINT;
	return TOUCH_CONTROL_NONE;
}

/* ---------- the fingers */

/* a finger takes what is under it, and presses it */
static void touch_take(struct touch_finger *finger, BOOL press)
{
	int control = touch_control_at(finger->x, finger->y);

	finger->control = control;
	if (!press)
		return;
	if (control == TOUCH_CONTROL_STICK)
	{
		/* the stick's base is where the finger went down */
		touch_stick_x = finger->x;
		touch_stick_y = finger->y;
		return;
	}
	if (control >= 0)
	{
		const struct touch_control *taken = &touch_controls[control];

		if (taken->kind == _touch_kind_toggle)
			touch_latched ^= 1u << control;
		touch_pressed |= 1u << control;
		platform_log("touch: %s at %.0f,%.0f", taken->label, finger->x, finger->y);
		platform_haptic(PLATFORM_HAPTIC_PRESS, 12);
	}
}

/* every finger's control again: the layout changed under them, and what a
finger is on now is not what it was pressed for (the finger that opened the
menu is still down, and must not press the button under it) */
static void touch_reassign(void)
{
	int index;

	for (index = 0; index < TOUCH_FINGER_COUNT; index++)
	{
		struct touch_finger *finger = &touch_fingers[index];

		if (!finger->down)
			continue;
		if (finger->control != TOUCH_CONTROL_NONE)
		{
			/* it already has something (a button, the stick, the view): hold
			it to that until the finger lifts, rather than taking whatever the
			new layout has here - the thumb that pressed MENU is still down
			when the pause menu opens, and BACK sits under it */
			finger->control = TOUCH_CONTROL_FROZEN;
			continue;
		}
		touch_take(finger, FALSE);
	}
}

/* the layout for the game as it is now; called wherever the touch controls
are used, so what is drawn, hit and pressed always agree */
static void touch_update(void)
{
	enum touch_layout wanted = touch_wanted();
	BOOL changed = wanted != touch_layout;

	touch_measure();
	touch_layout = wanted;
	touch_place();
	if (changed)
	{
		int index;

		platform_log("touch: the controls are %s, picture %d,%d %dx%d, unit %d",
			touch_layout_name(wanted), touch_picture_rect.left, touch_picture_rect.top,
			touch_picture_width, touch_picture_height, touch_unit);
		for (index = 0; index < touch_control_count; index++)
		{
			platform_log("touch:   %s at %.0f,%.0f r %.0f", touch_controls[index].label,
				touch_controls[index].x, touch_controls[index].y, touch_controls[index].radius);
		}
		touch_reassign();
	}
}

void platform_touch_release_all(void)
{
	int index;

	for (index = 0; index < TOUCH_FINGER_COUNT; index++)
	{
		struct touch_finger *finger = &touch_fingers[index];

		finger->down = FALSE;
		finger->control = TOUCH_CONTROL_NONE;
		touch_finger_ids[index] = 0;
	}
	touch_look_x = 0.0f;
	touch_look_y = 0.0f;
	touch_look_polls = 0;
	touch_pointer_moved = FALSE;
	touch_pointer_clicks = 0;
	touch_pointer_right_clicks = 0;
}

/* a finger dragged: the view turns, or a menu's pointer moves */
static void touch_drag(struct touch_finger *finger, float x, float y)
{
	float unit = touch_unit > 0 ? (float)touch_unit : 1.0f;
	float dx = x - finger->x;
	float dy = y - finger->y;

	finger->x = x;
	finger->y = y;
	if (fabsf(x - finger->start_x) + fabsf(y - finger->start_y) > TOUCH_TAP_SLOP * unit)
		finger->moved = TRUE;
	switch (finger->control)
	{
	case TOUCH_CONTROL_LOOK:
		touch_look_x += dx / unit;
		touch_look_y += dy / unit;
		break;
	case TOUCH_CONTROL_POINT:
		/* the pointer hovers where the finger is, as the mouse's does */
		touch_pointer.nx = finger->normalized_x;
		touch_pointer.ny = finger->normalized_y;
		touch_pointer_moved = TRUE;
		break;
	default:
		/* the stick is the one thing that must not turn the view, but a
		button the thumb is holding may: the same thumb holds fire and aims,
		the way an index finger holds the trigger while the thumb steers */
		if (finger->control >= 0 && touch_layout == _touch_layout_game)
		{
			touch_look_x += dx / unit;
			touch_look_y += dy / unit;
		}
		break;
	}
}

/* a finger lifted: a tap, a long press (the mouse's right button, which the
menus take as B), or nothing at all */
static void touch_lift(struct touch_finger *finger)
{
	if (finger->control == TOUCH_CONTROL_POINT && !finger->moved)
	{
		BOOL held = SDL_GetTicks() - finger->start_ms >= TOUCH_LONG_PRESS_MS;

		touch_pointer.click_nx = finger->start_normalized_x;
		touch_pointer.click_ny = finger->start_normalized_y;
		if (held)
			touch_pointer_right_clicks++;
		else
			touch_pointer_clicks++;
	}
	finger->control = TOUCH_CONTROL_NONE;
}

/* the pointer, as the fingers left it, for the event pump (which holds the
lock that guards the platform layer's own) to take */
void platform_touch_pointer(struct platform_ui_pointer *pointer)
{
	if (touch_pointer_moved)
	{
		pointer->nx = touch_pointer.nx;
		pointer->ny = touch_pointer.ny;
		pointer->moved = TRUE;
		touch_pointer_moved = FALSE;
	}
	if (touch_pointer_clicks || touch_pointer_right_clicks)
	{
		pointer->click_nx = touch_pointer.click_nx;
		pointer->click_ny = touch_pointer.click_ny;
		pointer->left_clicks += touch_pointer_clicks;
		pointer->right_clicks += touch_pointer_right_clicks;
		touch_pointer_clicks = 0;
		touch_pointer_right_clicks = 0;
	}
}

void platform_touch_finger(Uint64 finger, BOOL down, float normalized_x, float normalized_y)
{
	struct touch_finger *state;
	int slot = -1;
	float x, y;
	int index;

	touch_update();
	if (touch_layout == _touch_layout_none)
	{
		platform_touch_release_all();
		return;
	}
	for (index = 0; index < TOUCH_FINGER_COUNT; index++)
	{
		if (touch_finger_ids[index] == finger)
			slot = index;
		else if (slot < 0 && down && !touch_finger_ids[index] && !touch_fingers[index].down)
			slot = index;
	}
	if (slot < 0)
		return; /* more fingers than the controls can use */
	x = (float)touch_picture_rect.left + normalized_x * touch_picture_rect.width;
	y = (float)touch_picture_rect.top + normalized_y * touch_picture_rect.height;
	state = &touch_fingers[slot];
	if (!down)
	{
		if (state->down)
			touch_lift(state);
		state->down = FALSE;
		state->control = TOUCH_CONTROL_NONE;
		touch_finger_ids[slot] = 0;
		return;
	}
	touch_finger_ids[slot] = finger;
	if (!state->down)
	{
		state->down = TRUE;
		state->x = state->start_x = x;
		state->y = state->start_y = y;
		state->normalized_x = state->start_normalized_x = normalized_x;
		state->normalized_y = state->start_normalized_y = normalized_y;
		state->start_ms = SDL_GetTicks();
		state->moved = FALSE;
		touch_take(state, TRUE);
	}
	else
	{
		state->normalized_x = normalized_x;
		state->normalized_y = normalized_y;
		touch_drag(state, x, y);
	}
}

/* ---------- the controller (xinput_sdl.c) */

/* the finger that holds the floating stick, or -1 */
static int touch_stick_finger(void)
{
	int index;

	for (index = 0; index < TOUCH_FINGER_COUNT; index++)
	{
		if (touch_fingers[index].down && touch_fingers[index].control == TOUCH_CONTROL_STICK)
			return index;
	}
	return -1;
}

/* the control a finger is holding, and whether it is down (a toggle stays
down between presses) */
static BOOL touch_control_held(int control, int *finger_index)
{
	int index;

	for (index = 0; index < TOUCH_FINGER_COUNT; index++)
	{
		const struct touch_finger *finger = &touch_fingers[index];

		if (finger->down && finger->control == control)
		{
			*finger_index = index;
			return touch_controls[control].kind != _touch_kind_toggle ||
				(touch_latched & (1u << control)) != 0;
		}
	}
	return FALSE;
}

void platform_touch_gamepad(XINPUT_GAMEPAD *pad)
{
	int index;

	/* motion nobody takes (a cutscene, the console) is dropped, as the
	mouse's is */
	if (++touch_look_polls > TOUCH_LOOK_POLLS)
		touch_look_x = touch_look_y = 0.0f;
	touch_update();
	if (touch_layout == _touch_layout_none)
	{
		touch_pressed = 0;
		return;
	}
	for (index = 0; index < touch_control_count; index++)
	{
		const struct touch_control *control = &touch_controls[index];
		int finger = -1;

		if (!touch_control_held(index, &finger) && !(touch_pressed & (1u << index)))
			continue;
		if (control->analog != TOUCH_NO_ANALOG)
			pad->bAnalogButtons[control->analog] = 0xff;
		if (control->mask)
			pad->wButtons |= control->mask;
	}
	touch_pressed = 0;
	/* the floating stick: the offset of the finger that took it from where it
	went down, at full deflection at TOUCH_STICK_RADIUS (the game has its own
	dead zone and curve) */
	{
		int finger = touch_stick_finger();

		if (finger >= 0)
		{
			float radius = TOUCH_STICK_RADIUS * touch_unit;
			float x = (touch_fingers[finger].x - touch_stick_x) / radius;
			float y = (touch_fingers[finger].y - touch_stick_y) / radius;
			float length = sqrtf(x * x + y * y);

			if (length > 1.0f)
			{
				x /= length;
				y /= length;
			}
			pad->sThumbLX = (SHORT)(x * 32767.0f);
			pad->sThumbLY = (SHORT)(-y * 32767.0f);
		}
	}
	/* holding the grenade button changes the grenade, which the Xbox
	controller's black button does and which has no button of its own here */
	for (index = 0; index < touch_control_count; index++)
	{
		int finger = -1;

		if (touch_controls[index].analog != XINPUT_GAMEPAD_LEFT_TRIGGER ||
			touch_layout != _touch_layout_game)
			continue;
		if (!touch_control_held(index, &finger))
		{
			touch_grenade_switched = FALSE;
		}
		else if (!touch_grenade_switched &&
			SDL_GetTicks() - touch_fingers[finger].start_ms >= TOUCH_GRENADE_HOLD_MS)
		{
			touch_grenade_switched = TRUE;
			pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = 0xff;
		}
	}
}

/* the view's turn since the last call, in heights of the picture (y down) */
void platform_touch_look(float *x, float *y)
{
	*x = touch_look_x;
	*y = touch_look_y;
	touch_look_x = 0.0f;
	touch_look_y = 0.0f;
	touch_look_polls = 0;
	touch_pointer_moved = FALSE;
	touch_pointer_clicks = 0;
	touch_pointer_right_clicks = 0;
}

/* ---------- drawing the controls (d3d8_gl.c) */

static const char *touch_vertex_source =
	"#version 300 es\n"
	"uniform vec2 u_resolution;\n"	/* the window's size in pixels */
	"uniform vec4 u_rect;\n"		/* the centre in window pixels, y up, and half the size */
	"uniform vec3 u_shape;\n"		/* the corner's radius, the hole's radius, a glyph's cell */
	"uniform vec4 u_color;\n"
	"out vec2 v_offset;\n"
	"out vec2 v_half;\n"
	"out vec3 v_shape;\n"
	"out vec4 v_color;\n"
	"void main()\n"
	"{\n"
	"	int index = gl_VertexID;\n"
	"	float x = (index == 0 || index == 2) ? -1.0 : 1.0;\n"
	"	float y = (index < 2) ? -1.0 : 1.0;\n"
	"	vec2 corner = vec2(x, y) * u_rect.zw;\n"
	"	gl_Position = vec4((u_rect.xy + corner) / u_resolution * 2.0 - 1.0, 0.0, 1.0);\n"
	"	v_offset = corner;\n"
	"	v_half = u_rect.zw;\n"
	"	v_shape = u_shape;\n"
	"	v_color = u_color;\n"
	"}\n";

static const char *touch_fragment_source =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform float u_glyph[5];\n"	/* a glyph's five columns, seven rows of bits each */
	"in vec2 v_offset;\n"
	"in vec2 v_half;\n"
	"in vec3 v_shape;\n"
	"in vec4 v_color;\n"
	"out vec4 color;\n"
	"void main()\n"
	"{\n"
	"	vec2 p = abs(v_offset);\n"
	"	vec2 q = p - (v_half - v_shape.x);\n"
	"	float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_shape.x;\n"
	"	float alpha = clamp(0.5 - distance, 0.0, 1.0);\n"
	"	if (v_shape.y > 0.0)\n"		/* a hole in the middle */
	"		alpha *= clamp(length(p) - v_shape.y + 0.5, 0.0, 1.0);\n"
	"	if (v_shape.z > 0.0)\n"		/* a glyph of the labels' font */
	"	{\n"
	"		vec2 cell = vec2(v_offset.x + v_half.x, v_half.y - v_offset.y) / v_shape.z;\n"
	"		int column = int(floor(cell.x));\n"
	"		int row = int(floor(cell.y));\n"
	"		float bits = u_glyph[0];\n"
	"		float on = 0.0;\n"
	"		if (column == 1)\n"
	"			bits = u_glyph[1];\n"
	"		else if (column == 2)\n"
	"			bits = u_glyph[2];\n"
	"		else if (column == 3)\n"
	"			bits = u_glyph[3];\n"
	"		else if (column == 4)\n"
	"			bits = u_glyph[4];\n"
	"		if (column >= 0 && column < 5 && row >= 0 && row < 7)\n"
	"			on = mod(floor(bits / exp2(float(row))), 2.0);\n"
	"		alpha *= on;\n"
	"	}\n"
	"	color = vec4(v_color.rgb, v_color.a * alpha);\n"
	"}\n";

static struct
{
	GLuint program;
	GLint resolution;
	GLint rect;
	GLint shape;
	GLint color;
	GLint glyph;
} touch_shader;

static BOOL touch_compile(GLenum type, const char *source, GLuint *shader)
{
	GLint status = 0;

	*shader = glCreateShader(type);
	glShaderSource(*shader, 1, &source, NULL);
	glCompileShader(*shader);
	glGetShaderiv(*shader, GL_COMPILE_STATUS, &status);
	if (status)
		return TRUE;
	{
		char log[512];

		glGetShaderInfoLog(*shader, sizeof(log), NULL, log);
		platform_log("the touch controls' shader does not compile: %s", log);
	}
	glDeleteShader(*shader);
	*shader = 0;
	return FALSE;
}

/* the program the controls are drawn with, made the first time they are */
static BOOL touch_make_program(void)
{
	GLuint vertex, fragment;
	GLint status = 0;

	if (touch_shader.program)
		return TRUE;
	if (!touch_compile(GL_VERTEX_SHADER, touch_vertex_source, &vertex))
		return FALSE;
	if (!touch_compile(GL_FRAGMENT_SHADER, touch_fragment_source, &fragment))
	{
		glDeleteShader(vertex);
		return FALSE;
	}
	touch_shader.program = glCreateProgram();
	glAttachShader(touch_shader.program, vertex);
	glAttachShader(touch_shader.program, fragment);
	glLinkProgram(touch_shader.program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(touch_shader.program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[512];

		glGetProgramInfoLog(touch_shader.program, sizeof(log), NULL, log);
		platform_log("the touch controls' program does not link: %s", log);
		glDeleteProgram(touch_shader.program);
		touch_shader.program = 0;
		return FALSE;
	}
	touch_shader.resolution = glGetUniformLocation(touch_shader.program, "u_resolution");
	touch_shader.rect = glGetUniformLocation(touch_shader.program, "u_rect");
	touch_shader.shape = glGetUniformLocation(touch_shader.program, "u_shape");
	touch_shader.color = glGetUniformLocation(touch_shader.program, "u_color");
	touch_shader.glyph = glGetUniformLocation(touch_shader.program, "u_glyph[0]");
	if (touch_shader.resolution < 0 || touch_shader.rect < 0 || touch_shader.shape < 0 ||
		touch_shader.color < 0 || touch_shader.glyph < 0)
	{
		platform_log("the touch controls' program has no uniforms");
		glDeleteProgram(touch_shader.program);
		touch_shader.program = 0;
		return FALSE;
	}
	return TRUE;
}

/* the colours the controls are drawn in */
static const float touch_control_color[4] = { 0.90f, 0.92f, 0.95f, 0.16f };
static const float touch_control_held_color[4] = { 1.0f, 1.0f, 1.0f, 0.45f };
static const float touch_label_color[4] = { 1.0f, 1.0f, 1.0f, 0.62f };
static const float touch_label_held_color[4] = { 1.0f, 1.0f, 1.0f, 0.95f };

/* one rounded box (a circle when the corner's radius is half the size), with
a hole in the middle, and (cell > 0) a glyph of the labels' font. The centre
is in the window's pixels with y up. */
static void touch_draw(float x, float y, float half_x, float half_y, float radius,
	float hole, float cell, const float *color)
{
	glUniform4f(touch_shader.rect, x, y, half_x, half_y);
	glUniform3f(touch_shader.shape, radius, hole, cell);
	glUniform4fv(touch_shader.color, 1, color);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void touch_draw_label(const char *label, float x, float y, float radius, const float *color)
{
	int characters = (int)strlen(label);
	int columns = characters * (GLYPH_COLUMNS + 1) - 1;
	float cell = radius * 1.8f / (float)columns;
	float left;
	int index;

	/* the label stays inside the button */
	if (cell * GLYPH_ROWS > radius)
		cell = radius / GLYPH_ROWS;
	left = x - cell * (float)columns * 0.5f;
	for (index = 0; index < characters; index++)
	{
		int glyph = glyph_index(label[index]);

		if (glyph >= 0)
		{
			float columns_of_glyph[GLYPH_COLUMNS];
			int column;

			for (column = 0; column < GLYPH_COLUMNS; column++)
				columns_of_glyph[column] = touch_font[glyph][column];
			glUniform1fv(touch_shader.glyph, GLYPH_COLUMNS, columns_of_glyph);
			/* the glyph's five columns, in the cell it starts at */
			touch_draw(left + cell * GLYPH_COLUMNS * 0.5f, y,
				cell * GLYPH_COLUMNS * 0.5f, cell * GLYPH_ROWS * 0.5f, 0.0f, 0.0f, cell, color);
		}
		left += cell * (GLYPH_COLUMNS + 1);
	}
}

void platform_touch_draw(const struct platform_touch_rect *rect)
{
	float resolution[2];
	int window_width = 0, window_height = 0;
	int stick_finger;
	int index;

	if (!rect || rect->width <= 0 || rect->height <= 0)
		return;
	/* where the game drew, for the touches and for the next frame */
	touch_picture_rect = *rect;
	touch_picture_known = TRUE;
	touch_update();
	if (touch_layout == _touch_layout_none || !touch_make_program())
		return;
	platform_video_drawable_size(&window_width, &window_height);
	if (window_width <= 0 || window_height <= 0)
	{
		window_width = rect->left + rect->width;
		window_height = rect->top + rect->height;
	}
	resolution[0] = (float)window_width;
	resolution[1] = (float)window_height;
	glUseProgram(touch_shader.program);
	/* the whole window: the presentation blit leaves the renderer's own
	viewport (the back buffer's, which is not the window's) in place, and the
	quads below are placed in the window's pixels */
	glViewport(0, 0, window_width, window_height);
	glUniform2fv(touch_shader.resolution, 1, resolution);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	/* the window's GL context has no depth buffer (SDL_GL_DEPTH_SIZE is 0),
	but the renderer may have left its own state on: it culls with the front
	face the other way round, and the game may have turned depth testing on
	for its own targets */
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	for (index = 0; index < touch_control_count; index++)
	{
		const struct touch_control *control = &touch_controls[index];
		int finger = -1;
		BOOL held;
		/* the picture's y is down and OpenGL's is up */
		float x = (float)rect->left + control->x;
		float y = (float)(rect->top + rect->height) - control->y;

		held = touch_control_held(index, &finger);
		touch_draw(x, y, control->radius, control->radius, control->radius, 0.0f, 0.0f,
			held ? touch_control_held_color : touch_control_color);
		if (control->label && control->label[0])
		{
			touch_draw_label(control->label, x, y, control->radius,
				held ? touch_label_held_color : touch_label_color);
		}
	}
	/* the floating stick, where its finger went down: a ring for the reach
	and the knob where the finger is */
	stick_finger = touch_stick_finger();
	if (stick_finger >= 0)
	{
		float radius = TOUCH_STICK_RADIUS * touch_unit;
		float x = (float)rect->left + touch_stick_x;
		float y = (float)(rect->top + rect->height) - touch_stick_y;
		float knob = radius * 0.38f;
		float dx = touch_fingers[stick_finger].x - touch_stick_x;
		float dy = touch_fingers[stick_finger].y - touch_stick_y;
		float length = sqrtf(dx * dx + dy * dy);
		float ring[4];

		if (length > radius)
		{
			dx *= radius / length;
			dy *= radius / length;
		}
		memcpy(ring, touch_control_color, sizeof(ring));
		ring[3] = 0.20f;
		touch_draw(x, y, radius, radius, radius, radius * 0.55f, 0.0f, ring);
		touch_draw(x + dx, y - dy, knob, knob, knob, 0.0f, 0.0f, touch_control_held_color);
	}
}

#endif /* HALO_ANDROID */
