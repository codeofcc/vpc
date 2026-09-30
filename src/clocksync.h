#pragma once
#include<math.h>
/// <summary>
/// ʱ��ͬ������
/// </summary>
typedef enum
{
	// ͬ������Ƶ
	CLOCKSYNCTYPE_AUDIO,
	// ͬ������Ƶ
	CLOCKSYNCTYPE_VIDEO,
	// ͬ��������ʱ��
	CLOCKSYNCTYPE_ABSOLUTE
} ClockSyncType;

class Clock
{
public:
	bool isDisabled = false;
	// ��ʼʱ��
	//double startTime=0;
	// ��ǰpts
	double startPts=NAN;
	// ��ǰpts
	double currentPts = NAN;
	double getTime();
	void updatePts(double pts);

};

class ClockSync
{
public:
	void reset();
	Clock* getMasterClock();
	//double getMasterTime();
	double calVideoDelay(double pts, double duration);
	double calAudioDelay(double pts, double duration);
	void updateVideoPts(double pts);
	void updateAudioPts(double pts);
	ClockSyncType type;
	Clock audio;
	Clock video;
	Clock absolute;
private:
	/// <summary>
/// ��Ƶʱ��
/// </summary>

	// double estimateVideoDuration{ 0 };
	// double n{0};
};

