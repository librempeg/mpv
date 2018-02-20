#ifndef MP_CLIENT_H_
#define MP_CLIENT_H_

#include <stdint.h>
#include <stdbool.h>

#include "libmpv/client.h"
#include "libmpv/stream_cb.h"
#include "misc/bstr.h"

struct MPContext;
struct mpv_handle;
struct mp_client_api;
struct mp_log;
struct mpv_global;

// Includes space for \0
#define MAX_CLIENT_NAME 64

void mp_clients_init(struct MPContext *mpctx);
void mp_client_enter_shutdown(struct MPContext *mpctx);
void mp_clients_destroy(struct MPContext *mpctx);
int mp_clients_num(struct MPContext *mpctx);
bool mp_clients_all_initialized(struct MPContext *mpctx);

bool mp_client_exists(struct MPContext *mpctx, const char *client_name);
void mp_client_broadcast_event(struct MPContext *mpctx, int event, void *data);
int mp_client_send_event(struct MPContext *mpctx, const char *client_name,
                         int event, void *data);
int mp_client_send_event_dup(struct MPContext *mpctx, const char *client_name,
                             int event, void *data);
bool mp_client_event_is_registered(struct MPContext *mpctx, int event);
void mp_client_property_change(struct MPContext *mpctx, const char *name);

struct mpv_handle *mp_new_client(struct mp_client_api *clients, const char *name);
struct mp_log *mp_client_get_log(struct mpv_handle *ctx);
struct mpv_global *mp_client_get_global(struct mpv_handle *ctx);
struct MPContext *mp_client_get_core(struct mpv_handle *ctx);
struct MPContext *mp_client_api_get_core(struct mp_client_api *api);
void *mp_get_sub_api2(mpv_handle *ctx, mpv_sub_api sub_api, bool lock);

// m_option.c
void *node_get_alloc(struct mpv_node *node);

// for vo_libmpv.c
struct osd_state;
struct mpv_render_context;
bool mp_set_main_render_context(struct mp_client_api *client_api,
                                struct mpv_render_context *ctx, bool active);
struct mpv_render_context *
mp_client_api_acquire_render_context(struct mp_client_api *ca);
void kill_video(struct mp_client_api *client_api);

bool mp_streamcb_lookup(struct mpv_global *g, const char *protocol,
                        void **out_user_data, mpv_stream_cb_open_ro_fn *out_fn);

// Legacy.
typedef int (*mpv_opengl_cb_control_fn)(void *cb_ctx, int *events,
                                        uint32_t request, void *data);
struct mpv_opengl_cb_context;
void mp_client_set_control_callback(struct mpv_opengl_cb_context *ctx,
                                    mpv_opengl_cb_control_fn callback,
                                    void *callback_ctx);
void mp_client_set_icc_profile(struct mpv_opengl_cb_context *ctx, bstr icc_data);
void mp_client_set_ambient_lux(struct mpv_opengl_cb_context *ctx, int lux);

#endif
