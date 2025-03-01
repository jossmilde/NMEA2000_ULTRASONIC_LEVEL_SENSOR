#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "n2k_can_driver.h"
#include "ultrasonic.h"
#include "web_server.h"
#include "N2kMessages.h"
#include <esp_log.h>
#include <nvs_flash.h>

static const char* TAG = "Main";

N2kCanDriver NMEA2000(GPIO_NUM_27, GPIO_NUM_26, GPIO_NUM_23);
Ultrasonic sensor;
WebServer webServer(&NMEA2000);

const unsigned long DeviceSerial = 123456;
const unsigned short ProductCode = 2001;

void setupNMEA2000() {
    ESP_LOGI(TAG, "Setting up NMEA2000...");
    NMEA2000.SetProductInformation("00000001", ProductCode, NMEA2000.getDeviceName().c_str(), "1.00", "0.1");
    NMEA2000.SetDeviceInformation(DeviceSerial, 130, 75, 2046);
    NMEA2000.SetMode(tNMEA2000::N2km_NodeOnly);
    NMEA2000.EnableForward(false);
    NMEA2000.SetMsgHandler([](const tN2kMsg& msg) {
        ESP_LOGI(TAG, "Received PGN: %lu", msg.PGN);
    });
    NMEA2000.Init();  // Use manual init
    ESP_LOGI(TAG, "NMEA2000 initialized");
}

void sendFluidLevel() {
    static unsigned long last_sent = 0;
    unsigned long now = esp_timer_get_time() / 1000;
    uint32_t interval = NMEA2000.getTransmissionInterval();

    if (now - last_sent >= interval) {
        float level_percent = sensor.getLevelPercentage();
        tN2kMsg N2kMsg;
        SetN2kFluidLevel(N2kMsg, 0, N2kft_Water, level_percent / 100.0, 1.0);
        if (NMEA2000.SendMsg(N2kMsg)) {
            ESP_LOGI(TAG, "Sent fluid level: %.1f%%", level_percent);
        } else {
            ESP_LOGE(TAG, "Failed to send fluid level");
        }
        last_sent = now;
    }
}

void nmeaTask(void* pvParameters) {
    ESP_LOGI(TAG, "NMEA task started");
    setupNMEA2000();
    while (1) {
        sendFluidLevel();
        NMEA2000.ParseMessages();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void webServerTask(void* pvParameters) {
    ESP_LOGI(TAG, "Web server task started");
    webServer.start();
    ESP_LOGI(TAG, "Web server startup completed");
    vTaskDelay(pdMS_TO_TICKS(5000));
    xTaskCreate(nmeaTask, "nmea_task", 8192, NULL, 5, NULL);
    vTaskDelete(NULL);
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "Starting app_main...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init failed, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "NVS initialized");

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Starting web server task...");
    xTaskCreate(webServerTask, "web_server_task", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "Entering main loop...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}