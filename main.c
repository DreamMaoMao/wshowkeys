#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <assert.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* ---- 共享内存 buffer 的简单封装 ---- */
struct pool_buffer {
	struct wl_buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cairo;
	void *data;
	size_t size;
	int width, height;
};

static int create_shm_file(void) {
	char name[] = "/tmp/wshowkeys-XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0)
		return -1;
	unlink(name);
	return fd;
}

static bool create_buffer(struct wl_shm *shm, struct pool_buffer *buf,
						  int width, int height, uint32_t format) {
	int stride = width * 4;
	int size = stride * height;
	int fd = create_shm_file();
	if (fd < 0)
		return false;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return false;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buf->buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->data = data;
	buf->size = size;
	buf->width = width;
	buf->height = height;
	buf->surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, width, height, stride);
	buf->cairo = cairo_create(buf->surface);
	return true;
}

static void destroy_buffer(struct pool_buffer *buf) {
	if (buf->cairo)
		cairo_destroy(buf->cairo);
	if (buf->surface)
		cairo_surface_destroy(buf->surface);
	if (buf->buffer)
		wl_buffer_destroy(buf->buffer);
	if (buf->data)
		munmap(buf->data, buf->size);
	memset(buf, 0, sizeof(*buf));
}

/* ---- 用 Pango 计算和绘制文本 ---- */
static void get_text_size(cairo_t *cairo, const char *font, int *width,
						  int *height, int *baseline, int scale,
						  const char *fmt, ...) {
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	PangoLayout *layout = pango_cairo_create_layout(cairo);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);

	PangoRectangle logical;
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	if (width)
		*width = logical.width;
	if (height)
		*height = logical.height;
	if (baseline)
		*baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	g_object_unref(layout);
}

static void pango_printf(cairo_t *cairo, const char *font, int scale,
						 const char *fmt, ...) {
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	PangoLayout *layout = pango_cairo_create_layout(cairo);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);
}

/* ---- libinput 设备打开辅助（简单版，不需要额外的 devmgr） ---- */
#ifndef INPUTDEVPATH
#define INPUTDEVPATH "/dev/input/"
#endif

static int devmgr_start(int *fd, pid_t *pid, const char *path) {
	// 这里不维护子进程，直接返回成功
	*fd = -1;
	*pid = 0;
	return 0;
}

static void devmgr_finish(int fd, pid_t pid) {
	(void)fd;
	(void)pid;
}

static int devmgr_open(int mgr_fd, const char *path) {
	return open(path, O_RDONLY | O_NONBLOCK);
}

/* ---- 每个按键在屏幕上对应的信息 ---- */
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

/* ---- 整个应用的状态 ---- */
struct wsk_state {
	int devmgr;
	pid_t devmgr_pid;
	struct udev *udev;
	struct libinput *libinput;

	uint32_t foreground, background, specialfg;
	const char *font;
	int timeout;
	int length_limit;
	bool show_mods;			 // 是否显示修饰键面板，-M
	bool show_mouse_buttons; // 是否显示鼠标按键状态，-U
	bool show_scroll;		 // 是否显示滚轮状态，-S

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zxdg_output_manager_v1 *output_mgr;
	struct zwlr_layer_shell_v1 *layer_shell;

	// 两个 layer surface：上面的是修饰键 / 鼠标 / 滚轮指示器，下面的是按键记录
	struct wl_surface *keys_surface;
	struct zwlr_layer_surface_v1 *keys_layer_surface;
	uint32_t keys_width, keys_height;

	struct wl_surface *mods_surface;
	struct zwlr_layer_surface_v1 *mods_layer_surface;
	uint32_t mods_width, mods_height;

	struct wsk_output *output, *outputs;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	struct wsk_keypress *keys;
	struct timespec last_key;

	bool run;
	int ctrl_l_hold, ctrl_r_hold;
	int alt_l_hold, alt_r_hold;
	int super_l_hold, supre_r_hold;
	int shift_l_hold, shift_r_hold;

	// 鼠标三个按钮的按下状态
	int mouse_left, mouse_middle, mouse_right;

	// 滚轮方向是否激活（短暂高亮）
	int scroll_up_active;
	int scroll_down_active;
	struct timespec last_scroll;

	char current_combination_key[128];
	char prev_combination_keye[128];
	int combination_keye_repetition;

	// 超时后不立刻清屏，先打标记，等下一次按键才真正清除
	bool pending_clear;
};

// 提前声明，后面实现
static void clear_full_keylink(struct wsk_state *state);
static void set_dirty(struct wsk_state *state);

static struct pool_buffer buffer_mods;
static struct pool_buffer buffer_keys;

/* ---------- 工具函数 ---------- */
static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(
		cairo, ((color >> 24) & 0xFF) / 255.0, ((color >> 16) & 0xFF) / 255.0,
		((color >> 8) & 0xFF) / 255.0, ((color >> 0) & 0xFF) / 255.0);
}

static cairo_subpixel_order_t
to_cairo_subpixel_order(enum wl_output_subpixel subpixel) {
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
}

// 估算一个按键名在屏幕上占多少“字符宽度”，用于限制总长度
static int get_char_width(char *name) {
	if (strstr("⏎ ␣ ⇦ ⇧ ⇨ ", name))
		return 4;
	if (strstr("⌫ F12 F10 F11  Esc ", name))
		return 5;
	if (strstr("ijl-\\*()!,./[]{}012", name))
		return 1;
	if (strstr("-=+abcdefghkmnoqrstuvwxyz@#$%^&?><", name))
		return 2;
	if (strstr("F1F2F3F4F5F6F7F8F93456789", name))
		return 3;
	if (strstr(" Ctrl+", name))
		return 8;
	if (strstr(" Alt+", name))
		return 6;
	if (strstr(" Shift+", name))
		return 10;
	if (strstr(" Super+", name))
		return 10;
	if (strstr("Tab ", name))
		return 10;
	if (strstr("Caps ", name))
		return 8;
	return strlen(name);
}

// 把一些键的原始名字替换成好看一点的符号
static void custome_key_name(char *name) {
	if (strcmp(name, "Return") == 0) {
		strcpy(name, "⏎ ");
	} else if (strcmp(name, "space") == 0) {
		strcpy(name, "␣ ");
	} else if (strcmp(name, "Escape") == 0) {
		strcpy(name, " Esc ");
	} else if (strcmp(name, "Control_L") == 0) {
		strcpy(name, " Ctrl+");
	} else if (strcmp(name, "Control_R") == 0) {
		strcpy(name, " Ctrl+");
	} else if (strcmp(name, "Alt_L") == 0 || strcmp(name, "Meta_L") == 0) {
		strcpy(name, " Alt+");
	} else if (strcmp(name, "Alt_R") == 0 || strcmp(name, "Meta_R") == 0) {
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
		strcpy(name, "⏎ ");
	}
}

// 从主字体名派生出用于修饰键 / 指示器的小号字体
static void get_mod_font(const char *font, char *mod_font, size_t buf_size) {
	int size = 24;
	const char *last_space = strrchr(font, ' ');
	if (last_space && sscanf(last_space + 1, "%d", &size) == 1) {
		size = (int)(size * 0.6);
		if (size < 8)
			size = 8;
		size_t prefix_len = last_space - font;
		snprintf(mod_font, buf_size, "%.*s %d", (int)prefix_len, font, size);
		return;
	}
	snprintf(mod_font, buf_size, "Sans Bold %d", size);
}

// 用 Pango 量一段文本的像素尺寸和基线偏移
static void get_text_metrics(cairo_t *cairo, const char *font, const char *text,
							 int scale, int *width, int *height,
							 int *baseline) {
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoRectangle logical;
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	*width = logical.width;
	*height = logical.height;
	*baseline = -logical.y;
	g_object_unref(layout);
}

// 计算整个指示器面板需要的像素尺寸（修饰键 + 可选鼠标 + 可选滚轮）
static void calc_mods_size(struct wsk_state *state, int scale, uint32_t *width,
						   uint32_t *height) {
	cairo_surface_t *dummy =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(dummy);
	char mod_font[256];
	get_mod_font(state->font, mod_font, sizeof(mod_font));
	const char *mod_names[] = {"Ctrl", "Super", "Alt", "Shift"};
	const int num_mods = 4;
	const int pad_h = 8, pad_v = 4, gap = 8;
	int max_tw = 0, max_th = 0, max_bl = 0;

	// 修饰键
	for (int i = 0; i < num_mods; i++) {
		int tw, th, bl;
		get_text_metrics(cairo, mod_font, mod_names[i], scale, &tw, &th, &bl);
		if (tw > max_tw)
			max_tw = tw;
		if (th > max_th)
			max_th = th;
		if (bl > max_bl)
			max_bl = bl;
	}

	// 鼠标按钮 L M R
	if (state->show_mouse_buttons) {
		const char *mouse_names[] = {"L", "M", "R"};
		for (int i = 0; i < 3; i++) {
			int tw, th, bl;
			get_text_metrics(cairo, mod_font, mouse_names[i], scale, &tw, &th,
							 &bl);
			if (tw > max_tw)
				max_tw = tw;
			if (th > max_th)
				max_th = th;
			if (bl > max_bl)
				max_bl = bl;
		}
	}

	// 滚轮 ▲ ▼
	if (state->show_scroll) {
		const char *scroll_names[] = {"▲", "▼"};
		for (int i = 0; i < 2; i++) {
			int tw, th, bl;
			get_text_metrics(cairo, mod_font, scroll_names[i], scale, &tw, &th,
							 &bl);
			if (tw > max_tw)
				max_tw = tw;
			if (th > max_th)
				max_th = th;
			if (bl > max_bl)
				max_bl = bl;
		}
	}

	int box_w = max_tw + 2 * pad_h;
	int box_h = max_th + 2 * pad_v;
	int total_boxes = num_mods;
	if (state->show_mouse_buttons)
		total_boxes += 3;
	if (state->show_scroll)
		total_boxes += 2;
	*width = total_boxes * box_w + (total_boxes - 1) * gap;
	*height = box_h;
	cairo_destroy(cairo);
	cairo_surface_destroy(dummy);
}

// 只绘制指示器面板的内容（不画整个 surface 的背景），返回像素宽高
static void render_mods_only(cairo_t *cairo, struct wsk_state *state, int scale,
							 uint32_t *width, uint32_t *height) {
	char mod_font[256];
	get_mod_font(state->font, mod_font, sizeof(mod_font));
	const char *mod_names[] = {"Ctrl", "Super", "Alt", "Shift"};
	bool mod_active[] = {state->ctrl_l_hold || state->ctrl_r_hold,
						 state->super_l_hold || state->supre_r_hold,
						 state->alt_l_hold || state->alt_r_hold,
						 state->shift_l_hold || state->shift_r_hold};
	const int num_mods = 4;
	const int pad_h = 8, pad_v = 4, gap = 8;

	int max_tw = 0, max_th = 0, max_bl = 0;

	// 先测量所有可能出现的文字，找出最大宽度 / 高度 / 基线
	for (int i = 0; i < num_mods; i++) {
		int tw, th, bl;
		get_text_metrics(cairo, mod_font, mod_names[i], scale, &tw, &th, &bl);
		if (tw > max_tw)
			max_tw = tw;
		if (th > max_th)
			max_th = th;
		if (bl > max_bl)
			max_bl = bl;
	}
	if (state->show_mouse_buttons) {
		const char *mouse_names[] = {"L", "M", "R"};
		for (int i = 0; i < 3; i++) {
			int tw, th, bl;
			get_text_metrics(cairo, mod_font, mouse_names[i], scale, &tw, &th,
							 &bl);
			if (tw > max_tw)
				max_tw = tw;
			if (th > max_th)
				max_th = th;
			if (bl > max_bl)
				max_bl = bl;
		}
	}
	if (state->show_scroll) {
		const char *scroll_names[] = {"▲", "▼"};
		for (int i = 0; i < 2; i++) {
			int tw, th, bl;
			get_text_metrics(cairo, mod_font, scroll_names[i], scale, &tw, &th,
							 &bl);
			if (tw > max_tw)
				max_tw = tw;
			if (th > max_th)
				max_th = th;
			if (bl > max_bl)
				max_bl = bl;
		}
	}

	int box_w = max_tw + 2 * pad_h;
	int box_h = max_th + 2 * pad_v;
	int total_boxes = num_mods;
	if (state->show_mouse_buttons)
		total_boxes += 3;
	if (state->show_scroll)
		total_boxes += 2;
	uint32_t total_w = total_boxes * box_w + (total_boxes - 1) * gap;
	uint32_t total_h = box_h;

	int box_index = 0;

	// 绘制四个修饰键
	for (int i = 0; i < num_mods; i++) {
		int x = box_index * (box_w + gap);
		int y = 0;

		if (mod_active[i])
			cairo_set_source_u32(cairo, state->specialfg);
		else
			cairo_set_source_u32(cairo, state->background);
		cairo_rectangle(cairo, x, y, box_w, box_h);
		cairo_fill_preserve(cairo);
		cairo_set_source_u32(cairo, state->foreground);
		cairo_set_line_width(cairo, 2.0);
		cairo_stroke(cairo);

		cairo_set_source_u32(cairo, mod_active[i] ? state->background
												  : state->foreground);

		PangoLayout *layout = pango_cairo_create_layout(cairo);
		PangoFontDescription *desc =
			pango_font_description_from_string(mod_font);
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, mod_names[i], -1);

		PangoRectangle logical;
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		int tw = logical.width;
		int text_x = x + pad_h + (max_tw - tw) / 2;
		int text_y = y + pad_v + max_bl;

		cairo_move_to(cairo, text_x, text_y);
		pango_cairo_show_layout(cairo, layout);
		g_object_unref(layout);
		box_index++;
	}

	// 绘制鼠标左中右键（如果启用）
	if (state->show_mouse_buttons) {
		const char *mouse_names[] = {"L", "M", "R"};
		bool mouse_active[] = {state->mouse_left, state->mouse_middle,
							   state->mouse_right};
		for (int i = 0; i < 3; i++) {
			int x = box_index * (box_w + gap);
			int y = 0;

			if (mouse_active[i])
				cairo_set_source_u32(cairo, state->specialfg);
			else
				cairo_set_source_u32(cairo, state->background);
			cairo_rectangle(cairo, x, y, box_w, box_h);
			cairo_fill_preserve(cairo);
			cairo_set_source_u32(cairo, state->foreground);
			cairo_set_line_width(cairo, 2.0);
			cairo_stroke(cairo);

			cairo_set_source_u32(cairo, mouse_active[i] ? state->background
														: state->foreground);

			PangoLayout *layout = pango_cairo_create_layout(cairo);
			PangoFontDescription *desc =
				pango_font_description_from_string(mod_font);
			pango_layout_set_font_description(layout, desc);
			pango_font_description_free(desc);
			pango_layout_set_text(layout, mouse_names[i], -1);

			PangoRectangle logical;
			pango_layout_get_pixel_extents(layout, NULL, &logical);
			int tw = logical.width;
			int text_x = x + pad_h + (max_tw - tw) / 2;
			int text_y = y + pad_v + max_bl;

			cairo_move_to(cairo, text_x, text_y);
			pango_cairo_show_layout(cairo, layout);
			g_object_unref(layout);
			box_index++;
		}
	}

	// 绘制滚轮方向（如果启用）
	if (state->show_scroll) {
		const char *scroll_names[] = {"▲", "▼"};
		bool scroll_active[] = {state->scroll_up_active,
								state->scroll_down_active};
		for (int i = 0; i < 2; i++) {
			int x = box_index * (box_w + gap);
			int y = 0;

			if (scroll_active[i])
				cairo_set_source_u32(cairo, state->specialfg);
			else
				cairo_set_source_u32(cairo, state->background);
			cairo_rectangle(cairo, x, y, box_w, box_h);
			cairo_fill_preserve(cairo);
			cairo_set_source_u32(cairo, state->foreground);
			cairo_set_line_width(cairo, 2.0);
			cairo_stroke(cairo);

			cairo_set_source_u32(cairo, scroll_active[i] ? state->background
														 : state->foreground);

			PangoLayout *layout = pango_cairo_create_layout(cairo);
			PangoFontDescription *desc =
				pango_font_description_from_string(mod_font);
			pango_layout_set_font_description(layout, desc);
			pango_font_description_free(desc);
			pango_layout_set_text(layout, scroll_names[i], -1);

			PangoRectangle logical;
			pango_layout_get_pixel_extents(layout, NULL, &logical);
			int tw = logical.width;
			int text_x = x + pad_h + (max_tw - tw) / 2;
			int text_y = y + pad_v + max_bl;

			cairo_move_to(cairo, text_x, text_y);
			pango_cairo_show_layout(cairo, layout);
			g_object_unref(layout);
			box_index++;
		}
	}

	*width = total_w;
	*height = total_h;
}

// 只绘制按键行（不画背景），返回像素宽高
static void render_keys_only(cairo_t *cairo, struct wsk_state *state, int scale,
							 uint32_t *width, uint32_t *height) {
	uint32_t w = 0, h = 0;
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

		cairo_move_to(cairo, w, 0);
		int tw, th;
		if (special) {
			custome_key_name(name);
			get_text_size(cairo, state->font, &tw, &th, NULL, scale, "%s",
						  name);
			pango_printf(cairo, state->font, scale, "%s", name);
		} else {
			get_text_size(cairo, state->font, &tw, &th, NULL, scale, "%s",
						  name);
			pango_printf(cairo, state->font, scale, "%s", name);
		}
		w += tw;
		if ((int)h < th)
			h = th;
		key = key->next;
	}
	*width = w;
	*height = h;
}

// 绘制完整一帧到修饰键 surface
static void render_mods_frame(struct wsk_state *state) {
	if (!state->show_mods || !state->mods_surface || !state->mods_width ||
		!state->mods_height)
		return;
	int scale = state->output ? state->output->scale : 1;

	cairo_surface_t *recorder =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	if (state->output)
		cairo_font_options_set_subpixel_order(
			fo, to_cairo_subpixel_order(state->output->subpixel));
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background);
	cairo_paint(cairo);

	uint32_t w, h;
	render_mods_only(cairo, state, scale, &w, &h);

	if (w != state->mods_width || h != state->mods_height) {
		zwlr_layer_surface_v1_set_size(state->mods_layer_surface, w / scale,
									   h / scale);
		wl_surface_commit(state->mods_surface);
	} else {
		if (!create_buffer(state->shm, &buffer_mods, state->mods_width * scale,
						   state->mods_height * scale,
						   WL_SHM_FORMAT_ARGB8888)) {
			cairo_destroy(cairo);
			cairo_surface_destroy(recorder);
			return;
		}
		cairo_t *shm = buffer_mods.cairo;
		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);
		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(state->mods_surface, scale);
		wl_surface_attach(state->mods_surface, buffer_mods.buffer, 0, 0);
		wl_surface_damage_buffer(state->mods_surface, 0, 0, state->mods_width,
								 state->mods_height);
		wl_surface_commit(state->mods_surface);
		destroy_buffer(&buffer_mods);
	}
	cairo_destroy(cairo);
	cairo_surface_destroy(recorder);
}

// 绘制完整一帧到按键 surface
static void render_keys_frame(struct wsk_state *state) {
	if (!state->keys_surface || !state->keys_width || !state->keys_height)
		return;
	int scale = state->output ? state->output->scale : 1;

	cairo_surface_t *recorder =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	if (state->output)
		cairo_font_options_set_subpixel_order(
			fo, to_cairo_subpixel_order(state->output->subpixel));
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background);
	cairo_paint(cairo);

	uint32_t w, h;
	render_keys_only(cairo, state, scale, &w, &h);

	uint32_t new_w = w / scale, new_h = h / scale;
	if (new_w < 1)
		new_w = 1;
	if (new_h < 1)
		new_h = 1;

	if (new_h != state->keys_height || new_w != state->keys_width) {
		zwlr_layer_surface_v1_set_size(state->keys_layer_surface, new_w, new_h);
		wl_surface_commit(state->keys_surface);
	} else {
		if (!create_buffer(state->shm, &buffer_keys, state->keys_width * scale,
						   state->keys_height * scale,
						   WL_SHM_FORMAT_ARGB8888)) {
			cairo_destroy(cairo);
			cairo_surface_destroy(recorder);
			return;
		}
		cairo_t *shm = buffer_keys.cairo;
		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);
		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(state->keys_surface, scale);
		wl_surface_attach(state->keys_surface, buffer_keys.buffer, 0, 0);
		wl_surface_damage_buffer(state->keys_surface, 0, 0, state->keys_width,
								 state->keys_height);
		wl_surface_commit(state->keys_surface);
		destroy_buffer(&buffer_keys);
	}
	cairo_destroy(cairo);
	cairo_surface_destroy(recorder);
}

// 标记需要重绘，同时刷新两个 layer surface
static void set_dirty(struct wsk_state *state) {
	if (state->show_mods)
		render_mods_frame(state);
	render_keys_frame(state);
}

/* ---------- Layer surface 回调 ---------- */
static void mods_layer_surface_configure(void *data,
										 struct zwlr_layer_surface_v1 *surface,
										 uint32_t serial, uint32_t width,
										 uint32_t height) {
	struct wsk_state *state = data;
	state->mods_width = width;
	state->mods_height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_mods_frame(state);
}
static void mods_layer_surface_closed(void *data,
									  struct zwlr_layer_surface_v1 *surface) {
	struct wsk_state *state = data;
	state->run = false;
}
static const struct zwlr_layer_surface_v1_listener mods_layer_surface_listener =
	{
		.configure = mods_layer_surface_configure,
		.closed = mods_layer_surface_closed,
};

static void keys_layer_surface_configure(void *data,
										 struct zwlr_layer_surface_v1 *surface,
										 uint32_t serial, uint32_t width,
										 uint32_t height) {
	struct wsk_state *state = data;
	state->keys_width = width;
	state->keys_height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_keys_frame(state);
}
static void keys_layer_surface_closed(void *data,
									  struct zwlr_layer_surface_v1 *surface) {
	// 按键窗口关闭我们不管，程序结束时会统一清理
}
static const struct zwlr_layer_surface_v1_listener keys_layer_surface_listener =
	{
		.configure = keys_layer_surface_configure,
		.closed = keys_layer_surface_closed,
};

// surface 进入 / 离开某个输出时，更新当前 scale 等
static void surface_enter(void *data, struct wl_surface *wl_surface,
						  struct wl_output *output) {
	struct wsk_state *state = data;
	struct wsk_output *out = state->outputs;
	while (out && out->output != output)
		out = out->next;
	if (out)
		state->output = out;
}
static void surface_leave(void *data, struct wl_surface *wl_surface,
						  struct wl_output *output) {}
static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

/* ---------- 键盘相关 ---------- */
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
						   uint32_t serial, struct wl_surface *surface,
						   struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
						   uint32_t serial, struct wl_surface *surface) {}
static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
						 uint32_t serial, uint32_t time, uint32_t key,
						 uint32_t state) {}
static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
							   uint32_t serial, uint32_t mods_depressed,
							   uint32_t mods_latched, uint32_t mods_locked,
							   uint32_t group) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
								 int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* ---------- Seat ---------- */
static void seat_capabilities(void *data, struct wl_seat *wl_seat,
							  uint32_t capabilities) {
	struct wsk_state *state = data;
	if (state->keyboard)
		return;
	if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		fprintf(stderr, "wl_seat does not support keyboard\n");
		state->run = false;
		return;
	}
	state->keyboard = wl_seat_get_keyboard(wl_seat);
	wl_keyboard_add_listener(state->keyboard, &wl_keyboard_listener, state);
}
static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
	struct wsk_state *state = data;
	if (libinput_udev_assign_seat(state->libinput, "seat0") != 0) {
		fprintf(stderr, "Failed to assign libinput seat\n");
		state->run = false;
	}
}
static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---------- Output ---------- */
static void output_geometry(void *data, struct wl_output *wl_output, int32_t x,
							int32_t y, int32_t physical_width,
							int32_t physical_height, int32_t subpixel,
							const char *make, const char *model,
							int32_t transform) {
	struct wsk_output *output = data;
	output->subpixel = subpixel;
}
static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
						int32_t width, int32_t height, int32_t refresh) {}
static void output_done(void *data, struct wl_output *wl_output) {}
static void output_scale(void *data, struct wl_output *wl_output,
						 int32_t factor) {
	struct wsk_output *output = data;
	output->scale = factor;
}
static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

/* ---------- Registry ---------- */
static void registry_global(void *data, struct wl_registry *wl_registry,
							uint32_t name, const char *interface,
							uint32_t version) {
	struct wsk_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
			wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat =
			wl_registry_bind(wl_registry, name, &wl_seat_interface, 5);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->output_mgr = wl_registry_bind(
			wl_registry, name, &zxdg_output_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(
			wl_registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wsk_output *output = calloc(1, sizeof(struct wsk_output));
		output->output =
			wl_registry_bind(wl_registry, name, &wl_output_interface, 3);
		output->scale = 1;
		struct wsk_output **link = &state->outputs;
		while (*link)
			link = &(*link)->next;
		*link = output;
		wl_output_add_listener(output->output, &wl_output_listener, output);
	}
}
static void registry_global_remove(void *data, struct wl_registry *wl_registry,
								   uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------- 按键组合显示与重复检测 ---------- */
static int caculat_del_charnum_of_int(int num) {
	int count = 0;
	if (num == 1)
		return 0;
	while (num != 0) {
		num /= 10;
		++count;
	}
	return count + 2;
}
static int caculat_add_charnum_of_int(int num) {
	int count = 0;
	while (num != 0) {
		num /= 10;
		++count;
	}
	return count + 1;
}
static void del_last_key(struct wsk_state *state, int n) {
	while (n > 0) {
		struct wsk_keypress **link = &state->keys;
		while (*link) {
			struct wsk_keypress **next = &(*link)->next;
			if (*next == NULL) {
				free(*link);
				*link = NULL;
				break;
			}
			link = next;
		}
		n--;
	}
}
static void attach_to_last(struct wsk_state *state, struct wsk_keypress *key) {
	struct wsk_keypress **attach = &state->keys;
	while (*attach)
		attach = &(*attach)->next;
	*attach = key;
}
static void change_numchar_to_special(char *target, char numchar) {
	switch (numchar) {
	case '0':
		strcpy(target, "₀");
		break;
	case '1':
		strcpy(target, "₁");
		break;
	case '2':
		strcpy(target, "₂");
		break;
	case '3':
		strcpy(target, "₃");
		break;
	case '4':
		strcpy(target, "₄");
		break;
	case '5':
		strcpy(target, "₅");
		break;
	case '6':
		strcpy(target, "₆");
		break;
	case '7':
		strcpy(target, "₇");
		break;
	case '8':
		strcpy(target, "₈");
		break;
	case '9':
		strcpy(target, "₉");
		break;
	}
}
static void attach_repeat_flag(struct wsk_state *state, int num, int num_len) {
	struct wsk_keypress *repeat_flag = calloc(1, sizeof(struct wsk_keypress));
	strcpy(repeat_flag->name, "ₓ");
	attach_to_last(state, repeat_flag);
	char *repeat_num_char = calloc(num_len + 1, sizeof(char));
	sprintf(repeat_num_char, "%d", num);
	for (int i = 0; i < num_len; i++) {
		struct wsk_keypress *repeat_num =
			calloc(1, sizeof(struct wsk_keypress));
		change_numchar_to_special(repeat_num->name, repeat_num_char[i]);
		attach_to_last(state, repeat_num);
	}
	free(repeat_num_char);
}

static void clear_full_keylink(struct wsk_state *state) {
	struct wsk_keypress *key = state->keys;
	while (key) {
		struct wsk_keypress *next = key->next;
		free(key);
		key = next;
	}
	state->combination_keye_repetition = 1;
	memset(state->current_combination_key, 0,
		   sizeof(state->current_combination_key));
	memset(state->prev_combination_keye, 0,
		   sizeof(state->prev_combination_keye));
	state->keys = NULL;
	state->pending_clear = false;
	set_dirty(state);
}

// 处理来自 libinput 的事件：按键、鼠标按钮、滚轮
static void handle_libinput_event(struct wsk_state *state,
								  struct libinput_event *event) {
	// 鼠标按钮事件
	if (libinput_event_get_type(event) == LIBINPUT_EVENT_POINTER_BUTTON) {
		struct libinput_event_pointer *pev =
			libinput_event_get_pointer_event(event);
		uint32_t button = libinput_event_pointer_get_button(pev);
		enum libinput_button_state bstate =
			libinput_event_pointer_get_button_state(pev);
		int pressed = (bstate == LIBINPUT_BUTTON_STATE_PRESSED);
		switch (button) {
		case BTN_LEFT:
			state->mouse_left = pressed;
			break;
		case BTN_MIDDLE:
			state->mouse_middle = pressed;
			break;
		case BTN_RIGHT:
			state->mouse_right = pressed;
			break;
		default:
			return; // 其他按键不关心
		}
		set_dirty(state);
		return;
	}

	// 滚轮事件（仅垂直方向）
	if (libinput_event_get_type(event) == LIBINPUT_EVENT_POINTER_AXIS) {
		struct libinput_event_pointer *pev =
			libinput_event_get_pointer_event(event);
		double value = libinput_event_pointer_get_axis_value(
			pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
		if (value > 0.0) {
			state->scroll_down_active = 1;
			state->scroll_up_active = 0;
		} else if (value < 0.0) {
			state->scroll_up_active = 1;
			state->scroll_down_active = 0;
		}
		if (value != 0.0) {
			clock_gettime(CLOCK_MONOTONIC, &state->last_scroll);
			set_dirty(state);
		}
		return;
	}

	// 不是键盘事件就忽略
	if (!state->xkb_state)
		return;
	if (libinput_event_get_type(event) != LIBINPUT_EVENT_KEYBOARD_KEY)
		return;

	// 如果标记了超时待清，先清掉旧的按键显示
	if (state->pending_clear) {
		clear_full_keylink(state);
	}

	struct libinput_event_keyboard *kbevent =
		libinput_event_get_keyboard_event(event);
	uint32_t keycode = libinput_event_keyboard_get_key(kbevent) + 8;
	enum libinput_key_state key_state =
		libinput_event_keyboard_get_key_state(kbevent);

	xkb_state_update_key(
		state->xkb_state, keycode,
		key_state == LIBINPUT_KEY_STATE_RELEASED ? XKB_KEY_UP : XKB_KEY_DOWN);

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, keycode);
	struct wsk_keypress *keypress = calloc(1, sizeof(struct wsk_keypress));
	assert(keypress);
	keypress->sym = keysym;
	xkb_keysym_get_name(keypress->sym, keypress->name, sizeof(keypress->name));
	if (xkb_state_key_get_utf8(state->xkb_state, keycode, keypress->utf8,
							   sizeof(keypress->utf8)) <= 0 ||
		keypress->utf8[0] <= ' ')
		keypress->utf8[0] = '\0';

	memset(state->current_combination_key, 0,
		   sizeof(state->current_combination_key));
	int special_key_num = 0;

	switch (key_state) {
	case LIBINPUT_KEY_STATE_RELEASED:
		// 松开修饰键时更新对应的 hold 标志
		if (strlen(keypress->name) > 2 &&
			strstr("Control_LControl_RAlt_LAlt_RSuper_LSuper_RShift_LShift_R"
				   "Meta_LMeta_R",
				   keypress->name)) {
			if (strcmp(keypress->name, "Control_L") == 0)
				state->ctrl_l_hold = 0;
			else if (strcmp(keypress->name, "Control_R") == 0)
				state->ctrl_r_hold = 0;
			else if (strcmp(keypress->name, "Alt_L") == 0 ||
					 strcmp(keypress->name, "Meta_L") == 0)
				state->alt_l_hold = 0;
			else if (strcmp(keypress->name, "Alt_R") == 0 ||
					 strcmp(keypress->name, "Meta_R") == 0)
				state->alt_r_hold = 0;
			else if (strcmp(keypress->name, "Super_L") == 0)
				state->super_l_hold = 0;
			else if (strcmp(keypress->name, "Super_R") == 0)
				state->supre_r_hold = 0;
			else if (strcmp(keypress->name, "Shift_L") == 0)
				state->shift_l_hold = 0;
			else if (strcmp(keypress->name, "Shift_R") == 0)
				state->shift_r_hold = 0;
		}
		break;
	case LIBINPUT_KEY_STATE_PRESSED:
		if (strlen(keypress->name) > 2 &&
			strstr("Control_LControl_RAlt_LAlt_RSuper_LSuper_RShift_LShift_R"
				   "Meta_LMeta_R",
				   keypress->name)) {
			// 按下修饰键
			if (strcmp(keypress->name, "Control_L") == 0)
				state->ctrl_l_hold = 1;
			else if (strcmp(keypress->name, "Control_R") == 0)
				state->ctrl_r_hold = 1;
			else if (strcmp(keypress->name, "Alt_L") == 0 ||
					 strcmp(keypress->name, "Meta_L") == 0)
				state->alt_l_hold = 1;
			else if (strcmp(keypress->name, "Alt_R") == 0 ||
					 strcmp(keypress->name, "Meta_R") == 0)
				state->alt_r_hold = 1;
			else if (strcmp(keypress->name, "Super_L") == 0)
				state->super_l_hold = 1;
			else if (strcmp(keypress->name, "Super_R") == 0)
				state->supre_r_hold = 1;
			else if (strcmp(keypress->name, "Shift_L") == 0)
				state->shift_l_hold = 1;
			else if (strcmp(keypress->name, "Shift_R") == 0)
				state->shift_r_hold = 1;
		} else {
			// 普通按键，前面可能挂着当前按住的修饰键
			struct wsk_keypress **link = &state->keys;
			while (*link)
				link = &(*link)->next;

			if (state->shift_l_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Shift_L");
				strcat(state->current_combination_key, "Shift_L");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->shift_r_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Shift_R");
				strcat(state->current_combination_key, "Shift_R");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->ctrl_l_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Control_L");
				strcat(state->current_combination_key, "Control_L");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->ctrl_r_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Control_R");
				strcat(state->current_combination_key, "Control_R");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->super_l_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Super_L");
				strcat(state->current_combination_key, "Super_L");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->supre_r_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Super_R");
				strcat(state->current_combination_key, "Super_R");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->alt_l_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Alt_L");
				strcat(state->current_combination_key, "Alt_L");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}
			if (state->alt_r_hold) {
				struct wsk_keypress *k = calloc(1, sizeof(struct wsk_keypress));
				strcpy(k->name, "Alt_R");
				strcat(state->current_combination_key, "Alt_R");
				special_key_num++;
				*link = k;
				link = &(*link)->next;
			}

			*link = keypress;
			strcat(state->current_combination_key, keypress->name);
			special_key_num++;

			// 检测是否是连续的同一组合键，如果是就显示重复次数
			if (strcmp(state->prev_combination_keye, "") != 0 &&
				strcmp(state->prev_combination_keye,
					   state->current_combination_key) == 0) {
				int del_charnum = caculat_del_charnum_of_int(
					state->combination_keye_repetition);
				if (state->combination_keye_repetition > 2)
					del_last_key(state, special_key_num + del_charnum);
				state->combination_keye_repetition++;
				if (state->combination_keye_repetition > 2) {
					int add_charnum = caculat_add_charnum_of_int(
						state->combination_keye_repetition);
					attach_repeat_flag(
						state, state->combination_keye_repetition, add_charnum);
				}
			} else {
				memset(state->prev_combination_keye, 0,
					   sizeof(state->prev_combination_keye));
				strcat(state->prev_combination_keye,
					   state->current_combination_key);
				state->combination_keye_repetition = 1;
			}
		}
		break;
	}

	clock_gettime(CLOCK_MONOTONIC, &state->last_key);
	set_dirty(state);
}

/* ---------- libinput 接口 ---------- */
static int libinput_open_restricted(const char *path, int flags, void *data) {
	int *fd = data;
	return devmgr_open(*fd, path);
}
static void libinput_close_restricted(int fd, void *data) { close(fd); }
static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted,
};

static uint32_t parse_color(const char *color) {
	if (color[0] == '#')
		++color;
	int len = strlen(color);
	if (len != 6 && len != 8) {
		fprintf(stderr, "Invalid color %s, defaulting to 0xFFFFFFFF\n", color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6)
		res = (res << 8) | 0xFF;
	return res;
}

int main(int argc, char *argv[]) {
	struct wsk_state state = {0};
	if (devmgr_start(&state.devmgr, &state.devmgr_pid, INPUTDEVPATH) > 0)
		return 1;

	int ret = 0;
	unsigned int anchor = 0;
	int margin = 32;
	state.background = 0x000000CC;
	state.specialfg = 0xAAAAAAFF;
	state.foreground = 0xFFFFFFFF;
	state.font = "Sans Bold 40";
	state.timeout = 200;
	state.length_limit = 100;
	state.combination_keye_repetition = 1;
	state.pending_clear = false;
	state.show_mods = false;
	state.show_mouse_buttons = false;
	state.show_scroll = false;
	state.scroll_up_active = 0;
	state.scroll_down_active = 0;

	int c;
	while ((c = getopt(argc, argv, "hb:f:s:F:t:a:m:o:l:MUS")) != -1) {
		switch (c) {
		case 'l':
			state.length_limit = atoi(optarg);
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
			if (strcmp(optarg, "top") == 0)
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
			else if (strcmp(optarg, "left") == 0)
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
			else if (strcmp(optarg, "right") == 0)
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			else if (strcmp(optarg, "bottom") == 0)
				anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
			break;
		case 'm':
			margin = atoi(optarg);
			break;
		case 'M':
			state.show_mods = true;
			break;
		case 'U':
			state.show_mouse_buttons = true;
			break;
		case 'S':
			state.show_scroll = true;
			break;
		case 'o':
			fprintf(stderr, "-o is unimplemented\n");
			return 0;
		default:
			fprintf(stderr,
					"usage: wshowkeys [-b|-f|-s #RRGGBB[AA]] [-F font] "
					"[-t timeout]\n\t[-a top|left|right|bottom] [-m margin] "
					"[-M] [-U] [-S] [-o output] [-l numOfLengthLimit]\n");
			return 1;
		}
	}

	// 鼠标或滚轮指示器自动打开修饰键面板
	if (state.show_mouse_buttons || state.show_scroll)
		state.show_mods = true;

	state.udev = udev_new();
	if (!state.udev) {
		ret = 1;
		goto exit;
	}
	state.libinput =
		libinput_udev_create_context(&libinput_impl, &state.devmgr, state.udev);
	udev_unref(state.udev);
	if (!state.libinput) {
		ret = 1;
		goto exit;
	}

	state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!state.xkb_context) {
		ret = 1;
		goto exit;
	}

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		ret = 1;
		goto exit;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	if (!state.compositor || !state.shm || !state.seat || !state.layer_shell) {
		fprintf(stderr, "Missing required Wayland interfaces\n");
		ret = 1;
		goto exit;
	}
	wl_seat_add_listener(state.seat, &wl_seat_listener, &state);
	wl_display_roundtrip(state.display);

	int scale = state.output ? state.output->scale : 1;

	uint32_t mods_surf_h = 0;
	if (state.show_mods) {
		uint32_t mods_pixel_w, mods_pixel_h;
		calc_mods_size(&state, scale, &mods_pixel_w, &mods_pixel_h);
		uint32_t mods_surf_w = mods_pixel_w / scale;
		mods_surf_h = mods_pixel_h / scale;
		if (mods_surf_w < 1)
			mods_surf_w = 1;
		if (mods_surf_h < 1)
			mods_surf_h = 1;

		state.mods_surface = wl_compositor_create_surface(state.compositor);
		assert(state.mods_surface);
		wl_surface_add_listener(state.mods_surface, &wl_surface_listener,
								&state);
		state.mods_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state.layer_shell, state.mods_surface, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "showkeys-mods");
		assert(state.mods_layer_surface);
		struct wl_region *mods_input =
			wl_compositor_create_region(state.compositor);
		wl_surface_set_input_region(state.mods_surface, mods_input);
		wl_region_destroy(mods_input);
		zwlr_layer_surface_v1_add_listener(
			state.mods_layer_surface, &mods_layer_surface_listener, &state);
		zwlr_layer_surface_v1_set_anchor(state.mods_layer_surface, anchor);
		zwlr_layer_surface_v1_set_margin(state.mods_layer_surface, margin,
										 margin, margin, margin);
		zwlr_layer_surface_v1_set_exclusive_zone(state.mods_layer_surface, -1);
		zwlr_layer_surface_v1_set_size(state.mods_layer_surface, mods_surf_w,
									   mods_surf_h);
		wl_surface_commit(state.mods_surface);
	}

	const int keys_gap = 4;
	state.keys_surface = wl_compositor_create_surface(state.compositor);
	assert(state.keys_surface);
	wl_surface_add_listener(state.keys_surface, &wl_surface_listener, &state);
	state.keys_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		state.layer_shell, state.keys_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "showkeys-keys");
	assert(state.keys_layer_surface);
	struct wl_region *keys_input =
		wl_compositor_create_region(state.compositor);
	wl_surface_set_input_region(state.keys_surface, keys_input);
	wl_region_destroy(keys_input);
	zwlr_layer_surface_v1_add_listener(state.keys_layer_surface,
									   &keys_layer_surface_listener, &state);
	zwlr_layer_surface_v1_set_anchor(state.keys_layer_surface, anchor);
	zwlr_layer_surface_v1_set_margin(state.keys_layer_surface, margin, margin,
									 margin + mods_surf_h + keys_gap, margin);
	zwlr_layer_surface_v1_set_exclusive_zone(state.keys_layer_surface, -1);
	zwlr_layer_surface_v1_set_size(state.keys_layer_surface, 1, 1);
	wl_surface_commit(state.keys_surface);

	wl_display_roundtrip(state.display);

	struct pollfd pollfds[] = {
		{.fd = libinput_get_fd(state.libinput), .events = POLLIN},
		{.fd = wl_display_get_fd(state.display), .events = POLLIN},
	};

	state.run = true;
	while (state.run) {
		errno = 0;
		do {
			if (wl_display_flush(state.display) == -1 && errno != EAGAIN)
				break;
		} while (errno == EAGAIN);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		// 计算 poll 超时：取按键超时和滚轮超时中较小的那个
		int poll_timeout = -1;
		if (state.keys) {
			long elapsed_ms = (now.tv_sec - state.last_key.tv_sec) * 1000 +
							  (now.tv_nsec - state.last_key.tv_nsec) / 1000000;
			if (elapsed_ms >= state.timeout) {
				if (!state.pending_clear) {
					state.pending_clear = true;
				}
				poll_timeout = -1;
			} else {
				poll_timeout = state.timeout - (int)elapsed_ms;
				if (poll_timeout < 1)
					poll_timeout = 1;
			}
		}

		if (state.show_scroll &&
			(state.scroll_up_active || state.scroll_down_active)) {
			long elapsed_ms =
				(now.tv_sec - state.last_scroll.tv_sec) * 1000 +
				(now.tv_nsec - state.last_scroll.tv_nsec) / 1000000;
			if (elapsed_ms >= state.timeout) {
				state.scroll_up_active = 0;
				state.scroll_down_active = 0;
				set_dirty(&state);
			} else {
				int remaining = state.timeout - (int)elapsed_ms;
				if (poll_timeout < 0 || remaining < poll_timeout)
					poll_timeout = remaining;
				if (poll_timeout < 1)
					poll_timeout = 1;
			}
		}

		if (poll(pollfds, 2, poll_timeout) < 0)
			break;
		clock_gettime(CLOCK_MONOTONIC, &now);

		// 如果按键太长，从头部开始丢弃
		if (state.keys) {
			int all_key_len = 0;
			char *temp_name = calloc(1, 129);
			struct wsk_keypress *key = state.keys;
			while (key) {
				strcpy(temp_name, key->name);
				custome_key_name(temp_name);
				all_key_len += get_char_width(temp_name);
				key = key->next;
			}
			free(temp_name);
			if (all_key_len > state.length_limit) {
				struct wsk_keypress *next = state.keys->next;
				free(state.keys);
				state.keys = next;
				set_dirty(&state);
			}
		}

		if (pollfds[0].revents & POLLIN) {
			if (libinput_dispatch(state.libinput) != 0)
				break;
			struct libinput_event *event;
			while ((event = libinput_get_event(state.libinput))) {
				handle_libinput_event(&state, event);
				libinput_event_destroy(event);
			}
		}
		if (pollfds[1].revents & POLLIN) {
			if (wl_display_dispatch(state.display) == -1)
				break;
		}
	}

exit:
	if (state.mods_layer_surface)
		zwlr_layer_surface_v1_destroy(state.mods_layer_surface);
	if (state.keys_layer_surface)
		zwlr_layer_surface_v1_destroy(state.keys_layer_surface);
	if (state.mods_surface)
		wl_surface_destroy(state.mods_surface);
	if (state.keys_surface)
		wl_surface_destroy(state.keys_surface);
	wl_display_disconnect(state.display);
	libinput_unref(state.libinput);
	devmgr_finish(state.devmgr, state.devmgr_pid);
	return ret;
}