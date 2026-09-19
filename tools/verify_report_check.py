# -*- coding: utf-8 -*-
"""`--verify-report` 的跨进程回归（5.7）：全真 / 混编造 / 零引用。

【为什么必须专门造"假报告"来验】`--verify-report` 存在的唯一理由是
**抓出编造的出处**。而这恰恰是"正常跑一遍"永远验不到的东西：
真流程产出的引用都是对的，于是核对器永远打绿勾 ——
一个只会打绿勾的核对器，和没有核对没有区别。
所以这个脚本的核心是 B 和 C 两个**必须失败**的用例。

三份样本：
  A 全真            → exit 0，全绿
  B 混了编造的      → exit 1，且要**指名道姓**列出哪几处对不上
  C 一处出处都没有  → exit 2
    ⚠️ 零引用**不能**算通过：一份没有任何出处的报告是"没做这件事"，
       不是"做对了"。把它算成成功，等于给最该被质疑的那种报告发绿灯。

【为什么样本不写进仓库的 deliverables/】它刻意用假会话号（#8888），
混进真实交付物里会让人以为库里真有那一场。
"""
import io
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
DB = os.path.join(ROOT, r"build\RelWithDebInfo\tact.db")

# 样本引用的会话/段号：会话 9002 是 tools/seed_demo_week.py 造的演示会（4 段）
SEED_HINT = ("先造数据：python tools/seed_demo_week.py " + DB)

A_REAL = u"""# 演示会议纪要

## 行动项

| # | 事项 | 出处 |
|---|---|---|
| 1 | 重写接口文档 | [#9002·2](#seg-2) |
| 2 | 提交压测报告 | [#9002·3](#seg-3) |
"""

# 第 1 条真；第 2 条段号越界（那一场只有 4 段）；第 3 条整个会话都不存在
B_MIXED = u"""# 周报

- 接口文档重写是主线（#9002·2）。
- 压测报告由李经理负责（#9002·99）。
- 预算审批已通过（#8888·1）。
"""

C_NONE = u"""# 周报

- 接口文档重写是主线。
- 压测报告由李经理负责。
"""

failures = []


def run_verify(path):
    p = subprocess.run([EXE, "--verify-report", path, "--db", DB],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace", cwd=ROOT)
    return p.returncode, (p.stdout or ""), (p.stderr or "")


def stats(out):
    m = re.search(r"\[verify-report\] citations=(\d+) ok=(\d+) bad=(\d+)", out)
    return tuple(int(x) for x in m.groups()) if m else None


def main():
    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建")
        return 1
    if not os.path.exists(DB):
        print("[失败] 找不到 " + DB)
        print("       " + SEED_HINT)
        return 1

    # 样本目录放在 build/ 下（.gitignore 已覆盖），不用系统 TEMP：
    # 在受限沙箱里 TEMP 的临时目录会被拒写，而 build/ 一定可写。
    tmp = os.path.join(ROOT, "build", "_vr_samples")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)
    try:
        cases = [("A_all_real.md", A_REAL, 0, (2, 2, 0)),
                 ("B_mixed.md", B_MIXED, 1, (3, 1, 2)),
                 ("C_none.md", C_NONE, 2, (0, 0, 0))]

        for name, text, want_rc, want_stats in cases:
            p = os.path.join(tmp, name)
            io.open(p, "w", encoding="utf-8", newline="").write(text)
            rc, out, err = run_verify(p)
            got = stats(out)
            mark = "✅" if (rc == want_rc and got == want_stats) else "❌"
            print(f"{mark} {name}: exit={rc}（期望 {want_rc}）  {got}（期望 {want_stats}）")
            if rc != want_rc:
                failures.append(f"{name} 退出码 {rc}，期望 {want_rc}")
            if got != want_stats:
                failures.append(f"{name} 统计 {got}，期望 {want_stats}")

            if name.startswith("B"):
                # 编造的两处必须被**指名**（只报一个数字等于没说清哪里错了）
                for must in ("#9002·99", "#8888·1"):
                    if must not in out and must not in err:
                        failures.append(f"B 没有把无效出处 {must} 点出来")
                # 有效的那一处不该被误报
                if "#9002·2" in (err or ""):
                    failures.append("B 把有效出处 #9002·2 也报成了问题")

            if name.startswith("C"):
                # 零引用时**不许**出现"通过"字样
                if "处出处都指向" in out:
                    failures.append("C 零引用却报了「全部通过」——"
                                    "没有出处的报告不能算通过")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        print("== 有项目未通过 ==")
        for f in failures:
            print("  ❌ " + f)
        return 1
    print("== 全部通过 ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
