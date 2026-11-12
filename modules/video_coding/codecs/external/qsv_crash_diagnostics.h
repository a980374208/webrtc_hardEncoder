/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_CRASH_DIAGNOSTICS_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_CRASH_DIAGNOSTICS_H_

#include <cstdio>
#include <cwchar>
#include <iostream>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Windows types must precede the DbgHelp declarations.
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace webrtc::qsv_smoke_diagnostics {

// Diagnostic-only process-local state. Load DbgHelp and prepare the path before
// entering a possibly corrupt vendor stack; keep its module until process exit.
inline wchar_t g_dump_path[MAX_PATH] = {};
inline decltype(&MiniDumpWriteDump) g_dump_writer = nullptr;
inline volatile LONG g_dump_started = 0;

inline LONG WINAPI CaptureUnhandledException(EXCEPTION_POINTERS* pointers) {
  if (InterlockedExchange(&g_dump_started, 1) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  BOOL written = FALSE;
  DWORD dump_error = ERROR_SUCCESS;
  HANDLE file =
      CreateFileW(g_dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION exception;
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    written = g_dump_writer(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                   MiniDumpWithUnloadedModules),
        &exception, nullptr, nullptr);
    if (!written)
      dump_error = GetLastError();
    FlushFileBuffers(file);
    CloseHandle(file);
  } else {
    dump_error = GetLastError();
  }
  // Avoid iostream allocation/locking while handling a possibly damaged heap.
  char message[256];
  const int length = std::snprintf(
      message, sizeof(message),
      "QSV_UNHANDLED_EXCEPTION code=0x%08lx address=%p thread=%lu "
      "dump_written=%d dump_error=%lu\n",
      pointers && pointers->ExceptionRecord
          ? pointers->ExceptionRecord->ExceptionCode
          : 0UL,
      pointers && pointers->ExceptionRecord
          ? pointers->ExceptionRecord->ExceptionAddress
          : nullptr,
      GetCurrentThreadId(), written, dump_error);
  if (length > 0 && length < static_cast<int>(sizeof(message))) {
    DWORD bytes_written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, length, &bytes_written,
              nullptr);
  }
  // Preserve the original exception and crash exit code. A dump is not a PASS.
  return EXCEPTION_CONTINUE_SEARCH;
}

inline bool Install(const std::string& directory, const wchar_t* smoke_name) {
  wchar_t converted[MAX_PATH];
  wchar_t absolute[MAX_PATH];
  if (!MultiByteToWideChar(CP_ACP, 0, directory.c_str(), -1, converted,
                           MAX_PATH))
    return false;
  const DWORD path_length =
      GetFullPathNameW(converted, MAX_PATH, absolute, nullptr);
  if (path_length == 0 || path_length >= MAX_PATH)
    return false;
  const DWORD attributes = GetFileAttributesW(absolute);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;
  const int length = std::swprintf(g_dump_path, MAX_PATH, L"%ls\\%ls-%lu.dmp",
                                   absolute, smoke_name, GetCurrentProcessId());
  if (length <= 0 || length >= MAX_PATH)
    return false;
  HMODULE dbghelp =
      LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dbghelp)
    return false;
  g_dump_writer = reinterpret_cast<decltype(g_dump_writer)>(
      GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  if (!g_dump_writer) {
    FreeLibrary(dbghelp);
    return false;
  }
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(CaptureUnhandledException);
  std::cerr << "DIAGNOSTIC minidump enabled directory=" << directory << '\n';
  return true;
}

}  // namespace webrtc::qsv_smoke_diagnostics

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_CRASH_DIAGNOSTICS_H_
