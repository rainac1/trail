#include "diag.h"

#include <cstdio>
#include <cstring>

namespace {
// 日志状态。当前不变式：DiagLog/DiagFatal 仅由主线程调用（采样线程不写日志），
// DiagClose 在采样线程 join 之后执行，因此不存在并发访问；g_log 检查与使用之间
// 无 TOCTOU。若未来允许其他线程写日志，需在此加 CRITICAL_SECTION 保护。
FILE* g_log = nullptr;
bool g_noUi = false;
wchar_t g_logPath[MAX_PATH] = {};
}  // namespace

static void OpenLog() {
  if (g_log) return;
  const DWORD n = GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  wchar_t* slash = wcsrchr(g_logPath, L'\\');
  if (slash) wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - g_logPath),
                      L"trail.log");
  _wfopen_s(&g_log, g_logPath, L"a, ccs=UTF-8");
  if (g_log) {
    fwprintf(g_log, L"\n===== %ls =====\n", L"--- session ---");
    fflush(g_log);
  }
}

void DiagInit() {
  OpenLog();
  // 仅当变量值为 "1" 时才禁用弹窗（避免 "0"/"false" 误触发）。
  wchar_t buf[2] = {};
  // TRAIL_NO_UI=1 禁用弹窗；旧名 SUBFRAME_NO_UI 仍兼容（项目重命名前遗留）。
  g_noUi = (GetEnvironmentVariableW(L"TRAIL_NO_UI", buf, 2) > 0 && buf[0] == L'1') ||
           (GetEnvironmentVariableW(L"SUBFRAME_NO_UI", buf, 2) > 0 && buf[0] == L'1');
}

void DiagLog(const wchar_t* fmt, ...) {
  OpenLog();
  va_list args;
  va_start(args, fmt);
  if (g_log) {
    vfwprintf(g_log, fmt, args);
    fputwc(L'\n', g_log);
    fflush(g_log);
  }
  va_end(args);

  va_start(args, fmt);
  vfwprintf(stderr, fmt, args);
  fputwc(L'\n', stderr);
  va_end(args);
}

void DiagFatal(const wchar_t* title, const wchar_t* msg) {
  DiagLog(L"FATAL: %ls", msg);
  if (!g_noUi) MessageBoxW(nullptr, msg, title, MB_OK | MB_ICONERROR);
}

void DiagClose() {
  if (g_log) {
    fclose(g_log);
    g_log = nullptr;
  }
}

const wchar_t* DiagLogPath() {
  OpenLog();
  return g_logPath[0] ? g_logPath : L"<unknown>";
}
