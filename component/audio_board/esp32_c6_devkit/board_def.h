/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_


/**
 * @brief Audio Codec Chip Function Definition
 */
#define BOARD_PA_GAIN               (10) /* Power amplifier gain defined by board (dB) */


/**
 * @brief ADC Function Definition
 */
#define MIC_ADC_GPIO              -1
#define PA_ENABLE_GPIO            GPIO_NUM_7
#define BUTTON_ADC_GPIO           GPIO_NUM_2


/**
 * @brief SDCARD Function Definition
 */
#define FUNC_SDCARD_EN             (0)
#define SDCARD_OPEN_FILE_NUM_MAX    5
#define SDCARD_INTR_GPIO            -1
#define SDCARD_PWR_CTRL             -1

#define ESP_SD_PIN_CLK              -1
#define ESP_SD_PIN_CMD              -1
#define ESP_SD_PIN_D0               -1
#define ESP_SD_PIN_D3               -1


/**
 * @brief PDM TX Function Definition
 */
#define PDM_TX_GPIO               GPIO_NUM_3


/**
 * @brief LED Function Definition
 */
#define WS2812_LED_GPIO_PIN       GPIO_NUM_8
#define WS2812_LED_BAR_NUMBERS    1


/**
 * @brief IR Function Definition
 */
#define ESP_IR_TX_GPIO            GPIO_NUM_18
#define ESP_IR_RX_GPIO            GPIO_NUM_19


/**
 * @brief I2C Function Definition
 */
#define I2C_CLK_GPIO              GPIO_NUM_18
#define I2C_DATA_GPIO             GPIO_NUM_19


/**
 * @brief Button Function Definition
 */
#define INPUT_KEY_NUM             6
#define BUTTON_VOLUP_ID           0
#define BUTTON_VOLDOWN_ID         1
#define BUTTON_SET_ID             2
#define BUTTON_PLAY_ID            3
#define BUTTON_MODE_ID            4
#define BUTTON_REC_ID             5

// Remove or comment out ES8311 codec handle
// extern audio_hal_func_t AUDIO_CODEC_ES8311_DEFAULT_HANDLE;

// Remove or comment out AUDIO_CODEC_DEFAULT_CONFIG

// Set I2S pins for your S3 board and 1334A DAC
#define I2S_MCK_IO_NUM   <your_mck_pin>
#define I2S_BCK_IO_NUM   <your_bck_pin>
#define I2S_WS_IO_NUM    <your_ws_pin>
#define I2S_DO_IO_NUM    <your_do_pin>
#define I2S_DI_IO_NUM    <your_di_pin> // if needed

// Optionally, add a macro to indicate no codec
#define BOARD_NO_AUDIO_CODEC 1

#define INPUT_KEY_DEFAULT_INFO() {                  \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_REC,           \
        .act_id = BUTTON_REC_ID,                    \
    },                                              \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_MODE,          \
        .act_id = BUTTON_MODE_ID,                   \
    },                                              \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_SET,           \
        .act_id = BUTTON_SET_ID,                    \
    },                                              \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_PLAY,          \
        .act_id = BUTTON_PLAY_ID,                   \
    },                                              \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_VOLUP,         \
        .act_id = BUTTON_VOLUP_ID,                  \
    },                                              \
    {                                               \
        .type = PERIPH_ID_ADC_BTN,                  \
        .user_id = INPUT_KEY_USER_ID_VOLDOWN,       \
        .act_id = BUTTON_VOLDOWN_ID,                \
    }                                               \
}

#endif
