#include "w12_board.h"

#include "driver/gpio.h"

void w12_board_init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << W12_PIN_FEM_LF_PWR) | (1ULL << W12_PIN_FEM_HF_PWR) |
                        (1ULL << W12_PIN_VEXT_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(W12_PIN_FEM_LF_PWR, 1);
    gpio_set_level(W12_PIN_FEM_HF_PWR, 1);
    gpio_set_level(W12_PIN_VEXT_EN, 0);
}
