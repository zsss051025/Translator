#!/usr/bin/env python3
"""
§6.5 红线的行为级回归：确认过的知识能改变译文，模型猜的一个字都不能影响。

用法:
    python tools/ab_translation_constraint.py

⚠️ **它绝不碰你的知识库。** 见下面的"事故记录"。

事故记录（第一版，已修）
--------------------------------------------------------------------
第一版为了让 A/B 干净，直接对 `--db` 指向的库执行 `DELETE FROM knowledge`。
结果**把用户真实知识库里的 4 条 confirmed 全删了**（Erica / Marco / EnglishPod / TV，
全都来自会话 #43 的自动抽取 + 用户逐个确认）。

后果不只是"少了 4 条数据"：
  · 下次录音时识别提示、翻译约束、摘要背景**三条腿全部落空**
  · 用户看不到任何变化，而且完全不知道为什么

修法（两层，缺一不可）：
  ① **默认在一次性 scratch 库上跑**（程序打开时自动建表），跟用户数据没有任何接触
  ② 就算有人手工指定了别的库，**开工前先检查里面有没有知识条目，有就拒绝运行**

这条写在这里而不是只写在提交信息里：下次有人（或下一个我）想"顺手加个 --db" 时，
能先看到这段话。
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

# 一次性 scratch 库：与用户数据无关，跑完删掉
SCRATCH_DB = r"build\RelWithDebInfo\ab_scratch.db"


def find_exe(root):
    for rel in (r"build\RelWithDebInfo\Translator.exe", "Translator.exe"):
        p = os.path.join(root, rel)
        if os.path.exists(p):
            return p
    return None


def knowledge_count(db):
    if not os.path.exists(db):
        return 0
    try:
        c = sqlite3.connect(db)
        n = c.execute("SELECT count(*) FROM knowledge").fetchone()[0]
        c.close()
        return n
    except sqlite3.Error:
        return 0


def ensure_schema(exe, db):
    """让**程序自己**建表，而不是在 Python 里抄一份 DDL。

    抄一份的下场是两边迟早不一致（表结构是程序定义的，Python 只是旁观者）。
    `--gaps` 是最轻的入口：它只读库、不加载模型、不采集音频，但会走完整的
    SessionStore::init()（建表 + 补列 + rebuild 索引）。
    """
    if os.path.exists(db):
        try:
            c = sqlite3.connect(db)
            has = c.execute("SELECT count(*) FROM sqlite_master "
                            "WHERE type='table' AND name='knowledge'").fetchone()[0]
            c.close()
            if has:
                return True
        except sqlite3.Error:
            pass
    p = subprocess.run([exe, "--gaps", "--db", db],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode == 0


def remove_db(db):
    for suffix in ("", "-wal", "-shm"):
        p = db + suffix
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


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
    ap.add_argument("--db", default=None,
                    help="默认用一次性 scratch 库；**不要指向你的真实知识库**")
    args = ap.parse_args()

    root = args.root
    db = args.db or os.path.join(root, SCRATCH_DB)
    exe = find_exe(root)
    if not exe:
        print("[失败] 找不到 Translator.exe，先构建")
        return 1

    # ---- 护栏：绝不碰有数据的库 ----
    n = knowledge_count(db)
    if n > 0:
        print(f"[拒绝运行] {os.path.abspath(db)} 里已经有 {n} 条知识条目。")
        print("           这个脚本为了做对照实验会清空 knowledge 表，")
        print("           跑在真实库上会**删掉用户确认过的知识**（第一版就这么干过）。")
        print("           不要传 --db，让它用一次性 scratch 库。")
        return 1

    # scratch 库是空文件时要先让程序建表（程序是表结构的定义者）
    if not ensure_schema(exe, db):
        print("[失败] 建不出 scratch 库的表结构：" + os.path.abspath(db))
        remove_db(db)
        return 1

    print("数据库: " + os.path.abspath(db) + "（一次性 scratch，与你的知识库无关）")
    print("输入  : " + SRC)
    print("历史失败译文: " + HISTORIC_TGT)
    print()

    try:
        return run_ab(exe, db)
    finally:
        # 无论成功失败都删掉这个一次性库（不动任何真实数据）
        remove_db(db)
        print("\n已删除一次性 scratch 库：" + os.path.abspath(db))


def run_ab(exe, db):
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

    if ok_repro and ok_redline and ok_fix:
        print("== 全部通过 ==")
        return 0
    print("== 有项目未通过 ==")
    return 1


if __name__ == "__main__":
    sys.exit(main())
