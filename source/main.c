/*
 * ScreenFX - OnionHEN plugin. This process owns the settings and the Toolbox
 * page and publishes the active look to FX_STATE_PATH. Drawing and the button
 * combo live in the renderer module, which this process loads into SceShellUI
 * (fx_inject.c) and which talks back through FX_REQUEST_PATH.
 */
#include <onion/client.h>
#include <onion/plugin.h>
#include <onion/status.h>
#include <onion/transport.h>
#include <onion/ui.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "fx_inject.h"
#include "fx_settings.h"
#include "fx_state.h"
#include "fx_ui.h"
#include "plugin_config.h"

#define CONNECT_ATTEMPTS 30
#define CONNECT_RETRY_US (250 * 1000)
#define EVENT_POLL_US (50 * 1000)
#define INJECT_POLL_TICKS 40 /* every ~2 s */

extern const onion_plugin_descriptor_v1 onion_plugin_descriptor;

typedef struct NotifyRequest {
    char useless[45];
    char message[3075];
} NotifyRequest;

int sceKernelSendNotificationRequest(int device, NotifyRequest *request, size_t size, int unused);

typedef struct plugin_app {
    onion_transport transport;
    onion_socket_transport socket;
    onion_client client;
    onion_host_services_v1 services;
    onion_ui_document *document;
    onion_ui_handle ui_handle;
    fx_settings settings;
    FILE *log_file;
    int transport_connected;
    int client_initialized;
} plugin_app;

static volatile sig_atomic_t running = 1;

static void request_stop(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void log_message(plugin_app *app, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);

    if (!app->log_file) return;
    va_start(arguments, format);
    vfprintf(app->log_file, format, arguments);
    va_end(arguments);
    fflush(app->log_file);
}

static void send_notification(plugin_app *app, const char *text) {
    if (app->client_initialized && onion_services_validate(&app->services) == ONION_OK &&
        app->services.notify && onion_service_notify(&app->services, text) == ONION_OK) {
        return;
    }
    /* The host notify service is not implemented in every OnionHEN build. */
    NotifyRequest request;
    memset(&request, 0, sizeof(request));
    snprintf(request.message, sizeof(request.message), "%s", text);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

/* Persists and publishes the current settings after any change. */
static void commit(plugin_app *app) {
    if (fx_settings_save(PLUGIN_SETTINGS_PATH, &app->settings) != 0) {
        log_message(app, "[%s] failed to save %s\n", PLUGIN_ID, PLUGIN_SETTINGS_PATH);
    }
    if (fx_state_publish(&app->settings) != 0) {
        log_message(app, "[%s] failed to publish %s\n", PLUGIN_ID, FX_STATE_PATH);
    }
}

static onion_status connect_to_daemon(plugin_app *app) {
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS && running; ++attempt) {
        if (onion_socket_transport_connect(&app->transport, &app->socket,
                                           ONION_PLUGIN_IPC_SOCKET_PATH) == ONION_OK) {
            app->transport_connected = 1;
            return ONION_OK;
        }
        if (attempt < CONNECT_ATTEMPTS) usleep(CONNECT_RETRY_US);
    }
    return ONION_E_IO;
}

static onion_status start_plugin(plugin_app *app) {
    onion_status status = connect_to_daemon(app);
    if (status != ONION_OK) return status;

    status = onion_client_init(&app->client, &app->transport);
    if (status != ONION_OK) return status;
    app->client_initialized = 1;

    status = onion_client_open_session(&app->client, &onion_plugin_descriptor);
    if (status == ONION_OK) status = onion_client_make_services(&app->client, &app->services);
    if (status != ONION_OK) return status;

    status = fx_ui_create(&app->settings, &app->document);
    if (status == ONION_OK) {
        status = onion_ui_register(&app->services, app->document, &app->ui_handle);
    }
    return status;
}

static void stop_plugin(plugin_app *app) {
    if (app->ui_handle != 0) (void)onion_ui_unregister(&app->services, app->ui_handle);
    if (app->document) onion_ui_document_destroy(app->document);
    if (app->client_initialized) onion_client_deinit(&app->client);
    if (app->transport_connected) onion_socket_transport_deinit(&app->transport);

    /* Leave the screen clean when the plugin is stopped from the Toolbox. */
    fx_settings off = app->settings;
    off.enabled = 0;
    (void)fx_state_publish(&off);
}

static void handle_combo_request(plugin_app *app, fx_request request) {
    char text[96];
    if (request == FX_REQUEST_TOGGLE) {
        app->settings.enabled = !app->settings.enabled;
        snprintf(text, sizeof(text), "ScreenFX %s", app->settings.enabled ? "on" : "off");
    } else {
        int next = app->settings.preset + 1;
        if (next >= FX_PRESET_COUNT) next = FX_PRESET_CRT; /* skip Custom while cycling */
        fx_settings_apply_preset(&app->settings, next);
        app->settings.enabled = 1;
        snprintf(text, sizeof(text), "ScreenFX: %s", fx_preset_name(next));
    }
    commit(app);
    fx_ui_sync(&app->services, app->ui_handle, &app->settings);
    log_message(app, "[%s] combo: %s\n", PLUGIN_ID, text);
    if (app->settings.notify) send_notification(app, text);
}

static void poll_renderer(plugin_app *app) {
    char detail[160];
    const fx_inject_result result = fx_inject_poll(detail, sizeof(detail));
    if (result == FX_INJECT_NOTHING) return;
    log_message(app, "[%s] renderer: %s\n", PLUGIN_ID, detail);
    if (result == FX_INJECT_GUARD_TRIPPED) {
        /* The system UI restarted right after we loaded into it: stop trying. */
        app->settings.renderer = 0;
        commit(app);
        fx_ui_sync(&app->services, app->ui_handle, &app->settings);
        send_notification(app, "ScreenFX: drawing switched off (system UI restarted)");
    }
}

static int run_event_loop(plugin_app *app) {
    int inject_ticks = INJECT_POLL_TICKS; /* check right away on start */
    while (running) {
        const fx_request request = fx_state_take_request();
        if (request != FX_REQUEST_NONE) handle_combo_request(app, request);

        if (app->settings.renderer && ++inject_ticks >= INJECT_POLL_TICKS) {
            inject_ticks = 0;
            poll_renderer(app);
        }

        onion_ui_event_v1 event;
        onion_status status = onion_client_poll_ui_event(&app->client, &event);
        if (status == ONION_E_NOT_FOUND) {
            usleep(EVENT_POLL_US);
            continue;
        }
        if (status != ONION_OK) {
            log_message(app, "[%s] UI event poll failed: %s\n", PLUGIN_ID,
                        onion_status_string(status));
            return 1;
        }

        const int old_preset = app->settings.preset;
        status = fx_ui_apply_event(app->ui_handle, &event, &app->settings);
        if (status == ONION_OK) {
            commit(app);
            /* A preset rewrites other rows, and a manual edit flips Preset to Custom. */
            if (strcmp(event.node_id, "preset") == 0 || app->settings.preset != old_preset) {
                fx_ui_sync(&app->services, app->ui_handle, &app->settings);
            }
        } else if (status != ONION_E_NOT_FOUND) {
            log_message(app, "[%s] action %s failed: %s\n", PLUGIN_ID, event.node_id,
                        onion_status_string(status));
        }
    }
    return 0;
}

int main(void) {
    plugin_app app;
    memset(&app, 0, sizeof(app));

    fx_settings_defaults(&app.settings);
    (void)fx_settings_load(PLUGIN_SETTINGS_PATH, &app.settings);
    app.log_file = fopen(PLUGIN_LOG_PATH, "a");

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    signal(SIGPIPE, SIG_IGN);
    (void)syscall(SYS_thr_set_name, -1, "screenfx.elf");

    log_message(&app, "[%s] starting %s %s\n", PLUGIN_ID, PLUGIN_NAME, PLUGIN_VERSION);
    if (fx_state_prepare() != 0 || fx_state_publish(&app.settings) != 0) {
        log_message(&app, "[%s] cannot write %s\n", PLUGIN_ID, FX_STATE_PATH);
    }

    const onion_status status = start_plugin(&app);
    int exit_code = 1;
    if (status == ONION_OK) {
        log_message(&app, "[%s] ready; enabled=%d preset=%s\n", PLUGIN_ID,
                    app.settings.enabled, fx_preset_name(app.settings.preset));
        exit_code = run_event_loop(&app);
    } else {
        log_message(&app, "[%s] startup failed: %s\n", PLUGIN_ID, onion_status_string(status));
    }

    stop_plugin(&app);
    log_message(&app, "[%s] stopped with exit_code=%d\n", PLUGIN_ID, exit_code);
    if (app.log_file) fclose(app.log_file);
    return exit_code;
}
