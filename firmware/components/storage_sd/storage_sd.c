#include "storage_sd.h"

#include <stdio.h>
#include <string.h>

#include "board_rlcd42.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card = NULL;

esp_err_t storage_sd_init(void)
{
    if (s_card) {
        return ESP_OK;
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
    ESP_RETURN_ON_FALSE(rel_path && line && s_card, ESP_ERR_INVALID_STATE, TAG, "未挂载或参数错");

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_SD_MOUNT_POINT, rel_path);

    FILE *f = fopen(path, "a");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "打开失败: %s", path);

    int n = fprintf(f, "%s\n", line);
    fclose(f);

    ESP_RETURN_ON_FALSE(n > 0, ESP_FAIL, TAG, "写入失败: %s", path);
    return ESP_OK;
}
