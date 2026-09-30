/*
 * ESP32-S3 one-button e-reader
 * TFT (ST7735/ST7789/ILI9341/ST7796) + EPUB/PDF/TXT + WiFi upload.
 */
#include "display.h"
#include "button.h"
#include "settings.h"
#include "library.h"
#include "ui.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool woke = cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1;
    if (woke) {
        /* release pins that were held/configured for deep sleep */
        uint64_t mask = button_wake_mask();
        for (int p = 0; p <= 21; p++)
            if (mask & (1ULL << p)) rtc_gpio_deinit(p);
#if CONFIG_EREADER_PIN_BL >= 0
        gpio_hold_dis(CONFIG_EREADER_PIN_BL);
#endif
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (display_init() != 0) ESP_LOGE(TAG, "display init failed - check wiring and menuconfig");
    settings_load();
    display_set_rotation(g_set.rotation);
    button_init();
    if (storage_init() != 0) ESP_LOGE(TAG, "book storage could not be mounted");
    ESP_LOGI(TAG, "starting UI (%s)", woke ? "wake from sleep" : "power on");
    ui_run(woke);
}
