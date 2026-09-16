#!/usr/bin/env python3
"""
校验长期记忆（知识库）的数据层是否真的可用。

用法:
    python tools/verify_memory.py <数据库路径>

它做三件事，而且**不改动原库**（全程在一个事务里，最后回滚）：

  1. 列出所有表，检查四张长期记忆表在不在
       knowledge / knowledge_history / actions / knowledge_fts

  2. 功能验证：只往 knowledge 插两条记录，**一句 FTS 语句都不写**，
     索引必须由触发器自己跟上，然后用 MATCH 查回来，再删一条确认删也同步。
     这一步才是"检索真的能用"的证据 —— 只看到表存在是不够的，
     外部内容表还要能索引和检索。

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
    if not missing:
        print("\n四张长期记忆表都在")
        return True

    print("\n[缺少] " + ", ".join(missing))
    print(hint_for(missing, tables))
    return False


def hint_for(missing, tables):
    """
    给**具体的**下一步，不是笼统的"请检查配置"。

    实测踩过：用户按文档跑验证，报"缺少四张表"，而真相只是
    **跑在了另一个同名文件上** —— 程序在 build\\RelWithDebInfo\\ 下用的 t.db
    和仓库根目录那个 t.db 是两个不同的文件。
    所以下面第一条提示永远是"确认你验的是程序真正在用的那个库"。
    """
    lines = []
    if "sessions" in tables and "segments" in tables:
        # 典型的老形状库：有会话表、没有记忆表 —— 说明它还没被新版程序打开过
        lines.append("         这个库有 sessions/segments 但没有记忆表，是**改动前建的旧库**。")
        lines.append("         让新版程序打开它一次就会自动补表（建表语句都是 IF NOT EXISTS）：")
        lines.append("             Translator.exe --selftest --db <这个库的完整路径>")
    else:
        lines.append("         这个库既没有记忆表也没有会话表，可能不是本项目建的库。")
    lines.append("")
    lines.append("      也可能是路径不对。**注意 `--db t.db` 是相对当前目录的**：")
    lines.append("         build\\RelWithDebInfo\\t.db  ← 从 build 目录跑程序时用的")
    lines.append("         t.db                        ← 从仓库根跑程序时用的")
    lines.append("      程序真正在用哪个，就用哪个去验。")
    return "\n".join(lines)


def functional_fts5_check(conn):
    """
    在事务里插入 + 检索 + 删除，最后回滚 —— 原库一个字节都不变。

    验的是**真实机制**：knowledge 上的三个触发器有没有把 FTS5 索引维护对。
    早先这里是自己 rebuild 一把再查，结果验过了、线上却查不到
    —— 因为 rebuild 是我在测试里手动做的，产品代码根本没做这件事。
    现在故意**一句 FTS 语句都不写**：只动 knowledge 表，索引必须自己跟上。
    """
    print("\nFTS5 功能验证（只写 knowledge 表 -> 索引应自动跟上 -> 回滚）")

    triggers = [r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='trigger' AND name LIKE 'knowledge_a%' "
        "ORDER BY name"
    ).fetchall()]
    print("    knowledge 上的 FTS 触发器: " + (repr(triggers) if triggers else "一个都没有"))
    if len(triggers) < 3:
        print("[失败] 触发器不全（应有 knowledge_ai / knowledge_ad / knowledge_au）")
        print("       让新版程序打开这个库一次就会自动补上：")
        print("           Translator.exe --selftest --db <这个库的完整路径>")
        return False

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

        hit = conn.execute(
            "SELECT key, value FROM knowledge_fts WHERE knowledge_fts MATCH ? ORDER BY key",
            ("erika",),
        ).fetchall()
        print("    插入后 MATCH 'erika' -> " + repr(hit))
        if [h[0] for h in hit] != ["erika"]:
            print("[失败] 触发器没把新行写进索引")
            return False

        # 删除也必须同步，否则索引里留着已删的行 —— 检索结果会带上幽灵
        conn.execute("DELETE FROM knowledge WHERE key = 'erika'")
        after = conn.execute(
            "SELECT key FROM knowledge_fts WHERE knowledge_fts MATCH ?",
            ("erika",),
        ).fetchall()
        print("    删除后 MATCH 'erika' -> " + repr(after))
        if after:
            print("[失败] 删除没有同步到索引（检索会返回已删除的记录）")
            return False

        print("[通过] 触发器维护索引：增、删都能自动跟上")
        return True
    finally:
        conn.execute("ROLLBACK")


def seed_demo(conn):
    """
    **会写库**（需要显式 --seed-demo 才走这里）。

    造三条形态各异的待确认知识，好让 `Translator.exe --gaps` 有东西可检测。
    存在的理由：缺口检测是纯规则、有单测，但单测用的是手搓数据；
    这个项目两次栽在"测试数据与真实数据形状不一致"上，所以留一条对真库跑的路。

    全部用 __demo_ 前缀的 key，且可以用 --seed-demo --clear 清掉。
    """
    rows = [
        # 高频未确认：出现 5 次还是 candidate → 该问
        ("term", "__demo_phoenix", "Phoenix", "candidate", 0.90, 5, None, None),
        # 低置信度专名 → 该问
        ("person", "__demo_marko", "Marko", "candidate", 0.35, 1, None, None),
        # confirmed 且没变化 → **不该问**（提交稀缺性的对照组）
        ("term", "__demo_englishpod", "EnglishPod", "confirmed", 0.95, 9, None, None),
    ]
    with conn:
        for kind, key, value, status, conf, hits, _, _ in rows:
            conn.execute(
                "INSERT INTO knowledge"
                "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
                " source_session,source_seq,source_text) "
                "VALUES (?,?,?,?,?,?,datetime('now'),datetime('now'),NULL,NULL,'') "
                "ON CONFLICT(kind,key) DO UPDATE SET "
                "  value=excluded.value, status=excluded.status, "
                "  confidence=excluded.confidence, hits=excluded.hits",
                (kind, key, value, status, conf, hits),
            )
        # 第四条规则（旧值≠新值）要走历史，单独造一条
        conn.execute(
            "INSERT INTO knowledge"
            "(kind,key,value,status,confidence,hits,first_seen_at,updated_at,"
            " source_session,source_seq,source_text) "
            "VALUES ('fact','__demo_asr_engine','Qwen ASR','candidate',0.8,2,"
            "        datetime('now'),datetime('now'),NULL,NULL,'') "
            "ON CONFLICT(kind,key) DO UPDATE SET value=excluded.value, status=excluded.status"
        )
        kid = conn.execute(
            "SELECT id FROM knowledge WHERE kind='fact' AND key='__demo_asr_engine'"
        ).fetchone()[0]
        conn.execute(
            "INSERT INTO knowledge_history"
            "(knowledge_id,old_value,new_value,changed_at,source_session,source_seq,"
            " source_text,reason) VALUES (?,?,?,datetime('now'),NULL,NULL,'',?)",
            (kid, "Whisper large-v3", "Qwen ASR", "value_changed_demoted"),
        )
    print("已写入 4 条演示知识（key 以 __demo_ 开头，可用 --clear 清掉）")


def clear_demo(conn):
    # FTS 索引不用手动删：knowledge 上的 AFTER DELETE 触发器会负责。
    # （早先这里写 DELETE FROM knowledge_fts，外部内容表不接受这种写法，
    #   会直接报 "database disk image is malformed"）
    with conn:
        conn.execute(
            "DELETE FROM knowledge_history WHERE knowledge_id IN "
            "  (SELECT id FROM knowledge WHERE key LIKE '__demo_%')"
        )
        n = conn.execute("DELETE FROM knowledge WHERE key LIKE '__demo_%'").rowcount
    print("已清掉 %d 条演示知识" % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db", help="SQLite 数据库路径")
    ap.add_argument("--seed-demo", action="store_true",
                    help="写入 4 条演示知识（会改库，供 --gaps 演示用）")
    ap.add_argument("--clear", action="store_true",
                    help="清掉 --seed-demo 写入的演示知识")
    args = ap.parse_args()

    try:
        conn = sqlite3.connect(args.db)
    except sqlite3.Error as e:
        print("打不开数据库: " + str(e))
        return 1

    try:
        # 打**绝对路径**。同名文件散在不同目录是这个项目踩过的坑：
        # --db t.db 是相对当前目录的，从 build 目录跑和从仓库根跑是两个不同的库。
        import os
        print("数据库: " + os.path.abspath(args.db))
        print("python sqlite3 版本: " + sqlite3.sqlite_version)
        print()

        if args.clear:
            clear_demo(conn)
            print()
        if args.seed_demo:
            seed_demo(conn)
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
