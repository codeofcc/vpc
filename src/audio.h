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
#include "libswresample/swresample.h"
#include <libavutil/channel_layout.h> 
#ifdef __cplusplus
}
#endif
#include"libgo.h"
#include"decoder.h"
#include"audioringbuffer.h"
#include"SDL.h"
#include"waitgroup.h"
#include "clocksync.h"
class Audio
{
public:
	bool open(AVStream* stream, const std::shared_ptr<ClockSync> &clock, const std::function<void()>& onEof);
	void close();
	void write(AVPacket* packet);
	void setVolume(int value);
private:
	class Channel {
	public:
		Channel(int capacity):frameQueue(capacity){

		}
		std::shared_ptr<ClockSync> clock;
		AudioRingBuffer<uint8_t>frameQueue;
		int volume;
	};
	void _displayThread();
	static void SDLCALL _audioCallback(void* userdata, Uint8* stream, int len);
    std::function<void()> _onEof;
	Decoder            _decoder;
	AudioRingBuffer<AVFrame*,8> _frameQueue;
	std::shared_ptr<Channel>_channel;
	struct SwrContext* _swrContext{ 0 };
	AVStream* _stream;
	int                _volume = { SDL_MIX_MAXVOLUME };
	int _isAlive;
	WaitGroup _wg;
	static std::unordered_set<std::shared_ptr<Audio::Channel>> _audioChannels;
};