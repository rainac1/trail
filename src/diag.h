#pragma once
#include <windows.h>

// 诊断输出：GUI 程序（/SUBSYSTEM:WINDOWS）没有控制台，fwprintf(stderr) 在正常
// 双击启动时不可见。本模块提供三通道输出：
//   1. 日志文件：exe 同目录 subframe_cursor_trail.log（追加）
//   2. stderr：  从 shell / 调试器启动时可见
//   3. MessageBox：任何启动方式下都可见（设置环境变量 SUBFRAME_NO_UI=1 可禁用，
//                  用于自动化/无人值守场景）
void DiagInit();
void DiagLog(const wchar_t* fmt, ...);
void DiagFatal(const wchar_t* title, const wchar_t* msg);
void DiagClose();

// 日志文件完整路径（用于在错误信息中提示用户查看位置）。
const wchar_t* DiagLogPath();
