#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlhangul.h"
#include "input-method-unstable-v2-client-protocol.h"

static void handle_key(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct wlhangul_seat *seat = data;
	xkb_keycode_t xkb_key = key + 8;

	if (seat->xkb_state == NULL) {
		return;
	}

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}

	bool handled;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->xkb_state, xkb_key);
	switch (sym) {
	case XKB_KEY_Escape:
		seat->state->running = false;
		handled = true;
		break;
	case XKB_KEY_BackSpace:
		handled = hangul_ic_backspace(seat->input_context);
		break;
	// TODO: XKB_KEY_Hangul
	default:;
		uint32_t ch = xkb_state_key_get_utf32(seat->xkb_state, xkb_key);
		handled = hangul_ic_process(seat->input_context, ch);
		break;
	}

	const ucschar *commit_ucsstr =
		hangul_ic_get_commit_string(seat->input_context);
	if (commit_ucsstr[0] != 0) {
		char *commit_str = ucsstr_to_str(commit_ucsstr);
		zwp_input_method_v2_commit_string(seat->input_method, commit_str);
		free(commit_str);
	}

	const ucschar *preedit_ucsstr =
		hangul_ic_get_preedit_string(seat->input_context);
	char *preedit_str = ucsstr_to_str(preedit_ucsstr);
	zwp_input_method_v2_set_preedit_string(seat->input_method,
		preedit_str, 0, strlen(preedit_str));
	free(preedit_str);

	zwp_input_method_v2_commit(seat->input_method, seat->serial);

	if (!handled) {
		printf("key: %d\n", xkb_key);
		// TODO: forward key
	}
}

static void handle_modifiers(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct wlhangul_seat *seat = data;

	if (seat->xkb_state == NULL) {
		return;
	}

	xkb_state_update_mask(seat->xkb_state, mods_depressed,
		mods_latched, mods_locked, 0, 0, group);
}

static void handle_keymap(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
		uint32_t format, int32_t fd, uint32_t size) {
	struct wlhangul_seat *seat = data;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	char *str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (str == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return;
	}

	if (seat->xkb_keymap != NULL) {
		xkb_keymap_unref(seat->xkb_keymap);
	}
	seat->xkb_keymap = xkb_keymap_new_from_string(seat->xkb_context, str,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(str, size);
	close(fd);

	if (seat->xkb_keymap == NULL) {
		fprintf(stderr, "Failed to compile keymap\n");
		return;
	}

	if (seat->xkb_state != NULL) {
		xkb_state_unref(seat->xkb_state);
	}
	seat->xkb_state = xkb_state_new(seat->xkb_keymap);
	if (seat->xkb_state == NULL) {
		fprintf(stderr, "Failed to create XKB state\n");
		return;
	}
}

static void handle_repeat_info(void *data,
		struct zwp_input_method_keyboard_grab_v2 *keyboard_grab, int32_t rate,
		int32_t delay) {
	// TODO
}

static const struct zwp_input_method_keyboard_grab_v2_listener
		keyboard_grab_listener = {
	.key = handle_key,
	.modifiers = handle_modifiers,
	.keymap = handle_keymap,
	.repeat_info = handle_repeat_info,
};

static void handle_activate(void *data,
		struct zwp_input_method_v2 *input_method) {
	struct wlhangul_seat *seat = data;
	seat->pending_activate = true;
}

static void handle_deactivate(void *data,
		struct zwp_input_method_v2 *input_method) {
	struct wlhangul_seat *seat = data;
	seat->pending_deactivate = true;
}

static void handle_surrounding_text(void *data,
		struct zwp_input_method_v2 *input_method, const char *text,
		uint32_t cursor, uint32_t anchor) {
}

static void handle_text_change_cause(void *data,
		struct zwp_input_method_v2 *input_method, uint32_t cause) {
}

static void handle_content_type(void *data,
		struct zwp_input_method_v2 *input_method, uint32_t hint,
		uint32_t purpose) {
}

static void handle_done(void *data, struct zwp_input_method_v2 *input_method) {
	struct wlhangul_seat *seat = data;
	seat->serial++;

	if (seat->pending_activate && !seat->active) {
		seat->keyboard_grab = zwp_input_method_v2_grab_keyboard(input_method);
		zwp_input_method_keyboard_grab_v2_add_listener(seat->keyboard_grab,
			&keyboard_grab_listener, seat);
		seat->active = true;
	} else if (seat->pending_deactivate && seat->active) {
		zwp_input_method_keyboard_grab_v2_release(seat->keyboard_grab);
		hangul_ic_reset(seat->input_context);
		seat->keyboard_grab = NULL;
		seat->active = false;
	}

	seat->pending_activate = false;
	seat->pending_deactivate = false;
}

static void handle_unavailable(void *data,
		struct zwp_input_method_v2 *input_method) {
}

static const struct zwp_input_method_v2_listener input_method_listener = {
	.activate = handle_activate,
	.deactivate = handle_deactivate,
	.surrounding_text = handle_surrounding_text,
	.text_change_cause = handle_text_change_cause,
	.content_type = handle_content_type,
	.done = handle_done,
	.unavailable = handle_unavailable,
};

static struct wlhangul_seat *create_seat(struct wlhangul_state *state,
		struct wl_seat *wl_seat) {
	struct wlhangul_seat *seat = calloc(1, sizeof(*seat));
	seat->wl_seat = wl_seat;
	seat->state = state;
	wl_list_insert(&state->seats, &seat->link);
	return seat;
}

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wlhangul_state *state = data;
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		create_seat(state, seat);
	} else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
		state->input_method_manager = wl_registry_bind(registry, name,
			&zwp_input_method_manager_v2_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	// TODO
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

int main(int argc, char *argv[]) {
	struct wlhangul_state state = {0};
	wl_list_init(&state.seats);

	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		fprintf(stderr, "failed to connect to Wayland display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	if (state.input_method_manager == NULL) {
		fprintf(stderr, "missing wl_seat or zwp_input_method_manager_v2\n");
		return 1;
	}

	struct wlhangul_seat *seat;
	wl_list_for_each(seat, &state.seats, link) {
		seat->input_context = hangul_ic_new("2");
		seat->input_method = zwp_input_method_manager_v2_get_input_method(
			state.input_method_manager, seat->wl_seat);
		zwp_input_method_v2_add_listener(seat->input_method,
			&input_method_listener, seat);
		seat->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	}

	state.running = true;
	while (state.running && wl_display_dispatch(state.display) != -1) {
		// This space is intentionally left blank
	}

	return 0;
}