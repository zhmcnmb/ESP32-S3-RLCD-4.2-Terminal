/* tts_text.c - TTS 文本预处理纯函数：Markdown 过滤 + 句子切分 + UTF-8 安全截断。
 * 从 voice_tts_stream 抽出为独立 module，无 FreeRTOS/状态依赖，可独立纯测。 */
#include "tts_text.h"

#include <string.h>

/* 快路径判定与过滤共用同一字符集（含 "- " 配对），新增记号只改这一处 */
bool tts_contains_markdown(const char *text)
{
    return strpbrk(text, "*`#_-") != NULL;
}

size_t tts_strip_markdown(const char *src, size_t len, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '*' || c == '`' || c == '#' || c == '_') continue;
        /* Markdown 列表标记 "- " 不朗读(减号无空格后如 3-5 不受影响) */
        if (c == '-' && i + 1 < len && src[i + 1] == ' ') {
            i++;
            continue;
        }
        out[n++] = c;
    }
    return n;
}

static size_t tts_sentence_len(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' || text[i] == '.' || text[i] == '!' || text[i] == '?') return i + 1;
        if (i + 2 < len && (memcmp(text + i, "。", 3) == 0
            || memcmp(text + i, "！", 3) == 0 || memcmp(text + i, "？", 3) == 0)) {
            return i + 3;
        }
    }
    return 0;
}

/* 把缓冲内所有完整句子并成一个合成块:首块仍最小化开口延迟,
 * 后续块因模型产出快于播放自然变长,减少逐句HTTPS往返造成的停顿。 */
size_t tts_sentences_len(const char *text, size_t len)
{
    size_t total = 0;
    for (;;) {
        size_t n = tts_sentence_len(text + total, len - total);
        if (n == 0) return total;
        total += n;
    }
}

/* 流缓冲边界可能落进UTF-8字符内，只交付完整前缀，不能读取text[len]外的字节。 */
size_t tts_safe_chunk_len(const char *text, size_t len)
{
    size_t chunk_len = len < TTS_SENTENCE_MAX ? len : TTS_SENTENCE_MAX;
    if (chunk_len == 0) return 0;
    size_t start = chunk_len;
    while (start > 0 && (((unsigned char)text[start - 1] & 0xc0) == 0x80)) start--;
    if (start == chunk_len) {
        unsigned char lead = (unsigned char)text[chunk_len - 1];
        return lead < 0x80 || lead >= 0xf8 ? chunk_len : chunk_len - 1;
    }
    if (start == 0) return chunk_len; /* 上游非法连续字节原样交给服务端处理。 */
    unsigned char lead = (unsigned char)text[start - 1];
    size_t expected = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : lead < 0xf8 ? 3 : 0;
    return expected == chunk_len - start ? chunk_len : start - 1;
}
