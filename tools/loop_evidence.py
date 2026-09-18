#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""闭环证据：从库里还原"第 N 场学会什么、下一场用上了什么"。

【为什么需要它 —— 它是演示的**可验证面**】
"越用越懂你"是个**跨会话**的承诺，而任何单独一场会话的记录都看不出它：
一场会里只有转录和译文，与上一场的关系不在里面。
评审（或你自己）要信这件事，唯一办法是**把三场按顺序摆在一起**，指出
    第 1 场问了这个 → 第 2 场它进了识别提示 → 第 3 场它还在用
所以这个工具不产出任何新数据，它只把库里**已经存在**的证据按时间线排出来。

【数据来源，全部是库里的原始行，不重新推断】
  · sessions          哪几场、什么时候、多少段
  · knowledge         每条知识"是哪一场贡献的"（source_session）
  · knowledge_history **只追加**的变化流水（含时间戳）—— 这是弧线的骨架
  · Translator.exe --extract <id>   还原"那一场抽出了什么候选"（只读）

⚠️ 抽取那一步刻意**调同一个 exe**，而不是在 Python 里重写一遍规则：
   重写一份就等于有两个实现，两边迟早走散（本项目已经栽过三次）。

用法
----
  python tools/loop_evidence.py --db <库> [--last N] [--exe <Translator.exe>]

  --last N   只看最近 N 场（默认全部真实会话，不含 engine=SelfTest 的自检会话）
"""
import argparse
import os
import re
import sqlite3
import subprocess
import sys

SEP = "=" * 78


def q(con, sql, args=()):
    return list(con.execute(sql, args))


def real_sessions(con, last):
    rows = q(con, "SELECT id, started_at, ended_at, engine, "
                  "(SELECT count(*) FROM segments s WHERE s.session_id = sessions.id) "
                  "FROM sessions WHERE engine <> 'SelfTest' ORDER BY id")
    if last:
        rows = rows[-last:]
    return rows


def extract_candidates(exe, db, sid):
    """调真实 exe 做只读抽取，解析它的输出。返回 [(kind, value, hits, why)]。"""
    if not exe or not os.path.exists(exe):
        return None
    try:
        out = subprocess.run([exe, "--extract", str(sid), "--db", db],
                             capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=300)
    except Exception as e:              # noqa: BLE001
        return [("?", "抽取失败: " + str(e), 0, "")]
    cands = []
    for line in (out.stdout or "").splitlines():
        m = re.match(r"\s*\[(\w+)\]\s+(\S+)\s+hits=(\d+)\s+conf=[\d.]+\s+seq=\d+\s+依据:\s*(.*)$",
                     line)
        if m:
            cands.append((m.group(1), m.group(2), int(m.group(3)), m.group(4)))
    return cands


def knowledge_at(con, sid):
    """到第 sid 场为止，库里**已经确认**且能进识别提示/翻译约束的词。

    ⚠️ 这是**近似重建**，不是当时的快照：库里不存历史快照，
       `status` 是"现在"的值。所以下面会显式标注近似，并且只在
       "之后没被降级过"的前提下成立 —— 把近似说成精确是更坏的行为。
    """
    rows = q(con, "SELECT value, kind, definition, source_session FROM knowledge "
                  "WHERE status='confirmed' AND COALESCE(source_session, 0) <= ? "
                  "ORDER BY hits DESC, id", (sid,))
    return rows


def terms_from_exe(exe, db):
    """跑真实的 `--terms`，拿"这台机器现在这次启动会喂什么"。

    【为什么必须调 exe，而不是在这里再算一遍】
    本文件开头已经写过这条原则（抽取那步调同一个 exe），但 ③ 当初违反了它：
    自己用 SQL 拼了一个"下一场的 initial_prompt 里会有这些词"，**漏了去重**。
    于是它打印出 `Marco、EnglishPod、Erika、Erika、VAD` ——
    而真实路径（`constraint_terms()`，按 value 大小写不敏感去重）只会给出
    一个 `Erika`。同一个 key 存成 person 和 term 两行时就会这样。

    后果比"显示难看"严重：**这个工具是演示的"可验证面"**。
    它撒谎的方向恰恰是"看起来库是坏的"，而库其实是好的 ——
    和 run_dump_prompt 那两次"诊断工具自己撒谎"是同一类事故。
    所以 ③ 只负责"历史上大概是什么"（近似，明确标注），
    而"现在真实会喂什么"一律从 exe 拿，见 ③b。

    返回 (terms, background, counts) 或 None（找不到 exe / 跑失败）。
    """
    if not exe or not os.path.exists(exe):
        return None
    try:
        out = subprocess.run([exe, "--terms", "--db", db],
                             capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=120)
    except Exception:                   # noqa: BLE001
        return None
    if out.returncode != 0:
        return None

    terms, background, counts = [], [], ""
    grab_next = False
    for line in (out.stdout or "").splitlines():
        if grab_next:
            terms = [t.strip() for t in line.split(",") if t.strip()]
            grab_next = False
        elif "① 识别提示" in line:
            grab_next = True
        elif line.strip().startswith("- ") and "（" in line:
            background.append(line.strip()[2:])
        elif line.startswith("[terms]"):
            counts = line.strip()
    return terms, background, counts


def history_for(con, sid, next_sid):
    """第 sid 场到下一场之间发生的知识变化（用时间窗归属，精确到时间戳）。"""
    if next_sid is None:
        rows = q(con, "SELECT h.changed_at, h.reason, h.old_value, h.new_value, "
                      "k.kind, k.value FROM knowledge_history h "
                      "JOIN knowledge k ON k.id = h.knowledge_id "
                      "WHERE h.changed_at >= (SELECT started_at FROM sessions WHERE id=?) "
                      "ORDER BY h.changed_at", (sid,))
    else:
        rows = q(con, "SELECT h.changed_at, h.reason, h.old_value, h.new_value, "
                      "k.kind, k.value FROM knowledge_history h "
                      "JOIN knowledge k ON k.id = h.knowledge_id "
                      "WHERE h.changed_at >= (SELECT started_at FROM sessions WHERE id=?) "
                      "  AND h.changed_at <  (SELECT started_at FROM sessions WHERE id=?) "
                      "ORDER BY h.changed_at", (sid, next_sid))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--last", type=int, default=0)
    ap.add_argument("--exe", default=None)
    a = ap.parse_args()

    if not os.path.exists(a.db):
        print(f"库不存在: {a.db}")
        return 2

    exe = a.exe
    if exe is None:
        guess = r"build\RelWithDebInfo\Translator.exe"
        exe = guess if os.path.exists(guess) else None

    con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    sessions = real_sessions(con, a.last)
    if not sessions:
        print("库里没有真实会话（只有自检会话）。")
        return 1

    print(f"库: {os.path.abspath(a.db)}")
    print(f"真实会话 {len(sessions)} 场"
          + (f"（只显示最近 {a.last} 场）" if a.last else ""))
    print()

    for i, (sid, started, ended, engine, nseg) in enumerate(sessions):
        next_sid = sessions[i + 1][0] if i + 1 < len(sessions) else None
        print(SEP)
        print(f"会话 #{sid}   {started} ~ {ended or '(未结束)'}   {nseg} 段   engine={engine}")
        print(SEP)

        # ① 这一场学到了什么（抽取调真实 exe）
        cands = extract_candidates(exe, a.db, sid)
        print("① 这一场抽出的候选（跑真实抽取器）：")
        if cands is None:
            print("   （没找到 Translator.exe，跳过 —— 用 --exe 指定）")
        elif not cands:
            print("   （没有）")
        else:
            for kind, value, hits, why in cands:
                print(f"   [{kind}] {value}  ×{hits}   依据: {why}")

        # ② 这一场之后库里多了什么（历史流水，时间戳精确）
        hist = history_for(con, sid, next_sid)
        print(f"\n② 这一场到下一场之间发生的变化（knowledge_history，{len(hist)} 条）：")
        if not hist:
            print("   （没有 —— 也就是说这一场没让系统学到新东西）")
        for changed, reason, oldv, newv, kind, value in hist:
            if reason == "definition_defined":
                print(f"   {changed[11:19]}  学到含义：{value} = {newv}")
            else:
                print(f"   {changed[11:19]}  {reason}: {value}  {oldv!r} -> {newv!r}")

        # ③ 到这一场为止，下一次开会会带上的东西
        kk = knowledge_at(con, sid)
        print(f"\n③ 到这一场结束时，下次开会能用的知识（{len(kk)} 条，**近似重建**）：")
        if not kk:
            print("   （空 —— 下一场的识别提示和翻译约束都是干净的）")
        for value, kind, definition, src in kk:
            extra = f"：{definition}" if definition else ""
            print(f"   - {value}（{kind}）{extra}   ← 来自 #{src}")
        if kk:
            # 词条要按 value 去重（同一个 key 可能存成 person/term 两行，
            # 真实路径 constraint_terms() 就是这么收敛的）。不去重就会打印出
            # `Erika、Erika`，让好库看起来像坏的 —— 这正是本工具踩过的坑。
            seen, terms = set(), []
            for value, _k, _d, _s in kk:
                low = value.lower()
                if low in seen:
                    continue
                seen.add(low)
                terms.append(value)
            print(f"   下一场的 initial_prompt 里会有这些词：{'、'.join(terms)}")
            print("   ⚠️ 上面这行是**我自己拼的近似**；精确答案看末尾的 ③b。")

        # ④ 这一场自己又被问过什么（gaps 是"现在"算的，所以只作提示）
        print()

    # ③b 真实路径：直接问 exe「现在这次启动会喂什么」。
    #
    # 这一段存在的唯一理由：③ 是近似重建，而**演示的说服力不能建立在近似上**。
    # 评审问"你怎么证明第二场真的收到了这个词"，答案必须是
    # 「不用证明，这是程序自己打印的」。
    print(SEP)
    print("③b 真实路径（直接跑 `Translator.exe --terms`，不是重建）")
    print(SEP)
    real = terms_from_exe(exe, a.db)
    if real is None:
        print("   （没找到 Translator.exe 或它跑失败了 —— 用 --exe 指定）")
    else:
        rterms, rbg, rcounts = real
        print(f"   {rcounts}")
        print(f"   ① 识别提示（Whisper initial_prompt）："
              f"{', '.join(rterms) if rterms else '(空)'}")
        print(f"   ③ 摘要背景 {len(rbg)} 行：")
        for b in rbg:
            print(f"      - {b}")

        # 交叉核对：重建 vs 真实。**只在最后一场上比**才有意义 ——
        # --terms 反映的是"现在"，只有"到最新一场为止"和它是同一个时点。
        if sessions:
            last_kk = knowledge_at(con, sessions[-1][0])
            seen, mine = set(), []
            for value, _k, _d, _s in last_kk:
                low = value.lower()
                if low not in seen:
                    seen.add(low)
                    mine.append(value)
            if mine == rterms:
                print(f"\n   ✅ 交叉核对：近似重建 == 真实路径（{len(mine)} 个词条）")
            else:
                print(f"\n   ⚠️ 交叉核对不一致：重建 {mine} vs 真实 {rterms}")
                print("      差异通常来自此后的 status 变化或库被外部改动，"
                      "以 ③b 为准。")

    print(SEP)
    print("怎么读这份输出（演示时的讲法）")
    print(SEP)
    print("  ① 是「输入」：这一场有什么陌生词。")
    print("  ② 是「学习」：用户确认/纠正之后，库里真的变了什么（含学到含义）。")
    print("  ③ 是「复用」：下一场开局就带上的东西 —— 这就是「第二次知道」的证据。")
    print("  ③b 是**真实路径**：程序自己打印的「现在会喂什么」，不是重建。")
    print()
    print("  ⚠️ ③ 是**近似重建**：库里不存历史快照，status 是「现在」的值。")
    print("     要精确证明「当时就是这样」，看 ② 的时间戳（那是只追加的流水）。")
    print("     要精确证明「现在真的喂进去了」，看 ③b —— 或启动日志的 [识别提示]。")
    print("  ⚠️ ④ 故意没做：缺口检测依赖「当前」状态，回放它需要给库做快照。")
    print("     真跑演示时，当场看控制台的确认交互即可（那才是真实路径）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
