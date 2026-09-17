#!/usr/bin/env python3
"""
中文专名抽取的回归检查（R6/R7）。

用法:
    python tools/zh_extract_cases.py

它做什么
--------------------------------------------------------------------
在**一次性 scratch 库**上造一场中文会议片段，跑 `--extract`，检查：
  · 该抽到的 8 个（张伟/李经理/李小明/张总/王工/王老师/凤凰项目/星辰科技）有没有漏
  · 不该抽到的 9 个（交付计划/这个项目/于是/成为/边的/施工/工作/工具/好处）有没有误报

为什么这个脚本和自检里的那一组都要有
--------------------------------------------------------------------
自检（`--selftest`）里已经有同样的一组断言，而且是秒级的。
这个脚本存在的理由是**能看到真实输出**：自检只告诉你"通过/失败"，
而这里会打印 `--extract` 的原始行，改规则时能直接看到多抽了什么、少抽了什么。

⚠️ **这些中文句子是手写的，不是真实 ASR 输出**
--------------------------------------------------------------------
§8.8⑨ 的教训是"用例输入必须来自真实路径"。中文侧目前**没有**真实录音可用，
所以这里只能验"规则在中文会议的语言形状上认不认得出"，
**验不了"真实 ASR 输出上准不准"**（ASR 会有同音错字、有时不加标点）。

真实验证方法：录一场中文会议，然后
    Translator.exe --extract <会话id> --db <你的库>
看它认出了什么、认错了什么，再回来调规则和词表。
"""

import os
import sqlite3
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
DB = os.path.join(ROOT, r"build\RelWithDebInfo\zh_extract_scratch.db")

# 一场"像样"的中文周会片段 + 一批必须**不被**抽出来的干扰句
MEETING = [
    (1, "大家好，今天这个会我们主要讨论三件事。"),
    (2, "张伟负责下周的报价，李经理跟进客户那边的反馈。"),
    (3, "凤凰项目的排期由王工来定，星辰科技那边也在等我们的回复。"),
    (4, "另外李小明提出，交付计划要不要往后挪一周。"),
    (5, "这个项目需要延期，于是我们就成为了负责人。"),
    (6, "张总说下周五之前必须给出结论。"),
    (7, "星辰科技的对接人是王老师，凤凰项目下周启动。"),
    (8, "好处是我们不用重新做，他说的对。"),
    (9, "施工队那边今天进不了场，工具还没到位。"),
]

EXPECT_HIT = ["张伟", "李经理", "李小明", "张总", "王工", "王老师", "凤凰项目", "星辰科技"]
EXPECT_MISS = ["交付计划", "这个项目", "于是", "成为", "边的", "施工", "工作", "工具", "好处"]


def remove_db():
    for suffix in ("", "-wal", "-shm"):
        p = DB + suffix
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


def ensure_schema():
    """让**程序自己**建表 —— 不在 Python 里抄一份 DDL（抄一份迟早两边不一致）。"""
    p = subprocess.run([EXE, "--gaps", "--db", DB],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode == 0


def seed():
    c = sqlite3.connect(DB)
    sid = c.execute(
        "INSERT INTO sessions(started_at, engine, note) "
        "VALUES(datetime('now'),'zh_test','中文抽取回归')"
    ).lastrowid
    for seq, text in MEETING:
        c.execute(
            "INSERT INTO segments(session_id, seq, ts, src_text, tgt_text, engine, ms, confidence) "
            "VALUES(?,?,datetime('now'),?,?,'passthrough',0,0.85)",
            (sid, seq, text, text),
        )
    c.commit()
    c.close()
    return sid


def main():
    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建")
        return 1

    remove_db()
    if not ensure_schema():
        print("[失败] 建不出 scratch 库的表结构")
        remove_db()
        return 1

    try:
        sid = seed()
        p = subprocess.run([EXE, "--extract", str(sid), "--db", DB],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        lines = [l.rstrip() for l in (p.stdout or "").splitlines()
                 if l.strip().startswith("[Extract]") or
                    (l.strip().startswith("[") and "hits=" in l)]

        got = []
        for l in lines:
            s = l.strip()
            if s.startswith("[") and "]" in s and "hits=" in s:
                got.append(s.split("]", 1)[1].strip().split()[0])

        print(f"数据库: {os.path.abspath(DB)}（一次性 scratch，与你的知识库无关）")
        print(f"语料  : {len(MEETING)} 段手写中文会议片段\n")
        for l in lines:
            print("   " + l)

        miss = [w for w in EXPECT_HIT if w not in got]
        bad = [w for w in EXPECT_MISS if w in got]

        print("\n=== 判定 ===")
        print(f"抽到 {len(got)} 个: {got}")
        print(("✅" if not miss else "❌") + " 该抽到的没漏"
              + ("" if not miss else f"  →  漏了 {miss}"))
        print(("✅" if not bad else "❌") + " 不该抽到的没误报"
              + ("" if not bad else f"  →  误报了 {bad}"))

        # 次数也要准（凤凰项目/星辰科技 各出现 2 次）
        for l in lines:
            s = l.strip()
            if s.startswith("[") and "hits=" in s:
                name = s.split("]", 1)[1].strip().split()[0]
                hits = int(s.split("hits=")[1].split()[0])
                if name in ("凤凰项目", "星辰科技") and hits != 2:
                    print(f"❌ {name} 出现次数应为 2，实际 {hits}")
                    bad.append(name)

        ok = not miss and not bad
        print()
        print("== 全部通过 ==" if ok else "== 有偏差（见上）==")
        print("\n⚠️ 语料是手写的，只能验规则的形状；真实 ASR 输出上的准确率")
        print("   要靠录一场中文会议后跑 --extract <会话id> 来验。")
        return 0 if ok else 1
    finally:
        remove_db()
        print("\n已删除一次性 scratch 库：" + os.path.abspath(DB))


if __name__ == "__main__":
    sys.exit(main())
