#pragma once
#include <string>
#include <vector>

#include "SessionStore.h"

// ============================================================
// 交付物层
//
// 赛道硬指标要求"结果交付：能交付可被直接验收、使用的成果"。
// 这一层把 SessionStore 里的段落，渲染成四种可直接使用的文件：
//   meeting-<id>.md    双语纪要      -> 对应"报告"
//   actions.csv        行动项表格    -> 对应"表格"
//   meeting-<id>.html  单文件网页    -> 对应"网页"
//   transcript.srt     双语字幕      -> 加分项
// ============================================================

// 一条行动项
struct ActionItem {
    int         seq = 0;      // 来源段落序号
    std::string owner;        // 负责人（可能推断不出，留空）
    std::string task;         // 要做的事
    std::string due;          // 截止时间（可能为空）
    std::string source;       // 来源原文
};

// 一个主题下的要点
struct TopicSummary {
    std::string              title;
    std::vector<std::string> points;
};

// 会议摘要。目前由规则提取填充，后续可由大模型填充（generator 字段标出来源）
struct MeetingSummary {
    std::string               overview;    // 一句话概述
    std::vector<TopicSummary> topics;      // 分主题要点
    std::vector<ActionItem>   actions;     // 行动项
    std::string               generator;   // 来源："规则提取" / "Hunyuan" / "DeepSeek"

    // 内容类型：会议 / 课程 / 视频 / 对话（空则按"会议"处理）。
    // 同一个产品要适应任何场景，产出就得跟着场景变——
    // 看视频输出"内容笔记"，上课输出"课堂笔记"，开会输出"会议纪要"。
    std::string               content_type;
};

// 会话元信息（渲染头部需要）
struct SessionMeta {
    long long   id = 0;
    std::string started_at;
    std::string ended_at;
    std::string engine;
    std::string note;
    int         count = 0;
    long long   avg_ms = 0;
    long long   duration_sec = 0;
};

class DeliverableWriter {
public:
    struct Result {
        bool                     ok = false;
        std::string              dir;      // 输出目录
        std::vector<std::string> files;    // 生成的文件完整路径
        std::string              error;
    };

    // 生成全部交付物到 <out_root>/session-<id>/
    // 纯函数：只依赖传入的数据，不访问数据库，便于单独测试
    static Result write(long long session_id,
                        const std::vector<Segment>& segs,
                        const SessionMeta& meta,
                        const MeetingSummary& summary,
                        const std::string& out_root);

    // 规则版摘要与行动项提取：完全离线，不依赖大模型。
    // 作为兜底路径，也保证"拔网线可用"这个卖点成立。
    static MeetingSummary extract_by_rules(const std::vector<Segment>& segs);

    // 由会话信息与段落组装元信息
    static SessionMeta make_meta(const SessionInfo& info,
                                 int count,
                                 long long avg_ms,
                                 const std::vector<Segment>& segs);
};
