# -*- coding: utf-8 -*-
"""用**你自己的音频**校准分句参数，出一张对照表。

    python tools/segment_tune.py <音频.wav>
    python tools/segment_tune.py <音频.wav> --combos "500,4;300,4;700,4;500,6"
    python tools/segment_tune.py <音频.wav> --limit 180      # 只用前 3 分钟（快）

---------------------------------------------------------------------------
【为什么要有这个脚本】
分句参数（停多久算说完、连续说话到多久必须切）**不该由我在一段 11 秒的英文
单人演讲上拍板** —— 中文的停顿习惯、多人交替说话的节奏都跟它不一样。
但"自己跑几遍、肉眼看哪遍顺眼"也不是办法：那既不可比、也说不出理由。

所以把它变成一次可比的测量：同一段音频，只改参数，看几个**能算出来的指标**。

【四个指标，以及为什么是这四个】
  段数         —— 最直观，但**单独看会骗人**：窗口开得越大段数越少，
                  而我们已经实测过"窗口太大反而丢内容"（见下）
  总字符数     —— **防丢内容的护栏**。段数少了但总字符也少了 = 内容被吃掉了，
                  那不是"分句变好"，是"识别变差"。这两个必须一起看。
  疑似断句     —— 以功能词结尾的段（`…just got` / `…how` / `…and I'm`）。
                  这是碎片化**最直接的指纹**：一句话不会以 "the/of/to/how/了/的" 结尾。
  短段         —— ≤2 个词的段。碎片化的另一个指纹。

【怎么用结果】
  找「疑似断句 + 短段」最少、**同时总字符数没有明显下降**的那一组。
  如果某组段数骤降但总字符也骤降，那组是坏的（内容被丢），不是好的。
  ⚠️ 本脚本**只给指标，不给结论** —— 最后的判定要你自己听/读一遍，
     因为"分句好不好"最终是语义判断，指标只能把范围缩小。
"""
import argparse
import os
import re
import shutil
import sqlite3
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
WORK = os.path.join(ROOT, "build", "seg_tune")

# 功能词：**句子不会以它们结尾**（除非是被切断的）。
#
# ⚠️ 这份表**刻意排除代词和指示词**（you / it / that / this / them …）。
# 第一版把它们收进来了，结果 `Ask not what your country can do for you,`
# —— 一个**切在逗号上的正确切分** —— 被标成"疑似被切断"（因为它以 you 结尾），
# 于是脚本推荐了一个更差的参数组合。**指标错了会把结论带偏，比没有指标更坏。**
#
# 留下的都是"结尾就说明话没说完"的：连词、介词、冠词/物主代词、助动词、疑问词。
# （严格说介词也能结尾 —— "what are you looking for" —— 所以这仍然是个
#   有噪声的启发式，只用来**排序**，不用来下结论。脚本里也照实说了。）
FUNC_EN = set("""
and or but so because if when while that
of to in on at for with from by about into over under between during before after
the a an my your his her its our their
is are was were be been being am do does did have has had
will would shall should can could may might must
what how why where who whom whose which
not very more most than just
""".split())
FUNC_ZH = ["的", "了", "是", "在", "和", "与", "就", "也", "都", "很", "把", "被",
           "让", "给", "对", "从", "向", "为", "因为", "所以", "但是", "而且",
           "如果", "然后", "这个", "那个", "一个"]


def ends_with_function_word(text):
    t = text.strip().rstrip(".,!?;:，。！？；：、…\"'")
    if not t:
        return False
    # 中文：看结尾两个字
    for w in FUNC_ZH:
        if t.endswith(w):
            return True
    # 英文：最后一个词
    m = re.findall(r"[A-Za-z']+", t)
    if m and m[-1].lower() in FUNC_EN:
        return True
    return False


def run_one(wav, db, out_dir, endpoint_ms, max_sec, lang, target, extra):
    for suffix in ("", "-wal", "-shm"):
        p = db + suffix
        if os.path.exists(p):
            os.remove(p)
    args = [EXE, "--wav", wav, "--summarizer", "rules",
            "--db", db, "--out", out_dir,
            "--endpoint-ms", str(endpoint_ms), "--max-utter-sec", str(max_sec)]
    if lang:
        args += ["--lang", lang]
    if target:
        args += ["--target", target]
    args += extra
    p = subprocess.run(args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", cwd=ROOT, timeout=3600)
    if not os.path.exists(db):
        return None, p
    con = sqlite3.connect(db)
    sid = con.execute("SELECT max(id) FROM sessions").fetchone()
    rows = []
    if sid and sid[0]:
        rows = [(r[0], r[1]) for r in con.execute(
            "SELECT seq, src_text FROM segments WHERE session_id=? ORDER BY seq",
            (sid[0],))]
        dur = con.execute("SELECT (julianday(max(ts))-julianday(min(ts)))*86400 "
                          "FROM segments WHERE session_id=?", (sid[0],)).fetchone()[0]
    else:
        dur = 0
    con.close()
    return {"rows": rows, "dur": dur or 0}, p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--combos", default="500,4;300,4;700,4;500,6;900,4",
                    help="endpoint_ms,max_sec 用 ; 分隔（默认 5 组）")
    ap.add_argument("--lang", default="", help="源语言（如 en / zh）；不填=whisper 自动检测")
    ap.add_argument("--target", default="", help="目标语言（默认 zh）")
    ap.add_argument("--limit", type=int, default=0, help="只用前 N 秒（0=全部）")
    ap.add_argument("--keep", action="store_true", help="保留中间库")
    a = ap.parse_args()

    wav = a.wav
    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建")
        return 1
    if not os.path.exists(wav):
        print("[失败] 找不到音频：" + wav)
        return 1

    # --limit：截一段出来，避免每次都跑整场会议
    if a.limit > 0:
        import wave
        src = os.path.join(WORK, "clip.wav")
        shutil.rmtree(WORK, ignore_errors=True)
        os.makedirs(WORK, exist_ok=True)
        with wave.open(wav, "rb") as w:
            ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            data = w.readframes(min(n, fr * a.limit))
        with wave.open(src, "wb") as o:
            o.setnchannels(ch); o.setsampwidth(sw); o.setframerate(fr)
            o.writeframes(data)
        print(f"只取前 {a.limit} 秒 → {src}")
        wav = src

    os.makedirs(WORK, exist_ok=True)
    combos = []
    for c in a.combos.split(";"):
        c = c.strip()
        if not c:
            continue
        ms, sec = c.split(",")
        combos.append((int(ms), float(sec)))

    print(f"音频: {wav}")
    print(f"组合: {combos}")
    print()

    table = []
    for ms, sec in combos:
        tag = f"{ms}_{sec}".replace(".", "p")
        r, p = run_one(wav, os.path.join(WORK, f"t{tag}.db"),
                       os.path.join(WORK, f"out{tag}"),
                       ms, sec, a.lang, a.target, [])
        if r is None:
            print(f"  endpoint={ms} max={sec}: 跑失败")
            continue
        rows = r["rows"]
        texts = [t.strip() for _, t in rows]
        n = len(texts)
        total_chars = sum(len(t) for t in texts)
        broken = sum(1 for t in texts if ends_with_function_word(t))
        short = sum(1 for t in texts if len(re.findall(r"[A-Za-z']+|[\u4e00-\u9fff]", t)) <= 2)
        dur_min = (r["dur"] / 60.0) if r["dur"] > 0 else 0
        table.append({"ms": ms, "sec": sec, "n": n, "chars": total_chars,
                      "broken": broken, "short": short, "permin": (n / dur_min) if dur_min else 0,
                      "texts": texts})

    if not table:
        print("没有任何一组跑成功")
        return 1

    # ---- 对照表 ----
    print("=" * 78)
    print("对照表（同一段音频，只改分句参数）")
    print("=" * 78)
    print(f"{'endpoint':>9} {'max':>5} | {'段数':>5} {'总字符':>7} {'疑似断句':>9} {'短段':>5} {'段/分':>7}")
    print("-" * 78)
    for t in table:
        print(f"{t['ms']:>9} {t['sec']:>5} | {t['n']:>5} {t['chars']:>7} "
              f"{t['broken']:>9} {t['short']:>5} {t['permin']:>7.1f}")

    print()
    print("=" * 78)
    print("逐组转录（自己读一遍 —— 指标只能缩小范围，不能替你判断）")
    print("=" * 78)
    for t in table:
        print(f"\n--- endpoint={t['ms']}ms / max={t['sec']}s → {t['n']} 段 / "
              f"{t['chars']} 字符 / 疑似断句 {t['broken']} ---")
        for i, x in enumerate(t["texts"], 1):
            flag = "  ← 疑似被切断" if ends_with_function_word(x) else ""
            print(f"   {i:>2}. {x}{flag}")

    # ---- 提示（不是结论）----
    print()
    print("=" * 78)
    print("怎么挑（这是提示，结论要你自己下）")
    print("=" * 78)

    maxchars = max(t["chars"] for t in table)

    # ⚠️ **排序必须"内容优先"，不能"碎片优先"。**
    # 第一版是按「疑似断句 + 短段」最少来推荐的 —— 它推荐了丢内容的那一组，
    # 因为那一组的碎片指标刚好是 0。但两件事的严重性差得远：
    #   切得难看  → 读起来别扭，但意思都在
    #   内容被吃掉 → **用户永远不知道自己少听到了一句**
    # 所以：先要求总字符不掉队，再在其中挑碎片最少的。
    LOSS_TOL = 0.98          # 允许 2% 的字符波动（标点/分词差异）
    intact = [t for t in table if t["chars"] >= maxchars * LOSS_TOL]
    pool = intact if intact else table
    best = min(pool, key=lambda t: (t["broken"] + t["short"], -t["chars"]))

    print(f"  · 总字符最多的一组：{maxchars}（这是"内容有没有丢"的基准线）")
    if intact:
        print(f"  · 内容没掉队（≥ {int(maxchars * LOSS_TOL)} 字符）的组里，"
              f"碎片最少的是 endpoint={best['ms']}ms / max={best['sec']}s")
    else:
        print("  · ⚠️ 没有任何一组保住了 98% 的内容 —— 这本身是个信号：")
        print("     也许这段音频里 Whisper 的表现本身就不稳定，换参数救不了。")
    for t in table:
        if t["chars"] < maxchars * LOSS_TOL:
            print(f"    ⚠️ endpoint={t['ms']}/max={t['sec']}：总字符 {t['chars']}"
                  f"（比最多少 {100 - t['chars'] * 100 // maxchars}%）→ **疑似丢内容，别选它**")
    print()
    print("  ⚠️ 「疑似断句」是个**有噪声的启发式**：介词结尾在英文里可以是合法的")
    print("     （\"what are you looking for\"），所以它只用来排序，不用来下结论。")
    print("     **最终请把上面每组转录读一遍** —— 分句好不好最终是语义判断。")
    print()
    print("  确认某一组之后，它就是你的默认值，用这两个开关固定下来：")
    print(f"    --endpoint-ms {best['ms']} --max-utter-sec {best['sec']}")
    print()
    print(f"（中间产物在 {WORK}" + ("" if a.keep else "，跑完可以整个删掉") + "）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
