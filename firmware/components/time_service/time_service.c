#include "time_service.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "rtc_pcf85063.h"

static const char *TAG = "time";

static bool s_valid = false;

/* SNTP校时成功回调: 把系统时间写回RTC，断电后仍保时 */
static void on_sntp_synced(struct timeval *tv)
{
    (void)tv;

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc); /* RTC存UTC，显示时再按时区转换 */

    esp_err_t err = rtc_pcf85063_set_time(&utc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写回RTC失败: %s", esp_err_to_name(err));
        return;
    }
    s_valid = true;
    ESP_LOGI(TAG, "SNTP已同步并写回RTC");
}

/* newlib(本项目为兼容esp-sr预编译库的_ctype_符号选用newlib而非picolibc)不像
 * picolibc那样声明timegm()——它不是ISO C标准函数，newlib干脆没实现。手写按UTC
 * 解释struct tm的经典逐年累加算法，避免依赖某个libc特有扩展 */
static time_t utc_mktime(const struct tm *tm)
{
    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
    };
    int year = tm->tm_year + 1900;
    long days = 0;
    for (int y = 1970; y < year; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }
    days += days_before_month[tm->tm_mon];
    if (tm->tm_mon > 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        days += 1;   /* 闰年2月已过，补一天 */
    }
    days += tm->tm_mday - 1;
    return (time_t)days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}

esp_err_t time_service_init(const char *timezone)
{
    ESP_RETURN_ON_FALSE(timezone, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    setenv("TZ", timezone, 1);
    tzset();

    bool rtc_valid = false;
    esp_err_t err = rtc_pcf85063_is_valid(&rtc_valid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读RTC状态失败: %s", esp_err_to_name(err));
        return err;
    }

    if (!rtc_valid) {
        ESP_LOGW(TAG, "RTC掉电过，时间不可信，等待NTP校时");
        return ESP_OK;
    }

    struct tm utc;
    ESP_RETURN_ON_ERROR(rtc_pcf85063_get_time(&utc), TAG, "读RTC失败");

    /* RTC存的是UTC，utc_mktime按UTC解释(newlib没有timegm) */
    time_t t = utc_mktime(&utc);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    s_valid = true;

    struct tm local;
    localtime_r(&t, &local);
    ESP_LOGI(TAG, "从RTC恢复本地时间: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    return ESP_OK;
}

esp_err_t time_service_start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = on_sntp_synced;
    cfg.start = true;

    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&cfg), TAG, "sntp init");
    ESP_LOGI(TAG, "SNTP已启动");
    return ESP_OK;
}

bool time_service_is_valid(void)
{
    return s_valid;
}
