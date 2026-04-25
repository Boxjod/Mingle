# Box2Robot CAM — ESP32-S3 M5AtomS3R-CAM
cmake_minimum_required(VERSION 3.13)

set(EXCLUDE_COMPONENTS
    "espressif__esp_insights"
    "espressif__esp_diagnostics"
    "espressif__esp_diag_data_store"
    "espressif__esp_rainmaker"
    "espressif__esp_schedule"
    "espressif__rmaker_common"
    "espressif__esp-zboss-lib"
    "espressif__esp-zigbee-lib"
    "espressif__esp_rcp_update"
    "espressif__network_provisioning"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(box2robot-cam)
