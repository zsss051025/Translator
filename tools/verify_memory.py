#!/usr/bin/env python3
"""
校验长期记忆（知识库）的数据层是否真的可用。

用法:
    python tools/verify_memory.py <数据库路径>

它做三件事，而且**不改动原库**（全程在一个事务里，最后回滚）：

  1. 列出所有表，检查四张长期记忆表在不在
       knowledge / knowledge_history / actions / knowledge_fts

  2. 功能验证：往 knowledge 插两条记录 → 重建 FTS5 索引 →
     用 MATCH 查回来。**这一步才是"FTS5 真的能用"的证据** ——
     只看到表存在是不够的，外部内容表还要能索引和检索。

  3. 迁移验证：如果这是一个改动前建的旧库，用它跑一遍
     `Translator.exe --selftest --db <库>` 之后，这四张表应该自动出现
     （建表语句全是 IF NOT EXISTS，所以老库打开即迁移）。

为什么要用 Python 而不是程序自己的自检：
    这是**独立实现**的验证 —— Python 自带的 sqlite3 和项目里 vendored 的
    sqlite3.c 是两套完全不同的构建。两套都能用 FTS5，才说明"能建表"
    不是靠某个侥幸的编译选项，而是数据格式本身是对的。
"""

import argparse
import sqlite3
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

MEMORY_TABLES = ["knowledge", "knowledge_history", "actions", "knowledge_fts"]


def list_tables(conn):
    rows = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name"
    ).fetchall()
    return [r[0] for r in rows]


def check_tables(conn):
    tables = list_tables(conn)
    print("库里共有 %d 张表:" % len(tables))
    for t in tables:
        mark = "  <-- 长期记忆" if t in MEMORY_TABLES else ""
        print("   " + t + mark)

    missing = [t for t in MEMORY_TABLES if t not in tables]
    if missing:
        print("\n[缺少] " + ", ".join(missing))
        print("提示: 若刚改过 CMakeLists.txt 的 SQLITE_ENABLE_FTS5，")
        print("      只 build 不生效，必须重新 configure。")
        return False
    print("\n四张长期记忆表都在")
    return True


def functional_fts5_check(conn):
    """
    在事务里插入 + 建索引 + 检索，最后回滚 —— 原库一个字节都不变。
    """
    print("\nFTS5 功能验证（插两条 -> 重建索引 -> MATCH 检索 -> 回滚）")
    conn.execute("BEGIN")
    try:
        rows = [
            ("person", "erika", "Erika", "confirmed", 3,
             "Hello, I'm Erika. How are you, Erika?"),
            ("term", "englishpod", "EnglishPod", "candidate", 1,
             "Welcome to EnglishPod. My name is Marco."),
        ]
        for kind, key, value, status, hits, src in rows:
            conn.execute(
                "INSERT INTO knowledge"
                "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
                " source_session,source_seq,source_text) "
                "VALUES (?,?,?,?,?,?,datetime('now'),datetime('now'),NULL,NULL,?)",
                (kind, key, value, status, 0.9, hits, src),
            )

        # 外部内容表（content='knowledge'）不会自动跟着变，
        # 要么建触发器、要么显式 rebuild。2.2 的 KnowledgeStore 会做同步，
        # 这里用 rebuild 先把"索引能建起来、能查"这件事验掉。
        conn.execute("INSERT INTO knowledge_fts(knowledge_fts) VALUES('rebuild')")

        hits = conn.execute(
            "SELECT key, value FROM knowledge_fts WHERE knowledge_fts MATCH ? "
            "ORDER BY key",
            ("erika",),
        ).fetchall()
        print("    MATCH 'erika' -> " + repr(hits))

        if not hits:
            print("[失败] FTS5 检索不到刚索引的内容")
            return False
        if hits[0][0] != "erika":
            print("[失败] 检索结果不符合预期")
            return False
        print("[通过] FTS5 索引与检索都正常")
        return True
    finally:
        conn.execute("ROLLBACK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db", help="SQLite 数据库路径")
    args = ap.parse_args()

    try:
        conn = sqlite3.connect(args.db)
    except sqlite3.Error as e:
        print("打不开数据库: " + str(e))
        return 1

    try:
        print("数据库: " + args.db)
        print("python sqlite3 版本: " + sqlite3.sqlite_version)
        print()
        ok_tables = check_tables(conn)
        ok_fts = functional_fts5_check(conn) if ok_tables else False
        print()
        if ok_tables and ok_fts:
            print("== 全部通过 ==")
            return 0
        print("== 有项目未通过 ==")
        return 1
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
