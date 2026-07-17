/* memory_recall.c - 小规模板载记忆的本地相关性排序。 */
#include "memory_engine_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static size_t utf8_char_len(unsigned char lead)
{
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xe0) == 0xc0) return 2;
    if ((lead & 0xf0) == 0xe0) return 3;
    if ((lead & 0xf8) == 0xf0) return 4;
    return 1;
}

static bool useful_token(const char *text, size_t len)
{
    return len != 1 || (!isspace((unsigned char)text[0]) && !ispunct((unsigned char)text[0]));
}

static bool has_token(const char *text, const char *token, size_t token_len)
{
    for (const char *cursor = text; *cursor; cursor++) {
        if (strncmp(cursor, token, token_len) == 0) return true;
    }
    return false;
}

static int single_char_overlap(const char *query, const char *content)
{
    int matches = 0;
    for (const char *cursor = query; *cursor;) {
        size_t len = utf8_char_len((unsigned char)*cursor);
        if (strlen(cursor) < len) break;
        if (useful_token(cursor, len) && has_token(content, cursor, len)) matches++;
        cursor += len;
    }
    return matches;
}

static int token_overlap_score(const char *query, const char *content)
{
    int bigrams = 0;
    int matches = 0;
    const char *previous = NULL;
    size_t previous_len = 0;
    for (const char *cursor = query; *cursor;) {
        size_t len = utf8_char_len((unsigned char)*cursor);
        if (strlen(cursor) < len) break;
        if (!useful_token(cursor, len)) {
            previous = NULL;
        } else if (previous) {
            char bigram[9];
            memcpy(bigram, previous, previous_len);
            memcpy(bigram + previous_len, cursor, len);
            bigrams++;
            if (has_token(content, bigram, previous_len + len)) matches++;
            previous = cursor;
            previous_len = len;
        } else {
            previous = cursor;
            previous_len = len;
        }
        cursor += len;
    }
    return bigrams ? matches * 2 : single_char_overlap(query, content);
}

bool memory_record_expired(const memory_record_t *record, time_t now)
{
    return record && record->expires_at != 0 && now >= record->expires_at;
}

static int score_record(const memory_record_t *record, const char *query, time_t now)
{
    int score = record->importance;
    if (!query || !query[0]) return score;
    int overlap = token_overlap_score(query, record->content);
    if (overlap == 0 && record->type != MEMORY_TYPE_PROFILE) return 0;
    score += overlap * 20;
    if (strstr(record->content, query)) score += 100;
    if (record->type == MEMORY_TYPE_CORRECTION) score += 15;
    if (record->created_at > 0 && now >= record->created_at
        && now - record->created_at < 7 * 24 * 60 * 60) score += 10;
    return score;
}

static void insert_match(memory_context_t *out, const memory_record_t *record, int score,
                         int *scores, uint8_t limit)
{
    uint8_t index = out->count;
    if (index == limit && score <= scores[index - 1]) return;
    if (index == limit) index--;
    while (index > 0 && score > scores[index - 1]) {
        if (index < limit) {
            out->records[index] = out->records[index - 1];
            scores[index] = scores[index - 1];
        }
        index--;
    }
    out->records[index] = *record;
    scores[index] = score;
    if (out->count < limit) out->count++;
}

static void build_context(memory_context_t *out, size_t max_bytes)
{
    size_t cap = max_bytes < sizeof(out->context) - 1 ? max_bytes : sizeof(out->context) - 1;
    size_t used = 0;
    for (uint8_t i = 0; i < out->count; i++) {
        const memory_record_t *record = &out->records[i];
        int written = snprintf(out->context + used, cap - used + 1, "[%s #%u] %s\n",
                               memory_type_name(record->type), (unsigned)record->id,
                               record->content);
        if (written < 0 || (size_t)written > cap - used) break;
        used += (size_t)written;
    }
    out->context[used] = '\0';
}

esp_err_t memory_recall_build(const memory_state_t *state, const memory_query_t *query,
                              memory_context_t *out)
{
    if (!state || !query || !out || query->limit == 0 || query->limit > MEMORY_ENGINE_MAX_MATCHES
        || query->max_bytes == 0) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    time_t now = query->now ? query->now : time(NULL);
    int scores[MEMORY_ENGINE_MAX_MATCHES] = {0};
    for (size_t i = 0; i < state->count; i++) {
        const memory_record_t *record = &state->entries[i].record;
        if (memory_record_expired(record, now)) continue;
        int score = score_record(record, query->text, now);
        if (score > 0) insert_match(out, record, score, scores, query->limit);
    }
    build_context(out, query->max_bytes);
    return ESP_OK;
}
