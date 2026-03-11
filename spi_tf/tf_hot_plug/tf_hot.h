/*
 * TF Card Hot-Plug Detection Module
 * 
 * This module provides hot-plug detection functionality for TF/SD cards
 * in both SPI and SDIO modes. It detects card insertion/removal events
 * and notifies application layer through callbacks.
 */

#ifndef __TF_HOT_H__
#define __TF_HOT_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TF Card State Definition
 */
typedef enum {
    TF_STATE_OUT = 0,      /*!< TF card removed */
    TF_STATE_IN = 1,       /*!< TF card inserted */
} tf_state_t;

/**
 * @brief TF Card Event Callback Type
 * 
 * @param state The current state of TF card (IN or OUT)
 * @param user_data User-provided data passed during callback registration
 */
typedef void (*tf_hot_plug_callback_t)(tf_state_t state, void *user_data);

/**
 * @brief Initialize TF card hot-plug detection module
 * 
 * This function initializes the GPIO pin for card detection, sets up
 * interrupt handlers, and creates the detection thread.
 * 
 * @param pin_num GPIO pin number for card detection
 * @return rt_err_t RT_EOK on success, RT_ERROR on failure
 */
rt_err_t tf_hot_init(uint8_t pin_num);

/**
 * @brief Register state change callback
 * 
 * When TF card state changes, the registered callback will be invoked
 * with the new state and user-provided data.
 * 
 * @param callback Callback function pointer
 * @param user_data User data to be passed to callback
 * @return rt_err_t RT_EOK on success, RT_ERROR on failure
 */
rt_err_t tf_hot_register_callback(tf_hot_plug_callback_t callback, void *user_data);

/**
 * @brief Synchronize initial TF card state after initialization
 *
 * This function is intended to be called once after tf_hot_init() and
 * callback registration. If the card is already inserted at boot time,
 * it will trigger insertion handling immediately so filesystem/device
 * state is consistent before runtime hot-plug events arrive.
 *
 * @return rt_err_t RT_EOK on success, RT_ERROR on failure
 */
rt_err_t tf_hot_sync_initial_state(void);

/**
 * @brief Get current TF card state
 * 
 * @return tf_state_t Current state (TF_STATE_IN or TF_STATE_OUT)
 */
tf_state_t tf_hot_get_state(void);

/**
 * @brief Check if TF card is present
 * 
 * @return rt_bool_t RT_TRUE if card is inserted, RT_FALSE otherwise
 */
rt_bool_t tf_hot_is_present(void);

/**
 * @brief Deinitialize TF card hot-plug detection module
 * 
 * This function stops the detection thread and releases all resources.
 * 
 * @return rt_err_t RT_EOK on success
 */
rt_err_t tf_hot_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __TF_HOT_H__ */
