#include "wifi_prov.h"

#include <string.h>

#include <esp_wifi.h>

#include "wifi_manager.h"

/* 原版工程（sdgoods-radio/main/wifi_prov.c）在 GOT_IP 时会调 esp_wifi_set_ps(WIFI_PS_NONE)：
 * 默认的 WIFI_PS_MIN_MODEM 让射频在 DTIM 信标之间休眠，下行要等到下一个信标窗口，
 * 电台这种持续小码率流每一段都要多等 100~300ms，表现就是「每台都要缓冲很久」，
 * 播起来还会周期性卡一下。移植时这段丢了，这里补回来。
 * 只在电台页轮询（radio_tick 120ms），连接后把省电模式钉成 NONE；
 * 应用层其它状态切换若又把它改回去，这里下一次轮询会再纠正一次。 */
static void ensure_wifi_no_power_save(void) {
    wifi_ps_type_t ps;
    if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

extern "C" esp_err_t wifi_prov_start(void) {
    return ESP_OK;
}

extern "C" bool wifi_prov_erase_credentials(void) {
    return false;
}

extern "C" wifi_prov_state_t wifi_prov_get_state(void) {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        return WIFI_STATE_CONNECTING;
    }
    if (wifi.IsConnected()) {
        ensure_wifi_no_power_save();
        return WIFI_STATE_CONNECTED;
    }
    if (wifi.IsConfigMode()) {
        return WIFI_STATE_SETUP;
    }
    return WIFI_STATE_CONNECTING;
}

extern "C" const char* wifi_prov_get_ip(void) {
    return "";
}

extern "C" bool wifi_prov_is_setup_mode(void) {
    return false;
}

extern "C" void wifi_prov_get_ap_ssid(char* out, size_t outlen) {
    if (out && outlen) out[0] = '\0';
}

extern "C" const char* wifi_prov_get_gateway(void) {
    return "";
}

extern "C" int wifi_prov_scan(char ssids[][33], int8_t rssi[], int max) {
    (void)ssids;
    (void)rssi;
    (void)max;
    return 0;
}

extern "C" esp_err_t wifi_prov_submit(const char* ssid, const char* pass) {
    (void)ssid;
    (void)pass;
    return ESP_ERR_NOT_SUPPORTED;
}
