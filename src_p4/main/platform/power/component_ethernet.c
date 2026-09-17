
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
#define IP101G_REG_PHYID1            2U
#define IP101G_REG_PHYID2            3U
#define IP101G_OUI                   0x0090C3U
#define IP101G_MODEL                 0x05U
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
#define IP101G_RESET_ASSERT_US       10000U
#define IP101G_RESET_RELEASE_US      10000U

static const char *TAG = "component_ethernet";

static bool s_reset_asserted;
static bool s_user_enabled = true;
static bool s_light_reduced_mode_active;
static bool s_light_reset_low_active;
static bool s_bmcr_snapshot_valid;
static uint16_t s_bmcr_snapshot;
static bool s_deep_bmcr_power_down;
static bool s_policy_initialized;


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
 * The Waveshare schematic provides a 1.5k external MDIO pull-up (R59).
 * Use open-drain MDIO to avoid driving against the PHY at turnaround.
 * The Deep-sleep path separately holds MDC idle LOW and disables the MDIO
 * input/output buffers; it never holds MDIO LOW against that pull-up.
 */
static inline void mdio_delay_half_period(void)
{
    esp_rom_delay_us(IP101G_MDIO_HALF_US);
}


static esp_err_t mdio_prepare_bus(void)
{
    esp_err_t ret = gpio_hold_dis(ETHERNET_MDC_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }
    gpio_config_t mdc_cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&mdc_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_config_t mdio_cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDIO_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&mdio_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(ETHERNET_MDC_GPIO, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    return gpio_set_level(ETHERNET_MDIO_GPIO, 1);
}


static void mdio_release_bus(void)
{
    /* Keep MDC quiet; MDIO is externally pulled HIGH. A floating input buffer
     * is unnecessary between management transactions, including Light-sleep. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_set_level(ETHERNET_MDC_GPIO, 0);
    (void)gpio_config(&cfg);
    (void)gpio_sleep_sel_dis(ETHERNET_MDC_GPIO);
    cfg.pin_bit_mask = 1ULL << ETHERNET_MDIO_GPIO;
    cfg.mode = GPIO_MODE_DISABLE;
    (void)gpio_config(&cfg);
    (void)gpio_sleep_sel_dis(ETHERNET_MDIO_GPIO);
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
    gpio_set_direction(ETHERNET_MDIO_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
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

    /*
     * The previous bit-bang reader consumed two samples after releasing MDIO.
     * On this board that eats D15 and shifts every 16-bit register left by one
     * (observed: 0x0243 -> 0x0487, 0x0c54 -> 0x18a9). Sample the PHY ACK once,
     * then read the 16 data bits.
     */
    gpio_set_direction(ETHERNET_MDIO_GPIO, GPIO_MODE_INPUT);
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


/* IP101 reset timings match ESP-IDF v5.5.4's IP101 PHY driver. */
static esp_err_t reset_phy_to_defaults(void)
{
    esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE, false, true);
    if (ret != ESP_OK) {
        return ret;
    }
    s_deep_bmcr_power_down = false;
    esp_rom_delay_us(IP101G_RESET_ASSERT_US);
    ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED, false, true);
    if (ret == ESP_OK) {
        esp_rom_delay_us(IP101G_RESET_RELEASE_US);
    }
    return ret;
}


static esp_err_t hold_deep_mdio_idle(void)
{
    esp_err_t ret = gpio_hold_dis(ETHERNET_MDC_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ETHERNET_MDC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gpio_set_level(ETHERNET_MDC_GPIO, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gpio_sleep_sel_dis(ETHERNET_MDC_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = gpio_hold_en(ETHERNET_MDC_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }
    cfg.pin_bit_mask = 1ULL << ETHERNET_MDIO_GPIO;
    cfg.mode = GPIO_MODE_DISABLE;
    ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    return gpio_sleep_sel_dis(ETHERNET_MDIO_GPIO);
}


static esp_err_t enter_deep_reset_state(void)
{
    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE, true, true);
    if (ret != ESP_OK) {
        return ret;
    }
    s_deep_bmcr_power_down = false;
    return hold_deep_mdio_idle();
}


static esp_err_t deep_reset_fallback(esp_err_t cause)
{
    const esp_err_t ret = enter_deep_reset_state();
    ESP_LOGW(TAG, "DS-OPT-2: Ethernet BMCR verification failed (%s); "
             "RESET_LOW fallback=%s. Register power-down is NOT verified.",
             esp_err_to_name(cause), esp_err_to_name(ret));
    /* Preserve the failure so callers do not report verified BMCR shutdown. */
    return ret == ESP_OK ? cause : ret;
}


static esp_err_t enter_light_reset_state(esp_err_t cause, const char *reason)
{
    s_light_reduced_mode_active = false;
    s_bmcr_snapshot_valid = false;

    const esp_err_t ret = enter_deep_reset_state();
    if (ret == ESP_OK) {
        s_light_reset_low_active = true;
        ESP_LOGW(
            TAG,
            "SLEEP-FIX-V3: Light Ethernet RESET_LOW reason=%s cause=%s",
            reason != NULL ? reason : "policy",
            esp_err_to_name(cause));
        return ESP_OK;
    }

    s_light_reset_low_active = false;
    ESP_LOGE(
        TAG,
        "SLEEP-FIX-V3: RESET_LOW fallback failed after %s: %s",
        esp_err_to_name(cause),
        esp_err_to_name(ret));
    return ret;
}


static esp_err_t identify_ip101(void)
{
    uint16_t id1 = 0, id2 = 0;
    esp_err_t ret = mdio_read_register(IP101G_REG_PHYID1, &id1);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mdio_read_register(IP101G_REG_PHYID2, &id2);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t oui = ((uint32_t)id1 << 6) | (id2 >> 10);
    const unsigned model = (id2 >> 4) & 0x3FU;
    ESP_LOGI(TAG, "DS-OPT-2: PHY%d ID1=0x%04x ID2=0x%04x OUI=0x%06" PRIx32
             " model=0x%02x", ETHERNET_PHY_ADDRESS, id1, id2, oui, model);
    return oui == IP101G_OUI && model == IP101G_MODEL
        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}


esp_err_t component_ethernet_enter_light_sleep_reduced_mode(void)
{
    if (!s_user_enabled) {
        s_light_reset_low_active = false;
        return enter_deep_reset_state();
    }
    if (s_light_reduced_mode_active || s_light_reset_low_active) {
        return ESP_OK;
    }
    if (s_bmcr_snapshot_valid) {
        return ESP_ERR_INVALID_STATE;
    }

#if APP_LIGHT_SLEEP_ETHERNET_RESET_LOW
    /*
     * Ethernet is not a wake source. Hardware RESET is reversible and keeps
     * the PHY off for the complete timer-sliced touchscreen Light-sleep window.
     */
    return enter_light_reset_state(ESP_OK, "configured");
#else
    esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED, true, true);
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "reset_release");
    }

    ret = identify_ip101();
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "phy_id");
    }

    uint16_t bmcr = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &bmcr);
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "bmcr_read");
    }
    if (bmcr == 0xFFFFU || (bmcr & IP101G_BMCR_RESET) != 0U) {
        return enter_light_reset_state(ESP_ERR_INVALID_RESPONSE, "bmcr_invalid");
    }
    s_bmcr_snapshot = bmcr;
    s_bmcr_snapshot_valid = true;

#if APP_LIGHT_SLEEP_ETHERNET_BMCR_POWER_DOWN
    const uint16_t requested = (bmcr | IP101G_BMCR_POWER_DOWN) &
        (uint16_t)~(IP101G_BMCR_RESET | IP101G_BMCR_RESTART_AUTONEG);
#else
    const uint16_t requested = bmcr & (uint16_t)~(
        IP101G_BMCR_RESET | IP101G_BMCR_LOOPBACK | IP101G_BMCR_SPEED_100 |
        IP101G_BMCR_AUTONEG_ENABLE | IP101G_BMCR_POWER_DOWN |
        IP101G_BMCR_ISOLATE | IP101G_BMCR_RESTART_AUTONEG |
        IP101G_BMCR_COLLISION_TEST);
#endif

    ret = mdio_write_register(IP101G_REG_BMCR, requested);
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "bmcr_write");
    }

    vTaskDelay(pdMS_TO_TICKS(IP101G_REDUCED_SETTLE_MS) + 1);
    uint16_t readback = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &readback);
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "bmcr_verify_read");
    }

    const uint16_t verify_mask = (uint16_t)~IP101G_BMCR_RESTART_AUTONEG;
    if ((readback & verify_mask) != (requested & verify_mask)) {
        ESP_LOGE(TAG, "Light PHY verify failed: requested=0x%04x actual=0x%04x",
                 requested, readback);
        return enter_light_reset_state(ESP_ERR_INVALID_STATE, "bmcr_verify");
    }

    ret = hold_deep_mdio_idle();
    if (ret != ESP_OK) {
        return enter_light_reset_state(ret, "mdio_idle");
    }

    s_light_reduced_mode_active = true;
    ESP_LOGI(TAG, "SLEEP-PWR: Light PHY=%s RESET=HIGH (no reset) "
             "BMCR_saved=0x%04x BMCR_sleep=0x%04x",
             APP_LIGHT_SLEEP_ETHERNET_BMCR_POWER_DOWN
                 ? "BMCR_POWER_DOWN" : "POWERED_10M", bmcr, readback);
    return ESP_OK;
#endif
}

esp_err_t component_ethernet_restore_after_light_sleep(void)
{
    if (!s_user_enabled) {
        s_light_reduced_mode_active = false;
        s_light_reset_low_active = false;
        s_bmcr_snapshot_valid = false;
        return enter_deep_reset_state();
    }

    if (s_light_reset_low_active) {
        s_light_reset_low_active = false;
        s_light_reduced_mode_active = false;
        s_bmcr_snapshot_valid = false;

        esp_err_t reset_ret = reset_phy_to_defaults();
        if (reset_ret != ESP_OK) {
            return reset_ret;
        }
        const esp_err_t bus_ret = gpio_hold_dis(ETHERNET_MDC_GPIO);
        if (bus_ret != ESP_OK && bus_ret != ESP_ERR_NOT_SUPPORTED) {
            return bus_ret;
        }
        mdio_release_bus();
        ESP_LOGI(TAG, "SLEEP-FIX-V3: Ethernet restored from RESET_LOW");
        return ESP_OK;
    }

    esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED, false, true);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Restore even when entry failed after a write but before verification. */
    if (s_bmcr_snapshot_valid) {
        uint16_t requested = s_bmcr_snapshot & (uint16_t)~IP101G_BMCR_RESET;
        if ((requested & IP101G_BMCR_AUTONEG_ENABLE) != 0U &&
            (requested & IP101G_BMCR_POWER_DOWN) == 0U) {
            requested |= IP101G_BMCR_RESTART_AUTONEG;
        }
        ret = mdio_write_register(IP101G_REG_BMCR, requested);
        if (ret != ESP_OK) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(IP101G_RESTORE_SETTLE_MS) + 1);
        uint16_t readback = 0;
        ret = mdio_read_register(IP101G_REG_BMCR, &readback);
        if (ret != ESP_OK) {
            return ret;
        }
        const uint16_t verify_mask = (uint16_t)~IP101G_BMCR_RESTART_AUTONEG;
        if ((readback & verify_mask) != (requested & verify_mask)) {
            ESP_LOGE(TAG, "Light PHY restore failed: expected=0x%04x actual=0x%04x",
                     requested, readback);
            return ESP_ERR_INVALID_STATE;
        }
        /* Retain the snapshot on every failed restore so recovery can retry. */
        s_bmcr_snapshot_valid = false;
    }
    s_light_reduced_mode_active = false;
    s_light_reset_low_active = false;
    ret = gpio_hold_dis(ETHERNET_MDC_GPIO);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }
    mdio_release_bus();
    ESP_LOGI(TAG, "SLEEP-PWR: saved Ethernet policy restored; RESET stayed HIGH");
    return ESP_OK;
}

esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    /* OFF stays OFF. Do not reboot a user-disabled PHY just to put it back
     * to sleep, which also used to replace RESET_LOW with RESET_HIGH. */
    if (!s_user_enabled || s_light_reset_low_active) {
        s_light_reduced_mode_active = false;
        s_light_reset_low_active = false;
        s_bmcr_snapshot_valid = false;
        return enter_deep_reset_state();
    }
#if APP_LIGHT_SLEEP_ETHERNET_BMCR_POWER_DOWN && APP_PWR_ETHERNET_DEEP_BMCR_POWER_DOWN
    if (s_light_reduced_mode_active) {
        uint16_t bmcr = 0;
        esp_err_t ret = mdio_read_register(IP101G_REG_BMCR, &bmcr);
        if (ret == ESP_OK && bmcr != 0xFFFFU &&
            (bmcr & (IP101G_BMCR_POWER_DOWN | IP101G_BMCR_RESET)) ==
                IP101G_BMCR_POWER_DOWN) {
            ret = configure_phy_reset_level(ETHERNET_PHY_RESET_RELEASED, true, true);
            if (ret == ESP_OK) {
                ret = hold_deep_mdio_idle();
            }
            if (ret == ESP_OK) {
                s_light_reduced_mode_active = false;
                s_bmcr_snapshot_valid = false;
                s_deep_bmcr_power_down = true;
                ESP_LOGI(TAG, "SLEEP-PWR: Light->Deep retains PHY POWER_DOWN; no reset");
                return ESP_OK;
            }
        }
        /* Reuse failed: take the existing verified Deep-sleep shutdown path. */
    }
#endif
    s_light_reduced_mode_active = false;
    s_bmcr_snapshot_valid = false;

#if APP_PWR_ETHERNET_DEEP_BMCR_POWER_DOWN
    /* Reset first so a previous reduced-mode/register-page state cannot
     * change the meaning of the standard Clause-22 registers below. */
    esp_err_t ret = reset_phy_to_defaults();
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    ret = identify_ip101();
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }

    uint16_t bmcr = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &bmcr);
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    if (bmcr == 0xFFFFU || (bmcr & IP101G_BMCR_RESET) != 0U) {
        return deep_reset_fallback(ESP_ERR_INVALID_RESPONSE);
    }
    const uint16_t requested = (bmcr | IP101G_BMCR_POWER_DOWN) &
        (uint16_t)~(IP101G_BMCR_RESET | IP101G_BMCR_RESTART_AUTONEG);
    ret = mdio_write_register(IP101G_REG_BMCR, requested);
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    vTaskDelay(pdMS_TO_TICKS(IP101G_REDUCED_SETTLE_MS));

    uint16_t readback = 0;
    ret = mdio_read_register(IP101G_REG_BMCR, &readback);
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    if ((readback & (IP101G_BMCR_POWER_DOWN | IP101G_BMCR_RESET)) !=
            IP101G_BMCR_POWER_DOWN) {
        return deep_reset_fallback(ESP_ERR_INVALID_STATE);
    }

    /* Asserting RESET here would erase BMCR. Retain HIGH deliberately.
     * DS-RETENTION-1 must retain this pad once the P4 HP domain turns off. */
    ret = configure_phy_reset_level(ETHERNET_PHY_RESET_RELEASED, true, true);
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    ret = hold_deep_mdio_idle();
    if (ret != ESP_OK) {
        return deep_reset_fallback(ret);
    }
    s_deep_bmcr_power_down = true;
    ESP_LOGI(TAG, "DS-OPT-2: PHY BMCR_POWER_DOWN verified: before=0x%04x "
             "readback=0x%04x RESET=HIGH held MDC=LOW MDIO=Hi-Z; "
             "scope=PRE_ENTRY_ONLY", bmcr, readback);
    return ESP_OK;
#else
    const esp_err_t ret = enter_deep_reset_state();
    ESP_LOGI(TAG, "DS-OPT-2: Ethernet comparison policy=RESET_LOW result=%s",
             esp_err_to_name(ret));
    return ret;
#endif
}


esp_err_t component_ethernet_verify_power_down(void)
{
    const int level = gpio_get_level(ETHERNET_PHY_RESET_GPIO);
    const int expected = component_ethernet_deep_sleep_reset_level();
    if (level != expected) {
        return deep_reset_fallback(ESP_ERR_INVALID_STATE);
    }

    if (s_deep_bmcr_power_down) {
        uint16_t bmcr = 0;
        esp_err_t ret = mdio_read_register(IP101G_REG_BMCR, &bmcr);
        if (ret != ESP_OK) {
            return deep_reset_fallback(ret);
        }
        if ((bmcr & (IP101G_BMCR_POWER_DOWN | IP101G_BMCR_RESET)) !=
                IP101G_BMCR_POWER_DOWN) {
            return deep_reset_fallback(ESP_ERR_INVALID_STATE);
        }
        ret = hold_deep_mdio_idle();
        if (ret != ESP_OK) {
            return deep_reset_fallback(ret);
        }
        ESP_LOGI(TAG, "DS-OPT-2: PHY final audit BMCR=0x%04x "
                 "POWER_DOWN=1 RESET=HIGH; scope=PRE_ENTRY_ONLY", bmcr);
    } else {
        const esp_err_t ret = hold_deep_mdio_idle();
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGI(TAG, "DS-OPT-2: PHY final audit RESET=LOW; "
                 "BMCR power-down not used; scope=PRE_ENTRY_ONLY");
    }

    return ESP_OK;
}


int component_ethernet_deep_sleep_reset_level(void)
{
    return s_deep_bmcr_power_down
        ? ETHERNET_PHY_RESET_RELEASED : ETHERNET_PHY_RESET_ACTIVE;
}


const char *component_ethernet_deep_sleep_state(void)
{
    if (s_deep_bmcr_power_down) {
        return "BMCR_POWER_DOWN";
    }
    return s_reset_asserted ? "RESET_LOW" : "NOT_PREPARED";
}


esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    /* Reapply the saved policy, clearing retained POWER_DOWN when enabled. */
    return component_ethernet_set_enabled(s_user_enabled);
}


esp_err_t component_ethernet_set_enabled(bool enabled)
{
    const bool previous_policy = s_user_enabled;
    s_user_enabled = enabled;
    s_light_reduced_mode_active = false;
    s_light_reset_low_active = false;
    s_bmcr_snapshot_valid = false;

    /* Deep-sleep restarts the P4, but not the externally powered PHY.
     * On first policy application, reset a retained BMCR before enabling.
     * Later idempotent ON calls leave an already-active PHY undisturbed. */
    const bool needs_reset = enabled &&
        (!s_policy_initialized || s_deep_bmcr_power_down || s_reset_asserted);
    const esp_err_t ret = needs_reset
        ? reset_phy_to_defaults()
        : configure_phy_reset_level(
              enabled ? ETHERNET_PHY_RESET_RELEASED : ETHERNET_PHY_RESET_ACTIVE,
              !enabled, true);

    if (ret != ESP_OK) {
        s_user_enabled = previous_policy;
        return ret;
    }
    s_policy_initialized = true;
    s_deep_bmcr_power_down = false;

    const esp_err_t bus_ret = gpio_hold_dis(ETHERNET_MDC_GPIO);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_NOT_SUPPORTED) {
        return bus_ret;
    }
    mdio_release_bus();

    ESP_LOGI(
        TAG,
        "IP101GRI saved policy applied: %s",
        enabled ? "ENABLED (RESET released)" : "DISABLED (RESET LOW held)");

    return ESP_OK;
}


bool component_ethernet_is_enabled(void)
{
    return s_user_enabled && !s_reset_asserted && !s_deep_bmcr_power_down;
}
