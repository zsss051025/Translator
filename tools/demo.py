# -*- coding: utf-8 -*-
"""一条命令演完全部能力（§7 步骤 5.8）。

    python tools/demo.py                     # 完整演示（不需要 API Key）
    python tools/demo.py --no-real           # 跳过"真实数据对照"那一幕
    $env:DEEPSEEK_API_KEY='...'; python tools/demo.py    # 多演一幕 agent 周报

---------------------------------------------------------------------------
【设计原则一：不加载 ASR 模型，也不碰声卡】
舞台上最贵、最容易出事的两件事就是"等模型加载"和"现场录音"：
  · Whisper large-v3 冷启动实测 ~100 秒，而且显存被占时可能直接失败
  · 录音要现场放音频、要有人对着麦克风说话，还要担心采集设备选错
所以这个脚本**全部用种子里已有的转录文本**走真实管道
（`--extract` / `--export` / `--report` 都是产品代码的真实路径，
不是为演示另写的捷径）。冷启动 0 秒，几分钟演完。
真实音频那条路另有一幕（第 6 幕）做对照，而且它是**只读**的。

【设计原则二：每一幕都自带"该看到什么"的断言】
演示脚本最怕的不是报错，是**悄悄演错**：屏幕上照样有输出，但内容和上一版不一样，
而讲的人不知道自己讲错了。所以每一幕跑完都核对关键结果，
对不上就当场标 ❌ 并说清期望是什么 —— 宁可当场难看，也不要台上悄悄讲错。

【设计原则三：缺什么就退什么，绝不中途崩】
没有 Key、没有真实库、没有模型 —— 每一种缺失都对应一句明确的降级说明，
并把该幕标成"跳过（原因）"，剩下的照常演。最后汇总表会列出哪些演了、哪些没演。

【为什么第 2 幕要"边问边答"而不是先把答案打进管道】
一开始我是把 4 个回答一次性 pipe 进去的，结果**答案分配错了**：
问题的顺序由内部优先级决定（实测是 极光平台 → 李经理 → 张伟 → 凤凰项目），
我按自己以为的顺序喂，于是给「张伟」答了凤凰项目的定义 ——
而屏幕上看起来完全正常（"记住了：张伟 是指 我们今年在做的交付项目"）。
这是**最坏的一类演示事故：它演得通，但演的是错的东西**。
所以改成按问题内容回答：读到哪一问，就答哪一问的答案；认不出的一律回 y。
"""
import argparse
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
SEED = os.path.join(ROOT, "tools", "seed_demo_week.py")
DEMO_DB = os.path.join(ROOT, r"build\RelWithDebInfo\tdemo.db")
REAL_DB = os.path.join(ROOT, r"build\RelWithDebInfo\translations.db")
OUT_DIR = os.path.join(ROOT, "out_demo")

# 演示会话的 id（seed_demo_week.py 定的，刻意避开真实会话的 id 段）
S1, S2, S3 = 9001, 9002, 9003

# 用户会教给它的含义。**按"问的是哪个词"回答**，不按顺序 —— 见文件头。
ANSWERS = {
    u"凤凰项目": u"我们今年在做的交付项目，给客户做的数据中台",
    u"张伟":     u"凤凰项目的后端负责人，张伟明",
    u"李经理":   u"数据平台的负责人，李国强",
    u"极光平台": u"公司内部的大数据平台，我们做数据接入用的",
}

results = []      # (幕号, 标题, 状态, 说明)


def hr(title):
    print()
    print("=" * 72)
    print(title)
    print("=" * 72)


def run(args, stdin_text=None, timeout=600):
    """跑一次 exe。返回 (rc, stdout, stderr)。"""
    p = subprocess.run([EXE] + args, input=stdin_text,
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace",
                       cwd=ROOT, timeout=timeout)
    return p.returncode, (p.stdout or ""), (p.stderr or "")


def record(act, title, ok, note):
    results.append((act, title, "✅ 演了" if ok else "⚠️ 跳过", note))


# ---------------------------------------------------------------------------
# 第 2 幕用的**交互式**确认：边读问题边回答
# ---------------------------------------------------------------------------
Q_SUBJECT = re.compile(r"(?:提到|说的是|叫)\s*([^\s。，,？?]+)")


def confirm_session(db, sid, tag):
    """跑一次 `--extract --apply --ask`，按问题内容逐条回答。

    返回 (rc, 全部输出, 教了哪些词)。
    """
    p = subprocess.Popen([EXE, "--extract", str(sid), "--apply", "--ask",
                          "--db", db, "--out", OUT_DIR],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace",
                         cwd=ROOT, bufsize=1)

    out_lines = []
    taught = []
    try:
        for line in p.stdout:               # 逐行读，问题一出来就答
            out_lines.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()

            m = Q_SUBJECT.search(line)
            if not m:
                continue
            subject = m.group(1).strip()
            ans = ANSWERS.get(subject)
            if ans is None:
                # 认不出的词一律 "y"（只记住写法，不编含义）——
                # **不要瞎给一个含义**，那会把演示变成"展示错误数据"。
                ans = "y"
            p.stdin.write(ans + "\n")
            p.stdin.flush()
            if subject in ANSWERS:
                taught.append(subject)
    finally:
        try:
            p.stdin.close()
        except Exception:                    # noqa: BLE001
            pass
        p.wait(timeout=120)
    return p.returncode, "".join(out_lines), taught


# ---------------------------------------------------------------------------
# 各幕
# ---------------------------------------------------------------------------
def act1_empty(db):
    hr("第 1 幕 · 从零开始：它现在什么都不认识")
    rc, out, err = run(["--terms", "--db", db])
    print(out[out.find("库:"):] if "库:" in out else out)
    ok = "confirmed: 0 条" in out
    print("\n>>> 该看到：confirmed: 0 条，三个清单都是 (空)。")
    print(">>> 这四个字很重要：**「第一次不知道」是「第二次知道」的前提**，"
          "而它一开始确实什么都不知道。")
    record(1, "从零开始", ok, "空库 · 识别提示/翻译约束均为空")
    return ok


def act2_learn(db):
    hr("第 2 幕 · 第一场会：它按类型问对问题，用户教给它")
    rc, out, taught = confirm_session(db, S1, "第一场")

    # 断言：**问题要按类型区分**。
    #
    # ⚠️ 这里刻意**不**断言某个具体句式。第一版断言的是
    # 「必须出现『项目中的一个重要概念』」，结果**配了 Key 之后这一幕就失败** ——
    # 因为分诊层的模型把「极光平台」判成了 product，于是问法换成了
    # 「这是你们的产品或者内部工具吗？」。**那是更对的行为，却被我的断言判成错**。
    # 教训：演示脚本的断言要钉**性质**（"问法按类型变了"），
    # 不要钉**具体措辞**（措辞会随模型判断改善而变）。
    person_q = u"这看起来是个人名" in out
    other_markers = [m for m in (u"项目中的一个重要概念", u"产品或者内部工具",
                                 u"概念", u"术语") if m in out]
    checks = []
    if not person_q:
        checks.append("没有出现人名问法（应问「这看起来是个人名 —— 他是谁？」）")
    if not other_markers:
        checks.append("没有出现非人名的类型化问法（项目/产品/概念）")
    if not taught:
        checks.append("一个词都没教进去（回答没被识别）")

    # 把"模型改了类型"这件事显式打出来 —— 这是本幕最好的画面之一：
    # 抽取器只会说"首字母大写的词/带后缀的词"，是分诊层的模型
    # 看着那句话把它改成 product / person 的。
    reclass = [l.strip() for l in out.splitlines() if u"类型:" in l]
    if reclass:
        print("\n>>> 注意上面这几行 —— 抽取器给的标签被**模型改过**：")
        for l in reclass:
            print(">>>   " + l)
        print(">>> （抽取器只会看形状，看不出「极光平台」是个产品；"
              "模型看得到那句话，所以能纠正它。）")

    print("\n>>> 该看到：")
    print(">>>   ① 问题按类型不同 —— 人名问「他是谁」，项目/产品问「具体指什么」")
    print(">>>   ② 回答之后当场回执「记住了：X 是指 Y」")
    print(">>> 实际教进去的：" + ("、".join(taught) if taught else "（无）"))
    for c in checks:
        print(">>>   ❌ " + c)
    record(2, "第一场：按类型提问 + 学会含义",
           not checks, "教会 " + ("、".join(taught) if taught else "无"))
    return not checks


def act3_reuse(db):
    hr("第 3 幕 · 第二次知道：学到的东西真的被用上了")
    rc, out, err = run(["--terms", "--db", db])
    print(out[out.find("库:"):] if "库:" in out else out)

    checks = []
    for w in (u"凤凰项目", u"张伟", u"李经理", u"极光平台"):
        # 出现在 ② 翻译约束 或 ③ 摘要背景 里才算"用上了"
        if w not in out:
            checks.append(f"{w} 没有出现在翻译约束/摘要背景里")
    print("\n>>> 该看到：第 2 幕教过的词，现在出现在 ② 翻译约束 和 ③ 摘要背景 里。")
    print(">>> 这两条腿是「越用越懂你」的落点：译文里的专名写法被统一，")
    print(">>> 纪要不会和用户确认过的事实矛盾；`TermFixer` 还会按同一份词表")
    print(">>> 在识别之后纠正文本（确定性，不会幻觉）。")
    if u"(空)" in out and u"识别提示" in out:
        # 这一条是**有意的设计**，不是故障 —— 说清楚，免得看的人以为是坏的
        print(">>>")
        print(">>> 注意 ① 识别提示是**(空)** —— 那是刻意的（2026-09-19 的实测决定）：")
        print(">>>   给 Whisper 喂和音频内容无关的词会让它**漏字/幻觉**，")
        print(">>>   而知识库是跨会议全局累积的，「无关」恰恰是常态。")
        print(">>>   想启用：--asr-prompt-kb N（详见 tools/asr_prompt_ab.py 的实测）")
    for c in checks:
        print(">>>   ❌ " + c)
    record(3, "复用：知识进翻译约束/摘要背景", not checks,
           "4 个词被用上" if not checks else "有词没被用上")
    return not checks


def act4_actions(db):
    hr("第 4 幕 · 一周三场：跨会话记住「上周说过什么」")
    if not os.path.exists(SEED):
        print("跳过：找不到 " + SEED)
        record(4, "跨会话行动项", False, "缺 seed 脚本")
        return False

    rc, out, err = run(["--export", str(S2), "--summarizer", "rules",
                        "--db", db, "--out", OUT_DIR])
    print("\n".join(l for l in out.splitlines()
                    if "[行动项]" in l or "[Export]" in l))
    rc, out, err = run(["--export", str(S3), "--summarizer", "rules",
                        "--db", db, "--out", OUT_DIR])
    print("\n".join(l for l in out.splitlines()
                    if "[行动项]" in l or "[Export]" in l))
    # ⚠️ **第一场也必须导出。**
    # 第 2 幕走的是 `--extract --apply --ask`，那条路**不生成交付物**，
    # 所以它的行动项还没落库。第一版漏了这一句，后果是"跨会话聚合没生效"——
    # 因为库里只有第二、三场的行动项，**根本没有两场可以合**。
    # （这正是演示脚本最该防的那种错：屏幕上一切正常，只有断言把它抓出来。）
    rc, out, err = run(["--export", str(S1), "--summarizer", "rules",
                        "--db", db, "--out", OUT_DIR])
    print("\n".join(l for l in out.splitlines()
                    if "[行动项]" in l or "[Export]" in l))

    rc, out, err = run(["--actions", "--db", db])
    print(out[out.find("库:"):] if "库:" in out else out)

    checks = []
    if u"场会话里被提到" not in out:
        checks.append("没有任何行动项被标记为「在 N 场会话里被提到」"
                      "（跨会话聚合没生效）")
    if "[actions] total=" not in out:
        checks.append("没有输出 [actions] 汇总行")
    print("\n>>> 该看到：同一个任务在两场会里都出现 → 只留一条，"
          "并标注「（在 2 场会话里被提到，最近 #9002）」。")
    print(">>> 反过来，第三场没提的任务**仍然留在待办里** —— "
          "这正是「上周定的事这周还记得」。")
    for c in checks:
        print(">>>   ❌ " + c)
    record(4, "跨会话行动项聚合", not checks,
           "含多会话标注" if not checks else "聚合未生效")
    return not checks


def act5_report(db, key):
    hr("第 5 幕 · 生成周报（agent 自己查证 + 出处可核对）")
    goal = u"整理凤凰项目这一周的工作并生成周报"

    if not key:
        # ---- 兜底：没有 Key 也要产出 ----
        print("⚠️ 没有 DEEPSEEK_API_KEY → 多步规划那一路**用不了**，走兜底。")
        print("   兜底不是「什么都不做」，是换成两条不需要 Key 的路：")
        print("   ① --search：单次检索长期记忆（agent 的退化情形，同一份工具代码）")
        rc, out, _ = run(["--search", u"凤凰项目", "--db", db])
        print("\n".join(out.splitlines()[-6:]))
        print("\n   ② --export：为三场会生成交付物（纪要/行动项 CSV/网页/字幕）")
        for sid in (S1, S2, S3):
            rc, out, _ = run(["--export", str(sid), "--summarizer", "rules",
                              "--db", db, "--out", OUT_DIR])
            print("      " + "\n      ".join(
                l for l in out.splitlines() if "[Export]" in l and u"已导出" in l))
        print("\n>>> 该看到：即使没有 Key、完全不联网，也能产出可交付的周报素材。")
        print(">>> 这是**能力降级**，不是失败 —— 要设 Key 就再演一次这一幕。")
        record(5, "周报（兜底路径）", True, "无 Key → --search + --export")
        return True

    rc, out, err = run(["--report", goal, "--db", db, "--out", OUT_DIR],
                       timeout=900)
    # 只把结果和核对打出来（执行过程较长，舞台上可以滚动看）
    i = out.find("-------- 结果 --------")
    print(out[i:] if i >= 0 else out)

    checks = []
    m = re.search(r"出处核对：共 (\d+) 处，有效 (\d+) 处，对不上 (\d+) 处", out)
    if not m:
        checks.append("没有输出出处核对结果（5.7 的核对没跑？）")
    else:
        total, ok_n, bad = (int(x) for x in m.groups())
        if bad:
            checks.append(f"有 {bad} 处出处对不上 —— 报告里有编造的位置")
        if ok_n == 0:
            checks.append("一处有效出处都没有（模型没标，或格式没对齐）")
    print("\n>>> 该看到：")
    print(">>>   ① 执行过程里有多次工具调用（不是一问一答）")
    print(">>>   ② 结论后面带 `#会话·段号`")
    print(">>>   ③ 最后一行是出处核对，且「对不上 0 处」")
    for c in checks:
        print(">>>   ❌ " + c)
    record(5, "周报（agent + 出处核对）", not checks,
           (m.group(0) if m else "无核对输出"))
    return not checks


def act6_real(db):
    hr("第 6 幕 · 真实数据对照：不是只有种子数据能跑")
    if not os.path.exists(REAL_DB):
        print("跳过：找不到真实库 " + REAL_DB)
        print("     （那是你自己录的会话；没有它就只能演种子数据）")
        record(6, "真实数据对照", False, "没有真实库")
        return False

    # 只读！绝不往用户的真实库里写东西
    rc, out, _ = run(["--search", u"EnglishPod", "--db", REAL_DB])
    print("① 在**真实库**上检索（只读）：")
    print("\n".join("   " + l for l in out.splitlines()[-5:]))

    rc, out, _ = run(["--actions", "--db", REAL_DB])
    print("\n② 真实库的行动项：")
    print("\n".join("   " + l for l in out.splitlines()
                    if u"行动项:" in l or "[actions]" in l))

    print("\n③ 闭环证据（把库里的跨会话弧线排出来）：")
    rc, out, _ = run(["python"], timeout=10) if False else (0, "", "")
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "loop_evidence.py"),
                        "--db", REAL_DB, "--last", "1"],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT)
    tail = [l for l in (p.stdout or "").splitlines()
            if u"③b" in l or u"交叉核对" in l or u"识别提示" in l]
    print("\n".join("   " + l for l in tail[:6]))

    ok = bool(tail)
    print("\n>>> 该看到：同样的机制在你**自己录的会**上也在跑 —— "
          "上面这些不是种子数据。")
    print(">>> （本幕全部只读，不会改动真实库。）")
    record(6, "真实数据对照（只读）", ok, "真实库 20 场 / 506 段")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEMO_DB)
    ap.add_argument("--key", default=os.environ.get("DEEPSEEK_API_KEY", ""))
    ap.add_argument("--no-real", action="store_true", help="跳过第 6 幕")
    a = ap.parse_args()

    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建：")
        print("       cmake -S . -B build && cmake --build build --config RelWithDebInfo")
        return 1

    hr("环境检查（缺什么都会说明，不会中途崩）")
    print(f"程序      : {EXE}")
    print(f"演示库    : {a.db}")
    print(f"真实库    : {REAL_DB}  {'（有）' if os.path.exists(REAL_DB) else '（没有）'}")
    print(f"API Key   : {'已设置（会多演 agent 周报那一幕）' if a.key else '未设置（第 5 幕走兜底）'}")
    print("ASR 模型  : 本演示**不需要**（不加载、不录音，0 秒冷启动）")

    # 重置演示库：每次演都从同一个干净状态开始
    for suffix in ("", "-wal", "-shm"):
        p = a.db + suffix
        if os.path.exists(p):
            os.remove(p)
    run(["--terms", "--db", a.db])          # 建 schema
    p = subprocess.run([sys.executable, SEED, a.db], capture_output=True,
                       text=True, encoding="utf-8", errors="replace", cwd=ROOT)
    print("\n种子数据  : " + (p.stdout or "").strip())

    if a.key:
        os.environ["DEEPSEEK_API_KEY"] = a.key

    act1_empty(a.db)
    act2_learn(a.db)
    act3_reuse(a.db)
    act4_actions(a.db)
    act5_report(a.db, a.key)
    if not a.no_real:
        act6_real(a.db)

    hr("汇总")
    for act, title, status, note in results:
        print(f"  第 {act} 幕  {status}  {title}")
        print(f"            {note}")
    skipped = [r for r in results if r[2].startswith("⚠️")]
    print()
    if skipped:
        print(f"共 {len(results)} 幕，跳过 {len(skipped)} 幕（原因见上）。")
    else:
        print(f"共 {len(results)} 幕，全部演完。")
    print()
    print("交付物在：" + OUT_DIR)
    print("  每个会话一个目录：session-<id>/meeting-<id>.md / .html / actions.csv / transcript.srt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
