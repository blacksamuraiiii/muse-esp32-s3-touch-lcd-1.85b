/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_sentence.h"

/* 。 U+3002 is E3 80 82; ！ U+FF01 and ？ U+FF1F are EF BC 81 / EF BC 9F.
 * Compare the leading bytes as they are. The arithmetic guess that used to
 * be here (0xE0 + (lead >> 4)) matched none of them, so a Chinese reply was
 * always cut at the byte limit instead of on a full stop. */
static bool cjk_stop_before(const char *p, size_t i)
{
    if (i < 3) {
        return false;
    }
    unsigned char lead = (unsigned char)p[i - 3];
    unsigned char mid = (unsigned char)p[i - 2];
    unsigned char prev = (unsigned char)p[i - 1];
    return (lead == 0xE3 && mid == 0x80 && prev == 0x82)
           || (lead == 0xEF && mid == 0xBC && (prev == 0x81 || prev == 0x9f));
}

bool muse_sentence_break_before(const char *p, size_t i)
{
    if (i == 0) {
        return false;
    }
    unsigned char prev = (unsigned char)p[i - 1];
    if (prev == ' ' || prev == '\n' || prev == '\t' || prev == ',' || prev == '.'
        || prev == ';' || prev == ':' || prev == '!' || prev == '?') {
        return true;
    }
    return cjk_stop_before(p, i);
}

/* A sentence ends here, but a comma, a space or a newline does not: those
 * only end a request chunk (muse_sentence_break_before), not a sentence. */
static bool sentence_end_before(const char *p, size_t i)
{
    if (i == 0) {
        return false;
    }
    unsigned char prev = (unsigned char)p[i - 1];
    if (prev == '.' || prev == '!' || prev == '?' || prev == ';' || prev == ':') {
        return true;
    }
    return cjk_stop_before(p, i);
}

size_t muse_sentence_next_unit(const char *p, size_t len)
{
    size_t start = 0;
    while (start < len && (p[start] == ' ' || p[start] == '\n' || p[start] == '\t' || p[start] == '\r')) {
        start++;
    }
    if (start >= len) {
        return 0;
    }
    for (size_t i = start + 1; i <= len; i++) {
        if (sentence_end_before(p, i)) {
            return i;
        }
    }
    if (len < MUSE_SENTENCE_CHUNK) {
        return 0;
    }
    /* No boundary: the forced cut. Back off a UTF-8 continuation byte so the
     * cut never lands inside a character, then prefer an earlier break. */
    size_t cut = MUSE_SENTENCE_CHUNK;
    while (cut > 1 && ((unsigned char)p[cut] & 0xC0) == 0x80) {
        cut--;
    }
    for (size_t i = cut; i > cut / 2; i--) {
        if (muse_sentence_break_before(p, i)) {
            return i;
        }
    }
    return cut;
}
