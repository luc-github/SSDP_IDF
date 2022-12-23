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
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_mac.h"

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
    "<URLBase>http://%s:%u/</URLBase>" // LocalIPStr, port
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
    char * schema;
} ssdp_task_config_t;

/*
* Global variables
*/
static ssdp_task_config_t * ssdp_task_config = NULL;


/*
* Prototypes
*/

static  void ssdp_set_UUID(char **uuid, const char * root_uid);
static void ssdp_running_task(void *pvParameters);
static char * ssdp_get_LocalIP();

/*
* Local Functions
*/

char * ssdp_get_LocalIP()
{
    esp_err_t  err;
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_t * netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif =esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }
    if (netif == NULL) {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    if (netif == NULL) {
        return "0.0.0.0";
    }
    err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address info. Error 0x%x", err);
        return "0.0.0.0";
    }
    return ip4addr_ntoa((const ip4_addr_t*) &ip_info.ip);
}

/* Add a socket, either IPV4-only or IPV6 dual mode, to the IPV4
   multicast group */
static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if)
{
    if (!ssdp_task_config || !ssdp_task_config->config) {
        ESP_LOGE(TAG, "SSDP is not started.");
        return -1;
    }
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;
    // Configure source interface
    imreq.imr_interface.s_addr = IPADDR_ANY;
    // Configure multicast address to listen to
    err = inet_aton(SSDP_MULTICAST_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV4 multicast address '%s' is invalid.", SSDP_MULTICAST_ADDR);
        // Errors in the return value have to be negative
        err = -1;
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", SSDP_MULTICAST_ADDR);
    }

    if (assign_source_if) {
        // Assign the IPv4 multicast source interface, via its IP
        // (only necessary if this socket is IPV4 only)
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
                         sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

err:
    return err;
}



static int create_multicast_ipv4_socket(void)
{
    if (!ssdp_task_config || !ssdp_task_config->config) {
        ESP_LOGE(TAG, "SSDP is not started.");
        return -1;
    }
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Bind the socket to any address
    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(SSDP_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }


    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = ssdp_task_config->config->ttl;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = 1;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    err = socket_add_ipv4_multicast_group(sock, true);
    if (err < 0) {
        goto err;
    }

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}



void ssdp_running_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting ssdp_running_task");

    //todo: implement them
    (void) SSDP_PACKET_TEMPLATE;
    (void) SSDP_NOTIFY_TEMPLATE;
    (void) SSDP_RESPONSE_TEMPLATE;
    while (1) {
        int sock;

        sock = create_multicast_ipv4_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv4 multicast socket");
        }


        if (sock < 0) {
            // Nothing to do!
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        // set destination multicast addresses for sending from these sockets
        struct sockaddr_in sdestv4 = {
            .sin_family = PF_INET,
            .sin_port = htons(SSDP_PORT),
        };
        // We know this inet_aton will pass because we did it above already
        inet_aton(SSDP_MULTICAST_ADDR, &sdestv4.sin_addr.s_addr);

        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        int err = 1;
        while (err > 0) {
            struct timeval tv = {
                .tv_sec = 2,
                .tv_usec = 0,
            };
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);

            int s = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (s < 0) {
                ESP_LOGE(TAG, "Select failed: errno %d", errno);
                err = -1;
                break;
            } else if (s > 0) {
                if (FD_ISSET(sock, &rfds)) {
                    // Incoming datagram received
                    char recvbuf[48];
                    char raddr_name[32] = { 0 };

                    struct sockaddr_storage raddr; // Large enough for both IPv4
                    socklen_t socklen = sizeof(raddr);
                    int len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0,
                                       (struct sockaddr *)&raddr, &socklen);
                    if (len < 0) {
                        ESP_LOGE(TAG, "multicast recvfrom failed: errno %d", errno);
                        err = -1;
                        break;
                    }

                    // Get the sender's address as a string

                    if (raddr.ss_family == PF_INET) {
                        inet_ntoa_r(((struct sockaddr_in *)&raddr)->sin_addr,
                                    raddr_name, sizeof(raddr_name)-1);
                    }

                    ESP_LOGI(TAG, "received %d bytes from %s:", len, raddr_name);

                    recvbuf[len] = 0; // Null-terminate whatever we received and treat like a string...
                    ESP_LOGI(TAG, "%s", recvbuf);
                }
            } else { // s == 0
                // Timeout passed with no incoming data, so send something!
                static int send_count;
                const char sendfmt[] = "Multicast #%d sent by ESP32\n";
                char sendbuf[48];
                char addrbuf[32] = { 0 };
                int len = snprintf(sendbuf, sizeof(sendbuf), sendfmt, send_count++);
                if (len > sizeof(sendbuf)) {
                    ESP_LOGE(TAG, "Overflowed multicast sendfmt buffer!!");
                    send_count = 0;
                    err = -1;
                    break;
                }

                struct addrinfo hints = {
                    .ai_flags = AI_PASSIVE,
                    .ai_socktype = SOCK_DGRAM,
                };
                struct addrinfo *res;

                hints.ai_family = AF_INET; // For an IPv4 socket

                int err = getaddrinfo(SSDP_MULTICAST_ADDR,
                                      NULL,
                                      &hints,
                                      &res);
                if (err < 0) {
                    ESP_LOGE(TAG, "getaddrinfo() failed for IPV4 destination address. error: %d", err);
                    break;
                }
                if (res == 0) {
                    ESP_LOGE(TAG, "getaddrinfo() did not return any addresses");
                    break;
                }
                ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(SSDP_PORT);
                inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, addrbuf, sizeof(addrbuf)-1);
                ESP_LOGI(TAG, "Sending to IPV4 multicast address %s:%d...",  addrbuf, SSDP_PORT);

                err = sendto(sock, sendbuf, len, 0, res->ai_addr, res->ai_addrlen);
                freeaddrinfo(res);
                if (err < 0) {
                    ESP_LOGE(TAG, "IPV4 sendto failed. errno: %d", errno);
                    break;
                }

            }
        }

        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }

}

void ssdp_set_UUID(char **uuid, const char * root_uid)
{
    uint8_t mac[6];
    esp_err_t err  = esp_efuse_mac_get_default((uint8_t *)&mac);
    if (ESP_OK != err) {
        memset(mac, 0, 6);
        ESP_LOGW(TAG, "Not able to read MAC address, use 000000");
    }

    sprintf(*uuid, "%s%02x%02x%02x",
            root_uid,
            mac[2],
            mac[1],
            mac[0] );

}

/*
* Global Functions
*/
esp_err_t ssdp_start(ssdp_config_t* configuration)
{
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
    //Nothing is configured use default root and mac
    if ((!configuration->uuid_root || strlen(configuration->uuid_root)==0) && (!configuration->uuid || strlen(configuration->uuid)==0)) {
        ssdp_set_UUID(&ssdp_task_config->config->uuid,SSDP_UUID_ROOT);
    } else {
        //if no full UID is configured but has root
        if ((!configuration->uuid || strlen(configuration->uuid)==0)) {
            if (strlen(configuration->uuid_root)==strlen(SSDP_UUID_ROOT)) {
                ssdp_set_UUID(&ssdp_task_config->config->uuid,configuration->uuid_root);
            } else {
                ESP_LOGE(TAG, "Wrong size of uuid root parameter");
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            if (strlen(configuration->uuid)==SSDP_UUID_SIZE) {
                strcpy(ssdp_task_config->config->uuid, configuration->uuid);
            } else {
                ESP_LOGE(TAG, "Invalid uuid parameter");
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    //Device type
    if (configuration->device_type) {
        if (strlen(configuration->device_type)>SSDP_DEVICE_TYPE_SIZE) {
            ESP_LOGE(TAG, "Device type too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->device_type = (char *) malloc(strlen(configuration->device_type)+1);
        if (!ssdp_task_config->config->device_type ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->device_type, configuration->device_type);
    }


    //Friendly name
    if(configuration->friendly_name) {
        if (strlen(configuration->friendly_name)>SSDP_FRIENDLY_NAME_SIZE) {
            ESP_LOGE(TAG, "Friendly name too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->friendly_name = (char *) malloc(strlen(configuration->friendly_name)+1);
        if (!ssdp_task_config->config->friendly_name ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
    }
    strcpy(ssdp_task_config->config->friendly_name, configuration->friendly_name);

    //Serial Number
    if(configuration->serial_number) {
        if (strlen(configuration->serial_number)>SSDP_SERIAL_NUMBER_SIZE) {
            ESP_LOGE(TAG, "Serial number too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->serial_number = (char *) malloc(strlen(configuration->serial_number)+1);
        if (!ssdp_task_config->config->serial_number ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->serial_number, configuration->serial_number);
    }

    //Presentation url
    if(configuration->presentation_url) {
        if (strlen(configuration->presentation_url)>SSDP_PRESENTATION_URL_SIZE) {
            ESP_LOGE(TAG, "Presentation url too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->presentation_url = (char *) malloc(strlen(configuration->presentation_url)+1);
        if (!ssdp_task_config->config->presentation_url ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->presentation_url, configuration->presentation_url);
    }

    //Manufacturer name
    if(configuration->manufacturer_name) {
        if (strlen(configuration->manufacturer_name)>SSDP_MANUFACTURER_NAME_SIZE) {
            ESP_LOGE(TAG, "Manufacturer name too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->manufacturer_name = (char *) malloc(strlen(configuration->manufacturer_name)+1);
        if (!ssdp_task_config->config->manufacturer_name ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->manufacturer_name, configuration->manufacturer_name);
    }

    //Manufacturer url
    if(configuration->manufacturer_url) {
        if (strlen(configuration->manufacturer_url)>SSDP_MANUFACTURER_URL_SIZE) {
            ESP_LOGE(TAG, "Manufacturer url too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->manufacturer_url = (char *) malloc(strlen(configuration->manufacturer_url)+1);
        if (!ssdp_task_config->config->manufacturer_url ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->manufacturer_url, configuration->manufacturer_url);
    }

    //Model name
    if(configuration->model_name) {
        if (strlen(configuration->model_name)>SSDP_MODEL_NAME_SIZE) {
            ESP_LOGE(TAG, "Model name too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->model_name = (char *) malloc(strlen(configuration->model_name)+1);
        if (!ssdp_task_config->config->model_name ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->model_name, configuration->model_name);
    }

    //Model url
    if(configuration->model_url) {
        if (strlen(configuration->model_url)>SSDP_MODEL_URL_SIZE) {
            ESP_LOGE(TAG, "Model url too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->model_url = (char *) malloc(strlen(configuration->model_url)+1);
        if (!ssdp_task_config->config->model_url ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->model_url, configuration->model_url);
    }

    //Model number
    if (configuration->model_number) {
        if (strlen(configuration->model_number)>SSDP_MODEL_NUMBER_SIZE) {
            ESP_LOGE(TAG, "Model number too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->model_number = (char *) malloc(strlen(configuration->model_number)+1);
        if (!ssdp_task_config->config->model_number ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->model_number, configuration->model_number);
    }

    //Model description
    if (configuration->model_description) {
        if (strlen(configuration->model_description)>SSDP_MODEL_DESCRIPTION_SIZE) {
            ESP_LOGE(TAG, "Model description too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->model_description = (char *) malloc(strlen(configuration->model_description)+1);
        if (!ssdp_task_config->config->model_description ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->model_description, configuration->model_description);
    }
    //Server name
    if (configuration->server_name) {
        if (strlen(configuration->server_name)>SSDP_SERVER_NAME_SIZE) {
            ESP_LOGE(TAG, "Server name too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->server_name = (char *) malloc(strlen(configuration->server_name)+1);
        if (!ssdp_task_config->config->server_name ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->server_name, configuration->server_name);
    }
    //Services description
    if(configuration->services_description) {
        if (strlen(configuration->services_description)>SSDP_SERVICES_DESCRIPTION_SIZE) {
            ESP_LOGE(TAG, "Services description too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->services_description = (char *) malloc(strlen(configuration->services_description)+1);
        if (!ssdp_task_config->config->services_description ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->services_description, configuration->services_description);
    }

    //Icons description
    if(configuration->icons_description) {
        if (strlen(configuration->icons_description)>SSDP_ICONS_DESCRIPTION_SIZE) {
            ESP_LOGE(TAG, "Icons description too long");
            return ESP_ERR_INVALID_ARG;
        }
        ssdp_task_config->config->icons_description = (char *) malloc(strlen(configuration->icons_description)+1);
        if (!ssdp_task_config->config->icons_description ) {
            ESP_LOGE(TAG, "No enough memory for ssdp user task configuration");
            return ESP_ERR_NO_MEM;
        }
        strcpy(ssdp_task_config->config->icons_description, configuration->icons_description);
    }

    ESP_LOGI(TAG, "Task creation core %d, stack:  %d, priotity %d",configuration->core_id,configuration->stack_size,configuration->task_priority );

    //Task creation
    BaseType_t  res =  xTaskCreatePinnedToCore(ssdp_running_task, "ssdp_running_task", configuration->stack_size, NULL, configuration->task_priority, &ssdp_task_config->xHandle, configuration->core_id);
    if (!(res==pdPASS && ssdp_task_config->xHandle)) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ssdp_stop()
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
            free(ssdp_task_config->config);
        }
        free(ssdp_task_config->replySlots);
        free(ssdp_task_config->schema);
        free(ssdp_task_config);
        ssdp_task_config = NULL;
    }
    return ESP_OK;
}

const char* get_ssdp_schema_str()
{
    if (!ssdp_task_config) {
        ESP_LOGE(TAG, "SSDP not started");
        return NULL;
    }
    if (ssdp_task_config->schema) {
        free(ssdp_task_config->schema);
        ssdp_task_config->schema = NULL;
    }

    size_t template_size = sizeof(SSDP_SCHEMA_TEMPLATE)
                           + 15 //IP
                           + 5  //port
                           + (ssdp_task_config->config->device_type?strlen(ssdp_task_config->config->device_type):1)
                           + (ssdp_task_config->config->friendly_name?strlen(ssdp_task_config->config->friendly_name):1)
                           + (ssdp_task_config->config->presentation_url?strlen(ssdp_task_config->config->presentation_url):1)
                           + (ssdp_task_config->config->serial_number?strlen(ssdp_task_config->config->serial_number):1)
                           + (ssdp_task_config->config->model_name?strlen(ssdp_task_config->config->model_name):1)
                           + (ssdp_task_config->config->model_description?strlen(ssdp_task_config->config->model_description):1)
                           + (ssdp_task_config->config->model_number?strlen(ssdp_task_config->config->model_number):1)
                           + (ssdp_task_config->config->model_url?strlen(ssdp_task_config->config->model_url):1)
                           + (ssdp_task_config->config->manufacturer_name?strlen(ssdp_task_config->config->manufacturer_name):1)
                           + (ssdp_task_config->config->manufacturer_url?strlen(ssdp_task_config->config->manufacturer_url):1)
                           + (ssdp_task_config->config->uuid?strlen(ssdp_task_config->config->uuid):1)
                           + (ssdp_task_config->config->services_description?strlen(ssdp_task_config->config->services_description):1)
                           + (ssdp_task_config->config->icons_description?strlen(ssdp_task_config->config->icons_description):1) ;

    ssdp_task_config->schema = (char *) malloc(template_size+1);
    if (ssdp_task_config->schema) {
        if ( sprintf(ssdp_task_config->schema,SSDP_SCHEMA_TEMPLATE,ssdp_get_LocalIP(),
                     ssdp_task_config->config->port,
                     ssdp_task_config->config->device_type?ssdp_task_config->config->device_type:"",
                     ssdp_task_config->config->friendly_name?ssdp_task_config->config->friendly_name:"",
                     ssdp_task_config->config->presentation_url?ssdp_task_config->config->presentation_url:"",
                     ssdp_task_config->config->serial_number?ssdp_task_config->config->serial_number:"",
                     ssdp_task_config->config->model_name?ssdp_task_config->config->model_name:"",
                     ssdp_task_config->config->model_description?ssdp_task_config->config->model_description:"",
                     ssdp_task_config->config->model_number?ssdp_task_config->config->model_number:"",
                     ssdp_task_config->config->model_url?ssdp_task_config->config->model_url:"",
                     ssdp_task_config->config->manufacturer_name?ssdp_task_config->config->manufacturer_name:"",
                     ssdp_task_config->config->manufacturer_url?ssdp_task_config->config->manufacturer_url:"",
                     ssdp_task_config->config->uuid?ssdp_task_config->config->uuid:"",
                     ssdp_task_config->config->services_description?ssdp_task_config->config->services_description:"",
                     ssdp_task_config->config->icons_description?ssdp_task_config->config->icons_description:"")<0) {
            ESP_LOGE(TAG, "sprintf error for schema");
            free(ssdp_task_config->schema);
            ssdp_task_config->schema = NULL;
        }
    } else {
        ESP_LOGE(TAG, "Memory allocation error for schema");
    }
    return ssdp_task_config->schema;
}