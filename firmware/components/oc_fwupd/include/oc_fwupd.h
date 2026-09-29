/* oc_fwupd — firmware update over the host UART.
 *
 * The host streams the new app image as FW_CHUNK messages in order starting
 * at offset 0, then sends FW_COMMIT with the total size. A chunk at offset 0
 * (re)starts the update. A chunk the device already wrote (host retry after a
 * lost ACK) is acknowledged without rewriting. The platform ops write into
 * the inactive OTA slot and switch the boot partition on finish; the new app
 * must mark itself valid or the bootloader rolls back. */
#ifndef OC_FWUPD_H
#define OC_FWUPD_H

#include <stdint.h>

#include "oc_link.h"

typedef struct {
    void *ctx;
    int (*begin)(void *ctx);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint16_t len);
    int (*finish)(void *ctx, uint32_t image_size); /* verify image, set boot partition */
    void (*abort)(void *ctx);
} oc_fwupd_ops_t;

typedef struct {
    oc_fwupd_ops_t ops;
    int            open;
    uint32_t       written;
} oc_fwupd_t;

void oc_fwupd_init(oc_fwupd_t *u, const oc_fwupd_ops_t *ops);

/* Each returns an oc_ack_status_t. */
uint8_t oc_fwupd_chunk(oc_fwupd_t *u, const oc_fw_chunk_t *chunk);
uint8_t oc_fwupd_commit(oc_fwupd_t *u, const oc_fw_commit_t *commit);

#endif
