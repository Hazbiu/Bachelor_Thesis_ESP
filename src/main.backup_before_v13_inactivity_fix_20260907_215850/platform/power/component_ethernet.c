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

/* IEEE 802.3 Clause-22 / IP101G register definitions. */
#define IP101G_REG_PAGE_SELECT      20U
#define IP101G_REG_WOL_CONTROL      16U
#define IP101G_REG_WOL_STATUS       17U

#define IP101G_PAGE_WOL_CONTROL     4U
#define IP101G_PAGE_WOL_STATUS      17U
#define IP101G_PAGE_DEFAULT         16U

#define IP101G_WOL_PLUS_EN          (1U << 15)
#define IP101G_WOL_PLUS_MASTER      (1U << 14)
#define IP101G_WOL_SENSE_MAGIC      (1U << 11)
#define IP101G_WOL_SENSE_ANY        (1U << 10)
#define IP101G_WOL_SENSE_DUT        (1U << 9)
#define IP101G_WOL_DOWNSPEED_EN     (1U << 8)
#define IP101G_WOL_MANUAL_SET       (1U << 5)

#define IP101G_WOL_STATUS_SLEEPING  (1U << 2)
#define IP101G_WOL_STATUS_SLEEP_RDY (1U << 1)
#define IP101G_WOL_STATUS_WAKE_RDY  (1U << 0)

#define IP101G_MDIO_HALF_US         2U
#define IP101G_WOL_READY_TIMEOUT_MS 100U
#define IP101G_WOL_SLEEP_TIMEOUT_MS 100U
#define IP101G_WOL_WAKE_SETTLE_MS   20U

static const char *TAG = "component_ethernet";

static bool s_reset_asserted;
static bool s_user_enabled = true;
static bool s_light_wol_standby_active;
static bool s_wol_control_snapshot_valid;
static uint16_t s_wol_control_snapshot;


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


static esp_err_t ip101g_select_page(uint8_t page)
{
    return mdio_write_register(IP101G_REG_PAGE_SELECT, page);
}


static esp_err_t ip101g_read_paged(
    uint8_t page,
    uint8_t reg,
    uint16_t *value_out)
{
    esp_err_t ret = ip101g_select_page(page);
    if (ret != ESP_OK) {
        return ret;
    }
    return mdio_read_register(reg, value_out);
}


static esp_err_t ip101g_write_paged(
    uint8_t page,
    uint8_t reg,
    uint16_t value)
{
    esp_err_t ret = ip101g_select_page(page);
    if (ret != ESP_OK) {
        return ret;
    }
    return mdio_write_register(reg, value);
}


static void ip101g_restore_default_page(void)
{
    (void)ip101g_select_page(IP101G_PAGE_DEFAULT);
}


static esp_err_t ip101g_wait_status(
    uint16_t required_set,
    uint16_t required_clear,
    uint32_t timeout_ms,
    uint16_t *last_status_out)
{
    uint32_t waited_ms = 0;
    uint16_t status = 0;

    while (waited_ms <= timeout_ms) {
        esp_err_t ret = ip101g_read_paged(
            IP101G_PAGE_WOL_STATUS,
            IP101G_REG_WOL_STATUS,
            &status);
        if (ret != ESP_OK) {
            ip101g_restore_default_page();
            return ret;
        }

        if ((status & required_set) == required_set &&
            (status & required_clear) == 0U) {
            if (last_status_out != NULL) {
                *last_status_out = status;
            }
            ip101g_restore_default_page();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(5U));
        waited_ms += 5U;
    }

    if (last_status_out != NULL) {
        *last_status_out = status;
    }

    ip101g_restore_default_page();
    return ESP_ERR_TIMEOUT;
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


esp_err_t component_ethernet_enter_light_sleep_standby(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(
            TAG,
            "IP101GRI Light-sleep state follows saved Ethernet OFF policy: "
            "RESET GPIO%d LOW and held",
            ETHERNET_PHY_RESET_GPIO);
        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    /*
     * Keep the PHY alive. WOL+ slave mode is used as a true reduced-power PHY
     * state rather than RESET. The IP101G datasheet specifies that WOL+ sleep
     * down-speeds to 10 Mbps and can be exited by software.
     */
    esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t wol_control = 0;
    ret = ip101g_read_paged(
        IP101G_PAGE_WOL_CONTROL,
        IP101G_REG_WOL_CONTROL,
        &wol_control);
    if (ret != ESP_OK) {
        ip101g_restore_default_page();
        return ret;
    }

    s_wol_control_snapshot = wol_control;
    s_wol_control_snapshot_valid = true;

    /*
     * WOL+ slave, down-speed enabled.
     *
     * Packet-triggered PHY wake is deliberately disabled for this measurement.
     * The P4 has no Ethernet wake source in this application; allowing ordinary
     * LAN broadcasts to wake only the PHY would make Light-sleep current noisy
     * without waking the application. P4 wake remains touch/GPIO3.
     */
    uint16_t standby_control = wol_control;
    standby_control |= IP101G_WOL_PLUS_EN;
    standby_control |= IP101G_WOL_DOWNSPEED_EN;
    standby_control &= (uint16_t)~IP101G_WOL_PLUS_MASTER;
    standby_control &= (uint16_t)~IP101G_WOL_SENSE_MAGIC;
    standby_control &= (uint16_t)~IP101G_WOL_SENSE_ANY;
    standby_control &= (uint16_t)~IP101G_WOL_SENSE_DUT;
    standby_control &= (uint16_t)~IP101G_WOL_MANUAL_SET;

    ret = ip101g_write_paged(
        IP101G_PAGE_WOL_CONTROL,
        IP101G_REG_WOL_CONTROL,
        standby_control);
    if (ret != ESP_OK) {
        ip101g_restore_default_page();
        return ret;
    }

    uint16_t status = 0;
    ret = ip101g_wait_status(
        IP101G_WOL_STATUS_SLEEP_RDY,
        IP101G_WOL_STATUS_SLEEPING,
        IP101G_WOL_READY_TIMEOUT_MS,
        &status);

    /*
     * Some IP101G revisions accept the manual request even if the transient
     * "ready" status bit is missed by software. Continue with the documented
     * manual-set request, then require the actual SLEEPING bit as proof.
     */
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "IP101GRI WOL+ sleep-ready status was not observed "
            "(status=0x%04x); issuing manual sleep request anyway",
            status);
    }

    ret = ip101g_write_paged(
        IP101G_PAGE_WOL_CONTROL,
        IP101G_REG_WOL_CONTROL,
        (uint16_t)(standby_control | IP101G_WOL_MANUAL_SET));
    if (ret != ESP_OK) {
        ip101g_restore_default_page();
        return ret;
    }

    ret = ip101g_wait_status(
        IP101G_WOL_STATUS_SLEEPING,
        0U,
        IP101G_WOL_SLEEP_TIMEOUT_MS,
        &status);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "IP101GRI failed to enter WOL+ Light-sleep standby: "
            "status=0x%04x ret=%s",
            status,
            esp_err_to_name(ret));

        /* Restore the previous WOL+ control state before aborting Light-sleep. */
        if (s_wol_control_snapshot_valid) {
            (void)ip101g_write_paged(
                IP101G_PAGE_WOL_CONTROL,
                IP101G_REG_WOL_CONTROL,
                s_wol_control_snapshot);
        }
        ip101g_restore_default_page();
        return ret;
    }

    s_light_wol_standby_active = true;
    ip101g_restore_default_page();

    ESP_LOGI(
        TAG,
        "IP101GRI Light-sleep WOL+ standby ACTIVE: "
        "RESET=HIGH powered=YES downspeed=10Mbps packet_wake=DISABLED "
        "p4_wake=TOUCH_OR_GPIO3 status=0x%04x",
        status);

    return ESP_OK;
}


esp_err_t component_ethernet_restore_after_light_sleep(void)
{
    if (!s_user_enabled) {
        s_light_wol_standby_active = false;
        s_wol_control_snapshot_valid = false;
        return configure_phy_reset_level(
            ETHERNET_PHY_RESET_ACTIVE,
            true,
            true);
    }

    esp_err_t first_error = ESP_OK;

    if (s_light_wol_standby_active) {
        uint16_t current_control = 0;
        esp_err_t ret = ip101g_read_paged(
            IP101G_PAGE_WOL_CONTROL,
            IP101G_REG_WOL_CONTROL,
            &current_control);

        if (ret == ESP_OK) {
            /*
             * Datasheet-defined wake: disabling WOL+ returns the PHY to normal
             * operation. Clear MANUAL_SET because it is a self-clearing command.
             */
            ret = ip101g_write_paged(
                IP101G_PAGE_WOL_CONTROL,
                IP101G_REG_WOL_CONTROL,
                (uint16_t)((current_control &
                    (uint16_t)~IP101G_WOL_PLUS_EN) &
                    (uint16_t)~IP101G_WOL_MANUAL_SET));
        }

        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }

        vTaskDelay(pdMS_TO_TICKS(IP101G_WOL_WAKE_SETTLE_MS));

        if (s_wol_control_snapshot_valid) {
            ret = ip101g_write_paged(
                IP101G_PAGE_WOL_CONTROL,
                IP101G_REG_WOL_CONTROL,
                s_wol_control_snapshot);
            if (ret != ESP_OK && first_error == ESP_OK) {
                first_error = ret;
            }
        }

        ip101g_restore_default_page();
    }

    esp_err_t reset_ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false,
        true);
    if (reset_ret != ESP_OK && first_error == ESP_OK) {
        first_error = reset_ret;
    }

    s_light_wol_standby_active = false;
    s_wol_control_snapshot_valid = false;

    if (first_error == ESP_OK) {
        ESP_LOGI(
            TAG,
            "IP101GRI restored from Light-sleep WOL+ standby: "
            "RESET=HIGH normal PHY policy restored");
    }

    return first_error;
}


esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    /*
     * Deep-sleep remains the hard-off boundary. Even if Light-sleep used the
     * WOL+ 10 Mbps standby, assert hardware RESET LOW here.
     */
    s_light_wol_standby_active = false;
    s_wol_control_snapshot_valid = false;

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
    s_light_wol_standby_active = false;
    s_wol_control_snapshot_valid = false;

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
