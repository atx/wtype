
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
	size_t raw_len = 0;
	for (int i = 1; i < argc; i++) {
		// byte count + space
		raw_len += strlen(argv[i]) + 1;
	}
	wchar_t text[raw_len + 1]; // Upper bound on size
	memset(text, 0, sizeof(text));
	size_t text_len = 0;
	for (int i = 1; i < argc; i++) {
		wchar_t tmp[strlen(argv[i]) + 2];
		ssize_t ret = mbstowcs(tmp, argv[i], raw_len - text_len);
		if (ret < 0) {
			fail("Failed to deencode input argv");
		}
		if (i == argc - 1) {
			tmp[ret] = 0;
		} else {
			tmp[ret++] = L' ';
			tmp[ret] = 0;
		}
		wcscat(text, tmp);
		text_len += ret;
	}

	unsigned int text_codes[text_len];
	wchar_t key_to_char[text_len];  // Upper bound
	memset(key_to_char, 0, sizeof(key_to_char));
	int char_count = 0;
	for (size_t i = 0; i < text_len; i++) {
		bool found = false;
		for (ssize_t k = 0; k < char_count; k++) {
			if (text[i] == key_to_char[k]) {
				text_codes[i] = k;
				found = true;
				break;
			}
		}
		if (!found) {
			key_to_char[char_count] = text[i];
			text_codes[i] = char_count;
			char_count++;
		}
	}

	// Generate the keymap
	const char *filename_format = "/wlay-%d";
	char filename[sizeof(filename_format) + 10];
	int fd = -1;
	for (int i = 0; i < 1000; i++) {
		snprintf(filename, sizeof(filename), filename_format, i);
		fd = shm_open(filename, O_RDWR | O_CREAT | O_TRUNC, 0660);
		if (fd >= 0) {
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
		fprintf(f, "key <K%d> {[U%04x]};\n", i, key_to_char[i]);
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

	fclose(f);

	for (size_t i = 0; i < text_len; i++) {
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
