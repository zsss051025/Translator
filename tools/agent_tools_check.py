#!/usr/bin/env python3
"""
Agent 工具层的回归检查（§7 步骤 5.1）。

用法:
    python tools/agent_tools_check.py

它做什么
--------------------------------------------------------------------
对注册的每个只读工具**真跑一次**，并检查四件事：
  1. 工具名和 schema 都在（模型看不到 = 等于没注册）
  2. 结果能被 json.loads 解析（模型要按字段读；不是 JSON 就等于没结果）
  3. **限长真的生效**（不然长会话一次就撑爆上下文）
  4. 出错路径不崩：未知工具 / 参数不是 JSON / 目标不存在
     —— 这三种在 Agent 循环里都**一定会遇到**（模型会猜错名字、会吐半截 JSON），
        它们必须变成"回一句话给模型"，而不是异常

为什么要有这个脚本（而不是只在自检里断言）
--------------------------------------------------------------------
自检只能看到 C++ 里的返回值；这个脚本看到的是**经过进程边界和命令行之后**
模型实际会拿到的那串 JSON —— schema 拼歪、ensure_ascii 把中文转义丢了这类问题，
只有在这里才看得见。
"""

import json
import os
import subprocess
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\RelWithDebInfo\Translator.exe")
DB = os.path.join(ROOT, r"build\RelWithDebInfo\t.db")
OUT_DIR = os.path.join(ROOT, "deliverables")

EXPECTED = ["search_knowledge", "knowledge_history", "list_sessions",
            "get_session", "get_session_report", "list_actions",
            "propose_knowledge", "propose_action"]

failures = []


def run(args):
    p = subprocess.run([EXE] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", cwd=ROOT)
    return p.returncode, (p.stdout or ""), (p.stderr or "")


def call_tool(name, args_obj=None):
    """真跑一次工具，返回 (returncode, stdout, stderr)。"""
    a = ["--tools", name, "--db", DB, "--out", OUT_DIR]
    if args_obj is not None:
        a += ["--args", json.dumps(args_obj, ensure_ascii=False)]
    return run(a)


def extract_json(stdout):
    """工具的 JSON 结果打在 stdout 的最后一段（前面有 [Tools] 提示行）。"""
    lines = stdout.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].lstrip().startswith(("{", "[")):
            return "\n".join(lines[i:])
    return ""


def main():
    if not os.path.exists(EXE):
        print("[失败] 找不到 Translator.exe，先构建")
        return 1

    # 先看一眼有没有可读的纪要（没有的话 get_session_report 这条验不了"成功路径"）
    rep = os.path.join(OUT_DIR, "session-42", "meeting-42.md")
    print(f"预备：{'找到' if os.path.exists(rep) else '**没有**'} {rep}")
    if not os.path.exists(rep):
        print("      → 先跑：Translator.exe --export 42 --summarizer rules "
              f"--db {DB} --out deliverables")
    print()

    # ---- 1) 注册清单 ----
    rc, out, err = run(["--tools", "--db", DB])
    names = [l.strip()[1:].strip() for l in out.splitlines() if l.strip().startswith("·")]
    print(f"已注册 {len(names)} 个工具: {names}")
    for n in EXPECTED:
        if n not in names:
            failures.append(f"工具没注册：{n}")

    # schema 必须能被解析（拼歪了模型就不会调用）
    try:
        blob = extract_json(out)
        tools = json.loads(blob)
        got = [t["function"]["name"] for t in tools]
        print(f"tools 数组可解析，共 {len(tools)} 项: {got}")
        for t in tools:
            f = t["function"]
            if not f.get("description") or not f.get("parameters"):
                failures.append(f"{f['name']} 缺 description 或 parameters")
        if len(tools) != len(names):
            failures.append("tools 数组与工具数量不一致")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"tools 数组不是合法 JSON: {e}")

    # ---- 2) 逐个真跑 ----
    cases = [
        ("search_knowledge", {"query": "erica"}, "命中"),
        ("search_knowledge", {"query": "e", "limit": 3}, "限长"),
        ("knowledge_history", {"key": "erica"}, "变化"),
        ("list_sessions", {"days": 3650, "limit": 5}, "场会话"),
        ("get_session", {"session_id": 42, "max_segments": 3}, "段转录"),
        ("get_session_report", {"session_id": 42}, "纪要"),
        ("list_actions", {}, "行动项"),
        ("list_actions", {"status": "todo", "limit": 5}, "行动项"),
    ]
    print("\n=== 真跑 ===")
    for name, args, expect_sub in cases:
        rc, out, err = call_tool(name, args)
        js = extract_json(out)
        try:
            obj = json.loads(js)
        except Exception as e:                                 # noqa: BLE001
            failures.append(f"{name}({args}) 结果不是合法 JSON: {e}")
            print(f"  ❌ {name} {args} → 不是 JSON")
            continue
        audit = ""
        for l in out.splitlines():
            if l.startswith("[Tools] 摘要："):
                audit = l[len("[Tools] 摘要："):]
        ok = rc == 0 and isinstance(obj, (dict, list))
        size = len(js)
        mark = "✅" if ok else "❌"
        print(f"  {mark} {name} {args}")
        print(f"       摘要: {audit}   结果 {size} 字节")
        if size > 13000:
            failures.append(f"{name} 结果超过硬上限（{size} 字节）")

    # ---- 3) 限长 ----
    print("\n=== 限长 ===")
    rc, out, _ = call_tool("get_session", {"session_id": 43, "max_segments": 60})
    try:
        obj = json.loads(extract_json(out))
        print(f"  get_session(43, max=60): total={obj.get('total_segments')} "
              f"shown={obj.get('shown')} truncated={obj.get('truncated')}")
        if obj.get("shown", 0) > 60:
            failures.append("get_session 没有遵守 max_segments 上限")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"限长用例失败: {e}")

    # ---- 4) 出错路径：必须变成"回一句话"，不能崩 ----
    print("\n=== 出错路径 ===")
    rc, out, err = call_tool("no_such_tool", {})
    js = extract_json(out)
    try:
        obj = json.loads(js)
        avail = obj.get("available_tools", [])
        print(f"  未知工具 → rc={rc} 错误='{obj.get('error')}' 提示可用工具 {len(avail)} 个")
        if not avail:
            failures.append("未知工具时没把可用工具列表回给模型")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"未知工具路径没返回 JSON: {e}  stdout={out[-200:]!r}")

    # 参数不是合法 JSON
    a = ["--tools", "get_session", "--db", DB, "--args", '{"session_id":']
    rc, out, err = run(a)
    js = extract_json(out)
    try:
        obj = json.loads(js)
        print(f"  坏参数 → rc={rc} 错误='{obj.get('error')}'")
        if "JSON" not in obj.get("error", ""):
            failures.append("坏参数没有明确报'不是合法 JSON'")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"坏参数路径没返回 JSON: {e}  stdout={out[-200:]!r}")

    # 目标不存在
    rc, out, _ = call_tool("get_session", {"session_id": 999999})
    try:
        obj = json.loads(extract_json(out))
        print(f"  不存在的会话 → rc={rc} 错误='{obj.get('error')}'")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"不存在会话路径没返回 JSON: {e}")

    # propose_action 缺 title / title 过长 → 必须回一句话，不能写库
    rc, out, _ = call_tool("propose_action", {})
    try:
        obj = json.loads(extract_json(out))
        print(f"  propose_action 缺 title → rc={rc} 错误='{obj.get('error')}'")
        if rc == 0:
            failures.append("propose_action 缺 title 却报成功")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"propose_action 缺参数路径没返回 JSON: {e}")

    # ---- 5) propose_action 真写 + 幂等（5.5）----
    #
    # 【为什么这条要跨进程验】"写进去"和"再写一次不重复"是两件事，
    # 而后者只有真的落库再落一次才看得出来（内存里的行为可以完全正常）。
    # 用**固定标题**，所以重复跑这个脚本不会在库里堆垃圾。
    print("\n=== propose_action 落库与幂等 ===")
    probe = "ZZ toolcheck probe 行动项"
    rc1, out1, _ = call_tool("propose_action", {"title": probe, "evidence": "工具层自检"})
    rc2, out2, _ = call_tool("propose_action", {"title": probe, "evidence": "工具层自检"})
    try:
        o1 = json.loads(extract_json(out1))
        o2 = json.loads(extract_json(out2))
        print(f"  第一次: rc={rc1} inserted={o1.get('inserted')} merged={o1.get('merged')}")
        print(f"  第二次: rc={rc2} inserted={o2.get('inserted')} merged={o2.get('merged')}")
        if rc1 != 0 or rc2 != 0:
            failures.append("propose_action 真写失败")
        # 第二次必须是"并入已有"（inserted=0），这是幂等在返回值上的样子
        if o2.get("inserted") != 0 or o2.get("merged") != 1:
            failures.append(f"propose_action 第二次应为「并入已有」，实际 {o2}")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"propose_action 落库路径没返回 JSON: {e}")

    # 同一个标题必须只对应一条（幂等的可观测面）
    rc, out, _ = call_tool("list_actions", {"limit": 80})
    try:
        obj = json.loads(extract_json(out))
        hits = [i for i in obj.get("items", []) if i.get("title") == probe]
        print(f"  列表里同名条目: {len(hits)} 条（应为 1）")
        if len(hits) != 1:
            failures.append(f"propose_action 不幂等：同名条目 {len(hits)} 条")
        elif hits[0].get("origin") != "agent":
            failures.append(f"agent 提议的行动项 origin 应为 agent，实际 "
                            f"{hits[0].get('origin')!r}")
        elif hits[0].get("status") != "todo":
            failures.append(f"agent 提议的行动项状态必须是 todo，实际 "
                            f"{hits[0].get('status')!r}")
    except Exception as e:                                     # noqa: BLE001
        failures.append(f"list_actions 核对幂等时失败: {e}")

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
