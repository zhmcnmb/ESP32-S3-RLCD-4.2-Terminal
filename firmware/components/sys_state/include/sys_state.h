#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 全局状态快照：UI(及后续P6 agent工具)读取设备/业务状态的唯一权威入口。
 *
 * 两种数据来源：
 *  1. 已有独立后台任务的服务(env_logger历史/weather_client/claude_quota_client)
 *     在自己任务里完成一次采集后调用对应 sys_state_apply_*() 推送进来。
 *  2. 没有专属后台任务的"环境状态"(瞬时温湿度/电池/WiFi/SD挂载) 由本模块
 *     自己的内部轮询任务定期采集并写入，页面不需要关心谁在采集。
 * 时间字段(now/time_valid)开销极低，直接在 sys_state_get_snapshot() 里
 * 现读 time()，不走 apply，永远是"读快照那一刻"的准确值。
 *
 * 页面只调用一次 sys_state_get_snapshot() 做整体一致读取，不再分别调用
 * 多个组件的getter自己拼状态(那样会看到几次调用之间被其他任务改动过的
 * 不一致组合)。"深"体现在：调用方看不到锁、轮询任务和版本管理，只看到
 * snapshot结构体。
 *
 * R0阶段不提供"post command"：现有4个页面的BOOT ACTION都只是"用当前缓存
 * 重绘"，不需要驱动后台轮询立即重新抓取；命令通道等P6 agent工具出现真实
 * 需求时再加(coding-standards.md 第2节: 不为"以后可能"建立Interface)。
 *
 * 任务: sys_state_task, 栈3072, 优先级2 (登记于 docs/coding-standards.md 第7节)
 */

#define SYS_STATE_ENV_HISTORY_LEN 60 /* 与 env_logger 环形缓冲容量保持一致 */

typedef struct {
    time_t timestamp;
    float temperature;
    float humidity;
} sys_state_env_sample_t;

typedef struct {
    /* 时间 */
    bool time_valid;
    time_t now; /* UTC epoch，页面自行 localtime_r() 转本地时间 */

    /* 温湿度(瞬时读数，首页用) */
    bool env_ok;
    float temperature;
    float humidity;
    time_t env_updated_at;

    /* 电池 */
    bool battery_ok;
    float battery_voltage;
    int battery_percent;
    bool battery_charging;

    /* 网络 / SD */
    bool wifi_connected;
    bool sd_mounted;

    /* 环境历史(趋势页用，只读视图) */
    sys_state_env_sample_t env_history[SYS_STATE_ENV_HISTORY_LEN];
    int env_history_count;     /* 本次快照里实际有效的条数，按时间升序 */
    uint32_t env_sample_total; /* 累计采样数(含被环形缓冲淘汰的) */

    /* 天气 */
    bool weather_available;
    float weather_temperature;
    int weather_humidity;
    char weather_condition[24];
    char weather_wind[40];
    time_t weather_updated_at;

    /* Claude用量 */
    bool quota_available;
    float quota_used_5h;
    float quota_used_7d;
    time_t quota_resets_5h;
    time_t quota_resets_7d;
    time_t quota_updated_at;

    uint32_t version; /* 任意字段更新即递增，页面据此判断数据是否变化 */
} sys_state_snapshot_t;

/* 依赖 shtc3_init()/battery_init()/board_init() 已完成；内部轮询任务采集瞬时
 * 温湿度/电池/WiFi/SD状态，与env_logger/weather_client/claude_quota_client
 * 各自的后台任务相互独立、互不阻塞 */
esp_err_t sys_state_init(void);

/* 一次性一致读取全部字段(内部持锁拷贝，返回后 out 在锁外可安全使用) */
void sys_state_get_snapshot(sys_state_snapshot_t *out);

/* 以下均由对应Producer在一次采集后调用。env/battery在ok=false时数值字段填0
 * (页面对ok=false一律显示占位符"--"，不读具体数值)；weather/quota在
 * available=false时由调用方决定传入0(从未成功过)还是上一次的缓存值(曾经
 * 成功过，桥接服务临时不可达)，让页面能区分"从无数据"和"有旧数据可展示" */
void sys_state_apply_env(bool ok, float temperature, float humidity, time_t updated_at);
void sys_state_apply_battery(bool ok, float voltage, int percent, bool charging);
void sys_state_apply_wifi(bool connected);
void sys_state_apply_sd(bool mounted);
void sys_state_apply_env_history(const sys_state_env_sample_t *samples, int count, uint32_t total);
void sys_state_apply_weather(bool available, float temperature, int humidity,
                             const char *condition, const char *wind, time_t updated_at);
void sys_state_apply_quota(bool available, float used_5h, float used_7d,
                           time_t resets_5h, time_t resets_7d, time_t updated_at);

#ifdef __cplusplus
}
#endif
