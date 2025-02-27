#include "ultrasonic.h"
#include <esp_timer.h>
#include <math.h>

Ultrasonic::Ultrasonic() {
    // No hardware setup needed for dummy data
}

float Ultrasonic::getLevelPercentage() {
    // Simulate fluid level oscillating between 0% and 100% over ~60 seconds
    uint64_t time_ms = esp_timer_get_time() / 1000;  // Milliseconds
    float angle = (time_ms % 60000) * (2 * M_PI / 60000.0);  // One cycle every 60s
    float level = (sin(angle) + 1.0) * 50.0;  // Range 0 to 100%
    return level;
}