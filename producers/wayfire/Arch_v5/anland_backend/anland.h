#ifndef WLR_BACKEND_ANLAND_H
#define WLR_BACKEND_ANLAND_H

#include <wlr/backend.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wlr_backend *wlr_anland_backend_create(struct wl_event_loop *loop,
	const char *socket_path);

bool wlr_backend_is_anland(struct wlr_backend *backend);

#ifdef __cplusplus
}
#endif

#endif /* WLR_BACKEND_ANLAND_H */
