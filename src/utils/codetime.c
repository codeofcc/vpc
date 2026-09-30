#include "codetime.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#ifdef _WIN32
#include <windows.h>

static double get_time_sec()
{
	LARGE_INTEGER frequency;
	LARGE_INTEGER ticks;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&ticks);
	return (double)ticks.QuadPart / frequency.QuadPart;
}

#else
#include <time.h>

static double get_time_sec()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif // _WIN32

void rangeCodeTime_reset(RangeCodeTime *rct)
{
	memset(rct, 0, sizeof(RangeCodeTime));
}

void rangeCodeTime_begin(RangeCodeTime *rct)
{
	rct->_d = get_time_sec();
}

void rangeCodeTime_end(RangeCodeTime *rct,  const char* label)
{
	rct->time = get_time_sec() - rct->_d;
	rct->_times[rct->_i++] = rct->time;
	rct->_sum += rct->time;
	rct->average = rct->_sum / rct->_i ;
	if ((rct->_i) == sizeof(rct->_times) / sizeof(double))
	{
		rct->_sum -= rct->_times[0];
		memmove(rct->_times, rct->_times + 1, sizeof(double) * rct->_i);
		rct->_i--;
	}
	if (label)
	{
		printf("%s cost time (s):%lf average (s):%lf\n", label, rct->time, rct->average);
	}
}
