
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <stdbool.h>
#include <wayland-client.h>
#include <locale.h>

#include "wayland-virtual-keyboard-client-protocol.h"


struct wtype {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwp_virtual_keyboard_manager_v1 *manager;
	struct zwp_virtual_keyboard_v1 *keyboard;
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


int main(int argc, const char *argv[])
{
	if (argc < 2) {
		fail("Usage: %s <text-to-type>", argv[0]);
	}

	struct wtype wtype;
	memset(&wtype, 0, sizeof(wtype));

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

	// Now we process the input

	setlocale(LC_CTYPE, "");
	// TODO: Merge all argvs
	size_t raw_len = strlen(argv[1]);
	wchar_t text[raw_len + 1];
	memset(text, 0, sizeof(text));
	ssize_t text_len = mbstowcs(text, argv[1], raw_len);
	if (text_len < 0) {
		fail("Failed to deencode input argv");
	}

	unsigned int text_codes[text_len];
	wchar_t keychars[text_len];  // Upper bound
	memset(keychars, 0, sizeof(keychars));
	int char_count = 0;
	for (ssize_t i = 0; i < text_len; i++) {
		bool found = false;
		for (ssize_t k = 0; k < char_count; k++) {
			if (text[i] == keychars[k]) {
				text_codes[i] = k;
				found = true;
				break;
			}
		}
		if (!found) {
			keychars[char_count] = text[i];
			text_codes[i] = char_count;
			char_count++;
		}
	}

	// Generate the keymap
	// TODO: Move to SHM
	const char *filename = "/tmp/wtype";
	int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0660);
	FILE *f = fdopen(fd, "w");

	fprintf(f, "xkb_keymap {\n");

	fprintf(
		f,
		"xkb_keycodes \"(unnamed)\" {\n"
		"minimum = 8;\n"
		"maximum = %d;\n",
		char_count + 8
	);
	for (int i = 0; i < char_count; i++) {
		fprintf(f, "<K%d> = %d;\n", i, i + 8);
	}
	fprintf(f, "};\n");

	fprintf(f, "xkb_types \"(unnamed)\" {};\n");
	fprintf(f, "xkb_compatibility \"(unnamed)\" {};\n");

	fprintf(f, "xkb_symbols \"(unnamed)\" {\n");
	fprintf(f, "name[group1]=\"English (US)\";\n");
	for (int i = 0; i < char_count; i++) {
		fprintf(f, "key <K%d> {[U%04x]};\n", i, keychars[i]);
	}
	fprintf(f, "};\n");

	fprintf(f, "};\n");
	fputc('\0', f);
	fflush(f);
	size_t keymap_size = ftell(f);

	zwp_virtual_keyboard_v1_keymap(
		wtype.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(f), keymap_size
	);

	wl_display_dispatch(wtype.display);
	wl_display_roundtrip(wtype.display);

	for (int i = 0; i < text_len; i++) {
		zwp_virtual_keyboard_v1_key(
			wtype.keyboard, 0, text_codes[i], WL_KEYBOARD_KEY_STATE_PRESSED
		);
		wl_display_roundtrip(wtype.display);
		zwp_virtual_keyboard_v1_key(
			wtype.keyboard, 0, text_codes[i], WL_KEYBOARD_KEY_STATE_RELEASED
		);
		wl_display_roundtrip(wtype.display);
	}
}
