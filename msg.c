#include "globalconf.h"
#include "msg.h"

#include <assert.h>
#include <webkit2/webkit2.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "clib/web_module.h"
#include "clib/luakit.h"
#include "clib/widget.h"
#include "common/luaserialize.h"
#include "web_context.h"
#include "widgets/webview.h"

void webview_scroll_recv(void *d, const msg_scroll_t *msg);
void run_javascript_finished(const guint8 *msg, guint length);

#define NO_HANDLER(type) \
void \
msg_recv_##type(msg_endpoint_t *UNUSED(ipc), const gpointer UNUSED(msg), guint UNUSED(length)) \
{ \
    fatal("UI process should never receive message of type %s", #type); \
} \

NO_HANDLER(lua_require_module)
NO_HANDLER(web_lua_loaded)
NO_HANDLER(lua_js_register)
NO_HANDLER(web_extension_loaded)
NO_HANDLER(crash)

void
msg_recv_lua_msg(msg_endpoint_t *UNUSED(ipc), const msg_lua_msg_t *msg, guint length)
{
    web_module_recv(globalconf.L, msg->arg, length);
}

void
msg_recv_scroll(msg_endpoint_t *UNUSED(ipc), msg_scroll_t *msg, guint UNUSED(length))
{
    g_ptr_array_foreach(globalconf.webviews, (GFunc)webview_scroll_recv, msg);
}

void
msg_recv_eval_js(msg_endpoint_t *UNUSED(ipc), const guint8 *msg, guint length)
{
    run_javascript_finished(msg, length);
}

void
msg_recv_lua_js_call(msg_endpoint_t *from, const guint8 *msg, guint length)
{
    lua_State *L = globalconf.L;
    gint top = lua_gettop(L);

    int argc = lua_deserialize_range(L, msg, length) - 1;
    g_assert_cmpint(argc, >=, 1);

    /* Retrieve and pop view id and function ref */
    guint64 view_id = lua_tointeger(L, top + 1);
    gpointer ref = lua_touserdata(L, top + 2);
    lua_remove(L, top+1);
    lua_remove(L, top+1);

    /* push Lua callback function into position */
    luaH_object_push(L, ref);
    lua_insert(L, top+1);

    /* get webview and push into position */
    widget_t *w = webview_get_by_id(view_id);
    g_assert(w);
    luaH_object_push(L, w->ref);
    lua_insert(L, top+2);

    /* Call the function; push result/error and ok/error boolean */
    lua_pushboolean(L, lua_pcall(L, argc, 1, 0));
    if (lua_toboolean(L, -1))
        warn("Lua error: %s\n", lua_tostring(L, -2));

    /* Serialize the result, and send it back */
    msg_send_lua(from, MSG_TYPE_lua_js_call, L, -2, -1);
    lua_settop(L, top);
}

void
msg_recv_lua_js_gc(msg_endpoint_t *UNUSED(ipc), const guint8 *msg, guint length)
{
    lua_State *L = globalconf.L;
    /* Unref the function reference we got */
    gint n = lua_deserialize_range(L, msg, length);
    g_assert_cmpint(n, ==, 1);
    luaH_object_unref(L, lua_touserdata(L, -1));
    lua_pop(L, 1);
}

void
msg_recv_page_created(msg_endpoint_t *ipc, const guint64 *page_id, guint length)
{
    g_assert(length == sizeof(*page_id));
    widget_t *w = webview_get_by_id(*page_id);
    g_assert(w);

    web_module_load_modules_on_endpoint(ipc, globalconf.L);
    luaH_register_functions_on_endpoint(ipc, globalconf.L);
    webview_connect_to_endpoint(w, ipc);
}

static void
web_extension_connect(msg_endpoint_t *ipc, const gchar *socket_path)
{
    int sock, web_socket;
    struct sockaddr_un local, remote;
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, socket_path);
    int len = strlen(local.sun_path) + sizeof(local.sun_family);

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
        fatal("Can't open new socket");

    /* Remove any pre-existing socket, before opening */
    unlink(local.sun_path);

    if (bind(sock, (struct sockaddr *)&local, len) == -1)
        fatal("Can't bind socket to %s", socket_path);

    if (listen(sock, 5) == -1)
        fatal("Can't listen on %s", socket_path);

    debug("Waiting for a connection...");

    socklen_t size = sizeof(remote);
    if ((web_socket = accept(sock, (struct sockaddr *)&remote, &size)) == -1)
        fatal("Can't accept on %s", socket_path);

    close(sock);
    g_unlink(socket_path);

    debug("Creating channel...");

    msg_endpoint_connect_to_socket(ipc, web_socket);
}

static gpointer
web_extension_connect_thread(gpointer socket_path)
{
    while (TRUE) {
        msg_endpoint_t *ipc = msg_endpoint_new("UI");
        web_extension_connect(ipc, socket_path);
    }

    return NULL;
}

static void
initialize_web_extensions_cb(WebKitWebContext *context, gpointer socket_path)
{
#if DEVELOPMENT_PATHS
    gchar *extension_dir = g_get_current_dir();
#else
    const gchar *extension_dir = LUAKIT_INSTALL_PATH;
#endif

    /* There's a potential race condition here; the accept thread might not run
     * until after the web extension process has already started (and failed to
     * connect). TODO: add a busy wait */

    GVariant *payload = g_variant_new_string(socket_path);
    webkit_web_context_set_web_extensions_initialization_user_data(context, payload);
    webkit_web_context_set_web_extensions_directory(context, extension_dir);
#if DEVELOPMENT_PATHS
    g_free(extension_dir);
#endif
}

void
msg_init(void)
{
    gchar *socket_path = g_build_filename(globalconf.cache_dir, "socket", NULL);
    /* Start web extension connection accept thread */
    g_thread_new("accept_thread", web_extension_connect_thread, socket_path);
    g_signal_connect(web_context_get(), "initialize-web-extensions",
            G_CALLBACK (initialize_web_extensions_cb), socket_path);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
