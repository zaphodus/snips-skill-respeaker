#include "button.h"
#include "common.h"
#include "cAPA102.h"
#include "cCONFIG.h"
#include "gpio_rw.h"
#include "load_hw.h"
#include "load_sw.h"
#include "mqtt.h"
#include "mqtt_client.h"
#include "parse_opts.h"
#include "state_handler.h"
#include "verbose.h"

SNIPS_RUN_PARA RUN_PARA = {
    /* Hardware */
    "", {-1, -1, -1}, {-1, -1}, {-1, -1},
    /* Brightness */
    31,
    /* MQTT connection */
    "localhost", "1883", "", "",
    /* SiteId */
    "default",
    /* Client ID */
    NULL,
    /* Animation thread */
    0, // NULL
    ON_IDLE,
    /* Colour */
    {GREEN_C, BLUE_C, PURPLE_C, YELLOW_C, GREEN_C},
    /* Sleep mode */
    0, 0, 0, 0,
    /* Flags */
    0, 1, 0, 0,
    /* Animation Enable */
    {1, 1, 1, 1, 1, 1},
    /* Mute*/
    0
};

/**
 * @brief: callback for long click
 */
void long_press_hadler(void) {
    verbose(VV_INFO, stdout, BLUE"[%s]"NONE" toggling sound feedback!", __FUNCTION__);
    if (RUN_PARA.if_mute)
        RUN_PARA.if_mute = 0;
    else
        RUN_PARA.if_mute = 1;

    if (RUN_PARA.if_mute)
        mqtt_mute_feedback();
    else
        mqtt_unmute_feedback();
}

/**
 * @brief: callback for short click
 */
void short_press_handler(void) {
    verbose(VV_INFO, stdout, BLUE"[%s]"NONE" Starting new conversation!", __FUNCTION__);
    mqtt_hotword_trigger();
}

/**
 * @brief: callback for double click
 */
void double_press_handler(void) {
    if (RUN_PARA.if_disable)
        RUN_PARA.if_disable = 0;
    else
        RUN_PARA.if_disable = 1;

    if (RUN_PARA.if_disable) {
        verbose(VV_INFO, stdout, BLUE"[%s]"NONE" Disabling LEDs!", __FUNCTION__);
        RUN_PARA.if_update = 1;
        RUN_PARA.curr_state = ON_DISABLED;
    }else{
        verbose(VV_INFO, stdout, BLUE"[%s]"NONE" Enabling LEDs!", __FUNCTION__);
        RUN_PARA.if_update = 1;
        RUN_PARA.curr_state = ON_IDLE;
    }
}

/**
 * @brief: callback for ctrl+c exit
 */
void interrupt_handler(int sig) {
    RUN_PARA.if_terminate = 1;
}

/**
 * @brief: set power pin
 *
 * @returns: -1\ On Error
 *            0\ On Success
 */
int set_power_pin(void) {
    if (-1 == RUN_PARA.power.val || -1 == RUN_PARA.power.val) {
        verbose(VV_INFO, stdout, BLUE "[%s]"
            NONE " Mode has no power pin", __FUNCTION__);
        return 0;
    }

    if (-1 == cGPIO_export(RUN_PARA.power.pin))
        return -1;

    sleep(1);

    if (-1 == cGPIO_direction(RUN_PARA.power.pin, GPIO_OUT))
        return -1;

    if (-1 == cGPIO_write(RUN_PARA.power.pin, RUN_PARA.power.val))
        return -1;

    verbose(VV_INFO, stdout, BLUE "[%s]"
        NONE " Set power pin %d to "
        LIGHT_GREEN "<%s>"
        NONE, __FUNCTION__, RUN_PARA.power.pin, (RUN_PARA.power.val) ? "HIGH" : "LOW");
    return 1;
}

/**
 * @brief: release power pin
 *
 * @returns: -1\ On Error
 *            0\ On Success
 */
int reset_power_pin(void) {
    if (-1 == RUN_PARA.power.val || -1 == RUN_PARA.power.val) {
        verbose(VV_INFO, stdout, BLUE "[%s]"
            NONE " Mode has no power pin", __FUNCTION__);
        return 0;
    }

    if (-1 == cGPIO_unexport(RUN_PARA.power.pin))
        return -1;

    verbose(VV_INFO, stdout, BLUE "[%s]"
        NONE " Released power pin", __FUNCTION__);
    return 1;
}

/**
 * @brief: check nightmode (Using global running paramters)
 */
void check_nightmode(void) {
    time_t curr_time;
    struct tm * read_time = NULL;

    curr_time = time(NULL);
    read_time = localtime(&curr_time);

    if (read_time->tm_hour * 100 + read_time->tm_min >=
        RUN_PARA.sleep_hour * 100 + RUN_PARA.sleep_minute ||
        read_time->tm_hour * 100 + read_time->tm_min <=
        RUN_PARA.wake_hour * 100 + RUN_PARA.wake_minute) {
        if (ON_DISABLED != RUN_PARA.curr_state) {
            RUN_PARA.curr_state = ON_DISABLED;
            RUN_PARA.if_update = 1;
            verbose(VV_INFO, stdout, "Nightmode started");
        }
    } else {
        if (ON_DISABLED == RUN_PARA.curr_state) {
            RUN_PARA.curr_state = ON_IDLE;
            RUN_PARA.if_update = 1;
            verbose(VV_INFO, stdout, "Nightmode terminated");
        }
    }
}

/**
 * @brief: exit program, release all the resources
 *
 * @param status[in]: successful exit or faild exit
 */
void close_all(int status) {
    reset_power_pin();
    terminate_mqtt_client();
    cAPA102_Close();
    pthread_cancel(RUN_PARA.curr_thread);
    destroy_key();
    exit(status);
}

/**
 * @brief: program entrance
 */
int main(int argc, char * argv[]) {
    setVerbose(VV_INFO);
    if (-1 == load_sw_spec())
        close_all(EXIT_FAILURE);
    parse_opts(argc, argv);
    signal(SIGINT, interrupt_handler);

    if (-1 == load_hw_spec_json())
        close_all(EXIT_FAILURE);

    if (-1 == set_power_pin())
        close_all(EXIT_FAILURE);

    debug_run_para_dump();

    if (-1 == start_mqtt_client(RUN_PARA.client_id,
                                RUN_PARA.mqtt_host,
                                RUN_PARA.mqtt_port,
                                RUN_PARA.mqtt_user,
                                RUN_PARA.mqtt_pass))
        close_all(EXIT_FAILURE);

    RUN_PARA.if_mute ? mqtt_mute_feedback() : mqtt_unmute_feedback();

    if (-1 == cAPA102_Init(RUN_PARA.LEDs.number,
                           RUN_PARA.LEDs.spi_bus,
                           RUN_PARA.LEDs.spi_dev,
                           GLOBAL_BRIGHTNESS))
        close_all(EXIT_FAILURE);

    if (-1 == init_key(RUN_PARA.button.pin,
                       RUN_PARA.button.val,
                       short_press_handler,
                       double_press_handler,
                       long_press_hadler))
        close_all(EXIT_FAILURE);

    dump_running_info();

    while (1) {
        if (RUN_PARA.if_sleepmode && !RUN_PARA.if_disable)
            check_nightmode();

        if (RUN_PARA.if_update)
            state_machine_update();

        if (RUN_PARA.if_terminate)
            break;
        usleep(10000);
    }
    close_all(EXIT_SUCCESS);
    return 0;
}
