/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "button_hal.h"
#include "nrfx_timer.h"
#include "platform_config.h"
#include "interrupts_hal.h"
#include "pinmap_impl.h"
#include "nrfx_gpiote.h"
#include "gpio_hal.h"
#include "core_hal.h"
#include "dct.h"
#include "logging.h"

#define TIMERx_ID               2
#define TIMERx_IRQ_PRIORITY     APP_IRQ_PRIORITY_MID

// 16bit counter, 125KHz clock, interrupt interval: 10ms
#define TIMERx_BIT_WIDTH        NRF_TIMER_BIT_WIDTH_16
#define TIMERx_FREQUENCY        NRF_TIMER_FREQ_125kHz

button_config_t HAL_Buttons[] = {
    {
        .active         = false,
        .pin            = BUTTON1_PIN,
        .interrupt_mode = BUTTON1_INTERRUPT_MODE,
        .debounce_time  = 0,
    },
    {
        .active         = false,
        .pin            = BUTTON1_MIRROR_PIN,
        .interrupt_mode = BUTTON1_MIRROR_INTERRUPT_MODE,
        .debounce_time  = 0
    }
};

const nrfx_timer_t m_button_timer = NRFX_TIMER_INSTANCE(TIMERx_ID);
volatile bool   m_button_timer_initialized = false;

// TODO: Use RTC0/1/2 to implement button timer
static void button_timer_init(void);
static void button_timer_uninit(void);
static void button_timer_start(void);
static void button_timer_stop(void);
static void button_timer_event_handler(nrf_timer_event_t event_type, void* p_context);
static void button_reset(uint16_t button);

static void button_timer_init(void) {
    if (!m_button_timer_initialized) {
        nrfx_timer_config_t timer_cfg = {
            .frequency          = TIMERx_FREQUENCY,
            .mode               = NRF_TIMER_MODE_TIMER,
            .bit_width          = TIMERx_BIT_WIDTH,
            .interrupt_priority = TIMERx_IRQ_PRIORITY,
            .p_context          = NULL
        };
        SPARK_ASSERT(nrfx_timer_init(&m_button_timer, &timer_cfg, button_timer_event_handler) == NRF_SUCCESS);

        uint32_t time_ticks = nrfx_timer_ms_to_ticks(&m_button_timer, BUTTON_DEBOUNCE_INTERVAL);
        nrfx_timer_extended_compare(&m_button_timer, NRF_TIMER_CC_CHANNEL0, time_ticks, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);

        m_button_timer_initialized = true;
    }
}

static void button_timer_uninit(void) {
    nrfx_timer_uninit(&m_button_timer);
}

static void button_timer_start(void) {
    if (!nrfx_timer_is_enabled(&m_button_timer)) {
        nrfx_timer_enable(&m_button_timer);
    }
}

static void button_timer_stop(void) {
    nrfx_timer_disable(&m_button_timer);
    nrfx_timer_clear(&m_button_timer);
}

static void button_timer_event_handler(nrf_timer_event_t event_type, void* p_context) {
    if (event_type == NRF_TIMER_EVENT_COMPARE0) {
        if (HAL_Buttons[BUTTON1].active && (BUTTON_GetState(BUTTON1) == BUTTON1_PRESSED)) {
            if (!HAL_Buttons[BUTTON1].debounce_time) {
                HAL_Buttons[BUTTON1].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
#if MODULE_FUNCTION != MOD_FUNC_BOOTLOADER
                HAL_Notify_Button_State(BUTTON1, true); 
#endif
            }
            HAL_Buttons[BUTTON1].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
        } else if (HAL_Buttons[BUTTON1].active) {
            HAL_Buttons[BUTTON1].active = false;
            button_reset(BUTTON1);
        }

        if ((HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) && HAL_Buttons[BUTTON1_MIRROR].active &&
            BUTTON_GetState(BUTTON1_MIRROR) == (HAL_Buttons[BUTTON1_MIRROR].interrupt_mode == RISING ? 1 : 0)) 
        {
            if (!HAL_Buttons[BUTTON1_MIRROR].debounce_time) {
                HAL_Buttons[BUTTON1_MIRROR].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
#if MODULE_FUNCTION != MOD_FUNC_BOOTLOADER
                HAL_Notify_Button_State(BUTTON1_MIRROR, true);
#endif
            }
            HAL_Buttons[BUTTON1_MIRROR].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
        } else if ((HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) && HAL_Buttons[BUTTON1_MIRROR].active) {
            HAL_Buttons[BUTTON1_MIRROR].active = false;
            button_reset(BUTTON1_MIRROR);
        }
    }
}

static void button_reset(uint16_t button) {
    HAL_Buttons[button].debounce_time = 0x00;

    if (!HAL_Buttons[BUTTON1].active && !HAL_Buttons[BUTTON1_MIRROR].active) {
        button_timer_stop();
    }

#if MODULE_FUNCTION != MOD_FUNC_BOOTLOADER
    HAL_Notify_Button_State((Button_TypeDef)button, false); 
#endif

    /* Enable Button Interrupt */
    BUTTON_EXTI_Config((Button_TypeDef)button, ENABLE);
}

static void BUTTON_Interrupt_Handler(void *data) {
    Button_TypeDef button = (Button_TypeDef)data;

    HAL_Buttons[button].debounce_time = 0x00;
    HAL_Buttons[button].active = true;

    BUTTON_EXTI_Config(button, DISABLE);

    button_timer_start();
}

void BUTTON_Init(Button_TypeDef button, ButtonMode_TypeDef Button_Mode) {
    // Initialize button timer
    button_timer_init();

    // Configure button pin
    HAL_Pin_Mode(HAL_Buttons[button].pin, BUTTON1_PIN_MODE);
    if (Button_Mode == BUTTON_MODE_EXTI)  {
        /* Attach GPIOTE Interrupt */
        BUTTON_EXTI_Config(button, ENABLE);
    }

    // Check status when starting up
    if (HAL_Buttons[button].pin != PIN_INVALID && 
        BUTTON_GetState(button) == (HAL_Buttons[button].interrupt_mode == RISING ? 1 : 0)) 
    {
        HAL_Buttons[button].active = true;
        button_timer_start();
    }
}

void BUTTON_EXTI_Config(Button_TypeDef button, FunctionalState NewState) {
    HAL_InterruptExtraConfiguration config = {0};
    config.version = HAL_INTERRUPT_EXTRA_CONFIGURATION_VERSION;
    config.keepHandler = false;
    config.flags = HAL_DIRECT_INTERRUPT_FLAG_NONE;

    if (NewState == ENABLE) {
        HAL_Interrupts_Attach(HAL_Buttons[button].pin, BUTTON_Interrupt_Handler, (void *)((int)button), FALLING, &config); 
    } else {
        HAL_Interrupts_Detach(HAL_Buttons[button].pin);
    }
}

/**
 * @brief  Returns the selected Button non-filtered state.
 * @param  Button: Specifies the Button to be checked.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON1: Button1
 * @retval Actual Button Pressed state.
 */
uint8_t BUTTON_GetState(Button_TypeDef Button) {
    return HAL_GPIO_Read(HAL_Buttons[Button].pin);
}

/**
 * @brief  Returns the selected Button Debounced Time.
 * @param  Button: Specifies the Button to be checked.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON1: Button1
 * @retval Button Debounced time in millisec.
 */
uint16_t BUTTON_GetDebouncedTime(Button_TypeDef Button) {
    return HAL_Buttons[Button].debounce_time;
}

void BUTTON_ResetDebouncedState(Button_TypeDef Button) {
    HAL_Buttons[Button].debounce_time = 0;
}

void BUTTON_Check_State(uint16_t button, uint8_t pressed) {
    if (BUTTON_GetState(button) == pressed) {
        if (!HAL_Buttons[button].active) {
            HAL_Buttons[button].active = true;
        }
        HAL_Buttons[button].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
    } else if (HAL_Buttons[button].active) {
        HAL_Buttons[button].active = false;
        /* Enable button Interrupt */
        BUTTON_EXTI_Config(button, ENABLE);
    }
}

static void BUTTON_Mirror_Persist(button_config_t* conf) {
    button_config_t saved_config;
    dct_read_app_data_copy(DCT_MODE_BUTTON_MIRROR_OFFSET, &saved_config, sizeof(saved_config));

    if (conf) {
        if (saved_config.active == 0xFF || memcmp((void*)conf, (void*)&saved_config, sizeof(button_config_t))) {
            dct_write_app_data((void*)conf, DCT_MODE_BUTTON_MIRROR_OFFSET, sizeof(button_config_t));
        }
    } else {
        if (saved_config.active != 0xFF) {
            memset((void*)&saved_config, 0xff, sizeof(button_config_t));
            dct_write_app_data((void*)&saved_config, DCT_MODE_BUTTON_MIRROR_OFFSET, sizeof(button_config_t));
        }
    }
}

void HAL_Core_Button_Mirror_Pin_Disable(uint8_t bootloader, uint8_t button, void* reserved) {
    (void)button; // unused
    int32_t state = HAL_disable_irq();
    if (HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) {
        HAL_Interrupts_Detach_Ext(HAL_Buttons[BUTTON1_MIRROR].pin, 1, NULL);
        HAL_Buttons[BUTTON1_MIRROR].active = 0;
        HAL_Buttons[BUTTON1_MIRROR].pin = PIN_INVALID;
    }
    HAL_enable_irq(state);

    if (bootloader) {
        BUTTON_Mirror_Persist(NULL);
    }
}

void HAL_Core_Button_Mirror_Pin(uint16_t pin, InterruptMode mode, uint8_t bootloader, uint8_t button, void *reserved) {
    (void)button; // unused
    if (pin > TOTAL_PINS) {
        return;
    }

    if (mode != RISING && mode != FALLING) {
        return;
    }

    button_config_t conf = {
        .pin = pin,
        .debounce_time = 0,
        .interrupt_mode = mode,
    };

    HAL_Buttons[BUTTON1_MIRROR] = conf;

    BUTTON_Init(BUTTON1_MIRROR, BUTTON_MODE_EXTI);

    if (pin == HAL_Buttons[BUTTON1].pin) {
        LOG(WARN, "Pin %d shares the same EXTI as SETUP/MODE button", pin);
        BUTTON_Mirror_Persist(NULL);
        return;
    }

    if (!bootloader) {
        BUTTON_Mirror_Persist(NULL);
        return;
    }

    button_config_t bootloader_conf = {
        .active = 0xAA,
        .debounce_time = 0xBBCC,
        .pin = pin,
        .interrupt_mode = mode,
    };

    BUTTON_Mirror_Persist(&bootloader_conf);
}

void BUTTON_Init_Ext() {
    button_config_t button_config = {0};
    dct_read_app_data_copy(DCT_MODE_BUTTON_MIRROR_OFFSET, &button_config, sizeof(button_config_t));

    if (button_config.active == 0xAA && button_config.debounce_time == 0xBBCC) {
        //int32_t state = HAL_disable_irq();
        memcpy((void*)&HAL_Buttons[BUTTON1_MIRROR], (void*)&button_config, sizeof(button_config_t));
        HAL_Buttons[BUTTON1_MIRROR].active = 0;
        HAL_Buttons[BUTTON1_MIRROR].debounce_time = 0;
        BUTTON_Init(BUTTON1_MIRROR, BUTTON_MODE_EXTI);
        //HAL_enable_irq(state);
    }
}

void BUTTON_Uninit() {
    button_timer_uninit();
    HAL_Interrupts_Uninit();
}

// Just for compatibility in bootloader
// Check both BUTTON1 and BUTTON1_MIRROR
uint8_t BUTTON_Is_Pressed(Button_TypeDef button) {
    uint8_t pressed = 0;
    pressed = HAL_Buttons[button].active;

    if (button == BUTTON1 && HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) {
        pressed |= HAL_Buttons[BUTTON1_MIRROR].active;
    }

    return pressed;
}

// Just for compatibility in bootloader
// Check both BUTTON1 and BUTTON1_MIRROR
uint16_t BUTTON_Pressed_Time(Button_TypeDef button) {
    uint16_t pressed = 0;

    pressed = HAL_Buttons[button].debounce_time;
    if (button == BUTTON1 && HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) {
        if (HAL_Buttons[BUTTON1_MIRROR].debounce_time > pressed) {
            pressed = HAL_Buttons[BUTTON1_MIRROR].debounce_time;
        }
    }

    return pressed;
}

