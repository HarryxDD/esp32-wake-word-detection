cmake_minimum_required(VERSION 3.16.0)

# Exclude unnecessary components to reduce binary size
set(EXCLUDE_COMPONENTS
    "bt"
    "console"
    "app_trace"
    "perfmon"
    "unity"
    "coap"
    "asio"
    "cmd"
    "espcoredump"
    "app_update"
    "ulp"
    "usb"
    "touch_element"
    "rmaker_common"
    "json_parser"
    "json_generator"
    "cbor"
    "jsmn"
    "openssl"
    "expat"
    "wear_levelling"
    "fatfs"
    "spiffs"
    "littlefs"
    "sdmmc"
    "sdspi"
    "esp_hid"
    "esp_https_ota"
    "esp_https_server"
    "esp_local_ctrl"
    "wifi_provisioning"
    "protocomm"
    "protobuf-c"
    "mdns"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-tflite-wwd)
