/*
 * TF Card Hot-Plug Detection Implementation
 */

#include "tf_hot.h"
#include <dfs_file.h>
#include <dfs_fs.h>
#include "spi_msd.h"

#define LOG_TAG "tf_hot"
#define LOG_LVL LOG_LVL_DBG
#include <drv_log.h>

/* ======================== Configuration ======================== */

#define TF_DEBOUNCE_MS         10      /*!< Debounce delay in ms */
#define TF_DETECT_THREAD_STACK 2048    /*!< Detection thread stack size */
#define TF_DETECT_THREAD_PRIO  14      /*!< Detection thread priority */

/* ======================== Global Variables ======================== */

static tf_state_t               g_tf_state = TF_STATE_OUT;
static rt_sem_t                 g_tf_detect_sem = RT_NULL;
static struct rt_thread         g_tf_detect_thread_obj;
static rt_uint8_t               g_tf_thread_stack[TF_DETECT_THREAD_STACK];

static tf_hot_plug_callback_t   g_tf_callback = RT_NULL;
static void                    *g_tf_callback_user_data = RT_NULL;

static uint8_t                  g_tf_pin = TF_HOT_PLUG_PIN;
static rt_bool_t                g_tf_hot_initialized = RT_FALSE;

/* ======================== Helper Functions ======================== */

/**
 * @brief Get RT-Device for SDCARD
 */
static rt_device_t tf_get_sdcard_device(void)
{
    rt_device_t dev = rt_device_find("sd0");
    return dev;
}

/**
 * @brief Initialize SD device (SPI mode for SF32LB52X)
 */
static rt_err_t tf_device_init(void)
{
    rt_device_t dev = tf_get_sdcard_device();
    
    /* Device already initialized */
    if (dev != RT_NULL)
    {
        LOG_I("SD device already initialized");
        return RT_EOK;
    }
    
    /* Try to initialize SPI MSD mode */
#if defined(RT_USING_SPI_MSD)
    extern int rt_spi_msd_init(void);
    if (rt_spi_msd_init() == RT_EOK)
    {
        LOG_I("SPI MSD initialized successfully");
        return RT_EOK;
    }
#endif
    
    LOG_I("Failed to initialize SD device");
    return RT_ERROR;
}

/**
 * @brief Mount file system on SD card
 */
static rt_err_t tf_filesystem_mount(void)
{
    rt_device_t dev = tf_get_sdcard_device();
    if (dev == RT_NULL)
    {
        LOG_I("SD device not found, cannot mount filesystem");
        return RT_ERROR;
    }
    
    /* Try to mount filesystem directly */
    /* If already mounted, dfs_mount will handle it appropriately */
    if (dfs_mount("sd0", "/", "elm", 0, 0) != 0)
    {
        LOG_W("Failed to mount filesystem with sd0, retrying with sdcard device...");
        
        /* Try mounting on sdcard device */
        rt_device_t sdcard_dev = rt_device_find("sdcard");
        if (sdcard_dev != RT_NULL)
        {
            if (dfs_mount("sdcard", "/tf_card", "elm", 0, 0) == 0)
            {
                LOG_I("Filesystem mounted on /tf_card");
                return RT_EOK;
            }
        }
        
        LOG_I("Failed to mount filesystem");
        return RT_ERROR;
    }
    
    LOG_I("Filesystem mounted successfully on /");
    return RT_EOK;
}

/**
 * @brief Unmount file system from SD card
 */
static rt_err_t tf_filesystem_unmount(void)
{
    if (dfs_unmount("/") == 0)
    {
        LOG_I("Filesystem unmounted from /");
        return RT_EOK;
    }
    
    if (dfs_unmount("/tf_card") == 0)
    {
        LOG_I("Filesystem unmounted from /tf_card");
        return RT_EOK;
    }
    
    LOG_W("No mounted filesystem to unmount");
    return RT_EOK;
}

/**
 * @brief Unregister SD device
 */
static rt_err_t tf_device_deinit(void)
{
    rt_device_t dev = tf_get_sdcard_device();
    
    if (dev == RT_NULL)
    {
        return RT_EOK;
    }
    
#if defined(RT_USING_SPI_MSD)
    if (rt_device_unregister(dev) != RT_EOK)
    {
        LOG_W("Failed to unregister sd0 device");
        return RT_ERROR;
    }
#endif
    
    /* Try to unregister sdcard device if it exists */
    rt_device_t sdcard_dev = rt_device_find("sdcard");
    if (sdcard_dev != RT_NULL)
    {
        rt_device_unregister(sdcard_dev);
    }
    
    LOG_I("SD device unregistered");
    return RT_EOK;
}

/**
 * @brief Handle TF card insertion
 */
static rt_err_t tf_insert_handler(void)
{
    LOG_I("TF card insertion detected");
    
    /* Initialize device */
    if (tf_device_init() != RT_EOK)
    {
        LOG_I("Failed to initialize device");
        return RT_ERROR;
    }
    
    /* Give device time to be recognized */
    rt_thread_mdelay(100);
    
    /* Mount filesystem */
    if (tf_filesystem_mount() != RT_EOK)
    {
        LOG_I("Failed to mount filesystem");
        return RT_ERROR;
    }
    
    LOG_I("TF card insertion handled successfully");
    return RT_EOK;
}

/**
 * @brief Handle TF card removal
 */
static rt_err_t tf_extract_handler(void)
{
    LOG_I("TF card removal detected");
    
    /* Unmount filesystem */
    if (tf_filesystem_unmount() != RT_EOK)
    {
        LOG_W("Failed to unmount filesystem cleanly");
    }
    
    /* Unregister device */
    if (tf_device_deinit() != RT_EOK)
    {
        LOG_W("Failed to unregister device cleanly");
    }
    
    LOG_I("TF card removal handled");
    return RT_EOK;
}

/* ======================== Interrupt & Thread Handling ======================== */

/**
 * @brief GPIO interrupt callback
 */
static void tf_pin_irq_callback(void *args)
{
    /* Release semaphore to wake up detection thread */
    if (g_tf_detect_sem != RT_NULL)
    {
        rt_sem_release(g_tf_detect_sem);
    }
}

/**
 * @brief TF card detection thread
 */
static void tf_detection_thread(void *param)
{
    uint8_t last_pin_state = rt_pin_read(g_tf_pin);
    
    LOG_I("TF card detection thread started");
    
    while (g_tf_hot_initialized)
    {
        /* Wait for interrupt or timeout - both cases will check GPIO state */
        rt_sem_take(g_tf_detect_sem, rt_tick_from_millisecond(100));
        
        /* Disable interrupt to read pin safely */
        rt_pin_irq_enable(g_tf_pin, 0);
        rt_thread_mdelay(1);
        
        uint8_t current_pin_state = rt_pin_read(g_tf_pin);
        
        /* Debounce: read again after delay */
        rt_thread_mdelay(TF_DEBOUNCE_MS);
        uint8_t confirm_pin_state = rt_pin_read(g_tf_pin);
        
        /* Enable interrupt again */
        rt_pin_irq_enable(g_tf_pin, 1);
        
        /* Check if pin state is stable */
        if (current_pin_state == confirm_pin_state)
        {
            /* Pin state changed? */
            if (current_pin_state != last_pin_state)
            {
                LOG_D("Pin state changed: %d -> %d", last_pin_state, current_pin_state);
                
                /* State transition only if it makes sense */
                /* PIN_STATE_LOW (0) = Card Inserted */
                /* PIN_STATE_HIGH (1) = Card Removed */
                
                if (current_pin_state == 0 && g_tf_state == TF_STATE_OUT)
                {
                    /* Card inserted */
                    LOG_I("Card insertion confirmed");
                    tf_insert_handler();
                    g_tf_state = TF_STATE_IN;
                    
                    /* Notify application */
                    if (g_tf_callback != RT_NULL)
                    {
                        g_tf_callback(TF_STATE_IN, g_tf_callback_user_data);
                    }
                }
                else if (current_pin_state == 1 && g_tf_state == TF_STATE_IN)
                {
                    /* Card removed */
                    LOG_I("Card removal confirmed");
                    tf_extract_handler();
                    g_tf_state = TF_STATE_OUT;
                    
                    /* Notify application */
                    if (g_tf_callback != RT_NULL)
                    {
                        g_tf_callback(TF_STATE_OUT, g_tf_callback_user_data);
                    }
                }
                else
                {
                    LOG_D("State unchanged or duplicate event, skipping");
                }
                
                last_pin_state = current_pin_state;
            }
        }
        else
        {
            LOG_D("Pin state unstable: %d -> %d, discarding", current_pin_state, confirm_pin_state);
        }
    }
    
    LOG_I("TF card detection thread stopped");
}

/* ======================== Public API Functions ======================== */

rt_err_t tf_hot_init(uint8_t pin_num)
{
    if (g_tf_hot_initialized)
    {
        LOG_W("TF hot-plug already initialized");
        return RT_EOK;
    }
    
    g_tf_pin = pin_num;
    
    LOG_I("Initializing TF card hot-plug detection on pin %d", g_tf_pin);
    
    /* Configure pin as input */
    rt_pin_mode(g_tf_pin, PIN_MODE_INPUT);
    
    /* Read initial state */
    rt_thread_mdelay(10);
    uint8_t init_state = rt_pin_read(g_tf_pin);
    rt_thread_mdelay(10);
    
    if (init_state == rt_pin_read(g_tf_pin))
    {
        g_tf_state = (init_state == 0) ? TF_STATE_IN : TF_STATE_OUT;
        LOG_I("Initial TF state: %s", g_tf_state == TF_STATE_IN ? "IN" : "OUT");
    }
    
    /* Create semaphore */
    g_tf_detect_sem = rt_sem_create("tf_detect", 0, RT_IPC_FLAG_FIFO);
    if (g_tf_detect_sem == RT_NULL)
    {
        LOG_I("Failed to create detection semaphore");
        return RT_ERROR;
    }
    
    /* Create detection thread */
    rt_err_t ret = rt_thread_init(&g_tf_detect_thread_obj,
                                   "tf_detect",
                                   tf_detection_thread,
                                   RT_NULL,
                                   &g_tf_thread_stack[0],
                                   TF_DETECT_THREAD_STACK,
                                   TF_DETECT_THREAD_PRIO,
                                   20);
    
    if (ret != RT_EOK)
    {
        LOG_I("Failed to create detection thread");
        rt_sem_delete(g_tf_detect_sem);
        g_tf_detect_sem = RT_NULL;
        return RT_ERROR;
    }
    
    /* Attach and enable GPIO interrupt */
    rt_pin_attach_irq(g_tf_pin, PIN_IRQ_MODE_RISING_FALLING, tf_pin_irq_callback, RT_NULL);
    rt_pin_irq_enable(g_tf_pin, 1);
    
    /* Start thread */
    rt_thread_startup(&g_tf_detect_thread_obj);
    
    g_tf_hot_initialized = RT_TRUE;
    
    LOG_I("TF hot-plug detection initialized successfully");
    
    return RT_EOK;
}

rt_err_t tf_hot_register_callback(tf_hot_plug_callback_t callback, void *user_data)
{
    if (callback == RT_NULL)
    {
        LOG_W("Callback function is NULL");
        return RT_ERROR;
    }
    
    g_tf_callback = callback;
    g_tf_callback_user_data = user_data;
    
    LOG_I("TF hot-plug callback registered");
    
    return RT_EOK;
}

rt_err_t tf_hot_sync_initial_state(void)
{
    if (!g_tf_hot_initialized)
    {
        LOG_W("TF hot-plug is not initialized");
        return RT_ERROR;
    }

    if (g_tf_state == TF_STATE_IN)
    {
        LOG_I("Initial state is IN, syncing insertion handling");

        if (tf_insert_handler() != RT_EOK)
        {
            LOG_W("Failed to sync initial insertion state");
            return RT_ERROR;
        }

        if (g_tf_callback != RT_NULL)
        {
            g_tf_callback(TF_STATE_IN, g_tf_callback_user_data);
        }
    }
    else
    {
        LOG_I("Initial state is OUT, no sync action required");
    }

    return RT_EOK;
}

tf_state_t tf_hot_get_state(void)
{
    return g_tf_state;
}

rt_bool_t tf_hot_is_present(void)
{
    return (g_tf_state == TF_STATE_IN);
}

rt_err_t tf_hot_deinit(void)
{
    if (!g_tf_hot_initialized)
    {
        return RT_EOK;
    }
    
    LOG_I("Deinitializing TF hot-plug detection");
    
    /* Disable interrupt */
    rt_pin_irq_enable(g_tf_pin, 0);
    rt_pin_detach_irq(g_tf_pin);
    
    /* Mark as not initialized (thread will exit) */
    g_tf_hot_initialized = RT_FALSE;
    
    /* Release semaphore to wake thread */
    if (g_tf_detect_sem != RT_NULL)
    {
        rt_sem_release(g_tf_detect_sem);
        
        /* Give thread time to exit */
        rt_thread_mdelay(100);
        
        /* Clean up semaphore */
        rt_sem_delete(g_tf_detect_sem);
        g_tf_detect_sem = RT_NULL;
    }
    
    /* Clean up callback */
    g_tf_callback = RT_NULL;
    g_tf_callback_user_data = RT_NULL;
    
    LOG_I("TF hot-plug detection deinitialized");
    
    return RT_EOK;
}
