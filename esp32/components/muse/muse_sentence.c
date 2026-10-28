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

/* Closing marks belong to the sentence they follow, so a quote is spoken with
 * its sentence instead of starting the next one. 」 U+300D is E3 80 8D;
 * 』 E3 80 8F; 】 E3 80 91; 》 E3 80 8B; ” U+201D is E2 80 9D;
 * ’ U+2019 is E2 80 99; ） U+FF09 is EF BC 89. */
static size_t skip_closers(const char *p, size_t i, size_t len)
{
    for (;;) {
        if (i >= len) {
            return i;
        }
        if (p[i] == '"') {
            i++;
            continue;
        }
        if (i + 3 <= len) {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)p[i + 1];
            unsigned char c = (unsigned char)p[i + 2];
            if ((a == 0xE3 && b == 0x80 && (c == 0x8D || c == 0x8F || c == 0x91 || c == 0x8B))
                || (a == 0xE2 && b == 0x80 && (c == 0x9D || c == 0x99))
                || (a == 0xEF && b == 0xBC && c == 0x89)) {
                i += 3;
                continue;
            }
        }
        return i;
    }
}

/* A sentence ends here, but a comma, a space or a newline does not: those only
 * end a request chunk (muse_sentence_break_before), not a sentence.
 *
 * An ASCII mark ends a sentence only when whitespace follows it: "1.5c",
 * "3.14", "10:30" and "http://host:port" are one unit, not two. At the end of
 * what has arrived so far there is nothing to look at, so the mark is trusted
 * unless a digit sits in front of it ("1." may still turn into "1.5c"); every
 * other mark ends the unit at once, which is what keeps a delta that ends on a
 * full stop from waiting for more text. */
static bool sentence_end_before(const char *p, size_t i, size_t len)
{
    if (i == 0) {
        return false;
    }
    if (cjk_stop_before(p, i)) {
        return true;
    }
    unsigned char prev = (unsigned char)p[i - 1];
    if (!(prev == '.' || prev == '!' || prev == '?' || prev == ';' || prev == ':')) {
        return false;
    }
    if (i < len) {
        unsigned char next = (unsigned char)p[i];
        return next == ' ' || next == '\n' || next == '\r' || next == '\t';
    }
    return i < 2 || p[i - 2] < '0' || p[i - 2] > '9';
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
        if (sentence_end_before(p, i, len)) {
            return skip_closers(p, i, len);
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
