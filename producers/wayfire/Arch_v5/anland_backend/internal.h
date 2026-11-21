#ifndef WLR_ANLAND_INTERNAL_H
#define WLR_ANLAND_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>

#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include <anland_de_backend.h>
#include <anland_device.h>
#include <anland_audio.h>
#include <anland_camera.h>

#include "anland.h"

#define RECONNECT_MS 200
#define INPUT_PRESSED_MAX 64
#define INPUT_PAYLOAD_MAX ANLAND_DEVICE_MAX_PAYLOAD_SIZE

struct anland_buffer {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attrs;
	bool submitted;
};

struct anland_backend;

struct anland_output {
	struct wlr_output base;
	struct anland_backend *backend;
	struct wlr_swapchain *swapchain;
	struct anland_buffer *buffers[ANLAND_DEVICE_MAX_BUFS];
	uint64_t active_commit;
	uint32_t active_seq;
	uint64_t generation;
	uint32_t buffer_count;
};

struct anland_backend {
	struct wlr_backend base;
	struct wl_event_loop *loop;
	anland_de_backend *de;
	anland_device *device;
	uint64_t output_layer;
	char *socket_path;
	int drm_fd;
	struct anland_output output;
	struct wlr_keyboard keyboard;
	struct wlr_pointer pointer;
	struct wlr_touch touch;

	struct wl_event_source *data_source;
	struct wl_event_source *ready_source;
	struct wl_event_source *reconnect_source;
	struct wl_event_source *scene_source;
	struct wl_event_source *frame_timer;
	struct wl_event_source *pump_timer;
	struct wl_event_source *controls_timer;

	bool destroying;
	bool session_lost;
	bool announced;
	bool hdr_rejection_logged;
	bool output_initialized;
	bool audio_started;
	bool camera_started;

	uint32_t pressed_buttons[INPUT_PRESSED_MAX], button_count;
	uint32_t touch_ids[INPUT_PRESSED_MAX], touch_count;
	uint32_t width, height, format, refresh;

	bool input_fds_pending;
	uint32_t input_fd_count;
	char *input_payload;
	size_t input_payload_size;
	uint32_t input_payload_type;
	uint32_t input_resource_type;
	bool input_resource_invalid;
};

extern const struct wlr_backend_impl anland_backend_impl;
extern const struct wlr_output_impl anland_output_impl;
extern const struct wlr_keyboard_impl anland_keyboard_impl;
extern const struct wlr_pointer_impl anland_pointer_impl;
extern const struct wlr_touch_impl anland_touch_impl;

uint32_t anland_now_msec(void);
void anland_send_frame_result(struct anland_output *output, uint32_t seq, bool presented);
void anland_drop_buffers(struct anland_backend *backend);
bool anland_import_buffers(struct anland_backend *backend);
void anland_drop_session(struct anland_backend *backend);
void anland_arm_frame(struct anland_backend *backend);
void anland_retry_pump(struct anland_backend *backend);
void anland_dispatch_scene(struct anland_backend *backend);
void anland_reset_input(struct anland_backend *backend);
int anland_handle_ready(int fd, uint32_t mask, void *data);
int anland_handle_input(int fd, uint32_t mask, void *data);
int anland_handle_scene(int fd, uint32_t mask, void *data);
void anland_clear_input_payload(struct anland_backend *backend);

#endif /* WLR_ANLAND_INTERNAL_H */
