#pragma once
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "esp_arduino_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER
#error "ZOnlinE 708 diagnostic adapters require ESP32-S3 SYSTIMER"
#endif
#if ESP_IDF_VERSION_MAJOR!=5 || ESP_IDF_VERSION_MINOR!=5 || ESP_IDF_VERSION_PATCH!=2
#error "ZOnlinE 708 diagnostic adapters require IDF 5.5.2; review before SDK changes"
#endif
#if ESP_ARDUINO_VERSION_MAJOR!=3 || ESP_ARDUINO_VERSION_MINOR!=3 || ESP_ARDUINO_VERSION_PATCH!=7
#error "ZOnlinE 708 diagnostic build requires Arduino ESP32 Core 3.3.7"
#endif
#if CONFIG_FREERTOS_HZ!=1000
#error "ZOnlinE 708 tick bounds require the tested 1000 Hz configuration"
#endif
