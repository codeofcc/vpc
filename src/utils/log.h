#ifndef LOG_H
#define LOG_H
/************************************************************************
* @Project:  	log
* @Decription:  Logging utility.
*               Usage:
*               LOG_CONFIG("./log/", 1024 * 1024 * 10, 10, "warn"); // Configure logging
*               LOG_INFO("hello world!"); LOG_WARN("%s %d", "hello world!", 123); // Print logs
*               FATAL will exit the program directly.
*               PANIC will send an interrupt signal.
*               DPANIC will send an interrupt signal in debug mode.
*               ERROR and above levels write to file immediately. Frequent calls may impact performance. Use ERROR for errors that need to be recorded immediately.
*               WARN and below levels use buffered output for better performance, with minimal latency. Suitable for frequent calls where immediate recording is not required.
*               INFO is for general log messages.
*               DEBUG is only output in debug mode.
*               All logs are also printed to the console.
*               By default, logs are stored in the "log" directory under the working directory, with 10MB file rotation and 10 historical files retained.
* @Verision:  	v1.0.0
* @Author:  	Xin Nie
* @Create:  	2026/02/02 13:52:13
* @LastUpdate:  2026/06/05 16:46:12
************************************************************************
* Copyright @ 2026. All rights reserved.
************************************************************************/

/// <summary>
/// Configure logging.
/// </summary>
/// <param name="logDir">Log file save path. Multi-level directories will be auto-created. Must end with '/'.</param>
/// <param name="maxSize">Maximum size of a single log file in bytes. Files are rotated and named by timestamp when exceeded.</param>
/// <param name="maxBackups">Maximum number of log files to retain. Oldest files are deleted when exceeded.</param>
/// <param name="logLevel">Log level for file output. Valid values: panic, panic, dpanic, error, warn, info, debug.</param>
void LOG_CONFIG(const char* logDir, int maxSize, int maxBackups, const char* logLevel);

/// <summary>
/// Print a log message.
/// </summary>
/// <param name="data">String</param>
/// <param name="...">Formatting parameters, consistent with printf.</param>
#define LOG_FATAL(data, ...) _LOG_HELPER(data, 0, "   FATAL  ", ##__VA_ARGS__)
#define LOG_PANIC(data, ...) _LOG_HELPER(data, 1, "   PANIC  ", ##__VA_ARGS__)
#define LOG_DPANIC(data, ...) _LOG_HELPER(data, 2, "   DPANIC ", ##__VA_ARGS__)
#define LOG_ERROR(data, ...) _LOG_HELPER(data, 3, "   ERROR  ", ##__VA_ARGS__)
#define LOG_WARN(data, ...) _LOG_HELPER(data, 4, "   WARN   ", ##__VA_ARGS__)
#define LOG_INFO(data, ...) _LOG_HELPER(data, 5, "   INFO   ", ##__VA_ARGS__)

#if _DEBUG
#define LOG_DEBUG(data, ...) _LOG_HELPER(data, 6, "   DEBUG  ", ##__VA_ARGS__)
#else
#define LOG_DEBUG(data, ...)
#endif

void _log(const char* file, int line, const char* funcName, int level, const char* label, const char* log, ...);

#define _LOG_HELPER(data, level, label, ...) _log(__FILE__, __LINE__, __FUNCTION__, level, label, data, ##__VA_ARGS__)

#endif // LOG_H