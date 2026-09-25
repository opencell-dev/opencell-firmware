/* lc_fwupd — firmware update over the host UART.
 *
 * The host streams the new app image as FW_CHUNK messages in order starting
 * at offset 0, then sends FW_COMMIT with the total size. A chunk at offset 0
 * (re)starts the update. A chunk the device already wrote (host retry after a
 * lost ACK) is acknowledged without rewriting. The platform ops write into
 * the inactive OTA slot and switch the boot partition on finish; the new app
 * must mark itself valid or the bootloader rolls back. */
#ifndef LC_FWUPD_H
#define LC_FWUPD_H

#include <stdint.h>

#include "lc_link.h"

typedef struct {
    void *ctx;
    int (*begin)(void *ctx);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint16_t len);
    int (*finish)(void *ctx, uint32_t image_size); /* verify image, set boot partition */
    void (*abort)(void *ctx);
} lc_fwupd_ops_t;

typedef struct {
    lc_fwupd_ops_t ops;
    int            open;
    uint32_t       written;
} lc_fwupd_t;

void lc_fwupd_init(lc_fwupd_t *u, const lc_fwupd_ops_t *ops);

/* Each returns an lc_ack_status_t. */
uint8_t lc_fwupd_chunk(lc_fwupd_t *u, const lc_fw_chunk_t *chunk);
uint8_t lc_fwupd_commit(lc_fwupd_t *u, const lc_fw_commit_t *commit);

#endif
