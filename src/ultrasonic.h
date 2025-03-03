#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <vector>
#include "calibration.h"

class Ultrasonic {
public:
    Ultrasonic();
    float getLevelPercentage();
    void setSimulatedDistance(float distance);
    void loadCalibrationFromNVS(const std::vector<CalibrationPoint>& calibration);

private:
    float simulatedDistance;
    std::vector<CalibrationPoint> calibrationPoints;
    float interpolateLevel(float distance);
    const float maxDistance = 120.0;  // Max 120 cm
};

#endif