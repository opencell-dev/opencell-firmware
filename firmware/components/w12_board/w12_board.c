#include "w12_board.h"

#include "driver/gpio.h"

void w12_board_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << W12_PIN_FEM_LF_PWR) | (1ULL << W12_PIN_FEM_HF_PWR) |
                        (1ULL << W12_PIN_VEXT_EN) | (1ULL << W12_PIN_GNSS_FORCE) |
                        (1ULL << W12_PIN_GNSS_RESET) | (1ULL << W12_PIN_GNSS_SUPPLY),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(W12_PIN_FEM_LF_PWR, 1);
    gpio_set_level(W12_PIN_FEM_HF_PWR, 1);
    gpio_set_level(W12_PIN_VEXT_EN, 0);
    /* A GNSS module on the header supplies the PPS (bench, or a base station
     * without a Pi-side GPS). Powered, out of reset, awake. Harmless when the
     * header carries the Pi link instead. */
    gpio_set_level(W12_PIN_GNSS_SUPPLY, 0);
    gpio_set_level(W12_PIN_GNSS_RESET, 1);
    gpio_set_level(W12_PIN_GNSS_FORCE, 1);
}
