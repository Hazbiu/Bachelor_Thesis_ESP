#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Temporary ESP32-P4 -> ESP32-C6 raw UART bridge
 *
 * Existing board USB-C -> on-board USB-UART -> ESP32-P4 UART0
 *
 * Temporary wires:
 *   P4 GPIO20 (UART1 TX) -> C6_U0RXD
 *   P4 GPIO21 (UART1 RX) <- C6_U0TXD
 *
 * Existing board control:
 *   P4 GPIO54 -> C6 CHIP_PU
 *
 * Manual C6 boot strap:
 *   C6_IO9 -> GND
 *
 * No logs are printed on UART0. Every PC byte goes to the C6 and every C6
 * byte goes back to the PC, so the laptop's normal esptool.py can talk to the
 * C6 through the board's existing /dev/ttyACM0 USB-C connection.
 */

#define PC_UART              UART_NUM_0
#define PC_UART_TX_GPIO      GPIO_NUM_37
#define PC_UART_RX_GPIO      GPIO_NUM_38

#define C6_UART              UART_NUM_1
#define C6_UART_TX_GPIO      GPIO_NUM_20
#define C6_UART_RX_GPIO      GPIO_NUM_21

#define C6_CHIP_PU_GPIO      GPIO_NUM_54

#define BRIDGE_BAUD          115200
#define BRIDGE_BUF_SIZE      4096

static void configure_uart(
    uart_port_t port,
    gpio_num_t tx_gpio,
    gpio_num_t rx_gpio)
{
    uart_config_t config = {
        .baud_rate = BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(port, &config));
    ESP_ERROR_CHECK(
        uart_set_pin(
            port,
            tx_gpio,
            rx_gpio,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(
        uart_driver_install(
            port,
            BRIDGE_BUF_SIZE,
            BRIDGE_BUF_SIZE,
            0,
            NULL,
            0));
}

static void reset_c6_into_rom_download_mode(void)
{
    /*
     * C6_IO9 is physically tied to GND by the user's jumper.
     * LOW -> HIGH on CHIP_PU makes the C6 sample IO9 LOW and enter the ROM
     * serial bootloader.
     */
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << C6_CHIP_PU_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_ERROR_CHECK(gpio_set_level(C6_CHIP_PU_GPIO, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(gpio_set_level(C6_CHIP_PU_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(250));
}

typedef struct {
    uart_port_t source;
    uart_port_t destination;
} bridge_direction_t;

static void bridge_task(void *argument)
{
    const bridge_direction_t direction =
        *(const bridge_direction_t *)argument;

    uint8_t buffer[512];

    for (;;) {
        const int received = uart_read_bytes(
            direction.source,
            buffer,
            sizeof(buffer),
            portMAX_DELAY);

        if (received <= 0) {
            continue;
        }

        int written_total = 0;
        while (written_total < received) {
            const int written = uart_write_bytes(
                direction.destination,
                buffer + written_total,
                received - written_total);

            if (written <= 0) {
                break;
            }

            written_total += written;
        }
    }
}

void app_main(void)
{
    configure_uart(
        PC_UART,
        PC_UART_TX_GPIO,
        PC_UART_RX_GPIO);

    configure_uart(
        C6_UART,
        C6_UART_TX_GPIO,
        C6_UART_RX_GPIO);

    reset_c6_into_rom_download_mode();

    static const bridge_direction_t pc_to_c6 = {
        .source = PC_UART,
        .destination = C6_UART,
    };

    static const bridge_direction_t c6_to_pc = {
        .source = C6_UART,
        .destination = PC_UART,
    };

    BaseType_t ok;

    ok = xTaskCreate(
        bridge_task,
        "pc_to_c6",
        3072,
        (void *)&pc_to_c6,
        10,
        NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ok = xTaskCreate(
        bridge_task,
        "c6_to_pc",
        3072,
        (void *)&c6_to_pc,
        10,
        NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
