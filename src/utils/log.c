#include "log.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "thread.h"
#include <stdint.h>
#include <signal.h>
#include <inttypes.h>
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <direct.h>
#include <windows.h>
#define ACCESS(fileName, accessMode) _access(fileName, accessMode)
#define MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#define ACCESS(fileName, accessMode) access(fileName, accessMode)
#define MKDIR(path) mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)
#endif
#define MAX_PATH_LEN 1024
static char _logDir[1024] = {0};		// 日志文件目录，多级目录会自动创建
static int _maxSize = 1024 * 1024 * 10; // 限制每个日志文件的大小为10MB
static int _maxBackups = 10;			// 保留最近的10个日志文件
static int _logLevel = 4;				// 保留最近的10个日志文件
static int _isInit = 0;
static Mutex _mutex;
static Thread _thread = 0;
static char _fileName[1024] = {0};
static FILE *_file = 0;
static char *_buffer = NULL;
static size_t _bufferSize = 0;
static size_t _bufferCapacity = 0;
static char *getBuffer(size_t len);
static void returnBuffer(int len);
static void pushToBuffer(const char *buf, size_t len);
static int flushBuffer();
static TINT writeThread(void *userdata);
static time_t getSystemTimeMs(void);
static void getIso8601Format(char *buf, size_t len, time_t time1, int ms);
static void getTimeFileFormat(char *buf, size_t len, time_t time1, int ms);
static time_t timeFileFormatToTimestampMs(const char *filename);
static int createMultiLevelDir(const char *dir);
static void cleanDirectory(const char *path);

void LOG_CONFIG(const char *logDir, int maxSize, int maxBackups, const char *logLevel)
{
	if (strcmp(logLevel, "fatal") == 0)
		_logLevel = 0;
	else if (strcmp(logLevel, "panic") == 0)
		_logLevel = 1;
	else if (strcmp(logLevel, "dpanic") == 0)
		_logLevel = 2;
	else if (strcmp(logLevel, "error") == 0)
		_logLevel = 3;
	else if (strcmp(logLevel, "warn") == 0)
		_logLevel = 4;
	else if (strcmp(logLevel, "info") == 0)
		_logLevel = 5;
	else if (strcmp(logLevel, "debug") == 0)
		_logLevel = 6;
	strcpy(_logDir, logDir);
	_maxSize = maxSize;
	_maxBackups = maxBackups;
	if (!_isInit)
	{
		mutex_create(&_mutex);
		_isInit = 1;
	}
}

void _log(const char *file, int line, const char *funcName, int level, const char *label, const char *log, ...)
{
	char str[128];
	size_t total_len = 0; // 用于累计字符串总长度

	if (!_isInit)
	{
		mutex_create(&_mutex);
		_isInit = 1;
	}

	mutex_lock(&_mutex);
	time_t time = getSystemTimeMs();

	// 1. 时间戳部分
	getIso8601Format(str, 128, time / 1000, time % 1000);
	total_len += strlen(str);
	pushToBuffer(str, strlen(str));

	// 2. 空格
	total_len += 1;
	pushToBuffer(" ", 1);

	// 3. 标签部分
	total_len += strlen(label);
	pushToBuffer(label, strlen(label));

	// 4. 空格
	total_len += 1;
	pushToBuffer(" ", 1);

	// 5. 文件名部分
	total_len += strlen(file);
	pushToBuffer(file, strlen(file));

	// 6. 冒号
	total_len += 1;
	pushToBuffer(":", 1);

	// 7. 行号部分
	sprintf(str, "%d", line);
	total_len += strlen(str);
	pushToBuffer(str, strlen(str));

	// 8. 空格
	total_len += 1;
	pushToBuffer(" ", 1);

	// 9. 线程ID部分
	sprintf(str, "%" PRId64, (int64_t)thread_self_id());
	total_len += 1; // "<"
	pushToBuffer("<", 1);

	total_len += strlen(str);
	pushToBuffer(str, strlen(str));

	total_len += 2; // "> "
	pushToBuffer("> ", 2);

	// 10. 函数名部分
	total_len += strlen(funcName);
	pushToBuffer(funcName, strlen(funcName));

	// 11. 空格
	total_len += 1;
	pushToBuffer(" ", 1);

	// 12. 格式化日志内容部分
	va_list valist;
	va_start(valist, log);

	char *buf = getBuffer(512);
	int ret = vsnprintf(buf, 512, log, valist);

	if (ret <= 512)
	{
		total_len += ret; // 累加实际格式化长度
		returnBuffer(512 - ret);
	}
	else
	{
		total_len += ret; // 累加实际格式化长度
		buf = getBuffer(ret - 512);
		vsnprintf(buf, ret + 1, log, valist);
	}

	va_end(valist);

	// 13. 换行符
	total_len += 2;
	pushToBuffer("\r\n", 2);

	if (level <= _logLevel)
	{
		if (level <= 3)
		{
			flushBuffer();
			mutex_unlock(&_mutex);

			switch (level)
			{
			case 0:
				exit(1);
				break;
			case 1:
				raise(SIGINT);
				break;
#if _DEBUG
			case 2:
				raise(SIGINT);
				break;
#endif
			}
			return;
		}
		else if (!_thread)
		{
			if (thread_create(&_thread, writeThread, NULL) == 0)
			{
				thread_detach(_thread);
			}
		}
	}
	else
	{
		char *logStr = getBuffer(0) - total_len;
		logStr[total_len] = 0;
		printf("%s", logStr);
		returnBuffer(total_len);
	}

	mutex_unlock(&_mutex);
}
static TINT writeThread(void *userdata)
{
	int ret = 0;
	do
	{
		for (int i = 0; i < 30 * 4; i++)
		{
			mutex_lock(&_mutex);
			ret = flushBuffer();
			mutex_unlock(&_mutex);
			thread_sleep(33);
		}
	} while (ret > 0);
	mutex_lock(&_mutex);
	_thread = 0;
	mutex_unlock(&_mutex);
	return 0;
}
static char *getBuffer(size_t len)
{
	char *buf;
	size_t leftSize = _bufferCapacity - _bufferSize;
	if (leftSize < len)
	{
		_bufferCapacity = _bufferCapacity + len * 2 - leftSize;
		_buffer = realloc(_buffer, _bufferCapacity + 4);
	}
	buf = _buffer + _bufferSize;
	_bufferSize += len;
	return buf;
}
static void returnBuffer(int len)
{
	_bufferSize -= len;
}
static void pushToBuffer(const char *buf, size_t len)
{
	memcpy(getBuffer(len), buf, len);
}
static int flushBuffer()
{
	size_t size = 0;
	if (!_fileName[0])
	{
		sprintf(_fileName, "%slog.txt", _logDir);
		createMultiLevelDir(_fileName);
		if (!_file)
		{
			_file = fopen(_fileName, "ab+");
		}
	}
	if (_buffer != NULL && _bufferSize > 0)
	{
		size = _bufferSize;
		if (_file)
		{
			int ret;
			char *pBuf = _buffer;
			_buffer[_bufferSize] = 0;
			printf("%s", _buffer);
			do
			{
				ret = (int)fwrite(pBuf, sizeof(char), _bufferSize, _file);
				if (ret <= 0)
				{
					printf("Fail to write file: %s .error code:%d\n", _fileName, ret);
					break;
				}
				pBuf += ret;
				_bufferSize -= ret;
			} while (_bufferSize);
			fflush(_file);
			long size = ftell(_file);
			if (size >= _maxSize)
			{
				cleanDirectory(_logDir);
				fclose(_file);
				char str[128];
				char newFileName[1024];
				time_t time = getSystemTimeMs();
				getTimeFileFormat(str, 128, time / 1000, time % 1000);
				sprintf(newFileName, "%s%s.txt", _logDir, str);
				if (rename(_fileName, newFileName) != 0)
				{
					printf("Renamed failed %s.\n", newFileName);
				}
				_fileName[0] = 0;
				_file = NULL;
			}
		}
	}
	return size;
}

static void getIso8601Format(char *buf, size_t len, time_t time1, int ms)
{
	struct tm *ptm = localtime(&time1);
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03d+0800",
			 ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
			 ptm->tm_hour, ptm->tm_min, ptm->tm_sec, ms);
}

static void getTimeFileFormat(char *buf, size_t len, time_t time1, int ms)
{
	struct tm *ptm = localtime(&time1);
	snprintf(buf, len, "%04d-%02d-%02dT%02d_%02d_%02d.%03d",
			 ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
			 ptm->tm_hour, ptm->tm_min, ptm->tm_sec, ms);
}

static int createMultiLevelDir(const char *dir)
{
	size_t dirPathLen = strlen(dir);
	if (dirPathLen > MAX_PATH_LEN)
	{
		return -1;
	}
	char tmpDirPath[MAX_PATH_LEN] = {0};
	for (int i = 0; i < dirPathLen; ++i)
	{
		tmpDirPath[i] = dir[i];
		if (tmpDirPath[i] == '\\' || tmpDirPath[i] == '/')
		{
			if (ACCESS(tmpDirPath, 0) != 0)
			{
				int ret = MKDIR(tmpDirPath);
				if (ret != 0)
				{
					return -1;
				}
			}
		}
	}
	return 0;
}
static time_t getSystemTimeMs(void)
{
#if defined(_WIN32) || defined(_WIN64)
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	time_t t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (time_t)((t - 116444736000000000ULL) / 10000);
#elif defined(__unix__) || defined(__APPLE__)
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (time_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
}

static void cleanDirectory(const char *path)
{
#if defined(_WIN32) || defined(_WIN64)
	int fileCount = 0;
	TCHAR earlyfile[260] = {0};
	time_t current_time, earliest_time = MAXINT64;
	char pattern[1024];
	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	WIN32_FIND_DATA data;
	HANDLE h = FindFirstFile(pattern, &data);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (strcmp(data.cFileName, ".") && strcmp(data.cFileName, ".."))
		{
			current_time = timeFileFormatToTimestampMs(data.cFileName);
			if (current_time != -1)
			{
				if (current_time < earliest_time)
				{
					earliest_time = current_time;
					strcpy(earlyfile, data.cFileName);
				}

				fileCount++;
			}
		}
	} while (FindNextFile(h, &data));
	FindClose(h);
	if (fileCount > _maxBackups)
	{
		char fileName[1024];
		time_t time = getSystemTimeMs();
		sprintf(fileName, "%s%s", _logDir, earlyfile);
		DeleteFile(fileName);
	}
#else
	char earlyfile[512] = {0};
	time_t current_time, earliest_time = 0x7FFFFFFFFFFFFFFF;
	int fileCount = 0;
	DIR *dir = opendir(path);
	// int file_count = 0;
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)))
	{
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
		{
			current_time = timeFileFormatToTimestampMs(entry->d_name);
			if (current_time != -1)
			{
				if (current_time < earliest_time)
				{
					earliest_time = current_time;
					strcpy(earlyfile, entry->d_name);
				}

				fileCount++;
			}
		}
	}
	closedir(dir);
	if (fileCount > _maxBackups)
	{
		char fileName[1024];
		// time_t time = getSystemTimeMs();
		sprintf(fileName, "%s%s", _logDir, earlyfile);
		unlink(fileName);
	}
#endif
}

static time_t timeFileFormatToTimestampMs(const char *filename)
{
	int year, month, day, hour, minute, second, milli;
	if (sscanf(filename, "%d-%d-%dT%d_%d_%d.%d.txt",
			   &year, &month, &day,
			   &hour, &minute, &second, &milli) != 7)
	{
		return -1;
	}
	struct tm tm_time = {0};
	tm_time.tm_year = year - 1900;
	tm_time.tm_mon = month - 1;
	tm_time.tm_mday = day;
	tm_time.tm_hour = hour;
	tm_time.tm_min = minute;
	tm_time.tm_sec = second;
	tm_time.tm_isdst = -1;
	time_t seconds = mktime(&tm_time);
	if (seconds == (time_t)-1)
	{

		return -1;
	}
	return (time_t)seconds * 1000LL + milli;
}
