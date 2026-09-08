#include "platform/power/component_ethernet.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/app_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ETHERNET_PHY_RESET_GPIO     APP_PWR_ETHERNET_PHY_RESET_GPIO
#define ETHERNET_PHY_RESET_ACTIVE   APP_PWR_ETHERNET_RESET_ACTIVE_LEVEL
#define ETHERNET_PHY_RESET_RELEASED APP_PWR_ETHERNET_RESET_RELEASED_LEVEL

#define ETHERNET_MDC_GPIO           APP_PWR_ETHERNET_MDC_GPIO
#define ETHERNET_MDIO_GPIO          APP_PWR_ETHERNET_MDIO_GPIO
#define ETHERNET_PHY_ADDRESS        APP_PWR_ETHERNET_PHY_ADDRESS

/* IP101G standard IEEE 802.3 MII Control Register (BMCR, register 0). */
#define IP101G_REG_BMCR              0U
#define IP101G_BMCR_RESET            (1U << 15)
#define IP101G_BMCR_LOOPBACK         (1U << 14)
#define IP101G_BMCR_SPEED_100        (1U << 13)
#define IP101G_BMCR_AUTONEG_ENABLE   (1U << 12)
#define IP101G_BMCR_POWER_DOWN       (1U << 11)
#define IP101G_BMCR_ISOLATE          (1U << 10)
#define IP101G_BMCR_RESTART_AUTONEG  (1U << 9)
#define IP101G_BMCR_DUPLEX_FULL      (1U << 8)
#define IP101G_BMCR_COLLISION_TEST   (1U << 7)

#define IP101G_MDIO_HALF_US          2U
#define IP101G_REDUCED_SETTLE_MS     20U
#define IP101G_RESTORE_SETTLE_MS     20U

static const char *TAG = "component_ethernet";

static bool s_reset_asserted;
static bool s_user_enabled = true;
static bool s_light_reduced_mode_active;
static bool s_bmcr_snapshot_valid;
static uint16_t s_bmcr_snapshot;


static int wait_for_pad_level(
    gpio_num_t gpio_num,
    int expected_level,
    uint32_t *settle_ms_out)
{
    uint32_t waited_ms = 0;
    int level = gpio_get_level(gpio_num);

    while (level != expected_level &&
           waited_ms < APP_PWR_HOLD_VERIFY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(APP_PWR_HOLD_VERIFY_POLL_MS));
        waited_ms += APP_PWR_HOLD_VERIFY_POLL_MS;
        level = gpio_get_level(gpio_num);
    }

    if (settle_ms_out != NULL) {
        *settle_ms_out = waited_ms;
    }

    return level;
}


/*
 * Minimal Clause-22 MDC/MDIO bit-bang helper.
 *
 * The current application does not own an esp_eth MAC object, but the board
 * exposes the IP101GRI management pins directly:
 *     MDC  = GPIO31
 *     MDIO = GPIO52
 *
 * We use the pins only at the Light-sleep boundary, then return them to
 * INPUT/no-pull so they are not continuously driven while the P4 sleeps.
 */
static inline void mdio_delay_half_period(void)
{
    esp_rom_delay_us(IP101G_MDIO_HALF_US);
}


static esp_err_t mdio_prepare_bus(void)
{
    gpio_config_t mdc_cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&mdc_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_config_t mdio_cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDIO_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&mdio_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_set_level(ETHERNET_MDC_GPIO, 0);
    gpio_set_level(ETHERNET_MDIO_GPIO, 1);
    return ESP_OK;
}


static void mdio_release_bus(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask =
            (1ULL << ETHERNET_MDC_GPIO) |
            (1ULL << ETHERNET_MDIO_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);
}


static void mdio_write_bit(int bit)
{
    gpio_set_level(ETHERNET_MDIO_GPIO, bit ? 1 : 0);
    mdio_delay_half_period();
    gpio_set_level(ETHERNET_MDC_GPIO, 1);
    mdio_delay_half_period();
    gpio_set_level(ETHERNET_MDC_GPIO, 0);
}


static int mdio_read_bit(void)
{
    mdio_delay_half_period();
    gpio_set_level(ETHERNET_MDC_GPIO, 1);
    mdio_delay_half_period();
    const int bit = gpio_get_level(ETHERNET_MDIO_GPIO);
    gpio_set_level(ETHERNET_MDC_GPIO, 0);
    return bit;
}


static void mdio_write_bits(uint32_t value, unsigned bit_count)
{
    for (int bit = (int)bit_count - 1; bit >= 0; --bit) {
        mdio_write_bit((value >> bit) & 1U);
    }
}


static void mdio_preamble(void)
{
    gpio_set_direction(ETHERNET_MDIO_GPIO, GPIO_MODE_INPUT_OUTPUT);
    for (unsigned i = 0; i < 32U; ++i) {
        mdio_write_bit(1);
    }
}


static esp_err_t mdio_write_register(uint8_t reg, uint16_t value)
{
    esp_err_t ret = mdio_prepare_bus();
    if (ret != ESP_OK) {
        return ret;
    }

    mdio_preamble();

    /* ST=01, OP=01(write), PHYAD, REGAD, TA=10, DATA */
    mdio_write_bits(0x1U, 2U);
    mdio_write_bits(0x1U, 2U);
    mdio_write_bits((uint32_t)ETHERNET_PHY_ADDRESS & 0x1FU, 5U);
    mdio_write_bits((uint32_t)reg & 0x1FU, 5U);
    mdio_write_bits(0x2U, 2U);
    mdio_write_bits(value, 16U);

    gpio_set_level(ETHERNET_MDIO_GPIO, 1);
    mdio_delay_half_period();
    mdio_release_bus();
    return ESP_OK;
}


static esp_err_t mdio_read_register(uint8_t reg, uint16_t *value_out)
{
    if (value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mdio_prepare_bus();
    if (ret != ESP_OK) {
        return ret;
    }

    mdio_preamble();

    /* ST=01, OP=10(read), PHYAD, REGAD */
    mdio_write_bits(0x1U, 2U);
    mdio_write_bits(0x2U, 2U);
    mdio_write_bits((uint32_t)ETHERNET_PHY_ADDRESS & 0x1FU, 5U);
    mdio_write_bits((uint32_t)reg & 0x1FU, 5U);

    /* Read turnaround is Z0: release MDIO for both turnaround clocks. */
    gpio_set_direction(ETHERNET_MDIO_GPIO, GPIO_MODE_INPUT);
    (void)mdio_read_bit();
    const int turnaround = mdio_read_bit();
    if (turnaround != 0) {
        mdio_release_bus();
        ESP_LOGE(
            TAG,
            "IP101GRI MDIO read failed: PHY%d reg=%u turnaround=%d expected=0",
            ETHERNET_PHY_ADDRESS,
            (unsigned)reg,
            turnaround);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t value = 0;
    for (unsigned i = 0; i < 16U; ++i) {
        value = (uint16_t)((value << 1) | (uint16_t)mdio_read_bit());
    }

    mdio_release_bus();
    *value_out = value;
    return ESP_OK;
}



static esp_err_t configure_phy_reset_level(
    int level,
    bool hold,
    bool wait_for_settle)
{
    esp_err_t ret = gpio_hold_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(
            TAG,
            "Could not release previous GPIO%d hold: %s",
            ETHERNET_PHY_RESET_GPIO,
            esp_err_to_name(ret));
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(ETHERNET_PHY_RESET_GPIO, level);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_sleep_sel_dis(ETHERNET_PHY_RESET_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }

    if (hold) {
        ret = gpio_hold_en(ETHERNET_PHY_RESET_GPIO);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    uint32_t settle_ms = 0;
    const int readback = wait_for_settle
        ? wait_for_pad_level(
              ETHERNET_PHY_RESET_GPIO,
              level,
              &settle_ms)
        : gpio_get_level(ETHERNET_PHY_RESET_GPIO);

    if (readback != level) {
        ESP_LOGE(
            TAG,
            "IP101GRI RESET read-back=%d expected=%d after %" PRIu32 " ms",
            readback,
            level,
            settle_ms);
        return ESP_ERR_INVALID_STATE;
    }

    s_reset_asserted = (level == ETHERNET_PHY_RESET_ACTIVE);

    ESP_LOGI(
        TAG,
        "IP101GRI RESET GPIO%d=%d hold=%s readback=%d settle_ms=%" PRIu32,
        ETHERNET_PHY_RESET_GPIO,
        level,
        hold ? "ON" : "OFF",
        readback,
        settle_ms);

    return ESP_OK;
}


esp_err_t component_ethernet_enter_light_sleep_reduced_mode(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(
            TAG,
            "IP101GRI Light-sleep follows saved Ethernet OFF policy: "
            "RESET GPIO%d LOW and held",
            ETHERNET_PHY_RESET_GPIO);

        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    /*
     * Keep the PHY powered and out of reset. The reduced Light-sleep policy
     * changes only the standard MII Control Register.
     */
    esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t bmcr = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &bmcr);
    if (ret != ESP_OK) {
        return ret;
    }

    s_bmcr_snapshot = bmcr;
    s_bmcr_snapshot_valid = true;

    /*
     * Force 10 Mbps immediately:
     * SPEED_100=0, AUTO_NEGOTIATE=0, POWER_DOWN=0, ISOLATE=0.
     * Keep the currently reflected duplex bit.
     */
    uint16_t reduced_bmcr = bmcr;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_RESET;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_LOOPBACK;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_SPEED_100;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_AUTONEG_ENABLE;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_POWER_DOWN;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_ISOLATE;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_RESTART_AUTONEG;
    reduced_bmcr &= (uint16_t)~IP101G_BMCR_COLLISION_TEST;

    ret = mdio_write_register(IP101G_REG_BMCR, reduced_bmcr);
    if (ret != ESP_OK) {
        s_bmcr_snapshot_valid = false;
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(IP101G_REDUCED_SETTLE_MS));

    uint16_t readback = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &readback);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint16_t forbidden =
        IP101G_BMCR_SPEED_100 |
        IP101G_BMCR_AUTONEG_ENABLE |
        IP101G_BMCR_POWER_DOWN |
        IP101G_BMCR_ISOLATE;

    if ((readback & forbidden) != 0U) {
        ESP_LOGE(
            TAG,
            "IP101GRI reduced-mode verification failed: "
            "BMCR before=0x%04x requested=0x%04x readback=0x%04x",
            bmcr,
            reduced_bmcr,
            readback);

        (void)mdio_write_register(IP101G_REG_BMCR, s_bmcr_snapshot);
        s_bmcr_snapshot_valid = false;
        return ESP_ERR_INVALID_STATE;
    }

    s_light_reduced_mode_active = true;

    ESP_LOGI(
        TAG,
        "IP101GRI Light-sleep reduced mode ACTIVE: "
        "RESET=HIGH powered=YES speed=10Mbps autoneg=OFF duplex=%s "
        "BMCR_before=0x%04x BMCR_now=0x%04x",
        (readback & IP101G_BMCR_DUPLEX_FULL) ? "FULL" : "HALF",
        bmcr,
        readback);

    return ESP_OK;
}


esp_err_t component_ethernet_restore_after_light_sleep(void)
{
    if (!s_user_enabled) {
        s_light_reduced_mode_active = false;
        s_bmcr_snapshot_valid = false;

        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    esp_err_t first_error = ESP_OK;

    if (s_light_reduced_mode_active && s_bmcr_snapshot_valid) {
        uint16_t restore_bmcr = s_bmcr_snapshot;

        /*
         * A forced-speed interval breaks the previous negotiated link. If the
         * saved policy used auto-negotiation, restart it explicitly on wake.
         */
        if ((restore_bmcr & IP101G_BMCR_AUTONEG_ENABLE) != 0U) {
            restore_bmcr |= IP101G_BMCR_RESTART_AUTONEG;
        }

        esp_err_t ret =
            mdio_write_register(IP101G_REG_BMCR, restore_bmcr);

        if (ret != ESP_OK) {
            first_error = ret;
        } else {
            vTaskDelay(pdMS_TO_TICKS(IP101G_RESTORE_SETTLE_MS));

            uint16_t readback = 0;
            ret = mdio_read_register(IP101G_REG_BMCR, &readback);

            if (ret != ESP_OK) {
                first_error = ret;
            } else {
                ESP_LOGI(
                    TAG,
                    "IP101GRI BMCR restored after Light-sleep: "
                    "saved=0x%04x readback=0x%04x autoneg=%s",
                    s_bmcr_snapshot,
                    readback,
                    (s_bmcr_snapshot & IP101G_BMCR_AUTONEG_ENABLE)
                        ? "ON_RESTARTED"
                        : "OFF");
            }
        }
    }

    esp_err_t reset_ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);

    if (reset_ret != ESP_OK && first_error == ESP_OK) {
        first_error = reset_ret;
    }

    s_light_reduced_mode_active = false;
    s_bmcr_snapshot_valid = false;

    if (first_error == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI restored from Light-sleep reduced mode: "
            "RESET=HIGH normal PHY policy restored");
    }

    return first_error;
}


esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    /*
     * Deep-sleep remains the hard-off boundary. Even if Light-sleep used the
     * powered 10 Mbps reduced mode, assert hardware RESET LOW here.
     */
    s_light_reduced_mode_active = false;
    s_bmcr_snapshot_valid = false;

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE,
        true,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI Deep-sleep state: hardware RESET GPIO%d LOW and held; "
            "BMCR wake/reprogram step intentionally skipped",
            ETHERNET_PHY_RESET_GPIO);
    }

    return ret;
}


esp_err_t component_ethernet_verify_power_down(void)
{
    const int level = gpio_get_level(ETHERNET_PHY_RESET_GPIO);

    if (level != ETHERNET_PHY_RESET_ACTIVE) {
        ESP_LOGE(
            TAG,
            "IP101GRI Deep-sleep verification failed: RESET GPIO%d=%d "
            "expected=%d",
            ETHERNET_PHY_RESET_GPIO,
            level,
            ETHERNET_PHY_RESET_ACTIVE);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI final audit OK: hardware RESET GPIO%d LOW",
        ETHERNET_PHY_RESET_GPIO);

    return ESP_OK;
}


esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(
            TAG,
            "IP101GRI remains in hardware RESET because saved Ethernet policy is OFF");
        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI hardware RESET released after failed Deep-sleep entry");
    }

    return ret;
}


esp_err_t component_ethernet_set_enabled(bool enabled)
{
    const bool previous_policy = s_user_enabled;
    s_user_enabled = enabled;
    s_light_reduced_mode_active = false;
    s_bmcr_snapshot_valid = false;

    const esp_err_t ret = configure_phy_reset_level(
        enabled ? ETHERNET_PHY_RESET_RELEASED : ETHERNET_PHY_RESET_ACTIVE,
        !enabled,
        true);

    if (ret != ESP_OK) {
        s_user_enabled = previous_policy;
        return ret;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI saved policy applied: %s",
        enabled ? "ENABLED (RESET released)" : "DISABLED (RESET LOW held)");

    return ESP_OK;
}


bool component_ethernet_is_enabled(void)
{
    return s_user_enabled && !s_reset_asserted;
}
