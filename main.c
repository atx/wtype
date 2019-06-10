
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

#include "wayland-virtual-keyboard-client-protocol.h"


#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))


enum wtype_command_type {
	WTYPE_COMMAND_TEXT = 0,
	WTYPE_COMMAND_MOD_PRESS = 1,
	WTYPE_COMMAND_MOD_RELEASE = 2,
	WTYPE_COMMAND_SLEEP = 3
};


enum wtype_mod {
	WTYPE_MOD_NONE = 0,
	WTYPE_MOD_SHIFT = 1,
	WTYPE_MOD_CAPSLOCK = 2,
	WTYPE_MOD_CTRL = 4,
	WTYPE_MOD_LOGO = 64,
	WTYPE_MOD_ALTGR = 128
};


struct wtype_command {
	enum wtype_command_type type;
	union {
		struct {
			unsigned int *key_codes;
			size_t key_codes_len;
		};
		enum wtype_mod mod;
		unsigned int sleep_ms;
	};
};


struct wtype {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwp_virtual_keyboard_manager_v1 *manager;
	struct zwp_virtual_keyboard_v1 *keyboard;

	size_t keymap_len;
	wchar_t *keymap;
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
	{"alt", WTYPE_MOD_ALTGR},
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


unsigned int get_key_code(struct wtype *wtype, wchar_t ch)
{
	for (unsigned int i = 0; i < wtype->keymap_len; i++) {
		if (wtype->keymap[i] == ch) {
			return i;
		}
	}
	wtype->keymap = reallocarray(
		wtype->keymap, ++wtype->keymap_len, sizeof(wtype->keymap[0])
	);
	wtype->keymap[wtype->keymap_len - 1] = ch;
	return wtype->keymap_len - 1;
}


// TODO: Expand this
static const struct {wchar_t code; const char *name;} special_keys[] = {
	{'\t', "Tab"},
	{'\b', "BackSpace"},
	{'\n', "Return"},
};


static void parse_args(struct wtype *wtype, int argc, const char *argv[])
{
	wtype->commands = calloc(argc, sizeof(wtype->commands[0])); // Upper bound
	wtype->command_count = 0;
	bool raw_text = false;
	for (int i = 1; i < argc; i++) {
		struct wtype_command *cmd = &wtype->commands[wtype->command_count];
		if (!raw_text && !strcmp("--", argv[i])) {
			raw_text = true;
		} else if (!raw_text && argv[i][0] == '-'){
			if (i == argc - 1) {
				fail("Missing argument to %s", argv[i]);
			}
			if (!strcmp("-M", argv[i])) {
				// Press modifier
				cmd->type = WTYPE_COMMAND_MOD_PRESS;
				cmd->mod = name_to_mod(argv[i + 1]);
			} else if (!strcmp("-m", argv[i])) {
				// Release modifier
				cmd->type = WTYPE_COMMAND_MOD_RELEASE;
				cmd->mod = name_to_mod(argv[i + 1]);
			} else if (!strcmp("-s", argv[i])) {
				// Sleep
				cmd->type = WTYPE_COMMAND_SLEEP;
				cmd->sleep_ms = atoi(argv[i + 1]);
				if (cmd->sleep_ms <= 0) {
					fail("Invalid sleep time");
				}
			} else if (!strcmp("-k", argv[i])) {
				size_t k;
				for (k = 0; k < ARRAY_SIZE(special_keys); k++) {
					if (!strcasecmp(special_keys[k].name, argv[i + 1])) {
						break;
					}
				}
				if (k == ARRAY_SIZE(special_keys)) {
					fail("Unknown special key '%s'", argv[i + 1]);
				}
				cmd->type = WTYPE_COMMAND_TEXT;
				cmd->key_codes = malloc(sizeof(cmd->key_codes[0]));
				cmd->key_codes_len = 1;
				cmd->key_codes[0] = get_key_code(wtype, special_keys[k].code);
			} else {
				fail("Unknown parameter %s", argv[i]);
			}
			i++;
			wtype->command_count++;
		} else {
			// Text
			cmd->type = WTYPE_COMMAND_TEXT;
			wtype->command_count++;

			size_t raw_len = strlen(argv[i]) + 1;
			wchar_t text[raw_len];  // Upper bound on size
			memset(text, 0, sizeof(text));
			setlocale(LC_CTYPE, "");
			ssize_t ret = mbstowcs(text, argv[i], ARRAY_SIZE(text));
			if (ret < 0) {
				fail("Failed to deencode input argv");
			}
			cmd->key_codes = calloc(ret, sizeof(cmd->key_codes[0]));
			cmd->key_codes_len = ret;
			for (ssize_t k = 0; k < ret; k++) {
				cmd->key_codes[k] = get_key_code(wtype, text[k]);
			}
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

}


static void run_text(struct wtype *wtype, struct wtype_command *cmd)
{
	for (size_t i = 0; i < cmd->key_codes_len; i++) {
		zwp_virtual_keyboard_v1_key(
			wtype->keyboard, 0, cmd->key_codes[i], WL_KEYBOARD_KEY_STATE_PRESSED
		);
		wl_display_roundtrip(wtype->display);
		zwp_virtual_keyboard_v1_key(
			wtype->keyboard, 0, cmd->key_codes[i], WL_KEYBOARD_KEY_STATE_RELEASED
		);
		wl_display_roundtrip(wtype->display);
	}
}


static void run_commands(struct wtype *wtype)
{
	void (*handlers[])(struct wtype *, struct wtype_command *) = {
		[WTYPE_COMMAND_SLEEP] = run_sleep,
		[WTYPE_COMMAND_MOD_PRESS] = run_mod,
		[WTYPE_COMMAND_MOD_RELEASE] = run_mod,
		[WTYPE_COMMAND_TEXT] = run_text,
	};
	for (unsigned int i = 0; i < wtype->command_count; i++) {
		handlers[wtype->commands[i].type](wtype, &wtype->commands[i]);
	}
}


static void upload_keymap(struct wtype *wtype)
{
	const char *filename_format = "/wtype-%d";
	char filename[sizeof(filename_format) + 10];
	int fd = -1;
	for (int i = 0; i < 1000; i++) {
		snprintf(filename, sizeof(filename), filename_format, i);
		fd = shm_open(filename, O_RDWR | O_EXCL | O_CREAT | O_TRUNC, 0660);
		if (fd >= 0) {
			// We can unlink immediately - wayland eats the file descriptor
			// not filename
			shm_unlink(filename);
			break;
		}
	}
	if (fd <= 0) {
		fail("Failed to open SHM object");
	}
	// Note: this is technically undefined, we can't do fdopen on SHM objects
	FILE *f = fdopen(fd, "w");

	fprintf(f, "xkb_keymap {\n");

	fprintf(
		f,
		"xkb_keycodes \"(unnamed)\" {\n"
		"minimum = 8;\n"
		"maximum = %ld;\n",
		wtype->keymap_len + 8
	);
	for (size_t i = 0; i < wtype->keymap_len; i++) {
		fprintf(f, "<K%ld> = %ld;\n", i, i + 8);
	}
	fprintf(f, "};\n");

	fprintf(f, "xkb_types \"(unnamed)\" {};\n");
	fprintf(f, "xkb_compatibility \"(unnamed)\" {};\n");

	fprintf(f, "xkb_symbols \"(unnamed)\" {\n");
	for (size_t i = 0; i < wtype->keymap_len; i++) {
		size_t k;
		for (k = 0; k < ARRAY_SIZE(special_keys); k++) {
			if (special_keys[k].code == wtype->keymap[i]) {
				fprintf(f, "key <K%ld> {[%s]};\n", i, special_keys[k].name);
				break;
			}
		}
		if (k == ARRAY_SIZE(special_keys)) {
			fprintf(f, "key <K%ld> {[U%04x]};\n", i, wtype->keymap[i]);
		}
	}
	fprintf(f, "};\n");

	fprintf(f, "};\n");
	fputc('\0', f);
	fflush(f);
	size_t keymap_size = ftell(f);

	zwp_virtual_keyboard_v1_keymap(
		wtype->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(f), keymap_size
	);

	wl_display_dispatch(wtype->display);
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
