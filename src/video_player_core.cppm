// video_player_core.cppm
module;

#include "play.h"
#include "models.h"
#include <libavutil/frame.h>   // 确保 FFmpeg 头文件在全局模块片段里被包含

export module video_player_core;

// ✅ 用 export using 转出口，不是重声明
export using ::av_frame_clone;
export using ::av_frame_free;

export using ::AVPixelFormat;
export using ::Play;
export using ::DisplayEventArgs;
export using ::CursorTimeChangedEventArgs;

export class VideoPlayerCore : public ::Play
{
public:
    using ::Play::Play;
};