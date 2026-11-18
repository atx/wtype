
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wchar.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))


enum wtype_command_type {
	WTYPE_COMMAND_TEXT = 0,
	WTYPE_COMMAND_MOD_PRESS = 1,
	WTYPE_COMMAND_MOD_RELEASE = 2,
	WTYPE_COMMAND_KEY_PRESS = 3,
	WTYPE_COMMAND_KEY_RELEASE = 4,
	WTYPE_COMMAND_SLEEP = 5,
	WTYPE_COMMAND_TEXT_STDIN = 6
};


enum wtype_mod {
	WTYPE_MOD_NONE = 0,
	WTYPE_MOD_SHIFT = 1,
	WTYPE_MOD_CAPSLOCK = 2,
	WTYPE_MOD_CTRL = 4,
	WTYPE_MOD_ALT = 8,
	WTYPE_MOD_MOD2 = 16,
	WTYPE_MOD_MOD3 = 32,
	WTYPE_MOD_LOGO = 64,
	WTYPE_MOD_ALTGR = 128
};


struct wtype_command {
	enum wtype_command_type type;
	union {
		struct {
			unsigned int *key_codes;
			size_t key_codes_len;
			unsigned int delay_ms;
		};
		unsigned int single_key_code;
		enum wtype_mod mod;
		unsigned int sleep_ms;
	};
};


struct keymap_entry {
	xkb_keysym_t xkb;
	wchar_t wchr;
};

// Linux evdev codes for the printable section of a PC keyboard. Chromium may
// fall back to a key's physical meaning even when its keysym is printable.
// Never put literal text on Escape, Backspace, Tab, Enter, or modifier codes.
static const unsigned int text_keycodes[] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
	30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
	43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 57,
};


struct wtype {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_keyboard *input_keyboard;
	struct zwp_virtual_keyboard_manager_v1 *manager;
	struct zwp_virtual_keyboard_v1 *keyboard;

	// The seat's current keymap, when the compositor provides it. Uploading a
	// copy of it (with our own keys added) keeps the user's layout intact.
	char *base_keymap;
	struct xkb_keymap *base_xkb_keymap;
	xkb_keycode_t base_max_keycode;
	xkb_layout_index_t base_num_layouts;
	bool using_base_keymap;

	// Stores a one-indexed logical code -> (xkb_keysym_t, wchar_t) mapping.
	// Explicit key commands use these codes directly. Literal-only invocations
	// translate them to printable physical codes when uploading/typing a batch.
	// Beware that the wchar_t does not have to be valid (and is 0 in such cases)
	// This is since some keysyms need not have a unicode representation
	// (such as the arrow keys and similar)
	size_t keymap_len;
	struct keymap_entry *keymap;
	bool literal_only;
	size_t keymap_offset;

	uint32_t mod_status;
	size_t command_count;
	struct wtype_command *commands;
};


void fail(const char *format, ...)
{
	va_list vas;
	va_start(vas, format);
	vfprintf(stderr, format, vas);
	va_end(vas);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

struct wtype_buffer {
	char *data;
	size_t len;
	size_t capacity;
};


static void buffer_reserve(struct wtype_buffer *buffer, size_t extra)
{
	if (buffer->len + extra <= buffer->capacity) {
		return;
	}
	size_t capacity = buffer->capacity ? buffer->capacity : 4096;
	while (capacity < buffer->len + extra) {
		capacity *= 2;
	}
	buffer->data = realloc(buffer->data, capacity);
	if (buffer->data == NULL) {
		fail("Out of memory while building the keymap");
	}
	buffer->capacity = capacity;
}

static void buffer_append_data(struct wtype_buffer *buffer, const char *data,
							   size_t len)
{
	buffer_reserve(buffer, len);
	memcpy(buffer->data + buffer->len, data, len);
	buffer->len += len;
}

static void buffer_append(struct wtype_buffer *buffer, const char *string)
{
	buffer_append_data(buffer, string, strlen(string));
}

static void buffer_appendf(struct wtype_buffer *buffer, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	va_list args_copy;
	va_copy(args_copy, args);
	int needed = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (needed < 0) {
		fail("Failed to format the keymap");
	}
	buffer_reserve(buffer, (size_t)needed + 1);
	vsnprintf(buffer->data + buffer->len, (size_t)needed + 1, format, args_copy);
	va_end(args_copy);
	buffer->len += (size_t)needed;
}


static void set_base_keymap(struct wtype *wtype, const char *keymap_data)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context == NULL) {
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
		context, keymap_data, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS
	);
	if (keymap == NULL) {
		xkb_context_unref(context);
		return;
	}
	char *keymap_string = xkb_keymap_get_as_string(
		keymap, XKB_KEYMAP_FORMAT_TEXT_V1
	);
	if (keymap_string == NULL) {
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
		return;
	}

	free(wtype->base_keymap);
	if (wtype->base_xkb_keymap != NULL) {
		xkb_keymap_unref(wtype->base_xkb_keymap);
	}
	wtype->base_keymap = keymap_string;
	wtype->base_xkb_keymap = keymap;
	wtype->base_max_keycode = xkb_keymap_max_keycode(keymap);
	wtype->base_num_layouts = xkb_keymap_num_layouts(keymap);

	xkb_context_unref(context);
}


static void handle_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
								   uint32_t format, int32_t fd, uint32_t size)
{
	struct wtype *wtype = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	char *keymap_data = malloc((size_t)size + 1);
	if (keymap_data == NULL) {
		close(fd);
		return;
	}
	size_t length = 0;
	while (length < size) {
		ssize_t ret = read(fd, keymap_data + length, size - length);
		if (ret <= 0) {
			break;
		}
		length += (size_t)ret;
	}
	close(fd);
	keymap_data[length] = '\0';

	set_base_keymap(wtype, keymap_data);
	free(keymap_data);
}

static void handle_keyboard_enter(void *data, struct wl_keyboard *keyboard,
								  uint32_t serial, struct wl_surface *surface,
								  struct wl_array *keys)
{
}

static void handle_keyboard_leave(void *data, struct wl_keyboard *keyboard,
								  uint32_t serial, struct wl_surface *surface)
{
}

static void handle_keyboard_key(void *data, struct wl_keyboard *keyboard,
								uint32_t serial, uint32_t time, uint32_t key,
								uint32_t state)
{
}

static void handle_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
									  uint32_t serial, uint32_t mods_depressed,
									  uint32_t mods_latched, uint32_t mods_locked,
									  uint32_t group)
{
}

static void handle_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
										int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = handle_keyboard_keymap,
	.enter = handle_keyboard_enter,
	.leave = handle_keyboard_leave,
	.key = handle_keyboard_key,
	.modifiers = handle_keyboard_modifiers,
	.repeat_info = handle_keyboard_repeat_info,
};


static void handle_seat_capabilities(void *data, struct wl_seat *seat,
									 uint32_t capabilities)
{
	struct wtype *wtype = data;
	if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) ||
		wtype->input_keyboard != NULL) {
		return;
	}
	wtype->input_keyboard = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(
		wtype->input_keyboard, &keyboard_listener, wtype
	);
}

static void handle_seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = handle_seat_capabilities,
	.name = handle_seat_name,
};


static void upload_keymap(struct wtype *wtype);


static void handle_wl_event(void *data, struct wl_registry *registry,
							uint32_t name, const char *interface,
							uint32_t version)
{
	struct wtype *wtype = data;
	if (!strcmp(interface, wl_seat_interface.name)) {
		wtype->seat = wl_registry_bind(
			registry, name, &wl_seat_interface, version <= 7 ? version : 7
		);
		wl_seat_add_listener(wtype->seat, &seat_listener, wtype);
	} else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		wtype->manager = wl_registry_bind(
			registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1
		);
	}
}

static void handle_wl_event_remove(void *data, struct wl_registry *registry,
								   uint32_t name)
{
}


static const struct wl_registry_listener registry_listener = {
	.global = handle_wl_event,
	.global_remove = handle_wl_event_remove,
};


static const struct { const char *name; enum wtype_mod mod; } mod_names[] = {
	{"shift", WTYPE_MOD_SHIFT},
	{"capslock", WTYPE_MOD_CAPSLOCK},
	{"ctrl", WTYPE_MOD_CTRL},
	{"logo", WTYPE_MOD_LOGO},
	{"win", WTYPE_MOD_LOGO},
	{"alt", WTYPE_MOD_ALT},
	{"altgr", WTYPE_MOD_ALTGR},
	{"mod1", WTYPE_MOD_ALT},
	{"mod2", WTYPE_MOD_MOD2},
	{"mod3", WTYPE_MOD_MOD3},
	{"mod4", WTYPE_MOD_ALTGR},
};


enum wtype_mod name_to_mod(const char *name)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(mod_names); i++) {
		if (!strcasecmp(mod_names[i].name, name)) {
			return mod_names[i].mod;
		}
	}
	return WTYPE_MOD_NONE;
}


static unsigned int append_keymap_entry(struct wtype *wtype, wchar_t ch, xkb_keysym_t xkb)
{
	wtype->keymap = realloc(
		wtype->keymap, ++wtype->keymap_len * sizeof(wtype->keymap[0])
	);
	wtype->keymap[wtype->keymap_len - 1].wchr = ch;
	wtype->keymap[wtype->keymap_len - 1].xkb = xkb;
	return wtype->keymap_len;
}


unsigned int get_key_code_by_wchar(struct wtype *wtype, wchar_t ch)
{
	const struct {
		wchar_t from;
		xkb_keysym_t to;
	} remap_table[] = {
		{ L'\n', XKB_KEY_Return },
		{ L'\t', XKB_KEY_Tab },
		{ L'\e', XKB_KEY_Escape },
	};
	for (unsigned int i = 0; i < wtype->keymap_len; i++) {
		if (wtype->keymap[i].wchr == ch) {
			return i + 1;
		}
	}

	// TODO: Maybe warn if this actually ends up being XKB_KEY_NoSymbol or something?
	xkb_keysym_t xkb = xkb_utf32_to_keysym(ch);
	for (size_t i = 0; i < ARRAY_SIZE(remap_table); i++) {
		if (remap_table[i].from == ch) {
			// This overwrites whatever xkb gave us before.
			xkb = remap_table[i].to;
			break;
		}
	}

	return append_keymap_entry(wtype, ch, xkb);
}

unsigned int get_key_code_by_xkb(struct wtype *wtype, xkb_keysym_t xkb)
{
	for (unsigned int i = 0; i < wtype->keymap_len; i++) {
		if (wtype->keymap[i].xkb == xkb) {
			return i + 1;
		}
	}

	return append_keymap_entry(wtype, 0, xkb);
}


static void parse_args(struct wtype *wtype, int argc, const char *argv[])
{
	wtype->commands = calloc(argc, sizeof(wtype->commands[0])); // Upper bound
	wtype->command_count = 0;
	bool raw_text = false;
	bool prefix_with_space = false;
	bool use_stdin = false;
	unsigned int delay_ms = 0;
	wtype->literal_only = true;
	for (int i = 1; i < argc; i++) {
		struct wtype_command *cmd = &wtype->commands[wtype->command_count];
		if (!raw_text && !strcmp("--", argv[i])) {
			raw_text = true;
		} else if (!raw_text && !strcmp("-", argv[i])) {
			// Output text from stdin
			if (use_stdin) {
				fail("Stdin place-holder can only appear once");
			}
			use_stdin = true;
			cmd->type = WTYPE_COMMAND_TEXT_STDIN;
			cmd->delay_ms = delay_ms;
			wtype->command_count++;
		} else if (!raw_text && argv[i][0] == '-'){
			if (i == argc - 1) {
				fail("Missing argument to %s", argv[i]);
			}
			if (!strcmp("-M", argv[i])) {
				wtype->literal_only = false;
				// Press modifier
				cmd->type = WTYPE_COMMAND_MOD_PRESS;
				cmd->mod = name_to_mod(argv[i + 1]);
				if (cmd->mod == WTYPE_MOD_NONE) {
					fail("Invalid modifier name '%s'", argv[i + 1]);
				}
			} else if (!strcmp("-m", argv[i])) {
				wtype->literal_only = false;
				// Release modifier
				cmd->type = WTYPE_COMMAND_MOD_RELEASE;
				cmd->mod = name_to_mod(argv[i + 1]);
				if (cmd->mod == WTYPE_MOD_NONE) {
					fail("Invalid modifier name '%s'", argv[i + 1]);
				}
			} else if (!strcmp("-s", argv[i])) {
				// Sleep
				cmd->type = WTYPE_COMMAND_SLEEP;
				cmd->sleep_ms = atoi(argv[i + 1]);
				if (cmd->sleep_ms <= 0) {
					fail("Invalid sleep time");
				}
			} else if (!strcmp("-d", argv[i])) {
				// Set delay between type keystrokes
				delay_ms = atoi(argv[i + 1]);
				if (delay_ms <= 0) {
					fail("Invalid sleep time");
				}
			} else if (!strcmp("-k", argv[i])) {
				wtype->literal_only = false;
				//size_t k;
				xkb_keysym_t ks = xkb_keysym_from_name(argv[i + 1], XKB_KEYSYM_CASE_INSENSITIVE);
				if (ks == XKB_KEY_NoSymbol) {
					fail("Unknown key '%s'", argv[i + 1]);
				}
				cmd->type = WTYPE_COMMAND_TEXT;
				cmd->key_codes = malloc(sizeof(cmd->key_codes[0]));
				cmd->key_codes_len = 1;
				cmd->key_codes[0] = get_key_code_by_xkb(wtype, ks);
				cmd->delay_ms = delay_ms;
			} else if (!strcmp("-P", argv[i]) || !strcmp("-p", argv[i])) {
				wtype->literal_only = false;
				// Press/release a key
				xkb_keysym_t ks = xkb_keysym_from_name(argv[i + 1], XKB_KEYSYM_CASE_INSENSITIVE);
				if (ks == XKB_KEY_NoSymbol) {
					fail("Unknown key '%s'", argv[i + 1]);
				}
				cmd->type = argv[i][1] == 'P' ? WTYPE_COMMAND_KEY_PRESS : WTYPE_COMMAND_KEY_RELEASE;
				cmd->single_key_code = get_key_code_by_xkb(wtype, ks);
			} else {
				fail("Unknown parameter %s", argv[i]);
			}
			prefix_with_space = false;
			i++;
			wtype->command_count++;
		} else {
			// Text
			cmd->type = WTYPE_COMMAND_TEXT;
			cmd->delay_ms = delay_ms;
			wtype->command_count++;

			size_t raw_len = strlen(argv[i]) + 2; // NULL byte and the potential space
			wchar_t text[raw_len];  // Upper bound on size
			memset(text, 0, sizeof(text));
			setlocale(LC_CTYPE, "");
			ssize_t ret = mbstowcs(
				prefix_with_space ? &text[1] : text,
				argv[i],
				prefix_with_space ? ARRAY_SIZE(text) - 1 : ARRAY_SIZE(text)
			);
			if (ret < 0) {
				fail("Failed to deencode input argv");
			}
			if (prefix_with_space) {
				text[0] = L' ';
				ret++;
			}
			cmd->key_codes = calloc(ret, sizeof(cmd->key_codes[0]));
			cmd->key_codes_len = ret;
			for (ssize_t k = 0; k < ret; k++) {
				cmd->key_codes[k] = get_key_code_by_wchar(wtype, text[k]);
			}
			prefix_with_space = true;
		}
	}
}


static void run_sleep(struct wtype *wtype, struct wtype_command *cmd)
{
	usleep(cmd->sleep_ms * 1000);
}


static void run_mod(struct wtype *wtype, struct wtype_command *cmd)
{
	if (cmd->type == WTYPE_COMMAND_MOD_PRESS) {
		wtype->mod_status |= cmd->mod;
	} else {
		wtype->mod_status &= ~cmd->mod;
	}

	zwp_virtual_keyboard_v1_modifiers(
		wtype->keyboard, wtype->mod_status & ~WTYPE_MOD_CAPSLOCK, 0,
		wtype->mod_status & WTYPE_MOD_CAPSLOCK, 0
	);

	wl_display_roundtrip(wtype->display);
}


static unsigned int explicit_key_code(struct wtype *wtype,
									  unsigned int key_code)
{
	if (!wtype->using_base_keymap) {
		return key_code;
	}
	// Custom keys are appended after the compositor's highest keycode. The
	// virtual keyboard protocol uses evdev codes, which are XKB keycodes - 8.
	return wtype->base_max_keycode + key_code - 8;
}

static void run_key(struct wtype *wtype, struct wtype_command *cmd)
{
	zwp_virtual_keyboard_v1_key(
		wtype->keyboard, 0, explicit_key_code(wtype, cmd->single_key_code),
		cmd->type == WTYPE_COMMAND_KEY_PRESS ?
			WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
	);
	wl_display_roundtrip(wtype->display);
}

static void type_keycode(struct wtype *wtype, unsigned int key_code)
{
	if (wtype->literal_only) {
		// Reuse printable physical codes in bounded keymaps. A map changes only
		// between complete press/release pairs, never while a key is held.
		size_t offset = ((key_code - 1) / ARRAY_SIZE(text_keycodes))
			* ARRAY_SIZE(text_keycodes);
		if (offset != wtype->keymap_offset) {
			wtype->keymap_offset = offset;
			upload_keymap(wtype);
		}
		key_code = text_keycodes[(key_code - 1) % ARRAY_SIZE(text_keycodes)];
	} else {
		key_code = explicit_key_code(wtype, key_code);
	}

	zwp_virtual_keyboard_v1_key(
			wtype->keyboard, 0, key_code, WL_KEYBOARD_KEY_STATE_PRESSED
	);
	wl_display_roundtrip(wtype->display);
	usleep(2000);
	zwp_virtual_keyboard_v1_key(
		wtype->keyboard, 0, key_code, WL_KEYBOARD_KEY_STATE_RELEASED
	);
	wl_display_roundtrip(wtype->display);
	usleep(2000);
}

static void run_text(struct wtype *wtype, struct wtype_command *cmd)
{
	for (size_t i = 0; i < cmd->key_codes_len; i++) {
		type_keycode(wtype, cmd->key_codes[i]);
		usleep(cmd->delay_ms * 1000);
	}
}

static void run_text_stdin(struct wtype *wtype, struct wtype_command *cmd)
{
	char kbuf[8];
	size_t k = 0;
	wchar_t text_char;
	const size_t buf_size = 100;

	cmd->key_codes = calloc(buf_size, sizeof(cmd->key_codes[0]));
	cmd->key_codes_len = 0;

	setlocale(LC_CTYPE, "");
	while (1) {
		if ((kbuf[k] = fgetc(stdin)) == EOF) {
			break;
		}

		kbuf[k+1] = '\0';
		size_t ret = mbstowcs(&text_char, kbuf, 1);
		if (ret == -1) {
			k++;
			if (k >= ARRAY_SIZE(kbuf) - 1) {
				break;
			}
			continue;
		} else if (ret == 0) {
			k = 0;
			continue;
		}
		k = 0;

		unsigned int key_code = get_key_code_by_wchar(wtype, text_char);
		cmd->key_codes[cmd->key_codes_len++] = key_code;

		if (cmd->key_codes_len == buf_size) {
			upload_keymap(wtype);
			for (size_t i = 0; i < cmd->key_codes_len; i++) {
				type_keycode(wtype, cmd->key_codes[i]);
				usleep(cmd->delay_ms * 1000);
			}
			cmd->key_codes_len = 0;
		}
	}

	if (cmd->key_codes_len != 0) {
		upload_keymap(wtype);
		for (size_t i = 0; i < cmd->key_codes_len; i++) {
			type_keycode(wtype, cmd->key_codes[i]);
			usleep(cmd->delay_ms * 1000);
		}
	}

	free(cmd->key_codes);
}

static void run_commands(struct wtype *wtype)
{
	void (*handlers[])(struct wtype *, struct wtype_command *) = {
		[WTYPE_COMMAND_SLEEP] = run_sleep,
		[WTYPE_COMMAND_MOD_PRESS] = run_mod,
		[WTYPE_COMMAND_MOD_RELEASE] = run_mod,
		[WTYPE_COMMAND_KEY_PRESS] = run_key,
		[WTYPE_COMMAND_KEY_RELEASE] = run_key,
		[WTYPE_COMMAND_TEXT] = run_text,
		[WTYPE_COMMAND_TEXT_STDIN] = run_text_stdin,
	};
	for (unsigned int i = 0; i < wtype->command_count; i++) {
		handlers[wtype->commands[i].type](wtype, &wtype->commands[i]);
	}
}


static size_t find_section_end(const char *keymap, const char *section)
{
	const char *start = strstr(keymap, section);
	if (start == NULL) {
		return SIZE_MAX;
	}
	const char *open = strchr(start, '{');
	if (open == NULL) {
		return SIZE_MAX;
	}

	int depth = 0;
	bool in_string = false;
	for (const char *p = open; *p != '\0'; p++) {
		if (*p == '"' && (p == open || p[-1] != '\\')) {
			in_string = !in_string;
		} else if (!in_string) {
			if (*p == '{') {
				depth++;
			} else if (*p == '}') {
				depth--;
				if (depth == 0) {
					return (size_t)(p - keymap);
				}
			}
		}
	}
	return SIZE_MAX;
}

static bool find_number(const char *keymap, size_t section_end,
						const char *name, size_t *start, size_t *end)
{
	const char *p = strstr(keymap, name);
	if (p == NULL || (size_t)(p - keymap) >= section_end) {
		return false;
	}
	p += strlen(name);
	while (*p != '\0' && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != '=') {
		return false;
	}
	p++;
	while (*p != '\0' && isspace((unsigned char)*p)) {
		p++;
	}
	if (!isdigit((unsigned char)*p)) {
		return false;
	}
	*start = (size_t)(p - keymap);
	while (*p != '\0' && isdigit((unsigned char)*p)) {
		p++;
	}
	*end = (size_t)(p - keymap);
	return true;
}

static void append_keysym_name(struct wtype_buffer *buffer, xkb_keysym_t keysym)
{
	char sym_name[256];

	int ret = xkb_keysym_get_name(keysym, sym_name, sizeof(sym_name));
	if (ret <= 0) {
		fail("Unable to get XKB symbol name for keysym %04x", keysym);
	}
	buffer_append(buffer, sym_name);
}

static void custom_key_name(struct wtype *wtype, size_t index, char *name,
							size_t name_size)
{
	static const char *prefixes[] = {"WType", "WType_", "WTypeX"};
	for (size_t i = 0; i < ARRAY_SIZE(prefixes); i++) {
		snprintf(name, name_size, "%s%zu", prefixes[i], index);
		if (xkb_keymap_key_by_name(wtype->base_xkb_keymap, name) ==
			XKB_KEYCODE_INVALID) {
			return;
		}
	}
	snprintf(name, name_size, "WType%zu_%ld", index, (long)getpid());
}

static void append_key_definition(struct wtype_buffer *buffer, const char *name,
								  xkb_keysym_t keysym,
								  xkb_layout_index_t layouts)
{
	buffer_appendf(buffer, "key <%s> { [", name);
	for (xkb_layout_index_t i = 0; i < layouts; i++) {
		if (i != 0) {
			buffer_append(buffer, "], [");
		}
		append_keysym_name(buffer, keysym);
	}
	buffer_append(buffer, "] };\n");
}

static char *build_base_keymap(struct wtype *wtype, size_t first, size_t end,
							   size_t *keymap_size)
{
	const char *base = wtype->base_keymap;
	size_t base_len = strlen(base);
	size_t keycodes_end = find_section_end(base, "xkb_keycodes");
	size_t symbols_end = find_section_end(base, "xkb_symbols");
	if (keycodes_end == SIZE_MAX || symbols_end == SIZE_MAX) {
		return NULL;
	}

	bool explicit_keys = !wtype->literal_only;
	size_t maximum_start = 0;
	size_t maximum_end = 0;
	if (explicit_keys && !find_number(
			base, keycodes_end, "maximum", &maximum_start, &maximum_end)) {
		return NULL;
	}

	struct wtype_buffer buffer = {0};
	size_t cursor = 0;
	if (explicit_keys) {
		buffer_append_data(&buffer, base + cursor, maximum_start - cursor);
		buffer_appendf(
			&buffer, "%u", (unsigned int)(wtype->base_max_keycode + end)
		);
		cursor = maximum_end;
	}
	buffer_append_data(&buffer, base + cursor, keycodes_end - cursor);

	if (explicit_keys) {
		for (size_t i = 0; i < end; i++) {
			char name[64];
			custom_key_name(wtype, i, name, sizeof(name));
			buffer_appendf(
				&buffer, "\t<%s> = %u;\n", name,
				(unsigned int)(wtype->base_max_keycode + 1 + i)
			);
		}
	}

	cursor = keycodes_end;
	buffer_append_data(&buffer, base + cursor, symbols_end - cursor);

	xkb_layout_index_t layouts = wtype->base_num_layouts
		? wtype->base_num_layouts : 1;
	if (explicit_keys) {
		for (size_t i = 0; i < end; i++) {
			char name[64];
			custom_key_name(wtype, i, name, sizeof(name));
			append_key_definition(&buffer, name, wtype->keymap[i].xkb, layouts);
		}
	} else {
		for (size_t i = first; i < end; i++) {
			xkb_keycode_t keycode = text_keycodes[i - first] + 8;
			const char *name = xkb_keymap_key_get_name(
				wtype->base_xkb_keymap, keycode
			);
			if (name == NULL) {
				free(buffer.data);
				return NULL;
			}
			append_key_definition(
				&buffer, name, wtype->keymap[i].xkb, layouts
			);
		}
	}

	cursor = symbols_end;
	buffer_append_data(&buffer, base + cursor, base_len - cursor);
	buffer_append_data(&buffer, "", 1);

	*keymap_size = buffer.len;
	return buffer.data;
}

static char *build_standalone_keymap(struct wtype *wtype, size_t first,
									 size_t end, size_t *keymap_size)
{
	// Explicit key/modifier commands keep their original mapping and lifetime.
	size_t maximum = wtype->literal_only
		? text_keycodes[ARRAY_SIZE(text_keycodes) - 1] + 8
		: wtype->keymap_len + 9;

	struct wtype_buffer buffer = {0};
	buffer_append(&buffer, "xkb_keymap {\n");
	buffer_appendf(
		&buffer,
		"xkb_keycodes \"(unnamed)\" {\n"
		"minimum = 8;\n"
		"maximum = %zu;\n",
		maximum
	);
	for (size_t i = first; i < end; i++) {
		size_t code = wtype->literal_only ? text_keycodes[i - first] : i + 1;
		buffer_appendf(&buffer, "<K%zu> = %zu;\n", i + 1, code + 8);
	}
	buffer_append(&buffer, "};\n");

	// TODO: Is including "complete" here really a good idea?
	buffer_append(&buffer, "xkb_types \"(unnamed)\" { include \"complete\" };\n");
	buffer_append(&buffer, "xkb_compatibility \"(unnamed)\" { include \"complete\" };\n");

	buffer_append(&buffer, "xkb_symbols \"(unnamed)\" {\n");
	for (size_t i = first; i < end; i++) {
		buffer_appendf(&buffer, "key <K%zu> {[", i + 1);
		append_keysym_name(&buffer, wtype->keymap[i].xkb);
		buffer_append(&buffer, "]};\n");
	}
	buffer_append(&buffer, "};\n");
	buffer_append(&buffer, "};\n");
	buffer_append_data(&buffer, "", 1);

	*keymap_size = buffer.len;
	return buffer.data;
}


static void upload_keymap(struct wtype *wtype)
{
	size_t first = wtype->literal_only ? wtype->keymap_offset : 0;
	size_t end = wtype->keymap_len;
	if (wtype->literal_only && end > first + ARRAY_SIZE(text_keycodes)) {
		end = first + ARRAY_SIZE(text_keycodes);
	}

	size_t keymap_size = 0;
	char *keymap = NULL;
	wtype->using_base_keymap = false;
	if (wtype->base_keymap != NULL) {
		keymap = build_base_keymap(wtype, first, end, &keymap_size);
		if (keymap != NULL) {
			wtype->using_base_keymap = true;
		}
	}
	if (keymap == NULL) {
		keymap = build_standalone_keymap(wtype, first, end, &keymap_size);
	}

	char filename[] = "/tmp/wtype-XXXXXX";
	int fd = mkstemp(filename);
	if (fd < 0) {
		fail("Failed to create the temporary keymap file");
	}
	unlink(filename);
	FILE *f = fdopen(fd, "w");
	if (f == NULL) {
		fail("Failed to open the temporary keymap file");
	}

	if (fwrite(keymap, 1, keymap_size, f) != keymap_size) {
		fail("Failed to write the temporary keymap file");
	}
	fflush(f);

	zwp_virtual_keyboard_v1_keymap(
		wtype->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(f), keymap_size
	);

	wl_display_roundtrip(wtype->display);

	fclose(f);
	free(keymap);
}


int main(int argc, const char *argv[])
{
	if (argc < 2) {
		fail("Usage: %s <text-to-type>", argv[0]);
	}

	struct wtype wtype;
	memset(&wtype, 0, sizeof(wtype));

	parse_args(&wtype, argc, argv);

	wtype.display = wl_display_connect(NULL);
	if (wtype.display == NULL) {
		fail("Wayland connection failed");
	}
	wtype.registry = wl_display_get_registry(wtype.display);
	wl_registry_add_listener(wtype.registry, &registry_listener, &wtype);
	wl_display_dispatch(wtype.display);
	wl_display_roundtrip(wtype.display);

	if (wtype.input_keyboard != NULL && wtype.base_keymap == NULL) {
		// The keyboard capability may have arrived during the roundtrip above,
		// so give the compositor another chance to send its keymap.
		wl_display_roundtrip(wtype.display);
	}

	if (wtype.manager == NULL) {
		fail("Compositor does not support the virtual keyboard protocol");
	}
	if (wtype.seat == NULL) {
		fail("No seat found");
	}

	wtype.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		wtype.manager, wtype.seat
	);

	upload_keymap(&wtype);
	run_commands(&wtype);

	for (unsigned int i = 0; i < wtype.command_count; i++) {
		if (wtype.commands[i].type == WTYPE_COMMAND_TEXT) {
			free(wtype.commands[i].key_codes);
		}
	}
	free(wtype.commands);
	free(wtype.keymap);
	if (wtype.input_keyboard != NULL) {
		wl_keyboard_destroy(wtype.input_keyboard);
	}
	if (wtype.base_xkb_keymap != NULL) {
		xkb_keymap_unref(wtype.base_xkb_keymap);
	}
	free(wtype.base_keymap);
	zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
	wl_registry_destroy(wtype.registry);
	wl_display_disconnect(wtype.display);
}
