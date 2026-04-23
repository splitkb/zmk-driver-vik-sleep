/*
 * Copyright (c) 2026 Splitkb.com
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/event_manager.h>

/* Required to access physical hardware registers */
#ifdef CONFIG_SOC_SERIES_NRF52X
#include <soc.h>
#endif

#define DT_DRV_COMPAT splitkb_vik_sleep

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define GET_SPEC_AND_COMMA(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

#define EXTRACT_CHILD_OUTPUTS(child_node)                                                          \
    COND_CODE_1(DT_NODE_HAS_PROP(child_node, output_gpios),                                        \
                (DT_FOREACH_PROP_ELEM(child_node, output_gpios, GET_SPEC_AND_COMMA)), ())

#define EXTRACT_CHILD_INPUTS(child_node)                                                           \
    COND_CODE_1(DT_NODE_HAS_PROP(child_node, input_gpios),                                         \
                (DT_FOREACH_PROP_ELEM(child_node, input_gpios, GET_SPEC_AND_COMMA)), ())

static const struct gpio_dt_spec outputs[] = {DT_INST_FOREACH_CHILD(0, EXTRACT_CHILD_OUTPUTS)};
static const struct gpio_dt_spec inputs[] = {DT_INST_FOREACH_CHILD(0, EXTRACT_CHILD_INPUTS)};
static const struct gpio_dt_spec ctrl = GPIO_DT_SPEC_INST_GET_OR(0, control_gpios, {0});

static void disconnect_pins_for_sleep(const struct gpio_dt_spec *pins, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pins[i].port) {
            gpio_pin_interrupt_configure_dt(&pins[i], GPIO_INT_DISABLE);
            gpio_pin_configure_dt(&pins[i], GPIO_DISCONNECTED);
        }
    }
}

static int vik_force_sleep_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    
    /* We only care about the exact moment ZMK tells the board to sleep */
    if (ev == NULL || ev->state != ZMK_ACTIVITY_SLEEP) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* 1. Safely pacify the VIK pins */
    if (ctrl.port) {
        gpio_pin_interrupt_configure_dt(&ctrl, GPIO_INT_DISABLE);
        gpio_pin_configure_dt(&ctrl, GPIO_OUTPUT_INACTIVE); /* Actively kill VCC */
    }
    disconnect_pins_for_sleep(outputs, ARRAY_SIZE(outputs));
    disconnect_pins_for_sleep(inputs, ARRAY_SIZE(inputs));

    /* 2. NUCLEAR HARDWARE KILL-SWITCH */
    #ifdef CONFIG_SOC_SERIES_NRF52X
    
    /* Wait for any pending internal flash writes to finish (prevents corruption) */
    while (NRF_NVMC->READY == 0) {}

    /* Bypass Zephyr PM locks and pull the plug directly */
    NRF_POWER->SYSTEMOFF = 1;
    __DSB();
    
    /* The CPU is instantly dead. We should never reach this loop. */
    while(1); 
    
    #endif

    return ZMK_EV_EVENT_BUBBLE;
}

/* 
 * Named 'aa_' so it runs BEFORE ZMK's core power.c driver. 
 * This lets us execute our hardware kill-switch before Zephyr can block it.
 */
ZMK_LISTENER(aa_vik_sleep, vik_force_sleep_listener);
ZMK_SUBSCRIPTION(aa_vik_sleep, zmk_activity_state_changed);

static int vik_sleep_init(void) {
    if (ctrl.port && !device_is_ready(ctrl.port)) {
        return -ENODEV;
    }
    if (ctrl.port) {
        gpio_pin_configure_dt(&ctrl, GPIO_OUTPUT_ACTIVE);
    }
    return 0;
}

SYS_INIT(vik_sleep_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
