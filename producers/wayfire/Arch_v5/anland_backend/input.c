#include "internal.h"

const struct wlr_keyboard_impl anland_keyboard_impl = {.name = "anland-keyboard"};
const struct wlr_pointer_impl anland_pointer_impl = {.name = "anland-pointer"};
const struct wlr_touch_impl anland_touch_impl = {.name = "anland-touch"};

uint32_t anland_now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline double normalized(int32_t value, uint32_t max) {
	if (max <= 1) return 0.0;
	if (value <= 0) return 0.0;
	if ((uint32_t)value >= max) return 1.0;
	return (double)value / (double)(max - 1);
}

static inline void emit_pointer_frame(struct anland_backend *b) {
	wl_signal_emit_mutable(&b->pointer.events.frame, &b->pointer);
}

static bool track_input_id(uint32_t *ids, uint32_t *count, uint32_t id, bool down) {
	for (uint32_t i = 0; i < *count; ++i) {
		if (ids[i] != id) continue;
		if (down) return true;
		ids[i] = ids[--(*count)];
		return true;
	}
	if (!down) return true;
	if (*count == INPUT_PRESSED_MAX) return false;
	ids[(*count)++] = id;
	return true;
}

void anland_clear_input_payload(struct anland_backend *backend) {
	free(backend->input_payload);
	backend->input_payload = NULL;
	backend->input_payload_size = 0;
	backend->input_payload_type = 0;
}

void anland_reset_input(struct anland_backend *b) {
	uint32_t time = anland_now_msec();
	while (b->button_count > 0) {
		uint32_t button = b->pressed_buttons[--b->button_count];
		struct wlr_pointer_button_event e = {
			.pointer = &b->pointer,
			.time_msec = time,
			.button = button,
			.state = WL_POINTER_BUTTON_STATE_RELEASED,
		};
		wlr_pointer_notify_button(&b->pointer, &e);
	}
	emit_pointer_frame(b);

	while (b->touch_count > 0) {
		int32_t touch_id = (int32_t)b->touch_ids[--b->touch_count];
		struct wlr_touch_up_event e = {
			.touch = &b->touch,
			.time_msec = time,
			.touch_id = touch_id,
		};
		wl_signal_emit_mutable(&b->touch.events.up, &e);
	}
	wl_signal_emit_mutable(&b->touch.events.frame, &b->touch);
}

static bool drain_input_payload(struct anland_backend *backend) {
	int rc = anland_device_read_input(backend->device, backend->input_payload,
		backend->input_payload_size, 0);
	if (rc <= 0) return false;
	backend->input_payload[backend->input_payload_size] = '\0';
	anland_clear_input_payload(backend);
	return true;
}

static bool drain_input_fds(struct anland_backend *backend) {
	int fds[64], count = 0;
	int rc = anland_device_read_fds(backend->device, fds, 64, &count, 0);
	if (rc <= 0) return false;
	bool valid = (uint32_t)count == backend->input_fd_count;
	uint32_t resource_type = backend->input_resource_type;
	bool invalid = backend->input_resource_invalid;
	backend->input_fds_pending = false;
	backend->input_fd_count = 0;
	backend->input_resource_type = 0;
	backend->input_resource_invalid = false;
	if (valid && !invalid && resource_type == ANLAND_DEVICE_SERVICE_CAMERA &&
			backend->camera_started && count >= 2) {
		anland_camera_set_resources(fds[0], &fds[1], count - 1);
		close(fds[0]);
		for (int i = 1; i < count; ++i) close(fds[i]);
	} else {
		for (int i = 0; i < count; ++i) close(fds[i]);
	}
	if (!valid) anland_drop_session(backend);
	return valid;
}

int anland_handle_input(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct anland_backend *b = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		anland_drop_session(b); return 0;
	}
	if (b->input_payload && !drain_input_payload(b)) return 0;
	if (b->input_fds_pending && !drain_input_fds(b)) return 0;

	anland_device_input_t ev;
	int ret;
	while ((ret = anland_device_poll_input(b->device, &ev, 0)) > 0) {
		uint32_t time = anland_now_msec();
		switch (ev.type) {
		case ANLAND_DEVICE_IN_KEY: {
			struct wlr_keyboard_key_event e = {
				.time_msec = time,
				.keycode = ev.key.keycode,
				.update_state = true,
				.state = (ev.key.action == ANLAND_DEVICE_ACTION_DOWN)
					? WL_KEYBOARD_KEY_STATE_PRESSED
					: WL_KEYBOARD_KEY_STATE_RELEASED,
			};
			wlr_keyboard_notify_key(&b->keyboard, &e);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_MOTION: {
			struct wlr_pointer_motion_absolute_event e = {
				.pointer = &b->pointer,
				.time_msec = time,
				.x = normalized(ev.pointer_motion.x, b->width),
				.y = normalized(ev.pointer_motion.y, b->height),
			};
			wl_signal_emit_mutable(&b->pointer.events.motion_absolute, &e);
			emit_pointer_frame(b);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_BUTTON: {
			if (!track_input_id(b->pressed_buttons, &b->button_count,
					ev.pointer_button.button, ev.pointer_button.pressed)) {
				anland_drop_session(b); return 0;
			}
			struct wlr_pointer_button_event e = {
				.pointer = &b->pointer,
				.time_msec = time,
				.button = ev.pointer_button.button,
				.state = ev.pointer_button.pressed
					? WL_POINTER_BUTTON_STATE_PRESSED
					: WL_POINTER_BUTTON_STATE_RELEASED,
			};
			wlr_pointer_notify_button(&b->pointer, &e);
			emit_pointer_frame(b);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_AXIS: {
			struct wlr_pointer_axis_event e = {
				.pointer = &b->pointer,
				.time_msec = time,
				.source = WL_POINTER_AXIS_SOURCE_WHEEL,
				.orientation = (ev.pointer_axis.axis == 0)
					? WL_POINTER_AXIS_VERTICAL_SCROLL
					: WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
				.delta = ev.pointer_axis.value,
				.delta_discrete = ev.pointer_axis.discrete * WLR_POINTER_AXIS_DISCRETE_STEP,
			};
			wl_signal_emit_mutable(&b->pointer.events.axis, &e);
			emit_pointer_frame(b);
			break;
		}
		case ANLAND_DEVICE_IN_TOUCH: {
			if (ev.touch.action != ANLAND_DEVICE_ACTION_MOVE &&
					!track_input_id(b->touch_ids, &b->touch_count,
						(uint32_t)ev.touch.pointer_id, ev.touch.action == ANLAND_DEVICE_ACTION_DOWN)) {
				anland_drop_session(b); return 0;
			}
			double x = normalized(ev.touch.x, b->width);
			double y = normalized(ev.touch.y, b->height);
			if (ev.touch.action == ANLAND_DEVICE_ACTION_DOWN) {
				struct wlr_touch_down_event e = {
					.touch = &b->touch,
					.time_msec = time,
					.touch_id = ev.touch.pointer_id,
					.x = x,
					.y = y,
				};
				wl_signal_emit_mutable(&b->touch.events.down, &e);
			} else if (ev.touch.action == ANLAND_DEVICE_ACTION_MOVE) {
				struct wlr_touch_motion_event e = {
					.touch = &b->touch,
					.time_msec = time,
					.touch_id = ev.touch.pointer_id,
					.x = x,
					.y = y,
				};
				wl_signal_emit_mutable(&b->touch.events.motion, &e);
			} else {
				struct wlr_touch_up_event e = {
					.touch = &b->touch,
					.time_msec = time,
					.touch_id = ev.touch.pointer_id,
				};
				wl_signal_emit_mutable(&b->touch.events.up, &e);
			}
			break;
		}
		case ANLAND_DEVICE_IN_TOUCH_FRAME:
			wl_signal_emit_mutable(&b->touch.events.frame, &b->touch);
			break;
		case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
			if (ev.display.refresh_mhz && ev.display.refresh_mhz != b->refresh) {
				b->refresh = ev.display.refresh_mhz;
				struct wlr_output_state state;
				wlr_output_state_init(&state);
				wlr_output_state_set_custom_mode(&state, b->width, b->height, b->refresh);
				wlr_output_send_request_state(&b->output.base, &state);
				wlr_output_state_finish(&state);
			}
			break;
		case ANLAND_DEVICE_IN_CLIPBOARD:
		case ANLAND_DEVICE_IN_TEXT_INPUT: {
			size_t size = ev.type == ANLAND_DEVICE_IN_CLIPBOARD ? ev.clipboard.size : ev.text_input.size;
			if (size > INPUT_PAYLOAD_MAX) { anland_drop_session(b); return 0; }
			if (size == 0) break;
			b->input_payload = malloc(size + 1);
			if (!b->input_payload) { anland_drop_session(b); return 0; }
			b->input_payload_size = size;
			b->input_payload_type = ev.type;
			if (!drain_input_payload(b)) return 0;
			break;
		}
		case ANLAND_DEVICE_IN_RESOURCE:
		case ANLAND_DEVICE_IN_RESOURCE_INVALID:
			if (ev.resource.fdnum > 64) { anland_drop_session(b); return 0; }
			if (ev.resource.fdnum == 0) {
				if (ev.resource.type == ANLAND_DEVICE_SERVICE_CAMERA)
					anland_camera_clear();
				break;
			}
			b->input_fds_pending = true;
			b->input_fd_count = ev.resource.fdnum;
			b->input_resource_type = ev.resource.type;
			b->input_resource_invalid = ev.type == ANLAND_DEVICE_IN_RESOURCE_INVALID;
			if (!drain_input_fds(b)) return 0;
			break;
		default:
			break;
		}
	}
	if (ret < 0) anland_drop_session(b);
	return 0;
}
