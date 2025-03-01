#ifndef N2K_CAN_DRIVER_H
#define N2K_CAN_DRIVER_H

#include "NMEA2000.h"
#include <driver/twai.h>
#include <driver/gpio.h>
#include <string>

class N2kCanDriver : public tNMEA2000 {
public:
    N2kCanDriver(gpio_num_t tx_pin = GPIO_NUM_27, 
                 gpio_num_t rx_pin = GPIO_NUM_26, 
                 gpio_num_t rs_pin = GPIO_NUM_23);
    virtual ~N2kCanDriver();
    void Init();  // Manual TWAI init

    void setDeviceName(const std::string& name);
    std::string getDeviceName() const;
    void setTransmissionInterval(uint32_t interval_ms);
    uint32_t getTransmissionInterval() const;

protected:
    bool CANSendFrame(unsigned long id, unsigned char len, const unsigned char* buf, bool wait_sent = true) override;
    bool CANOpen() override;
    bool CANGetFrame(unsigned long& id, unsigned char& len, unsigned char* buf) override;

private:
    gpio_num_t _tx_pin;
    gpio_num_t _rx_pin;
    gpio_num_t _rs_pin;
    bool _is_open;
    std::string _device_name;
    uint32_t _transmission_interval_ms;
};

#endif