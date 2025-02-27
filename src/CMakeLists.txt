idf_component_register(SRCS "main.cpp" "n2k_can_driver.cpp" "ultrasonic.cpp" "web_server.cpp"
                       INCLUDE_DIRS "." "../.pio/libdeps/esp32dev/NMEA2000-library/src"
                       REQUIRES nvs_flash driver esp_wifi esp_http_server)