/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef __BT_APP_AV_H__
#define __BT_APP_AV_H__

#include <stdint.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/* log tags */
#define BT_AV_TAG       "BT_AV"
#define BT_RC_TG_TAG    "RC_TG"
#define BT_RC_CT_TAG    "RC_CT"

/**
 * @brief  callback function for A2DP sink
 *
 * @param [in] event  event id
 * @param [in] param  callback parameter
 */
void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/**
 * @brief  callback function for A2DP sink audio data stream
 *
 * @param [out] data  data stream writteen by application task
 * @param [in]  len   length of data stream in byte
 */
void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);

/**
 * @brief  callback function for AVRCP controller
 *
 * @param [in] event  event id
 * @param [in] param  callback parameter
 */
void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/**
 * @brief  callback function for AVRCP target
 *
 * @param [in] event  event id
 * @param [in] param  callback parameter
 */
void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

typedef enum {
    PLAYER_PLAY_OR_PUSE=1,
    PLAYER_NEXT_TRACK,
    PLAYER_PREVIOUS_TRACK,
    PLAYER_CHNAGE_PARTY_MODE
} player_type;


void mute_audio_output();
extern bool is_playing ;
extern bool party_mode ;

/* AVRCP used transaction labels */
#define APP_RC_CT_TL_GET_CAPS (0)
#define APP_RC_CT_TL_GET_META_DATA (1)
#define APP_RC_CT_TL_RN_TRACK_CHANGE (2)
#define APP_RC_CT_TL_RN_PLAYBACK_CHANGE (3)
#define APP_RC_CT_TL_RN_PLAY_POS_CHANGE (4)

/* Application layer causes delay value */
#define APP_DELAY_VALUE 50 // 5ms

#define ENCODER_PIN_A 12
#define ENCODER_PIN_B 13

#define CLICK_TIMEOUT_MS 400

#define PARTY_MODE_LED_GPIO 2 // پایه LED روی D1 R32 معمولاً GPIO2 است (LED آبی روی برد)



#endif /* __BT_APP_AV_H__*/
