#include "diag.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace {
// 日志状态。当前不变式：仅由主线程调用（消息循环与初始化/退出同线程），无并发访问。
FILE* g_log = nullptr;
bool g_logEnabled = false;
bool g_logAttempted = false;
bool g_noUi = false;
wchar_t g_logPath[MAX_PATH] = {};

void OpenLog() {
  if (!g_logEnabled || g_logAttempted) return;
  g_logAttempted = true;

  const DWORD n = GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    g_logPath[0] = L'\0';
    return;
  }
  constexpr wchar_t kLogName[] = L"trail.log";
  constexpr size_t kLogNameLen = sizeof(kLogName) / sizeof(kLogName[0]);
  wchar_t* slash = wcsrchr(g_logPath, L'\\');
  if (!slash) {
    g_logPath[0] = L'\0';
    return;
  }
  const size_t remain = MAX_PATH - static_cast<size_t>(slash + 1 - g_logPath);
  if (remain < kLogNameLen) {
    g_logPath[0] = L'\0';
    return;
  }
  wcscpy_s(slash + 1, remain, kLogName);

  // _SH_DENYWR：允许其他进程在程序运行期间读取日志。
  g_log = _wfsopen(g_logPath, L"a, ccs=UTF-8", _SH_DENYWR);
  if (g_log) {
    fwprintf(g_log, L"\n===== --- session --- =====\n");
    fflush(g_log);
  }
}

}  // namespace

void DiagInit(bool enableFileLog) {
  g_logEnabled = enableFileLog;
  // 仅当变量值为 "1" 时才禁用弹窗（避免 "0"/"false" 误触发）。
  wchar_t buf[2] = {};
  g_noUi = (GetEnvironmentVariableW(L"TRAIL_NO_UI", buf, 2) > 0 && buf[0] == L'1');
  if (g_logEnabled) OpenLog();
}

bool DiagLogEnabled() {
  return g_logEnabled;
}

void DiagLog(const wchar_t* fmt, ...) {
  if (!g_logEnabled) return;
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
  if (g_logEnabled) {
    DiagLog(L"FATAL: %ls", msg);
  } else {
    fwprintf(stderr, L"FATAL: %ls\n", msg);
  }
  if (g_noUi) return;

  if (g_log && g_logPath[0]) {
    wchar_t full[768];
    swprintf_s(full, L"%ls\n\n诊断日志: %ls", msg, g_logPath);
    MessageBoxW(nullptr, full, title, MB_OK | MB_ICONERROR);
  } else {
    MessageBoxW(nullptr, msg, title, MB_OK | MB_ICONERROR);
  }
}

void DiagClose() {
  if (g_log) {
    fclose(g_log);
    g_log = nullptr;
  }
  g_logEnabled = false;
  g_logAttempted = false;
}
