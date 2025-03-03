#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "n2k_can_driver.h"
#include "ultrasonic.h"
#include "web_server.h"
#include "N2kMessages.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <inttypes.h>

static const char* TAG = "Main";

N2kCanDriver NMEA2000(GPIO_NUM_27, GPIO_NUM_26, GPIO_NUM_23);
Ultrasonic sensor;
WebServer webServer(&NMEA2000, &sensor);

const unsigned long DeviceSerial = 123456;
const unsigned short ProductCode = 2001;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGE(TAG, "WiFi STA disconnected, reason: %d", ((wifi_event_sta_disconnected_t*)event_data)->reason);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi STA connected");
                break;
            default:
                ESP_LOGI(TAG, "WiFi event: %ld", event_id);
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void setupNMEA2000() {
    ESP_LOGI(TAG, "Setting up NMEA2000...");
    NMEA2000.SetProductInformation("00000001", ProductCode, NMEA2000.getDeviceName().c_str(), "1.00", "0.1");
    NMEA2000.SetDeviceInformation(DeviceSerial, 130, 75, 2046);
    NMEA2000.SetMode(tNMEA2000::N2km_NodeOnly);
    NMEA2000.EnableForward(false);
    NMEA2000.SetMsgHandler([](const tN2kMsg& msg) {
        ESP_LOGI(TAG, "Received PGN: %lu", msg.PGN);
    });
    NMEA2000.Init();
    ESP_LOGI(TAG, "NMEA2000 initialized");
}

void sendFluidLevel() {
    static unsigned long last_sent = 0;
    unsigned long now = esp_timer_get_time() / 1000;
    uint32_t interval = NMEA2000.getTransmissionInterval();

    if (now - last_sent >= interval) {
        float level_percent = sensor.getLevelPercentage();
        tN2kMsg N2kMsg;
        SetN2kFluidLevel(N2kMsg, 0, N2kft_Water, level_percent / 100.0, webServer.getTankVolumeLiters());
        if (!NMEA2000.SendMsg(N2kMsg)) {
            ESP_LOGW(TAG, "Failed to send NMEA2000 message");
        }
        webServer.checkAndSendAlarms();
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

void simulateUltrasonicTask(void* pvParameters) {
    ESP_LOGI(TAG, "Starting ultrasonic simulation...");
    float simulated_distance = 70.0;
    bool decreasing = true;

    while (1) {
        if (decreasing) {
            simulated_distance += 2.0;
            if (simulated_distance >= 120.0) decreasing = false;
        } else {
            simulated_distance -= 2.0;
            if (simulated_distance <= 20.0) decreasing = true;
        }
        sensor.setSimulatedDistance(simulated_distance);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void wifiScanTask(void* pvParameters) {
    std::string stored_ssid, stored_password;
    webServer.loadWiFiConfig(stored_ssid, stored_password);
    if (stored_ssid.empty() || stored_password.empty()) {
        ESP_LOGI(TAG, "No stored WiFi credentials, scan task exiting");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    int retries = 5;
    while (retries--) {
        wifi_scan_config_t scan_config = {};
        scan_config.ssid = nullptr;  // Scan all SSIDs
        scan_config.channel = 6;     // Target Channel 6 (LieMo_Gast)
        scan_config.scan_time.active.min = 4000;  // Min scan time (4s)
        scan_config.scan_time.active.max = 6000;  // Max scan time (6s)
        ESP_LOGI(TAG, "Starting WiFi scan on Channel 6 with min=%" PRIu32 " ms, max=%" PRIu32 " ms", 
                 scan_config.scan_time.active.min, scan_config.scan_time.active.max);
        esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi scan failed on Channel 6 with error %d, retries left: %d", ret, retries);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        wifi_ap_record_t* ap_list = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
        bool found = false;
        for (int i = 0; i < ap_count; i++) {
            const char* auth_mode;
            switch (ap_list[i].authmode) {
                case WIFI_AUTH_OPEN: auth_mode = "OPEN"; break;
                case WIFI_AUTH_WEP: auth_mode = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: auth_mode = "WPA_PSK"; break;
                case WIFI_AUTH_WPA2_PSK: auth_mode = "WPA2_PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: auth_mode = "WPA_WPA2_PSK"; break;
                default: auth_mode = "UNKNOWN"; break;
            }
            ESP_LOGI(TAG, "Scanned AP: SSID=%s, RSSI=%d, Channel=%d, Auth=%s", 
                     ap_list[i].ssid, ap_list[i].rssi, ap_list[i].primary, auth_mode);
            if (strcmp((char*)ap_list[i].ssid, stored_ssid.c_str()) == 0) {
                ESP_LOGI(TAG, "Found stored SSID %s (RSSI: %d, Channel: %d), switching to STA mode", 
                         stored_ssid.c_str(), ap_list[i].rssi, ap_list[i].primary);
                found = true;
                break;
            }
        }
        free(ap_list);

        if (found) {
            esp_wifi_stop();
            esp_wifi_deinit();
            esp_netif_create_default_wifi_sta();
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));
            webServer.connectToWiFi(stored_ssid.c_str(), stored_password.c_str());
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "Stored SSID %s not found in scan, retrying... (%d retries left)", stored_ssid.c_str(), retries);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGW(TAG, "Stored SSID %s not found after retries, staying in AP mode", stored_ssid.c_str());
    vTaskDelete(NULL);
}

void webServerTask(void* pvParameters) {
    ESP_LOGI(TAG, "Web server task started");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register WiFi event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    std::string ssid, password;
    webServer.loadWiFiConfig(ssid, password);
    if (!ssid.empty() && !password.empty()) {
        ESP_LOGI(TAG, "Found WiFi credentials in NVM: SSID=%s, Password=%s", ssid.c_str(), password.c_str());
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        // Direct connection to LieMo_Gast on Channel 6
        ESP_LOGI(TAG, "Attempting direct connection to %s on Channel 6", ssid.c_str());
        webServer.connectToWiFi(ssid.c_str(), password.c_str());

        int retries = 40;
        while (retries--) {
            wifi_ap_record_t ap_info;
            esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Connected to WiFi STA successfully: SSID=%s, RSSI=%d, Channel=%d", 
                         ap_info.ssid, ap_info.rssi, ap_info.primary);
                goto start_server;
            } else if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGI(TAG, "Still connecting to STA %s, retrying... (%d retries left)", ssid.c_str(), retries);
            } else {
                ESP_LOGE(TAG, "Failed to get STA info: %d", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGW(TAG, "Failed to connect to STA %s after 40 retries, falling back to AP mode", ssid.c_str());
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        webServer.startWiFiAP();
        xTaskCreate(wifiScanTask, "wifi_scan_task", 4096, NULL, 4, NULL);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials in NVM, starting AP mode...");
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        webServer.startWiFiAP();
    }

start_server:
    vTaskDelay(pdMS_TO_TICKS(1000));
    webServer.start();
    ESP_LOGI(TAG, "Web server startup completed");
    vTaskDelay(pdMS_TO_TICKS(5000));
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

    webServer.loadSettingsFromNVS();

    std::vector<CalibrationPoint> calibration;
    webServer.loadCalibrationFromNVS(calibration);
    if (!calibration.empty()) {
        webServer.updateCalibration(calibration);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Starting tasks...");
    xTaskCreate(webServerTask, "web_server_task", 24576, NULL, 5, NULL);
    xTaskCreate(nmeaTask, "nmea_task", 8192, NULL, 5, NULL);
    xTaskCreate(simulateUltrasonicTask, "ultrasonic_sim", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Entering main loop...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}