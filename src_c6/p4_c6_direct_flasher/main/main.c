#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "P4_C6_FLASHER";

/*
 * Physical wiring already installed by the user:
 *
 *   P4 GPIO20  -> C6_U0RXD
 *   P4 GPIO21  <- C6_U0TXD
 *   C6_IO9     -> GND
 *
 * Board wiring:
 *
 *   P4 GPIO54  -> C6 CHIP_PU
 *
 * C6_IO9 is held LOW physically, therefore GPIO54 LOW -> HIGH resets the C6
 * into its ROM download bootloader.
 *
 * esp-serial-flasher's ESP32 UART port expects both a reset and boot GPIO.
 * The real C6 boot pin is already forced LOW by the physical jumper, so
 * GPIO22 is used only as a harmless dummy port boot pin. It is NOT wired to
 * the C6.
 */

#define C6_UART_PORT           UART_NUM_1
#define C6_UART_TX_GPIO        GPIO_NUM_20
#define C6_UART_RX_GPIO        GPIO_NUM_21
#define C6_RESET_GPIO          GPIO_NUM_54
#define DUMMY_BOOT_GPIO        GPIO_NUM_22

#define INITIAL_BAUD           115200
#define FAST_BAUD              460800

#define BACKUP_PATH            "/sdcard/C6BACKUP.BIN"
#define BACKUP_OK_PATH         "/sdcard/C6BKOK.TXT"
#define SLEEP_FLASHED_PATH     "/sdcard/C6SLEEP.TXT"
#define RESTORE_REQUEST_PATH   "/sdcard/RESTORE.REQ"
#define RESTORED_PATH          "/sdcard/C6RESTOR.TXT"

#define IO_CHUNK_SIZE          4096U
#define MIN_VALID_SLEEP_IMAGE  (64U * 1024U)

extern const uint8_t c6_merged_start[]
    asm("_binary_c6_merged_bin_start");
extern const uint8_t c6_merged_end[]
    asm("_binary_c6_merged_bin_end");

static esp32_port_t s_c6_port = {
    .port.ops = &esp32_uart_ops,
    .baud_rate = INITIAL_BAUD,
    .uart_port = C6_UART_PORT,
    .uart_rx_pin = C6_UART_RX_GPIO,
    .uart_tx_pin = C6_UART_TX_GPIO,
    .reset_pin = C6_RESET_GPIO,
    .boot_pin = DUMMY_BOOT_GPIO,
    .rx_buffer_size = 4096,
    .tx_buffer_size = 4096,
};

static esp_loader_t s_loader;

static void stop_forever(const char *reason)
{
    ESP_LOGW(TAG, "STOP: %s", reason);
    ESP_LOGW(TAG, "No C6 flash write will occur after this point.");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool file_has_exact_size(const char *path, uint32_t expected_size)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }

    return st.st_size == (off_t)expected_size;
}

static esp_err_t write_marker(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not create %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    if (fprintf(f, "%s\n", text) < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    fflush(f);
    fclose(f);
    return ESP_OK;
}

static bool sleep_marker_is_verified_v2(void)
{
    FILE *f = fopen(SLEEP_FLASHED_PATH, "r");
    if (f == NULL) {
        return false;
    }

    char line[160] = {0};
    const bool ok =
        fgets(line, sizeof(line), f) != NULL &&
        strstr(line, "verified-image-v2") != NULL;

    fclose(f);
    return ok;
}


static esp_loader_error_t verify_flash_memory_image(
    const uint8_t *expected,
    uint32_t image_size,
    uint32_t flash_offset,
    bool *matches_out)
{
    if (expected == NULL || matches_out == NULL || image_size == 0) {
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }

    *matches_out = false;

    uint8_t *buffer = malloc(IO_CHUNK_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Could not allocate C6 verification buffer");
        return ESP_LOADER_ERROR_FAIL;
    }

    esp_loader_error_t result = ESP_LOADER_SUCCESS;

    for (uint32_t offset = 0; offset < image_size; offset += IO_CHUNK_SIZE) {
        const uint32_t amount =
            (image_size - offset > IO_CHUNK_SIZE)
                ? IO_CHUNK_SIZE
                : image_size - offset;

        result = esp_loader_flash_read(
            &s_loader,
            buffer,
            flash_offset + offset,
            amount);

        if (result != ESP_LOADER_SUCCESS) {
            ESP_LOGE(
                TAG,
                "C6 readback verification failed at 0x%08" PRIx32 ": %d",
                flash_offset + offset,
                (int)result);
            break;
        }

        if (memcmp(buffer, expected + offset, amount) != 0) {
            ESP_LOGW(
                TAG,
                "C6 readback differs from embedded sleep image at "
                "0x%08" PRIx32,
                flash_offset + offset);
            result = ESP_LOADER_SUCCESS;
            free(buffer);
            return result;
        }
    }

    free(buffer);

    if (result == ESP_LOADER_SUCCESS) {
        *matches_out = true;
        ESP_LOGW(
            TAG,
            "C6 READBACK VERIFY COMPLETE: embedded sleep image matches flash");
    }

    return result;
}


static bool security_is_safe_for_diagnostic(
    const esp_loader_target_security_info_t *info)
{
    ESP_LOGI(
        TAG,
        "C6 security: target=%d eco=%" PRIu32
        " secure_boot=%d flash_encryption=%d secure_download=%d "
        "jtag_sw_disabled=%d jtag_hw_disabled=%d",
        (int)info->target_chip,
        info->eco_version,
        info->secure_boot_enabled,
        info->flash_encryption_enabled,
        info->secure_download_mode_enabled,
        info->jtag_software_disabled,
        info->jtag_hardware_disabled);

    /*
     * This diagnostic intentionally refuses to overwrite a secured factory C6.
     * That avoids destroying a signed/encrypted factory image.
     */
    return !info->secure_boot_enabled &&
           !info->flash_encryption_enabled &&
           !info->secure_download_mode_enabled;
}

static esp_loader_error_t connect_c6(void)
{
    esp_loader_error_t err =
        esp_loader_init_serial(&s_loader, &s_c6_port.port);

    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "esp_loader_init_serial failed: %d", (int)err);
        return err;
    }

    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    args.sync_timeout = 300;
    args.trials = 20;

    /*
     * First connect through ROM only so security information can be inspected
     * before loading a flasher stub.
     */
    err = esp_loader_connect(&s_loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "ROM connect to C6 failed: %d", (int)err);
        return err;
    }

    if (esp_loader_get_target(&s_loader) != ESP32C6_CHIP) {
        ESP_LOGE(
            TAG,
            "Wrong target detected: %d; expected ESP32-C6 (%d)",
            (int)esp_loader_get_target(&s_loader),
            (int)ESP32C6_CHIP);
        return ESP_LOADER_ERROR_UNSUPPORTED_CHIP;
    }

    ESP_LOGI(TAG, "C6 ROM connection OK: target=ESP32-C6");

    esp_loader_target_security_info_t security = {0};
    err = esp_loader_get_security_info(&s_loader, &security);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Could not read C6 security information: %d", (int)err);
        return err;
    }

    if (!security_is_safe_for_diagnostic(&security)) {
        ESP_LOGE(
            TAG,
            "C6 has security features enabled. Refusing to read/write factory "
            "flash automatically.");
        return ESP_LOADER_ERROR_FAIL;
    }

    /*
     * Re-enter the bootloader and load Espressif's official flasher stub.
     * The stub gives reliable fast flash reads and writes without routing
     * esptool packets through the P4's PC console UART.
     */
    err = esp_loader_connect_with_stub(&s_loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C6 stub connection failed: %d", (int)err);
        return err;
    }

    ESP_LOGI(TAG, "C6 flasher stub connection OK");

    err = esp_loader_change_transmission_rate(&s_loader, FAST_BAUD);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGW(
            TAG,
            "Could not switch C6 UART to %d baud; continuing at %d",
            FAST_BAUD,
            INITIAL_BAUD);
    } else {
        ESP_LOGI(TAG, "C6 UART switched to %d baud", FAST_BAUD);
    }

    return ESP_LOADER_SUCCESS;
}

static esp_err_t mount_sdcard(void)
{
    esp_err_t ret = bsp_sdcard_mount();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "microSD mounted at /sdcard");
        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "microSD mount failed: %s. No C6 write will be attempted.",
        esp_err_to_name(ret));

    return ret;
}

static esp_loader_error_t backup_factory_flash(uint32_t flash_size)
{
    ESP_LOGW(
        TAG,
        "BACKUP BEGIN: reading full C6 flash (%" PRIu32 " bytes) -> %s",
        flash_size,
        BACKUP_PATH);

    FILE *f = fopen(BACKUP_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not open backup file: errno=%d", errno);
        return ESP_LOADER_ERROR_FAIL;
    }

    uint8_t *buffer = malloc(IO_CHUNK_SIZE);
    if (buffer == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "Could not allocate backup buffer");
        return ESP_LOADER_ERROR_FAIL;
    }

    esp_loader_error_t result = ESP_LOADER_SUCCESS;

    for (uint32_t offset = 0; offset < flash_size; offset += IO_CHUNK_SIZE) {
        const uint32_t amount =
            (flash_size - offset > IO_CHUNK_SIZE)
                ? IO_CHUNK_SIZE
                : flash_size - offset;

        result =
            esp_loader_flash_read(&s_loader, buffer, offset, amount);

        if (result != ESP_LOADER_SUCCESS) {
            ESP_LOGE(
                TAG,
                "C6 flash read failed at 0x%08" PRIx32 ": %d",
                offset,
                (int)result);
            break;
        }

        if (fwrite(buffer, 1, amount, f) != amount) {
            ESP_LOGE(
                TAG,
                "SD write failed at C6 offset 0x%08" PRIx32,
                offset);
            result = ESP_LOADER_ERROR_FAIL;
            break;
        }

        if ((offset % (256U * 1024U)) == 0) {
            ESP_LOGI(
                TAG,
                "Backup progress: %" PRIu32 " / %" PRIu32 " bytes",
                offset,
                flash_size);
        }
    }

    fflush(f);
    fclose(f);
    free(buffer);

    if (result != ESP_LOADER_SUCCESS) {
        unlink(BACKUP_PATH);
        return result;
    }

    if (!file_has_exact_size(BACKUP_PATH, flash_size)) {
        ESP_LOGE(TAG, "Backup size verification FAILED");
        unlink(BACKUP_PATH);
        return ESP_LOADER_ERROR_FAIL;
    }

    if (write_marker(
            BACKUP_OK_PATH,
            "Complete raw ESP32-C6 factory flash backup verified by file size.")
        != ESP_OK) {
        return ESP_LOADER_ERROR_FAIL;
    }

    ESP_LOGW(TAG, "====================================================");
    ESP_LOGW(TAG, "C6 FACTORY BACKUP COMPLETE");
    ESP_LOGW(TAG, "File: %s", BACKUP_PATH);
    ESP_LOGW(TAG, "Size: %" PRIu32 " bytes", flash_size);
    ESP_LOGW(TAG, "NO C6 FLASH WRITE WAS PERFORMED ON THIS BOOT.");
    ESP_LOGW(TAG, "Press P4 RESET once to continue to the sleep-firmware flash.");
    ESP_LOGW(TAG, "====================================================");

    return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t flash_memory_image(
    const uint8_t *data,
    uint32_t image_size,
    uint32_t flash_offset,
    const char *description)
{
    if ((image_size & 3U) != 0) {
        ESP_LOGE(
            TAG,
            "%s size %" PRIu32 " is not 4-byte aligned",
            description,
            image_size);
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }

    esp_loader_flash_cfg_t cfg = {
        .offset = flash_offset,
        .image_size = image_size,
        .block_size = IO_CHUNK_SIZE,
        .skip_verify = false,
    };

    ESP_LOGW(
        TAG,
        "FLASH BEGIN: %s offset=0x%08" PRIx32 " size=%" PRIu32,
        description,
        flash_offset,
        image_size);

    esp_loader_error_t err =
        esp_loader_flash_start(&s_loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) {
        return err;
    }

    uint32_t written = 0;

    while (written < image_size) {
        uint32_t amount = image_size - written;
        if (amount > IO_CHUNK_SIZE) {
            amount = IO_CHUNK_SIZE;
        }

        err = esp_loader_flash_write(
            &s_loader,
            &cfg,
            data + written,
            amount);

        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(
                TAG,
                "Flash write failed after %" PRIu32 " bytes: %d",
                written,
                (int)err);
            return err;
        }

        written += amount;

        if ((written % (64U * 1024U)) == 0 || written == image_size) {
            ESP_LOGI(
                TAG,
                "Flash progress: %" PRIu32 " / %" PRIu32,
                written,
                image_size);
        }
    }

    err = esp_loader_flash_finish(&s_loader, &cfg);

    if (err == ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "FLASH + MD5 VERIFY COMPLETE: %s", description);
    }

    return err;
}

static esp_loader_error_t flash_file_image(
    const char *path,
    uint32_t expected_size,
    const char *description)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not open %s", path);
        return ESP_LOADER_ERROR_FAIL;
    }

    uint8_t *buffer = malloc(IO_CHUNK_SIZE);
    if (buffer == NULL) {
        fclose(f);
        return ESP_LOADER_ERROR_FAIL;
    }

    esp_loader_flash_cfg_t cfg = {
        .offset = 0,
        .image_size = expected_size,
        .block_size = IO_CHUNK_SIZE,
        .skip_verify = false,
    };

    esp_loader_error_t err =
        esp_loader_flash_start(&s_loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) {
        free(buffer);
        fclose(f);
        return err;
    }

    uint32_t total = 0;

    while (total < expected_size) {
        uint32_t want = expected_size - total;
        if (want > IO_CHUNK_SIZE) {
            want = IO_CHUNK_SIZE;
        }

        const size_t got = fread(buffer, 1, want, f);
        if (got != want) {
            ESP_LOGE(TAG, "Backup file read failed at %" PRIu32, total);
            err = ESP_LOADER_ERROR_FAIL;
            break;
        }

        err =
            esp_loader_flash_write(&s_loader, &cfg, buffer, want);
        if (err != ESP_LOADER_SUCCESS) {
            break;
        }

        total += want;

        if ((total % (256U * 1024U)) == 0 || total == expected_size) {
            ESP_LOGI(
                TAG,
                "%s progress: %" PRIu32 " / %" PRIu32,
                description,
                total,
                expected_size);
        }
    }

    if (err == ESP_LOADER_SUCCESS) {
        err = esp_loader_flash_finish(&s_loader, &cfg);
    }

    free(buffer);
    fclose(f);
    return err;
}

void app_main(void)
{
    ESP_LOGW(TAG, "====================================================");
    ESP_LOGW(TAG, "ESP32-P4 -> ESP32-C6 OFFICIAL DIRECT FLASHER");
    ESP_LOGW(TAG, "P4 silicon configuration: revision < v3");
    ESP_LOGW(TAG, "UART: GPIO20 TX -> C6_U0RXD");
    ESP_LOGW(TAG, "UART: GPIO21 RX <- C6_U0TXD");
    ESP_LOGW(TAG, "RESET: GPIO54 -> C6 CHIP_PU");
    ESP_LOGW(TAG, "BOOT: C6_IO9 must remain physically connected to GND");
    ESP_LOGW(TAG, "====================================================");

    const uint32_t sleep_image_size =
        (uint32_t)(c6_merged_end - c6_merged_start);

    if (sleep_image_size < MIN_VALID_SLEEP_IMAGE ||
        c6_merged_start[0] != 0xE9) {
        ESP_LOGE(
            TAG,
            "Embedded C6 merged image is invalid "
            "(size=%" PRIu32 " first_byte=0x%02X).",
            sleep_image_size,
            c6_merged_start[0]);

        ESP_LOGE(
            TAG,
            "Run ./utilities/esp_P4_C6_direct.sh prepare before flashing "
            "this P4 helper.");

        stop_forever("invalid embedded C6 image");
    }

    esp_loader_error_t err = connect_c6();
    if (err != ESP_LOADER_SUCCESS) {
        stop_forever("could not establish reliable direct C6 connection");
    }

    uint32_t flash_size = 0;
    err = esp_loader_flash_detect_size(&s_loader, &flash_size);

    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Could not detect C6 flash size: %d", (int)err);
        stop_forever("C6 flash-size detection failed");
    }

    ESP_LOGW(TAG, "Detected C6 flash size: %" PRIu32 " bytes", flash_size);

    if (mount_sdcard() != ESP_OK) {
        stop_forever("microSD is required for factory backup");
    }

    /*
     * Optional recovery mode:
     * Place an empty /sdcard/RESTORE_C6_FACTORY.txt on the card and reboot
     * this helper. The full factory backup is written back to C6 flash.
     */
    if (file_exists(RESTORE_REQUEST_PATH)) {
        if (!file_has_exact_size(BACKUP_PATH, flash_size)) {
            stop_forever("restore requested but factory backup is missing/invalid");
        }

        ESP_LOGW(TAG, "FACTORY RESTORE REQUEST DETECTED");

        err = flash_file_image(
            BACKUP_PATH,
            flash_size,
            "full C6 factory backup");

        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "Factory restore failed: %d", (int)err);
            stop_forever("factory restore failed");
        }

        unlink(RESTORE_REQUEST_PATH);
        unlink(SLEEP_FLASHED_PATH);
        write_marker(RESTORED_PATH, "C6 factory full-flash image restored.");

        ESP_LOGW(TAG, "C6 FACTORY RESTORE COMPLETE");
        stop_forever("power off and remove temporary wires");
    }

    /*
     * Safety gate #1: first boot only creates the complete factory backup.
     */
    if (!file_has_exact_size(BACKUP_PATH, flash_size) ||
        !file_exists(BACKUP_OK_PATH)) {
        err = backup_factory_flash(flash_size);

        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "Factory backup failed: %d", (int)err);
            stop_forever("factory backup failed");
        }

        stop_forever("backup complete; reset P4 once to continue");
    }

    ESP_LOGW(
        TAG,
        "Verified existing C6 factory backup: %s (%" PRIu32 " bytes)",
        BACKUP_PATH,
        flash_size);

    /*
     * Safety gate #2, hardened:
     *
     * The old implementation trusted /sdcard/C6SLEEP.TXT by itself. That file
     * could outlive the actual C6 contents, causing a stale marker to suppress
     * the very reflash needed for a power-regression test.
     *
     * Legacy markers are intentionally treated as unverified and force one
     * real rewrite. A v2 marker is trusted only after the C6 flash is read back
     * and byte-compared against the currently embedded merged image.
     */
    if (file_exists(SLEEP_FLASHED_PATH)) {
        if (!sleep_marker_is_verified_v2()) {
            ESP_LOGW(
                TAG,
                "Legacy C6 sleep marker found. It does not prove the current "
                "C6 contents; forcing one real reflash.");
            (void)unlink(SLEEP_FLASHED_PATH);
        } else {
            bool flash_matches = false;
            err = verify_flash_memory_image(
                c6_merged_start,
                sleep_image_size,
                0,
                &flash_matches);

            if (err != ESP_LOADER_SUCCESS) {
                stop_forever("C6 readback verification failed");
            }

            if (flash_matches) {
                ESP_LOGW(
                    TAG,
                    "C6 sleep firmware verified in flash; no rewrite required.");
                stop_forever("verified C6 sleep image already installed");
            }

            ESP_LOGW(
                TAG,
                "C6 v2 marker exists but flash content does not match. "
                "Removing marker and reflashing.");
            (void)unlink(SLEEP_FLASHED_PATH);
        }
    }

    /*
     * Second boot after the verified backup: flash the merged C6 diagnostic
     * image. The build helper creates this merged image from the C6 project's
     * own ESP-IDF flash_args, so bootloader/partition/app offsets are preserved.
     */
    err = flash_memory_image(
        c6_merged_start,
        sleep_image_size,
        0,
        "C6 self-Deep-sleep merged image");

    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C6 sleep image flash failed: %d", (int)err);
        stop_forever("C6 sleep firmware flash failed");
    }

    /*
     * Do an explicit byte-for-byte readback in addition to the flasher's MD5
     * verification before creating the persistent marker.
     */
    bool flash_matches_after_write = false;
    err = verify_flash_memory_image(
        c6_merged_start,
        sleep_image_size,
        0,
        &flash_matches_after_write);

    if (err != ESP_LOADER_SUCCESS || !flash_matches_after_write) {
        ESP_LOGE(
            TAG,
            "C6 post-write readback did not match the embedded sleep image.");
        (void)unlink(SLEEP_FLASHED_PATH);
        stop_forever("C6 post-write verification failed");
    }

    if (write_marker(
            SLEEP_FLASHED_PATH,
            "verified-image-v2: C6 self-Deep-sleep merged image "
            "flashed, MD5 verified, and byte-readback verified.")
        != ESP_OK) {
        ESP_LOGW(TAG, "Could not write verified flash-complete marker to SD");
    }

    ESP_LOGW(TAG, "====================================================");
    ESP_LOGW(TAG, "C6 SELF-DEEP-SLEEP FIRMWARE FLASH COMPLETE");
    ESP_LOGW(TAG, "1. POWER OFF the board.");
    ESP_LOGW(TAG, "2. REMOVE C6_IO9 -> GND.");
    ESP_LOGW(TAG, "3. REMOVE GPIO20 -> C6_U0RXD.");
    ESP_LOGW(TAG, "4. REMOVE GPIO21 -> C6_U0TXD.");
    ESP_LOGW(TAG, "5. Power ON and re-flash the normal P4 application.");
    ESP_LOGW(TAG, "====================================================");

    stop_forever("C6 programming completed successfully");
}
