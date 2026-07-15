#include "storage_sd.h"

#include <stdio.h>
#include <string.h>

#include "board_rlcd42.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define MAX_PATH_LEN 128

static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_lock = NULL; /* 串行化所有文件操作，见头文件注释 */

/* 校验rel_path并拼出挂载点下的完整路径。拒绝空路径、绝对路径(以'/'开头，
 * 会被误解成另一套根)和任何含".."的路径段(防止越界写到挂载点外)。
 * 调用方必须已持有s_lock或本函数自身不需要锁(纯字符串校验，不碰文件系统) */
static esp_err_t build_path(const char *rel_path, char *out, size_t out_len)
{
    if (!rel_path || !out || rel_path[0] == '\0' || rel_path[0] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(rel_path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_len, "%s/%s", STORAGE_SD_MOUNT_POINT, rel_path);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t storage_sd_init(void)
{
    if (s_card) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex create failed");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; /* 本板只接了D0，1-bit模式 */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK_PIN;
    slot.cmd = BOARD_SD_CMD_PIN;
    slot.d0 = BOARD_SD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, /* 不擅自格式化用户的卡 */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        /* 区分失败原因，否则只看到笼统的ESP_FAIL无从下手 */
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "卡能通信但文件系统挂载失败——大概率是exFAT格式，"
                          "ESP-IDF默认只支持FAT16/FAT32，请把卡格式化为FAT32");
        } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "未检测到SD卡(%s)，请确认卡已插好", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "挂载失败: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "已挂载: %s, %lluMB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

bool storage_sd_is_mounted(void)
{
    return s_card != NULL;
}

esp_err_t storage_sd_get_info(storage_sd_info_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_card, ESP_ERR_INVALID_STATE, TAG, "未挂载");

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    /* FATFS挂载点用驱动号形式 */
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    ESP_RETURN_ON_FALSE(res == FR_OK, ESP_FAIL, TAG, "f_getfree失败: %d", res);

    uint64_t sector = fs->ssize;
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * sector;
    uint64_t freesp = (uint64_t)free_clusters * fs->csize * sector;

    out->total_bytes = total;
    out->used_bytes = total - freesp;
    strncpy(out->name, s_card->cid.name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    return ESP_OK;
}

esp_err_t storage_sd_append_line(const char *rel_path, const char *line)
{
    ESP_RETURN_ON_FALSE(line && s_card && s_lock, ESP_ERR_INVALID_STATE, TAG, "未挂载或参数错");

    char path[MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_path(rel_path, path, sizeof(path)), TAG, "非法路径: %s", rel_path);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "a");
    if (!f) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "打开失败: %s", path);
        return ESP_FAIL;
    }
    int n = fprintf(f, "%s\n", line);
    fclose(f);
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_FALSE(n > 0, ESP_FAIL, TAG, "写入失败: %s", path);
    return ESP_OK;
}

int storage_sd_read_chunk(const char *rel_path, uint32_t offset, void *buf, size_t buf_len)
{
    if (!buf || buf_len == 0 || !s_card || !s_lock) {
        return 0;
    }

    char path[MAX_PATH_LEN];
    if (build_path(rel_path, path, sizeof(path)) != ESP_OK) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "r");
    if (!f) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        xSemaphoreGive(s_lock);
        return 0;
    }
    size_t n = fread(buf, 1, buf_len, f);
    fclose(f);
    xSemaphoreGive(s_lock);
    return (int)n;
}

esp_err_t storage_sd_atomic_replace(const char *rel_path, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data && s_card && s_lock, ESP_ERR_INVALID_STATE, TAG, "未挂载或参数错");

    char path[MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_path(rel_path, path, sizeof(path)), TAG, "非法路径: %s", rel_path);

    char tmp_path[MAX_PATH_LEN + 4];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    ESP_RETURN_ON_FALSE((size_t)tn < sizeof(tmp_path), ESP_ERR_INVALID_SIZE, TAG, "路径过长");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ret = ESP_FAIL;
    } else {
        size_t written = fwrite(data, 1, len, f);
        int close_err = fclose(f);
        if (written != len || close_err != 0) {
            ret = ESP_FAIL;
            remove(tmp_path); /* 写失败别留半截临时文件 */
        } else if (rename(tmp_path, path) != 0) {
            ret = ESP_FAIL;
            remove(tmp_path);
        }
    }
    xSemaphoreGive(s_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "原子替换失败: %s", path);
    }
    return ret;
}

esp_err_t storage_sd_checkpoint(void)
{
    /* 当前所有写入(append/atomic_replace)都在同一次调用内完整open+write+close，
     * 返回时数据已经落盘，这里没有额外待落盘状态可刷。见头文件注释 */
    return ESP_OK;
}
