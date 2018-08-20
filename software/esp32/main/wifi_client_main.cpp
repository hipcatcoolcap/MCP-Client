/* HTTPS GET Example using plain mbedTLS sockets
 *
 * Contacts the howsmyssl.com API via TLS v1.2 and reads a JSON
 * response.
 *
 * Adapted from the ssl_client1 example in mbedtls.
 *
 * Original Copyright (C) 2006-2016, ARM Limited, All Rights Reserved, Apache 2.0 License.
 * Additions Copyright (C) Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache 2.0 License.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include <jsmn.h>

// #include "utils.h"
#include "mcp_api.h"
#include <reader.h>
#include <light.h>

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

#define SERVER CONFIG_SERVER
#define PORT CONFIG_PORT


/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/* Constants that aren't configurable in menuconfig */
// #define WEB_SERVER "security.makeict.org"
// #define WEB_SERVER "securitytest.makeict.org:4443"
// #define WEB_URL "https://securitytest.makeict.org:4443"

// #define WEB_SERVER CONFIG_SERVER ":" CONFIG_PORT
// #define WEB_URL "https://" WEB_SERVER
// #define AUTH_ENDPOINT WEB_URL "/api/login"

static const char *TAG = "wifi_client_main";

extern "C" {
  void app_main();
}

Reader card_reader;
Light red_light((gpio_num_t)19);
Light yellow_light((gpio_num_t)18);
Light green_light((gpio_num_t)17);

// static const char *REQUEST = "POST " AUTH_ENDPOINT "?email=" CONFIG_USERNAME    "&password=" CONFIG_PASSWORD "\r\n"
//     "Host: " WEB_SERVER "\r\n"
//     "Content-Type: application/x-www-form-urlencoded\r\n"
//     "\r\n";


/* Letsencrypt root cert, taken from server_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
// extern const uint8_t server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
// extern const uint8_t server_root_cert_pem_end[]   asm("_binary_server_root_cert_pem_end");

void init(void)
{
  card_reader.init();
}
    
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {};
    strcpy((char*) wifi_config.sta.ssid, (char*) CONFIG_WIFI_SSID);
    strcpy((char*) wifi_config.sta.password, (char*) CONFIG_WIFI_PASSWORD);

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

bool check_card(char* nfc_id) {
  int resp_len = get_user_by_NFC(nfc_id);
  if (resp_len < 2) {
    post_log("04+Security+Desk+-+Could+Not+Find+User","", nfc_id,"deny");
    return false;
  }
  char response[resp_len+1] = {'\0'};
  get_response(response, resp_len);

  jsmn_parser parser;
  jsmn_init(&parser);

  // char value[500];
  // char key[500];
  int r;

  int num_t = jsmn_parse(&parser, response, strlen(response), NULL, 0);
  printf("num tokens: %d\n", num_t);

  if (num_t < 0) {
    post_log("04+Security+Desk+-+Could+Not+Find+User","", nfc_id,"deny");
    return false;
  }

    jsmn_init(&parser);
  jsmntok_t t[num_t];
  r = jsmn_parse(&parser, response, strlen(response), t, num_t);

  if (r < 0) {
      printf("Failed to parse JSON: %d\n", r);
      return false;
  }

  /* Assume the top-level element is an object */
  if (r < 1) {
      printf("Object expected\n");
      return false;
  }

  // char firstName[30] = {'\0'};
  // char lastName[30] = {'\0'};
  char userID[5] = {'\0'};

 for (int i = 2; i < r; i++){

   jsmntok_t json_value = t[i+1];
   jsmntok_t json_key = t[i];


   int string_length = json_value.end - json_value.start;
   int key_length = json_key.end - json_key.start;

   char value[string_length];
   char key[key_length];


   int idx;

   for (idx = 0; idx < string_length; idx++){
       value[idx] = response[json_value.start + idx ];
   }

   for (idx = 0; idx < key_length; idx++){
       key[idx] = response[json_key.start + idx];
   }

   value[string_length] = '\0';
   key[key_length] = '\0';

   if(strcmp(key, "userID")==0) {
    strcpy(userID, value);
   }
  
  printf("%s : %s\n", key, value);
   i++;
 }

  if(atoi(userID) > 0) {
    post_log("04+Security+Desk",userID, nfc_id,"unlock");
    return true;
  }
  else {
    post_log("04+Security+Desk+-+Could+Not+Find+User","", nfc_id,"deny");
    return false;
  }
  return false;

}
    
void app_main()
{
    init();
    client_init();  
    ESP_ERROR_CHECK( nvs_flash_init() );
    initialise_wifi();
    
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                    false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to AP");
    authenticate_with_contact_credentials();
    xTaskCreate(&keepalive_task, "keepalive_task", 8192, NULL, 5, NULL);
    // get_user_by_NFC("04dbe822993c80");
      // execute_request("test", "test", "test");
    red_light.on();
    while(1) {
      uint8_t uid[7] = {0};
      uint8_t uid_size = card_reader.poll(uid);
      if (uid_size > 0) {
        char uid_string[15] = {'\0'};
        sprintf(uid_string, "%02X%02X%02X%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
        ESP_LOGI(TAG, "Read card UID: %s", uid_string);
        red_light.off();
        green_light.off();
        yellow_light.on();
        if (check_card(uid_string)) {
          yellow_light.off();
          green_light.on();
          vTaskDelay(2000 / portTICK_PERIOD_MS);

        }
        else {
          yellow_light.off();
          red_light.on();
          vTaskDelay(300 / portTICK_PERIOD_MS);
          red_light.off();
          vTaskDelay(200 / portTICK_PERIOD_MS);
          red_light.on();
          vTaskDelay(300 / portTICK_PERIOD_MS);
          red_light.off();
          vTaskDelay(200 / portTICK_PERIOD_MS);
          red_light.on();
          vTaskDelay(300 / portTICK_PERIOD_MS);
          red_light.off();
          vTaskDelay(200 / portTICK_PERIOD_MS);

        }

        // post_log("04+Security+Desk+-+Could+Not+Find+User","", uid_string,"deny");
        yellow_light.off();        

      //   for(int i=0; i< uid_size; i++) {
      //   printf("%d,", uid[i]);
      //   }
      //   printf("\n");
      }
      yellow_light.off();
      green_light.off();
      red_light.on();
    }

    // xTaskCreate(&https_get_task, "https_get_task", 8192, NULL, 5, NULL);
}

