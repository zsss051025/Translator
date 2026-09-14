#include "TranslationLogger.h"
#include "sqlite3.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

// ================================================================
// 单例
// ================================================================
TranslationLogger& TranslationLogger::instance() {
    static TranslationLogger logger;   // C++11 保证多线程安全，只构造一次
    return logger;
}

// ================================================================
// 初始化：打开数据库 + 建表 + 预编译 INSERT
// ================================================================
bool TranslationLogger::init(const std::string& db_path) {
    // ① 打开（或创建）数据库文件
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] open failed: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // ② 建表 —— 用 sqlite3_exec，因为只跑一次
    const char* sql_create =
        "CREATE TABLE IF NOT EXISTS translations ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts       TEXT    NOT NULL,"
        "  original TEXT    NOT NULL,"
        "  result   TEXT    NOT NULL,"
        "  engine   TEXT    NOT NULL,"
        "  ms       INTEGER NOT NULL"
        ");";

    char* err = nullptr;
    rc = sqlite3_exec(db_, sql_create, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] create table failed: " << err << std::endl;
        sqlite3_free(err);
        return false;
    }

    // ③ 预编译 INSERT —— 准备好模板，等 log() 时 bind 参数
    const char* sql_insert =
        "INSERT INTO translations (ts, original, result, engine, ms) "
        "VALUES (?, ?, ?, ?, ?);";
    //                              ↑ 5 个占位符，后面 bind 时按 1,2,3,4,5 对应

    rc = sqlite3_prepare_v2(db_, sql_insert, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] prepare failed: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::cout << "[DB] translation log ready: " << db_path << std::endl;
    return true;
}

// ================================================================
// 记录一条翻译：bind → step → reset
// ================================================================
void TranslationLogger::log(const std::string& original,
                             const std::string& translated,
                             const std::string& translator,
                             long long latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);  // 两个 worker 线程可能同时调

    // 生成当前时间戳
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    std::string ts = oss.str();

    // bind：把 5 个 ? 换成实际值
    sqlite3_bind_text(stmt_, 1, ts.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_, 2, original.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_, 3, translated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_, 4, translator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64( stmt_, 5, latency_ms);

    sqlite3_step(stmt_);          // 执行插入
    sqlite3_reset(stmt_);         // 重置，等待下次 bind
    sqlite3_clear_bindings(stmt_);// 清空上次绑定的值
}

// ================================================================
// 关闭
// ================================================================
void TranslationLogger::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stmt_) sqlite3_finalize(stmt_);  // 先释放 stmt
    if (db_)   sqlite3_close(db_);       // 再关闭数据库
    stmt_ = nullptr;
    db_ = nullptr;
}
