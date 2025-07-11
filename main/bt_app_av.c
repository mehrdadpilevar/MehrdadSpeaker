/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"

#include "bt_app_core.h"
#include "bt_app_av.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_EXAMPLE_A2DP_SINK_OUTPUT_INTERNAL_DAC
#include "driver/dac_continuous.h"
#else
#include "driver/i2s_std.h"
#endif
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sys/lock.h"
#define MAX_AUDIO_BUF 8192 // حداکثر اندازه بافر صوتی (بسته به پروژه قابل تغییر است)
#define IIR_ALPHA 0.04f    // ضریب فیلتر پایین‌گذر (120Hz برای 44100Hz)

// بافر استاتیک برای جلوگیری از malloc/free
static int16_t audio_mid[MAX_AUDIO_BUF / 2];
static int16_t audio_bass[MAX_AUDIO_BUF / 2];

// متغیر فیلتر پایین‌گذر
static float lp_y = 0;

// ولوم جداگانه برای هر خروجی (۰ تا ۱)
static float volume_bass = 0.2f;
static float volume_mid = 0.2f;
extern bool party_mode;

/*******************************
 * STATIC FUNCTION DECLARATIONS
 ******************************/

/* allocate new meta buffer */
static void bt_app_alloc_meta_buffer(esp_avrc_ct_cb_param_t *param);
/* handler for new track is loaded */
static void bt_av_new_track(void);
/* handler for track status change */
static void bt_av_playback_changed(void);
/* handler for track playing position change */
static void bt_av_play_pos_changed(void);
/* notification event handler */
static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter);
/* installation for i2s */
static void bt_i2s_driver_install(void);
/* uninstallation for i2s */
static void bt_i2s_driver_uninstall(void);
/* mute i2s*/
void mute_audio_output();
/* set volume by remote controller */
static void volume_set_by_controller(uint8_t volume);
/* set volume by local host */
static void volume_set_by_local_host(uint8_t volume);
/* a2dp event handler */
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param);
/* avrc controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);
/* avrc target event handler */
static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param);

static void encoder_poll_task(void *arg);
static void encoder_button_task(void *arg);

/*******************************
 * STATIC VARIABLE DEFINITIONS
 ******************************/

static uint32_t s_pkt_cnt = 0; /* count for audio packet */
static esp_a2d_audio_state_t s_audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
/* audio stream datapath state */
static const char *s_a2d_conn_state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
/* connection state in string */
static const char *s_a2d_audio_state_str[] = {"Suspended", "Started"};
/* audio stream datapath state in string */
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
/* AVRC target notification capability bit mask */
static _lock_t s_volume_lock;
static TaskHandle_t s_encoder_task_hdl = NULL;
static uint8_t s_volume = 100; /* local volume value */
static bool s_volume_notify;    /* notify volume change or not */
#ifndef CONFIG_EXAMPLE_A2DP_SINK_OUTPUT_INTERNAL_DAC
// i2s_chan_handle_t tx_chan = NULL;
i2s_chan_handle_t tx_chan_mid = NULL;
i2s_chan_handle_t tx_chan_bass = NULL;
#else
dac_continuous_handle_t tx_chan;
#endif

#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
static bool cover_art_connected = false;
static bool cover_art_getting = false;
static uint32_t cover_art_image_size = 0;
static uint8_t image_handle_old[7];
#endif

/********************************
 * STATIC FUNCTION DEFINITIONS
 *******************************/

static void bt_app_alloc_meta_buffer(esp_avrc_ct_cb_param_t *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(param);
    uint8_t *attr_text = (uint8_t *)malloc(rc->meta_rsp.attr_length + 1);

    memcpy(attr_text, rc->meta_rsp.attr_text, rc->meta_rsp.attr_length);
    attr_text[rc->meta_rsp.attr_length] = 0;
    rc->meta_rsp.attr_text = attr_text;
}

#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
static bool image_handle_check(uint8_t *image_handle, int len)
{
    /* Image handle length must be 7 */
    if (len == 7 && memcmp(image_handle_old, image_handle, 7) != 0)
    {
        memcpy(image_handle_old, image_handle, 7);
        return true;
    }
    return false;
}
#endif

static void bt_av_new_track(void)
{
    /* request metadata */
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE |
                        ESP_AVRC_MD_ATTR_ARTIST |
                        ESP_AVRC_MD_ATTR_ALBUM |
                        ESP_AVRC_MD_ATTR_GENRE;
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
    if (cover_art_connected)
    {
        attr_mask |= ESP_AVRC_MD_ATTR_COVER_ART;
    }
#endif
    esp_avrc_ct_send_metadata_cmd(APP_RC_CT_TL_GET_META_DATA, attr_mask);

    /* register notification if peer support the event_id */
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_TRACK_CHANGE))
    {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_TRACK_CHANGE,
                                                   ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

static void bt_av_playback_changed(void)
{
    /* register notification if peer support the event_id */
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_PLAY_STATUS_CHANGE))
    {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_PLAYBACK_CHANGE,
                                                   ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
}

static void bt_av_play_pos_changed(void)
{
    /* register notification if peer support the event_id */
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_PLAY_POS_CHANGED))
    {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_PLAY_POS_CHANGE,
                                                   ESP_AVRC_RN_PLAY_POS_CHANGED, 10);
    }
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id)
    {
    case ESP_AVRC_RN_TRACK_CHANGE:
        bt_av_new_track();
        break;
    case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
        ESP_LOGI(BT_AV_TAG, "Playback status changed: 0x%x", event_parameter->playback);
        is_playing = (event_parameter->playback == ESP_AVRC_PLAYBACK_PLAYING);
        bt_av_playback_changed();
        break;
    case ESP_AVRC_RN_PLAY_POS_CHANGED:
        bt_av_play_pos_changed();
        break;
    default:
        ESP_LOGI(BT_AV_TAG, "unhandled event: %d", event_id);
        break;
    }
}

void bt_i2s_driver_install(void)
{
    i2s_chan_config_t chan_cfg_mid = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_config_t chan_cfg_bass = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    i2s_std_config_t std_cfg_mid = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_MIDRANGE_I2S_BCK_PIN,
            .ws = CONFIG_MIDRANGE_I2S_LRCK_PIN,
            .dout = CONFIG_MIDRANGE_I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_std_config_t std_cfg_bass = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BASS_I2S_BCK_PIN,
            .ws = CONFIG_BASS_I2S_LRCK_PIN,
            .dout = CONFIG_BASS_I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // Initialize I2S channels
    esp_err_t ret_mid = i2s_new_channel(&chan_cfg_mid, &tx_chan_mid, NULL);
    esp_err_t ret_bass = i2s_new_channel(&chan_cfg_bass, &tx_chan_bass, NULL);

    if (ret_mid != ESP_OK || ret_bass != ESP_OK)
    {
        ESP_LOGE("I2S", "Failed to create I2S channels");
        return;
    }
    // Configure and enable channels
    i2s_channel_init_std_mode(tx_chan_mid, &std_cfg_mid);
    i2s_channel_init_std_mode(tx_chan_bass, &std_cfg_bass);

    i2s_channel_enable(tx_chan_mid);
    i2s_channel_enable(tx_chan_bass);
}
void mute_audio_output()
{
    memset(audio_mid, 0, sizeof(audio_mid));
    memset(audio_bass, 0, sizeof(audio_bass));
    size_t bytes_written_mid, bytes_written_bass;
    i2s_channel_write(tx_chan_mid, audio_mid, sizeof(audio_mid), &bytes_written_mid, portMAX_DELAY);
    i2s_channel_write(tx_chan_bass, audio_bass, sizeof(audio_bass), &bytes_written_bass, portMAX_DELAY);
}
void bt_i2s_driver_uninstall(void)
{
    if (tx_chan_mid)
    {
        i2s_channel_disable(tx_chan_mid);
        i2s_del_channel(tx_chan_mid);
        tx_chan_mid = NULL;
    }
    if (tx_chan_bass)
    {
        i2s_channel_disable(tx_chan_bass);
        i2s_del_channel(tx_chan_bass);
        tx_chan_bass = NULL;
    }
}

static void volume_set_by_controller(uint8_t volume)
{
    ESP_LOGI(BT_RC_TG_TAG, "Volume is set by remote controller to: %" PRIu32 "%%", (uint32_t)volume * 100 / 500);
    /* set the volume in protection of lock */
    _lock_acquire(&s_volume_lock);
    s_volume = volume;
    _lock_release(&s_volume_lock);
}

static void volume_set_by_local_host(uint8_t volume)
{
    ESP_LOGI(BT_RC_TG_TAG, "Volume is set locally to: %" PRIu32 "%%", (uint32_t)volume * 100 / 500);
    /* set the volume in protection of lock */
    _lock_acquire(&s_volume_lock);
    s_volume = volume;
    _lock_release(&s_volume_lock);

    /* send notification response to remote AVRCP controller */
    if (s_volume_notify)
    {
        esp_avrc_rn_param_t rn_param;
        rn_param.volume = s_volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        s_volume_notify = false;
    }
}

static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);

    esp_a2d_cb_param_t *a2d = NULL;

    switch (event)
    {
    /* when connection state changed, this event comes */
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        uint8_t *bda = a2d->conn_stat.remote_bda;
        ESP_LOGI(BT_AV_TAG, "A2DP connection state: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 s_a2d_conn_state_str[a2d->conn_stat.state], bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            mute_audio_output();
            vTaskDelay(pdMS_TO_TICKS(50));
            bt_i2s_driver_uninstall();
            bt_i2s_task_shut_down();
        }
        else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            bt_i2s_task_start_up();
        }
        else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING)
        {
            bt_i2s_driver_install();
        }
        break;
    }
    /* when audio stream transmission state changed, this event comes */
    case ESP_A2D_AUDIO_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "A2DP audio state: %s", s_a2d_audio_state_str[a2d->audio_stat.state]);
        s_audio_state = a2d->audio_stat.state;
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state)
        {
            s_pkt_cnt = 0;
        }
        break;
    }
    /* when audio codec is configured, this event comes */
    case ESP_A2D_AUDIO_CFG_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "A2DP audio stream configuration, codec type: %d", a2d->audio_cfg.mcc.type);
        /* for now only SBC stream is supported */
        if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC)
        {
            int sample_rate = 16000;
            int ch_count = 2;
            char oct0 = a2d->audio_cfg.mcc.cie.sbc[0];
            if (oct0 & (0x01 << 6))
            {
                sample_rate = 32000;
            }
            else if (oct0 & (0x01 << 5))
            {
                sample_rate = 44100;
            }
            else if (oct0 & (0x01 << 4))
            {
                sample_rate = 48000;
            }

            if (oct0 & (0x01 << 3))
            {
                ch_count = 1;
            }
#ifdef CONFIG_EXAMPLE_A2DP_SINK_OUTPUT_INTERNAL_DAC
            dac_continuous_disable(tx_chan);
            dac_continuous_del_channels(tx_chan);
            dac_continuous_config_t cont_cfg = {
                .chan_mask = DAC_CHANNEL_MASK_ALL,
                .desc_num = 8,
                .buf_size = 2048,
                .freq_hz = sample_rate,
                .offset = 127,
                .clk_src = DAC_DIGI_CLK_SRC_DEFAULT, // Using APLL as clock source to get a wider frequency range
                .chan_mode = (ch_count == 1) ? DAC_CHANNEL_MODE_SIMUL : DAC_CHANNEL_MODE_ALTER,
            };
            /* Allocate continuous channels */
            dac_continuous_new_channels(&cont_cfg, &tx_chan);
            /* Enable the continuous channels */
            dac_continuous_enable(tx_chan);
#else
            // i2s_channel_disable(tx_chan);
            // i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            // i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, ch_count);
            // i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
            // i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);
            // i2s_channel_enable(tx_chan);

            i2s_channel_disable(tx_chan_mid);
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, ch_count);
            i2s_channel_reconfig_std_clock(tx_chan_mid, &clk_cfg);
            i2s_channel_reconfig_std_slot(tx_chan_mid, &slot_cfg);
            i2s_channel_enable(tx_chan_mid);

            i2s_channel_disable(tx_chan_bass);
            // i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            // i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, ch_count);
            i2s_channel_reconfig_std_clock(tx_chan_bass, &clk_cfg);
            i2s_channel_reconfig_std_slot(tx_chan_bass, &slot_cfg);
            i2s_channel_enable(tx_chan_bass);

#endif
            ESP_LOGI(BT_AV_TAG, "Configure audio player: %x-%x-%x-%x",
                     a2d->audio_cfg.mcc.cie.sbc[0],
                     a2d->audio_cfg.mcc.cie.sbc[1],
                     a2d->audio_cfg.mcc.cie.sbc[2],
                     a2d->audio_cfg.mcc.cie.sbc[3]);
            ESP_LOGI(BT_AV_TAG, "Audio player configured, sample rate: %d", sample_rate);
        }
        break;
    }
    /* when a2dp init or deinit completed, this event comes */
    case ESP_A2D_PROF_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (ESP_A2D_INIT_SUCCESS == a2d->a2d_prof_stat.init_state)
        {
            ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: Init Complete");
        }
        else
        {
            ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: Deinit Complete");
        }
        break;
    }
    /* When protocol service capabilities configured, this event comes */
    case ESP_A2D_SNK_PSC_CFG_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "protocol service capabilities configured: 0x%x ", a2d->a2d_psc_cfg_stat.psc_mask);
        if (a2d->a2d_psc_cfg_stat.psc_mask & ESP_A2D_PSC_DELAY_RPT)
        {
            ESP_LOGI(BT_AV_TAG, "Peer device support delay reporting");
        }
        else
        {
            ESP_LOGI(BT_AV_TAG, "Peer device unsupported delay reporting");
        }
        break;
    }
    /* when set delay value completed, this event comes */
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (ESP_A2D_SET_INVALID_PARAMS == a2d->a2d_set_delay_value_stat.set_state)
        {
            ESP_LOGI(BT_AV_TAG, "Set delay report value: fail");
        }
        else
        {
            ESP_LOGI(BT_AV_TAG, "Set delay report value: success, delay_value: %u * 1/10 ms", a2d->a2d_set_delay_value_stat.delay_value);
        }
        break;
    }
    /* when get delay value completed, this event comes */
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "Get delay report value: delay_value: %u * 1/10 ms", a2d->a2d_get_delay_value_stat.delay_value);
        /* Default delay value plus delay caused by application layer */
        esp_a2d_sink_set_delay_value(a2d->a2d_get_delay_value_stat.delay_value + APP_DELAY_VALUE);
        break;
    }
    /* others */
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_RC_CT_TAG, "%s event: %d", __func__, event);

    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);

    switch (event)
    {
    /* when connection state changed, this event comes */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state event: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        if (rc->conn_stat.connected)
        {
            /* get remote supported event_ids of peer AVRCP Target */
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
        }
        else
        {
            /* clear peer notification capability record */
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    /* when passthrough response, this event comes */
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough rsp: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code,
                 rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    }
    /* when metadata response, this event comes */
    case ESP_AVRC_CT_METADATA_RSP_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC metadata rsp: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
        if (rc->meta_rsp.attr_id == 0x80 && cover_art_connected && cover_art_getting == false)
        {
            /* check image handle is valid and different with last one, wo dont want to get an image repeatedly */
            if (image_handle_check(rc->meta_rsp.attr_text, rc->meta_rsp.attr_length))
            {
                esp_avrc_ct_cover_art_get_linked_thumbnail(rc->meta_rsp.attr_text);
                cover_art_getting = true;
            }
        }
#endif
        free(rc->meta_rsp.attr_text);
        break;
    }
    /* when notified, this event comes */
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    {
        // ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    /* when feature of remote device indicated, this event comes */
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %" PRIx32 ", TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
        if ((rc->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) && !cover_art_connected)
        {
            ESP_LOGW(BT_RC_CT_TAG, "Peer support Cover Art feature, start connection...");
            /* set mtu to zero to use a default value */
            esp_avrc_ct_cover_art_connect(0);
        }
#endif
        break;
    }
    /* when notification capability of peer device got, this event comes */
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        bt_av_new_track();
        bt_av_playback_changed();
        bt_av_play_pos_changed();
        break;
    }
    case ESP_AVRC_CT_COVER_ART_STATE_EVT:
    {
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
        if (rc->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED)
        {
            cover_art_connected = true;
            ESP_LOGW(BT_RC_CT_TAG, "Cover Art Client connected");
        }
        else
        {
            cover_art_connected = false;
            ESP_LOGW(BT_RC_CT_TAG, "Cover Art Client disconnected, reason:%d", rc->cover_art_state.reason);
        }
#endif
        break;
    }
    case ESP_AVRC_CT_COVER_ART_DATA_EVT:
    {
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
        /* when rc->cover_art_data.final is true, it means we have received the entire image or get operation failed */
        if (rc->cover_art_data.final)
        {
            if (rc->cover_art_data.status == ESP_BT_STATUS_SUCCESS)
            {
                ESP_LOGI(BT_RC_CT_TAG, "Cover Art Client final data event, image size: %lu bytes", cover_art_image_size);
            }
            else
            {
                ESP_LOGE(BT_RC_CT_TAG, "Cover Art Client get operation failed");
            }
            cover_art_image_size = 0;
            /* set the getting state to false, we can get next image now */
            cover_art_getting = false;
        }
#endif
        break;
    }
    /* others */
    default:
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_RC_TG_TAG, "%s event: %d", __func__, event);

    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)(p_param);

    switch (event)
    {
    /* when connection state changed, this event comes */
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_TG_TAG, "AVRC conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (rc->conn_stat.connected)
        {
            /* create task to simulate volume change */
            xTaskCreate(encoder_poll_task, "encoder_task", 6144, NULL, 5, &s_encoder_task_hdl);
            // xTaskCreate(encoder_button_task, "encoder_button_task", 2048, NULL, 5, &s_encoderSW_task_hdl);
            ESP_LOGI(BT_RC_TG_TAG, " ------ avrc task created --------");
        }
        else
        {
            vTaskDelete(s_encoder_task_hdl);
            // vTaskDelete(s_encoderSW_task_hdl);

            ESP_LOGI(BT_RC_TG_TAG, " ------ avrc task deleted --------");
        }
        break;
    }
    /* when passthrough commanded, this event comes */
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC passthrough cmd: key_code 0x%x, key_state %d", rc->psth_cmd.key_code, rc->psth_cmd.key_state);
        break;
    }
    /* when absolute volume command from remote device set, this event comes */
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC set absolute volume: %d%%", (int)rc->set_abs_vol.volume * 100 / 500);
        volume_set_by_controller(rc->set_abs_vol.volume);
        break;
    }
    /* when notification registered, this event comes */
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC register event notification: %d, param: 0x%" PRIx32, rc->reg_ntf.event_id, rc->reg_ntf.event_parameter);
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
        {
            s_volume_notify = true;
            esp_avrc_rn_param_t rn_param;
            rn_param.volume = s_volume;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;
    }
    /* when feature of remote device indicated, this event comes */
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC remote features: %" PRIx32 ", CT features: %x", rc->rmt_feats.feat_mask, rc->rmt_feats.ct_feat_flag);
        break;
    }
    /* others */
    default:
        ESP_LOGE(BT_RC_TG_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void encoder_poll_task(void *arg)
{
    gpio_set_direction(ENCODER_PIN_A, GPIO_MODE_INPUT);
    gpio_set_direction(ENCODER_PIN_B, GPIO_MODE_INPUT);
    gpio_pullup_en(ENCODER_PIN_A);
    gpio_pullup_en(ENCODER_PIN_B);

    int lastA = gpio_get_level(ENCODER_PIN_A);
    int64_t last_tick = esp_timer_get_time() / 1000; // میلی‌ثانیه

    while (1)
    {
        // همیشه مقدار ولوم را از s_volume بگیر
        uint8_t volume;
        _lock_acquire(&s_volume_lock);
        volume = s_volume;
        _lock_release(&s_volume_lock);

        int A = gpio_get_level(ENCODER_PIN_A);
        int B = gpio_get_level(ENCODER_PIN_B);
        if (A != lastA)
        {
            int64_t now = esp_timer_get_time() / 1000;
            int64_t delta = now - last_tick;
            last_tick = now;

            int step = 1;
            if (delta < 50)
                step = 5;
            else if (delta < 120)
                step = 3;
            else if (delta < 300)
                step = 2;
            else
                step = 1;

            // فقط اگر ولوم به سقف/کف نرسیده باشد تغییر بده
            if (B != A && volume < 127)
            {
                if (volume <= 127 - step)
                    volume += step;
                else
                    volume = 127;
            }
            else if (B == A && volume > 0)
            {
                if (volume >= step)
                    volume -= step;
                else
                    volume = 0;
            }

            ESP_LOGI("ENCODER", "-------------------------------Volume: %d (step: %d) partymode:%d", volume, step, party_mode);
            volume_set_by_local_host(volume);
            lastA = A;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/********************************
 * EXTERNAL FUNCTION DEFINITIONS
 *******************************/

void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_PROF_STATE_EVT:
    case ESP_A2D_SNK_PSC_CFG_EVT:
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT:
    {
        bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "Invalid A2DP event: %d", event);
        break;
    }
}

void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (len > MAX_AUDIO_BUF)
        return;

    int16_t *audio_in = (int16_t *)data;
    size_t samples = len / 2;

    float vol_factor = (float)s_volume / 500;
    if (vol_factor > 1.0f)
        vol_factor = 1.0f;

    // float max_gain = party_mode ? 0.8f : 0.1f;
    // float gain_bass = volume_bass > max_gain ? max_gain : volume_bass;
    // float gain_mid = volume_mid > max_gain ? max_gain : volume_mid;

    if (party_mode)
    {
        volume_bass = 1.0f;
        volume_mid = 1.0f;
    }
    else
    {
        volume_bass = 0.3f;
        volume_mid = 0.3f;
    }

    for (size_t i = 0; i < samples; i++)
    {
        float x = (float)(audio_in[i] * vol_factor);

        lp_y = IIR_ALPHA * x + (1.0f - IIR_ALPHA) * lp_y;
        // float bass = lp_y * volume_bass;
        float bass = x * volume_bass;
        
        if (bass > 32767)
            bass = 32767;
        if (bass < -32768)
            bass = -32768;
        audio_bass[i] = (int16_t)bass;

        float mid = (x - lp_y) * volume_mid;
        if (mid > 32767)
            mid = 32767;
        if (mid < -32768)
            mid = -32768;
        audio_mid[i] = (int16_t)mid;
    }

    size_t bytes_written_mid, bytes_written_bass;
    i2s_channel_write(tx_chan_mid, audio_mid, len, &bytes_written_mid, portMAX_DELAY);
    i2s_channel_write(tx_chan_bass, audio_bass, len, &bytes_written_bass, portMAX_DELAY);
}

void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
#if CONFIG_EXAMPLE_AVRCP_CT_COVER_ART_ENABLE
    /* we must handle ESP_AVRC_CT_COVER_ART_DATA_EVT in this callback, copy image data to other buff before return if need */
    if (event == ESP_AVRC_CT_COVER_ART_DATA_EVT && param->cover_art_data.status == ESP_BT_STATUS_SUCCESS)
    {
        cover_art_image_size += param->cover_art_data.data_len;
        /* copy image data to other place */
        /* memcpy(p_buf, param->cover_art_data.p_data, param->cover_art_data.data_len); */
    }
#endif
    switch (event)
    {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        bt_app_alloc_meta_buffer(param);
        /* fall through */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    case ESP_AVRC_CT_COVER_ART_STATE_EVT:
    case ESP_AVRC_CT_COVER_ART_DATA_EVT:
    {
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    }
    default:
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
}

void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    case ESP_AVRC_TG_SET_PLAYER_APP_VALUE_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_tg_evt, event, param, sizeof(esp_avrc_tg_cb_param_t), NULL);
        break;
    default:
        ESP_LOGE(BT_RC_TG_TAG, "Invalid AVRC event: %d", event);
        break;
    }
}
