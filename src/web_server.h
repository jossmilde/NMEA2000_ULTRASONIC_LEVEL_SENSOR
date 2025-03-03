#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <string>
#include <vector>
#include "n2k_can_driver.h"
#include "ultrasonic.h"
#include "calibration.h"
#include "esp_http_server.h"

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

class WebServer {
public:
    WebServer(N2kCanDriver* nmea2000, Ultrasonic* sensor);
    ~WebServer();

    float tank_height = 100.0;  // cm
    float tank_volume = 100.0;  // liters
    float sensor_offset = 0.0;  // cm
    float low_alarm_percent = 10.0;  // % of tank_volume
    float high_alarm_percent = 90.0;  // % of tank_volume
    std::string tank_shape = "rectangular";
    std::string dist_unit = "cm";
    std::string vol_unit = "liter";

    void start();
    void startWiFiAP();
    void connectToWiFi(const char* ssid, const char* password);
    float getLevelPercentage();
    float getTankVolumeLiters();
    float getLowAlarmVolume() { return tank_volume * low_alarm_percent / 100.0; }
    float getHighAlarmVolume() { return tank_volume * high_alarm_percent / 100.0; }
    uint32_t getTransmissionInterval();
    std::string getDeviceName();
    std::string getVolUnit() { return vol_unit; }
    std::string getDistUnit() { return dist_unit; }
    void setTransmissionInterval(uint32_t interval);
    void setDeviceName(const std::string& name);
    void setTankHeight(float height) { tank_height = height; }
    void setTankVolume(float volume) { tank_volume = volume; }
    void setSensorOffset(float offset) { sensor_offset = offset; }
    void setLowAlarmPercent(float percent) { low_alarm_percent = percent; }
    void setHighAlarmPercent(float percent) { high_alarm_percent = percent; }
    void setTankShape(const std::string& shape) { tank_shape = shape; }
    void setDistanceUnit(const std::string& unit) { dist_unit = unit; }
    void setVolumeUnit(const std::string& unit) { vol_unit = unit; }
    void updateCalibration(const std::vector<CalibrationPoint>& calibration);
    void checkAndSendAlarms();
    void saveCalibrationToNVS(const std::vector<CalibrationPoint>& calibration);
    void loadCalibrationFromNVS(std::vector<CalibrationPoint>& calibration);
    void loadWiFiConfig(std::string& ssid, std::string& password);
    void saveSettingsToNVS();
    void loadSettingsFromNVS();

private:
    N2kCanDriver* _nmea2000;
    Ultrasonic* _sensor;
    httpd_handle_t _server;
    httpd_config_t config;

    template<typename T>
    void saveToNVM(const char* key, T value);
};

#endif