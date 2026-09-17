#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""长期记忆审计 / 清理（独立 oracle，不走 C++ 代码路径）。

为什么要独立：C++ 侧的 `--extract` / `--gaps` 都会经过抽取和确认逻辑，
用它们来检查"库里到底有什么"等于让被检查者自证清白。
这里直接用 sqlite3 读原始行。

用法
----
  python tools/knowledge_audit.py --db build/RelWithDebInfo/t.db
      # 只读：列出所有 status=confirmed / candidate 的条目，并标出可疑项

  python tools/knowledge_audit.py --db ... --archive-from-file bad.txt [--apply]
      # 把文件里列出的 key（每行一个，# 开头为注释）降级为 archived
      # 不给 --apply 就是 dry-run

⚠️ 安全约定
  * 只降级（→ archived），**绝不 DELETE**。删除是不可逆的；
    archived 不再进提示/约束（§6.5 红线），已经等于"不再影响行为"。
  * 拒绝在"看起来像正式库"以外的路径上跑？—— 不拒绝，但打印路径让操作者核对。
"""
import argparse
import os
import sqlite3
import sys

# 已知的假阳性（来自 2026-xx 会话 #46/#47 的抽取回归）。
# 保留在这里是为了让它成为**可复查的清单**，而不是散落在聊天记录里。
KNOWN_FALSE_POSITIVES = {
    "down", "downs", "keep", "midnight", "movies", "speaking",
    "learners", "exactly", "preview",
}

# 抽取这一类"英文播客里的大写普通词"时最容易踩的坑：
# 句首大写 + 全小写也常见 = 不是专名。
#
# ⚠️ 判断大小写要看 `value`（展示形），**不能看 `key`** ——
# `key` 是归一化后的小写，`Erica`/`TV` 的 key 都是 `erica`/`tv`。
# 我第一版拿 key 判大小写，把 4 个真名全标成可疑了。
def sus_reason(key: str, value: str) -> str:
    k = (key or "").strip()
    v = (value or "").strip()
    if k.lower() in KNOWN_FALSE_POSITIVES:
        return "已知假阳性（#46/#47 抽取回归）"
    # value 全是数字/空白 = 没有实际内容，不可能是术语
    if v and not any(ch.isalpha() or ord(ch) > 0x7F for ch in v):
        return f"value 无字母（{v!r}）—— 不可能是真术语"
    # 展示形也是全小写英文 = 更像普通词而非专名
    if v and v.isascii() and v.islower() and len(v) <= 12:
        return "全小写英文展示形（专名通常首字母大写）"
    return ""


def dump(db: str) -> int:
    if not os.path.exists(db):
        print(f"✗ 库不存在: {db}")
        return 2
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    print(f"库: {os.path.abspath(db)}")
    print()
    try:
        rows = list(con.execute(
            "SELECT id,kind,key,value,status,confidence,hits,source_session "
            "FROM knowledge ORDER BY CASE status "
            "WHEN 'confirmed' THEN 0 WHEN 'candidate' THEN 1 ELSE 2 END, hits DESC, id"
        ))
    except sqlite3.OperationalError as e:
        print(f"✗ 读取失败: {e}")
        return 1

    by = {}
    for r in rows:
        by.setdefault(r["status"], []).append(r)
    for st in ("confirmed", "candidate", "archived"):
        print(f"[{st}] {len(by.get(st, []))} 条")
    print()

    sus = []
    print("%-4s %-10s %-8s %-20s %-6s %-5s %-6s %s" %
          ("id", "status", "kind", "key", "hits", "conf", "src", "value"))
    print("-" * 104)
    seen = {}
    for r in rows:
        if r["status"] == "archived":
            continue
        seen.setdefault(r["key"], []).append(r["id"])
        why = sus_reason(r["key"], r["value"])
        flag = " ⚠ " + why if why else ""
        if why:
            sus.append(r)
        print("%-4s %-10s %-8s %-20s %-6s %-5s %-6s %s%s" % (
            r["id"], r["status"], r["kind"], r["key"], r["hits"],
            r["confidence"], r["source_session"] if r["source_session"] is not None else "-",
            (r["value"] or "")[:40], flag))

    dup = {k: v for k, v in seen.items() if len(v) > 1}
    if dup:
        print()
        print("⚠ 同一个 key 有多行（可能是不同 kind，也可能是重复）：")
        for k, ids in dup.items():
            kinds = [r["kind"] for r in rows if r["key"] == k and r["status"] != "archived"]
            print(f"   {k}: ids={ids} kinds={kinds}")

    print()
    if sus:
        print(f"⚠ {len(sus)} 条可疑：")
        for r in sus:
            print(f"   {r['key']}  ({r['status']}, hits={r['hits']}, src={r['source_session']})  {sus_reason(r['key'], r['value'])}")
        print()
        print("  可执行：")
        keys = " ".join(sorted({r["key"] for r in sus}))
        print(f"  python tools/knowledge_audit.py --db {db} --archive {keys} --apply")
    else:
        print("✅ 未发现可疑条目")

    # FTS 与主表是否一致（外部内容表最容易漂移）
    n_fts = list(con.execute("SELECT count(*) FROM knowledge_fts"))[0][0]
    print(f"\nFTS 行数: {n_fts}（主表 {len(rows)}）",
          "✅" if n_fts == len(rows) else "❌ 漂移，需要 rebuild")
    return 0


def archive(db: str, keys, apply: bool) -> int:
    if not os.path.exists(db):
        print(f"✗ 库不存在: {db}")
        return 2
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    hits = []
    for k in keys:
        rows = list(con.execute(
            "SELECT id,kind,status,value,hits FROM knowledge WHERE key = ?", (k,)))
        if not rows:
            print(f"  跳过 {k!r}：库里没有这个键")
            continue
        for r in rows:
            hits.append((r["id"], k, r["kind"], r["status"], r["value"], r["hits"]))
    if not hits:
        print("没有匹配的条目，什么都没做")
        return 1
    print(f"将降级 {len(hits)} 条 → archived：")
    for _id, k, kind, st, v, n in hits:
        print(f"  #{_id} {k}  (kind={kind}, {st}, hits={n}, value={v!r})")
    if not apply:
        print("\n（dry-run，未写入。加 --apply 才动库）")
        return 0
    con.execute("BEGIN")
    for _id, _k, _kind, _st, _v, _n in hits:
        con.execute(
            "UPDATE knowledge SET status='archived', updated_at=datetime('now') WHERE id=?",
            (_id,))
    con.commit()
    left = list(con.execute(
        "SELECT count(*) FROM knowledge WHERE status='confirmed'"))[0][0]
    print(f"\n✅ 已降级；剩余 confirmed = {left}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--archive", nargs="*", default=None, help="要降级为 archived 的 key")
    ap.add_argument("--archive-from-file", default=None, help="每行一个 key 的文件")
    ap.add_argument("--apply", action="store_true", help="真正写入（否则 dry-run）")
    a = ap.parse_args()

    keys = list(a.archive or [])
    if a.archive_from_file:
        with open(a.archive_from_file, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    keys.append(line)
    if keys:
        return archive(a.db, keys, a.apply)
    return dump(a.db)


if __name__ == "__main__":
    sys.exit(main())
