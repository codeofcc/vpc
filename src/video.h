#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
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
#include"libgo.h"
#include"decoder.h"
#include"waitgroup.h"
#include "clocksync.h"
class Video
{
public:
	bool open(AVStream* stream, ClockSync*sync,const std::function<void(AVFrame*)>& onFrame,const std::function<void()>& onEof);
	void close();
	void write(AVPacket*packet);
	void setPixelformat(AVPixelFormat value);
private:
	void _displayThread();
	void _present(AVFrame* frame);
	ClockSync* _sync;
	Decoder            _decoder;
	AudioRingBuffer<AVFrame*, 2> _frameQueue;
	AVStream* _stream;
	enum AVPixelFormat _pixelFormat { AV_PIX_FMT_NONE };
	struct SwsContext* _swsContext{ 0 };
	AVFrame* _swsFrame{ 0 };
	int _isAlive;
	std::function<void(AVFrame*)> _onFrame;
	std::function<void()> _onEof;
	WaitGroup _wg;
};
