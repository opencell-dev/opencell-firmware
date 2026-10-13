/* Which role this W12 runs. Same image for all three roles; the choice is an NVS value (app_role_rule.h).
 * To switch: power up, then within APP_ROLE_WINDOW_MS press and hold BOOT
 * (GPIO0) for APP_ROLE_HOLD_MS. The board flips the flag and restarts.
 * (Don't hold BOOT while resetting: that enters the ROM download mode.) */
#ifndef APP_ROLE_H
#define APP_ROLE_H

#include "app_role_rule.h"

#define APP_ROLE_WINDOW_MS 10000
#define APP_ROLE_HOLD_MS   3000

app_role_t app_role_get(void);         /* NVS must be initialised */
void       app_role_set(app_role_t r);
int        app_role_is_terminal(void); /* app_role_get() == APP_ROLE_TERMINAL */
void app_role_start_button_watch(void);

#endif
