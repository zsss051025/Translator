"""往一个库里塞一场"有花名/产品名/项目名"的假会话，用来验按类型提问。一次性脚手架。"""
import sqlite3
import sys
import os

db = sys.argv[1]
if os.path.exists(db):
    os.remove(db)

con = sqlite3.connect(db)
# 建表由程序自己负责；这里只塞会话 + 段落，程序打开时会补 schema
con.executescript("""
CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  started_at TEXT NOT NULL,
  ended_at TEXT,
  engine TEXT NOT NULL,
  note TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS segments (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  seq INTEGER NOT NULL,
  ts TEXT NOT NULL,
  src_text TEXT NOT NULL,
  tgt_text TEXT NOT NULL,
  engine TEXT NOT NULL,
  ms INTEGER NOT NULL,
  confidence REAL NOT NULL DEFAULT -1.0
);
""")

segs = [
    # ⚠️ 第一版我把名字都写在**段首**了，结果一个都没抽到 ——
    #   中文侧的英文规则 R1 要求"**非句首**大写出现 ≥2 次"，段首那次不算证据。
    #   这不是 bug，是抽取器的设计（句首大写是语法不是专名）。
    #   所以脚手架必须让每个名字在**句子中间**出现至少两次，否则测的不是我要测的东西。
    "这周的进度我跟我们的 PM Penny 对过了。",
    "Penny 说凤凰项目的接口文档要重写，所以我又找 Penny 确认了一遍。",
    "另外我们在评估 Gecko 这个新方案，Gecko 要和极光平台配合用。",
    "Penny 觉得 Gecko 的迁移成本太高，凤凰项目下一期再说。",
]
con.execute("INSERT INTO sessions(started_at, engine, note) VALUES (datetime('now'), 'Scaffold', '按类型提问验证')")
sid = con.execute("SELECT last_insert_rowid()").fetchone()[0]
for i, t in enumerate(segs, 1):
    con.execute("INSERT INTO segments(session_id,seq,ts,src_text,tgt_text,engine,ms,confidence) "
                "VALUES (?,?,datetime('now'),?,?, 'Scaffold', 100, 0.9)",
                (sid, i, t, t))
con.commit()
print(f"已写入会话 #{sid}，{len(segs)} 段")
