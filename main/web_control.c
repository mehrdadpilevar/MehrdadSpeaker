#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

extern void system_start(void);
extern void system_stop(void);
extern bool system_on;
extern bool party_mode;

#define WIFI_SSID      "Mehrdad Speaker"
#define WIFI_PASS      "123456789"
#define WIFI_CHANNEL   1
#define MAX_STA_CONN   4
#define PARTY_MODE_LED_GPIO 2

static const char *TAG = "WEB_CTRL";

// --- WiFi AP & Static IP ---
void wifi_init_softap(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .channel = WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

    // Set static IP
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton("1.2.3.4");
    ip_info.gw.addr = esp_ip4addr_aton("1.2.3.4");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi AP started. SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}

// --- HTML Panel ---
static const char *panel_html =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>Mehrdad Speaker</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"</head><body style='background:#fafafa;font-family:sans-serif;text-align:center;padding-top:40px;'>"
"<h2>Mehrdad Speaker Control</h2>"
"<form method='POST'>"
"<button name='power' value='on' style='font-size:1.1em;padding:10px 30px;margin:10px;'>روشن</button>"
"<button name='power' value='off' style='font-size:1.1em;padding:10px 30px;margin:10px;'>خاموش</button><br><br>"
"<button name='mode' value='party' style='font-size:1em;padding:8px 22px;margin:8px;'>پارتی مد</button>"
"<button name='mode' value='home' style='font-size:1em;padding:8px 22px;margin:8px;'>خونه مد</button>"
"</form>"
"<p style='margin-top:24px;'>وضعیت اسپیکر: <b>%s</b> | حالت: <b>%s</b></p>"
"</body></html>";

// --- HTTP Handlers ---
esp_err_t panel_get_handler(httpd_req_t *req)
{
    char resp[1024];
    snprintf(resp, sizeof(resp), panel_html,
        system_on ? "روشن" : "خاموش",
        party_mode ? "پارتی" : "خونه"
    );
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t panel_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    buf[ret] = 0;

    if (strstr(buf, "power=on")) {
        if (!system_on) {
            ESP_LOGI(TAG, "Calling system_start from web panel");
            system_start();
        }
    }
    if (strstr(buf, "power=off")) {
        if (system_on) {
            ESP_LOGI(TAG, "Calling system_stop from web panel");
            system_stop();
        }
    }
    if (strstr(buf, "mode=party")) {
        party_mode = true;
        gpio_set_level(PARTY_MODE_LED_GPIO, 1);
    }
    if (strstr(buf, "mode=home")) {
        party_mode = false;
        gpio_set_level(PARTY_MODE_LED_GPIO, 0);
    }

    return panel_get_handler(req);
}

// --- Web Server ---
void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    httpd_start(&server, &config);

    httpd_uri_t panel = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = panel_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &panel);

    httpd_uri_t panel_post = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = panel_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &panel_post);
}

// // --- Main Entry ---
// void app_main(void)
// {
//     nvs_flash_init();
//     wifi_init_softap();
//     start_webserver();
//     // بقیه کدهای راه‌اندازی اسپیکر و بلوتوث
// }