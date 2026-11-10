#!/usr/bin/env python3
"""Pre-generate the word-card MP3s into the TTS bridge's word directory.

Run it on the host, inside the bridge container (the Azure key is already in
its environment):

    docker exec muse-tts python gen_word_audio.py

One Azure call per word, none on a flip afterwards: the firmware asks
/tts?word=1&text=apple and the bridge answers with the file this script wrote.
Words already present and valid are skipped, so re-running is safe (that is how
a rebuilt container gets its files back).

The word list is the firmware's own table, same order:
esp32/components/muse/muse_wordcards.c (MUSE_WORDCARDS_SETS_TABLE, 8 sets x 4).
Add a word there, add it here, run again.
"""

import os
import sys

# In the container this file sits beside tts_server.py; run it from the repo's
# tts-server/ directory and it works the same way.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tts_server as srv            # noqa: E402

# 水果 / 动物 / 颜色 / 身体 / 数字 / 学校 / 食物 / 家庭
WORDS = [
    "apple", "banana", "cherry", "peach",
    "cat", "dog", "bird", "fish",
    "red", "blue", "green", "yellow",
    "eye", "nose", "mouth", "ear",
    "one", "two", "three", "four",
    "book", "pen", "bag", "pencil",
    "cake", "milk", "bread", "egg",
    "mum", "dad", "sister", "brother",
]


def main() -> int:
    print("voice %s rate %s dir %s" % (srv.WORD_VOICE, srv.WORD_RATE, srv.WORD_AUDIO_DIR))
    generated = cached = failed = 0
    for word in WORDS:
        if srv.word_audio_get(word) is not None:
            print("%s: already cached" % word)
            cached += 1
            continue
        try:
            audio = srv.synth_word(word)
        except Exception as e:  # noqa: BLE001 - one bad word must not stop the batch
            print("%s: FAILED %s" % (word, e))
            failed += 1
            continue
        path = srv.word_path(word)
        if not srv.word_audio_put(word, audio):
            print("%s: FAILED could not write %s" % (word, path))
            failed += 1
            continue
        over = " OVER 32KB" if len(audio) > srv.WORD_MAX_BYTES else ""
        print("%s: %d bytes -> %s%s" % (word, len(audio), path, over))
        generated += 1
    print("done: %d generated, %d already cached, %d failed"
          % (generated, cached, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
