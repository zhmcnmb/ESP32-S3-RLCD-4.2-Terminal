/* 仅供 llm_provider.c 内部调用的 base URL 查询函数 */

const char *llm_provider_get_siliconflow_url(void)
{
    return "https://api.siliconflow.cn/v1/chat/completions";
}
