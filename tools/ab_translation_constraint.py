#!/usr/bin/env python3
"""
§6.5 红线的行为级回归：确认过的知识能改变译文，模型猜的一个字都不能影响。

用法:
    python tools/ab_translation_constraint.py [--db build/RelWithDebInfo/t.db]

为什么需要它（而不是只看自检）
--------------------------------------------------------------------
自检那 22 组全是"数据层"验证：断言 constraint_terms() 不吐 candidate、
断言 prompt 拼装对了。但**完全可能**出现这种情况：

    · 过滤逻辑对了、prompt 也拼对了，可模型根本不听 → 用户看不到任何改善
    · 或者反过来，某条腿绕过了守卫，candidate 混了进去 → 用户看到莫名其妙的译文

这两种都必须用**真句子 + 真模型**跑出来才算数。这个脚本就是干这个的。

用的是**真实失败案例**，不是我编的句子
--------------------------------------------------------------------
出处：`build\\RelWithDebInfo\\translations.db`（旧库，11 场 206 段）会话 #11。
同一场会话里模型自相矛盾：

    #11 seq=1  src: Welcome to EnglishPod. My name is Marco. And I'm Erica.
               tgt: 欢迎来到EnglishPod。我叫Marco。我是Erica。      ← 保留原文
    #11 seq=2  src: How are you, Erica? Marco, I'm doing really well.
               tgt: 埃里卡，你怎么样？马可，我过得很好。             ← 音译了

（这个库不在仓库里（*.db 被 gitignore），所以句子硬编码在这里，并注明出处。）

三条断言
--------------------------------------------------------------------
  ① 基线必须**逐字复现**历史失败 —— 否则说明模型/提示变了，这个测试已经失效，
     要重新找失败案例，而不是让它静悄悄地"通过"。
  ② candidate 组必须与基线**逐字相同** —— 这就是 §6.5 红线在行为层面的证据。
  ③ confirmed 组必须把 Erica / Marco 恢复过来 —— 第二句承诺在这一刻才成立。

⚠️ 前提：本地混元模型必须存在，且该模型的输出是**确定的**。
   确定性已验证（同输入重复 3 次逐字相同）；若换了采样参数，先重验这一点，
   否则"逐字相同"这条断言会变成随机通过。
"""

import argparse
import os
import sqlite3
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# 真实失败句（出处见文件头注释）
SRC = "How are you, Erica? Marco, I'm doing really well."
HISTORIC_TGT = "埃里卡，你怎么样？马可，我过得很好。"


def find_exe(root):
    for rel in (r"build\RelWithDebInfo\Translator.exe", "Translator.exe"):
        p = os.path.join(root, rel)
        if os.path.exists(p):
            return p
    return None


def set_knowledge(db, rows):
    c = sqlite3.connect(db)
    c.execute("DELETE FROM knowledge")
    for kind, key, value, status in rows:
        c.execute(
            "INSERT INTO knowledge"
            "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,source_text) "
            "VALUES(?,?,?,?,1.0,9,datetime('now'),datetime('now'),'ab_translation_constraint')",
            (kind, key, value, status),
        )
    c.commit()
    c.close()


def dump(exe, db, text):
    """跑一次 --dump-prompt，返回 (译文, 约束条数行)。"""
    p = subprocess.run([exe, "--dump-prompt", text, "--db", db],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out, nterm = None, ""
    for line in (p.stdout or "").splitlines():
        if line.startswith("输出: "):
            out = line[len("输出: "):].strip()
        elif "翻译约束共" in line:
            nterm = line.strip()
    return out, nterm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--db", default=None, help="默认 <root>/build/RelWithDebInfo/t.db")
    args = ap.parse_args()

    root = args.root
    db = args.db or os.path.join(root, r"build\RelWithDebInfo\t.db")
    exe = find_exe(root)
    if not exe:
        print("[失败] 找不到 Translator.exe，先构建")
        return 1
    if not os.path.exists(db):
        print("[失败] 找不到数据库: " + os.path.abspath(db))
        return 1

    print("数据库: " + os.path.abspath(db))
    print("输入  : " + SRC)
    print("历史失败译文: " + HISTORIC_TGT)
    print()

    groups = [
        ("① 空库（基线）", []),
        ("② candidate（模型猜的）",
         [("person", "erica", "Erica", "candidate"),
          ("person", "marco", "Marco", "candidate")]),
        ("③ confirmed（用户确认过）",
         [("person", "erica", "Erica", "confirmed"),
          ("person", "marco", "Marco", "confirmed")]),
    ]

    results = []
    for label, rows in groups:
        set_knowledge(db, rows)
        out, nterm = dump(exe, db, SRC)
        results.append((label, out, nterm))
        print(f"{label}\n    {nterm}\n    输出: {out}\n")

    base = results[0][1]
    ok_repro = (base == HISTORIC_TGT)
    ok_redline = (results[1][1] == base)
    ok_fix = ("Erica" in results[2][1] and "Marco" in results[2][1])

    print("=== 判定 ===")
    print(("✅" if ok_repro else "❌") + " ① 基线逐字复现历史失败"
          + ("" if ok_repro else f"  (实际: {base})"))
    print(("✅" if ok_redline else "❌") + " ② 红线：candidate 与基线逐字相同"
          + ("" if ok_redline else f"  (candidate 输出: {results[1][1]})"))
    print(("✅" if ok_fix else "❌") + " ③ confirmed 把译文纠正成 Erica / Marco"
          + ("" if ok_fix else f"  (实际: {results[2][1]})"))

    # 收尾：把库清干净（这个脚本会写 knowledge 表，必须自己擦）
    set_knowledge(db, [])
    print("\n已清空 knowledge 表（脚本自己造的测试数据自己清）")

    if ok_repro and ok_redline and ok_fix:
        print("== 全部通过 ==")
        return 0
    print("== 有项目未通过 ==")
    return 1


if __name__ == "__main__":
    sys.exit(main())
