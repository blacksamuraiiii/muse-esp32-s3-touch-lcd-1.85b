/* SPDX-License-Identifier: Apache-2.0 */
/* Self-written 1.5a text tables; see muse_pet_text.h for the contract. */
#include "muse_pet_text.h"

#include "muse_pet_model.h"
#include <stddef.h>

typedef struct {
    const char *grade;
    const char *text;
} muse_pet_fortune_card_t;

/* Index-aligned with the grades: card N's grade is grades[N].
 * All 20 texts are original 1.5a copy (not TamaMe, not 六爻). */
static const muse_pet_fortune_card_t s_cards[MUSE_PET_FORTUNES] = {
    {"大吉", "宜出门走走、吃点甜的、早点睡；忌熬夜、忌冲动消费。"},
    {"大吉", "今天运气站在你这边，适合把拖了很久的事办了。"},
    {"大吉", "宜给家人打个电话，好消息可能在路上。"},
    {"大吉", "宜晒晒太阳、喝杯热茶，心情会跟着亮起来。"},
    {"中吉", "宜慢慢来，不着急，稳稳的一天就是好的一天。"},
    {"中吉", "宜整理桌面、收拾房间，清爽了运气也顺了。"},
    {"中吉", "今天适合学习新东西，记住的比平时多一点。"},
    {"中吉", "宜多喝水、早点出门，路上会遇到小惊喜。"},
    {"中吉", "宜听一首喜欢的歌，再出发，心情不一样。"},
    {"中吉", "今天宜和朋友聊聊天，一句话可能解开一个结。"},
    {"小吉", "宜吃一顿热乎的饭，身体暖了，事就顺了。"},
    {"小吉", "出门记得带伞，天气说变就变，有备无患。"},
    {"小吉", "宜把手机放下半小时，陪陪身边的人。"},
    {"小吉", "今天适合做计划，不适合赶工，慢就是快。"},
    {"小吉", "宜早睡一小时，明天会比今天更精神。"},
    {"吉", "平平常常也是福，今天宜按部就班。"},
    {"吉", "宜微笑待人，你的好运气会传染给别人。"},
    {"吉", "今天宜读几页书，不用多，翻开就是收获。"},
    {"吉", "宜给自己留一点空闲，什么都不做也很好。"},
    {"吉", "宜早点回家，和家人一起吃顿晚饭。"},
};

static const char *const s_foods[MUSE_PET_FOOD_COUNT] = {
    "米饭", "蛋糕", "咖啡",
};

uint32_t muse_pet_fortune_count(void)
{
    return MUSE_PET_FORTUNES;
}

const char *muse_pet_fortune_grade(uint32_t index)
{
    return index < MUSE_PET_FORTUNES ? s_cards[index].grade : NULL;
}

const char *muse_pet_fortune_text(uint32_t index)
{
    return index < MUSE_PET_FORTUNES ? s_cards[index].text : NULL;
}

const char *muse_pet_food_name(uint32_t index)
{
    return index < MUSE_PET_FOOD_COUNT ? s_foods[index] : NULL;
}
