#pragma once

#include <stdbool.h>
#include <stddef.h>
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
 *  1. 已有独立后台任务的服务(env_logger历史/weather_client)
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
 * R0阶段未提供通用command通道；P5.2 仅为语音取消增加最小请求位，
 * 不扩展成完整 post-command 框架(coding-standards.md 第2节)。
 *
 * 任务: sys_state_task, 栈3072, 优先级2 (登记于 docs/coding-standards.md 第7节)
 */

#define SYS_STATE_ENV_HISTORY_LEN 60 /* 与 env_logger 环形缓冲容量保持一致 */

/* scheduler→voice_assistant 的单槽提醒邮箱容量；固定上限避免任务间动态分配。 */
#define SYS_STATE_REMINDER_TEXT_MAX 96

/* 语音/agent 对外可见相位。UI 只读快照；状态机由 voice_assistant 推进 */
typedef enum {
    SYS_VOICE_IDLE = 0,
    SYS_VOICE_LISTENING,
    SYS_VOICE_TRANSCRIBING,
    SYS_VOICE_THINKING,
    SYS_VOICE_EXECUTING, /* P6 agent 工具执行预留 */
    SYS_VOICE_SPEAKING,
    SYS_VOICE_CONFIRM,   /* P6 物理确认预留 */
    SYS_VOICE_ERROR,
} sys_voice_phase_t;

typedef struct {
    time_t timestamp;
    float temperature;
    float humidity;
} sys_state_env_sample_t;

/* WiFi配网(wifi_setup组件/page_wifi页)。视图整体推送避免11参数函数。 */
#define SYS_WIFI_AP_MAX 12

typedef enum {
    SYS_WIFI_SETUP_OFF = 0, /* 配网未激活，页面仅显示连接状态 */
    SYS_WIFI_SETUP_SCANNING,
    SYS_WIFI_SETUP_AP_LIST,
    SYS_WIFI_SETUP_PASSWORD,
    SYS_WIFI_SETUP_CONNECTING,
    SYS_WIFI_SETUP_FAILED, /* 连接失败，可重输密码或返回列表 */
} sys_wifi_setup_state_t;

typedef enum {
    SYS_WIFI_CMD_NONE = 0,
    SYS_WIFI_CMD_ENTER,   /* OFF -> 开始扫描 */
    SYS_WIFI_CMD_NEXT,    /* 列表下移 / 候选字符后滚 */
    SYS_WIFI_CMD_PREV,    /* 列表上移 / 候选字符前滚 */
    SYS_WIFI_CMD_CONFIRM, /* 选中AP / 确认候选字符 / FAILED时重输 */
    SYS_WIFI_CMD_BACK,    /* 密码删一位(0位返回列表) / 列表退出OFF / 扫描中止 */
} sys_wifi_cmd_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
} sys_wifi_ap_t;

typedef struct {
    sys_wifi_setup_state_t state;
    char ssid[33];                      /* 当前连接或目标SSID */
    sys_wifi_ap_t aps[SYS_WIFI_AP_MAX]; /* 扫描结果，按信号降序 */
    int ap_count;
    int ap_sel;                         /* 列表选中索引 */
    char pwd[64];                       /* 已确认密码明文(仅内存，用于输入校验) */
    int pwd_len;
    int pwd_candidate;                  /* 候选索引，见 wifi_setup.h 字符集定义 */
} sys_wifi_setup_view_t;

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


    /* 语音助手(P5.2)：page_jarvis 只读这些字段 */
    sys_voice_phase_t voice_phase;
    uint32_t voice_turn_id; /* 当前轮次，异步回调用它丢弃过期结果 */
    char voice_error[24];   /* ERROR 相位的短原因("识别失败"等)；非ERROR相位为空串 */

    /* WiFi配网视图：wifi_setup 推送，page_wifi 只读 */
    sys_wifi_setup_view_t wifi_setup;

    uint32_t version; /* 任意字段更新即递增，页面据此判断数据是否变化 */
} sys_state_snapshot_t;

/* 依赖 shtc3_init()/battery_init()/board_init() 已完成；内部轮询任务采集瞬时
 * 温湿度/电池/WiFi/SD状态，与env_logger/weather_client
 * 各自的后台任务相互独立、互不阻塞 */
esp_err_t sys_state_init(void);

/* 一次性一致读取全部字段(内部持锁拷贝，返回后 out 在锁外可安全使用) */
void sys_state_get_snapshot(sys_state_snapshot_t *out);

/* 以下均由对应Producer在一次采集后调用。weather在
 * available=false时由调用方决定传入0(从未成功过)还是上一次的缓存值(曾经
 * 成功过，临时不可达)，让页面能区分"从无数据"和"有旧数据可展示" */
void sys_state_apply_env_history(const sys_state_env_sample_t *samples, int count, uint32_t total);
void sys_state_apply_weather(bool available, float temperature, int humidity,
                             const char *condition, const char *wind, time_t updated_at);

/* voice_assistant 推进相位；turn_id 由调用方递增后传入。reason 仅在
 * phase==SYS_VOICE_ERROR 时展示，可传NULL(显示通用"故障")；其余相位忽略 */
void sys_state_apply_voice(sys_voice_phase_t phase, uint32_t turn_id, const char *reason);

/* wifi_setup 在状态机任一变化后整体推送视图 */
void sys_state_apply_wifi_setup(const sys_wifi_setup_view_t *view);

/* 页面投递配网命令(单槽邮箱，未消费时被新命令覆盖)；wifi_setup 任务轮询取走 */
void sys_state_post_wifi_cmd(sys_wifi_cmd_t cmd);
sys_wifi_cmd_t sys_state_consume_wifi_cmd(void);

/* 页面提交取消请求；voice_assistant 轮询 consume 后清位 */
void sys_state_request_voice_cancel(void);
bool sys_state_consume_voice_cancel(void);

/* 页面提交手动触发请求(push-to-talk); voice_assistant 空闲时轮询 consume 后清位 */
void sys_state_request_voice_trigger(void);
bool sys_state_consume_voice_trigger(void);

/* scheduler 投递到期提醒；上一条尚未播出时返回false，调用方下秒重试。 */
bool sys_state_post_reminder(const char *text);

/* 检查是否有待播报提醒，不取走邮箱内容。 */
bool sys_state_reminder_pending(void);

/* voice_assistant 空闲时取走一条提醒；无提醒或out不足容纳全文时返回false且不清槽。 */
bool sys_state_consume_reminder(char *out, size_t out_len);

/* 串口文本注入(验收测试入口)：text_console 读一行串口文本投递本邮箱，
 * voice_assistant 空闲时取走，跳过录音/ASR、按完整 agent/TTS 链路处理。
 * 单槽，占槽时 post 返回false；容量与 voice_assistant 的 ASR_TEXT_MAX 对齐。 */
#define SYS_STATE_TEXT_TURN_MAX 512
bool sys_state_post_text_turn(const char *text);
bool sys_state_consume_text_turn(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
