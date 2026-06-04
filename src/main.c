#include <assert.h>
#include <errno.h>
#include <soc.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/types.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>

LOG_MODULE_REGISTER(gamepad, LOG_LEVEL_DBG);

//abcd
//Halid

#if DT_NODE_HAS_STATUS(DT_ALIAS(expanderreset), okay)
static const struct gpio_dt_spec expander_reset = GPIO_DT_SPEC_GET(DT_ALIAS(expanderreset), gpios);
#endif

#define CHK(X) ({ int err = X; if (err != 0) { LOG_ERR("%s returned %d (%s:%d)", #X, err, __FILE__, __LINE__); } err == 0; })

#define REPORT_ID 3
#define REPORT_ID_IDX 0
#define REPORT_LEN 10

#define HIDS_QUEUE_SIZE 10

#define DISCONNECTED_SLEEP_TIMEOUT K_SECONDS(60)
#define CONNECTED_SLEEP_TIMEOUT K_SECONDS(600)

#define SYS_BUTTON_LONG_PRESS_MS 3000
#define SYS_BUTTON_VERY_LONG_PRESS_MS 10000

struct __attribute__((packed)) report_t {
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

static struct report_t report;
static struct report_t prev_report;

static const uint8_t dpad_lut[] = { 0x0F, 0x06, 0x02, 0x0F, 0x00, 0x07, 0x01, 0x00, 0x04, 0x05, 0x03, 0x04, 0x0F, 0x06, 0x02, 0x0F };

#define GPIO_SPEC_AND_COMMA(button) GPIO_DT_SPEC_GET(button, gpios),

static const struct gpio_dt_spec buttons[] = {
    DT_FOREACH_CHILD(DT_PATH(buttons), GPIO_SPEC_AND_COMMA)
};

BT_HIDS_DEF(hids_obj, REPORT_LEN);

K_MSGQ_DEFINE(hids_queue, REPORT_LEN, HIDS_QUEUE_SIZE, 4);

static const struct bt_data ad[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, (sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
        (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
        (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL), BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static struct bt_conn* active_conn = NULL;
static bool try_directed;

static int prev_sys_button_state = 0;
static int64_t sys_button_pressed_at;

static void sleep_work_fn(struct k_work* work) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(expanderreset), okay)
    // drive expander reset pin low
    gpio_pin_set_dt(&expander_reset, 1);
#endif
    LOG_INF("Going to sleep...");
    sys_poweroff();
}
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_fn);

static void bond_find(const struct bt_bond_info* info, void* user_data) {
    bt_addr_le_copy(user_data, &info->addr);
}

static void advertising_work_fn(struct k_work* work) {
    LOG_INF("");

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

        bt_addr_le_to_str(&addr, addr_buf, BT_ADDR_LE_STR_LEN);
        LOG_INF("Directed advertising to %s started.", addr_buf);
    } else {
        adv_param = *BT_LE_ADV_CONN;
        adv_param.options |= BT_LE_ADV_OPT_ONE_TIME;
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
    bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    if (active_conn != NULL) {
        LOG_INF("Disconnecting...");
        CHK(bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN));
    } else {
        LOG_INF("(not connected)");
        CHK(bt_le_adv_stop());
        advertising_start();
    }
}
static K_WORK_DEFINE(clear_bonds_work, clear_bonds_work_fn);

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

    active_conn = conn;

    LOG_INF("%s", addr);

    CHK(bt_hids_connected(&hids_obj, conn));

    k_work_reschedule(&sleep_work, CONNECTED_SLEEP_TIMEOUT);
}

static void disconnected(struct bt_conn* conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("%s (reason=%u)", addr, reason);

    if (conn == active_conn) {
        CHK(bt_hids_disconnected(&hids_obj, conn));
        active_conn = NULL;
    } else {
        LOG_ERR("Disconnected from a different connection than the active one?");
    }

    advertising_start();
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

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
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
//selam
static uint8_t const report_map[] = {
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

static void hid_init(void) {
    struct bt_hids_init_param hids_init_param = { 0 };
    struct bt_hids_inp_rep* hids_inp_rep;

    hids_init_param.rep_map.data = report_map;
    hids_init_param.rep_map.size = sizeof(report_map);

    hids_init_param.info.bcd_hid = 0x0101;
    hids_init_param.info.b_country_code = 0x00;
    hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE |
                                  BT_HIDS_NORMALLY_CONNECTABLE);

    hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
    hids_inp_rep->size = sizeof(struct report_t);
    hids_inp_rep->id = REPORT_ID;
    hids_init_param.inp_rep_group_init.cnt++;

    CHK(bt_hids_init(&hids_obj, &hids_init_param));
}

static void report_sent_cb(struct bt_conn* conn, void* user_data) {
    LOG_DBG("");
}

static void hids_work_fn(struct k_work* work) {
    uint8_t report[REPORT_LEN];

    while (!k_msgq_get(&hids_queue, report, K_NO_WAIT)) {
        if (active_conn != NULL) {
            k_work_reschedule(&sleep_work, CONNECTED_SLEEP_TIMEOUT);
            LOG_DBG("Sending report...");
            CHK(bt_hids_inp_rep_send(&hids_obj, active_conn, REPORT_ID_IDX, report, REPORT_LEN, report_sent_cb));
        }
    }
}
static K_WORK_DEFINE(hids_work, hids_work_fn);

static void reset_to_bootloader() {
    // https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/src/main.c#L116
    sys_reboot(0x57);
}

static void report_init() {
    memset(&report, 0, sizeof(report));
    report.dpad = 8;
    report.lx = 0x80;
    report.ly = 0x80;
    report.rx = 0x80;
    report.ry = 0x80;
    memcpy(&prev_report, &report, sizeof(report));
}

static void configure_buttons(void) {
    for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
        CHK(gpio_pin_configure_dt(&buttons[i], GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW));
    }

    CHK(gpio_pin_interrupt_configure_dt(&buttons[0], GPIO_INT_EDGE_BOTH));
}

static void handle_buttons() {
    int sys_button_state = gpio_pin_get_dt(&buttons[0]);
    int64_t now = k_uptime_get();
    if (!prev_sys_button_state && sys_button_state) {
        sys_button_pressed_at = now;
    }

    if (prev_sys_button_state && !sys_button_state) {
        int64_t duration = now - sys_button_pressed_at;
        if (duration >= SYS_BUTTON_VERY_LONG_PRESS_MS) {
            reset_to_bootloader();
        } else if (duration >= SYS_BUTTON_LONG_PRESS_MS) {
            k_work_submit(&clear_bonds_work);
        }
    }
    prev_sys_button_state = sys_button_state;

    report.menu = gpio_pin_get_dt(&buttons[1]);
    report.options = gpio_pin_get_dt(&buttons[0]);
    report.stadia = gpio_pin_get_dt(&buttons[4]);
    report.capture = gpio_pin_get_dt(&buttons[2]);
    report.l3 = gpio_pin_get_dt(&buttons[12]);
    report.r3 = gpio_pin_get_dt(&buttons[16]);
    report.x = gpio_pin_get_dt(&buttons[14]);
    report.y = gpio_pin_get_dt(&buttons[10]);
    report.r1 = gpio_pin_get_dt(&buttons[17]);
    report.l1 = gpio_pin_get_dt(&buttons[13]);
    report.a = gpio_pin_get_dt(&buttons[11]);
    report.b = gpio_pin_get_dt(&buttons[15]);
    report.r2 = gpio_pin_get_dt(&buttons[5]);
    report.r2_axis = report.r2 * 255;
    report.l2 = gpio_pin_get_dt(&buttons[3]);
    report.l2_axis = report.l2 * 255;

    int dpad = gpio_pin_get_dt(&buttons[6]) | (gpio_pin_get_dt(&buttons[8]) << 1) | (gpio_pin_get_dt(&buttons[7]) << 2) | (gpio_pin_get_dt(&buttons[9]) << 3);
    

    report.dpad = dpad_lut[dpad];

    if (memcmp(&prev_report, &report, sizeof(report))) {
        k_msgq_put(&hids_queue, &report, K_NO_WAIT);
        k_work_submit(&hids_work);
        memcpy(&prev_report, &report, sizeof(report));
    }
}

int main() {
    LOG_INF("Gamepad nRF52");

    if (!CHK(bt_conn_auth_cb_register(&conn_auth_callbacks))) {
        return 0;
    }

    if (!CHK(bt_conn_auth_info_cb_register(&conn_auth_info_callbacks))) {
        return 0;
    }

    report_init();
    hid_init();

    if (!CHK(bt_enable(NULL))) {
        return 0;
    }

    settings_load();
    advertising_start();
    configure_buttons();

    while (1) {
        k_sleep(K_MSEC(1));
        handle_buttons();
    }
}
