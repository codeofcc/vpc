#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C"
{
#endif
#include "utils/log.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavutil/avutil.h"
#include "libavutil/time.h"
#ifdef __cplusplus
}
#endif
#include "libgo.h"
#include "video.h"
#include "audio.h"
#include "models.h"
#include "waitgroup.h"
#include "audioringbuffer.h"
#include "clocksync.h"
#include"utils/Delegate.h"
class Play
{
public:
	AC::Delegate<void(Play*, DisplayEventArgs*)> onDisplay = [](auto s, auto e) {};
	AC::Delegate<void(Play*, CursorTimeChangedEventArgs*)> onCursorTimeChanged = [](auto s, auto e) {};
	void start(const char* url);
	void stop();
	void seek(double value);
	void setPixelformat(int value);
	void setIsPaused(int value);
	void setVolume(int value);
	void setSpeed(double value);
	void setIsDisableVideo(int value);
	void setIsDisableAudio(int value);
	void setVideoTrackIndex(int value);
	void setAudioTrackIndex(int value);
protected:
	void _readThread();
	static int _interruptCb(void* arg);
	std::string _url;
	AVFormatContext* _formatContext;
	Video _video;
	Audio _audio;
	std::shared_ptr<ClockSync> _synchronize;
	int _step{ 0 };
	int _isPaused{ 0 };
	int _isLoop{ 1 };
	int _isAlive{ 0 };
	WaitGroup _wg;
};
