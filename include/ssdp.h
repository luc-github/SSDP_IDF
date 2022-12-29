/*
  ssdp.h simple ssdp protocol implementation for idf

  Copyright (c) 2022 Luc Lebosse. All rights reserved.
  This code is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with This code; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef ESP_SSDP_H_
#define ESP_SSDP_H_
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned task_priority;
    size_t stack_size;
    BaseType_t core_id;
    uint8_t ttl;
    uint16_t port;
    uint32_t interval;
    uint16_t mx_max_delay;
    const char * uuid_root;
    const char * uuid;
    const char * schema_url;
    const char * device_type;
    const char * friendly_name;
    const char * serial_number;
    const char * presentation_url;
    const char * manufacturer_name;
    const char * manufacturer_url;
    const char * model_name;
    const char * model_url;
    const char * model_number;
    const char * model_description;
    const char * server_name;
    const char * services_description;
    const char * icons_description;
} ssdp_config_t;

#define SDDP_DEFAULT_CONFIG() {                            \
        .task_priority       = tskIDLE_PRIORITY+5,         \
        .stack_size          = 4096,                       \
        .core_id             = tskNO_AFFINITY,             \
        .ttl                 = 2,                          \
        .port                = 80,                         \
        .interval            = 1200,                       \
        .mx_max_delay        = 10000,                      \
        .uuid_root           = NULL,                       \
        .uuid                =  NULL,                      \
        .schema_url          = "description.xml",          \
        .device_type         = "Basic",                    \
        .friendly_name       = "ESP32",                    \
        .serial_number       = "000000",                   \
        .presentation_url    = "/",                        \
        .manufacturer_name   = "Espressif Systems",        \
        .manufacturer_url    = "https://www.espressif.com",\
        .model_name          = "ESP32",                    \
        .model_url           = "https://www.espressif.com",\
        .model_number         = "12345",                   \
        .model_description    = NULL,                      \
        .server_name          = "SSDPServer/1.0",           \
        .services_description = NULL,                      \
        .icons_description    = NULL                       \
}

esp_err_t ssdp_start(ssdp_config_t* configuration);

esp_err_t ssdp_stop();

const char* get_ssdp_schema_str();


#ifdef __cplusplus
}
#endif

#endif /* ESP_SSDP_H_ */