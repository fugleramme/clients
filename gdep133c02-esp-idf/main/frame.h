#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FRAME_HOST_LEN 128
#define FRAME_ETAG_LEN 128
#define FRAME_RESULT_LEN 96

#define MIN_POLL_MINUTES 5
#define RETRY_MINUTES 1

typedef struct {
    char host[FRAME_HOST_LEN];
    uint16_t port;
    bool tls;
    char etag[FRAME_ETAG_LEN];
    uint8_t poll;
} frame_config_t;

typedef struct {
    char host[FRAME_HOST_LEN];
    uint16_t port;
    bool tls;
    uint8_t poll;
    char result[FRAME_RESULT_LEN];
    bool busy;
} frame_status_t;

void frame_init(void);
void frame_get_status(frame_status_t *out);
void frame_config_load(frame_config_t *cfg);

/* Stores the address, clears the ETag, and bumps the generation so a fetch
 * already in flight will not write the old ETag back. */
bool frame_config_save(const char *host, uint16_t port, bool tls);

/* Minutes until the next poll. Draws at most one lasting notice per message. */
int frame_fetch(const frame_config_t *cfg);
