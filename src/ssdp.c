/*
  ssdp.c simple ssdp protocol implementation for idf

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
#include <stdio.h>
#include "ssdp.h"
#include "esp_log.h"

static const char *TAG = "esp-ssdp";

/*
* Defines
*/
#define SSDP_INTERVAL     1200
#define SSDP_PORT         1900
#define SSDP_METHOD_SIZE  10
#define SSDP_URI_SIZE     2
#define SSDP_BUFFER_SIZE  64
#define SSDP_MULTICAST_TTL 2
#define SSDP_UUID_ROOT "38323636-4558-4dda-9188-cda0e6"
#define SSDP_MULTICAST_ADDR "239.255.255.250"

/*
* Sizes
*/
#define SSDP_UUID_SIZE              37
#define SSDP_SCHEMA_URL_SIZE        64
#define SSDP_DEVICE_TYPE_SIZE       64
#define SSDP_USN_SUFFIX_SIZE        64
#define SSDP_FRIENDLY_NAME_SIZE     64
#define SSDP_SERIAL_NUMBER_SIZE     32
#define SSDP_PRESENTATION_URL_SIZE  128
#define SSDP_MODEL_NAME_SIZE        64
#define SSDP_MODEL_URL_SIZE         128
#define SSDP_MODEL_NUMBER_SIZE      32
#define SSDP_MODEL_DESCRIPTION_SIZE 64
#define SSDP_SERVER_NAME_SIZE       64
#define SSDP_MANUFACTURER_NAME_SIZE 64
#define SSDP_MANUFACTURER_URL_SIZE  128
#define SSDP_SERVICES_DESCRIPTION_SIZE  256
#define SSDP_ICONS_DESCRIPTION_SIZE  256

/*
* Templates messages
*/

static const char SSDP_RESPONSE_TEMPLATE[]  =
    "HTTP/1.1 200 OK\r\n"
    "EXT:\r\n";

static const char SSDP_NOTIFY_TEMPLATE[]  =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "NTS: ssdp:alive\r\n";

static const char SSDP_PACKET_TEMPLATE[]  =
    "%s" //resonse or notify
    "CACHE-CONTROL: max-age=%u\r\n" // expire time
    "SERVER: %s UPNP/1.1 %s/%s\r\n" // server_name, model_name, model_number
    "USN: uuid:%s%s\r\n" // uuid, usn_suffix
    "%s: %s\r\n"  // "NT" or "ST", device_type
    "LOCATION: http://%u.%u.%u.%u:%u/%s\r\n" //LocalIP, port, schemaURL
    "\r\n";

static const char SSDP_SCHEMA_TEMPLATE[]  =
    "<?xml version=\"1.0\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
    "<specVersion>"
    "<major>1</major>"
    "<minor>0</minor>"
    "</specVersion>"
    "<URLBase>http://%u.%u.%u.%u:%u/</URLBase>" // LocalIP, port
    "<device>"
    "<deviceType>urn:schemas-upnp-org:device:%s:1</deviceType>" // device_type
    "<friendlyName>%s</friendlyName>" // friendly_name
    "<presentationURL>%s</presentationURL>" // presentation_url
    "<serialNumber>%s</serialNumber>" // serial_number
    "<modelName>%s</modelName>" // model_name
    "<modelDescription>%s</modelDescription>" // model_description
    "<modelNumber>%s</modelNumber>" // model_number
    "<modelURL>%s</modelURL>" // model_url
    "<manufacturer>%s</manufacturer>" // manufacturer_name
    "<manufacturerURL>%s</manufacturerURL>" // manufacturer_url
    "<UDN>uuid:%s</UDN>" // uuid
    "<serviceList>%s</serviceList>" // service_list
    "<iconList>%s</iconList>" // icon_list
    "</device>"
    "</root>\r\n"
    "\r\n";

/*
* Enums
*/

typedef enum {
    NONE,
    SEARCH,
    NOTIFY
} ssdp_method_t;

/*
* Struct definitions
*/

typedef struct {
    unsigned long process_time;
    int delay;
    //IPAddress _respondToAddr;
    uint16_t respondToPort;
    char respondType[SSDP_DEVICE_TYPE_SIZE];
    char usn_suffix[SSDP_USN_SUFFIX_SIZE];
} ssdp_reply_slot_item_t;

typedef struct {
    ssdp_config_t * config;
    //Task handle
    TaskHandle_t xHandle;
    //variables
    ssdp_reply_slot_item_t * replySlots;
} ssdp_task_config_t;

/*
* Global variables
*/

static = NULL;
static ssdp_task_config_t * ssdp_task_config = NULL;


/*
* Prototypes
*/

static void ssdp_set_UUID(const char *uuid);
static void ssdp_running_task(void *pvParameters);


/*
* Local Functions
*/

void ssdp_running_task(void *pvParameters)
{
    while (1) {
        //TODO
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}

void ssdp_set_UUID(const char **uuid, const char * root_uid)
{
    uint8_t mac[6];
    esp_err_t err  = esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, &mac, sizeof(mac) * 8);
    if (ESP_OK != err) {
        memset(mac, 0, 6);
        ESP_LOGW(TAG, "Not able to read MAC address, use 000000");
    }
    uint32_t chipId = ((uint16_t) ((uint64_t)mac)>>32);
    sprintf(*uuid, "%s%02x%02x%02x",
            root_uid,
            (uint16_t) ((chipId >> 16) & 0xff),
            (uint16_t) ((chipId >>  8) & 0xff),
            (uint16_t)   chipId        & 0xff  );

}

/*
* Global Functions
*/
esp_err_t ssdp_start(ssdp_config_t* configuration)
{
    esp_err_t ret = ESP_OK;
    if (! configuration) {
        ESP_LOGE(TAG, "Missing configuration parameter");
        return ESP_ERR_INVALID_ARG;
    }
    //if already have a config task it means it was not cleaned
    if (ssdp_task_config) {
        ESP_LOGE(TAG, "SSDP already started");
        return ESP_ERR_INVALID_STATE;
    }
    //Create task configuration workplace
    ssdp_task_config = (ssdp_task_config_t *)malloc(sizeof(ssdp_task_config_t));
    if (!ssdp_task_config) {
        ESP_LOGE(TAG, "No enough memory for ssdp task configuration");
        return ESP_ERR_NO_MEM;
    }
    //Init to 0 everywhere
    memset(ssdp_task_config, 0, sizeof(ssdp_task_config_t));
    //Create user configuration object
    ssdp_task_config->config = (ssdp_config_t *)malloc(sizeof(ssdp_config_t));
    if (!ssdp_task_config->config) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    //Init to 0 everywhere
    memset(ssdp_task_config->config, 0, sizeof(ssdp_config_t));
    ssdp_task_config->config->task_priority = configuration->task_priority;
    ssdp_task_config->config->stack_size = configuration->stack_size;
    ssdp_task_config->config->core_id = configuration->core_id;
    ssdp_task_config->config->port = configuration->port;
    ssdp_task_config->config->ttl = configuration->ttl;
    ssdp_task_config->config->interval = configuration->interval;
    ssdp_task_config->config->max_reply_slots = configuration->max_reply_slots;
    //Reply slots
    ssdp_task_config->replySlots = (ssdp_reply_slot_item_t *)malloc(sizeof(ssdp_reply_slot_item_t) *  configuration->max_reply_slots);
    if (!ssdp_task_config->replySlots) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    ssdp_task_config->config->mx_max_delay = configuration->mx_max_delay;

    //UUID
    ssdp_task_config->config->uuid = (char *)malloc(SSDP_UUID_SIZE +1);
    memset(ssdp_task_config->config->uuid, 0, SSDP_UUID_SIZE +1);
    if (!ssdp_task_config->config->uuid) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    if (!configuration->uuid_root || strlen(configuration->uuid_root)==)0 {
        ssdp_set_UUID(&ssdp_task_config->config->uuid,SSDP_UUID_ROOT);
    } else {
        if (strlen(configuration->uuid_root)==strlen(SSDP_UUID_ROOT)) {
            ssdp_set_UUID(&ssdp_task_config->config->uuid,configuration->uuid_root);
        } else {
            ESP_LOGE(TAG, "Wrong size of uuid root parameter");
            return ESP_ERR_INVALID_ARG;
        }
    }
    //Device type
    if (strlen(configuration->device_type)>SSDP_DEVICE_TYPE_SIZE) {
        ESP_LOGE(TAG, "Device type too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->device_type = (char *) malloc(strlen(configuration->device_type)+1);
    if (!ssdp_ssdp_task_config->config->device_type ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->device_type, configuration->device_type);

    //Friendly name
    if (strlen(configuration->friendly_name)>SSDP_FRIENDLY_NAME_SIZE) {
        ESP_LOGE(TAG, "Friendly name too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->friendly_name = (char *) malloc(strlen(configuration->friendly_name)+1);
    if (!ssdp_ssdp_task_config->config->friendly_name ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->friendly_name, configuration->friendly_name);

    //Serial Number
    if (strlen(configuration->serial_number)>SSDP_SERIAL_NUMBER_SIZE) {
        ESP_LOGE(TAG, "Serial number too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->serial_number = (char *) malloc(strlen(configuration->serial_number)+1);
    if (!ssdp_ssdp_task_config->config->serial_number ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->serial_number, configuration->serial_number);

    //Presentation url
    if (strlen(configuration->presentation_url)>SSDP_PRESENTATION_URL_SIZE) {
        ESP_LOGE(TAG, "Presentation url too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->presentation_url = (char *) malloc(strlen(configuration->presentation_url)+1);
    if (!ssdp_ssdp_task_config->config->presentation_url ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->presentation_url, configuration->presentation_url);

    //Manufacturer name
    if (strlen(configuration->manufacturer_name)>SSDP_MANUFACTURER_NAME_SIZE) {
        ESP_LOGE(TAG, "Manufacturer name too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->manufacturer_name = (char *) malloc(strlen(configuration->manufacturer_name)+1);
    if (!ssdp_ssdp_task_config->config->manufacturer_name ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->manufacturer_name, configuration->manufacturer_name);

    //Manufacturer url
    if (strlen(configuration->manufacturer_url)>SSDP_MANUFACTURER_URL_SIZE) {
        ESP_LOGE(TAG, "Manufacturer url too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->manufacturer_url = (char *) malloc(strlen(configuration->manufacturer_url)+1);
    if (!ssdp_ssdp_task_config->config->manufacturer_url ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->manufacturer_url, configuration->manufacturer_url);

    //Model name
    if (strlen(configuration->model_name)>SSDP_MODEL_NAME_SIZE) {
        ESP_LOGE(TAG, "Model name too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->model_name = (char *) malloc(strlen(configuration->model_name)+1);
    if (!ssdp_ssdp_task_config->config->model_name ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->model_name, configuration->model_name);

    //Model url
    if (strlen(configuration->model_url)>SSDP_MODEL_URL_SIZE) {
        ESP_LOGE(TAG, "Model url too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->model_url = (char *) malloc(strlen(configuration->model_url)+1);
    if (!ssdp_ssdp_task_config->config->model_url ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->model_url, configuration->model_url);

    //Model number
    if (strlen(configuration->model_number)>SSDP_MODEL_NUMBER_SIZE) {
        ESP_LOGE(TAG, "Model number too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->model_number = (char *) malloc(strlen(configuration->model_number)+1);
    if (!ssdp_ssdp_task_config->config->model_number ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->model_number, configuration->model_number);

    //Model description
    if (strlen(configuration->model_description)>SSDP_MODEL_DESCRIPTION_SIZE) {
        ESP_LOGE(TAG, "Model description too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->model_description = (char *) malloc(strlen(configuration->model_description)+1);
    if (!ssdp_ssdp_task_config->config->model_description ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->model_description, configuration->model_description);

    //Server name
    if (strlen(configuration->server_name)>SSDP_SERVER_NAME_SIZE) {
        ESP_LOGE(TAG, "Server name too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->server_name = (char *) malloc(strlen(configuration->server_name)+1);
    if (!ssdp_ssdp_task_config->config->server_name ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->_servername, configuration->_servername);


    //Services description
    if (strlen(configuration->services_description)>SSDP_SERVICES_DESCRIPTION_SIZE) {
        ESP_LOGE(TAG, "Services description too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->services_description = (char *) malloc(strlen(configuration->services_description)+1);
    if (!ssdp_ssdp_task_config->config->services_description ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->services_description, configuration->services_description);


    //Icons description
    if (strlen(configuration->icons_description)>SSDP_ICONS_DESCRIPTION_SIZE) {
        ESP_LOGE(TAG, "Icons description too long");
        return ESP_ERR_INVALID_ARG;
    }
    ssdp_task_config->config->icons_description = (char *) malloc(strlen(configuration->icons_description)+1);
    if (!ssdp_ssdp_task_config->config->icons_description ) {
        ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
        return ESP_ERR_NO_MEM;
    }
    strcpy(ssdp_task_config->config->icons_description, configuration->icons_description);

    //Task creation
    BaseType_t  res =  xTaskCreatePinnedToCore(ssdp_running_task, "ssdp_running_task", configuration->stack_size, NULL, configuration->task_priority, &ssdp_task_config->xHandle, configuration->core_id);
    if (!(res==pdPASS && xHandle)) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ssdpd_stop()
{
    ESP_LOGD(TAG, "Stopping SSDP");
    if (ssdp_task_config) {
        //Delete the Task
        if (ssdp_task_config->xHandle) {
            vTaskDelete(ssdp_task_config->xHandle);
            ssdp_task_config->xHandle = NULL;
        }
        //Free memory
        if (ssdp_task_config->config) {
            free(ssdp_task_config->config->device_type);
            free(ssdp_task_config->config->friendly_name);
            free(ssdp_task_config->config->serial_number);
            free(ssdp_task_config->config->presentation_url);
            free(ssdp_task_config->config->manufacturer_name);
            free(ssdp_task_config->config->manufacturer_url);
            free(ssdp_task_config->config->model_name );
            free(ssdp_task_config->config->model_url);
            free(ssdp_task_config->config->model_number);
            free(ssdp_task_config->config->model_description);
            free(ssdp_task_config->config->server_name );
            free(ssdp_task_config->config->services_description);
            free(ssdp_task_config->config->icons_description);
            free(ssdp_task_config->ssdp_config);
        }
        free(ssdp_task_config->replySlots);
        free(ssdp_task_config);
        ssdp_task_config = NULL;
    }
    return ESP_OK;
}

const char* ssdp_schema()
{
    return NULL;
}