#include "web_server.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include "N2kMessages.h"
#include "calibration.h"
#include "ultrasonic.h"

static const char* TAG = "WebServer";

WebServer::WebServer(N2kCanDriver* nmea2000, Ultrasonic* sensor) : _nmea2000(nmea2000), _sensor(sensor), _server(NULL) {
    config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 4;
    config.stack_size = 24576;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
}

WebServer::~WebServer() {
    if (_server) {
        httpd_stop(_server);
        _server = NULL;
    }
}

float convertDistance(float value, const std::string& from_unit, const std::string& to_unit) {
    if (from_unit == to_unit) return value;
    float cm_value = value;
    if (from_unit == "mm") cm_value = value / 10.0;
    else if (from_unit == "m") cm_value = value * 100.0;
    else if (from_unit == "inches") cm_value = value * 2.54;
    else if (from_unit == "ft") cm_value = value * 30.48;

    if (to_unit == "mm") return cm_value * 10.0;
    else if (to_unit == "m") return cm_value / 100.0;
    else if (to_unit == "inches") return cm_value / 2.54;
    else if (to_unit == "ft") return cm_value / 30.48;
    return cm_value;
}

float convertVolume(float value, const std::string& from_unit, const std::string& to_unit) {
    if (from_unit == to_unit) return value;
    float liter_value = value;
    if (from_unit == "gallon") liter_value = value * 3.78541;
    else if (from_unit == "imperial gallon") liter_value = value * 4.54609;
    else if (from_unit == "m³") liter_value = value * 1000.0;

    if (to_unit == "gallon") return liter_value / 3.78541;
    else if (to_unit == "imperial gallon") return liter_value / 4.54609;
    else if (to_unit == "m³") return liter_value / 1000.0;
    return liter_value;
}

std::string formatNumber(float value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << value;
    return oss.str();
}

float parseFloat(const std::string& value, float default_value = 0.0) {
    std::string cleaned = value;
    std::replace(cleaned.begin(), cleaned.end(), ',', '.');
    if (cleaned.empty() || cleaned.find_first_not_of(" \t\n") == std::string::npos) {
        ESP_LOGW(TAG, "Empty or whitespace-only input '%s', returning default: %.1f", value.c_str(), default_value);
        return default_value;
    }

    bool has_digit = false;
    bool has_decimal = false;
    for (char c : cleaned) {
        if (std::isdigit(c)) {
            has_digit = true;
        } else if (c == '.' && !has_decimal) {
            has_decimal = true;
        } else if (c != '-' || (c == '-' && &c != &cleaned[0])) {
            ESP_LOGW(TAG, "Invalid float format '%s', returning default: %.1f", cleaned.c_str(), default_value);
            return default_value;
        }
    }
    if (!has_digit) {
        ESP_LOGW(TAG, "No digits in '%s', returning default: %.1f", cleaned.c_str(), default_value);
        return default_value;
    }

    size_t pos;
    float result = std::stof(cleaned, &pos);
    if (pos != cleaned.length()) {
        ESP_LOGW(TAG, "Partial parse of '%s' to %.1f, trailing characters ignored", cleaned.c_str(), result);
    }
    return result;
}

esp_err_t WebServer::rootHandler(httpd_req_t* req) {
    float level_percent = getLevelPercentage();
    float volume_liters = getTankVolumeLiters();

    std::string resp = "<html><body><h1>Level Sensor</h1>";
    resp += "<p>Level: " + formatNumber(level_percent) + "%</p>";
    resp += "<p>Volume: " + formatNumber(convertVolume(volume_liters, "liter", getVolUnit())) + " " + getVolUnit() + "</p>";
    resp += "<p id='status' style='color:green;display:none'>Saved</p>";

    resp += "<h2>Tank</h2>";
    resp += "<p>Height: " + formatNumber(convertDistance(tank_height, "cm", dist_unit)) + " " + dist_unit + "</p>";
    resp += "<p>Volume: " + formatNumber(convertVolume(tank_volume, "liter", vol_unit)) + " " + vol_unit + "</p>";
    resp += "<p>Offset: " + formatNumber(convertDistance(sensor_offset, "cm", dist_unit)) + " " + dist_unit + "</p>";
    resp += "<p>Low Alarm: " + formatNumber(low_alarm_percent) + "% (" + formatNumber(convertVolume(getLowAlarmVolume(), "liter", vol_unit)) + " " + vol_unit + ")</p>";
    resp += "<p>High Alarm: " + formatNumber(high_alarm_percent) + "% (" + formatNumber(convertVolume(getHighAlarmVolume(), "liter", vol_unit)) + " " + vol_unit + ")</p>";
    resp += "<p>Shape: " + tank_shape + "</p>";
    resp += "<form id='tankForm' onsubmit='saveTank(event)'><input type='submit' value='Edit Tank Settings'></form>";

    resp += "<h2>Config</h2>";
    resp += "<p>Interval: " + std::to_string(getTransmissionInterval()) + " ms</p>";
    std::string device_name = getDeviceName();
    std::replace(device_name.begin(), device_name.end(), '+', ' ');
    resp += "<p>Name: " + device_name + "</p>";
    resp += "<form id='configForm' onsubmit='saveConfig(event)'><input type='submit' value='Edit Config'></form>";

    std::string ssid, password;
    loadWiFiConfig(ssid, password);
    resp += "<h2>WiFi</h2>";
    resp += "<p>SSID: " + ssid + "</p>";
    resp += "<form id='wifiForm' onsubmit='saveWifi(event)'><input type='submit' value='Edit WiFi'></form>";

    resp += "<h2>System</h2><a href='/reboot'><button>Reboot</button></a>";

    resp += "<script>";
    resp += "function showStatus(){document.getElementById('status').style.display='block';setTimeout(function(){document.getElementById('status').style.display='none';},3000);}";
    resp += "async function saveTank(e){e.preventDefault();const w=window.open('/tank_form','_blank','width=400,height=600');}";
    resp += "async function saveConfig(e){e.preventDefault();const w=window.open('/config_form','_blank','width=400,height=400');}";
    resp += "async function saveWifi(e){e.preventDefault();const w=window.open('/wifi_form','_blank','width=400,height=400');}";
    resp += "</script>";

    resp += "</body></html>";

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp.c_str(), resp.length());
    ESP_LOGI(TAG, "Served root page, level: %.1f%%", level_percent);
    return ESP_OK;
}

esp_err_t WebServer::tankFormHandler(httpd_req_t* req) {
    std::vector<CalibrationPoint> calibration;
    loadCalibrationFromNVS(calibration);

    int num_calibration_points = calibration.size();
    if (num_calibration_points < 3) num_calibration_points = 3;
    if (num_calibration_points > 8) num_calibration_points = 8;

    std::string resp = "<html><body><h1>Tank Settings</h1>";
    resp += "<form id='tankForm' onsubmit='save(event, \"tank\")'>";
    resp += "Height: <input type='text' name='tank_height' value='" + formatNumber(convertDistance(tank_height, "cm", dist_unit)) + "' id='tank_height' onchange='updateCalibrationPoints()'><br>";
    resp += "Offset: <input type='text' name='sensor_offset' value='" + formatNumber(convertDistance(sensor_offset, "cm", dist_unit)) + "' id='sensor_offset'><br>";
    resp += "Distance Unit: <select name='dist_unit' id='dist_unit' onchange='updateUnits(this.value)'>";
    for (const char* unit : {"mm", "cm", "m", "inches", "ft"}) {
        resp += "<option value='" + std::string(unit) + "' " + (dist_unit == unit ? "selected" : "") + ">" + unit + "</option>";
    }
    resp += "</select><br>";
    resp += "Volume: <input type='text' name='tank_volume' value='" + formatNumber(convertVolume(tank_volume, "liter", vol_unit)) + "' id='tank_volume'><br>";
    resp += "Volume Unit: <select name='vol_unit' id='vol_unit' onchange='updateVolumeUnit(this.value)'>";
    for (const char* unit : {"liter", "m³", "gallon", "imperial gallon"}) {
        resp += "<option value='" + std::string(unit) + "' " + (vol_unit == unit ? "selected" : "") + ">" + unit + "</option>";
    }
    resp += "</select><br>";
    resp += "Low Alarm (%): <input type='text' name='low_alarm_percent' value='" + formatNumber(low_alarm_percent) + "'>%<br>";
    resp += "High Alarm (%): <input type='text' name='high_alarm_percent' value='" + formatNumber(high_alarm_percent) + "'>%<br>";
    resp += "Shape: <select name='tank_shape' id='tank_shape' onchange='toggleCalibrationPoints(this.value)'>";
    for (const char* shape : {"rectangular", "cylindrical standing", "cylindrical laying flat", "custom"}) {
        resp += "<option value='" + std::string(shape) + "' " + (tank_shape == shape ? "selected" : "") + ">" + shape + "</option>";
    }
    resp += "</select><br>";

    // Add dropdown for number of calibration points
    resp += "<div id='calibration_settings' style='display:none'>";
    resp += "Number of Calibration Points: <select name='num_calibration_points' id='num_calibration_points' onchange='updateCalibrationPoints()'>";
    for (int i = 3; i <= 8; i++) {
        resp += "<option value='" + std::to_string(i) + "' " + (i == num_calibration_points ? "selected" : "") + ">" + std::to_string(i) + "</option>";
    }
    resp += "</select><br>";

    // Add fields for up to 8 calibration points
    for (int i = 0; i < 8; i++) {
        resp += "<div id='calibration_point_" + std::to_string(i) + "' style='display:" + (i < num_calibration_points ? "block" : "none") + "'>";
        resp += "Calibration Point " + std::to_string(i + 1) + ":<br>";
        if (i == 0) {
            resp += "Distance: <input type='text' name='calibration_distance_" + std::to_string(i) + "' value='0' disabled><br>";
            resp += "Percentage: <input type='text' name='calibration_percentage_" + std::to_string(i) + "' value='100' disabled><br>";
        } else if (i == num_calibration_points - 1) {
            resp += "Distance: <input type='text' name='calibration_distance_" + std::to_string(i) + "' value='" + formatNumber(tank_height) + "' id='last_calibration_distance' disabled><br>";
            resp += "Percentage: <input type='text' name='calibration_percentage_" + std::to_string(i) + "' value='0' disabled><br>";
        } else {
            float distance = (i < calibration.size()) ? calibration[i].distance : (tank_height / (num_calibration_points - 1)) * i;
            float percentage = (i < calibration.size()) ? calibration[i].percentage : 100.0 - (100.0 / (num_calibration_points - 1)) * i;
            resp += "Distance: <input type='text' name='calibration_distance_" + std::to_string(i) + "' value='" + formatNumber(distance) + "'><br>";
            resp += "Percentage: <input type='text' name='calibration_percentage_" + std::to_string(i) + "' value='" + formatNumber(percentage) + "'><br>";
        }
        resp += "</div>";
    }
    resp += "</div>";

    resp += "<input type='submit' value='Save'></form>";
    resp += "<script>";
    resp += "function updateUnits(newUnit){";
    resp += "  var h=document.getElementById('tank_height'), o=document.getElementById('sensor_offset'), cm_h=" + std::to_string(tank_height) + ", cm_o=" + std::to_string(sensor_offset) + ";";
    resp += "  h.value=(newUnit=='mm'?cm_h*10:(newUnit=='m'?cm_h/100:(newUnit=='inches'?cm_h/2.54:(newUnit=='ft'?cm_h/30.48:cm_h)))).toFixed(1);";
    resp += "  o.value=(newUnit=='mm'?cm_o*10:(newUnit=='m'?cm_o/100:(newUnit=='inches'?cm_o/2.54:(newUnit=='ft'?cm_o/30.48:cm_o)))).toFixed(1);";
    resp += "}";
    resp += "function updateVolumeUnit(newUnit){";
    resp += "  var v=document.getElementById('tank_volume'), liter=" + std::to_string(tank_volume) + ";";
    resp += "  v.value=(newUnit=='m³'?liter/1000:(newUnit=='gallon'?liter/3.78541:(newUnit=='imperial gallon'?liter/4.54609:liter))).toFixed(1);";
    resp += "}";
    resp += "function toggleCalibrationPoints(shape){";
    resp += "  var display = (shape == 'custom') ? 'block' : 'none';";
    resp += "  document.getElementById('calibration_settings').style.display = display;";
    resp += "  updateCalibrationPoints();";
    resp += "}";
    resp += "function updateCalibrationPoints(){";
    resp += "  var numPoints = document.getElementById('num_calibration_points').value;";
    resp += "  var tankHeight = parseFloat(document.getElementById('tank_height').value);";
    resp += "  for (var i = 0; i < 8; i++) {";
    resp += "    var pointDiv = document.getElementById('calibration_point_' + i);";
    resp += "    if (i < numPoints) {";
    resp += "      pointDiv.style.display = 'block';";
    resp += "      if (i == 0) {";
    resp += "        document.getElementById('calibration_distance_' + i).value = '0';";
    resp += "        document.getElementById('calibration_percentage_' + i).value = '100';";
    resp += "      } else if (i == numPoints - 1) {";
    resp += "        document.getElementById('calibration_distance_' + i).value = tankHeight;";
    resp += "        document.getElementById('calibration_percentage_' + i).value = '0';";
    resp += "      } else {";
    resp += "        var distance = (tankHeight / (numPoints - 1)) * i;";
    resp += "        var percentage = 100.0 - (100.0 / (numPoints - 1)) * i;";
    resp += "        document.getElementById('calibration_distance_' + i).value = distance.toFixed(1);";
    resp += "        document.getElementById('calibration_percentage_' + i).value = percentage.toFixed(1);";
    resp += "      }";
    resp += "    } else {";
    resp += "      pointDiv.style.display = 'none';";
    resp += "    }";
    resp += "  }";
    resp += "}";
    resp += "async function save(e,endpoint){";
    resp += "  e.preventDefault();";
    resp += "  const form=new FormData(e.target);";
    resp += "  form.set('tank_height', document.getElementById('tank_height').value);";
    resp += "  form.set('sensor_offset', document.getElementById('sensor_offset').value);";
    resp += "  form.set('tank_volume', document.getElementById('tank_volume').value);";
    resp += "  form.set('dist_unit', document.getElementById('dist_unit').value);";
    resp += "  form.set('vol_unit', document.getElementById('vol_unit').value);";
    resp += "  await fetch('/'+endpoint,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(form).toString()});";
    resp += "  window.opener.showStatus();window.opener.location.reload();window.close();}";
    resp += "window.onload = function() { toggleCalibrationPoints(document.getElementById('tank_shape').value); updateCalibrationPoints(); };";
    resp += "</script>";
    resp += "</body></html>";

    httpd_resp_send(req, resp.c_str(), resp.length());
    return ESP_OK;
}
esp_err_t WebServer::tankHandler(httpd_req_t* req) {
    char buf[2048];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
            ESP_LOGE(TAG, "Tank request timeout");
        } else {
            ESP_LOGE(TAG, "Tank request failed: %d", ret);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Tank request (POST) received, length=%d: %s", ret, buf);

    char param[64];
    float tank_height_new = tank_height;
    float tank_volume_new = tank_volume;
    float sensor_offset_new = sensor_offset;
    float low_alarm_percent_new = low_alarm_percent;
    float high_alarm_percent_new = high_alarm_percent;
    std::string tank_shape_new = tank_shape;
    std::string dist_unit_new = dist_unit;
    std::string vol_unit_new = vol_unit;
    int num_calibration_points = 3;

    if (httpd_query_key_value(buf, "dist_unit", param, sizeof(param)) == ESP_OK) {
        dist_unit_new = param;
    }
    if (httpd_query_key_value(buf, "vol_unit", param, sizeof(param)) == ESP_OK) {
        vol_unit_new = param;
    }
    if (httpd_query_key_value(buf, "tank_height", param, sizeof(param)) == ESP_OK) {
        tank_height_new = convertDistance(parseFloat(param, tank_height), dist_unit_new, "cm");
    }
    if (httpd_query_key_value(buf, "tank_volume", param, sizeof(param)) == ESP_OK) {
        tank_volume_new = convertVolume(parseFloat(param, tank_volume), vol_unit_new, "liter");
    }
    if (httpd_query_key_value(buf, "sensor_offset", param, sizeof(param)) == ESP_OK) {
        sensor_offset_new = convertDistance(parseFloat(param, sensor_offset), dist_unit_new, "cm");
    }
    if (httpd_query_key_value(buf, "low_alarm_percent", param, sizeof(param)) == ESP_OK) {
        low_alarm_percent_new = parseFloat(param, low_alarm_percent);
        if (low_alarm_percent_new > 100.0) low_alarm_percent_new = 100.0;
        if (low_alarm_percent_new < 0.0) low_alarm_percent_new = 0.0;
    }
    if (httpd_query_key_value(buf, "high_alarm_percent", param, sizeof(param)) == ESP_OK) {
        high_alarm_percent_new = parseFloat(param, high_alarm_percent);
        if (high_alarm_percent_new > 100.0) high_alarm_percent_new = 100.0;
        if (high_alarm_percent_new < 0.0) high_alarm_percent_new = 0.0;
    }
    if (httpd_query_key_value(buf, "tank_shape", param, sizeof(param)) == ESP_OK) {
        tank_shape_new = param;
    }
    if (httpd_query_key_value(buf, "num_calibration_points", param, sizeof(param)) == ESP_OK) {
        num_calibration_points = std::stoi(param);
        if (num_calibration_points < 3) num_calibration_points = 3;
        if (num_calibration_points > 8) num_calibration_points = 8;
    }

    // Handle calibration points
    std::vector<CalibrationPoint> calibration;
    for (int i = 0; i < num_calibration_points; i++) {
        std::string distance_key = "calibration_distance_" + std::to_string(i);
        std::string percentage_key = "calibration_percentage_" + std::to_string(i);
        if (httpd_query_key_value(buf, distance_key.c_str(), param, sizeof(param)) == ESP_OK) {
            float distance = parseFloat(param, 0.0);
            if (httpd_query_key_value(buf, percentage_key.c_str(), param, sizeof(param)) == ESP_OK) {
                float percentage = parseFloat(param, 0.0);
                calibration.push_back({distance, percentage});
            }
        }
    }

    tank_height = tank_height_new;
    tank_volume = tank_volume_new;
    sensor_offset = sensor_offset_new;
    low_alarm_percent = low_alarm_percent_new;
    high_alarm_percent = high_alarm_percent_new;
    tank_shape = tank_shape_new;
    dist_unit = dist_unit_new;
    vol_unit = vol_unit_new;

    // Save calibration points to NVS
    saveCalibrationToNVS(calibration);

    saveSettingsToNVS();

    ESP_LOGI(TAG, "Tank settings saved successfully");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t WebServer::configFormHandler(httpd_req_t* req) {
    std::string resp = "<html><body><h1>Config</h1>";
    resp += "<form id='configForm' onsubmit='save(event, \"config\")'>";
    resp += "Interval (ms): <input type='number' name='interval' min='500' max='10000' value='" + std::to_string(getTransmissionInterval()) + "'><br>";
    std::string device_name = getDeviceName();
    std::replace(device_name.begin(), device_name.end(), '+', ' ');
    resp += "Name: <input type='text' name='device_name' maxlength='31' value='" + device_name + "'><br>";
    resp += "<input type='submit' value='Save'></form>";
    resp += "<script>";
    resp += "async function save(e,endpoint){e.preventDefault();const form=new FormData(e.target);";
    resp += "await fetch('/'+endpoint,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(form).toString()});";
    resp += "window.opener.showStatus();window.close();}";
    resp += "</script>";
    resp += "</body></html>";

    httpd_resp_send(req, resp.c_str(), resp.length());
    return ESP_OK;
}

esp_err_t WebServer::configHandler(httpd_req_t* req) {
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Config request (POST): %s", buf);

    char param[32];
    if (httpd_query_key_value(buf, "interval", param, sizeof(param)) == ESP_OK) {
        uint32_t interval = std::stoi(param);
        setTransmissionInterval(interval);
    }
    if (httpd_query_key_value(buf, "device_name", param, sizeof(param)) == ESP_OK) {
        setDeviceName(param);
    }

    saveSettingsToNVS();

    ESP_LOGI(TAG, "Config saved");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t WebServer::wifiScanHandler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Starting WiFi scan on all channels");

    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %d", ret);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot determine WiFi mode");
        return ESP_FAIL;
    }

    bool was_ap_only = (current_mode == WIFI_MODE_AP);
    if (was_ap_only) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    wifi_scan_config_t scan_config = {};
    scan_config.ssid = nullptr;
    scan_config.channel = 0;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 500;
    scan_config.scan_time.active.max = 1000;

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed with error %d", ret);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        if (was_ap_only) esp_wifi_set_mode(WIFI_MODE_AP);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t* ap_list = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    std::string json = "[";
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + std::string((char*)ap_list[i].ssid) + "\",";
        json += "\"rssi\":" + std::to_string(ap_list[i].rssi) + ",";
        json += "\"channel\":" + std::to_string(ap_list[i].primary) + "}";
    }
    json += "]";
    free(ap_list);

    if (was_ap_only) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    ESP_LOGI(TAG, "WiFi scan completed, found %d APs", ap_count);
    return ESP_OK;
}

esp_err_t WebServer::wifiFormHandler(httpd_req_t* req) {
    std::string ssid, password;
    loadWiFiConfig(ssid, password);
    std::string resp = "<html><body><h1>WiFi Settings</h1>";
    resp += "<form id='wifiForm' onsubmit='saveWifi(event)'>";
    resp += "SSID: <select name='ssid' id='ssid'></select><br>";
    resp += "<button type='button' onclick='scanNetworks()'>Scan Networks</button><br>";
    resp += "Password: <input type='text' name='password' id='password' value='" + password + "'><br>";
    resp += "<input type='submit' value='Save & Connect'></form>";
    resp += "<br><form id='apModeForm' action='/wifi_reset' method='POST'><input type='submit' value='Switch to AP Mode'></form>";
    resp += "<script>";
    resp += "async function scanNetworks(){";
    resp += "  const res=await fetch('/wifi_scan');";
    resp += "  const networks=await res.json();";
    resp += "  const select=document.getElementById('ssid');";
    resp += "  select.innerHTML='';";
    resp += "  networks.forEach(n => {";
    resp += "    const opt=document.createElement('option');";
    resp += "    opt.value=n.ssid;opt.text=n.ssid + ' (' + n.rssi + ' dBm, Ch ' + n.channel + ')';";
    resp += "    select.appendChild(opt);";
    resp += "  });";
    resp += "}";
    resp += "async function saveWifi(e){";
    resp += "  e.preventDefault();";
    resp += "  const form=new FormData(e.target);";
    resp += "  await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(form).toString()});";
    resp += "  window.opener.showStatus();window.close();";
    resp += "}";
    resp += "window.onload = scanNetworks;";
    resp += "</script>";
    resp += "</body></html>";

    httpd_resp_send(req, resp.c_str(), resp.length());
    return ESP_OK;
}

esp_err_t WebServer::wifiHandler(httpd_req_t* req) {
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "WiFi request (POST): %s", buf);

    char ssid[33], password[65];
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "password", password, sizeof(password)) == ESP_OK) {
        connectToWiFi(ssid, password);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    ESP_LOGI(TAG, "WiFi STA config saved: SSID=%s", ssid);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t WebServer::wifiResetHandler(httpd_req_t* req) {
    ESP_LOGI(TAG, "WiFi reset requested, erasing credentials and switching to AP mode");
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_config", NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for WiFi reset: %d", ret);
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    startWiFiAP();
    httpd_resp_send(req, "OK", 2);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t WebServer::rebootHandler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Reboot requested");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void WebServer::start() {
    ESP_LOGI(TAG, "Starting HTTP server...");
    wifi_mode_t mode;
    esp_err_t wifi_status = esp_wifi_get_mode(&mode);
    if (wifi_status == ESP_OK) {
        ESP_LOGI(TAG, "WiFi mode before httpd_start: %d", mode);
    } else {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %d", wifi_status);
    }

    esp_err_t err = httpd_start(&_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->rootHandler(r); }, .user_ctx = this };
    httpd_uri_t tank_form = { .uri = "/tank_form", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->tankFormHandler(r); }, .user_ctx = this };
    httpd_uri_t tank = { .uri = "/tank", .method = HTTP_POST, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->tankHandler(r); }, .user_ctx = this };
    httpd_uri_t config_form = { .uri = "/config_form", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->configFormHandler(r); }, .user_ctx = this };
    httpd_uri_t config = { .uri = "/config", .method = HTTP_POST, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->configHandler(r); }, .user_ctx = this };
    httpd_uri_t wifi_scan = { .uri = "/wifi_scan", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->wifiScanHandler(r); }, .user_ctx = this };
    httpd_uri_t wifi_form = { .uri = "/wifi_form", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->wifiFormHandler(r); }, .user_ctx = this };
    httpd_uri_t wifi = { .uri = "/wifi", .method = HTTP_POST, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->wifiHandler(r); }, .user_ctx = this };
    httpd_uri_t wifi_reset = { .uri = "/wifi_reset", .method = HTTP_POST, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->wifiResetHandler(r); }, .user_ctx = this };
    httpd_uri_t reboot = { .uri = "/reboot", .method = HTTP_GET, .handler = [](httpd_req_t* r) { return static_cast<WebServer*>(r->user_ctx)->rebootHandler(r); }, .user_ctx = this };

    httpd_register_uri_handler(_server, &root);
    httpd_register_uri_handler(_server, &tank_form);
    httpd_register_uri_handler(_server, &tank);
    httpd_register_uri_handler(_server, &config_form);
    httpd_register_uri_handler(_server, &config);
    httpd_register_uri_handler(_server, &wifi_scan);
    httpd_register_uri_handler(_server, &wifi_form);
    httpd_register_uri_handler(_server, &wifi);
    httpd_register_uri_handler(_server, &wifi_reset);
    httpd_register_uri_handler(_server, &reboot);

    ESP_LOGI(TAG, "HTTP server started");
}

void WebServer::startWiFiAP() {
    ESP_LOGI(TAG, "Starting WiFi AP...");
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "NMEA2000_Sensor");
    wifi_config.ap.ssid_len = strlen("NMEA2000_Sensor");
    wifi_config.ap.channel = 11;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to set AP mode: %d", ret);
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to set AP config: %d", ret);
    ret = esp_wifi_start();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to start WiFi AP: %d", ret);
    ESP_LOGI(TAG, "WiFi AP started on Channel 11");
}

void WebServer::connectToWiFi(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "Connecting to WiFi STA: SSID=%s, Password=%s", ssid, password);
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.channel = 6;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to set STA mode: %d", ret);
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to set STA config: %d", ret);
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to disable power saving: %d", ret);
    ret = esp_wifi_start();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to start WiFi STA: %d", ret);
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(80));
    ESP_LOGI(TAG, "WiFi STA started, attempting connection...");

    nvs_handle_t nvs;
    ret = nvs_open("wifi_config", NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "password", password);
        esp_err_t commit_ret = nvs_commit(nvs);
        if (commit_ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi credentials committed to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to commit WiFi credentials to NVS: %d", commit_ret);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for WiFi config: %d", ret);
    }
}

float WebServer::getLevelPercentage() {
    float raw_distance = _sensor->getLevelPercentage();
    float distance = raw_distance - sensor_offset;
    if (distance < 0) distance = 0;
    float height = tank_height - sensor_offset;
    if (height <= 0) return 100.0;

    if (tank_shape == "rectangular" || tank_shape == "cylindrical standing") {
        return 100.0 * (1.0 - distance / height);
    } else if (tank_shape == "cylindrical laying flat") {
        float h = distance / height;
        float volume_percent = std::acos(1 - 2 * h) / M_PI + (2 * h - 1) * std::sqrt(2 * h - h * h) / M_PI;
        return 100.0 * (1.0 - volume_percent);
    } else if (tank_shape == "custom") {
        std::vector<CalibrationPoint> calibration;
        loadCalibrationFromNVS(calibration);
        if (calibration.empty()) return 0.0;
        if (distance <= calibration.front().distance) return calibration.front().percentage;
        if (distance >= calibration.back().distance) return calibration.back().percentage;

        for (size_t i = 0; i < calibration.size() - 1; i++) {
            if (distance >= calibration[i].distance && distance <= calibration[i + 1].distance) {
                float d1 = calibration[i].distance;
                float p1 = calibration[i].percentage;
                float d2 = calibration[i + 1].distance;
                float p2 = calibration[i + 1].percentage;
                return p1 + (distance - d1) * (p2 - p1) / (d2 - d1);
            }
        }
    }
    return 0.0;
}

float WebServer::getTankVolumeLiters() {
    float percent = getLevelPercentage() / 100.0;
    return tank_volume * percent;
}

uint32_t WebServer::getTransmissionInterval() {
    return _nmea2000->getTransmissionInterval();
}

std::string WebServer::getDeviceName() {
    std::string name = _nmea2000->getDeviceName();
    std::replace(name.begin(), name.end(), '+', ' ');
    return name;
}

void WebServer::setTransmissionInterval(uint32_t interval) {
    _nmea2000->setTransmissionInterval(interval);
}

void WebServer::setDeviceName(const std::string& name) {
    _nmea2000->setDeviceName(name);
}

void WebServer::updateCalibration(const std::vector<CalibrationPoint>& calibration) {
    _sensor->loadCalibrationFromNVS(calibration);
}

void WebServer::checkAndSendAlarms() {
    float volume_liters = getTankVolumeLiters();
    float low_alarm_liters = getLowAlarmVolume();
    float high_alarm_liters = getHighAlarmVolume();
    static bool low_alarm_active = false;
    static bool high_alarm_active = false;

    if (volume_liters <= low_alarm_liters && !low_alarm_active) {
        ESP_LOGI(TAG, "Low fluid level alarm triggered: %.1f liters", volume_liters);
        low_alarm_active = true;
    } else if (volume_liters > low_alarm_liters && low_alarm_active) {
        low_alarm_active = false;
    }

    if (volume_liters >= high_alarm_liters && !high_alarm_active) {
        ESP_LOGI(TAG, "High fluid level alarm triggered: %.1f liters", volume_liters);
        high_alarm_active = true;
    } else if (volume_liters < high_alarm_liters && high_alarm_active) {
        high_alarm_active = false;
    }
}

void WebServer::saveCalibrationToNVS(const std::vector<CalibrationPoint>& calibration) {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("calibration", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for calibration: %d", ret);
        return;
    }

    // Save the number of calibration points
    nvs_set_u8(nvs, "num_points", static_cast<uint8_t>(calibration.size()));

    // Save the calibration points as a blob
    std::vector<float> blob(calibration.size() * 2);
    for (size_t i = 0; i < calibration.size(); i++) {
        blob[i * 2] = calibration[i].distance;
        blob[i * 2 + 1] = calibration[i].percentage;
    }
    nvs_set_blob(nvs, "points", blob.data(), blob.size() * sizeof(float));

    // Commit the changes
    esp_err_t commit_ret = nvs_commit(nvs);
    if (commit_ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration committed to NVS, points: %d", calibration.size());
    } else {
        ESP_LOGE(TAG, "Failed to commit calibration to NVS: %d", commit_ret);
    }
    nvs_close(nvs);
}

void WebServer::loadCalibrationFromNVS(std::vector<CalibrationPoint>& calibration) {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("calibration", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for calibration: %d", ret);
        return;
    }

    // Get the number of calibration points
    uint8_t num_points = 0;
    ret = nvs_get_u8(nvs, "num_points", &num_points);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get number of calibration points: %d", ret);
        nvs_close(nvs);
        return;
    }

    // Load the calibration points as a blob
    size_t blob_size = num_points * 2 * sizeof(float);
    std::vector<float> blob(num_points * 2);
    ret = nvs_get_blob(nvs, "points", blob.data(), &blob_size);
    if (ret == ESP_OK && blob_size == num_points * 2 * sizeof(float)) {
        calibration.clear();
        for (uint8_t i = 0; i < num_points; i++) {
            CalibrationPoint point;
            point.distance = blob[i * 2];
            point.percentage = blob[i * 2 + 1];
            calibration.push_back(point);
        }
        ESP_LOGI(TAG, "Loaded %d calibration points from NVS", num_points);
    } else {
        ESP_LOGE(TAG, "Failed to load calibration points from NVS: %d", ret);
    }
    nvs_close(nvs);
}

void WebServer::loadWiFiConfig(std::string& ssid, std::string& password) {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_config", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for WiFi config: %d", ret);
        return;
    }

    char ssid_buf[33] = {0}, pwd_buf[65] = {0};
    size_t len = sizeof(ssid_buf);
    ret = nvs_get_str(nvs, "ssid", ssid_buf, &len);
    if (ret == ESP_OK) {
        ssid = ssid_buf;
        ESP_LOGI(TAG, "Loaded WiFi SSID: %s", ssid.c_str());
    } else {
        ESP_LOGW(TAG, "No WiFi SSID found in NVS: %d", ret);
    }
    len = sizeof(pwd_buf);
    ret = nvs_get_str(nvs, "password", pwd_buf, &len);
    if (ret == ESP_OK) {
        password = pwd_buf;
        ESP_LOGI(TAG, "Loaded WiFi password");
    } else {
        ESP_LOGW(TAG, "No WiFi password found in NVS: %d", ret);
    }
    nvs_close(nvs);
}

void WebServer::saveSettingsToNVS() {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("n2k_config", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for settings: %d", ret);
        return;
    }

    DeviceSettings_t settings;
    strncpy(settings.deviceName, getDeviceName().c_str(), sizeof(settings.deviceName) - 1);
    settings.deviceName[sizeof(settings.deviceName) - 1] = '\0';
    settings.tankHeight = tank_height;
    settings.tankVolume = tank_volume;
    settings.sensorOffset = sensor_offset;
    settings.lowAlarmPercent = low_alarm_percent;
    settings.highAlarmPercent = high_alarm_percent;
    strncpy(settings.tankShape, tank_shape.c_str(), sizeof(settings.tankShape) - 1);
    settings.tankShape[sizeof(settings.tankShape) - 1] = '\0';
    strncpy(settings.distUnit, dist_unit.c_str(), sizeof(settings.distUnit) - 1);
    settings.distUnit[sizeof(settings.distUnit) - 1] = '\0';
    strncpy(settings.volUnit, vol_unit.c_str(), sizeof(settings.volUnit) - 1);
    settings.volUnit[sizeof(settings.volUnit) - 1] = '\0';
    settings.interval = getTransmissionInterval();

    ret = nvs_set_blob(nvs, "settings", &settings, sizeof(DeviceSettings_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set settings blob: %d", ret);
        nvs_close(nvs);
        return;
    }

    esp_err_t commit_ret = nvs_commit(nvs);
    if (commit_ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings committed to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit settings to NVS: %d", commit_ret);
    }
    nvs_close(nvs);
}

void WebServer::loadSettingFromNVS() {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("n2k_config", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for settings: %d", ret);
        return;
    }

    DeviceSettings_t settings;
    size_t size = sizeof(DeviceSettings_t);
    ret = nvs_get_blob(nvs, "settings", &settings, &size);
    if (ret == ESP_OK && size == sizeof(DeviceSettings_t)) {
        setDeviceName(settings.deviceName);
        tank_height = settings.tankHeight;
        tank_volume = settings.tankVolume;
        sensor_offset = settings.sensorOffset;
        low_alarm_percent = settings.lowAlarmPercent;
        high_alarm_percent = settings.highAlarmPercent;
        tank_shape = settings.tankShape;
        dist_unit = settings.distUnit;
        vol_unit = settings.volUnit;
        setTransmissionInterval(settings.interval);
        ESP_LOGI(TAG, "Settings loaded from NVS");
    } else {
        ESP_LOGW(TAG, "No settings found in NVS or invalid size, using defaults: %d", ret);
    }
    nvs_close(nvs);
}

template<typename T>
void WebServer::saveToNVM(const char* key, T value) {
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("n2k_config", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for key '%s': %d", key, ret);
        return;
    }

    if constexpr (std::is_same<T, uint32_t>::value) {
        ret = nvs_set_u32(nvs, key, value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set uint32 '%s' = %u: %d", key, value, ret);
        } else {
            ESP_LOGI(TAG, "Saved uint32 '%s' = %u to NVS", key, value);
        }
    } else if constexpr (std::is_same<T, std::string>::value) {
        ret = nvs_set_str(nvs, key, value.c_str());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set string '%s' = '%s': %d", key, value.c_str(), ret);
        } else {
            ESP_LOGI(TAG, "Saved string '%s' = '%s' to NVS", key, value.c_str());
        }
    }

    esp_err_t commit_ret = nvs_commit(nvs);
    if (commit_ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS commit successful for key '%s'", key);
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS for key '%s': %d", key, commit_ret);
    }
    nvs_close(nvs);
}