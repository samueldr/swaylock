#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include "comm.h"
#include "log.h"
#include "loop.h"
#include "pinpad.h"
#include "seat.h"
#include "swaylock.h"
#include "unicode.h"

void clear_buffer(char *buf, size_t size) {
	// Use volatile keyword so so compiler can't optimize this out.
	volatile char *buffer = buf;
	volatile char zero = '\0';
	for (size_t i = 0; i < size; ++i) {
		buffer[i] = zero;
	}
}

void clear_password_buffer(struct swaylock_password *pw) {
	clear_buffer(pw->buffer, pw->buffer_len);
	pw->len = 0;
}

static bool backspace(struct swaylock_password *pw) {
	if (pw->len != 0) {
		pw->len -= utf8_last_size(pw->buffer);
		pw->buffer[pw->len] = 0;
		return true;
	}
	return false;
}

static void append_ch(struct swaylock_password *pw, uint32_t codepoint) {
	size_t utf8_size = utf8_chsize(codepoint);
	if (pw->len + utf8_size + 1 >= pw->buffer_len) {
		// TODO: Display error
		return;
	}
	utf8_encode(&pw->buffer[pw->len], codepoint);
	pw->buffer[pw->len + utf8_size] = 0;
	pw->len += utf8_size;
}

static void set_input_idle(void *data) {
	struct swaylock_state *state = data;
	state->input_idle_timer = NULL;
	state->input_state = INPUT_STATE_IDLE;
	damage_state(state);
}

static void set_auth_idle(void *data) {
	struct swaylock_state *state = data;
	state->auth_idle_timer = NULL;
	state->auth_state = AUTH_STATE_IDLE;
	damage_state(state);
}

static void schedule_input_idle(struct swaylock_state *state) {
	if (state->input_idle_timer) {
		loop_remove_timer(state->eventloop, state->input_idle_timer);
	}
	state->input_idle_timer = loop_add_timer(
		state->eventloop, 1500, set_input_idle, state);
}

static void cancel_input_idle(struct swaylock_state *state) {
	if (state->input_idle_timer) {
		loop_remove_timer(state->eventloop, state->input_idle_timer);
		state->input_idle_timer = NULL;
	}
}

void schedule_auth_idle(struct swaylock_state *state) {
	if (state->auth_idle_timer) {
		loop_remove_timer(state->eventloop, state->auth_idle_timer);
	}
	state->auth_idle_timer = loop_add_timer(
		state->eventloop, 3000, set_auth_idle, state);
}

static void clear_password(void *data) {
	struct swaylock_state *state = data;
	state->clear_password_timer = NULL;
	state->input_state = INPUT_STATE_CLEAR;
	schedule_input_idle(state);
	clear_password_buffer(&state->password);
	damage_state(state);
}

static void schedule_password_clear(struct swaylock_state *state) {
	if (state->clear_password_timer) {
		loop_remove_timer(state->eventloop, state->clear_password_timer);
	}
	state->clear_password_timer = loop_add_timer(
			state->eventloop, 10000, clear_password, state);
}

static void cancel_password_clear(struct swaylock_state *state) {
	if (state->clear_password_timer) {
		loop_remove_timer(state->eventloop, state->clear_password_timer);
		state->clear_password_timer = NULL;
	}
}

static void submit_password(struct swaylock_state *state) {
	if (state->args.ignore_empty && state->password.len == 0) {
		return;
	}
	if (state->auth_state == AUTH_STATE_VALIDATING) {
		return;
	}

	state->input_state = INPUT_STATE_IDLE;
	state->auth_state = AUTH_STATE_VALIDATING;
	cancel_password_clear(state);
	cancel_input_idle(state);

	if (!write_comm_request(&state->password)) {
		state->auth_state = AUTH_STATE_INVALID;
		schedule_auth_idle(state);
	}

	damage_state(state);
}

static void update_highlight(struct swaylock_state *state) {
#ifdef TRUE
	// Advance a single step amount
	state->highlight_start =
		(state->highlight_start + (1 % 1024) + 2048/9) % 2048;
#else
	// Advance a random amount between 1/4 and 3/4 of a full turn
	state->highlight_start =
		(state->highlight_start + (rand() % 1024) + 512) % 2048;
#endif
}

void swaylock_handle_key(struct swaylock_state *state,
		xkb_keysym_t keysym, uint32_t codepoint) {

	switch (keysym) {
	case XKB_KEY_KP_Enter: /* fallthrough */
	case XKB_KEY_Return:
		submit_password(state);
		break;
	case XKB_KEY_Delete:
	case XKB_KEY_BackSpace:
		if (state->xkb.control) {
			clear_password_buffer(&state->password);
			state->input_state = INPUT_STATE_CLEAR;
			cancel_password_clear(state);
		} else {
			if (backspace(&state->password) && state->password.len != 0) {
				state->input_state = INPUT_STATE_BACKSPACE;
				schedule_password_clear(state);
				update_highlight(state);
			} else {
				state->input_state = INPUT_STATE_CLEAR;
				cancel_password_clear(state);
			}
		}
		schedule_input_idle(state);
		damage_state(state);
		break;
	case XKB_KEY_Escape:
		clear_password_buffer(&state->password);
		state->input_state = INPUT_STATE_CLEAR;
		cancel_password_clear(state);
		schedule_input_idle(state);
		damage_state(state);
		break;
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		state->input_state = INPUT_STATE_NEUTRAL;
		schedule_password_clear(state);
		schedule_input_idle(state);
		damage_state(state);
		break;
	case XKB_KEY_m: /* fallthrough */
	case XKB_KEY_d:
	case XKB_KEY_j:
		if (state->xkb.control) {
			submit_password(state);
			break;
		}
		// fallthrough
	case XKB_KEY_c: /* fallthrough */
	case XKB_KEY_u:
		if (state->xkb.control) {
			clear_password_buffer(&state->password);
			state->input_state = INPUT_STATE_CLEAR;
			cancel_password_clear(state);
			schedule_input_idle(state);
			damage_state(state);
			break;
		}
		// fallthrough
	default:
		if (codepoint) {
			append_ch(&state->password, codepoint);
			state->input_state = INPUT_STATE_LETTER;
			schedule_password_clear(state);
			schedule_input_idle(state);
			update_highlight(state);
			damage_state(state);
		}
		break;
	}
}

// NOTE: See `draw_button` and `draw_pinpad`.
static enum pad_button pinpad_button_at(struct swaylock_surface *surface, int x, int y) {
	enum pad_button result = PAD_BUTTON_INVALID;
	int width = surface->child_width;
	int height = surface->child_height;
	int pad_width = width;
	int pad_height = pad_width;
	int button_i, button_j = 0;

	// Assumes the pinpad is in the square area at the bottom.
	// Translates the origin of the touch to the button pad's top left...
	int translated_x = x;
	int translated_y = y - (height - pad_height);

	// Translates the button padd padding.
	translated_x -= WIDGET_PADDING;
	translated_y -= WIDGET_PADDING;
	// And adapts height/width accordingly.
	pad_height -= 2 * WIDGET_PADDING;
	pad_width  -= 2 * WIDGET_PADDING;

	// Get the i,j coordinates of the button.
	button_i = translated_x / (pad_width / 3);
	button_j = translated_y / (pad_height / 4);

	if (translated_x < 0) {
		button_i -= 1;
	}
	if (translated_y < 0) {
		button_j -= 1;
	}

	if (button_i >= 0 && button_i < 3) {
		if (button_j >= 0 && button_j < 4) {
			result = button_j * 3 + button_i + 1;
		}
	}

	return result;
}

/**
 * Receives the zeroeth touch event.
 * No multitouch support at the moment.
 * The x and y values are scaled to the currently touched surface.
 * NOTE: When touch_event is TOUCH_EVENT_UP, x and y will be 0.
 * NOTE: Only TOUCH_EVENT_DOWN includes a valid surface.
 */
void swaylock_update_touch(struct swaylock_state *state, enum touch_event event, struct wl_surface *surface, int x, int y) {
	enum pad_button button = PAD_BUTTON_INVALID;
	struct swaylock_surface *swaylock_surface = NULL;
	struct swaylock_surface *swaylock_surface_candidate = NULL;

	// Use either the attached surface, or the surface we now need to attach...
	if (surface == NULL) {
		surface = state->touched_surface;
	}

	// ... to find the swaylock "surfaces" we are touching.
	wl_list_for_each(swaylock_surface_candidate, &state->surfaces, link) {
		if (surface == swaylock_surface_candidate->child) {
			swaylock_surface = swaylock_surface_candidate;
			break;
		}
	}

	// Not touching a pinpad? Do nothing.
	if (!swaylock_surface) {
		return;
	}

	switch (event) {
		case TOUCH_EVENT_DOWN:
			state->touched_surface = surface;
			break;
		case TOUCH_EVENT_UP:
			state->touched_surface = NULL;
			break;
		case TOUCH_EVENT_MOVE:
			break;
	}

	if (event == TOUCH_EVENT_UP) {
		button = pinpad_button_at(swaylock_surface, state->touch_x, state->touch_y);
		if (button == state->initial_button) {
		}
	}

	// Pre-scale the coordinates
	state->touch_x = x * swaylock_surface->scale;
	state->touch_y = y * swaylock_surface->scale;

	if (event == TOUCH_EVENT_DOWN) {
		button = pinpad_button_at(swaylock_surface, state->touch_x, state->touch_y);
		state->initial_button = button;
	}

	damage_state(state);
}
