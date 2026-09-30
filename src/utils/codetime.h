#ifndef CODE_TIME_H
#define CODE_TIME_H
typedef struct RangeCodeTime
{
	double _times[60];
	    int _i;
	double _d;
	double _sum;
	double time;
	double average;

} RangeCodeTime;
void rangeCodeTime_reset(RangeCodeTime *rct);
void rangeCodeTime_begin(RangeCodeTime *rct);
void rangeCodeTime_end(RangeCodeTime *rct, const char* label);
#endif