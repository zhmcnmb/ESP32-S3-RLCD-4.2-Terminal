#include "wifi_setup.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "sys_state.h"

/*
 * 状态机与命令解释：页面只投递 ENTER/NEXT/PREV/CONFIRM/BACK，
 * 同一命令在不同状态下含义不同(如NEXT在列表是下移、在密码页是候选后滚)。
 * CONNECTING 不响应任何命令，避免中止后凭据半新半旧；在密码页确认"完成"前
 * 不触碰WiFi配置，此前的逐层BACK退出都无副作用。
 * 提交过新凭据(s_credentials_touched)后放弃配网时恢复旧凭据，防止长期断网。
 */

static const char *TAG = "wifi_setup";

#define SCAN_AP_MAX        20 /* 驱动返回上限，排序后取前SYS_WIFI_AP_MAX个 */
#define CONNECT_TIMEOUT_MS 15000
#define CMD_POLL_MS        100

static sys_wifi_setup_view_t s_view;
static TaskHandle_t s_task;
static TickType_t s_connect_deadline;
static bool s_credentials_touched; /* 已提交新凭据但尚未连接成功 */

static void push_view(void)
{
    sys_state_apply_wifi_setup(&s_view);
}

static void set_state(sys_wifi_setup_state_t state)
{
    s_view.state = state;
    push_view();
    ESP_LOGI(TAG, "state -> %d", state);
}

/* 退回OFF：提交过新凭据则先恢复旧凭据(无快照属正常，不告警) */
static void exit_to_off(void)
{
    if (s_credentials_touched) {
        s_credentials_touched = false;
        esp_err_t err = net_credentials_restore();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "恢复旧凭据失败: %s", esp_err_to_name(err));
        }
    }
    set_state(SYS_WIFI_SETUP_OFF);
}

/* 事件回调只通知任务，AP记录拷贝与排序都在任务上下文完成(规范第7节) */
static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static esp_err_t start_scan(void)
{
    s_view.ap_count = 0;
    s_view.ap_sel = 0;
    set_state(SYS_WIFI_SETUP_SCANNING);
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        set_state(SYS_WIFI_SETUP_OFF);
    }
    return err;
}

/* 按信号降序插入排序取前SYS_WIFI_AP_MAX个；扫描量小，简单排序足够 */
static void collect_scan_results(void)
{
    uint16_t ap_num = SCAN_AP_MAX;
    wifi_ap_record_t records[SCAN_AP_MAX];
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) {
        ESP_LOGW(TAG, "扫描无结果");
        set_state(SYS_WIFI_SETUP_AP_LIST);
        return;
    }
    if (ap_num > SCAN_AP_MAX) {
        ap_num = SCAN_AP_MAX;
    }
    if (esp_wifi_scan_get_ap_records(&ap_num, records) != ESP_OK) {
        ESP_LOGW(TAG, "读取扫描结果失败");
        set_state(SYS_WIFI_SETUP_AP_LIST);
        return;
    }
    for (int i = 1; i < ap_num; i++) {
        wifi_ap_record_t cur = records[i];
        int j = i - 1;
        while (j >= 0 && records[j].rssi < cur.rssi) {
            records[j + 1] = records[j];
            j--;
        }
        records[j + 1] = cur;
    }
    int count = ap_num < SYS_WIFI_AP_MAX ? ap_num : SYS_WIFI_AP_MAX;
    for (int i = 0; i < count; i++) {
        strncpy(s_view.aps[i].ssid, (char *)records[i].ssid, sizeof(s_view.aps[i].ssid) - 1);
        s_view.aps[i].ssid[sizeof(s_view.aps[i].ssid) - 1] = '\0';
        s_view.aps[i].rssi = records[i].rssi;
    }
    s_view.ap_count = count;
    s_view.ap_sel = 0;
    ESP_LOGI(TAG, "扫描到 %d 个AP", count);
    set_state(SYS_WIFI_SETUP_AP_LIST);
}

static void cmd_in_off(sys_wifi_cmd_t cmd)
{
    if (cmd == SYS_WIFI_CMD_ENTER) {
        start_scan();
    }
}

static void cmd_in_list(sys_wifi_cmd_t cmd)
{
    if (cmd == SYS_WIFI_CMD_NEXT && s_view.ap_count > 0) {
        s_view.ap_sel = (s_view.ap_sel + 1) % s_view.ap_count;
        push_view();
    } else if (cmd == SYS_WIFI_CMD_PREV && s_view.ap_count > 0) {
        s_view.ap_sel = (s_view.ap_sel - 1 + s_view.ap_count) % s_view.ap_count;
        push_view();
    } else if (cmd == SYS_WIFI_CMD_CONFIRM && s_view.ap_count > 0) {
        strncpy(s_view.ssid, s_view.aps[s_view.ap_sel].ssid, sizeof(s_view.ssid) - 1);
        s_view.ssid[sizeof(s_view.ssid) - 1] = '\0';
        s_view.pwd_len = 0;
        s_view.pwd[0] = '\0';
        s_view.pwd_candidate = 0; /* 从'0'开始，数字密码占比最高 */
        set_state(SYS_WIFI_SETUP_PASSWORD);
    } else if (cmd == SYS_WIFI_CMD_BACK) {
        exit_to_off();
    }
}

static void password_confirm_candidate(void)
{
    int sel = s_view.pwd_candidate;
    if (sel < WIFI_SETUP_CHARSET_LEN) {
        if (s_view.pwd_len < WIFI_SETUP_PWD_MAX - 1) {
            s_view.pwd[s_view.pwd_len++] = WIFI_SETUP_CHARSET[sel];
            s_view.pwd[s_view.pwd_len] = '\0';
            push_view(); /* 候选停在原地，连续相同/相邻字符零成本 */
        }
    } else if (sel == WIFI_SETUP_SEL_DEL) {
        if (s_view.pwd_len > 0) {
            s_view.pwd[--s_view.pwd_len] = '\0';
            push_view();
        }
    } else if (sel == WIFI_SETUP_SEL_OK && s_view.pwd_len >= 8) {
        net_credentials_snapshot();
        if (net_apply_credentials(s_view.ssid, s_view.pwd) == ESP_OK) {
            s_credentials_touched = true;
            s_connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
            set_state(SYS_WIFI_SETUP_CONNECTING);
        } else {
            set_state(SYS_WIFI_SETUP_FAILED);
        }
    }
}

static void cmd_in_password(sys_wifi_cmd_t cmd)
{
    if (cmd == SYS_WIFI_CMD_NEXT) {
        s_view.pwd_candidate = (s_view.pwd_candidate + 1) % WIFI_SETUP_SEL_COUNT;
        push_view();
    } else if (cmd == SYS_WIFI_CMD_PREV) {
        s_view.pwd_candidate = (s_view.pwd_candidate - 1 + WIFI_SETUP_SEL_COUNT)
                               % WIFI_SETUP_SEL_COUNT;
        push_view();
    } else if (cmd == SYS_WIFI_CMD_CONFIRM) {
        password_confirm_candidate();
    } else if (cmd == SYS_WIFI_CMD_BACK) {
        if (s_view.pwd_len > 0) {
            s_view.pwd[--s_view.pwd_len] = '\0';
            push_view();
        } else {
            set_state(SYS_WIFI_SETUP_AP_LIST);
        }
    }
}

static void cmd_in_failed(sys_wifi_cmd_t cmd)
{
    if (cmd == SYS_WIFI_CMD_CONFIRM) {
        set_state(SYS_WIFI_SETUP_PASSWORD); /* 保留已输密码，方便改一两位重试 */
    } else if (cmd == SYS_WIFI_CMD_BACK) {
        set_state(SYS_WIFI_SETUP_AP_LIST); /* 再BACK经exit_to_off恢复旧凭据 */
    }
}

static void dispatch_cmd(sys_wifi_cmd_t cmd)
{
    if (cmd == SYS_WIFI_CMD_NONE || s_view.state == SYS_WIFI_SETUP_CONNECTING) {
        return; /* 连接中不响应，防止中止留下半新半旧的凭据 */
    }
    switch (s_view.state) {
    case SYS_WIFI_SETUP_OFF:
        cmd_in_off(cmd);
        break;
    case SYS_WIFI_SETUP_SCANNING:
        if (cmd == SYS_WIFI_CMD_BACK) {
            esp_wifi_scan_stop();
            set_state(SYS_WIFI_SETUP_OFF);
        }
        break;
    case SYS_WIFI_SETUP_AP_LIST:
        cmd_in_list(cmd);
        break;
    case SYS_WIFI_SETUP_PASSWORD:
        cmd_in_password(cmd);
        break;
    case SYS_WIFI_SETUP_FAILED:
        cmd_in_failed(cmd);
        break;
    default:
        break;
    }
}

/* CONNECTING 推进：连上则写NVS回OFF，超时进FAILED(保留密码可重输) */
static void check_connect_progress(void)
{
    if (s_view.state != SYS_WIFI_SETUP_CONNECTING) {
        return;
    }
    if (net_get_state() == NET_CONNECTED) {
        s_credentials_touched = false;
        esp_err_t err = net_credentials_save(s_view.ssid, s_view.pwd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "凭据写入NVS失败: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "已连接到 \"%s\"，凭据已保存", s_view.ssid);
        memset(s_view.pwd, 0, sizeof(s_view.pwd));
        s_view.pwd_len = 0;
        set_state(SYS_WIFI_SETUP_OFF);
    } else if (xTaskGetTickCount() >= s_connect_deadline) {
        ESP_LOGW(TAG, "连接 \"%s\" 超时", s_view.ssid);
        set_state(SYS_WIFI_SETUP_FAILED);
    }
}

static void setup_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 扫描完成通知优先：30s兜底超时防止事件丢失卡死SCANNING */
        if (s_view.state == SYS_WIFI_SETUP_SCANNING
            && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CMD_POLL_MS)) > 0) {
            collect_scan_results();
        } else {
            dispatch_cmd(sys_state_consume_wifi_cmd());
            check_connect_progress();
            if (s_view.state != SYS_WIFI_SETUP_SCANNING) {
                vTaskDelay(pdMS_TO_TICKS(CMD_POLL_MS));
            }
        }
    }
}

esp_err_t wifi_setup_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.state = SYS_WIFI_SETUP_OFF;
    esp_err_t err = net_get_current_ssid(s_view.ssid, sizeof(s_view.ssid));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取当前SSID失败: %s", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                            on_scan_done, NULL, NULL),
                        TAG, "scan handler");
    BaseType_t ok = xTaskCreate(setup_task, "wifi_setup_task", 4096, NULL, 3, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    push_view();
    return ESP_OK;
}
