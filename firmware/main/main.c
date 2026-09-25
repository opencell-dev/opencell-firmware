/* OpenCell W12 firmware — board bring-up stage: power the front ends and
 * initialise the LR2021. The full bs-radio app replaces this in the next task. */
#include "esp_log.h"
#include "lc_radio.h"
#include "w12_board.h"

static const char *TAG = "lc_main";

void app_main(void)
{
    w12_board_init();
    int err = lc_radio_init(LC_BAND_915);
    ESP_LOGI(TAG, "LR2021 init: %d (0 = OK)", err);
}
