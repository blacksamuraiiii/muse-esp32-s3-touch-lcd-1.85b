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

/*
 * Sentence boundaries in reply text, shared by the TTS request chunker
 * (muse_tts.c) and the streaming speaker (muse_chat_session.cpp). One
 * definition, so the two never drift apart: 1.5c proved two copies do.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Longest text spoken or fetched without a sentence boundary, in bytes.
 * The TTS request splitter and the streaming speaker's no-boundary
 * fallback both cut here. Short enough that any proxy in the path is happy
 * with the URL, and that playback starts before a long reply has finished
 * synthesising. */
#define MUSE_SENTENCE_CHUNK 180

/* True when a cut just before `i` lands after punctuation, so a request does
 * not end mid-clause. ASCII punctuation, plus the three-byte CJK marks. */
bool muse_sentence_break_before(const char *p, size_t i);

/* Length of the next speakable unit at the start of `p` (up to `len` bytes),
 * or 0 when there is not yet enough buffered text to speak. A unit ends at
 * a sentence boundary: ASCII . ! ? ; : or CJK 。！？. Leading ASCII
 * whitespace is skipped and counted in the length, so the caller advances
 * its cursor by exactly the returned value. When no boundary is found but
 * at least MUSE_SENTENCE_CHUNK bytes are buffered, the unit is a forced cut
 * at that limit, backed off to a UTF-8 boundary and preferably to an
 * earlier break, the same cut the request splitter makes. */
size_t muse_sentence_next_unit(const char *p, size_t len);
