#!/usr/bin/env python3
"""
查看 SessionStore 生成的 SQLite 数据库。

用法:
    python tools/db_dump.py <数据库路径> [--session N]

它只读不写，用于人工核对每一场会话是否被正确记录。
Python 自带 sqlite3，不需要额外安装任何依赖。
"""
import argparse
import sqlite3
import sys

# Windows 控制台默认不是 UTF-8，显式切一下，避免中文输出乱码
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def human_ms(ms: int) -> str:
    return f"{ms} ms"


def dump(db_path: str, only_session: int | None) -> int:
    try:
        conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    except sqlite3.Error as e:
        print(f"[错误] 打不开数据库 {db_path}: {e}", file=sys.stderr)
        return 2

    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    # 表是否存在
    tables = {r[0] for r in cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    missing = {"sessions", "segments"} - tables
    if missing:
        print(f"[错误] 缺少表: {', '.join(sorted(missing))}", file=sys.stderr)
        print(f"       现有表: {', '.join(sorted(tables)) or '(无)'}", file=sys.stderr)
        return 2

    sql = ("SELECT s.id, s.started_at, COALESCE(s.ended_at,'') AS ended_at, "
           "       s.engine, s.note,"
           "       (SELECT COUNT(*) FROM segments g WHERE g.session_id = s.id) AS n "
           "FROM sessions s")
    params: tuple = ()
    if only_session is not None:
        sql += " WHERE s.id = ?"
        params = (only_session,)
    sql += " ORDER BY s.id"

    sessions = list(cur.execute(sql, params))
    if not sessions:
        print("(数据库里没有任何会话)")
        return 0

    total_segments = 0
    problems: list[str] = []

    for s in sessions:
        sid = s["id"]
        flag = "已结束" if s["ended_at"] else "!! 未结束"
        print("=" * 72)
        print(f"会话 #{sid}   [{flag}]")
        print(f"  引擎  : {s['engine']}")
        print(f"  备注  : {s['note'] or '(空)'}")
        print(f"  开始  : {s['started_at']}")
        print(f"  结束  : {s['ended_at'] or '(未结束)'}")
        print(f"  段落数: {s['n']}")

        segs = list(cur.execute(
            "SELECT seq, ts, src_text, tgt_text, engine, ms, confidence "
            "FROM segments WHERE session_id = ? ORDER BY seq", (sid,)))

        # 校验 seq 是否从 1 连续递增
        expected = list(range(1, len(segs) + 1))
        actual = [r["seq"] for r in segs]
        if actual != expected:
            problems.append(f"会话 #{sid}: seq 不连续 {actual}")

        if segs:
            lat = [r["ms"] for r in segs]
            confs = [r["confidence"] for r in segs if r["confidence"] >= 0]
            print(f"  延迟  : 总计 {human_ms(sum(lat))}  平均 {human_ms(sum(lat)//len(lat))}"
                  f"  最小 {human_ms(min(lat))}  最大 {human_ms(max(lat))}")
            if confs:
                print(f"  置信度: {len(confs)}/{len(segs)} 条有值，"
                      f"平均 {sum(confs)/len(confs):.3f}")
            else:
                print("  置信度: (本会话没有任何段落带置信度)")
            print("-" * 72)
            for r in segs:
                c = f"{r['confidence']:.2f}" if r["confidence"] >= 0 else "  - "
                print(f"  #{r['seq']:>3} [{r['ts']}] {r['engine']:<10} {r['ms']:>5}ms  conf={c}")
                print(f"        原文: {r['src_text']}")
                print(f"        译文: {r['tgt_text']}")
        else:
            print("  (本会话没有任何段落)")
        total_segments += len(segs)

    print("=" * 72)
    print(f"合计: {len(sessions)} 场会话, {total_segments} 条段落")

    if problems:
        print()
        for p in problems:
            print(f"[警告] {p}")
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="查看 SessionStore 数据库")
    ap.add_argument("db", help="SQLite 数据库路径")
    ap.add_argument("--session", type=int, default=None, help="只看指定会话 id")
    args = ap.parse_args()
    return dump(args.db, args.session)


if __name__ == "__main__":
    sys.exit(main())
