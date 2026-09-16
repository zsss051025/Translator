#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct sqlite3;        // 前向声明——头文件里不暴露 sqlite3 的结构
struct sqlite3_stmt;

// 一条翻译记录（一场会话里的一句话）
struct Segment {
    long long   id = 0;
    long long   session_id = 0;
    int         seq = 0;            // 会话内序号，从 1 开始
    std::string ts;                 // 记录时间
    std::string src_text;           // 原文
    std::string tgt_text;           // 译文
    std::string engine;             // 产出该译文的引擎
    long long   ms = 0;             // 翻译耗时
    double      confidence = -1.0;  // <0 表示该后端未提供置信度
};

// 一场会话（一场会议 / 一次使用）的元信息
struct SessionInfo {
    long long   id = 0;
    std::string started_at;
    std::string ended_at;           // 空表示尚未结束
    std::string engine;
    std::string note;
    int         segment_count = 0;
};

// 会话与段落持久化。
//
// 替代原来的 TranslationLogger：原来只有一张扁平的 translations 表，
// 记录之间没有归属关系，因此无法回答"这场会议说了什么"，
// 也就无法生成纪要、行动项这类交付物。这里引入 session 概念，
// 把一组段落聚合到一场会话下，作为交付层的数据基础。
//
// 线程安全：两个翻译 worker 线程会并发调用 log_segment()。
class SessionStore {
public:
    static SessionStore& instance();

    // 打开数据库并建表。只需调一次
    bool init(const std::string& db_path);
    void close();

    // 开启一场会话并设为"当前会话"，返回 session_id（失败返回 -1）
    long long begin_session(const std::string& engine, const std::string& note = "");

    // 结束当前会话，写入 ended_at
    void end_session();

    // 当前会话 id；无会话时返回 -1
    long long current_session() const;

    // 记录一条翻译，自动归入当前会话。
    // 没有活跃会话时静默丢弃并返回 false（不抛异常，不影响翻译主链路）
    bool log_segment(const std::string& src_text,
                     const std::string& tgt_text,
                     const std::string& engine,
                     long long latency_ms,
                     double confidence = -1.0);

    // 读取一场会话的所有段落，按 seq 升序
    std::vector<Segment> fetch_segments(long long session_id) const;

    // 长期记忆的表与全文索引是否就绪（PROJECT.md §7 步骤 2.1 的验收接口）。
    //
    // 检查三件事：
    //   ① sqlite3 编译时真的带了 FTS5（SQLITE_ENABLE_FTS5 宏生效）
    //   ② knowledge / knowledge_history / actions 三张表可查询
    //   ③ FTS5 虚表 knowledge_fts 可查询 —— 宏没定义时建表那一步就会失败，
    //      所以这一条是"FTS5 真的能用"的实证，比只看编译选项可靠
    bool long_term_memory_ready() const;

    // 列出最近的会话（供跨会话检索/历史界面使用）
    std::vector<SessionInfo> list_sessions(int limit = 20) const;

    // 会话统计
    struct Stats {
        int       count = 0;
        long long total_ms = 0;
        long long avg_ms = 0;
    };
    Stats stats(long long session_id) const;

private:
    SessionStore() = default;
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    bool ensure_schema();           // 建表 + 预编译语句

    mutable std::mutex mutex_;
    sqlite3*           db_ = nullptr;

    // 预编译语句
    sqlite3_stmt* stmt_begin_session_ = nullptr;   // INSERT INTO sessions
    sqlite3_stmt* stmt_end_session_   = nullptr;   // UPDATE sessions SET ended_at
    sqlite3_stmt* stmt_insert_seg_    = nullptr;   // INSERT INTO segments
    sqlite3_stmt* stmt_next_seq_      = nullptr;   // SELECT MAX(seq)

    long long current_session_ = -1;
};
