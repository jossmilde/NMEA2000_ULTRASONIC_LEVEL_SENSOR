#include "n2k_can_driver.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <string>

static const char* TAG = "N2kCanDriver";

N2kCanDriver::N2kCanDriver(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t rs_pin) 
    : _tx_pin(tx_pin), _rx_pin(rx_pin), _rs_pin(rs_pin), _is_open(false), _transmission_interval_ms(1000) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("nmea_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        char name[32] = "Ultrasonic Level Sensor";
        size_t len = sizeof(name);
        nvs_get_str(nvs, "device_name", name, &len);
        _device_name = name;
        nvs_get_u32(nvs, "tx_interval", &_transmission_interval_ms);
        nvs_close(nvs);
    } else {
        _device_name = "Ultrasonic Level Sensor";
        _transmission_interval_ms = 1000;
    }
}

void N2kCanDriver::Init() {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << _rs_pin);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(_rs_pin, 0);
    ESP_LOGI(TAG, "RS pin %d set low for high-speed mode", _rs_pin);

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(_tx_pin, _rx_pin, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = 5;
    g_config.rx_queue_len = 5;
    g_config.alerts_enabled = TWAI_ALERT_NONE;
    g_config.clkout_divider = 0;
    g_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    g_config.controller_id = 0;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "TWAI driver installed");
        if (twai_start() == ESP_OK) {
            ESP_LOGI(TAG, "TWAI driver started");
            _is_open = true;
        } else {
            ESP_LOGE(TAG, "Failed to start TWAI driver");
        }
    } else {
        ESP_LOGE(TAG, "Failed to install TWAI driver");
    }
}

N2kCanDriver::~N2kCanDriver() {
    if (_is_open) {
        twai_stop();
        twai_driver_uninstall();
        ESP_LOGI(TAG, "TWAI driver stopped and uninstalled");
    }
    gpio_set_level(_rs_pin, 1);
}

void N2kCanDriver::setDeviceName(const std::string& name) {
    _device_name = name.substr(0, 31);
    nvs_handle_t nvs;
    if (nvs_open("nmea_config", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "device_name", _device_name.c_str());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

std::string N2kCanDriver::getDeviceName() const {
    return _device_name;
}

void N2kCanDriver::setTransmissionInterval(uint32_t interval_ms) {
    _transmission_interval_ms = (interval_ms < 500) ? 500 : (interval_ms > 10000 ? 10000 : interval_ms);
    nvs_handle_t nvs;
    if (nvs_open("nmea_config", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "tx_interval", _transmission_interval_ms);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

uint32_t N2kCanDriver::getTransmissionInterval() const {
    return _transmission_interval_ms;
}

bool N2kCanDriver::CANSendFrame(unsigned long id, unsigned char len, const unsigned char* buf, bool wait_sent) {
    if (!_is_open) return false;
    twai_message_t message = {};
    message.identifier = id;
    message.data_length_code = len;
    message.extd = 1;
    memcpy(message.data, buf, len);
    esp_err_t result = twai_transmit(&message, wait_sent ? pdMS_TO_TICKS(10) : 0);
    return (result == ESP_OK);
}

bool N2kCanDriver::CANOpen() {
    return _is_open;
}

bool N2kCanDriver::CANGetFrame(unsigned long& id, unsigned char& len, unsigned char* buf) {
    if (!_is_open) return false;
    twai_message_t message;
    if (twai_receive(&message, pdMS_TO_TICKS(10)) == ESP_OK) {
        if (message.extd) {
            id = message.identifier;
            len = message.data_length_code;
            memcpy(buf, message.data, len);
            return true;
        }
    }
    return false;
}//last known n2k_can_driver.cpp