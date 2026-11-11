#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "display.h"
#include "font8x8.h"
#include "frame.h"

static const char *TAG = "frame";

#define DEFAULT_HOST "fugleramme.local"
#define DEFAULT_PORT 8080
#define NOTICE_SCALE 6
#define NOTICE_MARGIN 72
#define CRC32_CHECK 0xCBF43926u

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint8_t encoding;
    uint8_t poll;
    uint16_t width;
    uint16_t height;
    uint32_t length;
    uint32_t crc;
} e6_header_t;

typedef struct {
    char etag[FRAME_ETAG_LEN];
} fetch_ctx_t;

static SemaphoreHandle_t s_mu;
static frame_config_t s_cfg;
static char s_result[FRAME_RESULT_LEN] = "waiting";
static char s_shown[160];
static volatile uint32_t s_busy;
static volatile uint32_t s_gen;
static uint8_t *s_pixels;
static bool s_crc_ok;

static void set_result(const char *text)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_result, sizeof(s_result), "%s", text);
    xSemaphoreGive(s_mu);
}

static void write_nvs_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("frame", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(handle, "host", s_cfg.host);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "port", s_cfg.port);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "tls", s_cfg.tls ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "etag", s_cfg.etag);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "poll", s_cfg.poll);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void load_nvs(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", DEFAULT_HOST);
    s_cfg.port = DEFAULT_PORT;
    s_cfg.poll = MIN_POLL_MINUTES;

    nvs_handle_t handle;
    if (nvs_open("frame", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_cfg.host);
    if (nvs_get_str(handle, "host", s_cfg.host, &len) != ESP_OK || s_cfg.host[0] == '\0') {
        snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", DEFAULT_HOST);
    }
    uint16_t port = 0;
    if (nvs_get_u16(handle, "port", &port) == ESP_OK && port != 0) {
        s_cfg.port = port;
    }
    uint8_t tls = 0;
    if (nvs_get_u8(handle, "tls", &tls) == ESP_OK) {
        s_cfg.tls = tls != 0;
    }
    len = sizeof(s_cfg.etag);
    if (nvs_get_str(handle, "etag", s_cfg.etag, &len) != ESP_OK) {
        s_cfg.etag[0] = '\0';
    }
    uint8_t poll = 0;
    if (nvs_get_u8(handle, "poll", &poll) == ESP_OK) {
        s_cfg.poll = poll;
    }
    nvs_close(handle);
}

static int wait_minutes(uint8_t poll)
{
    return poll < MIN_POLL_MINUTES ? MIN_POLL_MINUTES : poll;
}

static void set_pixel(int x, int y, uint8_t ink)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return;
    }
    int half = x >= (EPD_WIDTH / 2);
    int local_x = half ? x - (EPD_WIDTH / 2) : x;
    size_t index = (size_t)half * ((size_t)EPD_HALF_BYTES * EPD_HEIGHT)
        + (size_t)y * EPD_HALF_BYTES
        + (size_t)local_x / 2;
    uint8_t byte = s_pixels[index];
    if ((local_x & 1) == 0) {
        byte = (uint8_t)((byte & 0x0F) | (ink << 4));
    } else {
        byte = (uint8_t)((byte & 0xF0) | (ink & 0x0F));
    }
    s_pixels[index] = byte;
}

static void draw_glyph(int x, int y, unsigned char ch)
{
    if (ch > 127) {
        ch = '?';
    }
    const uint8_t *glyph = font8x8_basic[ch];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (((glyph[row] >> col) & 1) == 0) {
                continue;
            }
            for (int sy = 0; sy < NOTICE_SCALE; sy++) {
                for (int sx = 0; sx < NOTICE_SCALE; sx++) {
                    set_pixel(
                        x + col * NOTICE_SCALE + sx,
                        y + row * NOTICE_SCALE + sy,
                        0x0);
                }
            }
        }
    }
}

static void render_notice(const char *message)
{
    const int cell = 8 * NOTICE_SCALE;
    const int limit = EPD_WIDTH - NOTICE_MARGIN;
    memset(s_pixels, WHITE, EPD_FRAME_BYTES);
    int x = NOTICE_MARGIN;
    int y = NOTICE_MARGIN;
    const char *p = message;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '\n') {
            p++;
            x = NOTICE_MARGIN;
            y += cell + NOTICE_SCALE;
            continue;
        }
        const char *word = p;
        int len = 0;
        while (p[len] && p[len] != ' ' && p[len] != '\n') {
            len++;
        }
        if (x > NOTICE_MARGIN && x + len * cell > limit) {
            x = NOTICE_MARGIN;
            y += cell + NOTICE_SCALE;
        }
        for (int i = 0; i < len; i++) {
            if (y + cell > EPD_HEIGHT - NOTICE_MARGIN) {
                return;
            }
            draw_glyph(x, y, (unsigned char)word[i]);
            x += cell;
        }
        p += len;
        if (*p == ' ') {
            x += cell;
        }
    }
}

static void show_notice(const char *message)
{
    set_result(message);
    if (strcmp(s_shown, message) == 0) {
        ESP_LOGI(TAG, "notice unchanged");
        return;
    }
    if (s_pixels == NULL) {
        return;
    }
    ESP_LOGW(TAG, "%s", message);
    render_notice(message);
    s_busy = 1;
    epdDisplayFrame(s_pixels);
    s_busy = 0;
    snprintf(s_shown, sizeof(s_shown), "%s", message);
}

static esp_err_t on_http(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER || event->user_data == NULL) {
        return ESP_OK;
    }
    if (event->header_key == NULL || event->header_value == NULL) {
        return ESP_OK;
    }
    if (strcasecmp(event->header_key, "ETag") != 0) {
        return ESP_OK;
    }
    fetch_ctx_t *ctx = event->user_data;
    if (strlen(event->header_value) >= sizeof(ctx->etag)) {
        ESP_LOGW(TAG, "etag too long, ignoring it");
        ctx->etag[0] = '\0';
        return ESP_OK;
    }
    snprintf(ctx->etag, sizeof(ctx->etag), "%s", event->header_value);
    return ESP_OK;
}

static bool read_exact(esp_http_client_handle_t client, uint8_t *dest, int total)
{
    int got = 0;
    while (got < total) {
        int n = esp_http_client_read(client, (char *)dest + got, total - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

static void remember_page(uint32_t gen, const char *etag, uint8_t poll)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_gen == gen) {
        snprintf(s_cfg.etag, sizeof(s_cfg.etag), "%s", etag);
        s_cfg.poll = poll;
        write_nvs_locked();
    }
    xSemaphoreGive(s_mu);
}

void frame_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    load_nvs();
    const uint8_t check[] = "123456789";
    uint32_t crc = esp_crc32_le(0, check, sizeof(check) - 1);
    s_crc_ok = crc == CRC32_CHECK;
    if (s_crc_ok) {
        ESP_LOGI(TAG, "CRC32 self-check ok");
    } else {
        ESP_LOGE(TAG, "CRC32 self-check failed: %08lx", (unsigned long)crc);
        snprintf(s_result, sizeof(s_result), "CRC32 self-check failed");
    }
    s_pixels = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixels == NULL) {
        ESP_LOGE(TAG, "PSRAM frame buffer allocation failed");
        snprintf(s_result, sizeof(s_result), "no memory");
    } else {
        ESP_LOGI(TAG, "frame buffer %d bytes in PSRAM", EPD_FRAME_BYTES);
    }
    ESP_LOGI(TAG, "frame host %s:%u tls %d", s_cfg.host, s_cfg.port, s_cfg.tls);
}

void frame_get_status(frame_status_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(out->host, sizeof(out->host), "%s", s_cfg.host);
    out->port = s_cfg.port;
    out->tls = s_cfg.tls;
    out->poll = s_cfg.poll;
    snprintf(out->result, sizeof(out->result), "%s", s_result);
    xSemaphoreGive(s_mu);
    out->busy = s_busy != 0;
}

void frame_config_load(frame_config_t *cfg)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *cfg = s_cfg;
    xSemaphoreGive(s_mu);
}

bool frame_config_save(const char *host, uint16_t port, bool tls)
{
    if (host == NULL || host[0] == '\0' || strlen(host) >= FRAME_HOST_LEN || port == 0) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.port = port;
    s_cfg.tls = tls;
    s_cfg.etag[0] = '\0';
    s_gen++;
    write_nvs_locked();
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "saved frame %s:%u tls %d", host, port, tls);
    return true;
}

int frame_fetch(const frame_config_t *cfg)
{
    if (!s_crc_ok) {
        set_result("CRC32 self-check failed");
        return MIN_POLL_MINUTES;
    }
    if (s_pixels == NULL) {
        set_result("no memory");
        return RETRY_MINUTES;
    }

    uint32_t gen = s_gen;
    set_result("fetching");

    char url[192];
    snprintf(
        url,
        sizeof(url),
        "%s://%s:%u/frame.e6",
        cfg->tls ? "https" : "http",
        cfg->host,
        cfg->port);

    fetch_ctx_t ctx = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 120000,
        .event_handler = on_http,
        .user_data = &ctx,
        .skip_cert_common_name_check = true,
        .buffer_size = 2048,
        .user_agent = "fugleramme-gdep133c02",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        set_result("cannot reach the frame");
        return RETRY_MINUTES;
    }
    if (cfg->etag[0] != '\0') {
        esp_http_client_set_header(client, "If-None-Match", cfg->etag);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        set_result("cannot reach the frame");
        return RETRY_MINUTES;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGW(TAG, "headers failed for %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_result("cannot reach the frame");
        return RETRY_MINUTES;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "%s -> %d", url, status);

    char notice[160] = {0};
    char result[FRAME_RESULT_LEN] = {0};
    bool draw_page = false;
    uint8_t page_poll = cfg->poll;
    int minutes = RETRY_MINUTES;

    if (status == 304) {
        snprintf(result, sizeof(result), "unchanged");
        minutes = wait_minutes(cfg->poll);
    } else if (status == 404) {
        snprintf(notice, sizeof(notice), "Enable External e-ink panel under Frame on the admin page.");
        minutes = MIN_POLL_MINUTES;
    } else if (status == 503) {
        snprintf(result, sizeof(result), "the frame has not drawn a page yet");
    } else if (status != 200) {
        snprintf(result, sizeof(result), "the frame answered %d", status);
    } else {
        e6_header_t header;
        if (!read_exact(client, (uint8_t *)&header, sizeof(header))) {
            snprintf(result, sizeof(result), "download failed");
        } else if (memcmp(header.magic, "E6F1", 4) != 0 || header.version != 1 || header.encoding != 1) {
            snprintf(notice, sizeof(notice), "The frame sends a page this program cannot read.");
            minutes = MIN_POLL_MINUTES;
        } else if (header.width != EPD_WIDTH || header.height != EPD_HEIGHT || header.length != EPD_FRAME_BYTES) {
            snprintf(
                notice,
                sizeof(notice),
                "This page is %u x %u. This panel is %d x %d.",
                header.width,
                header.height,
                EPD_WIDTH,
                EPD_HEIGHT);
            minutes = MIN_POLL_MINUTES;
        } else if (!read_exact(client, s_pixels, EPD_FRAME_BYTES)) {
            snprintf(result, sizeof(result), "download failed");
        } else if (esp_crc32_le(0, s_pixels, EPD_FRAME_BYTES) != header.crc) {
            snprintf(notice, sizeof(notice), "The page arrived damaged.");
            minutes = MIN_POLL_MINUTES;
        } else {
            draw_page = true;
            page_poll = header.poll;
            minutes = wait_minutes(header.poll);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (notice[0] != '\0') {
        show_notice(notice);
        return minutes;
    }
    if (!draw_page) {
        set_result(result);
        return minutes;
    }

    ESP_LOGI(TAG, "page ok, poll %u", page_poll);
    s_busy = 1;
    epdDisplayFrame(s_pixels);
    s_busy = 0;
    s_shown[0] = '\0';
    remember_page(gen, ctx.etag, page_poll);
    set_result("updated");
    return minutes;
}
