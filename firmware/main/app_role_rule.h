/* Which role a W12 runs, as the NVS value "oc"/"term" stores it, and what
 * holding BOOT does: terminal <-> bs-radio; a cell goes to bs-radio (off the
 * air by hand), and nothing goes to cell (only oc-cell-prov sets it).
 * Portable: host-tested. */
#ifndef APP_ROLE_RULE_H
#define APP_ROLE_RULE_H

#include <stdint.h>

typedef enum { APP_ROLE_BS_RADIO = 0, APP_ROLE_TERMINAL = 1, APP_ROLE_CELL = 2 } app_role_t;

static inline app_role_t app_role_after_hold(app_role_t r)
{
    return r == APP_ROLE_BS_RADIO ? APP_ROLE_TERMINAL : APP_ROLE_BS_RADIO;
}

static inline app_role_t app_role_from_nvs(uint8_t v)
{
    return v == 1 ? APP_ROLE_TERMINAL : v == 2 ? APP_ROLE_CELL : APP_ROLE_BS_RADIO;
}

#endif
