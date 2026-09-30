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
#include"waitgroup.h"
#include"audioringbuffer.h"
class Decoder {
public:
	bool open(AVStream* stream, const std::function<void(AVFrame*)> &onFrame);
	void close();
	void write(AVPacket* packet);
private:
	AVCodecContext* _codecContext{ 0 };
	AudioRingBuffer<AVPacket*,2048> _packetQueue;
	int _isAlive;
	int _eof;
	std::function<void(AVFrame*)> _onFrame;
	WaitGroup _wg;
};
