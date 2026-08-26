#include "power_save/component_ethernet.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
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

#define ETHERNET_PHY_MDC_GPIO       APP_PWR_ETHERNET_MDC_GPIO
#define ETHERNET_PHY_MDIO_GPIO      APP_PWR_ETHERNET_MDIO_GPIO
#define ETHERNET_PHY_ADDRESS        APP_PWR_ETHERNET_PHY_ADDRESS

#define MII_BMCR_REG                0x00
#define MII_PHY_ID1_REG             0x02
#define MII_PHY_ID2_REG             0x03
#define MII_BMCR_POWER_DOWN         0x0800

/*
 * IEEE 802.3 Clause-22 permits MDC up to 2.5 MHz. A 1 us low/high software
 * half-period is intentionally conservative; the management transaction runs
 * only at sleep entry and does not affect Active-mode performance.
 */
#define MDIO_HALF_PERIOD_US         1U
#define PHY_RESET_RELEASE_MS        5U

static const char *TAG = "component_ethernet";

static bool s_light_reset_asserted;
static bool s_phy_powered_down;
static bool s_original_bmcr_valid;
static uint16_t s_original_bmcr;
static bool s_user_enabled = true;

/* -------------------------------------------------------------------------- */
/* GPIO helpers                                                               */
/* -------------------------------------------------------------------------- */

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

static esp_err_t configure_phy_reset_level(int level, bool hold)
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
    const int readback = wait_for_pad_level(
        ETHERNET_PHY_RESET_GPIO,
        level,
        &settle_ms);

    if (readback != level) {
        ESP_LOGE(
            TAG,
            "IP101GRI RESET read-back=%d expected=%d after %" PRIu32 " ms",
            readback,
            level,
            settle_ms);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t release_phy_reset_for_mdio(void)
{
    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release IP101GRI RESET before MDIO: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * If this follows a Light-sleep reset assertion, let the PHY management
     * interface settle before the first Clause-22 preamble.
     */
    vTaskDelay(pdMS_TO_TICKS(PHY_RESET_RELEASE_MS));
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Minimal Clause-22 SMI master                                               */
/* -------------------------------------------------------------------------- */

static esp_err_t configure_smi_bus(void)
{
    gpio_config_t mdc_config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_MDC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&mdc_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(ETHERNET_PHY_MDC_GPIO, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_config_t mdio_config = {
        .pin_bit_mask = 1ULL << ETHERNET_PHY_MDIO_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        /*
         * Logic-1 on MDIO is a released open-drain bus. The internal pull-up is
         * enabled only during this short transaction and is disabled again
         * before Deep-sleep.
         */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&mdio_config);
    if (ret != ESP_OK) {
        return ret;
    }

    return gpio_set_level(ETHERNET_PHY_MDIO_GPIO, 1);
}

static void release_phy_management_pins(void)
{
    /*
     * BMCR Power Down is retained while PHY_3V3 remains present. Leave MDC,
     * MDIO and RESET high-impedance so the sleeping P4 neither drives nor
     * back-powers the externally powered PHY.
     */
    (void)gpio_hold_dis(ETHERNET_PHY_RESET_GPIO);

    gpio_config_t input_config = {
        .pin_bit_mask =
            (1ULL << ETHERNET_PHY_MDC_GPIO) |
            (1ULL << ETHERNET_PHY_MDIO_GPIO) |
            (1ULL << ETHERNET_PHY_RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const esp_err_t ret = gpio_config(&input_config);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not float Ethernet management pins: %s",
            esp_err_to_name(ret));
    }

    /*
     * Keep the active high-impedance configuration instead of asking the
     * digital sleep selector to substitute another output state.
     */
    (void)gpio_sleep_sel_dis(ETHERNET_PHY_MDC_GPIO);
    (void)gpio_sleep_sel_dis(ETHERNET_PHY_MDIO_GPIO);
    (void)gpio_sleep_sel_dis(ETHERNET_PHY_RESET_GPIO);
}

static void mdc_clock_write(void)
{
    esp_rom_delay_us(MDIO_HALF_PERIOD_US);
    (void)gpio_set_level(ETHERNET_PHY_MDC_GPIO, 1);
    esp_rom_delay_us(MDIO_HALF_PERIOD_US);
    (void)gpio_set_level(ETHERNET_PHY_MDC_GPIO, 0);
}

static void mdio_write_bit(int bit)
{
    (void)gpio_set_level(ETHERNET_PHY_MDIO_GPIO, bit ? 1 : 0);
    mdc_clock_write();
}

static int mdio_read_bit(void)
{
    esp_rom_delay_us(MDIO_HALF_PERIOD_US);
    (void)gpio_set_level(ETHERNET_PHY_MDC_GPIO, 1);
    esp_rom_delay_us(MDIO_HALF_PERIOD_US);

    const int bit = gpio_get_level(ETHERNET_PHY_MDIO_GPIO);

    (void)gpio_set_level(ETHERNET_PHY_MDC_GPIO, 0);
    return bit;
}

static void mdio_write_bits(uint32_t value, unsigned bit_count)
{
    for (int bit = (int)bit_count - 1; bit >= 0; --bit) {
        mdio_write_bit((value >> bit) & 1U);
    }
}

static esp_err_t mdio_clause22_read(
    uint8_t phy_addr,
    uint8_t reg_addr,
    uint16_t *value_out)
{
    if (value_out == NULL || phy_addr > 31U || reg_addr > 31U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gpio_set_direction(
        ETHERNET_PHY_MDIO_GPIO,
        GPIO_MODE_INPUT_OUTPUT_OD);
    if (ret != ESP_OK) {
        return ret;
    }

    (void)gpio_set_level(ETHERNET_PHY_MDIO_GPIO, 1);

    /* PRE=32 ones, ST=01, OP=10(read), PHYAD, REGAD. */
    mdio_write_bits(UINT32_MAX, 32);
    mdio_write_bits(0x1U, 2);
    mdio_write_bits(0x2U, 2);
    mdio_write_bits(phy_addr, 5);
    mdio_write_bits(reg_addr, 5);

    /*
     * Clause-22 read turnaround is Z0, but after the MAC releases MDIO the
     * first clocked sample is the PHY-driven TA=0.  The high-impedance Z
     * interval is the bus hand-off itself; it must NOT be consumed as an extra
     * sampled clock.
     *
     * V17 sampled two turnaround clocks.  That shifted every 16-bit PHY read
     * left by one and appended the idle '1' bit.  The captured BMCR 0x6201 was
     * therefore the correctly expected 0x3100 shifted left once, which also
     * made post-write verification report 0xD001.
     *
     * This sequence matches the established Linux mdio-bitbang algorithm:
     *   release MDIO -> sample one TA=0 -> sample 16 data bits -> idle flush.
     */
    ret = gpio_set_direction(ETHERNET_PHY_MDIO_GPIO, GPIO_MODE_INPUT);
    if (ret != ESP_OK) {
        return ret;
    }

    const int turnaround = mdio_read_bit();

    uint16_t value = 0;
    for (int bit = 15; bit >= 0; --bit) {
        value |= (uint16_t)(mdio_read_bit() & 1) << bit;
    }

    /* One trailing idle clock leaves the management bus in a clean state. */
    (void)mdio_read_bit();

    ret = gpio_set_direction(
        ETHERNET_PHY_MDIO_GPIO,
        GPIO_MODE_INPUT_OUTPUT_OD);
    if (ret == ESP_OK) {
        (void)gpio_set_level(ETHERNET_PHY_MDIO_GPIO, 1);
    }

    if (turnaround != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *value_out = value;
    return ret;
}

static esp_err_t mdio_clause22_write(
    uint8_t phy_addr,
    uint8_t reg_addr,
    uint16_t value)
{
    if (phy_addr > 31U || reg_addr > 31U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gpio_set_direction(
        ETHERNET_PHY_MDIO_GPIO,
        GPIO_MODE_INPUT_OUTPUT_OD);
    if (ret != ESP_OK) {
        return ret;
    }

    (void)gpio_set_level(ETHERNET_PHY_MDIO_GPIO, 1);

    /* PRE=32 ones, ST=01, OP=01(write), PHYAD, REGAD, TA=10, DATA. */
    mdio_write_bits(UINT32_MAX, 32);
    mdio_write_bits(0x1U, 2);
    mdio_write_bits(0x1U, 2);
    mdio_write_bits(phy_addr, 5);
    mdio_write_bits(reg_addr, 5);
    mdio_write_bits(0x2U, 2);
    mdio_write_bits(value, 16);

    (void)gpio_set_level(ETHERNET_PHY_MDIO_GPIO, 1);
    return ESP_OK;
}

static esp_err_t begin_phy_management(void)
{
    esp_err_t ret = release_phy_reset_for_mdio();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = configure_smi_bus();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not configure IP101GRI MDC/MDIO: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t read_phy_identity(
    uint16_t *id1_out,
    uint16_t *id2_out)
{
    if (id1_out == NULL || id2_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_PHY_ID1_REG,
        id1_out);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_PHY_ID2_REG,
        id2_out);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((*id1_out == 0xFFFFU && *id2_out == 0xFFFFU) ||
        (*id1_out == 0x0000U && *id2_out == 0x0000U)) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Light-sleep: preserve the previous reset-based behavior                    */
/* -------------------------------------------------------------------------- */

esp_err_t component_ethernet_hold_reset_for_light_sleep(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(TAG, "Ethernet stays in BMCR Power Down during Light-sleep");
        return ESP_OK;
    }

    if (s_light_reset_asserted) {
        return ESP_OK;
    }

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_ACTIVE,
        true);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not hold IP101GRI in reset for Light-sleep: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_light_reset_asserted = true;

    ESP_LOGI(
        TAG,
        "IP101GRI Light-sleep state: RESET GPIO%d LOW",
        ETHERNET_PHY_RESET_GPIO);

    return ESP_OK;
}

esp_err_t component_ethernet_restore_after_light_sleep(void)
{
    if (!s_user_enabled) {
        return ESP_OK;
    }

    if (!s_light_reset_asserted) {
        return ESP_OK;
    }

    const esp_err_t ret = configure_phy_reset_level(
        ETHERNET_PHY_RESET_RELEASED,
        false);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not release IP101GRI reset after Light-sleep: %s",
            esp_err_to_name(ret));
        return ret;
    }

    s_light_reset_asserted = false;
    ESP_LOGI(TAG, "IP101GRI reset released after Light-sleep");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Deep-sleep: real BMCR Power Down                                           */
/* -------------------------------------------------------------------------- */

esp_err_t component_ethernet_disable_for_deep_sleep(void)
{
    if (s_phy_powered_down) {
        return ESP_OK;
    }

    /*
     * A HYBRID build may arrive here immediately after a Light-sleep window.
     * Ensure the old reset hold is gone before programming BMCR.
     */
    s_light_reset_asserted = false;

    esp_err_t ret = begin_phy_management();
    if (ret != ESP_OK) {
        release_phy_management_pins();
        return ret;
    }

    uint16_t phy_id1 = 0;
    uint16_t phy_id2 = 0;

    ret = read_phy_identity(&phy_id1, &phy_id2);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "IP101GRI did not answer Clause-22 MDIO at PHY address %u: %s",
            (unsigned)ETHERNET_PHY_ADDRESS,
            esp_err_to_name(ret));
        release_phy_management_pins();
        return ret;
    }

    uint16_t bmcr = 0;
    ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        &bmcr);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not read IP101GRI BMCR: %s",
            esp_err_to_name(ret));
        release_phy_management_pins();
        return ret;
    }

    if (!s_original_bmcr_valid) {
        s_original_bmcr = bmcr;
        s_original_bmcr_valid = true;
    }

    ESP_LOGI(
        TAG,
        "IP101GRI detected over MDIO: phy_addr=%u id1=0x%04X id2=0x%04X "
        "BMCR_before=0x%04X",
        (unsigned)ETHERNET_PHY_ADDRESS,
        phy_id1,
        phy_id2,
        bmcr);

    ret = mdio_clause22_write(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        bmcr | MII_BMCR_POWER_DOWN);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not set IP101GRI BMCR Power Down: %s",
            esp_err_to_name(ret));
        release_phy_management_pins();
        return ret;
    }

    /*
     * BMCR Power Down disables the PHY core/oscillator, while MDC/MDIO stay
     * accessible. Give the management interface a short settling window before
     * reading the register back.
     */
    vTaskDelay(pdMS_TO_TICKS(2));

    uint16_t verify_bmcr = 0;
    ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        &verify_bmcr);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not verify IP101GRI BMCR: %s",
            esp_err_to_name(ret));
        release_phy_management_pins();
        return ret;
    }

    if ((verify_bmcr & MII_BMCR_POWER_DOWN) == 0U) {
        ESP_LOGE(
            TAG,
            "IP101GRI BMCR Power Down did not latch: readback=0x%04X",
            verify_bmcr);
        release_phy_management_pins();
        return ESP_ERR_INVALID_STATE;
    }

    s_phy_powered_down = true;

    ESP_LOGI(
        TAG,
        "IP101GRI real power-down verified: BMCR=0x%04X "
        "POWER_DOWN(bit11)=1; RESET remains released",
        verify_bmcr);

    /*
     * Never assert RESET after this point. Hardware reset restores BMCR
     * defaults, which would immediately clear bit11.
     */
    release_phy_management_pins();
    return ESP_OK;
}

esp_err_t component_ethernet_verify_power_down(void)
{
    /*
     * Re-read the physical register even if the first post-write readback had
     * failed. Once the original BMCR has been captured, a power-down write may
     * have reached the PHY even if the following verification transaction was
     * interrupted.
     */
    if (!s_original_bmcr_valid) {
        ESP_LOGE(
            TAG,
            "IP101GRI final audit unavailable: original BMCR was not captured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = begin_phy_management();
    if (ret != ESP_OK) {
        release_phy_management_pins();
        return ret;
    }

    uint16_t bmcr = 0;
    ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        &bmcr);

    if (ret == ESP_OK && (bmcr & MII_BMCR_POWER_DOWN) == 0U) {
        ret = ESP_ERR_INVALID_STATE;
    }

    if (ret == ESP_OK) {
        s_phy_powered_down = true;
        ESP_LOGI(
            TAG,
            "IP101GRI final audit OK: BMCR=0x%04X POWER_DOWN(bit11)=1",
            bmcr);
    } else {
        ESP_LOGE(
            TAG,
            "IP101GRI final audit FAILED: BMCR=0x%04X ret=%s",
            bmcr,
            esp_err_to_name(ret));
    }

    release_phy_management_pins();
    return ret;
}

esp_err_t component_ethernet_restore_after_failed_sleep(void)
{
    if (!s_user_enabled) {
        ESP_LOGI(TAG,
                 "IP101GRI remains powered down because the saved Ethernet policy is OFF");
        return ESP_OK;
    }

    /*
     * Restore whenever an original BMCR snapshot exists. The write may have
     * reached the PHY even if the first verification read failed, so checking
     * only s_phy_powered_down would risk leaving a failed-sleep recovery in
     * Power Down.
     */
    if (!s_original_bmcr_valid) {
        return ESP_OK;
    }

    esp_err_t ret = begin_phy_management();
    if (ret != ESP_OK) {
        release_phy_management_pins();
        return ret;
    }

    ret = mdio_clause22_write(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        s_original_bmcr);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not restore IP101GRI BMCR: %s",
            esp_err_to_name(ret));
        release_phy_management_pins();
        return ret;
    }

    uint16_t verify_bmcr = 0;
    ret = mdio_clause22_read(
        ETHERNET_PHY_ADDRESS,
        MII_BMCR_REG,
        &verify_bmcr);

    if (ret != ESP_OK) {
        release_phy_management_pins();
        return ret;
    }

    if (verify_bmcr != s_original_bmcr) {
        ESP_LOGE(
            TAG,
            "IP101GRI restore verification failed: expected BMCR=0x%04X "
            "readback=0x%04X",
            s_original_bmcr,
            verify_bmcr);
        release_phy_management_pins();
        return ESP_ERR_INVALID_STATE;
    }

    s_phy_powered_down = false;
    s_original_bmcr_valid = false;

    ESP_LOGI(
        TAG,
        "IP101GRI BMCR restored after failed Deep-sleep: 0x%04X",
        verify_bmcr);

    release_phy_management_pins();
    return ESP_OK;
}

esp_err_t component_ethernet_set_enabled(bool enabled)
{
    const bool previous_policy = s_user_enabled;
    s_user_enabled = enabled;

    esp_err_t ret;

    if (!enabled) {
        ret = component_ethernet_disable_for_deep_sleep();
    } else if (s_original_bmcr_valid) {
        ret = component_ethernet_restore_after_failed_sleep();
    } else {
        ret = configure_phy_reset_level(
            ETHERNET_PHY_RESET_RELEASED,
            false);
        if (ret == ESP_OK) {
            s_light_reset_asserted = false;
            s_phy_powered_down = false;
            ESP_LOGI(TAG, "IP101GRI enabled by saved Ethernet policy");
        }
    }

    if (ret != ESP_OK) {
        s_user_enabled = previous_policy;
    }

    return ret;
}

bool component_ethernet_is_enabled(void)
{
    return s_user_enabled && !s_phy_powered_down && !s_light_reset_asserted;
}

