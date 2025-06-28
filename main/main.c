/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "bt_app_av.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "web_control.h"

#define RELAY_GPIO 18
#define ENCODER_SW_GPIO 19 
#define CLICK_TIMEOUT_MS 400

bool party_mode = false;
bool is_playing = false;
bool system_on = false;


static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
enum
{
    BT_APP_EVT_STACK_UP = 0,
};

static void bt_app_dev_cb(esp_bt_dev_cb_event_t event, esp_bt_dev_cb_param_t *param);
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
void mute_audio_output();

static char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18)
    {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void bt_app_dev_cb(esp_bt_dev_cb_event_t event, esp_bt_dev_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_DEV_NAME_RES_EVT:
    {
        if (param->name_res.status == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(BT_AV_TAG, "Get local device name success: %s", param->name_res.name);
        }
        else
        {
            ESP_LOGE(BT_AV_TAG, "Get local device name failed, status: %d", param->name_res.status);
        }
        break;
    }
    default:
    {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    uint8_t *bda = NULL;

    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        }
        else
        {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        ESP_LOGI(BT_AV_TAG, "link key type of current link is: %d", param->auth_cmpl.lk_type);
        break;
    }
    case ESP_BT_GAP_ENC_CHG_EVT:
    {
        char *str_enc[3] = {"OFF", "E0", "AES"};
        bda = (uint8_t *)param->enc_chg.bda;
        ESP_LOGI(BT_AV_TAG, "Encryption mode to [%02x:%02x:%02x:%02x:%02x:%02x] changed to %s",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], str_enc[param->enc_chg.enc_mode]);
        break;
    }

#if (CONFIG_EXAMPLE_A2DP_SINK_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d, interval: %.2f ms",
                 param->mode_chg.mode, param->mode_chg.interval * 0.625);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_conn_cmpl_stat.bda;
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT Connected to [%02x:%02x:%02x:%02x:%02x:%02x], status: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_conn_cmpl_stat.stat);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_disconn_cmpl_stat.bda;
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_ACL_DISC_CMPL_STAT_EVT Disconnected from [%02x:%02x:%02x:%02x:%02x:%02x], reason: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_disconn_cmpl_stat.reason);
        break;
    default:
    {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);

    switch (event)
    {
    case BT_APP_EVT_STACK_UP:
    {
        esp_bt_gap_set_device_name(local_device_name);
        esp_bt_dev_register_callback(bt_app_dev_cb);
        esp_bt_gap_register_callback(bt_app_gap_cb);

        if (esp_avrc_ct_init() != ESP_OK)
        {
            ESP_LOGW(BT_AV_TAG, "AVRC CT already initialized");
        }
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
        if (esp_avrc_tg_init() != ESP_OK)
        {
            ESP_LOGW(BT_AV_TAG, "AVRC TG already initialized");
        }
        esp_avrc_tg_register_callback(bt_app_rc_tg_cb);

        esp_avrc_rn_evt_cap_mask_t evt_set = {0};
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        esp_avrc_tg_set_rn_evt_cap(&evt_set);

        esp_err_t err = esp_a2d_sink_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(BT_AV_TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(err));
            break;
        }
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

        esp_a2d_sink_get_delay_value();
        esp_bt_gap_get_device_name();

        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

 void system_start(void)
{
    gpio_set_level(RELAY_GPIO, 1);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    esp_bluedroid_enable();

    bt_app_task_start_up();
    // مقدار event را صحیح ارسال کن
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);
    ESP_LOGI("SYSTEM", "System turned ON");

    system_on = true;
}

 void system_stop(void)
{
    gpio_set_level(RELAY_GPIO, 0);

    mute_audio_output();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err;
    err = esp_a2d_sink_deinit();
    ESP_LOGI("SYSTEM", "esp_a2d_sink_deinit: %s", esp_err_to_name(err));
    err = esp_avrc_ct_deinit();
    ESP_LOGI("SYSTEM", "esp_avrc_ct_deinit: %s", esp_err_to_name(err));
    err = esp_avrc_tg_deinit();
    ESP_LOGI("SYSTEM", "esp_avrc_tg_deinit: %s", esp_err_to_name(err));

    bt_app_task_shut_down();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    mute_audio_output();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI("SYSTEM", "System turned OFF");

    system_on = false;
}

void encoder_task(void *arg)
{
    int last_state = 1;
    int click_count = 0;
    int64_t last_click_time = 0;
    int64_t press_start = 0;
    bool pressed = false;

    while (1)
    {
        int state = gpio_get_level(ENCODER_SW_GPIO);

        // لبه پایین‌رو: شروع کلیک یا نگه داشتن
        if (last_state == 1 && state == 0)
        {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_click_time > CLICK_TIMEOUT_MS)
            {
                click_count = 0;
            }
            click_count++;
            last_click_time = now;
            press_start = now;
            pressed = true;
        }

        // لبه بالا‌رو: پایان کلیک یا نگه داشتن
        if (last_state == 0 && state == 1)
        {
            pressed = false;
        }

        // نگه داشتن دکمه
        if (pressed)
        {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - press_start > 3000)
            {
                if (!system_on)
                {
                    system_start();
                }
                else
                {
                    system_stop();
                }
                // منتظر رها شدن دکمه
                while (gpio_get_level(ENCODER_SW_GPIO) == 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                pressed = false;
                click_count = 0;
            }
        }

        // تشخیص کلیک‌های کوتاه
        if (click_count > 0)
        {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_click_time > CLICK_TIMEOUT_MS && !pressed)
            {
                if (system_on)
                {
                    if (click_count == 1)
                    {
                        // Play/Pause
                        if (is_playing)
                        {
                            // اگر در حال پخش است، Pause کن
                            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
                            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
                            is_playing = false; // وضعیت local را هم تغییر بده
                        }
                        else
                        {
                            // اگر پخش نیست، Play کن
                            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
                            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
                            is_playing = true; // وضعیت local را هم تغییر بده
                        }
                    }
                    else if (click_count == 2)
                    {
                        // Next Track
                        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
                        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
                    }
                    else if (click_count == 3)
                    {
                        // Previous Track
                        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
                        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
                    }
                    else if (click_count == 4)
                    {
                        party_mode = !party_mode;
                        gpio_set_level(PARTY_MODE_LED_GPIO, (party_mode ? 1 : 0));
                    }
                }
                click_count = 0;
            }
        }

        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    // مقداردهی اولیه پایه رله (قبل از هر چیز)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);
    gpio_set_level(RELAY_GPIO, 0);    // رله خاموش در ابتدا

    // مقداردهی اولیه پایه LED پارتی‌مد
    gpio_set_direction(PARTY_MODE_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PARTY_MODE_LED_GPIO, 0);

    // مقداردهی اولیه پایه دکمه (در صورت نیاز)
    gpio_set_direction(ENCODER_SW_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(ENCODER_SW_GPIO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    wifi_init_softap();
    start_webserver();

    xTaskCreate(encoder_task, "encoder_task", 4096, NULL, 5, NULL);

    // ... سایر کدهای راه‌اندازی (در صورت نیاز) ...
}
