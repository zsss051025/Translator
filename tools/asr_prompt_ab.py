# -*- coding: utf-8 -*-
"""识别腿 A/B：把知识库喂进 Whisper 的 initial_prompt，**识别是变好还是变坏**？

    python tools/asr_prompt_ab.py                # 跑全部档位（要加载 Whisper，约几分钟）
    python tools/asr_prompt_ab.py --sizes 0,4    # 只跑两档

---------------------------------------------------------------------------
【为什么这个实验必须做 —— 它是产品核心承诺唯一没验过的一环】
产品说「用得越久越懂你」。这句话的**机制落点**是：
用户确认过的专名 → 进 Whisper 的 `initial_prompt` → 下次识别偏向这些词。
而这一环从 2.5 做出来到现在，**只有"词进去了"的验证，没有"进去之后有没有用"的验证**
（`--terms` 只能证明那串词被拼出来了）。

更要紧的是它有一个**反向风险**：把一串词喂给 Whisper 是出了名的
会诱发**复读/幻觉**（Whisper 的 prompt 是"上文"，它倾向于续写上文）。
如果不验，可能出现这种情况：
    · 库里 5 个词 → 没问题
    · 库里 40 个词（`constraint_terms` 的上限）→ 识别开始把词表吐进转录
那时用户的感受是"**这东西用得越久越差**"，而这是我们最不可能自己发现的失败模式
（因为开发机上库里本来就没几个词）。

【判定口径：不许含糊】
用**不在音频里出现**的合成专名当探针。只要它们出现在转录里，就是
**确凿的 prompt 泄漏** —— 那份音频里根本没有人念过这些词。
反过来，"没泄漏"也不等于"有用"：这个实验只能证明**有没有害**。
要证明"有用"需要一段含易错专名的真实音频（见文档里的已知限制）。
所以脚本最后会明确区分这两件事，不会把"无害"说成"有效"。
"""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
WAV = r"C:\dev\projects\whisper.cpp\samples\jfk.wav"
WORK = os.path.join(ROOT, "build", "asr_ab")

# 探针词：**刻意全是编造的**（英文里不存在、中文里也不存在），
# 而且都是"首字母大写的一串字母"——正是专名的形状。
# 它们绝不可能出现在肯尼迪那 11 秒演讲里，所以一旦出现就是泄漏。
PROBES_EN = [
    "Zorbax", "Quillex", "Vantorr", "Mibrex", "Dralvex",
    "Kolthorn", "Praxil", "Yunvex", "Grimbolt", "Sephoraq",
]
# 再加一些中文侧的形状（用户确认过的中文项目名也会进同一个 prompt）
PROBES_ZH = ["凤凰项目", "极光平台", "星辰中台", "鲲鹏系统", "玄鸟平台"]


def make_db(path, n_terms, real_terms=None):
    """建一个库，塞进 n_terms 条 confirmed 知识。

    real_terms 非空时用它（**真实词表**），否则用合成探针词。
    【为什么要分两种】探针词是"胡言乱语"（编造的拉丁字母串 + 中文平台名），
    拿它当 prompt 属于最极端的情况。必须再验一遍**真实词表**，
    否则结论会被质疑成"只对合成噪声成立"。
    """
    for suffix in ("", "-wal", "-shm"):
        p = path + suffix
        if os.path.exists(p):
            os.remove(p)
    subprocess.run([EXE, "--terms", "--db", path], capture_output=True,
                   cwd=ROOT, timeout=120)
    import sqlite3
    con = sqlite3.connect(path)
    pool = real_terms if real_terms else (PROBES_EN + PROBES_ZH) * 4
    ts = "2026-09-19 10:00:00.000"
    for i in range(n_terms):
        v = pool[i % len(pool)]
        key = re.sub(r"\s+", "", v).lower()
        con.execute(
            "INSERT OR REPLACE INTO knowledge "
            "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
            " source_session,source_seq,source_text,asked_count,definition) "
            "VALUES ('term',?,?,'confirmed',0.9,3,?,?,NULL,NULL,NULL,0,NULL)",
            (key, v, ts, ts))
    con.commit()
    con.close()


def terms_from_real_db():
    """从真实库里取已确认的词（用户实际会喂进去的那一类：真实英文词 + 中文名）。"""
    import sqlite3
    p = os.path.join(ROOT, r"build\RelWithDebInfo\translations.db")
    if not os.path.exists(p):
        return None
    con = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
    vals = [r[0] for r in con.execute(
        "SELECT value FROM knowledge WHERE status='confirmed' "
        "ORDER BY hits DESC LIMIT 40")]
    con.close()
    return vals or None


def prompt_of(db, n_terms=0):
    """用 --terms 读出真实会喂进去的那串 prompt（几秒，不加载模型）。

    ⚠️ 必须和 `run_wav` 传**同一个** `--asr-prompt-kb`，否则这行显示会撒谎：
    它会说"prompt 是空的"，而实际跑的时候 prompt 是喂进去的 ——
    我第一版就是这样，差点把"两档一样"当成"prompt 无害"的证据。
    """
    args = [EXE, "--terms", "--db", db]
    if n_terms > 0:
        args += ["--asr-prompt-kb", str(n_terms)]
    p = subprocess.run(args, capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       cwd=ROOT, timeout=120)
    m = re.search(r"--- ① 识别提示（Whisper initial_prompt）---\n(.+)", p.stdout)
    return (m.group(1).strip() if m else ""), p.stdout


def run_wav(db, out_dir, n_terms=0):
    """跑一次 --wav，返回转录文本列表。

    ⚠️ **必须显式传 `--asr-prompt-kb`**：2026-09-19 起知识库**默认不进**识别提示
    （实测会漏字，见 tools/asr_prompt_ab.py 开头的说明）。所以"造一个带 N 条
    confirmed 知识的库"本身**不会**让它们进提示 —— 这个脚本第一版就栽在这里：
    两档跑出来一模一样，而屏幕上那行 prompt 明明写着"(空)"，
    我差点把"没差别"当成"prompt 无害"的证据。

    测试脚本和产品之间这种"默认值悄悄变了、脚本没跟上"，和
    「诊断工具撒谎」是同一类问题：**它不报错，只是测的东西不是你以为的那个。**
    """
    args = [EXE, "--wav", WAV, "--summarizer", "rules", "--db", db, "--out", out_dir]
    if n_terms > 0:
        args += ["--asr-prompt-kb", str(n_terms)]
    p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT, timeout=1800)
    import sqlite3
    con = sqlite3.connect(db)
    rows = [r[0] for r in con.execute(
        "SELECT src_text FROM segments WHERE session_id = "
        "(SELECT max(id) FROM sessions) ORDER BY seq")]
    con.close()
    return rows, p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="0,4,40",
                    help="逗号分隔的词条数（默认 0,4,40 —— 0 是基线，40 是产品上限）")
    ap.add_argument("--repeat", type=int, default=2,
                    help="每档跑几次（默认 2）。**必须 ≥2 才能区分'prompt 造成的差异'"
                         "和'Whisper 自己的运行噪声'** —— 否则会把随机波动报成效应")
    ap.add_argument("--real", action="store_true",
                    help="用**真实库里的词表**当 prompt（真实英文词/中文名），"
                         "而不是合成探针词 —— 用来排除「只对合成噪声成立」的质疑")
    a = ap.parse_args()
    sizes = [int(x) for x in a.sizes.split(",") if x.strip()]

    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe")
        return 1
    if not os.path.exists(WAV):
        print("[失败] 找不到测试音频 " + WAV)
        return 1

    real = terms_from_real_db() if a.real else None
    if a.real and not real:
        print("[失败] --real 需要真实库里已有 confirmed 词条")
        return 1

    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    print(f"音频: {WAV}（11 秒肯尼迪演讲，里面**没有**词表里的任何词）")
    if real:
        print(f"词表: 真实库里的 {len(real)} 个 confirmed 词"
              f"（{'、'.join(real[:8])}…）")
    else:
        print("词表: 合成探针词（编造的拉丁字母串 + 中文平台名）")
    print(f"档位: {sizes}；每档跑 {a.repeat} 次\n")

    results = {}
    for n in sizes:
        db = os.path.join(WORK, f"n{n}.db")
        make_db(db, n, real)
        prompt, terms_out = prompt_of(db, n)
        print("=" * 70)
        print(f"档位 N={n}：initial_prompt {len(prompt)} 字符"
              f"（近似 {len(prompt)//4}~{len(prompt)//2} token）")
        if prompt:
            print("  " + (prompt[:200] + ("…" if len(prompt) > 200 else "")))
        elif n > 0:
            # ⚠️ **这一行是个断言，不是提示**：造了 N 条 confirmed 知识，prompt 却是空的
            # ⇒ 说明产品侧"知识库进提示"是关着的，而这档测的其实是"无提示"。
            # 出现这行时**整份对照都不成立**，别把它当成"prompt 没影响"的证据。
            print("  ⚠️ 造了 %d 条 confirmed 知识，但 initial_prompt 是空的 ——" % n)
            print("     说明 --asr-prompt-kb 没生效，本档实际等于「无提示」，对照不成立！")
        runs = []
        for k in range(max(1, a.repeat)):
            rows, p = run_wav(db, os.path.join(WORK, f"out{n}_{k}"), n)
            text = "\n".join(rows)
            runs.append(text)
            print(f"  第 {k+1} 次：{len(rows)} 段 / {len(text)} 字符")
            print("    " + text.replace("\n", " ")[:260])
        results[n] = {"prompt": prompt, "runs": runs}

    # ---- 判定 ----
    print()
    print("=" * 70)
    print("判定")
    print("=" * 70)

    # ① 每一档内部：重复跑之间稳不稳？不稳的话**这一档的差异不能当效应**
    print("\n① 档内稳定性（重复跑同一档，转录是否一致）")
    unstable = set()
    for n in sizes:
        runs = results[n]["runs"]
        same = len(set(runs)) == 1
        if not same:
            unstable.add(n)
        print(f"  N={n}: {'稳定（每次一样）' if same else '**不稳定**（每次不同）'}"
              + ("" if same else f"  —— 这档的差异可能与 prompt 无关"))

    # ② 探针泄漏
    print("\n② 探针泄漏（探针词出现在转录里 = 确凿的 prompt 泄漏）")
    leaked_any = False
    for n in sizes:
        found = []
        for t in results[n]["runs"]:
            found += [w for w in PROBES_EN + PROBES_ZH if w in t]
        found = sorted(set(found))
        if found:
            leaked_any = True
        print(f"  {'❌' if found else '✅'} N={n}: "
              + (f"泄漏 {found}" if found else "没有探针词出现在转录里"))

    # ③ 与基线比
    base_n = sizes[0]
    base_runs = set(results[base_n]["runs"])
    print(f"\n③ 与基线 N={base_n} 比（每档取所有重复跑的结果集合）")
    for n in sizes[1:]:
        cur = set(results[n]["runs"])
        overlap = cur & base_runs
        print(f"  N={n}: 与基线{'有交集' if overlap else '**完全没有交集**'}"
              f"（{len(overlap)}/{len(cur)} 个结果在基线里出现过）")

    print()
    print("怎么读这份结果（**不要把无害说成有效，也不要把噪声说成效应**）：")
    print("  · 「没泄漏」只能证明这个档位**没有害**；它**不证明** initial_prompt 有用。")
    print("    要证明有用，需要一段**含易错专名的真实音频**。目前没有，")
    print("    所以「识别腿有效」这句话仍然**未经验证**。")
    print("  · 某一档**档内就不稳定** → 它和基线的差异不能算 prompt 造成的。")
    print("  · 档内稳定、但和基线完全不同，且**档位越高越差**（幻觉、复读、漏字）")
    print("    → 那是**确凿的产品问题**：知识库在污染识别。")
    print()
    print(f"（中间产物在 {WORK}；确认完可以删掉）")
    return 1 if leaked_any else 0


if __name__ == "__main__":
    sys.exit(main())
