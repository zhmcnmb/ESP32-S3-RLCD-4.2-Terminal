#include "storage_sd.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
/* 在已持有文件系统锁时创建相对路径中缺失的父目录。路径已由 build_path()
 * 拒绝绝对路径和 ".."，因此不会在挂载点外创建目录。 */
static esp_err_t ensure_parent_dirs(char *path)
{
    char *first = path + strlen(STORAGE_SD_MOUNT_POINT) + 1;
    for (char *cursor = first; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(path, 0775) != 0 && errno != EEXIST) {
            *cursor = '/';
            return ESP_FAIL;
        }
        *cursor = '/';
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
            ESP_LOGW(TAG, "卡能通信但文件系统无法挂载——可能是NTFS/exFAT或分区损坏，"
                          "ESP-IDF只支持FAT16/FAT32，请把卡格式化为FAT32");
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

esp_err_t storage_sd_append_line(const char *rel_path, const char *line)
{
    ESP_RETURN_ON_FALSE(line && s_card && s_lock, ESP_ERR_INVALID_STATE, TAG, "未挂载或参数错");

    char path[MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_path(rel_path, path, sizeof(path)), TAG, "非法路径: %s", rel_path);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t dir_err = ensure_parent_dirs(path);
    if (dir_err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "创建父目录失败: %s", path);
        return dir_err;
    }
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

esp_err_t storage_sd_get_file_size(const char *rel_path, uint32_t *out_size)
{
    ESP_RETURN_ON_FALSE(out_size && s_card && s_lock, ESP_ERR_INVALID_STATE, TAG, "未挂载或参数错");
    char path[MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_path(rel_path, path, sizeof(path)), TAG, "非法路径: %s", rel_path);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "r");
    if (!f) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    int seek_err = fseek(f, 0, SEEK_END);
    long length = seek_err == 0 ? ftell(f) : -1;
    fclose(f);
    xSemaphoreGive(s_lock);
    if (length < 0 || (unsigned long)length > UINT32_MAX) {
        return ESP_FAIL;
    }
    *out_size = (uint32_t)length;
    return ESP_OK;
}

esp_err_t storage_sd_list_dir(const char *rel_dir, storage_sd_dir_entry_cb_t on_entry, void *ctx)
{
    ESP_RETURN_ON_FALSE(rel_dir && on_entry && s_card && s_lock, ESP_ERR_INVALID_STATE, TAG,
                        "未挂载或参数错");
    char path[MAX_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_path(rel_dir, path, sizeof(path)), TAG, "非法目录: %s", rel_dir);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *dir = opendir(path);
    if (!dir) {
        xSemaphoreGive(s_lock);
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        err = on_entry(entry->d_name, ctx);
        if (err != ESP_OK) break;
    }
    closedir(dir);
    xSemaphoreGive(s_lock);
    return err;
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
    esp_err_t dir_err = ensure_parent_dirs(path);
    if (dir_err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "创建父目录失败: %s", path);
        return dir_err;
    }
    esp_err_t ret = ESP_OK;
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGW(TAG, "fopen(%s)失败 errno=%d(%s)", tmp_path, errno, strerror(errno));
        ret = ESP_FAIL;
    } else {
        size_t written = fwrite(data, 1, len, f);
        int fwrite_errno = errno;
        int close_err = fclose(f);
        if (written != len || close_err != 0) {
            ESP_LOGW(TAG, "fwrite/fclose失败 written=%u/%u close_err=%d errno=%d(%s)",
                     (unsigned)written, (unsigned)len, close_err, fwrite_errno, strerror(fwrite_errno));
            ret = ESP_FAIL;
            remove(tmp_path); /* 写失败别留半截临时文件 */
        } else {
            /* FATFS的f_rename严格要求目标不存在(不同于POSIX rename()的"覆盖"语义)，
             * 目标已存在会返回FR_EXIST->EEXIST。这里先unlink旧文件再rename——
             * 牺牲掉"unlink和rename之间掉电会短暂缺文件"的极窄窗口，换来实际能用的
             * 覆盖写；仍然避免了本函数真正要防的问题(写到一半掉电导致内容损坏/截断，
             * 因为写入始终先落到.tmp，只有完整写成功才会替换旧文件) */
            errno = 0;
            if (remove(path) != 0 && errno != ENOENT) {
                ESP_LOGW(TAG, "remove旧文件(%s)失败 errno=%d(%s)", path, errno, strerror(errno));
            }
            if (rename(tmp_path, path) != 0) {
                ESP_LOGW(TAG, "rename(%s -> %s)失败 errno=%d(%s)", tmp_path, path, errno, strerror(errno));
                ret = ESP_FAIL;
                remove(tmp_path);
            }
        }
    }
    xSemaphoreGive(s_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "原子替换失败: %s", path);
    }
    return ret;
}
