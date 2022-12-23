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
    uint8_t max_reply_slots;
    uint16_t mx_max_delay;
    char * uuid_root;
    char * uuid;
    char * device_type;
    char * friendly_name;
    char * serial_number;
    char * presentation_url;
    char * manufacturer_name;
    char * manufacturer_url;
    char * model_name;
    char * model_url;
    char * model_number;
    char * model_description;
    char * server_name;
    char * services_description;
    char * icons_description;
} ssdp_config_t;

#define SDDP_DEFAULT_CONFIG() {                            \
        .task_priority       = tskIDLE_PRIORITY+5,         \
        .stack_size          = 4096,                       \
        .core_id             = tskNO_AFFINITY,             \
        .port                = 80,                         \
        .ttl                 = 2,                          \
        .interval            = 1200,                       \
        .max_reply_slots     = 5,                          \
        .mx_max_delay        = 10000,                      \
        .uuid_root           = NULL,                       \
        .uuid                =  NULL,                      \
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
        .server_name          = "Espressif/1.0",           \
        .services_description = NULL,                      \
        .icons_description    = NULL                       \
}

esp_err_t ssdp_start(ssdp_config_t* configuration);

esp_err_t ssdpd_stop();

const char* ssdp_schema();


#ifdef __cplusplus
}
#endif

#endif /* ESP_SSDP_H_ */