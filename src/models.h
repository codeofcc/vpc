#pragma once

typedef enum {
	MESSAGE_TYPE_NONE = 0,
	MESSAGE_TYPE_AUDIO_ADD_CHANNEL,
	MESSAGE_TYPE_AUDIO_REMOVE_CHANNEL,
} MessageType;

class IMessage {
public:
	MessageType type;

};
template<typename T>
class Message :public IMessage {
public:
	T value;
};

//typedef enum {
//	EVENT_NONE = 0,
//	EVENT_VIDEO_FRAME = 1 << 0,
//	EVENT_PLAY_STARTED = 1 << 1,
//	EVENT_PLAY_STOPPED = 1 << 2,
//	EVENT_PLAY_EOF = 1 << 3,
//	EVENT_ALL = 0xFFFFFFFF
//} EventType;

typedef struct {
	AVFrame* frame;
} DisplayEventArgs;

typedef struct {
	double time;
}CursorTimeChangedEventArgs;
