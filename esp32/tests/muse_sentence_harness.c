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

/* Host harness for muse_sentence_next_unit(): reads reply text on stdin,
 * prints one unit length per line, then the unspoken tail as "rest N". */

#include <stdio.h>
#include <string.h>

#include "muse_sentence.h"

int main(void)
{
    static char buf[1 << 20];
    size_t len = fread(buf, 1, sizeof(buf), stdin);
    size_t off = 0;
    while (off < len) {
        size_t n = muse_sentence_next_unit(buf + off, len - off);
        if (!n) {
            break;
        }
        printf("%zu\n", n);
        off += n;
    }
    printf("rest %zu\n", len - off);
    return 0;
}
