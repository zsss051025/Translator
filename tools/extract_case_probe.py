#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抽取判据取证工具：把一组词的**每一次出现连同大小写和段号**列出来。

【为什么这是正式工具而不是临时脚本】
抽取规则 R1 的三条收紧判据（强形状 / 佐证 / 次数且从不小写）不是想出来的，
是**量出来的**：先跑一场真实会话，把 9 个假阳性和 4 个真名的每一次出现
全列出来，再看哪条信号能把两组分开。`PROJECT.md` §6.4① 那张表就是它的输出。
下次再要调抽取规则，第一件事还是跑这个 —— 不先看数据就改规则，
上一轮就是这么写出 8 个假阳性的。

用法
----
  python tools/extract_case_probe.py --db <库> --session 47 <词1> <词2> ...
  python tools/extract_case_probe.py --db build/RelWithDebInfo/t.db --session 47
      # 不给词就用内置的默认词表（就是 #47 那批假阳性 + 真名）

判定"是不是句首"的口径要和 C++ 那边**一致**：段首（含前导空白后）算句首。
这个脚本也顺带标出句末标点后的位置，方便人工核对。
"""
import argparse
import re
import sqlite3
import sys

DEFAULT_WORDS = [
    # #47 抽出过的假阳性
    "down", "keep", "downs", "midnight", "movies", "speaking",
    "learners", "exactly", "preview",
    # #47 里的真专名（对照组）
    "erica", "marco", "englishpod", "tv",
]

# 句末标点：这些字符之后重新起句，因此紧跟其后的首字母大写**不算证据**
SENT_END = ".!?。！？…"


def is_sentence_initial(text: str, pos: int) -> bool:
    i = pos - 1
    while i >= 0 and text[i] in " \t\u3000":
        i -= 1
    if i < 0:
        return True                      # 段首
    return text[i] in SENT_END           # 句末标点之后


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--session", type=int, required=True)
    ap.add_argument("words", nargs="*", default=None)
    a = ap.parse_args()

    words = a.words or DEFAULT_WORDS
    con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    segs = list(con.execute(
        "SELECT seq, src_text FROM segments WHERE session_id=? ORDER BY seq",
        (a.session,)))

    for w in words:
        print(f"=== {w} ===")
        # \w* 是为了把 keeping / Downs 这类变形也一起列出来 ——
        # 它们归一化后是**另一个 key**，判断 lower_seen 时会误导人
        pat = re.compile(r"\b" + re.escape(w) + r"\w*", re.IGNORECASE)
        forms = {}
        for seq, src in segs:
            src = src or ""
            for m in pat.finditer(src):
                forms.setdefault(m.group(0), []).append(
                    (seq, is_sentence_initial(src, m.start())))
        if not forms:
            print("   （本场没出现）")
        for form, hits in sorted(forms.items()):
            cap = form[0].isupper()
            n_init = sum(1 for _s, si in hits if si)
            n_mid = len(hits) - n_init
            label = "首字母大写" if cap else "全小写　　"
            detail = (f"段首={n_init} 非句首={n_mid}" if cap else "")
            print(f"   {form:<16} {label} 出现在 seq={[s for s, _ in hits]}  {detail}")
        print()
    print("口径：非句首大写次数 = 判据③的次数那一半；只要有任何一次全小写出现，"
          "判据③的'从不小写'那一半就不满足。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
