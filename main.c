
#include <fcntl.h>
#include <locale.h>
#include <stdbool.h>
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


struct wtype {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwp_virtual_keyboard_manager_v1 *manager;
	struct zwp_virtual_keyboard_v1 *keyboard;

	// Stores a keycode -> (xkb_keysym_t, wchar_t) mapping
	// Beware that this is one-indexed (as zero-keycodes are sometimes
	// not handled well by clients).
	// That is, keymap[0] is a (xkb_keysym_t, wchar_t) pair which we can type by sending
	// the keycode 1.
	// Beware that the wchar_t does not have to be valid (and is 0 in such cases)
	// This is since some keysyms need not have a unicode representation
	// (such as the arrow keys and similar)
	size_t keymap_len;
	struct keymap_entry *keymap;

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
				// Press modifier
				cmd->type = WTYPE_COMMAND_MOD_PRESS;
				cmd->mod = name_to_mod(argv[i + 1]);
				if (cmd->mod == WTYPE_MOD_NONE) {
					fail("Invalid modifier name '%s'", argv[i + 1]);
				}
			} else if (!strcmp("-m", argv[i])) {
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


static void run_key(struct wtype *wtype, struct wtype_command *cmd)
{
	zwp_virtual_keyboard_v1_key(
		wtype->keyboard, 0, cmd->single_key_code,
		cmd->type == WTYPE_COMMAND_KEY_PRESS ?
			WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
	);
	wl_display_roundtrip(wtype->display);
}

static void type_keycode(struct wtype *wtype, unsigned int key_code)
{
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


static void print_keysym_name(xkb_keysym_t keysym, FILE *f)
{
	char sym_name[256];

	int ret = xkb_keysym_get_name(keysym, sym_name, sizeof(sym_name));
	if (ret <= 0) {
		fail("Unable to get XKB symbol name for keysym %04x\n", keysym);
		return;
	}

	fprintf(f, "%s", sym_name);
}


static void upload_keymap(struct wtype *wtype)
{
	char filename[] = "/tmp/wtype-XXXXXX";
	int fd = mkstemp(filename);
	if (fd < 0) {
		fail("Failed to create the temporary keymap file");
	}
	unlink(filename);
	FILE *f = fdopen(fd, "w");

	fprintf(f, "xkb_keymap {\n");

	fprintf(
		f,
		"xkb_keycodes \"(unnamed)\" {\n"
		"minimum = 8;\n"
		"maximum = %ld;\n",
		wtype->keymap_len + 8 + 1
	);
	for (size_t i = 0; i < wtype->keymap_len; i++) {
		fprintf(f, "<K%ld> = %ld;\n", i + 1, i + 8 + 1);
	}
	fprintf(f, "};\n");

	// TODO: Is including "complete" here really a good idea?
	fprintf(f, "xkb_types \"(unnamed)\" { include \"complete\" };\n");
	fprintf(f, "xkb_compatibility \"(unnamed)\" { include \"complete\" };\n");

	fprintf(f, "xkb_symbols \"(unnamed)\" {\n");
	for (size_t i = 0; i < wtype->keymap_len; i++) {
		fprintf(f, "key <K%ld> {[", i + 1);
		print_keysym_name(wtype->keymap[i].xkb, f);
		fprintf(f, "]};\n");
	}
	fprintf(f, "};\n");

	fprintf(f, "};\n");
	fputc('\0', f);
	fflush(f);
	size_t keymap_size = ftell(f);

	zwp_virtual_keyboard_v1_keymap(
		wtype->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(f), keymap_size
	);

	wl_display_roundtrip(wtype->display);

	fclose(f);
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
	zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
	wl_registry_destroy(wtype.registry);
	wl_display_disconnect(wtype.display);
}
