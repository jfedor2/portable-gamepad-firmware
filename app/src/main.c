// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <soc.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/hash_function.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/types.h>

#ifdef CONFIG_USBD_HID_SUPPORT
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/usbd.h>
#endif

#ifdef CONFIG_BT
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <bluetooth/radio_notification_cb.h>
#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#endif

#include "bootloader.h"

LOG_MODULE_REGISTER(pgf, LOG_LEVEL_DBG);

static const struct gpio_dt_spec sys_button = GPIO_DT_SPEC_GET(DT_ALIAS(sys_button), gpios);

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

#define PM_DEVICE_RUNTIME_GET(node_id, prop, idx) CHK(pm_device_runtime_get(DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))));
#define PM_DEVICE_RUNTIME_PUT(node_id, prop, idx) CHK(pm_device_runtime_put(DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx))));

#define REPORT_ID_IDX 0

#define DISCONNECTED_SLEEP_TIMEOUT K_SECONDS(60)
#define CONNECTED_SLEEP_TIMEOUT K_SECONDS(600)

#define SYS_BUTTON_LONG_PRESS_MS 3000
#define SYS_BUTTON_VERY_LONG_PRESS_MS 10000

#ifdef CONFIG_BT

#define SUPERVISION_TIMEOUT_10MS 100
#define RADIO_NOTIFICATION_DISTANCE_US DT_PROP_OR(DT_PATH(zephyr_user), radio_notification_distance_us, 500)

#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
#define CONNECTION_INTERVAL_MIN_US DT_PROP_OR(DT_PATH(zephyr_user), connection_interval_min_us, 1000)
#define IDLE_TIMEOUT K_SECONDS(60)
#define IDLE_SUBRATE_FACTOR 10
#endif

#endif

static volatile bool keep_going = true;

enum InputMode {
    INPUT_MODE_UNKNOWN = 0,
    INPUT_MODE_STADIA = 1,
    INPUT_MODE_SWITCH = 2,
    INPUT_MODE_PC = 3,
};

static enum InputMode input_mode = INPUT_MODE_UNKNOWN;

#ifdef CONFIG_USBD_HID_SUPPORT
const uint8_t* wired_report_descriptor;
size_t wired_report_descriptor_length;
size_t wired_report_size;
uint8_t wired_report_id;
#endif

#ifdef CONFIG_BT
const uint8_t* bluetooth_report_map;
size_t bluetooth_report_map_length;
size_t bluetooth_report_size;
uint8_t bluetooth_report_id;
#endif

#define CONFIG_VERSION 1

struct __attribute__((packed)) config_t {
    uint8_t config_version;
    uint8_t input_mode;
};

static struct config_t config;

#define SETTINGS_KEY "pgf/config"

static int load_config_cb(const char* key, size_t len, settings_read_cb read_cb, void* cb_arg, void* param) {
    LOG_INF("");

    struct config_t* my_config = (struct config_t*) param;
    my_config->config_version = 0;

    if (((key != NULL) && (key[0] != '\0')) ||
        (len != sizeof(struct config_t))) {
        return 0;
    }

    int bytes_read = read_cb(cb_arg, my_config, len);

    if (bytes_read != sizeof(struct config_t)) {
        my_config->config_version = 0;
        return 0;
    }

    return 0;
}

enum LedMode {
    LED_OFF = 0,
    LED_ON = 1,
    LED_CONNECTED = 2,
    LED_ADVERTISING = 3,
    LED_PAIRING = 4,
};

static atomic_t led_mode = (atomic_t) ATOMIC_INIT(LED_OFF);
static bool led_next_blink_state = true;

#if DT_NODE_HAS_STATUS(DT_ALIAS(status_led), okay)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
#endif

static inline void set_status_led(bool state) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(status_led), okay)
    gpio_pin_set_dt(&status_led, state);
#endif
}

static void configure_leds() {
#if DT_NODE_HAS_STATUS(DT_ALIAS(status_led), okay)
    if (device_is_ready(status_led.port)) {
        CHK(gpio_pin_configure_dt(&status_led, GPIO_OUTPUT));
        set_status_led(false);
    } else {
        LOG_ERR("status_led device %s not ready", status_led.port->name);
    }
#endif
}

static void led_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_fn);

static void led_work_fn(struct k_work* work) {
    enum LedMode my_led_mode = (enum LedMode) atomic_get(&led_mode);
    int next_work = 0;
    switch (my_led_mode) {
        case LED_OFF:
            set_status_led(false);
            break;
        case LED_ON:
            set_status_led(true);
            break;
        case LED_ADVERTISING:
            set_status_led(led_next_blink_state);
            led_next_blink_state = !led_next_blink_state;
            next_work = led_next_blink_state ? 200 : 1800;
            break;
        case LED_CONNECTED:
            set_status_led(led_next_blink_state);
            led_next_blink_state = !led_next_blink_state;
            next_work = led_next_blink_state ? 1900 : 100;
            break;
        case LED_PAIRING:
            set_status_led(led_next_blink_state);
            led_next_blink_state = !led_next_blink_state;
            next_work = 100;
            break;
    }
    if (next_work > 0) {
        k_work_reschedule(&led_work, K_MSEC(next_work));
    }
}

static void set_led_mode(enum LedMode led_mode_) {
    enum LedMode previous = atomic_set(&led_mode, (atomic_val_t) led_mode_);
    if (previous != (atomic_val_t) led_mode_) {
        LOG_INF("led mode %d -> %d", previous, led_mode_);
        k_work_reschedule(&led_work, K_NO_WAIT);
    }
}

struct __attribute__((packed)) report_stadia_t {
    uint8_t dpad;
    uint8_t capture : 1;
    uint8_t assistant : 1;
    uint8_t l2 : 1;
    uint8_t r2 : 1;
    uint8_t stadia : 1;
    uint8_t menu : 1;
    uint8_t options : 1;
    uint8_t r3 : 1;
    uint8_t l3 : 1;
    uint8_t r1 : 1;
    uint8_t l1 : 1;
    uint8_t y : 1;
    uint8_t x : 1;
    uint8_t b : 1;
    uint8_t a : 1;
    uint8_t padding : 1;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t l2_axis;
    uint8_t r2_axis;
    uint8_t extra_buttons;
};

BUILD_ASSERT(sizeof(struct report_stadia_t) == 10, "wrong Stadia report size");

struct __attribute__((packed)) report_switch_t {
    uint8_t y : 1;
    uint8_t b : 1;
    uint8_t a : 1;
    uint8_t x : 1;
    uint8_t l : 1;
    uint8_t r : 1;
    uint8_t zl : 1;
    uint8_t zr : 1;
    uint8_t minus : 1;
    uint8_t plus : 1;
    uint8_t ls : 1;
    uint8_t rs : 1;
    uint8_t home : 1;
    uint8_t capture : 1;
    uint8_t padding1 : 2;
    uint8_t dpad : 4;
    uint8_t padding2 : 4;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t padding3;
};

BUILD_ASSERT(sizeof(struct report_switch_t) == 8, "wrong Switch report size");

struct __attribute__((packed)) report_xusb_t {
    uint8_t type;
    uint8_t length;
    uint8_t dpad_up : 1;
    uint8_t dpad_down : 1;
    uint8_t dpad_left : 1;
    uint8_t dpad_right : 1;
    uint8_t start : 1;
    uint8_t back : 1;
    uint8_t lsb : 1;
    uint8_t rsb : 1;
    uint8_t lb : 1;
    uint8_t rb : 1;
    uint8_t guide : 1;
    uint8_t padding1 : 1;
    uint8_t a : 1;
    uint8_t b : 1;
    uint8_t x : 1;
    uint8_t y : 1;
    uint8_t lt;
    uint8_t rt;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
    uint8_t padding2[6];
};

BUILD_ASSERT(sizeof(struct report_xusb_t) == 20, "wrong XUSB report size");

struct __attribute__((packed)) report_xbox_t {
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
    uint16_t lt;
    uint16_t rt;
    uint8_t dpad;
    uint8_t a : 1;
    uint8_t b : 1;
    uint8_t padding1 : 1;
    uint8_t x : 1;
    uint8_t y : 1;
    uint8_t padding2 : 1;
    uint8_t l1 : 1;
    uint8_t r1 : 1;
    uint8_t padding3 : 1;
    uint8_t padding4 : 1;
    uint8_t view : 1;
    uint8_t menu : 1;
    uint8_t guide : 1;
    uint8_t lsb : 1;
    uint8_t rsb : 1;
    uint8_t padding5 : 1;
    uint8_t capture : 1;
    uint8_t padding6 : 7;
};

BUILD_ASSERT(sizeof(struct report_xbox_t) == 16, "wrong Xbox report size");

#define MAX_REPORT_SIZE MAX_FROM_LIST(sizeof(struct report_stadia_t), sizeof(struct report_switch_t), sizeof(struct report_xusb_t), sizeof(struct report_xbox_t))

#ifdef CONFIG_USBD_HID_SUPPORT
static uint8_t wired_report[MAX_REPORT_SIZE];
static uint8_t prev_wired_report[MAX_REPORT_SIZE];
#endif

#ifdef CONFIG_BT
static uint8_t bluetooth_report[MAX_REPORT_SIZE];
static uint8_t prev_bluetooth_report[MAX_REPORT_SIZE];
#endif

static const uint8_t dpad_lut[] = { 0x0F, 0x06, 0x02, 0x0F, 0x00, 0x07, 0x01, 0x00, 0x04, 0x05, 0x03, 0x04, 0x0F, 0x06, 0x02, 0x0F };

static const uint8_t dpad_lut_xbox[] = { 0x00, 0x07, 0x03, 0x00, 0x01, 0x08, 0x02, 0x01, 0x05, 0x06, 0x04, 0x05, 0x00, 0x07, 0x03, 0x00 };

#define BUTTON(name) UTIL_CAT(button_, name)

#define BUTTON_FOR_ID(node_id) BUTTON(DT_NODE_FULL_NAME_TOKEN(node_id))

enum button_idx {
    DT_FOREACH_CHILD_SEP(DT_PATH(gamepad_buttons), BUTTON_FOR_ID, (, )),
    NUM_BUTTONS
};

#define BUTTON_GPIO_DEF(node_id) GPIO_DT_SPEC_GET(node_id, gpios),

static struct gpio_dt_spec buttons[] = {
    DT_FOREACH_CHILD(DT_PATH(gamepad_buttons), BUTTON_GPIO_DEF)
};

static gpio_port_value_t* button_port_states[NUM_BUTTONS];

#define _COUNT_GPIO_CONTROLLER(node_id) IF_ENABLED(DT_NODE_HAS_PROP(node_id, gpio_controller), (+1))
#define NUM_GPIO_PORTS (0 DT_FOREACH_STATUS_OKAY_NODE(_COUNT_GPIO_CONTROLLER))

static const struct device* gpio_ports[NUM_GPIO_PORTS];
static gpio_port_value_t gpio_port_states[NUM_GPIO_PORTS];

static int active_gpio_ports = 0;

static int prev_sys_button_state = 0;
static int64_t sys_button_pressed_at;
static bool sys_button_very_long_press_handled = false;

static bool usb_ready = false;

static void sleep_work_fn(struct k_work* work) {
    if (usb_ready) {
        LOG_INF("USB connected, not sleeping.");
        return;
    }

    keep_going = false;
}
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_fn);

#ifdef CONFIG_BT

enum ConnState {
    STATE_UNKNOWN = 0,
    STATE_NOK = 1,
    STATE_OK = 2,
};

enum ConnStep {
    STEP_INITIAL = 0,
    STEP_LEGACY_PARAMS,
    STEP_LEGACY_ACCOMPLISHED,
    STEP_LEGACY_GIVEN_UP,
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
    STEP_REMOTE_SUPPORTS_SCI,
    STEP_PHY,
    STEP_FRAME_SPACE,
    STEP_CONN_RATE,
    STEP_SCI_ACCOMPLISHED,
    STEP_SCI_GIVEN_UP,
    STEP_SUBRATE_FACTOR,
#endif
};

struct conn_state_t {
    enum ConnStep step;
    int16_t retries_left;
    enum ConnState legacy_params;
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
    enum ConnState remote_supports_sci;
    enum ConnState phy;
    enum ConnState frame_space;
    enum ConnState conn_rate;
    enum ConnState subrate_factor;
    uint32_t requested_interval_us;
    uint32_t interval_us;
    uint16_t latency;
    uint16_t desired_subrate_factor;
    bool idle;
#endif
};

struct conn_state_t conn_state;

static inline void reset_conn_state() {
    conn_state = (struct conn_state_t){ 0 };
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
    conn_state.desired_subrate_factor = 1;
#endif
}

BT_HIDS_DEF(hids_obj, MAX_REPORT_SIZE);

static K_SEM_DEFINE(bt_event_sem, 0, 1);

char bt_name[CONFIG_BT_DEVICE_NAME_MAX + 1];

// flags take 3 bytes, UUID takes 4 bytes, appearance takes 4 bytes, name has a 2-byte header
// max packet size is 31, 31-3-4-4-2=18
// we append 5 characters to the name in set_bt_name()

BUILD_ASSERT((sizeof(CONFIG_PGF_PRODUCT_NAME) + 5 - 1) <= 18, "CONFIG_PGF_PRODUCT_NAME too long");

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
        (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
        (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
    BT_DATA(BT_DATA_NAME_COMPLETE,
        bt_name,
        MIN(sizeof(CONFIG_PGF_PRODUCT_NAME) + 5 - 1, CONFIG_BT_DEVICE_NAME_MAX)),
};

// We have our own DIS implementation to be able to change VID/PID at runtime.

struct __attribute__((packed)) dis_pnp_t {
    uint8_t vid_src;
    uint16_t vid;
    uint16_t pid;
    uint16_t ver;
};

static struct dis_pnp_t dis_pnp_id = {
    .vid_src = 0x02,
    .vid = 0x0000,
    .pid = 0x0000,
    .ver = 0x0100,
};

static ssize_t read_str(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data, strlen(attr->user_data));
}

static ssize_t read_pnp_id(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &dis_pnp_id, sizeof(dis_pnp_id));
}

BT_GATT_SERVICE_DEFINE(
    dis_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MODEL_NUMBER, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_str, NULL, CONFIG_SOC),
    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MANUFACTURER_NAME, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_str, NULL, CONFIG_PGF_MANUFACTURER_NAME),
    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_PNP_ID, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_pnp_id, NULL, NULL), );

static struct bt_conn* active_conn = NULL;
static struct k_spinlock conn_lock;
static bool try_directed;

static inline struct bt_conn* get_active_conn() {
    k_spinlock_key_t key = k_spin_lock(&conn_lock);
    struct bt_conn* conn = active_conn ? bt_conn_ref(active_conn) : NULL;
    k_spin_unlock(&conn_lock, key);
    return conn;
}

static inline void release_conn(struct bt_conn* conn) {
    if (conn != NULL) {
        bt_conn_unref(conn);
    }
}

static void bond_find(const struct bt_bond_info* info, void* user_data) {
    bt_addr_le_copy(user_data, &info->addr);
}

static void advertising_work_fn(struct k_work* work) {
    LOG_INF("");
    if (usb_ready) {
        LOG_INF("USB connected, not starting advertising.");
        return;
    }

    struct bt_le_adv_param adv_param;

    char addr_buf[BT_ADDR_LE_STR_LEN];

    bt_addr_le_t addr;
    bt_addr_le_copy(&addr, BT_ADDR_LE_NONE);
    bt_foreach_bond(BT_ID_DEFAULT, bond_find, &addr);

    if (try_directed && !bt_addr_le_eq(&addr, BT_ADDR_LE_NONE)) {
        adv_param = *BT_LE_ADV_CONN_DIR(&addr);
        //        adv_param = *BT_LE_ADV_CONN_DIR_LOW_DUTY(&addr);
        adv_param.options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;

        if (!CHK(bt_le_adv_start(&adv_param, NULL, 0, NULL, 0))) {
            return;
        }

        set_led_mode(LED_ADVERTISING);

        bt_addr_le_to_str(&addr, addr_buf, BT_ADDR_LE_STR_LEN);
        LOG_INF("Directed advertising to %s started.", addr_buf);
    } else {
        adv_param = *BT_LE_ADV_CONN_FAST_2;
        if (!bt_addr_le_eq(&addr, BT_ADDR_LE_NONE)) {
            bt_addr_le_to_str(&addr, addr_buf, BT_ADDR_LE_STR_LEN);
            LOG_INF("Enabling filter: %s", addr_buf);
            adv_param.options |= BT_LE_ADV_OPT_FILTER_CONN;
            adv_param.options |= BT_LE_ADV_OPT_FILTER_SCAN_REQ;
            bt_le_filter_accept_list_clear();
            bt_le_filter_accept_list_add(&addr);
        } else {
            LOG_INF("Not enabling filter.");
        }

        if (!CHK(bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0))) {
            return;
        }

        if (!bt_addr_le_eq(&addr, BT_ADDR_LE_NONE)) {
            set_led_mode(LED_ADVERTISING);
        } else {
            set_led_mode(LED_PAIRING);
        }

        LOG_INF("Regular advertising started.");
    }
}
static K_WORK_DEFINE(advertising_work, advertising_work_fn);

static void advertising_start(void) {
    //    try_directed = true;
    try_directed = false;
    k_work_submit(&advertising_work);
    k_work_reschedule(&sleep_work, DISCONNECTED_SLEEP_TIMEOUT);
}

static void advertising_restart(void) {
    try_directed = false;
    k_work_submit(&advertising_work);
}

static void clear_bonds_work_fn(struct k_work* work) {
    LOG_INF("");
    struct bt_conn* conn = get_active_conn();
    bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    if (conn != NULL) {
        LOG_INF("Disconnecting...");
        CHK(bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN));
    } else {
        LOG_INF("(not connected)");
        CHK(bt_le_adv_stop());
        advertising_start();
    }
    release_conn(conn);
}
static K_WORK_DEFINE(clear_bonds_work, clear_bonds_work_fn);

static void conn_state_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(conn_state_work, conn_state_work_fn);

static inline void conn_state_work_if(enum ConnStep step) {
    if (conn_state.step == step) {
        k_work_reschedule(&conn_state_work, K_NO_WAIT);
    }
}

#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS

static void idle_work_fn(struct k_work* work) {
    LOG_INF("");
    conn_state.idle = true;
    conn_state_work_if(STEP_SCI_ACCOMPLISHED);
}
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_work_fn);

static inline void leave_idle(struct bt_conn* conn) {
    LOG_INF("");
    conn_state.idle = false;
    conn_state_work_if(STEP_SCI_ACCOMPLISHED);
}

#endif

static void connected(struct bt_conn* conn, uint8_t err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        if (err == BT_HCI_ERR_ADV_TIMEOUT) {
            LOG_INF("Directed advertising to %s timed out.", addr);
            advertising_restart();
        } else {
            LOG_ERR("Failed to connect to %s (%u).", addr, err);
        }
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&conn_lock);
    active_conn = bt_conn_ref(conn);
    k_spin_unlock(&conn_lock, key);

    reset_conn_state();

    LOG_INF("%s", addr);

    struct bt_conn_info info;
    if (CHK(bt_conn_get_info(conn, &info))) {
        if (info.type == BT_CONN_TYPE_LE) {
            LOG_INF("interval_us=%u, latency=%u, timeout=%u", info.le.interval_us, info.le.latency, info.le.timeout);
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
            conn_state.interval_us = info.le.interval_us;
#endif
        }
    }

    CHK(bt_hids_connected(&hids_obj, conn));

    set_led_mode(LED_CONNECTED);
    k_work_reschedule(&sleep_work, CONNECTED_SLEEP_TIMEOUT);
    k_work_reschedule(&conn_state_work, K_MSEC(500));
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
    k_work_reschedule(&idle_work, IDLE_TIMEOUT);
#endif
}

static void disconnected(struct bt_conn* conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("%s (reason=%u)", addr, reason);

    struct bt_conn* conn_to_unref = NULL;

    k_spinlock_key_t key = k_spin_lock(&conn_lock);
    if (conn == active_conn) {
        conn_to_unref = active_conn;
        active_conn = NULL;
    }
    k_spin_unlock(&conn_lock, key);

    if (conn_to_unref != NULL) {
        k_work_cancel_delayable(&conn_state_work);
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
        k_work_cancel_delayable(&idle_work);
#endif
        CHK(bt_hids_disconnected(&hids_obj, conn_to_unref));
        bt_conn_unref(conn_to_unref);
    } else {
        LOG_ERR("Disconnected from a different connection than the active one?");
    }

    if (keep_going) {
        advertising_start();
    }
}

static void security_changed(struct bt_conn* conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_INF("%s, level=%u", addr, level);
    } else {
        LOG_ERR("Security failed: %s, level=%u, err=%d\n", addr, level, err);
    }
}

#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS

static void conn_rate_request(struct bt_conn* conn) {
    LOG_INF("");

    static uint16_t our_min_interval_us = 0;

    CHK(bt_conn_le_read_min_conn_interval(&our_min_interval_us));

    LOG_INF("min interval supported=%d us, configured=%d us", our_min_interval_us, CONNECTION_INTERVAL_MIN_US);

    if (CONNECTION_INTERVAL_MIN_US > our_min_interval_us) {
        our_min_interval_us = CONNECTION_INTERVAL_MIN_US;
    }

    conn_state.requested_interval_us = our_min_interval_us;

    const struct bt_conn_le_conn_rate_param params = {
        .interval_min_125us = our_min_interval_us / 125,
        .interval_max_125us = 7500 / 125,
        .subrate_min = 1,
        .subrate_max = 1,
        .max_latency = 0,
        .continuation_number = 0,
        .supervision_timeout_10ms = SUPERVISION_TIMEOUT_10MS,
        .min_ce_len_125us = BT_HCI_LE_SCI_CE_LEN_MIN_125US,
        .max_ce_len_125us = BT_HCI_LE_SCI_CE_LEN_MAX_125US,
    };

    CHK(bt_conn_le_conn_rate_request(conn, &params));
}

static void update_to_2m_phy(struct bt_conn* conn) {
    LOG_INF("");
    struct bt_conn_le_phy_param phy;

    phy.options = BT_CONN_LE_PHY_OPT_NONE;
    phy.pref_rx_phy = BT_GAP_LE_PHY_2M;
    phy.pref_tx_phy = BT_GAP_LE_PHY_2M;

    CHK(bt_conn_le_phy_update(conn, &phy));
}

static void select_lowest_frame_space(struct bt_conn* conn) {
    LOG_INF("");
    const struct bt_conn_le_frame_space_update_param params = {
        .phys = BT_HCI_LE_FRAME_SPACE_UPDATE_PHY_2M_MASK,
        .spacing_types = BT_CONN_LE_FRAME_SPACE_TYPES_MASK_ACL_IFS,
        .frame_space_min = 0,
        .frame_space_max = 150,
    };

    CHK(bt_conn_le_frame_space_update(conn, &params));
}

static void subrate_factor_request(struct bt_conn* conn) {
    LOG_INF("");

    struct bt_conn_le_subrate_param params = {
        .subrate_min = conn_state.desired_subrate_factor,
        .subrate_max = conn_state.desired_subrate_factor,
        .max_latency = 0,
        .continuation_number = 0,
        .supervision_timeout = SUPERVISION_TIMEOUT_10MS,
    };

    CHK(bt_conn_le_subrate_request(conn, &params));
}

static void le_phy_updated(struct bt_conn* conn, struct bt_conn_le_phy_info* param) {
    LOG_INF("TX PHY %d, RX PHY %d", param->tx_phy, param->rx_phy);
    if ((param->tx_phy == BT_GAP_LE_PHY_2M) && (param->rx_phy == BT_GAP_LE_PHY_2M)) {
        conn_state.phy = STATE_OK;
        conn_state_work_if(STEP_PHY);
    } else {
        conn_state.phy = STATE_NOK;
    }
}

static void frame_space_updated(struct bt_conn* conn, const struct bt_conn_le_frame_space_updated* params) {
    if (params->status == BT_HCI_ERR_SUCCESS) {
        LOG_INF("%u us, PHYs: 0x%02x, spacing types: 0x%04x",
            params->frame_space,
            params->phys,
            params->spacing_types);
        // let's just assume what we got is good
        conn_state.frame_space = STATE_OK;
        conn_state_work_if(STEP_FRAME_SPACE);
    } else {
        LOG_WRN("failed (HCI status 0x%02x %s)", params->status, bt_hci_err_to_str(params->status));
    }
}

static void subrate_changed(struct bt_conn* conn, const struct bt_conn_le_subrate_changed* params) {
    if (params->status == BT_HCI_ERR_SUCCESS) {
        LOG_INF("factor %d, continuation_number %d, peripheral_latency %d, supervision_timeout %d", params->factor, params->continuation_number, params->peripheral_latency, params->supervision_timeout);
        if (params->factor == conn_state.desired_subrate_factor) {
            conn_state.subrate_factor = STATE_OK;
            conn_state_work_if(STEP_SUBRATE_FACTOR);
        }
    } else {
        LOG_WRN("failed (HCI status 0x%02x %s)", params->status, bt_hci_err_to_str(params->status));
    }
}

static void conn_rate_changed(struct bt_conn* conn, uint8_t status, const struct bt_conn_le_conn_rate_changed* params) {
    if (status == BT_HCI_ERR_SUCCESS) {
        LOG_INF(
            "interval %u us, "
            "subrate factor %d, "
            "peripheral latency %d, "
            "continuation number %d, "
            "supervision timeout %d ms",
            params->interval_us, params->subrate_factor, params->peripheral_latency,
            params->continuation_number, params->supervision_timeout_10ms * 10);
        conn_state.interval_us = params->interval_us;
        if (params->interval_us == conn_state.requested_interval_us) {
            conn_state.conn_rate = STATE_OK;
            conn_state_work_if(STEP_CONN_RATE);
        }
    } else {
        LOG_WRN("failed (HCI status 0x%02x %s)", status, bt_hci_err_to_str(status));
    }
}

static void read_all_remote_feat_complete(struct bt_conn* conn, const struct bt_conn_le_read_all_remote_feat_complete* params) {
    LOG_INF("SCI %d SCI_host_supp %d", !!BT_FEAT_LE_SHORTER_CONN_INTERVALS(params->features), !!BT_FEAT_LE_SHORTER_CONN_INTERVALS_HOST_SUPP(params->features));

    conn_state.remote_supports_sci =
        BT_FEAT_LE_SHORTER_CONN_INTERVALS(params->features) &&
                BT_FEAT_LE_SHORTER_CONN_INTERVALS_HOST_SUPP(params->features)
            ? STATE_OK
            : STATE_NOK;

    conn_state_work_if(STEP_REMOTE_SUPPORTS_SCI);
}

#endif

static void request_legacy_params(struct bt_conn* conn) {
    LOG_INF("");

    struct bt_le_conn_param conn_param = {
        .interval_min = 6,
        .interval_max = 6,
        .latency = 0,
        .timeout = SUPERVISION_TIMEOUT_10MS,
    };

    CHK(bt_conn_le_param_update(conn, &conn_param));
}

static k_timeout_t conn_step_next_think;

static inline void next_step(enum ConnStep step, k_timeout_t delay, int16_t retries_left) {
    conn_step_next_think = delay;
    conn_state.retries_left = retries_left;
    conn_state.step = step;
}

static inline bool should_retry_step() {
    if (conn_state.retries_left > 0) {
        conn_state.retries_left--;
        return true;
    }
    return false;
}

static void conn_state_work_fn(struct k_work* work) {
    struct bt_conn* conn = get_active_conn();
    if (conn == NULL) {
        return;
    }

    conn_step_next_think = K_NO_WAIT;

    while (K_TIMEOUT_EQ(conn_step_next_think, K_NO_WAIT)) {
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
        LOG_INF("conn_step=%d retries_left=%d RS=%d PH=%d FS=%d CR=%d SF=%d LP=%d", conn_state.step, conn_state.retries_left, conn_state.remote_supports_sci, conn_state.phy, conn_state.frame_space, conn_state.conn_rate, conn_state.subrate_factor, conn_state.legacy_params);
#else
        LOG_INF("conn_step=%d retries_left=%d LP=%d", conn_state.step, conn_state.retries_left, conn_state.legacy_params);
#endif
        switch (conn_state.step) {
            case STEP_INITIAL:
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
                next_step(STEP_REMOTE_SUPPORTS_SCI, K_NO_WAIT, 3);
#else
                next_step(STEP_LEGACY_PARAMS, K_NO_WAIT, 3);
#endif
                break;
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
            case STEP_REMOTE_SUPPORTS_SCI:
                if (conn_state.remote_supports_sci == STATE_OK) {
                    next_step(STEP_PHY, K_NO_WAIT, 3);
                } else if (conn_state.remote_supports_sci == STATE_NOK) {
                    next_step(STEP_LEGACY_PARAMS, K_NO_WAIT, 3);
                } else if (should_retry_step()) {
                    CHK(bt_conn_le_read_all_remote_features(conn, 2));
                    conn_step_next_think = K_MSEC(2000);
                } else {
                    next_step(STEP_LEGACY_PARAMS, K_NO_WAIT, 3);
                }
                break;
            case STEP_PHY:
                if (conn_state.phy == STATE_OK) {
                    next_step(STEP_FRAME_SPACE, K_NO_WAIT, 3);
                } else if (should_retry_step()) {
                    update_to_2m_phy(conn);
                    conn_step_next_think = K_MSEC(2000);
                } else {
                    next_step(STEP_SCI_GIVEN_UP, K_NO_WAIT, 0);
                }
                break;
            case STEP_FRAME_SPACE:
                if (conn_state.frame_space == STATE_OK) {
                    next_step(STEP_CONN_RATE, K_NO_WAIT, 3);
                } else if (should_retry_step()) {
                    select_lowest_frame_space(conn);
                    conn_step_next_think = K_MSEC(2000);
                } else {
                    next_step(STEP_SCI_GIVEN_UP, K_NO_WAIT, 0);
                }
                break;
            case STEP_CONN_RATE:
                if (conn_state.conn_rate == STATE_OK) {
                    next_step(STEP_SCI_ACCOMPLISHED, K_NO_WAIT, 0);
                } else if (should_retry_step()) {
                    conn_rate_request(conn);
                    conn_step_next_think = K_MSEC(2000);
                } else {
                    next_step(STEP_SCI_GIVEN_UP, K_NO_WAIT, 0);
                }
                break;
            case STEP_SCI_ACCOMPLISHED:
                if ((conn_state.idle && (conn_state.desired_subrate_factor != IDLE_SUBRATE_FACTOR)) ||
                    (!conn_state.idle && (conn_state.desired_subrate_factor != 1))) {
                    conn_state.desired_subrate_factor = conn_state.idle ? IDLE_SUBRATE_FACTOR : 1;
                    conn_state.subrate_factor = STATE_UNKNOWN;
                    next_step(STEP_SUBRATE_FACTOR, K_NO_WAIT, 3);
                } else {
                    LOG_INF("SCI params accomplished");
                    conn_step_next_think = K_FOREVER;
                }
                break;
            case STEP_SCI_GIVEN_UP:
                // we didn't get what we wanted but perhaps it's still no worse than legacy
                if ((conn_state.interval_us <= 7500) && (conn_state.latency == 0)) {
                    LOG_INF("%" PRIu32 "us, eh, good enough", conn_state.interval_us);
                    next_step(STEP_SCI_ACCOMPLISHED, K_NO_WAIT, 0);
                } else {
                    next_step(STEP_LEGACY_PARAMS, K_NO_WAIT, 3);
                }
                break;
            case STEP_SUBRATE_FACTOR:
                if (conn_state.subrate_factor == STATE_OK) {
                    next_step(STEP_SCI_ACCOMPLISHED, K_NO_WAIT, 0);
                } else if (should_retry_step()) {
                    subrate_factor_request(conn);
                    conn_step_next_think = K_MSEC(500);
                } else {
                    next_step(STEP_SCI_ACCOMPLISHED, K_NO_WAIT, 0);
                }
                break;
#endif
            case STEP_LEGACY_PARAMS:
                if (conn_state.legacy_params == STATE_OK) {
                    next_step(STEP_LEGACY_ACCOMPLISHED, K_NO_WAIT, 0);
                } else if (should_retry_step()) {
                    request_legacy_params(conn);
                    conn_step_next_think = K_MSEC(5000);
                } else {
                    next_step(STEP_LEGACY_GIVEN_UP, K_NO_WAIT, 0);
                }
                break;
            case STEP_LEGACY_ACCOMPLISHED:
                LOG_INF("Legacy params accomplished");
                conn_step_next_think = K_FOREVER;
                break;
            case STEP_LEGACY_GIVEN_UP:
                LOG_INF("Failed to accomplish legacy params");
                conn_step_next_think = K_FOREVER;
                break;
        }
    }

    if (!K_TIMEOUT_EQ(conn_step_next_think, K_FOREVER)) {
        k_work_reschedule(&conn_state_work, conn_step_next_think);
    }

    release_conn(conn);
}

static void le_param_updated(struct bt_conn* conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
    LOG_INF("interval=%u, latency=%u, timeout=%u", interval, latency, timeout);
    if ((interval == 6) && (latency == 0)) {
        conn_state.legacy_params = STATE_OK;
        conn_state_work_if(STEP_LEGACY_PARAMS);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
    .le_phy_updated = le_phy_updated,
    .subrate_changed = subrate_changed,
    .conn_rate_changed = conn_rate_changed,
    .frame_space_updated = frame_space_updated,
    .read_all_remote_feat_complete = read_all_remote_feat_complete,
#endif
    .security_changed = security_changed,
};

static void auth_cancel(struct bt_conn* conn) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("%s", addr);
}

static void pairing_complete(struct bt_conn* conn, bool bonded) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("%s, bonded=%d", addr, bonded);
}

static void pairing_failed(struct bt_conn* conn, enum bt_security_err reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_ERR("%s, reason=%d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed
};

const char id_chars[33] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

static void set_bt_name() {
    uint8_t device_id[16];
    ssize_t length;
    uint32_t hash = 0;

    length = hwinfo_get_device_id(device_id, sizeof(device_id));
    if (length > 0) {
        hash = sys_hash32(device_id, length);
    } else {
        LOG_ERR("hwinfo_get_device_id returned %d", length);
    }

    snprintf(bt_name, sizeof(bt_name), "%s %c%c%c%c",
        CONFIG_PGF_PRODUCT_NAME,
        id_chars[(hash >> 0) & 0x1F],
        id_chars[(hash >> 5) & 0x1F],
        id_chars[(hash >> 10) & 0x1F],
        id_chars[(hash >> 15) & 0x1F]);

    LOG_INF("%s", bt_name);

    bt_set_name(bt_name);
}

static void hid_init(void) {
    struct bt_hids_init_param hids_init_param = { 0 };
    struct bt_hids_inp_rep* hids_inp_rep;

    hids_init_param.rep_map.data = bluetooth_report_map;
    hids_init_param.rep_map.size = bluetooth_report_map_length;

    hids_init_param.info.bcd_hid = 0x0101;
    hids_init_param.info.b_country_code = 0x00;
    hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE |
                                  BT_HIDS_NORMALLY_CONNECTABLE);

    hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
    hids_inp_rep->size = bluetooth_report_size;
    hids_inp_rep->id = bluetooth_report_id;
    hids_init_param.inp_rep_group_init.cnt++;

    CHK(bt_hids_init(&hids_obj, &hids_init_param));
}

static void report_sent_cb(struct bt_conn* conn, void* user_data) {
    LOG_DBG("");
}

static void radio_notification_conn_cb(struct bt_conn* conn) {
    k_sem_give(&bt_event_sem);
}

static const struct bt_radio_notification_conn_cb radio_notification_callbacks = {
    .prepare = radio_notification_conn_cb,
};

#endif  // CONFIG_BT

static uint8_t const report_descriptor_stadia[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x25, 0x07,        //   Logical Maximum (7)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
    0x09, 0x39,        //   Usage (Hat switch)
    0x81, 0x42,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x45, 0x00,        //   Physical Maximum (0)
    0x65, 0x00,        //   Unit (None)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x09,        //   Usage Page (Button)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0F,        //   Report Count (15)
    0x09, 0x12,        //   Usage (0x12)
    0x09, 0x11,        //   Usage (0x11)
    0x09, 0x14,        //   Usage (0x14)
    0x09, 0x13,        //   Usage (0x13)
    0x09, 0x0D,        //   Usage (0x0D)
    0x09, 0x0C,        //   Usage (0x0C)
    0x09, 0x0B,        //   Usage (0x0B)
    0x09, 0x0F,        //   Usage (0x0F)
    0x09, 0x0E,        //   Usage (0x0E)
    0x09, 0x08,        //   Usage (0x08)
    0x09, 0x07,        //   Usage (0x07)
    0x09, 0x05,        //   Usage (0x05)
    0x09, 0x04,        //   Usage (0x04)
    0x09, 0x02,        //   Usage (0x02)
    0x09, 0x01,        //   Usage (0x01)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x15, 0x01,        //   Logical Minimum (1)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              //   End Collection
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x32,        //     Usage (Z)
    0x09, 0x35,        //     Usage (Rz)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              //   End Collection
    0x05, 0x02,        //   Usage Page (Sim Ctrls)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x02,        //   Report Count (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x09, 0xC5,        //   Usage (Brake)
    0x09, 0xC4,        //   Usage (Accelerator)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x0C,        //   Usage Page (Consumer)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x09, 0xE9,        //   Usage (Volume Increment)
    0x09, 0xEA,        //   Usage (Volume Decrement)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x09, 0xCD,        //   Usage (Play/Pause)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x05,        //   Report Count (5)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)

    /*
        0x85, 0x05,                    //   Report ID (5)
        0x06, 0x0F, 0x00,              //   Usage Page (PID Page)
        0x09, 0x97,                    //   Usage (0x97)
        0x75, 0x10,                    //   Report Size (16)
        0x95, 0x02,                    //   Report Count (2)
        0x27, 0xFF, 0xFF, 0x00, 0x00,  //   Logical Maximum (65535)
        0x91, 0x02,                    //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    */

    0xC0,  // End Collection
};

static uint8_t const report_descriptor_switch[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x35, 0x00,        //   Physical Minimum (0)
    0x45, 0x01,        //   Physical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0E,        //   Report Count (14)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0E,        //   Usage Maximum (0x0E)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x25, 0x07,        //   Logical Maximum (7)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
    0x09, 0x39,        //   Usage (Hat switch)
    0x81, 0x42,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00,        //   Unit (None)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x46, 0xFF, 0x00,  //   Physical Maximum (255)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection
};

static uint8_t const report_descriptor_xbox[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,                    // Usage (Game Pad)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x01,                    //   Report ID (1)
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x95, 0x02,                    //     Report Count (2)
    0x75, 0x10,                    //     Report Size (16)
    0x81, 0x02,                    //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,                          //   End Collection
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x32,                    //     Usage (Z)
    0x09, 0x35,                    //     Usage (Rz)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x95, 0x02,                    //     Report Count (2)
    0x75, 0x10,                    //     Report Size (16)
    0x81, 0x02,                    //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,                          //   End Collection
    0x05, 0x02,                    //   Usage Page (Sim Ctrls)
    0x09, 0xC5,                    //   Usage (Brake)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (1023)
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x0A,                    //   Report Size (10)
    0x81, 0x02,                    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x02,                    //   Usage Page (Sim Ctrls)
    0x09, 0xC4,                    //   Usage (Accelerator)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (1023)
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x0A,                    //   Report Size (10)
    0x81, 0x02,                    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,                    //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39,                    //   Usage (Hat switch)
    0x15, 0x01,                    //   Logical Minimum (1)
    0x25, 0x08,                    //   Logical Maximum (8)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x46, 0x3B, 0x01,              //   Physical Maximum (315)
    0x66, 0x14, 0x00,              //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x42,                    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x45, 0x00,                    //   Physical Maximum (0)
    0x65, 0x00,                    //   Unit (None)
    0x81, 0x03,                    //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01,                    //   Usage Minimum (0x01)
    0x29, 0x0F,                    //   Usage Maximum (0x0F)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x0F,                    //   Report Count (15)
    0x81, 0x02,                    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x0C,                    //   Usage Page (Consumer)
    0x0A, 0xB2, 0x00,              //   Usage (Record)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x01,                    //   Report Size (1)
    0x81, 0x02,                    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x07,                    //   Report Size (7)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x0F,                    //   Usage Page (PID Page)
    0x09, 0x21,                    //   Usage (0x21)
    0x85, 0x03,                    //   Report ID (3)
    0xA1, 0x02,                    //   Collection (Logical)
    0x09, 0x97,                    //     Usage (0x97)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x75, 0x04,                    //     Report Size (4)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x00,                    //     Logical Maximum (0)
    0x75, 0x04,                    //     Report Size (4)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x03,                    //     Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x09, 0x70,                    //     Usage (0x70)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x64,                    //     Logical Maximum (100)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x04,                    //     Report Count (4)
    0x91, 0x02,                    //     Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x09, 0x50,                    //     Usage (0x50)
    0x66, 0x01, 0x10,              //     Unit (System: SI Linear, Time: Seconds)
    0x55, 0x0E,                    //     Unit Exponent (-2)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x26, 0xFF, 0x00,              //     Logical Maximum (255)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x09, 0xA7,                    //     Usage (0xA7)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x26, 0xFF, 0x00,              //     Logical Maximum (255)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x65, 0x00,                    //     Unit (None)
    0x55, 0x00,                    //     Unit Exponent (0)
    0x09, 0x7C,                    //     Usage (0x7C)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x26, 0xFF, 0x00,              //     Logical Maximum (255)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0,                          //   End Collection
    0xC0,                          // End Collection
};

static void report_init_stadia(uint8_t* report) {
    struct report_stadia_t* report_ = (struct report_stadia_t*) report;
    report_->dpad = dpad_lut[0];
    report_->lx = 0x80;
    report_->ly = 0x80;
    report_->rx = 0x80;
    report_->ry = 0x80;
}

static void report_init_switch(uint8_t* report) {
    struct report_switch_t* report_ = (struct report_switch_t*) report;
    report_->dpad = dpad_lut[0];
    report_->lx = 0x80;
    report_->ly = 0x80;
    report_->rx = 0x80;
    report_->ry = 0x80;
}

static void report_init_xusb(uint8_t* report) {
    struct report_xusb_t* report_ = (struct report_xusb_t*) report;
    report_->length = 20;
}

static void report_init_xbox(uint8_t* report) {
    struct report_xbox_t* report_ = (struct report_xbox_t*) report;
    report_->dpad = dpad_lut_xbox[0];
    report_->lx = 0x8000;
    report_->ly = 0x8000;
    report_->rx = 0x8000;
    report_->ry = 0x8000;
}

static void report_init() {
#ifdef CONFIG_USBD_HID_SUPPORT
    memset(wired_report, 0, wired_report_size);
#endif
#ifdef CONFIG_BT
    memset(bluetooth_report, 0, bluetooth_report_size);
#endif
    switch (input_mode) {
        case INPUT_MODE_STADIA: {
#ifdef CONFIG_USBD_HID_SUPPORT
            report_init_stadia(wired_report);
#endif
#ifdef CONFIG_BT
            report_init_stadia(bluetooth_report);
#endif
            break;
        }
        case INPUT_MODE_SWITCH: {
#ifdef CONFIG_USBD_HID_SUPPORT
            report_init_switch(wired_report);
#endif
#ifdef CONFIG_BT
            report_init_switch(bluetooth_report);
#endif
            break;
        }
        case INPUT_MODE_PC: {
#ifdef CONFIG_USBD_HID_SUPPORT
            report_init_xusb(wired_report);
#endif
#ifdef CONFIG_BT
            report_init_xbox(bluetooth_report);
#endif
            break;
        }
        default:
            break;
    }
#ifdef CONFIG_USBD_HID_SUPPORT
    memcpy(prev_wired_report, wired_report, wired_report_size);
#endif
#ifdef CONFIG_BT
    memcpy(prev_bluetooth_report, bluetooth_report, bluetooth_report_size);
#endif
}

#ifdef CONFIG_USBD_HID_SUPPORT

static K_SEM_DEFINE(hid_report_sem, 1, 1);

const struct device* hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);

USBD_DEVICE_DEFINE(context, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x0000, 0x0000);

USBD_DESC_LANG_DEFINE(desc_lang);
USBD_DESC_MANUFACTURER_DEFINE(desc_manufacturer, CONFIG_PGF_MANUFACTURER_NAME);
USBD_DESC_PRODUCT_DEFINE(desc_product, CONFIG_PGF_PRODUCT_NAME);
USBD_DESC_SERIAL_NUMBER_DEFINE(desc_serial_number);

static const uint8_t attributes = 0;

USBD_CONFIGURATION_DEFINE(fs_config, attributes, 50, NULL);  // 50*2 mA = 100 mA

USBD_CONFIGURATION_DEFINE(hs_config, attributes, 50, NULL);  // 50*2 mA = 100 mA

UDC_STATIC_BUF_DEFINE(usb_report, MAX_REPORT_SIZE + 1);

static void set_usb_ready(const bool ready) {
    LOG_INF("%d", ready);
    usb_ready = ready;
#ifdef CONFIG_BT
    struct bt_conn* conn = get_active_conn();
    if (usb_ready) {
        set_led_mode(LED_OFF);
        if (conn != NULL) {
            LOG_INF("Disconnecting...");
            CHK(bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN));
        } else {
            LOG_INF("(not connected)");
            CHK(bt_le_adv_stop());
        }
    } else {
        advertising_start();
    }

    release_conn(conn);
#endif
}

static void iface_ready(const struct device* dev, const bool ready) {
    set_usb_ready(ready);
}

static int get_report(const struct device* dev,
    const uint8_t type,
    const uint8_t id,
    const uint16_t len,
    uint8_t* const buf) {
    LOG_INF("type=%u, id=%u", type, id);

    return 0;
}

static int set_report(const struct device* dev, const uint8_t type, const uint8_t id, const uint16_t len, const uint8_t* const buf) {
    LOG_HEXDUMP_DBG(buf, len, "");
    return 0;
}

static void output_report(const struct device* dev, const uint16_t len, const uint8_t* const buf) {
    LOG_HEXDUMP_DBG(buf, len, "");
}

static void input_report_done(const struct device* dev, const uint8_t* const report) {
    k_sem_give(&hid_report_sem);
}

static struct hid_device_ops ops = {
    .iface_ready = iface_ready,
    .get_report = get_report,
    .set_report = set_report,
    .output_report = output_report,
    .input_report_done = input_report_done,
};

static void usbd_msg_cb(struct usbd_context* const ctx, const struct usbd_msg* msg) {
    switch (msg->type) {
        case USBD_MSG_VBUS_READY:
            LOG_INF("USBD_MSG_VBUS_READY");
            CHK(usbd_enable(ctx));
            break;
        case USBD_MSG_VBUS_REMOVED:
            LOG_INF("USBD_MSG_VBUS_REMOVED");
            CHK(usbd_disable(ctx));
            break;
        default:
            break;
    }
}

// XUSB

#define XUSB_EP_IN_MPS 20
#define XUSB_EP_OUT_MPS 8

static struct usb_if_descriptor xusb_desc_if0 = {
    .bLength = sizeof(struct usb_if_descriptor),
    .bDescriptorType = USB_DESC_INTERFACE,
    .bInterfaceNumber = 0,  // will be patched by Zephyr
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = 0xFF,
    .bInterfaceSubClass = 0x5D,
    .bInterfaceProtocol = 0x01,
    .iInterface = 0,
};

#define XUSB_PROPRIETARY_DESCRIPTOR_TYPE 0x21

static struct usb_xusb_proprietary_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bData[15];
} __packed xusb_desc_proprietary = {
    .bLength = 17,
    .bDescriptorType = XUSB_PROPRIETARY_DESCRIPTOR_TYPE,
    .bData = { 0x00, 0x01, 0x01, 0x25, 0x81 /* will be patched by us */, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01 /* will be patched by us */, 0x08, 0x00, 0x00 },
};

static struct usb_ep_descriptor xusb_desc_if0_in_ep = {
    .bLength = sizeof(struct usb_ep_descriptor),
    .bDescriptorType = USB_DESC_ENDPOINT,
    .bEndpointAddress = 0x81,  // will be patched by Zephyr
    .bmAttributes = USB_EP_TYPE_INTERRUPT,
    .wMaxPacketSize = sys_cpu_to_le16(32),
    .bInterval = 1,
};

static struct usb_ep_descriptor xusb_desc_if0_out_ep = {
    .bLength = sizeof(struct usb_ep_descriptor),
    .bDescriptorType = USB_DESC_ENDPOINT,
    .bEndpointAddress = 0x01,  // will be patched by Zephyr
    .bmAttributes = USB_EP_TYPE_INTERRUPT,
    .wMaxPacketSize = sys_cpu_to_le16(32),
    .bInterval = 1,
};

static const struct usb_desc_header* xusb_fs_desc[] = {
    (struct usb_desc_header*) &xusb_desc_if0,
    (struct usb_desc_header*) &xusb_desc_proprietary,
    (struct usb_desc_header*) &xusb_desc_if0_in_ep,
    (struct usb_desc_header*) &xusb_desc_if0_out_ep,
    NULL,
};

static void* xusb_get_desc(struct usbd_class_data* const c_data, const enum usbd_speed speed) {
    return (void*) xusb_fs_desc;
}

static int xusb_request(struct usbd_class_data* const c_data, struct net_buf* buf, int err) {
    struct udc_buf_info* bi = udc_get_buf_info(buf);

    if (err) {
        usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
        return 0;
    }

    if (bi->ep == xusb_desc_if0_out_ep.bEndpointAddress) {
        LOG_HEXDUMP_DBG(buf->data, buf->len, "");

        if (buf->len > 0) {
            // ignore rumble for now
        }

        net_buf_reset(buf);
        usbd_ep_enqueue(c_data, buf);
    } else {
        // IN transfer complete, free buffer
        usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
        k_sem_give(&hid_report_sem);
    }
    return 0;
}

static int xusb_control_to_host(struct usbd_class_data* const c_data, const struct usb_setup_packet* setup, struct net_buf* buf) {
    LOG_INF("");
    uint16_t wValue = sys_le16_to_cpu(setup->wValue);
    uint16_t wLength = sys_le16_to_cpu(setup->wLength);

    if (setup->bRequest == USB_SREQ_GET_DESCRIPTOR) {
        uint8_t desc_type = wValue >> 8;

        if (desc_type == XUSB_PROPRIETARY_DESCRIPTOR_TYPE) {
            size_t len = MIN(wLength, sizeof(xusb_desc_proprietary));
            net_buf_add_mem(buf, &xusb_desc_proprietary, len);
            return 0;
        }
    }

    errno = -ENOTSUP;
    return 0;
}

static int xusb_control_to_dev(struct usbd_class_data* const c_data, const struct usb_setup_packet* setup, const struct net_buf* buf) {
    LOG_INF("");
    errno = -ENOTSUP;
    return 0;
}

static void xusb_enable(struct usbd_class_data* const c_data) {
    set_usb_ready(true);

    struct net_buf* buf = usbd_ep_buf_alloc(c_data, xusb_desc_if0_out_ep.bEndpointAddress, XUSB_EP_OUT_MPS);
    if (buf) {
        if (!CHK(usbd_ep_enqueue(c_data, buf))) {
            usbd_ep_buf_free(usbd_class_get_ctx(c_data), buf);
        }
    }
}

static void xusb_disable(struct usbd_class_data* const c_data) {
    set_usb_ready(false);
}

static int xusb_init(struct usbd_class_data* const c_data) {
    LOG_INF("xusb if=%d ep_in=0x%02x ep_out=0x%02x", xusb_desc_if0.bInterfaceNumber, xusb_desc_if0_in_ep.bEndpointAddress, xusb_desc_if0_out_ep.bEndpointAddress);

    xusb_desc_proprietary.bData[4] = xusb_desc_if0_in_ep.bEndpointAddress;
    xusb_desc_proprietary.bData[11] = xusb_desc_if0_out_ep.bEndpointAddress;

    return 0;
}

struct usbd_class_api xusb_api = {
    .request = xusb_request,
    .get_desc = xusb_get_desc,
    .control_to_host = xusb_control_to_host,
    .control_to_dev = xusb_control_to_dev,
    .init = xusb_init,
    .enable = xusb_enable,
    .disable = xusb_disable,
};

USBD_DEFINE_CLASS(xusb, &xusb_api, NULL, NULL);

static bool initialize_usb() {
    if (input_mode != INPUT_MODE_PC) {
        if (!device_is_ready(hid_dev)) {
            LOG_ERR("hid_dev not ready");
            return false;
        }

        if (!CHK(hid_device_register(hid_dev, wired_report_descriptor, wired_report_descriptor_length, &ops))) {
            return false;
        }
    }

    if (!CHK(usbd_add_descriptor(&context, &desc_lang))) {
        return false;
    }

    if (!CHK(usbd_add_descriptor(&context, &desc_manufacturer))) {
        return false;
    }

    if (!CHK(usbd_add_descriptor(&context, &desc_product))) {
        return false;
    }

    if (!CHK(usbd_add_descriptor(&context, &desc_serial_number))) {
        return false;
    }

    if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&context) == USBD_SPEED_HS) {
        if (!CHK(usbd_add_configuration(&context, USBD_SPEED_HS, &hs_config))) {
            return false;
        }

        if (!CHK(usbd_register_class(&context, (input_mode == INPUT_MODE_PC) ? "xusb" : "hid_0", USBD_SPEED_HS, 1))) {
            return false;
        }

        usbd_device_set_code_triple(&context, USBD_SPEED_HS, 0, 0, 0);
    }

    if (!CHK(usbd_add_configuration(&context, USBD_SPEED_FS, &fs_config))) {
        return false;
    }

    if (!CHK(usbd_register_class(&context, (input_mode == INPUT_MODE_PC) ? "xusb" : "hid_0", USBD_SPEED_FS, 1))) {
        return false;
    }

    usbd_device_set_code_triple(&context, USBD_SPEED_FS, 0, 0, 0);

    usbd_self_powered(&context, false);

    if (!CHK(usbd_msg_register_cb(&context, usbd_msg_cb))) {
        return false;
    }

    if (!CHK(usbd_init(&context))) {
        return false;
    }

    if (!usbd_can_detect_vbus(&context)) {
        if (!CHK(usbd_enable(&context))) {
            return false;
        }
    }

    return true;
}

#endif  // CONFIG_USBD_HID_SUPPORT

static void configure_buttons(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        CHK(gpio_pin_configure_dt(&buttons[i], GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW));
        int16_t port_idx = -1;

        for (uint8_t j = 0; j < active_gpio_ports; j++) {
            if (gpio_ports[j] == buttons[i].port) {
                port_idx = j;
                break;
            }
        }

        if (port_idx == -1) {
            port_idx = active_gpio_ports;
            gpio_ports[port_idx] = buttons[i].port;
            active_gpio_ports++;
        }

        button_port_states[i] = &gpio_port_states[port_idx];
    }

    LOG_INF("active_gpio_ports=%d", active_gpio_ports);

    // sys_button is probably one of the gamepad buttons we just configured,
    // but configure it anyway in case it's not.
    CHK(gpio_pin_configure_dt(&sys_button, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW));
}

#define BUTTON_GET(name)                                                                                        \
    COND_CODE_1(DT_NODE_EXISTS(DT_PATH(gamepad_buttons, name)),                                                 \
        ((0 != (*button_port_states[BUTTON(name)] & BIT(DT_GPIO_PIN(DT_PATH(gamepad_buttons, name), gpios))))), \
        (0))

static void read_all_gpio_ports() {
    for (uint8_t i = 0; i < active_gpio_ports; i++) {
        CHK(gpio_port_get(gpio_ports[i], &gpio_port_states[i]));
    }
}

static void handle_buttons() {
    int sys_button_state = gpio_pin_get_dt(&sys_button);
    int64_t now = k_uptime_get();
    if (!prev_sys_button_state && sys_button_state) {
        sys_button_pressed_at = now;
        sys_button_very_long_press_handled = false;
    }

    if (sys_button_state && !sys_button_very_long_press_handled) {
        int64_t duration = now - sys_button_pressed_at;
        if (duration >= SYS_BUTTON_VERY_LONG_PRESS_MS) {
            sys_button_very_long_press_handled = true;
            if (usb_ready) {
                set_status_led(false);
                if (reset_to_bootloader) {
                    reset_to_bootloader();
                }
            } else {
#ifdef CONFIG_BT
                k_work_submit(&clear_bonds_work);
#endif
            }
        } else if (duration > SYS_BUTTON_LONG_PRESS_MS) {
            if (!usb_ready) {
                set_led_mode(LED_ON);
            }
        }
    }

    if (prev_sys_button_state && !sys_button_state) {
        int64_t duration = now - sys_button_pressed_at;
        if (!usb_ready && !sys_button_very_long_press_handled &&
            (duration >= SYS_BUTTON_LONG_PRESS_MS)) {
            k_work_reschedule(&sleep_work, K_NO_WAIT);
        }
    }
    prev_sys_button_state = sys_button_state;

    read_all_gpio_ports();
}

static void fill_out_report(uint8_t* report, bool wired) {
    switch (input_mode) {
        case INPUT_MODE_STADIA: {
            struct report_stadia_t* report_ = (struct report_stadia_t*) report;

            report_->menu = BUTTON_GET(start);
            report_->options = BUTTON_GET(select);
            report_->stadia = BUTTON_GET(home);
            report_->capture = BUTTON_GET(button14);
            report_->l3 = BUTTON_GET(l3);
            report_->r3 = BUTTON_GET(r3);
            report_->x = BUTTON_GET(west);
            report_->y = BUTTON_GET(north);
            report_->r1 = BUTTON_GET(r1);
            report_->l1 = BUTTON_GET(l1);
            report_->a = BUTTON_GET(south);
            report_->b = BUTTON_GET(east);
            report_->r2 = BUTTON_GET(r2);
            report_->r2_axis = report_->r2 * 255;
            report_->l2 = BUTTON_GET(l2);
            report_->l2_axis = report_->l2 * 255;

            int dpad = BUTTON_GET(dpad_left) | (BUTTON_GET(dpad_right) << 1) | (BUTTON_GET(dpad_up) << 2) | (BUTTON_GET(dpad_down) << 3);

            report_->dpad = dpad_lut[dpad];

            break;
        }
        case INPUT_MODE_SWITCH: {
            struct report_switch_t* report_ = (struct report_switch_t*) report;

            report_->b = BUTTON_GET(south);
            report_->a = BUTTON_GET(east);
            report_->y = BUTTON_GET(west);
            report_->x = BUTTON_GET(north);
            report_->minus = BUTTON_GET(select);
            report_->plus = BUTTON_GET(start);
            report_->home = BUTTON_GET(home);
            report_->capture = BUTTON_GET(button14);
            report_->l = BUTTON_GET(l1);
            report_->r = BUTTON_GET(r1);
            report_->zl = BUTTON_GET(l2);
            report_->zr = BUTTON_GET(r2);
            report_->ls = BUTTON_GET(l3);
            report_->rs = BUTTON_GET(r3);

            int dpad = BUTTON_GET(dpad_left) | (BUTTON_GET(dpad_right) << 1) | (BUTTON_GET(dpad_up) << 2) | (BUTTON_GET(dpad_down) << 3);

            report_->dpad = dpad_lut[dpad];

            break;
        }
        case INPUT_MODE_PC: {
            if (wired) {
#ifdef CONFIG_USBD_HID_SUPPORT
                struct report_xusb_t* report_ = (struct report_xusb_t*) report;
                report_->dpad_left = BUTTON_GET(dpad_left);
                report_->dpad_right = BUTTON_GET(dpad_right);
                report_->dpad_up = BUTTON_GET(dpad_up);
                report_->dpad_down = BUTTON_GET(dpad_down);
                report_->a = BUTTON_GET(south);
                report_->b = BUTTON_GET(east);
                report_->x = BUTTON_GET(west);
                report_->y = BUTTON_GET(north);
                report_->back = BUTTON_GET(select);
                report_->start = BUTTON_GET(start);
                report_->guide = BUTTON_GET(home);
                report_->lb = BUTTON_GET(l1);
                report_->rb = BUTTON_GET(r1);
                report_->lsb = BUTTON_GET(l3);
                report_->rsb = BUTTON_GET(r3);
                report_->lt = BUTTON_GET(l2) * 255;
                report_->rt = BUTTON_GET(r2) * 255;
#endif
            } else {
#ifdef CONFIG_BT
                struct report_xbox_t* report_ = (struct report_xbox_t*) report;
                report_->a = BUTTON_GET(south);
                report_->b = BUTTON_GET(east);
                report_->x = BUTTON_GET(west);
                report_->y = BUTTON_GET(north);
                report_->l1 = BUTTON_GET(l1);
                report_->r1 = BUTTON_GET(r1);
                report_->lsb = BUTTON_GET(l3);
                report_->rsb = BUTTON_GET(r3);
                report_->view = BUTTON_GET(select);
                report_->menu = BUTTON_GET(start);
                report_->guide = BUTTON_GET(home);
                report_->capture = BUTTON_GET(button14);
                report_->lt = BUTTON_GET(l2) * 1023;
                report_->rt = BUTTON_GET(r2) * 1023;

                int dpad = BUTTON_GET(dpad_left) | (BUTTON_GET(dpad_right) << 1) | (BUTTON_GET(dpad_up) << 2) | (BUTTON_GET(dpad_down) << 3);

                report_->dpad = dpad_lut_xbox[dpad];
#endif
            }
            break;
        }
        default:
            break;
    }
}

static void determine_input_mode() {
#ifdef CONFIG_PGF_INPUT_MODE_DYNAMIC
    if (CHK(settings_load_subtree_direct(SETTINGS_KEY, load_config_cb, &config))) {
        if (config.config_version == CONFIG_VERSION) {
            input_mode = config.input_mode;
            LOG_INF("saved input mode: %d", input_mode);
        }
    }

    read_all_gpio_ports();

    if (BUTTON_GET(south)) {
        input_mode = INPUT_MODE_STADIA;
    } else if (BUTTON_GET(east)) {
        input_mode = INPUT_MODE_SWITCH;
    } else if (BUTTON_GET(west)) {
        input_mode = INPUT_MODE_PC;
    }
#endif

    if (input_mode == INPUT_MODE_UNKNOWN) {
        input_mode = CONFIG_PGF_INPUT_MODE_DEFAULT;
    }

#ifdef CONFIG_PGF_INPUT_MODE_DYNAMIC
    config.config_version = CONFIG_VERSION;
    config.input_mode = input_mode;

    LOG_INF("final input_mode: %d", input_mode);

    CHK(settings_save_one(SETTINGS_KEY, &config, sizeof(struct config_t)));
#endif

    switch (input_mode) {
        case INPUT_MODE_STADIA:
#ifdef CONFIG_USBD_HID_SUPPORT
            usbd_device_set_vid(&context, 0x18D1);
            usbd_device_set_pid(&context, 0x9400);
            wired_report_descriptor = report_descriptor_stadia;
            wired_report_descriptor_length = sizeof(report_descriptor_stadia);
            wired_report_id = 3;
            wired_report_size = sizeof(struct report_stadia_t);
#endif
#ifdef CONFIG_BT
            dis_pnp_id.vid = 0x18D1;
            dis_pnp_id.pid = 0x9400;
            bluetooth_report_map = report_descriptor_stadia;
            bluetooth_report_map_length = sizeof(report_descriptor_stadia);
            bluetooth_report_id = 3;
            bluetooth_report_size = sizeof(struct report_stadia_t);
#endif
            break;
        case INPUT_MODE_SWITCH:
#ifdef CONFIG_USBD_HID_SUPPORT
            usbd_device_set_vid(&context, 0x0F0D);
            usbd_device_set_pid(&context, 0x00C1);
            wired_report_descriptor = report_descriptor_switch;
            wired_report_descriptor_length = sizeof(report_descriptor_switch);
            wired_report_id = 0;
            wired_report_size = sizeof(struct report_switch_t);
#endif
#ifdef CONFIG_BT
            dis_pnp_id.vid = 0x0F0D;
            dis_pnp_id.pid = 0x00C1;
            bluetooth_report_map = report_descriptor_switch;
            bluetooth_report_map_length = sizeof(report_descriptor_switch);
            bluetooth_report_id = 0;
            bluetooth_report_size = sizeof(struct report_switch_t);
#endif
            break;
        case INPUT_MODE_PC:
#ifdef CONFIG_USBD_HID_SUPPORT
            usbd_device_set_vid(&context, 0x045E);
            usbd_device_set_pid(&context, 0x028E);
            wired_report_size = sizeof(struct report_xusb_t);
#endif
#ifdef CONFIG_BT
            dis_pnp_id.vid = 0x045E;
            dis_pnp_id.pid = 0x0B13;
            bluetooth_report_map = report_descriptor_xbox;
            bluetooth_report_map_length = sizeof(report_descriptor_xbox);
            bluetooth_report_id = 1;
            bluetooth_report_size = sizeof(struct report_xbox_t);
#endif
            break;
        default:
            break;
    }
}

int main() {
    LOG_INF("Portable Gamepad Firmware");

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), keep_awake_devices) && defined(CONFIG_PM_DEVICE_RUNTIME)
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), keep_awake_devices, PM_DEVICE_RUNTIME_GET)
#endif

    CHK(settings_subsys_init());
    configure_buttons();
    determine_input_mode();
    report_init();

#ifdef CONFIG_BT
    if (!CHK(bt_conn_auth_cb_register(&conn_auth_callbacks))) {
        return 0;
    }

    if (!CHK(bt_conn_auth_info_cb_register(&conn_auth_info_callbacks))) {
        return 0;
    }

    hid_init();
#endif

#ifdef CONFIG_USBD_HID_SUPPORT
    if (!initialize_usb()) {
        LOG_ERR("initialize_usb() failed");
        return 0;
    }
#endif

    configure_leds();

#ifdef CONFIG_BT
    if (!CHK(bt_enable(NULL))) {
        return 0;
    }

    CHK(bt_radio_notification_conn_cb_register(&radio_notification_callbacks, RADIO_NOTIFICATION_DISTANCE_US));

    settings_load();
    set_bt_name();
    advertising_start();
#endif

    while (keep_going) {
        // this makes logging work, but potentially stops us from achieving max polling rate
        // k_sleep(K_USEC(1));
#ifdef CONFIG_BT
        if (!usb_ready) {
            k_sem_take(&bt_event_sem, K_USEC(10000));
        }
#endif
        handle_buttons();

        if (usb_ready) {
#ifdef CONFIG_USBD_HID_SUPPORT
            fill_out_report(wired_report, true);
            if (memcmp(prev_wired_report, wired_report, wired_report_size)) {
                if (!k_sem_take(&hid_report_sem, K_NO_WAIT)) {
                    if (input_mode == INPUT_MODE_PC) {
                        struct net_buf* xbuf = usbd_ep_buf_alloc(&xusb, xusb_desc_if0_in_ep.bEndpointAddress, wired_report_size);
                        if (xbuf) {
                            net_buf_add_mem(xbuf, wired_report, wired_report_size);
                            if (CHK(usbd_ep_enqueue(&xusb, xbuf))) {
                                memcpy(prev_wired_report, wired_report, wired_report_size);
                            } else {
                                usbd_ep_buf_free(usbd_class_get_ctx(&xusb), xbuf);
                                k_sem_give(&hid_report_sem);
                            }
                        } else {
                            k_sem_give(&hid_report_sem);
                        }
                    } else {
                        uint8_t offset = 0;
                        if (wired_report_id != 0) {
                            usb_report[0] = wired_report_id;
                            offset = 1;
                        }
                        memcpy(usb_report + offset, wired_report, wired_report_size);
                        if (CHK(hid_device_submit_report(hid_dev, wired_report_size + offset, usb_report))) {
                            memcpy(prev_wired_report, wired_report, wired_report_size);
                        } else {
                            k_sem_give(&hid_report_sem);
                        }
                    }
                }
            }
#endif
        } else {
#ifdef CONFIG_BT
            fill_out_report(bluetooth_report, false);
            if (memcmp(prev_bluetooth_report, bluetooth_report, bluetooth_report_size)) {
                struct bt_conn* conn = get_active_conn();
                if (conn != NULL) {
                    LOG_DBG("Sending report...");
                    if (CHK(bt_hids_inp_rep_send(&hids_obj, conn, REPORT_ID_IDX, bluetooth_report, bluetooth_report_size, report_sent_cb))) {
                        memcpy(prev_bluetooth_report, bluetooth_report, bluetooth_report_size);
                    }
#ifdef CONFIG_BT_SHORTER_CONNECTION_INTERVALS
                    // struct k_work_sync work_sync;
                    // k_work_cancel_delayable_sync(&idle_work, &work_sync);
                    if (conn_state.idle) {
                        leave_idle(conn);
                    }
                    k_work_reschedule(&idle_work, IDLE_TIMEOUT);
#endif
                    k_work_reschedule(&sleep_work, CONNECTED_SLEEP_TIMEOUT);
                }
                release_conn(conn);
            }
#endif
        }
    }

#ifdef CONFIG_BT
    struct bt_conn* conn = get_active_conn();
    if (conn != NULL) {
        CHK(bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN));
    } else {
        CHK(bt_le_adv_stop());
    }
    release_conn(conn);

    CHK(bt_disable());
#endif

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), keep_awake_devices) && defined(CONFIG_PM_DEVICE_RUNTIME)
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), keep_awake_devices, PM_DEVICE_RUNTIME_PUT)
#endif

    k_sleep(K_MSEC(100));

    set_status_led(false);
    CHK(gpio_pin_interrupt_configure_dt(&sys_button, GPIO_INT_LEVEL_ACTIVE));

    LOG_INF("Going to sleep...");
    LOG_PANIC();
#ifdef CONFIG_POWEROFF
    sys_poweroff();
#endif
}
