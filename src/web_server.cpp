#include "web_server.h"
#include <nvs_flash.h>
#include <string.h>
#include <string>

static const char* TAG = "WebServer";

WebServer::WebServer(N2kCanDriver* nmea) : _server(nullptr), _nmea(nmea) {
    ESP_LOGI(TAG, "WebServer constructor called");
    loadCalibrationFromNVS();
}

WebServer::~WebServer() {
    ESP_LOGI(TAG, "WebServer destructor called");
    if (_server) httpd_stop(_server);
}

void WebServer::start() {
    ESP_LOGI(TAG, "Starting WiFi...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return;
    }
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi AP mode");
        return;
    }
    wifi_config_t ap_config = {};
    strcpy((char*)ap_config.ap.ssid, "NMEA2000_Sensor");
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure WiFi AP");
        return;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return;
    }
    ESP_LOGI(TAG, "WiFi AP started");

    ESP_LOGI(TAG, "Starting HTTP server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;  // Increased stack size
    if (httpd_start(&_server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = this
        };
        httpd_register_uri_handler(_server, &root);

        httpd_uri_t config_uri = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = config_post_handler,
            .user_ctx = this
        };
        httpd_register_uri_handler(_server, &config_uri);
        ESP_LOGI(TAG, "Web server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

std::vector<CalibrationPoint>& WebServer::getCalibrationPoints() {
    return _calibration_points;
}

void WebServer::setCalibrationPoints(const std::vector<CalibrationPoint>& points) {
    _calibration_points = points;
    if (_calibration_points.size() > 16) _calibration_points.resize(16);
    saveCalibrationToNVS();
}

void WebServer::loadCalibrationFromNVS() {
    nvs_handle_t nvs;
    if (nvs_open("sensor_config", NVS_READWRITE, &nvs) == ESP_OK) {
        uint8_t count = 0;
        nvs_get_u8(nvs, "cal_count", &count);
        _calibration_points.resize(count);
        for (uint8_t i = 0; i < count; i++) {
            char key[16];
            uint32_t val;
            snprintf(key, sizeof(key), "depth_%d", i);
            if (nvs_get_u32(nvs, key, &val) == ESP_OK) _calibration_points[i].depth_cm = val / 100.0f;
            snprintf(key, sizeof(key), "perc_%d", i);
            if (nvs_get_u32(nvs, key, &val) == ESP_OK) _calibration_points[i].percentage = val / 100.0f;
        }
        nvs_close(nvs);
    }
}

void WebServer::saveCalibrationToNVS() {
    nvs_handle_t nvs;
    if (nvs_open("sensor_config", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "cal_count", _calibration_points.size());
        for (size_t i = 0; i < _calibration_points.size(); i++) {
            char key[16];
            snprintf(key, sizeof(key), "depth_%d", i);
            nvs_set_u32(nvs, key, (uint32_t)(_calibration_points[i].depth_cm * 100));
            snprintf(key, sizeof(key), "perc_%d", i);
            nvs_set_u32(nvs, key, (uint32_t)(_calibration_points[i].percentage * 100));
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

esp_err_t WebServer::root_get_handler(httpd_req_t* req) {
    WebServer* ws = (WebServer*)req->user_ctx;
    std::string html = "<html><body><h1>Configure NMEA2000 Sensor</h1>"
                       "<form action=\"/config\" method=\"post\">"
                       "Device Name: <input type=\"text\" name=\"device_name\" value=\"" + ws->_nmea->getDeviceName() + "\"><br>"
                       "Transmission Interval (ms): <input type=\"number\" name=\"tx_interval\" value=\"" + std::to_string(ws->_nmea->getTransmissionInterval()) + "\" min=\"500\" max=\"10000\"><br>"
                       "<h2>Calibration Points (up to 16)</h2>";
    for (size_t i = 0; i < 16; i++) {
        std::string depth = i < ws->_calibration_points.size() ? std::to_string(ws->_calibration_points[i].depth_cm) : "";
        std::string perc = i < ws->_calibration_points.size() ? std::to_string(ws->_calibration_points[i].percentage) : "";
        html += "Point " + std::to_string(i + 1) + ": Depth (cm): <input type=\"number\" name=\"depth_" + std::to_string(i) + "\" value=\"" + depth + "\" step=\"0.1\"> "
                "Percentage: <input type=\"number\" name=\"perc_" + std::to_string(i) + "\" value=\"" + perc + "\" step=\"0.1\"><br>";
    }
    html += "<input type=\"submit\" value=\"Save\"></form></body></html>";
    httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServer::config_post_handler(httpd_req_t* req) {
    WebServer* ws = (WebServer*)req->user_ctx;
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    std::string device_name;
    uint32_t tx_interval = 0;
    std::vector<CalibrationPoint> points;
    char* param = strtok(buf, "&");
    while (param) {
        char* value = strchr(param, '=');
        if (value) {
            *value++ = '\0';
            if (strcmp(param, "device_name") == 0) device_name = value;
            else if (strcmp(param, "tx_interval") == 0) tx_interval = atoi(value);
            else if (strncmp(param, "depth_", 6) == 0) {
                int idx = atoi(param + 6);
                float depth = atof(value);
                char perc_key[16];
                snprintf(perc_key, sizeof(perc_key), "perc_%d", idx);
                char* perc_param = strtok(NULL, "&");
                if (perc_param && strncmp(perc_param, perc_key, strlen(perc_key)) == 0) {
                    float perc = atof(strchr(perc_param, '=') + 1);
                    if (depth > 0 && perc >= 0 && perc <= 100) {
                        points.push_back({depth, perc});
                    }
                }
            }
        }
        param = strtok(NULL, "&");
    }

    if (!device_name.empty()) ws->_nmea->setDeviceName(device_name);
    if (tx_interval >= 500 && tx_interval <= 10000) ws->_nmea->setTransmissionInterval(tx_interval);
    ws->setCalibrationPoints(points);

    httpd_resp_send(req, "Configuration saved. <a href=\"/\">Back</a>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}