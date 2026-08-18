/**
 * @file web_server.c
 * @brief lwIP-based HTTP server that serves a dashboard and SSE telemetry stream.
 */

#include "web_server.h"

#include "config.h"
#include "web_dashboard_page.h"
#include "web_motor_test.h"
#include "wifi_sta.h"
#include "imu.h"
#include "dht11.h"

#include <btstack_run_loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

typedef struct {
    struct tcp_pcb* pcb;
    uint32_t backpressure_since_ms;
} sse_client_t;

static struct tcp_pcb* g_server_pcb = NULL;
static motor_controller_t* g_motors_ctrl = NULL;
static bool g_running = false;
static sse_client_t g_sse_clients[WEB_SERVER_MAX_SSE_CLIENTS];
static btstack_timer_source_t g_sse_timer;
static uint32_t g_last_sse_tick_ms = 0;
static uint32_t g_last_sse_backpressure_log_ms = 0;
static char g_page_buffer[WEB_SERVER_PAGE_BUFFER_SIZE];
static char g_event_buffer[WEB_SERVER_EVENT_BUFFER_SIZE];
static DHT11Data g_dht11_data = {0};
static uint32_t g_dht11_last_read_ms = 0;
#define DHT11_POLL_INTERVAL_MS 3000

static err_t http_accept(void* arg, struct tcp_pcb* newpcb, err_t err);
static err_t http_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
static void http_err(void* arg, err_t err);
static void http_close(struct tcp_pcb* tpcb);
static void http_abort(struct tcp_pcb* tpcb);
static void sse_broadcast_timer(btstack_timer_source_t* ts);

static int sse_client_index(const sse_client_t* client) {
    if (client == NULL) {
        return -1;
    }

    return (int)(client - g_sse_clients);
}

const char* web_server_state_text(p_state state) {
    switch (state) {
        case active:
            return "ACTIVE";
        case initializing:
            return "INIT";
        case stopped:
        default:
            return "STOPPED";
    }
}

void web_server_get_snapshot(web_server_snapshot_t* out) {
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const IMUData imu = imu_get_data();
    web_motor_test_status_t test_status = {0};

    web_motor_test_snapshot(now_ms, &test_status);

    out->left = g_motors_ctrl->left_speed;
    out->right = g_motors_ctrl->right_speed;
    out->weapon = g_motors_ctrl->weapon_speed;
    out->state = g_motors_ctrl->state;
    out->failsafe = g_motors_ctrl->failsafe_triggered;
    out->age_ms = now_ms - g_motors_ctrl->last_command_time_ms;
    out->roll = imu.roll;
    out->pitch = imu.pitch;
    out->yaw = imu.yaw;
    out->accel_x = imu.accel_x;
    out->accel_y = imu.accel_y;
    out->accel_z = imu.accel_z;
    out->gyro_x = imu.gyro_x;
    out->gyro_y = imu.gyro_y;
    out->gyro_z = imu.gyro_z;
    out->test_active = test_status.active;
    out->test_motor = test_status.motor_name;
    out->test_power = test_status.power;
    out->test_remaining_ms = test_status.remaining_ms;

    // Poll DHT11 at most every 3 seconds
    if ((now_ms - g_dht11_last_read_ms) >= DHT11_POLL_INTERVAL_MS) {
        DHT11Data reading;
        if (dht11_read(&reading)) {
            g_dht11_data = reading;
        }
        g_dht11_last_read_ms = now_ms;
    }
    out->temp_c = g_dht11_data.temperature;
    out->humidity = g_dht11_data.humidity;
}

static int clamp_response_len(int len, int max_len) {
    if (len < 0) {
        return -1;
    }
    if (len >= max_len) {
        return max_len - 1;
    }
    return len;
}

static err_t tcp_write_response(struct tcp_pcb* tpcb, const char* buffer, int response_len, int max_len) {
    response_len = clamp_response_len(response_len, max_len);
    if (response_len < 0) {
        return ERR_VAL;
    }

    err_t err = tcp_write(tpcb, buffer, (u16_t)response_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        return err;
    }

    return tcp_output(tpcb);
}

static err_t tcp_write_const_response(struct tcp_pcb* tpcb, const char* buffer, int response_len) {
    err_t err = tcp_write(tpcb, buffer, (u16_t)response_len, 0);
    if (err != ERR_OK) {
        return err;
    }

    return tcp_output(tpcb);
}

static int generate_text_response(char* buffer, int max_len, int status_code, const char* status_text, const char* body) {
    return snprintf(buffer, max_len,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: text/plain\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s\r\n",
                    status_code, status_text, body);
}

static int generate_sse_headers(char* buffer, int max_len) {
    return snprintf(buffer, max_len,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "retry: 1000\n\n");
}

static int generate_sse_frame(char* buffer, int max_len) {
    web_server_snapshot_t snap = {0};

    web_server_get_snapshot(&snap);

    return snprintf(buffer, max_len,
                    "data: %s,%d,%d,%d,%d,%lu,%.1f,%.1f,%.1f,%d,%s,%d,%lu,%u,%u\n\n",
                    web_server_state_text(snap.state),
                    snap.failsafe ? 1 : 0,
                    snap.left,
                    snap.right,
                    snap.weapon,
                    (unsigned long)snap.age_ms,
                    snap.roll,
                    snap.pitch,
                    snap.yaw,
                    snap.test_active ? 1 : 0,
                    snap.test_motor,
                    snap.test_power,
                    (unsigned long)snap.test_remaining_ms,
                    snap.temp_c,
                    snap.humidity);
}

static int generate_404(char* buffer, int max_len) {
    return snprintf(buffer, max_len,
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/html\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "<html><body><h1>404 Not Found</h1></body></html>");
}

static int generate_503(char* buffer, int max_len) {
    return snprintf(buffer, max_len,
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Type: text/plain\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Too many SSE clients\r\n");
}

static bool extract_request_target(const char* request, char* target, size_t target_size) {
    const char* first_space = strchr(request, ' ');
    if (first_space == NULL) {
        return false;
    }

    const char* target_start = first_space + 1;
    const char* target_end = strchr(target_start, ' ');
    if (target_end == NULL || target_end <= target_start) {
        return false;
    }

    size_t len = (size_t)(target_end - target_start);
    if (len >= target_size) {
        len = target_size - 1;
    }

    memcpy(target, target_start, len);
    target[len] = '\0';
    return true;
}

static bool extract_query_value(const char* query, const char* key, char* out, size_t out_size) {
    if (query == NULL || *query == '\0') {
        return false;
    }

    const size_t key_len = strlen(key);
    const char* pos = query;
    while (pos != NULL && *pos != '\0') {
        if ((pos == query || pos[-1] == '&') && strncmp(pos, key, key_len) == 0 && pos[key_len] == '=') {
            const char* value = pos + key_len + 1;
            const char* end = value;
            while (*end != '\0' && *end != '&') {
                ++end;
            }

            size_t len = (size_t)(end - value);
            if (len >= out_size) {
                len = out_size - 1;
            }

            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }

        pos = strchr(pos, '&');
        if (pos != NULL) {
            ++pos;
        }
    }

    return false;
}

static int handle_test_start(const char* query, char* buffer, int max_len) {
    char motor_value[16];
    char power_value[16];
    char duration_value[16];

    if (!extract_query_value(query, "motor", motor_value, sizeof(motor_value)) ||
        !extract_query_value(query, "power", power_value, sizeof(power_value)) ||
        !extract_query_value(query, "duration", duration_value, sizeof(duration_value))) {
        return generate_text_response(buffer, max_len, 400, "Bad Request", "Missing motor, power, or duration.");
    }

    char* power_end = NULL;
    long power = strtol(power_value, &power_end, 10);
    if (power_end == power_value || *power_end != '\0' || power < 0 || power > 100) {
        return generate_text_response(buffer, max_len, 400, "Bad Request", "Power must be 0..100.");
    }

    char* duration_end = NULL;
    float duration_s = strtof(duration_value, &duration_end);
    if (duration_end == duration_value || *duration_end != '\0' || duration_s < 0.0f) {
        return generate_text_response(buffer, max_len, 400, "Bad Request", "Duration must be >= 0 seconds.");
    }

    const uint32_t duration_ms = duration_s > 0.0f ? (uint32_t)(duration_s * 1000.0f) : 0;
    if (!web_motor_test_start(motor_value, (int)power, duration_ms)) {
        return generate_text_response(buffer, max_len, 400, "Bad Request", "Invalid motor.");
    }

    return generate_text_response(buffer, max_len, 200, "OK",
                                  duration_ms > 0 ? "Motor test started." : "Motor test started; waiting for manual stop.");
}

static int handle_test_stop(char* buffer, int max_len) {
    const bool was_active = web_motor_test_is_active();
    web_motor_test_stop("web stop request");

    return generate_text_response(buffer, max_len, 200, "OK",
                                  was_active ? "Motor test stopped." : "No active motor test; motors stopped.");
}

static int handle_state_toggle(char* buffer, int max_len) {
    if (g_motors_ctrl->state == initializing) {
        printf("Web Server: state toggle rejected while controller is initializing\n");
        return generate_text_response(buffer, max_len, 409, "Conflict", "Controller is still initializing.");
    }

    if (g_motors_ctrl->state == active && web_motor_test_is_active()) {
        web_motor_test_stop("web e-stop");
    }

    if (motor_controller_toggle_state(g_motors_ctrl) == stopped) {
        printf("Web Server: emergency stop engaged from web request\n");
        return generate_text_response(buffer, max_len, 200, "OK", "Emergency stop engaged.");
    }

    printf("Web Server: controller input resumed from web request\n");
    return generate_text_response(buffer, max_len, 200, "OK", "Controller input resumed.");
}

static sse_client_t* allocate_sse_client(struct tcp_pcb* tpcb) {
    for (size_t i = 0; i < WEB_SERVER_MAX_SSE_CLIENTS; ++i) {
        if (g_sse_clients[i].pcb == NULL) {
            g_sse_clients[i].pcb = tpcb;
            return &g_sse_clients[i];
        }
    }

    return NULL;
}

static void release_sse_client(sse_client_t* client) {
    if (client != NULL) {
        memset(client, 0, sizeof(*client));
    }
}

static void drop_sse_client(size_t index, const char* reason, uint32_t age_ms) {
    struct tcp_pcb* pcb = g_sse_clients[index].pcb;
    if (pcb == NULL) {
        return;
    }

    printf("Web Server: dropping SSE client (%s after %lu ms)\n",
           reason,
           (unsigned long)age_ms);
    release_sse_client(&g_sse_clients[index]);
    http_abort(pcb);
}

static err_t http_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    sse_client_t* sse_client = (sse_client_t*)arg;

    (void)err;

    if (p == NULL) {
        if (sse_client != NULL) {
            printf("Web Server: SSE client slot=%d disconnected\n", sse_client_index(sse_client));
        } else {
            printf("Web Server: HTTP client disconnected\n");
        }
        release_sse_client(sse_client);
        http_close(tpcb);
        return ERR_OK;
    }

    char request[WEB_SERVER_REQUEST_BUFFER_SIZE];
    char target[WEB_SERVER_REQUEST_BUFFER_SIZE];
    const u16_t copy_len = p->tot_len < (WEB_SERVER_REQUEST_BUFFER_SIZE - 1) ? p->tot_len : (WEB_SERVER_REQUEST_BUFFER_SIZE - 1);
    pbuf_copy_partial(p, request, copy_len, 0);
    request[copy_len] = '\0';

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    if (sse_client != NULL) {
        return ERR_OK;
    }

    if (!extract_request_target(request, target, sizeof(target))) {
        const int response_len = generate_text_response(g_page_buffer, sizeof(g_page_buffer), 400, "Bad Request", "Malformed request.");
        tcp_write_response(tpcb, g_page_buffer, response_len, sizeof(g_page_buffer));
        http_close(tpcb);
        return ERR_OK;
    }

    char* query = strchr(target, '?');
    if (query != NULL) {
        *query++ = '\0';
    }

    if (strcmp(target, "/events") == 0) {
        sse_client = allocate_sse_client(tpcb);
        if (sse_client == NULL) {
            printf("Web Server: rejected SSE client, all %d slots are busy\n", WEB_SERVER_MAX_SSE_CLIENTS);
            const int response_len = generate_503(g_page_buffer, sizeof(g_page_buffer));
            tcp_write_response(tpcb, g_page_buffer, response_len, sizeof(g_page_buffer));
            http_close(tpcb);
            return ERR_OK;
        }

        tcp_arg(tpcb, sse_client);
        tcp_nagle_disable(tpcb);

        int response_len = generate_sse_headers(g_page_buffer, sizeof(g_page_buffer));
        err_t write_err = tcp_write_response(tpcb, g_page_buffer, response_len, sizeof(g_page_buffer));
        if (write_err != ERR_OK) {
            release_sse_client(sse_client);
            http_close(tpcb);
            return write_err;
        }
        sse_client->backpressure_since_ms = 0;

        response_len = generate_sse_frame(g_event_buffer, sizeof(g_event_buffer));
        write_err = tcp_write_response(tpcb, g_event_buffer, response_len, sizeof(g_event_buffer));
        if (write_err != ERR_OK && write_err != ERR_MEM) {
            release_sse_client(sse_client);
            http_close(tpcb);
            return write_err;
        }
        if (write_err == ERR_OK) {
            sse_client->backpressure_since_ms = 0;
        }

        printf("Web Server: SSE client connected slot=%d\n", sse_client_index(sse_client));

        return ERR_OK;
    }

    int response_len = 0;
    if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
        printf("Web Server: serving dashboard page\n");
        err_t write_err = tcp_write_const_response(tpcb, web_dashboard_page_data(), (int)web_dashboard_page_size());
        if (write_err != ERR_OK) {
            printf("Web Server: tcp_write failed with error %d\n", write_err);
        }
        http_close(tpcb);
        return ERR_OK;
    }

    if (strcmp(target, "/api/test/start") == 0) {
        response_len = handle_test_start(query, g_page_buffer, sizeof(g_page_buffer));
    } else if (strcmp(target, "/api/test/stop") == 0) {
        response_len = handle_test_stop(g_page_buffer, sizeof(g_page_buffer));
    } else if (strcmp(target, "/api/estop/toggle") == 0 || strcmp(target, "/api/state/toggle") == 0) {
        response_len = handle_state_toggle(g_page_buffer, sizeof(g_page_buffer));
    } else {
        printf("Web Server: 404 for path '%s'\n", target);
        response_len = generate_404(g_page_buffer, sizeof(g_page_buffer));
    }

    err_t write_err = tcp_write_response(tpcb, g_page_buffer, response_len, sizeof(g_page_buffer));
    if (write_err != ERR_OK) {
        printf("Web Server: tcp_write failed with error %d\n", write_err);
    }
    http_close(tpcb);

    return ERR_OK;
}

static void http_err(void* arg, err_t err) {
    if (arg != NULL) {
        printf("Web Server: SSE client slot=%d errored with %d\n", sse_client_index((const sse_client_t*)arg), err);
    } else {
        printf("Web Server: HTTP connection errored with %d\n", err);
    }
    release_sse_client((sse_client_t*)arg);
}

static err_t http_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_err);

    return ERR_OK;
}

static void http_close(struct tcp_pcb* tpcb) {
    if (tpcb == NULL) {
        return;
    }

    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    if (tcp_close(tpcb) != ERR_OK) {
        printf("Web Server: tcp_close failed\n");
    }
}

static void http_abort(struct tcp_pcb* tpcb) {
    if (tpcb == NULL) {
        return;
    }

    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_abort(tpcb);
}

static void sse_broadcast_timer(btstack_timer_source_t* ts) {
    if (!g_running) {
        return;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (g_last_sse_tick_ms != 0) {
        const uint32_t delta_ms = now_ms - g_last_sse_tick_ms;
        if (delta_ms > WEB_SERVER_SSE_DELAY_LOG_THRESHOLD_MS) {
            printf("Web Server: SSE timer delayed to %lu ms\n", (unsigned long)delta_ms);
        }
    }
    g_last_sse_tick_ms = now_ms;

    web_motor_test_service(now_ms);

    const int frame_len = generate_sse_frame(g_event_buffer, sizeof(g_event_buffer));

    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < WEB_SERVER_MAX_SSE_CLIENTS; ++i) {
        struct tcp_pcb* pcb = g_sse_clients[i].pcb;
        if (pcb == NULL) {
            continue;
        }

        err_t write_err = tcp_write_response(pcb, g_event_buffer, frame_len, sizeof(g_event_buffer));
        if (write_err == ERR_MEM) {
            if (g_sse_clients[i].backpressure_since_ms == 0) {
                g_sse_clients[i].backpressure_since_ms = now_ms;
            }

            if ((now_ms - g_last_sse_backpressure_log_ms) >= WEB_SERVER_SSE_BACKPRESSURE_LOG_INTERVAL_MS) {
                printf("Web Server: SSE backpressure sndbuf=%u\n", tcp_sndbuf(pcb));
                g_last_sse_backpressure_log_ms = now_ms;
            }

            if ((now_ms - g_sse_clients[i].backpressure_since_ms) >= WEB_SERVER_SSE_STALL_TIMEOUT_MS) {
                drop_sse_client(i, "stalled backpressure", now_ms - g_sse_clients[i].backpressure_since_ms);
            }
            continue;
        }

        if (write_err != ERR_OK) {
            release_sse_client(&g_sse_clients[i]);
            http_close(pcb);
            continue;
        }

        g_sse_clients[i].backpressure_since_ms = 0;
    }
    cyw43_arch_lwip_end();

    btstack_run_loop_set_timer(ts, WEB_SERVER_SSE_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

bool web_server_init(motor_controller_t* motors) {
    g_motors_ctrl = motors;
    memset(g_sse_clients, 0, sizeof(g_sse_clients));
    g_last_sse_tick_ms = 0;
    g_last_sse_backpressure_log_ms = 0;
    web_motor_test_init(motors);

    printf("Starting web server on port %d...\n", WEB_SERVER_PORT);

    cyw43_arch_lwip_begin();
    g_server_pcb = tcp_new();
    if (!g_server_pcb) {
        cyw43_arch_lwip_end();
        printf("Failed to create TCP PCB\n");
        return false;
    }

    err_t err = tcp_bind(g_server_pcb, IP_ADDR_ANY, WEB_SERVER_PORT);
    if (err != ERR_OK) {
        printf("Failed to bind to port %d\n", WEB_SERVER_PORT);
        tcp_close(g_server_pcb);
        g_server_pcb = NULL;
        cyw43_arch_lwip_end();
        return false;
    }

    g_server_pcb = tcp_listen(g_server_pcb);
    if (!g_server_pcb) {
        cyw43_arch_lwip_end();
        printf("Failed to listen\n");
        return false;
    }

    tcp_accept(g_server_pcb, http_accept);
    cyw43_arch_lwip_end();

    g_running = true;
    btstack_run_loop_set_timer_handler(&g_sse_timer, sse_broadcast_timer);
    btstack_run_loop_set_timer(&g_sse_timer, WEB_SERVER_SSE_INTERVAL_MS);
    btstack_run_loop_add_timer(&g_sse_timer);
    printf("Web server ready at http://%s/\n", wifi_sta_get_ip());

    return true;
}

void web_server_poll(void) {
    // Requests are handled via lwIP callbacks and the SSE stream is driven by btstack timers.
}

void web_server_stop(void) {
    btstack_run_loop_remove_timer(&g_sse_timer);

    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < WEB_SERVER_MAX_SSE_CLIENTS; ++i) {
        if (g_sse_clients[i].pcb != NULL) {
            struct tcp_pcb* pcb = g_sse_clients[i].pcb;
            release_sse_client(&g_sse_clients[i]);
            http_close(pcb);
        }
    }

    if (g_server_pcb != NULL) {
        http_close(g_server_pcb);
        g_server_pcb = NULL;
    }
    cyw43_arch_lwip_end();

    g_running = false;
    g_last_sse_tick_ms = 0;
    g_last_sse_backpressure_log_ms = 0;
    web_motor_test_deinit();
    printf("Web server stopped\n");
}

bool web_server_is_running(void) {
    return g_running;
}

bool web_server_interrupt_test_run(void) {
    return web_motor_test_interrupt();
}
