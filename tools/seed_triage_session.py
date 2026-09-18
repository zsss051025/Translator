# -*- coding: utf-8 -*-
"""造一场内容可控的会话，用来验判断缓存真的在**真会话路径**上生效。

【为什么要造，而不是录一段真音频】
要验的是"某个词第一次联网判过、第二次就命中缓存不再联网"。
用真音频的话：抽出来什么词由 ASR 和抽取器决定，验一次要等 Whisper 加载 ~100 秒，
而且第二次还得保证抽到**同一个词**。这里直接写段落文本，
把变量降到只剩"缓存有没有生效"这一件事。

【文本是照抽取器的规矩写的，不是随便造的句子】
抽取器的英文规则里，句首大写的词不算（无法和普通句首大写区分），
所以下面要考的每一个词都**出现在句子中间**，而且每个至少出现两次 ——
这两条都是规则的一部分，不是凑数。否则它们压根进不了候选，
也就验不到分诊和缓存（第一次就是这么翻车的：写了 `inconsiderate`、`Gecko`，
结果一个是句中小写、一个只出现两次但都贴着 "the "，一个都没抽出来）。
"""
import os
import sqlite3
import sys

db = sys.argv[1]
if not os.path.exists(db):
    print("库不存在：%s —— 先跑一次 `Translator.exe --terms --db %s` 建 schema"
          % (db, db))
    sys.exit(1)

segs = [
    # 要考的普通词（不在本地通用词表里 → 应该走模型 → 模型判"通用" → 落缓存）
    "We should finalize the Budget before the Friday call.",
    "The Friday call depends on the Budget review.",
    "Our Revenue number for the Quarter is not final yet.",
    "The Revenue slide needs the Budget delta for the Quarter.",
    # 要考的私有专名（应该**被问**，且**不该**落缓存）
    "Please verify the Nimbus rollout before the Budget meeting.",
    "Nimbus is the internal name we agreed on, so keep saying Nimbus.",
]

con = sqlite3.connect(db)
con.execute("DELETE FROM segments WHERE session_id = 1;")
con.execute("DELETE FROM sessions WHERE id = 1;")
con.execute("INSERT INTO sessions (id, started_at, ended_at, engine, note) "
            "VALUES (1, '2026-09-19 10:00:00.000', '2026-09-19 10:05:00.000', "
            "'Probe', '判断缓存探针')")
for i, s in enumerate(segs):
    con.execute("INSERT INTO segments "
                "(session_id, seq, ts, src_text, tgt_text, engine, ms) "
                "VALUES (1, ?, '2026-09-19 10:0%d:00.000', ?, '(未翻译)', 'Probe', 0)"
                % (i + 1), (i + 1, s))
con.commit()
con.close()
print("已造会话 #1，%d 段" % len(segs))
