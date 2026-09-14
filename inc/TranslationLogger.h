#pragma once
#include <string>
#include <mutex>

struct sqlite3;        // 前向声明——头文件里不暴露 sqlite3 的结构
struct sqlite3_stmt;   // 用户只需要知道有这么个类型

class TranslationLogger {
public:
    // 单例入口——全局唯一的实例
    static TranslationLogger& instance();

    // 打开数据库，建表，预编译 INSERT 语句。只需调一次
    bool init(const std::string& db_path);

    // 记录一条翻译
    void log(const std::string& original,
             const std::string& translated,
             const std::string& translator,
             long long latency_ms);

    // 程序退出前关闭
    void close();

private:
    TranslationLogger() = default;   // 构造函数私有——外部不能 new，只能通过 instance()
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};
