#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-client.h>
#include "cairo.h"
#include "background-image.h"
#include "swaylock.h"
#include "log.h"

#define M_PI 3.14159265358979323846
const float TYPE_INDICATOR_RANGE = M_PI / 3.0f;

struct render_context {
	cairo_t *cairo;
	int buffer_diameter;
	int buffer_height;
	int buffer_width;
	int arc_radius;
	int arc_thickness;
	struct swaylock_state *state;
	struct swaylock_surface *surface;
	char *text;
	const char *layout_text;
};

static void set_color_for_state(cairo_t *cairo, struct swaylock_state *state,
		struct swaylock_colorset *colorset) {
	if (state->input_state == INPUT_STATE_CLEAR) {
		cairo_set_source_u32(cairo, colorset->cleared);
	} else if (state->auth_state == AUTH_STATE_VALIDATING) {
		cairo_set_source_u32(cairo, colorset->verifying);
	} else if (state->auth_state == AUTH_STATE_INVALID) {
		cairo_set_source_u32(cairo, colorset->wrong);
	} else {
		if (state->xkb.caps_lock && state->args.show_caps_lock_indicator) {
			cairo_set_source_u32(cairo, colorset->caps_lock);
		} else if (state->xkb.caps_lock && !state->args.show_caps_lock_indicator &&
				state->args.show_caps_lock_text) {
			uint32_t inputtextcolor = state->args.colors.text.input;
			state->args.colors.text.input = state->args.colors.text.caps_lock;
			cairo_set_source_u32(cairo, colorset->input);
			state->args.colors.text.input = inputtextcolor;
		} else {
			cairo_set_source_u32(cairo, colorset->input);
		}
	}
}

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	struct swaylock_surface *surface = data;

	wl_callback_destroy(callback);
	surface->frame = NULL;

	render(surface);
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

static bool render_frame(struct swaylock_surface *surface);

void render(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	int buffer_width = surface->width * surface->scale;
	int buffer_height = surface->height * surface->scale;
	if (buffer_width == 0 || buffer_height == 0) {
		return; // not yet configured
	}

	if (!surface->dirty || surface->frame) {
		// Nothing to do or frame already pending
		return;
	}

	bool need_destroy = false;
	struct pool_buffer buffer;

	if (buffer_width != surface->last_buffer_width ||
			buffer_height != surface->last_buffer_height) {
		need_destroy = true;
		if (!create_buffer(state->shm, &buffer, buffer_width, buffer_height,
				WL_SHM_FORMAT_ARGB8888)) {
			swaylock_log(LOG_ERROR,
				"Failed to create new buffer for frame background.");
			return;
		}

		cairo_t *cairo = buffer.cairo;
		cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

		cairo_save(cairo);
		cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_u32(cairo, state->args.colors.background);
		cairo_paint(cairo);
		if (surface->image && state->args.mode != BACKGROUND_MODE_SOLID_COLOR) {
			cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
			render_background_image(cairo, surface->image,
				state->args.mode, buffer_width, buffer_height);
		}
		cairo_restore(cairo);
		cairo_identity_matrix(cairo);

		wl_surface_set_buffer_scale(surface->surface, surface->scale);
		wl_surface_attach(surface->surface, buffer.buffer, 0, 0);
		wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
		need_destroy = true;

		surface->last_buffer_width = buffer_width;
		surface->last_buffer_height = buffer_height;
	}

	render_frame(surface);
	surface->dirty = false;
	surface->frame = wl_surface_frame(surface->surface);
	wl_callback_add_listener(surface->frame, &surface_frame_listener, surface);
	wl_surface_commit(surface->surface);

	if (need_destroy) {
		destroy_buffer(&buffer);
	}
}

static void configure_font_drawing(cairo_t *cairo, struct swaylock_state *state,
		enum wl_output_subpixel subpixel, int arc_radius) {
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(subpixel));

	cairo_set_font_options(cairo, fo);
	cairo_select_font_face(cairo, state->args.font,
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	if (state->args.font_size > 0) {
		cairo_set_font_size(cairo, state->args.font_size);
	} else {
		cairo_set_font_size(cairo, arc_radius / 3.0f);
	}
	cairo_font_options_destroy(fo);
}

static void draw_classic_wheel(struct render_context *render_context) {
	// Fill inner circle
	cairo_set_line_width(render_context->cairo, 0);
	cairo_arc(render_context->cairo, render_context->buffer_width / 2, render_context->buffer_diameter / 2,
			render_context->arc_radius - render_context->arc_thickness / 2, 0, 2 * M_PI);
	set_color_for_state(render_context->cairo, render_context->state, &render_context->state->args.colors.inside);
	cairo_fill_preserve(render_context->cairo);
	cairo_stroke(render_context->cairo);

	// Draw ring
	cairo_set_line_width(render_context->cairo, render_context->arc_thickness);
	cairo_arc(render_context->cairo, render_context->buffer_width / 2, render_context->buffer_diameter / 2, render_context->arc_radius,
			0, 2 * M_PI);
	set_color_for_state(render_context->cairo, render_context->state, &render_context->state->args.colors.ring);
	cairo_stroke(render_context->cairo);

	// Draw a message
	configure_font_drawing(render_context->cairo, render_context->state, render_context->surface->subpixel, render_context->arc_radius);
	set_color_for_state(render_context->cairo, render_context->state, &render_context->state->args.colors.text);

	if (render_context->text) {
		cairo_text_extents_t extents;
		cairo_font_extents_t fe;
		double x, y;
		cairo_text_extents(render_context->cairo, render_context->text, &extents);
		cairo_font_extents(render_context->cairo, &fe);
		x = (render_context->buffer_width / 2) -
			(extents.width / 2 + extents.x_bearing);
		y = (render_context->buffer_diameter / 2) +
			(fe.height / 2 - fe.descent);

		cairo_move_to(render_context->cairo, x, y);
		cairo_show_text(render_context->cairo, render_context->text);
		cairo_close_path(render_context->cairo);
		cairo_new_sub_path(render_context->cairo);
	}

	// Typing indicator: Highlight random part on keypress
	if (render_context->state->input_state == INPUT_STATE_LETTER ||
			render_context->state->input_state == INPUT_STATE_BACKSPACE) {
		double highlight_start = render_context->state->highlight_start * (M_PI / 1024.0);
		cairo_arc(render_context->cairo, render_context->buffer_width / 2, render_context->buffer_diameter / 2,
				render_context->arc_radius, highlight_start,
				highlight_start + TYPE_INDICATOR_RANGE);
		if (render_context->state->input_state == INPUT_STATE_LETTER) {
			if (render_context->state->xkb.caps_lock && render_context->state->args.show_caps_lock_indicator) {
				cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.caps_lock_key_highlight);
			} else {
				cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.key_highlight);
			}
		} else {
			if (render_context->state->xkb.caps_lock && render_context->state->args.show_caps_lock_indicator) {
				cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.caps_lock_bs_highlight);
			} else {
				cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.bs_highlight);
			}
		}
		cairo_stroke(render_context->cairo);

		// Draw borders
		double inner_radius = render_context->buffer_diameter / 2.0 - render_context->arc_thickness * 1.5;
		double outer_radius = render_context->buffer_diameter / 2.0 - render_context->arc_thickness / 2.0;

		cairo_set_line_width(render_context->cairo, 2.0 * render_context->surface->scale);
		cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.separator);
		cairo_move_to(render_context->cairo,
			render_context->buffer_width / 2.0 + cos(highlight_start) * inner_radius,
			render_context->buffer_diameter / 2.0 + sin(highlight_start) * inner_radius
		);
		cairo_line_to(render_context->cairo,
			render_context->buffer_width / 2.0 + cos(highlight_start) * outer_radius,
			render_context->buffer_diameter / 2.0 + sin(highlight_start) * outer_radius
		);
		cairo_stroke(render_context->cairo);

		cairo_move_to(render_context->cairo,
			render_context->buffer_width / 2.0 + cos(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius,
			render_context->buffer_diameter / 2.0 + sin(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius
		);
		cairo_line_to(render_context->cairo,
			render_context->buffer_width / 2.0 + cos(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius,
			render_context->buffer_diameter / 2.0 + sin(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius
		);
		cairo_stroke(render_context->cairo);
	}

	// Draw inner + outer border of the circle
	set_color_for_state(render_context->cairo, render_context->state, &render_context->state->args.colors.line);
	cairo_set_line_width(render_context->cairo, 2.0 * render_context->surface->scale);
	cairo_arc(render_context->cairo, render_context->buffer_width / 2, render_context->buffer_diameter / 2,
			render_context->arc_radius - render_context->arc_thickness / 2, 0, 2 * M_PI);
	cairo_stroke(render_context->cairo);
	cairo_arc(render_context->cairo, render_context->buffer_width / 2, render_context->buffer_diameter / 2,
			render_context->arc_radius + render_context->arc_thickness / 2, 0, 2 * M_PI);
	cairo_stroke(render_context->cairo);

	// display layout text separately
	if (render_context->layout_text) {
		cairo_text_extents_t extents;
		cairo_font_extents_t fe;
		double x, y;
		double box_padding = 4.0 * render_context->surface->scale;
		cairo_text_extents(render_context->cairo, render_context->layout_text, &extents);
		cairo_font_extents(render_context->cairo, &fe);
		// upper left coordinates for box
		x = (render_context->buffer_width / 2) - (extents.width / 2) - box_padding;
		y = render_context->buffer_diameter;

		// background box
		cairo_rectangle(render_context->cairo, x, y,
			extents.width + 2.0 * box_padding,
			fe.height + 2.0 * box_padding);
		cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.layout_background);
		cairo_fill_preserve(render_context->cairo);
		// border
		cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.layout_border);
		cairo_stroke(render_context->cairo);

		// take font extents and padding into account
		cairo_move_to(render_context->cairo,
			x - extents.x_bearing + box_padding,
			y + (fe.height - fe.descent) + box_padding);
		cairo_set_source_u32(render_context->cairo, render_context->state->args.colors.layout_text);
		cairo_show_text(render_context->cairo, render_context->layout_text);
		cairo_new_sub_path(render_context->cairo);
	}
}

static bool render_frame(struct swaylock_surface *surface) {
	struct render_context render_context;
	render_context.surface = surface;
	render_context.state = surface->state;

	// First, compute the text that will be drawn, if any, since this
	// determines the size/positioning of the surface

	char attempts[4]; // like i3lock: count no more than 999
	render_context.text = NULL;
	render_context.layout_text = NULL;

	bool draw_indicator = render_context.state->args.show_indicator &&
		(render_context.state->auth_state != AUTH_STATE_IDLE ||
			render_context.state->input_state != INPUT_STATE_IDLE ||
			render_context.state->args.indicator_idle_visible);

	// Prepare the actual text content.
	if (draw_indicator) {
		if (render_context.state->input_state == INPUT_STATE_CLEAR) {
			// This message has highest priority
			render_context.text = "Cleared";
		} else if (render_context.state->auth_state == AUTH_STATE_VALIDATING) {
			render_context.text = "Verifying";
		} else if (render_context.state->auth_state == AUTH_STATE_INVALID) {
			render_context.text = "Wrong";
		} else {
			// Caps Lock has higher priority
			if (render_context.state->xkb.caps_lock && render_context.state->args.show_caps_lock_text) {
				render_context.text = "Caps Lock";
			} else if (render_context.state->args.show_failed_attempts &&
					render_context.state->failed_attempts > 0) {
				if (render_context.state->failed_attempts > 999) {
					render_context.text = "999+";
				} else {
					snprintf(attempts, sizeof(attempts), "%d", render_context.state->failed_attempts);
					render_context.text = attempts;
				}
			}

			if (render_context.state->xkb.keymap) {
				xkb_layout_index_t num_layout = xkb_keymap_num_layouts(render_context.state->xkb.keymap);
				if (!render_context.state->args.hide_keyboard_layout &&
						(render_context.state->args.show_keyboard_layout || num_layout > 1)) {
					xkb_layout_index_t curr_layout = 0;

					// advance to the first active layout (if any)
					while (curr_layout < num_layout &&
						xkb_state_layout_index_is_active(render_context.state->xkb.state,
							curr_layout, XKB_STATE_LAYOUT_EFFECTIVE) != 1) {
						++curr_layout;
					}
					// will handle invalid index if none are active
					render_context.layout_text = xkb_keymap_layout_get_name(render_context.state->xkb.keymap, curr_layout);
				}
			}
		}
	}

	// Compute the size of the buffer needed
	render_context.arc_radius = render_context.state->args.radius * surface->scale;
	render_context.arc_thickness = render_context.state->args.thickness * surface->scale;
	render_context.buffer_diameter = (render_context.arc_radius + render_context.arc_thickness) * 2;
	render_context.buffer_width = render_context.buffer_diameter;
	render_context.buffer_height = render_context.buffer_diameter;

	if (render_context.text || render_context.layout_text) {
		cairo_set_antialias(render_context.state->test_cairo, CAIRO_ANTIALIAS_BEST);
		configure_font_drawing(render_context.state->test_cairo, render_context.state, surface->subpixel, render_context.arc_radius);

		if (render_context.text) {
			cairo_text_extents_t extents;
			cairo_text_extents(render_context.state->test_cairo, render_context.text, &extents);
			if (render_context.buffer_width < extents.width) {
				render_context.buffer_width = extents.width;
			}
		}
		if (render_context.layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double box_padding = 4.0 * surface->scale;
			cairo_text_extents(render_context.state->test_cairo, render_context.layout_text, &extents);
			cairo_font_extents(render_context.state->test_cairo, &fe);
			render_context.buffer_height += fe.height + 2 * box_padding;
			if (render_context.buffer_width < extents.width + 2 * box_padding) {
				render_context.buffer_width = extents.width + 2 * box_padding;
			}
		}
	}
	// Ensure buffer size is multiple of buffer scale - required by protocol
	render_context.buffer_height += surface->scale - (render_context.buffer_height % surface->scale);
	render_context.buffer_width += surface->scale - (render_context.buffer_width % surface->scale);

	int subsurf_xpos;
	int subsurf_ypos;

	// Center the indicator unless overridden by the user
	if (render_context.state->args.override_indicator_x_position) {
		subsurf_xpos = render_context.state->args.indicator_x_position -
			render_context.buffer_width / (2 * surface->scale) + 2 / surface->scale;
	} else {
		subsurf_xpos = surface->width / 2 -
			render_context.buffer_width / (2 * surface->scale) + 2 / surface->scale;
	}

	if (render_context.state->args.override_indicator_y_position) {
		subsurf_ypos = render_context.state->args.indicator_y_position -
			(render_context.state->args.radius + render_context.state->args.thickness);
	} else {
		subsurf_ypos = surface->height / 2 -
			(render_context.state->args.radius + render_context.state->args.thickness);
	}

	struct pool_buffer *buffer = get_next_buffer(render_context.state->shm,
			surface->indicator_buffers, render_context.buffer_width, render_context.buffer_height);
	if (buffer == NULL) {
		swaylock_log(LOG_ERROR, "No buffer");
		return false;
	}

	// Render the buffer
	render_context.cairo = buffer->cairo;
	cairo_set_antialias(render_context.cairo, CAIRO_ANTIALIAS_BEST);

	cairo_identity_matrix(render_context.cairo);

	// Clear
	cairo_save(render_context.cairo);
	cairo_set_source_rgba(render_context.cairo, 0, 0, 0, 0);
	cairo_set_operator(render_context.cairo, CAIRO_OPERATOR_SOURCE);
	cairo_paint(render_context.cairo);
	cairo_restore(render_context.cairo);

	if (draw_indicator) {
		draw_classic_wheel(&render_context);
	}

	// Send Wayland requests
	wl_subsurface_set_position(surface->subsurface, subsurf_xpos, subsurf_ypos);

	wl_surface_set_buffer_scale(surface->child, surface->scale);
	wl_surface_attach(surface->child, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(surface->child, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->child);

	return true;
}
