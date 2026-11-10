/*
 * cube_tasks.h —— 全机任务参数单点登记表(L0)
 *
 * 优先级全局序(高→低, docs/06 §7):
 *   mixer(20) > voice(18) > player(12) > ai(5) > LVGL(4)
 * 音频实时 > 唤醒检测 > 流解码 > 云端会话 > UI 渲染。
 * 绑核策略: core0 = 音频实时/网络协议栈/BT; core1 = 解码/渲染/云任务。
 * 各组件创建任务一律引用本表宏, 调度关系改动只改这一处。
 */
#pragma once

/* ---- 混音引擎(app_audio) ---- */
#define CUBE_TASK_MIXER_NAME   "mixer"
#define CUBE_TASK_MIXER_STACK  8192
#define CUBE_TASK_MIXER_PRIO   20
#define CUBE_TASK_MIXER_CORE   0

/* ---- 语音前端(app_voice; esp-sr 推理负载) ---- */
#define CUBE_TASK_VOICE_NAME   "voice"
#define CUBE_TASK_VOICE_STACK  8192
#define CUBE_TASK_VOICE_PRIO   18
#define CUBE_TASK_VOICE_CORE   0

/* ---- 网络播放器(app_player; 栈走 PSRAM 静态双槽) ---- */
#define CUBE_TASK_PLAYER_NAME      "player"
#define CUBE_TASK_PLAYER_STACK     12288
#define CUBE_TASK_PLAYER_PRIO      12
#define CUBE_TASK_PLAYER_CORE      1

/* ---- AI 会话编排(app_ai_session 三任务) ---- */
#define CUBE_TASK_AISESS_NAME  "aisess"
#define CUBE_TASK_AISESS_STACK 8192
#define CUBE_TASK_AISESS_PRIO  5
#define CUBE_TASK_AISESS_CORE  0

#define CUBE_TASK_AITTS_NAME   "aitts"
#define CUBE_TASK_AITTS_STACK  6144
#define CUBE_TASK_AITTS_PRIO   5
#define CUBE_TASK_AITTS_CORE   1

#define CUBE_TASK_AIMIC_NAME   "aimic"
#define CUBE_TASK_AIMIC_STACK  6144
#define CUBE_TASK_AIMIC_PRIO   5
#define CUBE_TASK_AIMIC_CORE   0

/* ---- LVGL 渲染(app_ui; esp_lvgl_port 任务) ---- */
#define CUBE_TASK_LVGL_STACK   16384
#define CUBE_TASK_LVGL_PRIO    4
#define CUBE_TASK_LVGL_CORE    1
