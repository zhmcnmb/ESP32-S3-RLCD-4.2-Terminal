#pragma once

#include "esp_err.h"

/* 首次挂载时写入三个内置声明式 Skill；已有用户文件绝不覆盖。 */
esp_err_t skill_defaults_seed(void);
