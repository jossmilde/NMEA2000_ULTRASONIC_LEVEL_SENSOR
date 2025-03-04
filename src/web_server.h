#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <string>
#include <vector>
#include "n2k_can_driver.h"
#include "calibration.h"
#include <esp_http_server.h>

class Ultrasonic;

class WebServer {
public:
    WebServer(N2kCanDriver* nmea2000, Ultrasonic* sensor);
    ~WebServer();

    void start();
    void startWiFiAP();
    void connectToWiFi(const char* ssid, const char* password);
    float getLevelPercentage();
    float getTankVolumeLiters();
    uint32_t getTransmissionInterval();
    std::string getDeviceName();
    std::string getVolUnit() { return vol_unit; }
    void setTransmissionInterval(uint32_t interval);
    void setDeviceName(const std::string& name);
    void updateCalibration(const std::vector<CalibrationPoint>& calibration);
    void checkAndSendAlarms();
    void saveCalibrationToNVS(const std::vector<CalibrationPoint>& calibration);
    void loadCalibrationFromNVS(std::vector<CalibrationPoint>& calibration);
    void loadWiFiConfig(std::string& ssid, std::string& password);
    void saveSettingsToNVS();
    void loadSettingFromNVS();

    float getLowAlarmVolume() { return tank_volume * low_alarm_percent / 100.0; }
    float getHighAlarmVolume() { return tank_volume * high_alarm_percent / 100.0; }

    esp_err_t rootHandler(httpd_req_t* req);
    esp_err_t tankFormHandler(httpd_req_t* req);
    esp_err_t tankHandler(httpd_req_t* req);
    esp_err_t configFormHandler(httpd_req_t* req);
    esp_err_t configHandler(httpd_req_t* req);
    esp_err_t wifiScanHandler(httpd_req_t* req);
    esp_err_t wifiFormHandler(httpd_req_t* req);
    esp_err_t wifiHandler(httpd_req_t* req);
    esp_err_t wifiResetHandler(httpd_req_t* req);
    esp_err_t rebootHandler(httpd_req_t* req);

private:
    N2kCanDriver* _nmea2000;
    Ultrasonic* _sensor;
    httpd_handle_t _server;
    httpd_config_t config;

    float tank_height = 100.0;
    float tank_volume = 100.0;
    float sensor_offset = 0.0;
    float low_alarm_percent = 10.0;
    float high_alarm_percent = 90.0;
    std::string tank_shape = "rectangular";
    std::string dist_unit = "cm";
    std::string vol_unit = "liter";

    struct DeviceSettings_t {
        char deviceName[32];
        float tankHeight;         // cm
        float tankVolume;         // liters
        float sensorOffset;       // cm
        float lowAlarmPercent;    // %
        float highAlarmPercent;   // %
        char tankShape[32];
        char distUnit[8];
        char volUnit[16];
        uint32_t interval;        // ms
    };

    template<typename T>
    void saveToNVM(const char* key, T value);
};

#endif