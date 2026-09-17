#!/usr/bin/env python3
"""
源语言 == 目标语言时，**必须跳过翻译** —— 否则小模型会改写内容。

用法:
    python tools/passthrough_check.py

它在验什么
--------------------------------------------------------------------
用 `--lang en --target en`（源==目标）跑一段英文音频，然后断言：
  ① 每一段落库的 `engine` 都是 `passthrough`
  ② 每一段的 `tgt_text` 与 `src_text` **逐字节相同**

为什么这条必须单独验（而不是放进 --selftest）
--------------------------------------------------------------------
它要真跑 Whisper，秒级自检做不到。但这正是它值得单独存在的原因 ——
**这是一个静默改写内容的 bug**，光看日志看不出来。

真实事故（2026-09-17 发现）
--------------------------------------------------------------------
`--lang en --target en` 本该跳过翻译，实际走了翻译，而且**改了内容**：

    src: Ask not!                        → tgt: Do not ask!        ← 否定语气被改了
    src: what your country can do…       → tgt: What your country… ← 首字母被大写

根因是三处串起来的：
  ① `requested_lang_ != "auto"` → `should_detect_language()` 永远返回 false
  ② `update_detected_language()` 只在 `lang_arg == "auto"` 时才调用
  ⇒ `current_lang_` 永远是空的 ⇒ `get_language()` 返回 ""
  ⇒ 主循环里那条"源语言 == 目标语言就跳过翻译"的守卫**永远不触发**

**为什么这条很要紧**：中文会议正是靠这条守卫才不被"中译中"。
用 `--lang zh` 指定语言（本来是"避免 auto 检测锁错 120 秒"的推荐做法）
会**恰好踩中这个 bug** —— 也就是说，为了躲一个坑会掉进另一个更隐蔽的坑。
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
DB = os.path.join(ROOT, r"build\RelWithDebInfo\passthrough_scratch.db")
WAV = r"C:\dev\projects\whisper.cpp\samples\jfk.wav"


def remove_db():
    for suffix in ("", "-wal", "-shm"):
        p = DB + suffix
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


def main():
    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建")
        return 1
    if not os.path.exists(WAV):
        print(f"[跳过] 找不到音频素材 {WAV}")
        print("       （它来自 whisper.cpp 的 samples，路径可按需改）")
        return 0

    remove_db()
    # 让程序自己建表
    subprocess.run([EXE, "--gaps", "--db", DB], capture_output=True,
                   text=True, encoding="utf-8", errors="replace")

    try:
        print("跑一段英文音频，源=目标（en→en）…")
        p = subprocess.run(
            [EXE, "--wav", WAV, "--lang", "en", "--target", "en",
             "--summarizer", "rules", "--no-ask",
             "--db", DB, "--out", os.path.join(ROOT, "out_passthrough")],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            cwd=ROOT)

        c = sqlite3.connect(DB)
        rows = c.execute(
            "SELECT seq, engine, src_text, tgt_text FROM segments ORDER BY seq"
        ).fetchall()
        c.close()

        if not rows:
            print("[失败] 没有任何段落落库 —— 音频没跑通？")
            print((p.stdout or "")[-800:])
            return 1

        bad_engine = [r for r in rows if r[1] != "passthrough"]
        bad_text = [r for r in rows if r[2] != r[3]]

        print(f"\n共 {len(rows)} 段：")
        for seq, eng, src, tgt in rows:
            mark = "✅" if (eng == "passthrough" and src == tgt) else "❌"
            changed = "" if src == tgt else f"   ← 被改成了 {tgt[:40]!r}"
            print(f"  {mark} #{seq} engine={eng}  {src[:48]!r}{changed}")

        print("\n=== 判定 ===")
        ok = True
        if bad_engine:
            ok = False
            print(f"❌ {len(bad_engine)} 段没走 passthrough（engine 不是 passthrough）"
                  f" —— 说明 get_language() 返回空、跳过翻译的守卫没触发")
        else:
            print("✅ 全部走 passthrough")
        if bad_text:
            ok = False
            print(f"❌ {len(bad_text)} 段的译文与原文不一致 —— 内容被改写了")
            for seq, eng, src, tgt in bad_text[:3]:
                print(f"     #{seq}: {src!r} → {tgt!r}")
        else:
            print("✅ 译文与原文逐字节相同（没有被改写）")

        print()
        print("== 全部通过 ==" if ok else "== 有项目未通过 ==")
        return 0 if ok else 1
    finally:
        remove_db()
        print("\n已删除一次性 scratch 库：" + os.path.abspath(DB))


if __name__ == "__main__":
    sys.exit(main())
