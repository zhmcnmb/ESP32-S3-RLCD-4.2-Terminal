#pragma once

#include <stddef.h>

#define TTS_SENTENCE_MAX 1024

/* 过滤 Markdown 记号(* ` # _ 和 "- " 列表标记)，out 至少 len 字节，
 * 返回写入字节数(<=len，不含结尾'\0'，调用方自行补)。 */
size_t tts_strip_markdown(const char *src, size_t len, char *out);

/* 返回首个完整句子的长度(含句末标点 \n . ! ? 及中文。！？)，无句末返回 0 */
size_t tts_sentence_len(const char *text, size_t len);

/* 合并缓冲内所有完整句子为一个合成块，减少逐句 HTTPS 往返停顿 */
size_t tts_sentences_len(const char *text, size_t len);

/* 流缓冲边界可能落进 UTF-8 字符内，只交付完整前缀，不读 text[len] 外字节 */
size_t tts_safe_chunk_len(const char *text, size_t len);
