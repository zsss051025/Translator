# -*- coding: utf-8 -*-
"""往一次性 scratch 库里塞几条判断缓存，用来验 `--triage-cache` 命令本身。

【为什么不直接跑真会话产生缓存】那要先录一场会、等 Whisper 加载 100 秒，
而且要有 DeepSeek Key。命令行工具的输出格式是**独立于**缓存怎么产生的一环，
用已知内容验它，比"真跑一次看看"可靠得多 —— 也才能验到复用计数那种
真跑一次未必会出现的字段。

【刻意自己建表 —— 是的，这是有意为之】
调用方必须先跑一次 `Translator.exe --triage-cache --db <库>` 让 schema 建好，
本脚本**不** CREATE TABLE。这样验的就是**真实 schema**：
两边列名/列数一旦不一致，这里立刻报错，而不是"脚本自己建了张凑合的表、
把 schema 的问题盖过去"。
"""
import os
import sqlite3
import sys

db = sys.argv[1]
if not os.path.exists(db):
    print("库不存在：%s —— 先跑一次 `Translator.exe --triage-cache --db %s`"
          " 让它建 schema" % (db, db))
    sys.exit(1)

con = sqlite3.connect(db)
con.execute("DELETE FROM triage_verdicts;")

rows = [
    # (key, value, kind, why, reused)
    ("tv", "TV", "term", "大众缩写，人人皆知", 7),
    ("down", "down", "term", "常见副词/介词，不是专名", 3),
    ("inconsiderate", "inconsiderate", "term", "普通形容词", 0),
    ("zzstale", "ZZStale", "term", "很久以前判的", 0),
]
for key, value, kind, why, reused in rows:
    # 最后一条故意做成 2020 年判的 —— 验"陈旧提示"那一行
    judged = ("2020-01-01 10:00:00.000" if value == "ZZStale"
              else "2026-09-19 02:00:00.000")
    con.execute(
        "INSERT INTO triage_verdicts "
        "(key, value, verdict, kind, primer, why, source, reused, judged_at) "
        "VALUES (?,?, 'skip', ?, '', ?, 'model', ?, ?);",
        (key, value, kind, why, reused, judged))
con.commit()
con.close()
print("已写入 %d 条到 %s" % (len(rows), db))
