/*
 * fuglefeed.cpp: see fuglefeed.h.
 *
 * The streaming loop and the TLS setup follow artfeed.cpp from
 * FutureSharks/e-paper-art-display (MIT), which drives the same panel.
 */
#include "fuglefeed.h"

#include <ArduinoLog.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdarg.h>
#include "el133.h"

/* The Mozilla root store bundled with arduino-esp32, for https:// URLs. */
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

static const char *NVS_NAMESPACE = "fugleramme";
static const char *KEY_CRC = "crc";
static const char *KEY_ETAG = "etag";
static const char *KEY_POLL = "poll";

static const size_t E6_HEADER_BYTES = 20;
static const uint16_t E6_VERSION = 1;
static const uint8_t E6_ENCODING = 1;

/* Pulled off the socket and pushed to the panel in one go. */
static const size_t CHUNK_BYTES = 2048;

static char last_error[200];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, args);
    va_end(args);
    Log.error("%s" CR, last_error);
}

const char *fuglefeed_last_error()
{
    return last_error;
}

FuglefeedConfig fuglefeed_default_config()
{
    FuglefeedConfig cfg = {};
    cfg.url = nullptr;
    cfg.http_timeout_ms = 20000;
    cfg.force = false;
    cfg.on_stage = nullptr;
    return cfg;
}

static inline void report(const FuglefeedConfig &cfg, FuglefeedStage stage)
{
    if (cfg.on_stage != nullptr)
        cfg.on_stage(stage);
}

/* ---- CRC-32 (the zlib / PNG polynomial) -------------------------------- */

static uint32_t crc_table[256];
static bool crc_ready = false;

uint32_t fuglefeed_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!crc_ready)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = true;
    }
    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- What is on the glass, kept in NVS ---------------------------------- */

struct Drawn
{
    bool known;
    uint32_t crc;
    String etag;
};

static Drawn load_drawn()
{
    Drawn d;
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    d.known = prefs.isKey(KEY_CRC);
    d.crc = prefs.getUInt(KEY_CRC, 0);
    d.etag = prefs.getString(KEY_ETAG, "");
    prefs.end();
    return d;
}

static void save_drawn(uint32_t crc, const String &etag)
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(KEY_CRC, crc);
    prefs.putString(KEY_ETAG, etag);
    prefs.end();
}

static void save_etag(const String &etag)
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(KEY_ETAG, etag);
    prefs.end();
}

static void save_poll(uint8_t poll)
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    if (prefs.getUChar(KEY_POLL, 255) != poll)
        prefs.putUChar(KEY_POLL, poll);
    prefs.end();
}

uint8_t fuglefeed_poll_minutes()
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t poll = prefs.getUChar(KEY_POLL, 0);
    prefs.end();
    return poll;
}

void fuglefeed_forget()
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove(KEY_CRC);
    prefs.remove(KEY_ETAG);
    prefs.end();
}

/* ---- HTTP ------------------------------------------------------------- */

/* Both client kinds live here so either can back the request. */
struct Connection
{
    NetworkClient plain;
    NetworkClientSecure secure;
    HTTPClient http;
};

static bool is_https(const char *url)
{
    return strncasecmp(url, "https://", 8) == 0;
}

/* GET the page, with If-None-Match when `etag` is given. Returns the HTTP
 * status, or a negative HTTPClient error. */
static int request(const FuglefeedConfig &cfg, Connection &c, const char *etag)
{
    NetworkClient *client = &c.plain;
    if (is_https(cfg.url))
    {
        c.secure.setCACertBundle(rootca_crt_bundle_start,
                                 (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
        client = &c.secure;
    }

    c.http.setTimeout(cfg.http_timeout_ms);
    c.http.setReuse(false);
    c.http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    c.http.setUserAgent("fugleramme-frame");

    Log.notice("GET %s%s" CR, cfg.url, (etag != nullptr && etag[0]) ? " (If-None-Match)" : "");
    if (!c.http.begin(*client, cfg.url))
    {
        set_error("Not a valid URL: %s", cfg.url);
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }

    static const char *headers[] = {"ETag", "Location"};
    c.http.collectHeaders(headers, 2);
    if (etag != nullptr && etag[0] != '\0')
        c.http.addHeader("If-None-Match", etag);

    return c.http.GET();
}

/* Turn a failed status into something a person can act on. */
static void explain_status(Connection &c, int code, const char *url)
{
    if (code < 0)
    {
        set_error("Cannot reach %s: %s (WiFi %s, RSSI %d dBm)", url,
                  c.http.errorToString(code).c_str(),
                  WiFi.isConnected() ? "up" : "DOWN", (int)WiFi.RSSI());
        return;
    }

    switch (code)
    {
    case 301:
    case 302:
    case 307:
    case 308:
        set_error("HTTP %d: the server redirects to %s - put that in FRAME_URL", code,
                  c.http.header("Location").c_str());
        break;
    case 404:
        set_error("HTTP 404: turn on \"External e-ink panel\" in the Fugleramme admin page");
        break;
    case 503:
        set_error("HTTP 503: Fugleramme has not drawn a page yet");
        break;
    case 502:
    case 504:
        set_error("HTTP %d: the proxy cannot reach Fugleramme", code);
        break;
    default:
        set_error("HTTP %d from %s", code, url);
        break;
    }
}

/* Read exactly `len` bytes, waiting out the gaps a slow link leaves. */
static bool read_exact(WiFiClient *stream, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    uint32_t last = millis();
    while (got < len)
    {
        int n = stream->read(buf + got, len - got);
        if (n > 0)
        {
            got += (size_t)n;
            last = millis();
            continue;
        }
        if (!stream->connected() && stream->available() == 0)
            return false;
        if (millis() - last > timeout_ms)
            return false;
        delay(5);
    }
    return true;
}

/* ---- The E6 header ---------------------------------------------------- */

struct E6Header
{
    uint16_t version;
    uint8_t encoding;
    uint8_t poll;
    uint16_t width;
    uint16_t height;
    uint32_t length;
    uint32_t crc;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Read and check the header at the start of the body. */
static bool read_header(const FuglefeedConfig &cfg, Connection &c, E6Header &h)
{
    uint8_t b[E6_HEADER_BYTES];
    if (!read_exact(c.http.getStreamPtr(), b, sizeof(b), cfg.http_timeout_ms))
    {
        set_error("The page ended before its header - is FRAME_URL the /frame.e6 address?");
        return false;
    }
    if (memcmp(b, "E6F1", 4) != 0)
    {
        set_error("Not an E6 page (starts \"%.4s\") - is FRAME_URL the /frame.e6 address?",
                  (const char *)b);
        return false;
    }

    h.version = le16(b + 4);
    h.encoding = b[6];
    h.poll = b[7];
    h.width = le16(b + 8);
    h.height = le16(b + 10);
    h.length = le32(b + 12);
    h.crc = le32(b + 16);
    char crc_text[12];
    snprintf(crc_text, sizeof(crc_text), "%08lX", (unsigned long)h.crc);
    Log.notice("Page %ux%u, CRC %s, Panel refresh %u min" CR, h.width, h.height, crc_text,
               h.poll);

    if (h.version != E6_VERSION || h.encoding != E6_ENCODING)
    {
        set_error("Unsupported E6 version %u / encoding %u - update this firmware",
                  h.version, h.encoding);
        return false;
    }
    if (h.width != EL133_PORTRAIT_W || h.height != EL133_PORTRAIT_H ||
        h.length != EL133_STREAM_BYTES)
    {
        set_error("Page is %ux%u - set External panel size to 13.3 in the Fugleramme admin page",
                  h.width, h.height);
        return false;
    }
    return true;
}

/* ---- Streaming -------------------------------------------------------- */

/* Push the payload into the two controllers, computing its CRC on the way. */
static bool stream_payload(const FuglefeedConfig &cfg, Connection &c, uint32_t &crc_out)
{
    WiFiClient *stream = c.http.getStreamPtr();
    uint8_t buf[CHUNK_BYTES];
    uint32_t crc = 0;
    const uint32_t t0 = millis();

    for (int half = 0; half < 2; half++)
    {
        el133_stream_begin(half);
        size_t remaining = EL133_HALF_BYTES;
        uint32_t last = millis();

        while (remaining > 0)
        {
            size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
            int got = stream->read(buf, want);
            if (got > 0)
            {
                el133_stream_write(buf, (size_t)got);
                crc = fuglefeed_crc32(crc, buf, (size_t)got);
                remaining -= (size_t)got;
                last = millis();
                continue;
            }
            const size_t done = half * EL133_HALF_BYTES + EL133_HALF_BYTES - remaining;
            if (!stream->connected() && stream->available() == 0)
            {
                el133_stream_end();
                set_error("Connection closed after %u of %u bytes", (unsigned)done,
                          (unsigned)EL133_STREAM_BYTES);
                return false;
            }
            if (millis() - last > cfg.http_timeout_ms)
            {
                el133_stream_end();
                set_error("Download stalled after %u of %u bytes", (unsigned)done,
                          (unsigned)EL133_STREAM_BYTES);
                return false;
            }
            delay(5);
        }
        el133_stream_end();
    }

    Log.notice("Streamed %u bytes in %u ms" CR, (unsigned)EL133_STREAM_BYTES,
               (unsigned)(millis() - t0));
    crc_out = crc;
    return true;
}

/* ---- The wake's work -------------------------------------------------- */

FuglefeedResult fuglefeed_update(const FuglefeedConfig &cfg)
{
    last_error[0] = '\0';
    const Drawn drawn = load_drawn();

    /* 1. Has it changed? A 304, or a header whose CRC is the one on the glass,
     *    both mean no. Only the header is read; the rest is left unread. */
    report(cfg, FUGLEFEED_STAGE_CHECK);
    {
        Connection c;
        const char *etag = (!cfg.force && drawn.known) ? drawn.etag.c_str() : nullptr;
        int code = request(cfg, c, etag);

        if (code == HTTP_CODE_NOT_MODIFIED)
        {
            c.http.end();
            Log.notice("Page unchanged (304)" CR);
            return FUGLEFEED_UNCHANGED;
        }
        if (code != HTTP_CODE_OK)
        {
            explain_status(c, code, cfg.url);
            c.http.end();
            return FUGLEFEED_FAILED;
        }

        E6Header h;
        const String etag_now = c.http.header("ETag");
        bool ok = read_header(cfg, c, h);
        c.http.end();
        if (!ok)
            return FUGLEFEED_FAILED;

        save_poll(h.poll);
        if (!cfg.force && drawn.known && h.crc == drawn.crc)
        {
            /* A proxy that strips or weakens the ETag lands here every time,
             * which still costs only the header. */
            if (etag_now != drawn.etag)
                save_etag(etag_now);
            Log.notice("Page unchanged (same CRC)" CR);
            return FUGLEFEED_UNCHANGED;
        }
        Log.notice(cfg.force ? "Redrawing the page" CR : "New page" CR);
    }

    /* 2. New page. Init the panel before the download: its ~7 s of blocking
     *    delays would otherwise leave the socket unread long enough to stall. */
    el133_init_panel();

    report(cfg, FUGLEFEED_STAGE_DOWNLOAD);
    Connection c;
    int code = request(cfg, c, nullptr);
    if (code != HTTP_CODE_OK)
    {
        explain_status(c, code, cfg.url);
        c.http.end();
        return FUGLEFEED_FAILED;
    }

    const String etag_now = c.http.header("ETag");
    E6Header h;
    if (!read_header(cfg, c, h))
    {
        c.http.end();
        return FUGLEFEED_FAILED;
    }
    save_poll(h.poll);

    const int size = c.http.getSize();
    if (size >= 0 && (size_t)size != E6_HEADER_BYTES + EL133_STREAM_BYTES)
    {
        set_error("Got %d bytes, expected %u", size,
                  (unsigned)(E6_HEADER_BYTES + EL133_STREAM_BYTES));
        c.http.end();
        return FUGLEFEED_FAILED;
    }

    uint32_t crc = 0;
    bool streamed = stream_payload(cfg, c, crc);
    c.http.end();
    if (!streamed)
        return FUGLEFEED_FAILED; // nothing reaches the glass without a refresh

    if (crc != h.crc)
    {
        set_error("Download corrupted: CRC %X, header says %X", (unsigned)crc, (unsigned)h.crc);
        return FUGLEFEED_FAILED;
    }

    /* The radio is the expensive part of a wake, the refresh the long part. */
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    report(cfg, FUGLEFEED_STAGE_REFRESH);
    if (!el133_refresh())
        return FUGLEFEED_REFRESH_FAILED;

    save_drawn(h.crc, etag_now);
    return FUGLEFEED_SHOWN;
}
