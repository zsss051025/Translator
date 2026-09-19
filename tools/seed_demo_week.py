# -*- coding: utf-8 -*-
"""造「凤凰项目」一周三场会的种子数据（5.5 验证 + 5.8 演示共用）。

【为什么需要它 —— 两个理由，第二个更重要】

① **5.5 没法用现有会话验**。真实库里那 20 场是 EnglishPod 播客和一段课堂，
   规则抽取器在它们上面抽出的 5 条行动项**全部被校验器正确地丢掉了**
   （播客里没有待办）。所以 ingest 那条路径一次都没被走到 ——
   而"跨会话聚合"必须要有**同一件事出现在多场会里**才能验。

② **演示场景需要一个能演的数据前提**（PROJECT.md §7 5.8 已记）：
   场景定的是「整理我过去一周关于凤凰项目的工作并生成周报」，
   而库里只有播客。要演这个场景，就得有"过去一周的凤凰项目会议"。

【这些是合成数据，不是真实录音 —— 标记清楚】
每一场的 note 都写「演示种子数据」，`engine='Seed'`（**不是** 'Hunyuan'/'Whisper'），
所以任何按 engine 过滤的地方都能一眼把它们和真实会话分开，
`loop_evidence.py` 也不会把它们当成真实会话
（它按 `engine <> 'SelfTest'` 过滤 —— 这点下面会再提一次）。

【文本是照抽取器规矩写的】
  · 行动项要有**触发词**（需要/负责/跟进/提交/安排…），否则抽不出来
  · 还要有**严格动作词**（能接宾语的动词：完成/提交/评审/汇总…），
    否则会被 0.1 的假行动项校验器丢掉 —— 这是实测踩到的：
    第一版只写了"需要"，5 条全被丢掉，`--actions` 还是空的
  · 同一个任务在**两场**里出现（换个说法），用来验跨会话聚合：
    确定性层（key 相同）能合上的那种，和 key 不同、需要判断器的那种，各留一个

【幂等】重复跑脚本会先删掉这三场，所以可以反复用。
"""
import os
import sqlite3
import sys

db = sys.argv[1]
if not os.path.exists(db):
    print("库不存在：%s —— 先跑一次 `Translator.exe --terms --db %s` 建 schema"
          % (db, db))
    sys.exit(1)

# 三场会，id 用 9001~9003（**刻意避开真实会话的 id 段**，
# 万一有人把它们和真实数据混在一起看，一眼能认出是种子数据）
SESSIONS = [
    (9001, "2026-09-14 10:00:00.000", "2026-09-14 10:42:00.000",
     "凤凰项目周会（演示种子数据）"),
    (9002, "2026-09-16 14:00:00.000", "2026-09-16 14:35:00.000",
     "凤凰项目接口评审（演示种子数据）"),
    (9003, "2026-09-18 09:30:00.000", "2026-09-18 10:05:00.000",
     "凤凰项目复盘（演示种子数据）"),
]

TRANSCRIPTS = {
    # 第一场：定下三件事。
    #
    # ⚠️ **每个要考的专名都出现至少两次，而且都不在句首。**
    # 这不是凑字数 —— 抽取器的规则要求句中大写词有重复出现当"佐证"，
    # 只出现一次的词会被正确丢掉。第一版写的时候每个名字只提一次，
    # 结果只抽出 3 个候选，而其中**没有一个产品名**：
    # 演示时想说"人名/项目名/产品名它都会问"就说不出来了。
    # （用户原话就是这个要求：「出现陌生的人的花名例如 penny，那它就会问我
    #   这似乎是一个人名…出现一个陌生的产品名也会类似询问…新的项目名字也会询问」。）
    9001: [
        "我们先过一下凤凰项目这周的进度。",
        "凤凰项目的接口文档需要重写，这块张伟负责跟进。",
        "张伟说他会在这周五之前完成凤凰项目的接口文档。",
        "另外李经理需要提交一份极光平台的压测报告。",
        "极光平台的压测报告要覆盖 Gecko 模块的高并发场景。",
        "Gecko 这块李经理最熟，压测报告里要写清 Gecko 的瓶颈在哪。",
        "记得把极光平台的容量水位也写进压测报告。",
        "最后我们安排下周做一次安全评审。",
    ],
    # 第二场：同一个任务又提了一次（**照抄的 key** —— 归一化后完全一样）
    #         加上一个换了说法的（key 不同，留给判断器）
    9002: [
        "今天主要评审凤凰项目的接口文档。",
        "凤凰项目的接口文档需要重写，这个上周就说了，张伟负责跟进，这周五之前完成。",
        "李经理需要提交一份压测报告，负责极光平台那部分。",
        "还要安排安全评审，时间定在下周三。",
    ],
    # 第三场：复盘。**刻意不提**第一场的两件事（用来验"漏掉的任务还在待办里"）
    9003: [
        "凤凰项目这周整体推进得还行。",
        "安全评审已经安排好了，下周三下午两点。",
        "我们需要跟进一下 Gecko 模块的遗留问题。",
    ],
}


def main() -> int:
    con = sqlite3.connect(db)
    ids = [s[0] for s in SESSIONS]
    # 先清掉旧的（三张表都要清，否则台账会留着上一轮的关联）
    q = ",".join("?" * len(ids))
    old = [r[0] for r in con.execute(
        "SELECT id FROM actions WHERE source_session IN (%s)" % q, ids)]
    if old:
        aq = ",".join("?" * len(old))
        con.execute("DELETE FROM action_sessions WHERE action_id IN (%s)" % aq, old)
        con.execute("DELETE FROM actions WHERE id IN (%s)" % aq, old)
    con.execute("DELETE FROM segments WHERE session_id IN (%s)" % q, ids)
    con.execute("DELETE FROM sessions WHERE id IN (%s)" % q, ids)

    n_seg = 0
    for sid, started, ended, note in SESSIONS:
        con.execute("INSERT INTO sessions (id,started_at,ended_at,engine,note) "
                    "VALUES (?,?,?,?,?)", (sid, started, ended, "Seed", note))
        for i, text in enumerate(TRANSCRIPTS[sid]):
            # ⚠️ **tgt_text 必须等于 src_text，不能填占位串。**
            # 中文会议的约定是 passthrough（检测到 zh == 目标 zh → src=tgt 落库），
            # 而交付物/行动项抽取读的是 tgt_text。第一版我在这里填了
            # "(中文会议，无需翻译)"，结果抽取器把**那句占位串当成了任务文本**，
            # 校验器报的是「没有可执行的动作词：(中文会议，无需翻译)」——
            # 也就是说我在验自己的占位符，而不是在验产品。
            # 教训：**种子数据必须遵守管道的真实约定**，否则测的是别的东西。
            con.execute(
                "INSERT INTO segments (session_id,seq,ts,src_text,tgt_text,engine,ms) "
                "VALUES (?,?,?,?,?,?,0)",
                (sid, i + 1,
                 (started[:11] + "%02d:%02d:00.000" % (10 + i // 60, i % 60)),
                 text, text, "Seed"))
            n_seg += 1
    con.commit()
    con.close()
    print("已写入 %d 场演示会话 / %d 段（engine=Seed，note 标了「演示种子数据」）"
          % (len(SESSIONS), n_seg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
