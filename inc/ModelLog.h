#pragma once

// 把 llama.cpp / whisper.cpp 的**加载日志**压下去，只留警告和错误。
//
// 【为什么这是一个真问题，不是"看着不舒服"】
// 实测一次真实启动，模型加载阶段会打**几百行**内部日志：
//     load: control token: 120546 '<｜hy_place▁holder▁no▁528｜>' is not marked as EOG
//     create_tensor: loading tensor blk.0.attn_q_norm.weight
//     llama_kv_cache: layer  17: dev = CUDA0
//     ...
// 而用户真正需要看到的两行被埋在里面：
//     >>> 已开始记录 <<<
//     >>> 结束并生成纪要: Ctrl+Alt+Q <<<
//
// 后果很具体：用户不知道什么时候可以开始放视频 —— 他往上翻一屏，
// 看见满屏 tensor 和 control token，只能猜。而"猜错了几秒"意味着
// 那几秒的音频没被录到。**这是可用性问题，不是美观问题。**
//
// 【为什么不干脆全关】警告和错误必须留下：显存不够、模型文件损坏这类事
// 只能从这里看出来。所以按级别过滤，不是静音一切。
//
// `--verbose` 可以恢复原样（排查模型加载问题时要看那些行）。
namespace modellog {

// keep_verbose = true 时**什么都不做**（保持各库默认行为）。
void install_silencer(bool keep_verbose);

}  // namespace modellog
