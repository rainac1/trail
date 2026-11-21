#pragma once
#include <windows.h>

// 诊断输出：GUI 程序（/SUBSYSTEM:WINDOWS）没有控制台，正常双击启动时 stderr 不可见。
//   1. 日志文件：仅传入 --log 时在 exe 同目录写入 trail.log（追加、允许外部并发读取）
//   2. stderr：  启用日志或发生致命错误时输出
//   3. MessageBox：致命错误时弹窗（设置环境变量 TRAIL_NO_UI=1 可禁用）
void DiagInit(bool enableFileLog);
bool DiagLogEnabled();
void DiagLog(const wchar_t* fmt, ...);
void DiagFatal(const wchar_t* title, const wchar_t* msg);
void DiagClose();
