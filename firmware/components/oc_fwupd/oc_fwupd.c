#include "oc_fwupd.h"

#include <string.h>

void oc_fwupd_init(oc_fwupd_t *u, const oc_fwupd_ops_t *ops)
{
    memset(u, 0, sizeof(*u));
    u->ops = *ops;
}

static void close_update(oc_fwupd_t *u)
{
    if (u->open) {
        u->ops.abort(u->ops.ctx);
    }
    u->open = 0;
    u->written = 0;
}

uint8_t oc_fwupd_chunk(oc_fwupd_t *u, const oc_fw_chunk_t *c)
{
    if (c->len == 0 || c->data == NULL) {
        return OC_ACK_ERR_MALFORMED;
    }
    if (c->offset == 0) {
        close_update(u);
        if (u->ops.begin(u->ops.ctx) != 0) {
            return OC_ACK_ERR_FLASH;
        }
        u->open = 1;
    } else if (!u->open) {
        return OC_ACK_ERR_MALFORMED;
    } else if (c->offset < u->written) {
        /* Retry of a chunk already written: acknowledge if it lines up. */
        return (uint64_t)c->offset + c->len <= u->written ? OC_ACK_OK : OC_ACK_ERR_MALFORMED;
    } else if (c->offset > u->written) {
        return OC_ACK_ERR_MALFORMED; /* gap */
    }
    if (u->ops.write(u->ops.ctx, c->offset, c->data, c->len) != 0) {
        close_update(u);
        return OC_ACK_ERR_FLASH;
    }
    u->written += c->len;
    return OC_ACK_OK;
}

uint8_t oc_fwupd_commit(oc_fwupd_t *u, const oc_fw_commit_t *commit)
{
    if (!u->open || commit->image_size != u->written || commit->image_size == 0) {
        close_update(u);
        return OC_ACK_ERR_MALFORMED;
    }
    int err = u->ops.finish(u->ops.ctx, commit->image_size);
    u->open = 0;
    u->written = 0;
    return err == 0 ? OC_ACK_OK : OC_ACK_ERR_FLASH;
}
