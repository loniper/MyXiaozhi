// WiFi status shim: the radio app reads network state from the global
// xiaozhi WiFi manager instead of its own provisioning module.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STATE_SETUP = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_prov_state_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_prov_start(void);
bool wifi_prov_erase_credentials(void);
wifi_prov_state_t wifi_prov_get_state(void);
const char *wifi_prov_get_ip(void);
bool wifi_prov_is_setup_mode(void);
void wifi_prov_get_ap_ssid(char *out, size_t outlen);
const char *wifi_prov_get_gateway(void);
int  wifi_prov_scan(char ssids[][33], int8_t rssi[], int max);
esp_err_t wifi_prov_submit(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
