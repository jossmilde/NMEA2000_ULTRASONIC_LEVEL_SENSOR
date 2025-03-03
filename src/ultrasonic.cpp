#include "ultrasonic.h"
#include <esp_log.h>
#include <algorithm>
#include "calibration.h"

//static const char* TAG = "Ultrasonic";

Ultrasonic::Ultrasonic() : simulatedDistance(100.0) {
    calibrationPoints.push_back({20.0, 100.0});
    calibrationPoints.push_back({120.0, 0.0});  // Updated to 120 cm
}

float Ultrasonic::getLevelPercentage() {
    return interpolateLevel(simulatedDistance);
}

void Ultrasonic::setSimulatedDistance(float distance) {
    simulatedDistance = (distance > maxDistance) ? maxDistance : distance;  // Cap at 120 cm
}

void Ultrasonic::loadCalibrationFromNVS(const std::vector<CalibrationPoint>& calibration) {
    if (calibration.empty()) return;
    calibrationPoints = calibration;
    for (auto& point : calibrationPoints) {
        if (point.distance > maxDistance) point.distance = maxDistance;  // Cap at 120 cm
    }
    std::sort(calibrationPoints.begin(), calibrationPoints.end(), 
              [](const CalibrationPoint& a, const CalibrationPoint& b) { return a.distance < b.distance; });
}

float Ultrasonic::interpolateLevel(float distance) {
    if (calibrationPoints.empty()) return 0.0;
    if (distance <= calibrationPoints.front().distance) return calibrationPoints.front().percentage;
    if (distance >= calibrationPoints.back().distance) return calibrationPoints.back().percentage;

    for (size_t i = 0; i < calibrationPoints.size() - 1; i++) {
        if (distance >= calibrationPoints[i].distance && distance <= calibrationPoints[i + 1].distance) {
            float d1 = calibrationPoints[i].distance;
            float p1 = calibrationPoints[i].percentage;
            float d2 = calibrationPoints[i + 1].distance;
            float p2 = calibrationPoints[i + 1].percentage;
            return p1 + (distance - d1) * (p2 - p1) / (d2 - d1);  // Linear interpolation
        }
    }
    return 0.0;
}