#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "n2k_can_driver.h"  // For N2kCanDriver
#include <esp_http_server.h>  // For httpd_config_t, httpd_start, etc.
#include <esp_wifi.h>         // For wifi_init_config_t, esp_wifi_init, etc.
#include <esp_log.h>          // For ESP_LOGI, ESP_LOGE
#include <vector>
#include <utility>

struct CalibrationPoint {
    float depth_cm;
    float percentage;
};

class WebServer {
public:
    WebServer(N2kCanDriver* nmea);
    ~WebServer();
    void start();
    std::vector<CalibrationPoint>& getCalibrationPoints();
    void setCalibrationPoints(const std::vector<CalibrationPoint>& points);

private:
    httpd_handle_t _server;
    N2kCanDriver* _nmea;
    std::vector<CalibrationPoint> _calibration_points;

    static esp_err_t root_get_handler(httpd_req_t* req);
    static esp_err_t config_post_handler(httpd_req_t* req);
    void loadCalibrationFromNVS();
    void saveCalibrationToNVS();
};

#endif