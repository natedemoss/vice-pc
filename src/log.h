#pragma once

// Simple thread-safe logger: writes to stdout and (optionally) a log file.
void log_init(const wchar_t* file_path);
void log_write(const char* fmt, ...);
void log_flush();

#define LOG(...) log_write(__VA_ARGS__)
