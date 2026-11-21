/*
 * fuglefeed.h: fetch a Fugleramme page from /frame.e6 and draw it on the
 * Pimoroni Inky Impression 13.3" (EL133UF1) through the el133 driver.
 *
 * /frame.e6 is a 20-byte little-endian header followed by the panel's own
 * colour codes, two pixels a byte, left half of every row for the first
 * controller and then the right halves for the second. That is exactly the
 * stream el133 expects (see el133.h), so nothing is rotated or buffered here.
 *
 *     0  "E6F1"            4  version (u16, 1)   6  encoding (u8, 1)
 *     7  poll minutes (u8) 8  width (u16, 1200)  10 height (u16, 1600)
 *     12 payload length    16 payload CRC-32 (zlib)
 *
 * A wake costs as little as possible when nothing has changed:
 *
 *   1. GET with If-None-Match. A 304 means the page is the one on the glass.
 *   2. Otherwise read only the 20-byte header and drop the connection. If its
 *      CRC matches the page last drawn, nothing changed either - this catches
 *      a reverse proxy that strips or weakens the ETag.
 *   3. Only for a new page: init the panel (~7 s, done before the download so
 *      the socket is never left unread), GET again, stream both halves while
 *      computing the CRC, and refresh only if it matches.
 *
 * What was drawn (CRC, ETag) and the frame's Panel refresh setting are kept
 * in NVS, so they survive deep sleep and power loss.
 */
#pragma once

#include <Arduino.h>

enum FuglefeedStage
{
    FUGLEFEED_STAGE_CHECK,    // asking whether the page changed
    FUGLEFEED_STAGE_DOWNLOAD, // streaming a new page into the controllers
    FUGLEFEED_STAGE_REFRESH,  // the panel's ~35 s refresh
};

enum FuglefeedResult
{
    FUGLEFEED_UNCHANGED,      // the glass already shows this page
    FUGLEFEED_SHOWN,          // a new page was drawn
    FUGLEFEED_FAILED,         // network, server or format problem; glass untouched
    FUGLEFEED_REFRESH_FAILED, // the page streamed but the panel did not refresh
};

struct FuglefeedConfig
{
    /* Full URL of the page, e.g. "http://birds-frame.example/frame.e6".
     * http:// and https:// both work; redirects are reported, not followed. */
    const char *url;

    /* Request timeout, and how long a download may stall before it is dropped. */
    uint32_t http_timeout_ms;

    /* Draw the page even if it matches the one last drawn. */
    bool force;

    /* Optional progress hook. May be null. */
    void (*on_stage)(FuglefeedStage stage);
};

FuglefeedConfig fuglefeed_default_config();

/* Check the page and draw it if it changed. Drops the radio before any
 * refresh, so WiFi may be down on return. */
FuglefeedResult fuglefeed_update(const FuglefeedConfig &cfg);

/* The frame's Panel refresh setting from the last page seen, in minutes.
 * 0 means "as soon as it changes" or not known yet. */
uint8_t fuglefeed_poll_minutes();

/* Forget what was drawn, so the next update redraws even an unchanged page.
 * Call after putting something else on the glass, such as the error screen. */
void fuglefeed_forget();

/* Why the last fuglefeed_update() failed, in a line or two for the error
 * screen. Only meaningful after FUGLEFEED_FAILED. */
const char *fuglefeed_last_error();

/* zlib-compatible CRC-32. Start with crc = 0 and feed the result back in. */
uint32_t fuglefeed_crc32(uint32_t crc, const uint8_t *data, size_t len);
