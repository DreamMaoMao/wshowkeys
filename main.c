#include <assert.h>
#include <errno.h>
#include <cairo/cairo.h>
#include <getopt.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "devmgr.h"
#include "shm.h"
#include "pango.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static struct pool_buffer buffer;

struct wsk_keypress {
	xkb_keysym_t sym;
	char name[128];
	char utf8[128];
	struct wsk_keypress *next;
};

struct wsk_output {
	struct wl_output *output;
	int scale;
	enum wl_output_subpixel subpixel;
	struct wsk_output *next;
};

struct wsk_state {
	int devmgr;
	pid_t devmgr_pid;
	struct udev *udev;
	struct libinput *libinput;

	uint32_t foreground, background, specialfg;
	const char *font;
	int timeout;
	int lenmax;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zxdg_output_manager_v1 *output_mgr;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	struct wsk_output *output, *outputs;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	struct wsk_keypress *keys;
	int ctrl_l_hold;
	int ctrl_r_hold;
	int alt_l_hold;
	int alt_r_hold;
	int super_l_hold;
	int supre_r_hold;
	int shift_l_hold;
	int shift_r_hold;
	struct timespec last_key;

	bool run;
};

void logtofile(const char *fmt, ...) {
  char buf[256];
  char cmd[256];
  va_list ap;
  va_start(ap, fmt);
  vsprintf((char *)buf, fmt, ap);
  va_end(ap);
  unsigned int i = strlen((const char *)buf);

  sprintf(cmd, "echo '%.*s' >> ~/log", i, buf);
  system(cmd);
}

static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

static cairo_subpixel_order_t to_cairo_subpixel_order(
		enum wl_output_subpixel subpixel) {
	switch (subpixel) {
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
		return CAIRO_SUBPIXEL_ORDER_RGB;
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
		return CAIRO_SUBPIXEL_ORDER_BGR;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
		return CAIRO_SUBPIXEL_ORDER_VRGB;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
		return CAIRO_SUBPIXEL_ORDER_VBGR;
	default:
		return CAIRO_SUBPIXEL_ORDER_DEFAULT;
	}
	return CAIRO_SUBPIXEL_ORDER_DEFAULT;
}

static const char *longstr = "󱁐 ⌫ \\ | ";

static void custome_key_name(char *name){
    if (strcmp(name, "Return") == 0) {
        strcpy(name, "⏎ ");
    } else if (strcmp(name, "space") == 0) {
        strcpy(name, "󱁐 ");
    } else if (strcmp(name, "Escape") == 0) {
        strcpy(name, " Esc ");
    } else if (strcmp(name, "Control_L") == 0) {
        strcpy(name, " CTRL+");
    } else if (strcmp(name, "Control_R") == 0) {
        strcpy(name, " CTRL+");
    } else if (strcmp(name, "Alt_L") == 0) {
        strcpy(name, " Alt+");
    } else if (strcmp(name, "Alt_R") == 0) {
        strcpy(name, " Alt+");
    } else if (strcmp(name, "Shift_L") == 0) {
        strcpy(name, " Shift+");
    } else if (strcmp(name, "Shift_R") == 0) {
        strcpy(name, " Shift+");
    } else if (strcmp(name, "Super_L") == 0) {
        strcpy(name, " Super+");
    } else if (strcmp(name, "Super_R") == 0) {
        strcpy(name, " Super+");
    } else if (strcmp(name, "Tab") == 0) {
        strcpy(name, "Tab ");
    } else if (strcmp(name, "backslash") == 0) {
        strcpy(name, "\\");
    } else if (strcmp(name, "BackSpace") == 0) {
        strcpy(name, "⌫ ");
    } else if (strcmp(name, "Caps_Lock") == 0) {
        strcpy(name, "Caps ");
    } else if (strcmp(name, "Left") == 0) {
        strcpy(name, "⇦ ");
    } else if (strcmp(name, "Up") == 0) {
        strcpy(name, "⇧ ");
    } else if (strcmp(name, "Down") == 0) {
        strcpy(name, "⇩ ");
    } else if (strcmp(name, "Right") == 0) {
        strcpy(name, "⇨ ");
    } else if (strcmp(name, "KP_Insert") == 0) {
        strcpy(name, "0");
    } else if (strcmp(name, "KP_End") == 0) {
        strcpy(name, "1");
    } else if (strcmp(name, "KP_Down") == 0) {
        strcpy(name, "2");
    } else if (strcmp(name, "KP_Next") == 0) {
        strcpy(name, "3");
    } else if (strcmp(name, "KP_Left") == 0) {
        strcpy(name, "4");
    } else if (strcmp(name, "KP_Begin") == 0) {
        strcpy(name, "5");
    } else if (strcmp(name, "KP_Right") == 0) {
        strcpy(name, "6");
    } else if (strcmp(name, "KP_Home") == 0) {
        strcpy(name, "7");
    } else if (strcmp(name, "KP_Up") == 0) {
        strcpy(name, "8");
    } else if (strcmp(name, "KP_Prior") == 0) {
        strcpy(name, "9");
    } else if (strcmp(name, "KP_Delete") == 0) {
        strcpy(name, ".");
    } else if (strcmp(name, "KP_Enter") == 0) {
        strcpy(name, "⏎");
    }

}


static void render_to_cairo(cairo_t *cairo, struct wsk_state *state,
		int scale, uint32_t *width, uint32_t *height) {
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background);
	cairo_paint(cairo);

	struct wsk_keypress *key = state->keys;
	while (key) {
		bool special = false;
		char *name = key->utf8;
		if (!name[0]) {
			special = true;
			cairo_set_source_u32(cairo, state->specialfg);
			name = key->name;
		} else {
			cairo_set_source_u32(cairo, state->foreground);
		}

		cairo_move_to(cairo, *width, 0);

		int w, h;
		if (special) {
			custome_key_name(name);
			get_text_size(cairo, state->font, &w, &h, NULL, scale, "%s", name);
			pango_printf(cairo, state->font, scale,  "%s", name);
		} else {
			get_text_size(cairo, state->font, &w, &h, NULL, scale, "%s", name);
			pango_printf(cairo, state->font, scale,  "%s", name);
		}

		*width = *width + w;
		if ((int)*height < h) {
			*height = h;
		}
		key = key->next;
	}
}

static void render_frame(struct wsk_state *state) {
	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	if (state->output) {
		cairo_font_options_set_subpixel_order(
				fo, to_cairo_subpixel_order(state->output->subpixel));
	}
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	int scale = state->output ? state->output->scale : 1;
	uint32_t width = 0, height = 0;
	render_to_cairo(cairo, state, scale, &width, &height);
	if (height / scale != state->height
			|| width / scale != state->width
			|| state->width == 0) {
		// Reconfigure surface
		if (width == 0 || height == 0) {
//			wl_surface_attach(state->surface, NULL, 0, 0);
			;
		} else {
			zwlr_layer_surface_v1_set_size(
					state->layer_surface, width / scale, height / scale);
		}

		// TODO: this could infinite loop if the compositor assigns us a
		// different height than what we asked for
		wl_surface_commit(state->surface);
	} else if (height > 0) {
		// Replay recording into shm and send it off
		if (!create_buffer(state->shm, &buffer, state->width * scale,
				state->height * scale, WL_SHM_FORMAT_ARGB8888)) {
			cairo_surface_destroy(recorder);
			cairo_destroy(cairo);
			return;
		}
		cairo_t *shm = buffer.cairo;

		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);

		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(state->surface, scale);
		wl_surface_attach(state->surface,
				buffer.buffer, 0, 0);
		wl_surface_damage_buffer(state->surface, 0, 0,
				state->width, state->height);
		wl_surface_commit(state->surface);
		destroy_buffer(&buffer);
	}
}

bool
surface_is_configured(struct wsk_state *state)
{
	return (state->width && state->height);
}

static void set_dirty(struct wsk_state *state) {
	if (!surface_is_configured(state)) {
		return;
	}
	if (state->surface) {
		render_frame(state);
	}
}

static void layer_surface_configure(void *data,
			struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1,
			uint32_t serial, uint32_t width, uint32_t height) {
	struct wsk_state *state = data;
	state->width = width;
	state->height = height;
	zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface_v1, serial);
	set_dirty(state);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1) {
	struct wsk_state *state = data;
	state->run = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void surface_enter(void *data,
		struct wl_surface *wl_surface, struct wl_output *output) {
	struct wsk_state *state = data;
	struct wsk_output *wsk_output = state->outputs;
	while (wsk_output->output != output) {
		wsk_output = wsk_output->next;
	}
	state->output = wsk_output;
}

static void surface_leave(void *data,
		struct wl_surface *wl_surface, struct wl_output *output) {
	// Who cares (not really possible with layer shell)
}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct wsk_state *state = data;
	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		fprintf(stderr, "Unable to mmap keymap: %s", strerror(errno));
		return;
	}
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		munmap(map_shm, size);
		close(fd);
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
			state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_state_unref(state->xkb_state);
	state->xkb_keymap = keymap;
	state->xkb_state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	// Who cares
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	// Who cares
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	// Who cares
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	// Who cares
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	// TODO
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(
		void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	struct wsk_state *state = data;
	if (state->keyboard) {
		// TODO: support multiple seats
		return;
	}

	if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		fprintf(stderr, "wl_seat does not support keyboard");
		state->run = false;
		return;
	}

	state->keyboard = wl_seat_get_keyboard(wl_seat);
	wl_keyboard_add_listener(state->keyboard, &wl_keyboard_listener, state);
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
	struct wsk_state *state = data;
	/* TODO: support multiple seats */
	if (libinput_udev_assign_seat(state->libinput, "seat0") != 0) {
		fprintf(stderr, "Failed to assign libinput seat\n");
		state->run = false;
		return;
	}
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct wsk_output *output = data;
	output->subpixel = subpixel;
}

static void output_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void output_done(void *data, struct wl_output *wl_output) {
	// Who cares
}

static void output_scale(void *data,
		struct wl_output *wl_output, int32_t factor) {
	struct wsk_output *output = data;
	output->scale = factor;
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wsk_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(wl_registry,
				name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(wl_registry,
				name, &wl_seat_interface, 5);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->output_mgr = wl_registry_bind(wl_registry,
				name, &zxdg_output_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(wl_registry,
				name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wsk_output *output = calloc(1, sizeof(struct wsk_output));
		output->output = wl_registry_bind(wl_registry,
				name, &wl_output_interface, 3);
		output->scale = 1;
		struct wsk_output **link = &state->outputs;
		while (*link) {
			link = &(*link)->next;
		}
		*link = output;
		wl_output_add_listener(output->output, &wl_output_listener, output);
	}
}

static void registry_global_remove(void *data,
		struct wl_registry *wl_registry, uint32_t name) {
	/* This space deliberately left blank */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

//listen key keydown and record to keys link
static void handle_libinput_event(struct wsk_state *state,
		struct libinput_event *event) {
	if (!state->xkb_state) {
		return;
	}

	enum libinput_event_type event_type = libinput_event_get_type(event);
	if (event_type != LIBINPUT_EVENT_KEYBOARD_KEY) {
		return;
	}

	struct libinput_event_keyboard *kbevent =
		libinput_event_get_keyboard_event(event);

	uint32_t keycode = libinput_event_keyboard_get_key(kbevent) + 8;
	enum libinput_key_state key_state =
		libinput_event_keyboard_get_key_state(kbevent);
	xkb_state_update_key(state->xkb_state, keycode,
			key_state == LIBINPUT_KEY_STATE_RELEASED ?
				XKB_KEY_UP : XKB_KEY_DOWN);

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, keycode);

	struct wsk_keypress *keypress;
	keypress = calloc(1, sizeof(struct wsk_keypress));
	assert(keypress);
	keypress->sym = keysym;
	xkb_keysym_get_name(keypress->sym, keypress->name,
			sizeof(keypress->name));
	if (xkb_state_key_get_utf8(state->xkb_state, keycode,
			keypress->utf8, sizeof(keypress->utf8)) <= 0 ||
			keypress->utf8[0] <= ' ') {
		keypress->utf8[0] = '\0';
	}

	switch (key_state) {
	case LIBINPUT_KEY_STATE_RELEASED:
		if(strlen(keypress->name) > 2 && strstr("Control_LControl_RAlt_LAlt_RSuper_LSuper_RShift_LShift_R",keypress->name)){
			if(strcmp(keypress->name,"Control_L")==0){
				state->ctrl_l_hold = 0;
			} else if(strcmp(keypress->name,"Control_R")==0){
				state->ctrl_r_hold = 0;
			} else if(strcmp(keypress->name,"Alt_L")==0){
				state->alt_l_hold = 0;
			} else if(strcmp(keypress->name,"Alt_R")==0){
				state->alt_r_hold = 0;
			} else if(strcmp(keypress->name,"Super_L")==0){
				state->super_l_hold = 0;
			} else if(strcmp(keypress->name,"Super_R")==0){
				state->supre_r_hold = 0;
			} else if(strcmp(keypress->name,"Shift_L")==0){
				state->shift_l_hold = 0;
			} else if(strcmp(keypress->name,"Shift_R")==0){
				state->shift_r_hold = 0;
			}
		} 
		break;
	case LIBINPUT_KEY_STATE_PRESSED:
		if(strlen(keypress->name) > 2 && strstr("Control_LControl_RAlt_LAlt_RSuper_LSuper_RShift_LShift_R",keypress->name)){
			if(strcmp(keypress->name,"Control_L")==0){
				state->ctrl_l_hold = 1;
			} else if(strcmp(keypress->name,"Control_R")==0){
				state->ctrl_r_hold = 1;
			} else if(strcmp(keypress->name,"Alt_L")==0){
				state->alt_l_hold = 1;
			} else if(strcmp(keypress->name,"Alt_R")==0){
				state->alt_r_hold = 1;
			} else if(strcmp(keypress->name,"Super_L")==0){
				state->super_l_hold = 1;
			} else if(strcmp(keypress->name,"Super_R")==0){
				state->supre_r_hold = 1;
			} else if(strcmp(keypress->name,"Shift_L")==0){
				state->shift_l_hold = 1;
			} else if(strcmp(keypress->name,"Shift_R")==0){
				state->shift_r_hold = 1;
			}
		} else {
			struct wsk_keypress **link = &state->keys;
			while (*link) {
				link = &(*link)->next;
			}

			if(state->ctrl_l_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Control_L");
				*link = temp_keypress;
				link = &(*link)->next;
			} 
			if(state->ctrl_r_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Control_R");
				*link = temp_keypress;
				link = &(*link)->next;
			} 
			if(state->alt_l_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Alt_L");
				*link = temp_keypress;
				link = &(*link)->next;
			}
			if(state->alt_r_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Alt_R");
				*link = temp_keypress;
				link = &(*link)->next;
			}
			if(state->super_l_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Super_L");
				*link = temp_keypress;
				link = &(*link)->next;
			}
			if(state->supre_r_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Super_R");
				*link = temp_keypress;
				link = &(*link)->next;
			}
			if(state->shift_l_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Shift_L");
				*link = temp_keypress;
				link = &(*link)->next;
			}
			if(state->shift_r_hold) {
				struct wsk_keypress *temp_keypress = calloc(1, sizeof(struct wsk_keypress));
				strcpy(temp_keypress->name,"Shift_R");
				*link = temp_keypress;
				link = &(*link)->next;
			}

			*link = keypress;
		}
		break;
	}

	clock_gettime(CLOCK_MONOTONIC, &state->last_key);
	set_dirty(state);
}

static int libinput_open_restricted(const char *path,
		int flags, void *data) {
	int *fd = data;
	return devmgr_open(*fd, path);
}

static void libinput_close_restricted(int fd, void *data) {
	close(fd);
}

static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted,
};

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		fprintf(stderr, "Invalid color %s, defaulting to color "
				"0xFFFFFFFF\n", color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

int main(int argc, char *argv[]) {
	/* NOTICE: This code runs as root */
	struct wsk_state state = { 0 };
	if (devmgr_start(&state.devmgr, &state.devmgr_pid, INPUTDEVPATH) > 0) {
		return 1;
	}

	/* Begin normal user code: */
	int ret = 0;

	unsigned int anchor = 0;
	int margin = 32;
	state.background = 0x000000CC;
	state.specialfg = 0xAAAAAAFF;
	state.foreground = 0xFFFFFFFF;
	state.font = "monospace 24";
	state.timeout = 200;
	state.lenmax = 100;
	state.ctrl_l_hold = 0;
	state.ctrl_r_hold = 0;
	state.alt_l_hold = 0;
	state.alt_r_hold = 0;
	state.super_l_hold = 0;
	state.supre_r_hold = 0;
	state.shift_l_hold = 0;
	state.shift_r_hold = 0;

	int c;
	while ((c = getopt(argc, argv, "hb:f:s:F:t:a:m:o:l:")) != -1) {
		switch (c) {
		case 'l':
			state.lenmax = (int)*optarg;
			break;
		case 'b':
			state.background = parse_color(optarg);
			break;
		case 'f':
			state.foreground = parse_color(optarg);
			break;
		case 's':
			state.specialfg = parse_color(optarg);
			break;
		case 'F':
			state.font = optarg;
			break;
		case 't':
			state.timeout = atoi(optarg);
			break;
		case 'a':
			if (strcmp(optarg, "top") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
			} else if (strcmp(optarg, "left") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
			} else if (strcmp(optarg, "right") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			} else if (strcmp(optarg, "bottom") == 0) {
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
			}
			break;
		case 'm':
			margin = atoi(optarg);
			break;
		case 'o':
			fprintf(stderr, "-o is unimplemented\n");
			return 0;
		default:
			fprintf(stderr, "usage: wshowkeys [-b|-f|-s #RRGGBB[AA]] [-F font] "
					"[-t timeout]\n\t[-a top|left|right|bottom] [-m margin] "
					"[-o output]\n[-l lenmax]");
			return 1;
		}
	}

	state.udev = udev_new();
	if (!state.udev) {
		fprintf(stderr, "udev_create: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.libinput = libinput_udev_create_context(
			&libinput_impl, &state.devmgr, state.udev);
	udev_unref(state.udev);
	if (!state.libinput) {
		fprintf(stderr, "libinput_udev_create_context: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!state.xkb_context) {
		fprintf(stderr, "xkb_context_new: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		fprintf(stderr, "wl_display_connect: %s\n", strerror(errno));
		ret = 1;
		goto exit;
	}

	state.registry = wl_display_get_registry(state.display);
	assert(state.registry);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	struct {
		const char *name;
		void *ptr;
	} need_globals[] = {
		"wl_compositor", &state.compositor,
		"wl_shm", &state.shm,
		"wl_seat", &state.seat,
		"wlr_layer_shell", &state.layer_shell,
	};
	for (size_t i = 0; i < sizeof(need_globals) / sizeof(need_globals[0]); ++i) {
		if (!need_globals[i].ptr) {
			fprintf(stderr, "Error: required Wayland interface '%s' "
					"is not present\n", need_globals[i].name);
			ret = 1;
			goto exit;
		}
	}

	// TODO: Listener for xdg output

	wl_seat_add_listener(state.seat, &wl_seat_listener, &state);
	wl_display_roundtrip(state.display);
	
	state.surface = wl_compositor_create_surface(state.compositor);
	assert(state.surface);
	wl_surface_add_listener(state.surface, &wl_surface_listener, &state);

	state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state.layer_shell, state.surface, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP, "showkeys");
	assert(state.layer_surface);
	zwlr_layer_surface_v1_add_listener(
			state.layer_surface, &layer_surface_listener, &state);
	zwlr_layer_surface_v1_set_size(state.layer_surface, 1, 1);
	zwlr_layer_surface_v1_set_anchor(state.layer_surface, anchor);
	zwlr_layer_surface_v1_set_margin(state.layer_surface,
			margin, margin, margin, margin);
	zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
	wl_surface_commit(state.surface);

	struct pollfd pollfds[] = {
		{ .fd = libinput_get_fd(state.libinput), .events = POLLIN, },
		{ .fd = wl_display_get_fd(state.display), .events = POLLIN, },
	};

	state.run = true;
	while (state.run) {
		errno = 0;
		do {
			if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		int timeout = -1;
		if (state.keys) {
			timeout = 200;
		}

		if (poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), timeout) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		/* Clear out old keys */
		struct timespec now;
		struct wsk_keypress *key = state.keys;
		int all_key_len = 0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > state.last_key.tv_sec + 1) {
			while (key) {
				struct wsk_keypress *next = key->next;
				free(key);
				key = next;
			}
			state.keys = NULL;
			set_dirty(&state);
		} else if (now.tv_sec == state.last_key.tv_sec &&
				now.tv_nsec > state.last_key.tv_nsec + (state.timeout * 1000000) ){
				while (key) {
					struct wsk_keypress *next = key->next;
					free(key);
					key = next;
				}
				state.keys = NULL;
				set_dirty(&state);					
		} else {
			char *temp_name = calloc(1, 129);
			while (key) {
				strcpy(temp_name,key->name);
				custome_key_name(temp_name);
				if(strstr(longstr,temp_name)) {
					all_key_len = all_key_len + 2;
				} else {
					all_key_len = all_key_len + strlen(temp_name);
				}
				struct wsk_keypress *next = key->next;
				key = next;
			}
			free(temp_name);
			if(all_key_len > state.lenmax){
				key = state.keys;
				struct wsk_keypress *next = key->next;
				free(key);
				state.keys = next;
				set_dirty(&state);					
			}
		}

		if ((pollfds[0].revents & POLLIN)) {
			if (libinput_dispatch(state.libinput) != 0) {
				fprintf(stderr, "libinput_dispatch: %s\n", strerror(errno));
				break;
			}
			struct libinput_event *event;
			while ((event = libinput_get_event(state.libinput))) {
				handle_libinput_event(&state, event);
				libinput_event_destroy(event);
			}
		}

		if ((pollfds[1].revents & POLLIN)
				&& wl_display_dispatch(state.display) == -1) {
			fprintf(stderr, "wl_display_dispatch: %s\n", strerror(errno));
			break;
		}
	}

exit:
	wl_display_disconnect(state.display);
	libinput_unref(state.libinput);
	devmgr_finish(state.devmgr, state.devmgr_pid);
	return ret;
}
