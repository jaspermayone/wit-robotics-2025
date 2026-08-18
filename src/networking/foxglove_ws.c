/**
 * @file foxglove_ws.c
 * @brief Foxglove WebSocket server running on the bot.
 *
 * This module implements enough of the Foxglove WebSocket protocol
 * (subprotocol `foxglove.sdk.v1`) for the Foxglove app to connect straight to
 * the bot. There is no host-side bridge.
 *
 * The Foxglove SDK cannot run here. Its core is a prebuilt Rust library that
 * needs a full operating system. So this file speaks the wire protocol
 * directly on top of lwIP raw TCP.
 *
 * All messages use the `json` encoding and `jsonschema` schemas. That removes
 * the need for a protobuf runtime on the microcontroller.
 */

#include "foxglove_ws.h"

#include "config.h"
#include "web_motor_test.h"
#include "web_server.h"
#include "wifi_sta.h"

#include <btstack_run_loop.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

// =============================================================================
// WebSocket constants
// =============================================================================

#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

/** Bytes kept free in front of every payload, for the WebSocket frame header. */
#define WS_HEADER_RESERVE 4

/** The GUID that RFC 6455 mixes into the handshake key. */
#define WS_HANDSHAKE_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/** Foxglove client-to-server binary opcode for a published message. */
#define FG_CLIENT_OP_MESSAGE_DATA 0x01

/** Foxglove server-to-client binary opcode for a channel message. */
#define FG_SERVER_OP_MESSAGE_DATA 0x01

// =============================================================================
// SHA-1, needed by the WebSocket handshake
// =============================================================================

typedef struct {
    uint32_t state[5];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} sha1_ctx_t;

static uint32_t sha1_rotl(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_compress(sha1_ctx_t* ctx, const uint8_t* block) {
    uint32_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f;
        uint32_t k;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        const uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(sha1_ctx_t* ctx) {
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

static void sha1_update(sha1_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;

    ctx->bit_count += (uint64_t)len * 8u;
    while (len > 0) {
        const size_t space = 64u - ctx->block_len;
        const size_t take = len < space ? len : space;

        memcpy(ctx->block + ctx->block_len, bytes, take);
        ctx->block_len += take;
        bytes += take;
        len -= take;

        if (ctx->block_len == 64u) {
            sha1_compress(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t* ctx, uint8_t digest[20]) {
    const uint64_t bit_count = ctx->bit_count;
    const uint8_t pad = 0x80;

    sha1_update(ctx, &pad, 1);
    while (ctx->block_len != 56u) {
        const uint8_t zero = 0x00;
        sha1_update(ctx, &zero, 1);
    }

    uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] = (uint8_t)(bit_count >> (56 - i * 8));
    }
    sha1_update(ctx, length_bytes, sizeof(length_bytes));

    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

// =============================================================================
// Base64
// =============================================================================

static void base64_encode(const uint8_t* data, size_t len, char* out, size_t out_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t written = 0;

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0u;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0u;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        const char chunk[4] = {
            alphabet[(triple >> 18) & 0x3F],
            alphabet[(triple >> 12) & 0x3F],
            (i + 1 < len) ? alphabet[(triple >> 6) & 0x3F] : '=',
            (i + 2 < len) ? alphabet[triple & 0x3F] : '=',
        };

        for (int j = 0; j < 4 && written + 1 < out_size; ++j) {
            out[written++] = chunk[j];
        }
    }

    out[written < out_size ? written : out_size - 1] = '\0';
}

// =============================================================================
// Bounded string builder
// =============================================================================

typedef struct {
    char* buf;
    size_t cap;
    size_t len;
    bool overflow;
} strbuf_t;

static void sb_init(strbuf_t* sb, char* buf, size_t cap) {
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = false;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void sb_puts(strbuf_t* sb, const char* text) {
    if (sb->overflow) {
        return;
    }

    const size_t len = strlen(text);
    if (sb->len + len + 1 > sb->cap) {
        sb->overflow = true;
        return;
    }

    memcpy(sb->buf + sb->len, text, len + 1);
    sb->len += len;
}

static void sb_printf(strbuf_t* sb, const char* fmt, ...) {
    if (sb->overflow) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= sb->cap - sb->len) {
        sb->overflow = true;
        return;
    }
    sb->len += (size_t)written;
}

/** Writes `text` as a quoted JSON string, with the characters JSON needs escaped. */
static void sb_put_json_string(strbuf_t* sb, const char* text) {
    if (sb->overflow) {
        return;
    }

    sb_puts(sb, "\"");
    for (const char* p = text; *p != '\0' && !sb->overflow; ++p) {
        switch (*p) {
            case '"': sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n"); break;
            case '\r': sb_puts(sb, "\\r"); break;
            case '\t': sb_puts(sb, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    sb_printf(sb, "\\u%04x", (unsigned)(unsigned char)*p);
                } else {
                    const char chunk[2] = {*p, '\0'};
                    sb_puts(sb, chunk);
                }
                break;
        }
    }
    sb_puts(sb, "\"");
}

// =============================================================================
// Small JSON reader
//
// The Foxglove app sends short, flat control messages. A full parser is not
// worth the flash, so these helpers scan for a key inside one object.
// =============================================================================

static const char* json_skip_space(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    return p;
}

/** Returns the value that follows `"key":` inside `obj`, or NULL. */
static const char* json_find_value(const char* obj, const char* key) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = obj;
    while ((pos = strstr(pos, pattern)) != NULL) {
        const char* after = json_skip_space(pos + strlen(pattern));
        if (*after == ':') {
            return json_skip_space(after + 1);
        }
        pos += strlen(pattern);
    }

    return NULL;
}

static bool json_get_number(const char* obj, const char* key, double* out) {
    const char* value = json_find_value(obj, key);
    if (value == NULL) {
        return false;
    }

    char* end = NULL;
    const double parsed = strtod(value, &end);
    if (end == value) {
        return false;
    }

    *out = parsed;
    return true;
}

static bool json_get_uint32(const char* obj, const char* key, uint32_t* out) {
    double value = 0.0;
    if (!json_get_number(obj, key, &value) || value < 0.0 || value > 4294967295.0) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static bool json_get_string(const char* obj, const char* key, char* out, size_t out_size) {
    const char* value = json_find_value(obj, key);
    if (value == NULL || *value != '"') {
        return false;
    }

    ++value;
    size_t written = 0;
    while (*value != '\0' && *value != '"') {
        if (*value == '\\' && value[1] != '\0') {
            ++value;
        }
        if (written + 1 < out_size) {
            out[written++] = *value;
        }
        ++value;
    }

    out[written] = '\0';
    return *value == '"';
}

/**
 * Copies the next `{...}` object at or after *cursor into `buf`, then moves
 * *cursor past it. Returns false at the end of the array.
 */
static bool json_next_object(const char** cursor, char* buf, size_t buf_size) {
    const char* p = *cursor;

    for (;;) {
        while (*p != '\0' && *p != '{' && *p != ']') {
            ++p;
        }
        if (*p != '{') {
            *cursor = p;
            return false;
        }

        const char* start = p;
        int depth = 0;
        bool in_string = false;
        for (; *p != '\0'; ++p) {
            if (in_string) {
                if (*p == '\\' && p[1] != '\0') {
                    ++p;
                } else if (*p == '"') {
                    in_string = false;
                }
                continue;
            }
            if (*p == '"') {
                in_string = true;
            } else if (*p == '{') {
                ++depth;
            } else if (*p == '}') {
                if (--depth == 0) {
                    ++p;
                    break;
                }
            }
        }
        if (depth != 0) {
            *cursor = p;
            return false;
        }

        const size_t len = (size_t)(p - start);
        *cursor = p;
        if (len + 1 <= buf_size) {
            memcpy(buf, start, len);
            buf[len] = '\0';
            return true;
        }
        // The object is too long to inspect. Skip it and try the next one.
    }
}

/** Reads an array of numbers, for example `"subscriptionIds":[1,2]`. */
static int json_get_uint32_array(const char* obj, const char* key, uint32_t* out, int max_count) {
    const char* value = json_find_value(obj, key);
    if (value == NULL || *value != '[') {
        return 0;
    }

    int count = 0;
    const char* p = value + 1;
    while (count < max_count) {
        p = json_skip_space(p);
        if (*p == ']' || *p == '\0') {
            break;
        }

        char* end = NULL;
        const double parsed = strtod(p, &end);
        if (end == p) {
            break;
        }
        if (parsed >= 0.0 && parsed <= 4294967295.0) {
            out[count++] = (uint32_t)parsed;
        }

        p = json_skip_space(end);
        if (*p == ',') {
            ++p;
        }
    }

    return count;
}

// =============================================================================
// Channels
//
// The schemas match `foxglove/tools/src/schemas.cpp`, so the layout in
// `foxglove/layouts/battlebot-dashboard.json` works with live data and with a
// recording made by the generator.
// =============================================================================

typedef enum {
    FG_CH_TF = 0,
    FG_CH_MOTORS,
    FG_CH_IMU,
    FG_CH_THERMAL,
    FG_CH_STATE,
    FG_CH_COUNT
} fg_channel_id_t;

typedef struct {
    const char* topic;
    const char* schema_name;
    const char* schema;
    uint8_t period;  /**< Publish on every Nth broadcast tick. */
} fg_channel_t;

static const char kSchemaFrameTransform[] =
    "{\"type\":\"object\",\"title\":\"foxglove.FrameTransform\",\"properties\":{"
    "\"timestamp\":{\"type\":\"object\",\"title\":\"time\",\"properties\":{"
    "\"sec\":{\"type\":\"integer\",\"minimum\":0},"
    "\"nsec\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":999999999}}},"
    "\"parent_frame_id\":{\"type\":\"string\"},"
    "\"child_frame_id\":{\"type\":\"string\"},"
    "\"translation\":{\"type\":\"object\",\"title\":\"foxglove.Vector3\",\"properties\":{"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},"
    "\"rotation\":{\"type\":\"object\",\"title\":\"foxglove.Quaternion\",\"properties\":{"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
    "\"z\":{\"type\":\"number\"},\"w\":{\"type\":\"number\"}}}}}";

static const char kSchemaMotors[] =
    "{\"type\":\"object\","
    "\"description\":\"Motor command values for the tank drive + weapon.\",\"properties\":{"
    "\"left\":{\"type\":\"number\",\"description\":\"Left drive motor (-100..100)\"},"
    "\"right\":{\"type\":\"number\",\"description\":\"Right drive motor (-100..100)\"},"
    "\"weapon\":{\"type\":\"number\",\"description\":\"Weapon spinner motor (0..100)\"}}}";

static const char kSchemaImu[] =
    "{\"type\":\"object\","
    "\"description\":\"IMU data matching sensor_msgs/Imu layout.\",\"properties\":{"
    "\"orientation\":{\"type\":\"object\",\"properties\":{"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
    "\"z\":{\"type\":\"number\"},\"w\":{\"type\":\"number\"}}},"
    "\"angular_velocity\":{\"type\":\"object\",\"properties\":{"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}},"
    "\"linear_acceleration\":{\"type\":\"object\",\"properties\":{"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}}}";

static const char kSchemaThermal[] =
    "{\"type\":\"object\","
    "\"description\":\"Internal temperature and humidity from the DHT11 sensor.\",\"properties\":{"
    "\"temperature_c\":{\"type\":\"number\",\"description\":\"Internal temperature (C)\"},"
    "\"humidity\":{\"type\":\"number\",\"description\":\"Humidity (%)\"}}}";

static const char kSchemaState[] =
    "{\"type\":\"object\","
    "\"description\":\"Controller state and safety flags. Drives Indicator panels.\",\"properties\":{"
    "\"controller\":{\"type\":\"string\",\"enum\":[\"ACTIVE\",\"INIT\",\"STOPPED\"]},"
    "\"failsafe\":{\"type\":\"boolean\"},"
    "\"command_age_ms\":{\"type\":\"number\","
    "\"description\":\"Time since last controller command (ms)\"}}}";

static const fg_channel_t kChannels[FG_CH_COUNT] = {
    {"/tf", "foxglove.FrameTransform", kSchemaFrameTransform, 1},
    {"/motors", "battlebot.Motors", kSchemaMotors, 1},
    {"/imu", "battlebot.Imu", kSchemaImu, 1},
    {"/thermal", "battlebot.Thermal", kSchemaThermal, 15},
    {"/state", "battlebot.State", kSchemaState, 5},
};

/** Channel ids on the wire start at 1. */
static uint32_t fg_channel_wire_id(int index) {
    return (uint32_t)index + 1u;
}

// =============================================================================
// Bench command topics a client may publish to
// =============================================================================

typedef enum {
    FG_CMD_NONE = 0,
    FG_CMD_ESTOP,
    FG_CMD_TEST_START,
    FG_CMD_TEST_STOP
} fg_command_t;

static fg_command_t fg_command_for_topic(const char* topic) {
    if (strcmp(topic, "/cmd/estop") == 0) {
        return FG_CMD_ESTOP;
    }
    if (strcmp(topic, "/cmd/test") == 0) {
        return FG_CMD_TEST_START;
    }
    if (strcmp(topic, "/cmd/test_stop") == 0) {
        return FG_CMD_TEST_STOP;
    }
    return FG_CMD_NONE;
}

// =============================================================================
// Client state
// =============================================================================

typedef struct {
    struct tcp_pcb* pcb;
    bool handshake_done;
    uint16_t rx_len;
    uint8_t rx[FOXGLOVE_WS_RX_BUFFER_SIZE + 1];
    bool subscribed[FG_CH_COUNT];
    uint32_t subscription_id[FG_CH_COUNT];
    uint32_t client_channel_id[FOXGLOVE_WS_MAX_CLIENT_CHANNELS];
    uint8_t client_channel_cmd[FOXGLOVE_WS_MAX_CLIENT_CHANNELS];
    uint8_t client_channel_count;
    uint32_t backpressure_since_ms;
} fg_client_t;

static struct tcp_pcb* g_listen_pcb = NULL;
static motor_controller_t* g_motors = NULL;
static bool g_running = false;
static fg_client_t g_clients[FOXGLOVE_WS_MAX_CLIENTS];
static btstack_timer_source_t g_broadcast_timer;
static uint32_t g_tick = 0;
static uint32_t g_session_id = 0;

/** Scratch for the handshake reply and the channel advertisement. */
static char g_tx[WS_HEADER_RESERVE + FOXGLOVE_WS_TX_BUFFER_SIZE];

/** Scratch for one telemetry message, including its binary header. */
static uint8_t g_msg[WS_HEADER_RESERVE + FOXGLOVE_WS_MSG_BUFFER_SIZE];

static void fg_close(struct tcp_pcb* pcb);
static void fg_abort(struct tcp_pcb* pcb);
static void fg_drop_client(fg_client_t* client, const char* reason);

static int fg_client_index(const fg_client_t* client) {
    return client == NULL ? -1 : (int)(client - g_clients);
}

// =============================================================================
// Frame output
//
// Server frames are never masked, so the header is at most 4 bytes. Every
// caller builds its payload at `area + WS_HEADER_RESERVE`, which leaves room
// for the header in the same buffer and keeps the send to one tcp_write.
// =============================================================================

static err_t ws_send_frame(fg_client_t* client, uint8_t opcode, uint8_t* area, size_t payload_len) {
    if (client->pcb == NULL || payload_len > 0xFFFFu) {
        return ERR_VAL;
    }

    uint8_t* payload = area + WS_HEADER_RESERVE;
    const size_t header_len = payload_len < 126u ? 2u : 4u;
    uint8_t* start = payload - header_len;

    start[0] = (uint8_t)(0x80u | opcode);
    if (header_len == 2u) {
        start[1] = (uint8_t)payload_len;
    } else {
        start[1] = 126u;
        start[2] = (uint8_t)(payload_len >> 8);
        start[3] = (uint8_t)payload_len;
    }

    const size_t total = header_len + payload_len;
    if (tcp_sndbuf(client->pcb) < total) {
        return ERR_MEM;
    }

    const err_t err = tcp_write(client->pcb, start, (u16_t)total, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        return err;
    }

    return tcp_output(client->pcb);
}

// =============================================================================
// Handshake
// =============================================================================

/** Compares `n` characters without regard to letter case. */
static bool ascii_prefix_equals(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? (char)(b[i] + 32) : b[i];

        if (ca != cb || ca == '\0') {
            return ca == cb;
        }
    }

    return true;
}

/** Finds a header value in an HTTP request. The header name match ignores case. */
static bool http_header_value(const char* request, const char* name, char* out, size_t out_size) {
    const size_t name_len = strlen(name);

    for (const char* line = request; line != NULL && *line != '\0';) {
        if (ascii_prefix_equals(line, name, name_len)) {
            const char* value = json_skip_space(line + name_len);
            if (*value == ':') {
                value = json_skip_space(value + 1);

                size_t written = 0;
                while (*value != '\0' && *value != '\r' && *value != '\n') {
                    if (written + 1 < out_size) {
                        out[written++] = *value;
                    }
                    ++value;
                }
                out[written] = '\0';
                return written > 0;
            }
        }

        line = strstr(line, "\r\n");
        if (line != NULL) {
            line += 2;
        }
    }

    return false;
}

/** Builds the RFC 6455 accept token from the client key. */
static void ws_accept_token(const char* client_key, char* out, size_t out_size) {
    sha1_ctx_t ctx;
    uint8_t digest[20];

    sha1_init(&ctx);
    sha1_update(&ctx, client_key, strlen(client_key));
    sha1_update(&ctx, WS_HANDSHAKE_GUID, strlen(WS_HANDSHAKE_GUID));
    sha1_final(&ctx, digest);

    base64_encode(digest, sizeof(digest), out, out_size);
}

static err_t ws_send_raw(struct tcp_pcb* pcb, const char* text) {
    const err_t err = tcp_write(pcb, text, (u16_t)strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        return err;
    }
    return tcp_output(pcb);
}

/** Sends the serverInfo message, then the channel list. */
static bool fg_send_greeting(fg_client_t* client) {
    strbuf_t sb;

    sb_init(&sb, g_tx + WS_HEADER_RESERVE, sizeof(g_tx) - WS_HEADER_RESERVE);
    sb_puts(&sb, "{\"op\":\"serverInfo\",\"name\":");
    sb_put_json_string(&sb, ROBOT_NAME);
    sb_puts(&sb, ",\"capabilities\":[\"clientPublish\"],\"supportedEncodings\":[\"json\"]"
                 ",\"metadata\":{},\"sessionId\":");
    sb_printf(&sb, "\"%lu\"}", (unsigned long)g_session_id);
    if (sb.overflow || ws_send_frame(client, WS_OP_TEXT, (uint8_t*)g_tx, sb.len) != ERR_OK) {
        return false;
    }

    sb_init(&sb, g_tx + WS_HEADER_RESERVE, sizeof(g_tx) - WS_HEADER_RESERVE);
    sb_puts(&sb, "{\"op\":\"advertise\",\"channels\":[");
    for (int i = 0; i < FG_CH_COUNT; ++i) {
        sb_printf(&sb, "%s{\"id\":%lu,\"topic\":", i == 0 ? "" : ",",
                  (unsigned long)fg_channel_wire_id(i));
        sb_put_json_string(&sb, kChannels[i].topic);
        sb_puts(&sb, ",\"encoding\":\"json\",\"schemaName\":");
        sb_put_json_string(&sb, kChannels[i].schema_name);
        sb_puts(&sb, ",\"schemaEncoding\":\"jsonschema\",\"schema\":");
        sb_put_json_string(&sb, kChannels[i].schema);
        sb_puts(&sb, "}");
    }
    sb_puts(&sb, "]}");
    if (sb.overflow) {
        printf("Foxglove: channel list does not fit in %d bytes\n", FOXGLOVE_WS_TX_BUFFER_SIZE);
        return false;
    }

    return ws_send_frame(client, WS_OP_TEXT, (uint8_t*)g_tx, sb.len) == ERR_OK;
}

/** Completes the HTTP upgrade. Returns false if the client must be dropped. */
static bool fg_do_handshake(fg_client_t* client, const char* request) {
    char key[64];
    char accept[32];

    if (strncmp(request, "GET ", 4) != 0) {
        ws_send_raw(client->pcb, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        return false;
    }

    if (!http_header_value(request, "Sec-WebSocket-Key", key, sizeof(key))) {
        ws_send_raw(client->pcb,
                    "HTTP/1.1 426 Upgrade Required\r\n"
                    "Connection: close\r\n"
                    "Content-Type: text/plain\r\n\r\n"
                    "This port serves the Foxglove WebSocket protocol.\r\n");
        return false;
    }

    ws_accept_token(key, accept, sizeof(accept));

    strbuf_t sb;
    sb_init(&sb, g_tx, sizeof(g_tx));
    sb_puts(&sb, "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Protocol: foxglove.sdk.v1\r\n"
                 "Sec-WebSocket-Accept: ");
    sb_puts(&sb, accept);
    sb_puts(&sb, "\r\n\r\n");
    if (sb.overflow || ws_send_raw(client->pcb, g_tx) != ERR_OK) {
        return false;
    }

    client->handshake_done = true;
    tcp_nagle_disable(client->pcb);
    printf("Foxglove: client connected slot=%d\n", fg_client_index(client));

    return fg_send_greeting(client);
}

// =============================================================================
// Control messages from the client
// =============================================================================

static void fg_handle_subscribe(fg_client_t* client, const char* text) {
    const char* list = json_find_value(text, "subscriptions");
    if (list == NULL) {
        return;
    }

    char object[192];
    const char* cursor = list;
    while (json_next_object(&cursor, object, sizeof(object))) {
        uint32_t subscription_id = 0;
        uint32_t channel_id = 0;

        if (!json_get_uint32(object, "id", &subscription_id) ||
            !json_get_uint32(object, "channelId", &channel_id)) {
            continue;
        }
        if (channel_id < 1u || channel_id > (uint32_t)FG_CH_COUNT) {
            continue;
        }

        const int index = (int)channel_id - 1;
        client->subscribed[index] = true;
        client->subscription_id[index] = subscription_id;
    }
}

static void fg_handle_unsubscribe(fg_client_t* client, const char* text) {
    uint32_t ids[FG_CH_COUNT];
    const int count = json_get_uint32_array(text, "subscriptionIds", ids, FG_CH_COUNT);

    for (int i = 0; i < count; ++i) {
        for (int ch = 0; ch < FG_CH_COUNT; ++ch) {
            if (client->subscribed[ch] && client->subscription_id[ch] == ids[i]) {
                client->subscribed[ch] = false;
            }
        }
    }
}

static void fg_handle_client_advertise(fg_client_t* client, const char* text) {
    const char* list = json_find_value(text, "channels");
    if (list == NULL) {
        return;
    }

    char object[192];
    char topic[48];
    const char* cursor = list;
    while (json_next_object(&cursor, object, sizeof(object))) {
        uint32_t channel_id = 0;
        if (!json_get_uint32(object, "id", &channel_id) ||
            !json_get_string(object, "topic", topic, sizeof(topic))) {
            continue;
        }

        const fg_command_t command = fg_command_for_topic(topic);
        if (command == FG_CMD_NONE) {
            printf("Foxglove: ignoring client topic '%s'\n", topic);
            continue;
        }
        if (client->client_channel_count >= FOXGLOVE_WS_MAX_CLIENT_CHANNELS) {
            printf("Foxglove: too many client topics, ignoring '%s'\n", topic);
            continue;
        }

        client->client_channel_id[client->client_channel_count] = channel_id;
        client->client_channel_cmd[client->client_channel_count] = (uint8_t)command;
        ++client->client_channel_count;
    }
}

static void fg_handle_client_unadvertise(fg_client_t* client, const char* text) {
    uint32_t ids[FOXGLOVE_WS_MAX_CLIENT_CHANNELS];
    const int count = json_get_uint32_array(text, "channelIds", ids, FOXGLOVE_WS_MAX_CLIENT_CHANNELS);

    for (int i = 0; i < count; ++i) {
        for (uint8_t slot = 0; slot < client->client_channel_count;) {
            if (client->client_channel_id[slot] != ids[i]) {
                ++slot;
                continue;
            }

            const uint8_t last = (uint8_t)(client->client_channel_count - 1u);
            client->client_channel_id[slot] = client->client_channel_id[last];
            client->client_channel_cmd[slot] = client->client_channel_cmd[last];
            client->client_channel_count = last;
        }
    }
}

// =============================================================================
// Bench commands
//
// These run the same code paths as the HTTP endpoints in web_server.c. They
// are for bench testing only and must never be treated as a real e-stop.
// =============================================================================

static void fg_run_estop(void) {
    if (g_motors->state == initializing) {
        printf("Foxglove: state toggle rejected while controller is initializing\n");
        return;
    }

    if (g_motors->state == active && web_motor_test_is_active()) {
        web_motor_test_stop("foxglove e-stop");
    }

    if (motor_controller_toggle_state(g_motors) == stopped) {
        printf("Foxglove: emergency stop engaged from a publish\n");
    } else {
        printf("Foxglove: controller input resumed from a publish\n");
    }
}

static void fg_run_test_start(const char* payload) {
    char motor[16] = "";
    double power = 0.0;
    double duration_s = 0.0;

    if (!json_get_string(payload, "motor", motor, sizeof(motor))) {
        printf("Foxglove: motor test publish has no 'motor' field\n");
        return;
    }
    if (!json_get_number(payload, "power", &power) || power < 0.0 || power > 100.0) {
        printf("Foxglove: motor test publish needs power 0..100\n");
        return;
    }
    if (!json_get_number(payload, "duration", &duration_s) || duration_s < 0.0) {
        duration_s = 0.0;
    }

    const uint32_t duration_ms = duration_s > 0.0 ? (uint32_t)(duration_s * 1000.0) : 0u;
    if (!web_motor_test_start(motor, (int)power, duration_ms)) {
        printf("Foxglove: motor test publish named an unknown motor '%s'\n", motor);
    }
}

static void fg_handle_client_publish(fg_client_t* client, const uint8_t* data, size_t len) {
    if (len < 5u) {
        return;
    }

    const uint32_t channel_id = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                                ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
    fg_command_t command = FG_CMD_NONE;

    for (uint8_t slot = 0; slot < client->client_channel_count; ++slot) {
        if (client->client_channel_id[slot] == channel_id) {
            command = (fg_command_t)client->client_channel_cmd[slot];
            break;
        }
    }

    if (command == FG_CMD_NONE) {
        return;
    }

    // The payload is JSON and the frame parser already ended it with a NUL.
    const char* payload = (const char*)(data + 5);

    switch (command) {
        case FG_CMD_ESTOP:
            fg_run_estop();
            break;
        case FG_CMD_TEST_START:
            fg_run_test_start(payload);
            break;
        case FG_CMD_TEST_STOP:
            web_motor_test_stop("foxglove stop request");
            printf("Foxglove: motor test stopped from a publish\n");
            break;
        default:
            break;
    }
}

// =============================================================================
// Frame input
// =============================================================================

static void fg_handle_text(fg_client_t* client, const char* text) {
    char op[24];

    if (!json_get_string(text, "op", op, sizeof(op))) {
        return;
    }

    if (strcmp(op, "subscribe") == 0) {
        fg_handle_subscribe(client, text);
    } else if (strcmp(op, "unsubscribe") == 0) {
        fg_handle_unsubscribe(client, text);
    } else if (strcmp(op, "advertise") == 0) {
        fg_handle_client_advertise(client, text);
    } else if (strcmp(op, "unadvertise") == 0) {
        fg_handle_client_unadvertise(client, text);
    }
}

/**
 * Consumes every complete frame in the receive buffer.
 *
 * Client frames are always masked, so the payload is unmasked in place. Any
 * frame that cannot fit in the receive buffer drops the client, because it
 * could never be completed.
 */
static void fg_process_frames(fg_client_t* client) {
    size_t offset = 0;

    while (client->rx_len - offset >= 2u) {
        uint8_t* frame = client->rx + offset;
        const size_t available = client->rx_len - offset;
        const uint8_t opcode = frame[0] & 0x0Fu;
        const bool masked = (frame[1] & 0x80u) != 0u;
        size_t payload_len = frame[1] & 0x7Fu;
        size_t header_len = 2u;

        if (payload_len == 126u) {
            if (available < 4u) {
                break;
            }
            payload_len = ((size_t)frame[2] << 8) | (size_t)frame[3];
            header_len = 4u;
        } else if (payload_len == 127u) {
            fg_drop_client(client, "frame is too long");
            return;
        }

        const size_t mask_len = masked ? 4u : 0u;
        const size_t frame_len = header_len + mask_len + payload_len;
        if (frame_len > FOXGLOVE_WS_RX_BUFFER_SIZE) {
            fg_drop_client(client, "frame does not fit the receive buffer");
            return;
        }
        if (available < frame_len) {
            break;
        }

        uint8_t* mask = masked ? frame + header_len : NULL;
        uint8_t* payload = frame + header_len + mask_len;
        if (mask != NULL) {
            for (size_t i = 0; i < payload_len; ++i) {
                payload[i] ^= mask[i & 3u];
            }
        }

        // Terminate the payload so the JSON helpers can treat it as a string.
        // The byte belongs to the next frame, so put it back afterwards.
        const uint8_t saved = payload[payload_len];
        payload[payload_len] = '\0';

        switch (opcode) {
            case WS_OP_TEXT:
                fg_handle_text(client, (const char*)payload);
                break;
            case WS_OP_BINARY:
                if (payload_len >= 1u && payload[0] == FG_CLIENT_OP_MESSAGE_DATA) {
                    fg_handle_client_publish(client, payload, payload_len);
                }
                break;
            case WS_OP_PING:
                if (payload_len + WS_HEADER_RESERVE <= sizeof(g_msg)) {
                    memcpy(g_msg + WS_HEADER_RESERVE, payload, payload_len);
                    ws_send_frame(client, WS_OP_PONG, g_msg, payload_len);
                }
                break;
            case WS_OP_PONG:
                break;
            case WS_OP_CLOSE:
                // RFC 6455 asks for a close frame in reply. Echo the status
                // code back, then drop the client.
                if (payload_len >= 2) {
                    memcpy(g_msg + WS_HEADER_RESERVE, payload, 2);
                    ws_send_frame(client, WS_OP_CLOSE, g_msg, 2);
                } else {
                    ws_send_frame(client, WS_OP_CLOSE, g_msg, 0);
                }
                fg_drop_client(client, "client closed the connection");
                return;
            default:
                break;
        }

        if (client->pcb == NULL) {
            return;  // A handler dropped the client.
        }

        payload[payload_len] = saved;
        offset += frame_len;
    }

    if (offset > 0u) {
        client->rx_len = (uint16_t)(client->rx_len - offset);
        memmove(client->rx, client->rx + offset, client->rx_len);
    }
}

// =============================================================================
// Telemetry
// =============================================================================

#define FG_PI 3.14159265358979323846
#define FG_GRAVITY 9.80665

typedef struct {
    double x;
    double y;
    double z;
    double w;
} fg_quaternion_t;

/**
 * Converts Euler angles in degrees to a quaternion, in the XYZ order that the
 * Foxglove `.@rpy` message path function expects. This matches
 * `foxglove/tools/src/telemetry.cpp`.
 */
static fg_quaternion_t fg_euler_to_quaternion(double roll_deg, double pitch_deg, double yaw_deg) {
    const double roll = roll_deg * FG_PI / 180.0;
    const double pitch = pitch_deg * FG_PI / 180.0;
    const double yaw = yaw_deg * FG_PI / 180.0;

    const double cr = cos(roll * 0.5);
    const double sr = sin(roll * 0.5);
    const double cp = cos(pitch * 0.5);
    const double sp = sin(pitch * 0.5);
    const double cy = cos(yaw * 0.5);
    const double sy = sin(yaw * 0.5);

    fg_quaternion_t q;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    q.w = cr * cp * cy + sr * sp * sy;
    return q;
}

/**
 * Builds the JSON body of one channel message.
 *
 * The bot has no real-time clock, so timestamps count from boot. Foxglove
 * plots relative time, so the panels still line up.
 */
static void fg_build_message(int channel, const web_server_snapshot_t* snap, uint64_t now_ns,
                             const fg_quaternion_t* q, strbuf_t* sb) {
    switch (channel) {
        case FG_CH_TF:
            sb_printf(sb,
                      "{\"timestamp\":{\"sec\":%lu,\"nsec\":%lu},"
                      "\"parent_frame_id\":\"world\",\"child_frame_id\":\"bot\","
                      "\"translation\":{\"x\":0,\"y\":0,\"z\":0},"
                      "\"rotation\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"w\":%.6f}}",
                      (unsigned long)(now_ns / 1000000000ull),
                      (unsigned long)(now_ns % 1000000000ull),
                      q->x, q->y, q->z, q->w);
            break;

        case FG_CH_MOTORS:
            sb_printf(sb, "{\"left\":%d,\"right\":%d,\"weapon\":%d}",
                      snap->left, snap->right, snap->weapon);
            break;

        case FG_CH_IMU:
            sb_printf(sb,
                      "{\"orientation\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"w\":%.6f},"
                      "\"angular_velocity\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f},"
                      "\"linear_acceleration\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f}}",
                      q->x, q->y, q->z, q->w,
                      (double)snap->gyro_x * FG_PI / 180.0,
                      (double)snap->gyro_y * FG_PI / 180.0,
                      (double)snap->gyro_z * FG_PI / 180.0,
                      (double)snap->accel_x * FG_GRAVITY,
                      (double)snap->accel_y * FG_GRAVITY,
                      (double)snap->accel_z * FG_GRAVITY);
            break;

        case FG_CH_THERMAL:
            sb_printf(sb, "{\"temperature_c\":%u,\"humidity\":%u}",
                      (unsigned)snap->temp_c, (unsigned)snap->humidity);
            break;

        case FG_CH_STATE:
            sb_printf(sb, "{\"controller\":\"%s\",\"failsafe\":%s,\"command_age_ms\":%lu}",
                      web_server_state_text(snap->state),
                      snap->failsafe ? "true" : "false",
                      (unsigned long)snap->age_ms);
            break;

        default:
            break;
    }
}

/** Wraps a JSON body in the Foxglove binary message header and sends it. */
static err_t fg_send_message(fg_client_t* client, int channel, const web_server_snapshot_t* snap,
                             uint64_t now_ns, const fg_quaternion_t* q) {
    uint8_t* payload = g_msg + WS_HEADER_RESERVE;
    const size_t header_len = 13u;  // opcode + subscription id + timestamp
    strbuf_t sb;

    sb_init(&sb, (char*)payload + header_len, sizeof(g_msg) - WS_HEADER_RESERVE - header_len);
    fg_build_message(channel, snap, now_ns, q, &sb);
    if (sb.overflow) {
        return ERR_MEM;
    }

    const uint32_t subscription_id = client->subscription_id[channel];
    payload[0] = FG_SERVER_OP_MESSAGE_DATA;
    for (int i = 0; i < 4; ++i) {
        payload[1 + i] = (uint8_t)(subscription_id >> (i * 8));
    }
    for (int i = 0; i < 8; ++i) {
        payload[5 + i] = (uint8_t)(now_ns >> (i * 8));
    }

    return ws_send_frame(client, WS_OP_BINARY, g_msg, header_len + sb.len);
}

// =============================================================================
// Connection handling
// =============================================================================

static void fg_close(struct tcp_pcb* pcb) {
    if (pcb == NULL) {
        return;
    }

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
    }
}

static void fg_abort(struct tcp_pcb* pcb) {
    if (pcb == NULL) {
        return;
    }

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_abort(pcb);
}

static void fg_release_client(fg_client_t* client) {
    if (client != NULL) {
        memset(client, 0, sizeof(*client));
    }
}

static void fg_drop_client(fg_client_t* client, const char* reason) {
    if (client == NULL || client->pcb == NULL) {
        return;
    }

    struct tcp_pcb* pcb = client->pcb;
    printf("Foxglove: dropping client slot=%d (%s)\n", fg_client_index(client), reason);
    fg_release_client(client);
    fg_abort(pcb);
}

static fg_client_t* fg_allocate_client(struct tcp_pcb* pcb) {
    for (size_t i = 0; i < FOXGLOVE_WS_MAX_CLIENTS; ++i) {
        if (g_clients[i].pcb == NULL) {
            fg_release_client(&g_clients[i]);
            g_clients[i].pcb = pcb;
            return &g_clients[i];
        }
    }

    return NULL;
}

static err_t fg_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    fg_client_t* client = (fg_client_t*)arg;

    (void)err;

    if (p == NULL) {
        if (client != NULL) {
            printf("Foxglove: client slot=%d disconnected\n", fg_client_index(client));
            fg_release_client(client);
        }
        fg_close(pcb);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);

    if (client == NULL) {
        pbuf_free(p);
        fg_close(pcb);
        return ERR_OK;
    }

    const size_t space = FOXGLOVE_WS_RX_BUFFER_SIZE - client->rx_len;
    if (p->tot_len > space) {
        pbuf_free(p);
        fg_drop_client(client, "receive buffer is full");
        return ERR_OK;
    }

    pbuf_copy_partial(p, client->rx + client->rx_len, p->tot_len, 0);
    client->rx_len = (uint16_t)(client->rx_len + p->tot_len);
    client->rx[client->rx_len] = '\0';
    pbuf_free(p);

    if (!client->handshake_done) {
        char* end = strstr((char*)client->rx, "\r\n\r\n");
        if (end == NULL) {
            return ERR_OK;  // Wait for the rest of the request.
        }

        const size_t request_len = (size_t)(end - (char*)client->rx) + 4u;
        end[2] = '\0';  // Keep the final CRLF so header scanning stops cleanly.

        const bool ok = fg_do_handshake(client, (char*)client->rx);
        if (!ok) {
            struct tcp_pcb* dead = client->pcb;
            printf("Foxglove: handshake failed on slot=%d\n", fg_client_index(client));
            fg_release_client(client);
            fg_close(dead);
            return ERR_OK;
        }

        client->rx_len = (uint16_t)(client->rx_len - request_len);
        memmove(client->rx, client->rx + request_len, client->rx_len);
        client->rx[client->rx_len] = '\0';
    }

    fg_process_frames(client);
    return ERR_OK;
}

static void fg_err(void* arg, err_t err) {
    fg_client_t* client = (fg_client_t*)arg;

    if (client != NULL) {
        printf("Foxglove: client slot=%d errored with %d\n", fg_client_index(client), err);
        fg_release_client(client);
    }
}

static err_t fg_accept(void* arg, struct tcp_pcb* newpcb, err_t err) {
    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    fg_client_t* client = fg_allocate_client(newpcb);
    if (client == NULL) {
        printf("Foxglove: rejected a client, all %d slots are busy\n", FOXGLOVE_WS_MAX_CLIENTS);
        ws_send_raw(newpcb,
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Connection: close\r\n"
                    "Content-Type: text/plain\r\n\r\n"
                    "Too many Foxglove clients\r\n");
        fg_close(newpcb);
        return ERR_OK;
    }

    tcp_arg(newpcb, client);
    tcp_recv(newpcb, fg_recv);
    tcp_err(newpcb, fg_err);

    return ERR_OK;
}

// =============================================================================
// Broadcast
// =============================================================================

static void fg_broadcast_timer(btstack_timer_source_t* ts) {
    if (!g_running) {
        return;
    }

    ++g_tick;

    bool any_subscriber = false;
    for (size_t i = 0; i < FOXGLOVE_WS_MAX_CLIENTS && !any_subscriber; ++i) {
        if (g_clients[i].pcb == NULL || !g_clients[i].handshake_done) {
            continue;
        }
        for (int ch = 0; ch < FG_CH_COUNT; ++ch) {
            if (g_clients[i].subscribed[ch]) {
                any_subscriber = true;
                break;
            }
        }
    }

    if (any_subscriber) {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const uint64_t now_ns = to_us_since_boot(get_absolute_time()) * 1000ull;
        web_server_snapshot_t snap = {0};

        web_server_get_snapshot(&snap);
        const fg_quaternion_t q = fg_euler_to_quaternion(snap.roll, snap.pitch, snap.yaw);

        cyw43_arch_lwip_begin();
        for (size_t i = 0; i < FOXGLOVE_WS_MAX_CLIENTS; ++i) {
            fg_client_t* client = &g_clients[i];
            if (client->pcb == NULL || !client->handshake_done) {
                continue;
            }

            bool backpressured = false;
            for (int ch = 0; ch < FG_CH_COUNT; ++ch) {
                if (!client->subscribed[ch] || (g_tick % kChannels[ch].period) != 0u) {
                    continue;
                }

                const err_t write_err = fg_send_message(client, ch, &snap, now_ns, &q);
                if (write_err == ERR_MEM) {
                    backpressured = true;
                    break;
                }
                if (write_err != ERR_OK) {
                    fg_drop_client(client, "write failed");
                    break;
                }
            }

            if (client->pcb == NULL) {
                continue;
            }

            if (!backpressured) {
                client->backpressure_since_ms = 0;
                continue;
            }

            if (client->backpressure_since_ms == 0u) {
                client->backpressure_since_ms = now_ms;
            } else if ((now_ms - client->backpressure_since_ms) >= FOXGLOVE_WS_STALL_TIMEOUT_MS) {
                fg_drop_client(client, "stalled backpressure");
            }
        }
        cyw43_arch_lwip_end();
    }

    btstack_run_loop_set_timer(ts, FOXGLOVE_WS_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

// =============================================================================
// Lifecycle
// =============================================================================

bool foxglove_ws_init(motor_controller_t* motors) {
    g_motors = motors;
    g_tick = 0;
    g_session_id = to_ms_since_boot(get_absolute_time());
    memset(g_clients, 0, sizeof(g_clients));

    printf("Starting Foxglove WebSocket server on port %d...\n", FOXGLOVE_WS_PORT);

    cyw43_arch_lwip_begin();
    g_listen_pcb = tcp_new();
    if (g_listen_pcb == NULL) {
        cyw43_arch_lwip_end();
        printf("Foxglove: failed to create TCP PCB\n");
        return false;
    }

    if (tcp_bind(g_listen_pcb, IP_ADDR_ANY, FOXGLOVE_WS_PORT) != ERR_OK) {
        printf("Foxglove: failed to bind to port %d\n", FOXGLOVE_WS_PORT);
        tcp_close(g_listen_pcb);
        g_listen_pcb = NULL;
        cyw43_arch_lwip_end();
        return false;
    }

    g_listen_pcb = tcp_listen(g_listen_pcb);
    if (g_listen_pcb == NULL) {
        cyw43_arch_lwip_end();
        printf("Foxglove: failed to listen\n");
        return false;
    }

    tcp_accept(g_listen_pcb, fg_accept);
    cyw43_arch_lwip_end();

    g_running = true;
    btstack_run_loop_set_timer_handler(&g_broadcast_timer, fg_broadcast_timer);
    btstack_run_loop_set_timer(&g_broadcast_timer, FOXGLOVE_WS_INTERVAL_MS);
    btstack_run_loop_add_timer(&g_broadcast_timer);

    printf("Foxglove ready. In Foxglove, open a connection to ws://%s:%d\n",
           wifi_sta_get_ip(), FOXGLOVE_WS_PORT);

    return true;
}

void foxglove_ws_stop(void) {
    btstack_run_loop_remove_timer(&g_broadcast_timer);

    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < FOXGLOVE_WS_MAX_CLIENTS; ++i) {
        if (g_clients[i].pcb != NULL) {
            struct tcp_pcb* pcb = g_clients[i].pcb;
            fg_release_client(&g_clients[i]);
            fg_close(pcb);
        }
    }

    if (g_listen_pcb != NULL) {
        fg_close(g_listen_pcb);
        g_listen_pcb = NULL;
    }
    cyw43_arch_lwip_end();

    g_running = false;
    printf("Foxglove WebSocket server stopped\n");
}

bool foxglove_ws_is_running(void) {
    return g_running;
}

int foxglove_ws_client_count(void) {
    int count = 0;

    for (size_t i = 0; i < FOXGLOVE_WS_MAX_CLIENTS; ++i) {
        if (g_clients[i].pcb != NULL && g_clients[i].handshake_done) {
            ++count;
        }
    }

    return count;
}
