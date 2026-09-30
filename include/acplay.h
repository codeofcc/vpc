#ifndef AC_PLAY_H
#define AC_PLAY_H
#include"config.h"
#include<stdint.h>
/************************************************************************
* @Project:  	ACPlay
* @Decription:  视频播放器
* 这是一个功能完整的支持主流常见媒体格式且支持多路播放的音视频播放器。
* 其播放主体为ffmpeg+sdl，本项目是通过对ffplay的源码修改拓展而成。
* 目前是支持跨平台，在Windows和Linux均可以正常使用。
* 主要更新特性
* v1.0:提供基本的播放控制、多实例化
* v1.1:支持精准定位，播放周期事件
* v1.2:支持windows摄像头预览、倍速播放
* v1.3:支持dxva2硬解渲染，以及设置解码器
* v1.3.10 修复窗口尺寸修改画面卡住问题
*         修改窗口渲染区域为填充
* v1.3.11 播放时间回调根据当前同步类型进行回调、
		  解决异常结束播放时可能出现的关闭音频设备死锁
		  优化精确定位，添加阈值判断。
* v1.3.12 destroy时添加判断避免重复触发stop回调
		  播放结束时销毁SDL_Window并重新显示外部窗口
* v1.3.13 Windows,全局初始化添加原子操作
		  Windows,设置hwnd后，start和stop时将操作抛到窗口所在线程执行
* v1.3.14 Windows渲染采用d3d9，解决sdl窗口相关问题
          去除音频滤镜，音频倍速切换为soundtouch实现，不再限制0.5-2倍速范围。
* v1.3.15 ACPixelFormat的值与AVPixelFormat统一，可以直接使用AVPixelFormat，不再限制ACPixelFormat的几个格式中，需要注意ffmpeg版本枚举值不同，当前使用版本为4.3。
*         去除dxva2渲染h265的绿边
*         去除多线程解码，解决反复播放停止变卡问题。去除多线程解码后，内存使用大量降低，延迟降低（实时流），但解码性能会减弱，播放4k解码性能不足时考虑使用硬解。
* v1.3.16 适配ffmpeg 8.1.2
*
* 下个版本特性：
* 添加mediainfo、解码器信息
* 音轨选择
* 输出频谱
* 流畅定位
* 全平台摄像头播放
* 播放字幕
* 统计信息
* 截图、录制
* 最低延时播放
* 补全硬件加速
* 改为cmake项目
* 优化倍速性能
* 缓存网络流
* 多路同步播放
* 支持sip播放
* 支持国标播放
* drm渲染
* @Verision:  	v1.3.16
* @Author:  	Xin Nie
* @Create:  	2018/11/27 13:34:00
* @LastUpdate:  2024/03/22 11:29:00
************************************************************************
* Copyright @ 2024. All rights reserved.
************************************************************************/
/// <summary>
/// 像素格式
/// ACPixelFormat的值与AVPixelFormat统一，可以直接使用AVPixelFormat
/// 需要注意ffmpeg版本枚举值不同，当前使用版本为4.3
/// </summary>
typedef enum
{
	AC_PIXELFORMAT_NONE = -1,
	/// <summary>
	/// 也叫i420或ffmpeg的yuv420p
	/// </summary>s
	AC_PIXELFORMAT_YU12 = 0,
	/// <summary>
	/// yuyv422
	/// </summary>
	AC_PIXELFORMAT_YUY2 = 1,
	/// <summary>
	/// RGB 8:8:8
	/// </summary>
	AC_PIXELFORMAT_RGB24 = 2,
	/// <summary>
	/// uv交叉排列的420
	/// </summary>
	AC_PIXELFORMAT_NV12 = 23,
	/// <summary>
	/// ARGB 8:8:8:8
	/// </summary>
	AC_PIXELFORMAT_ARGB32 = 25,
	/// <summary>
	/// BGRA 8:8:8:8
	/// </summary>
	AC_PIXELFORMAT_BGRA32 = 28,
	/// <summary>
	/// dxva2解码格式，通常为data[3]是surface对象。
	/// </summary>
	AC_PIXELFORMAT_DXVA2_VLD = 53
}ACPixelFormat;

/// <summary>
/// 硬件加速选项
/// </summary>
typedef enum
{
	AC_HARDWAREACCELERATETYPE_DISABLED,
	AC_HARDWAREACCELERATETYPE_AUTO,
	//使用dxva解码,仅在Windows有效,成功启动：started、display事件的pixformat为AC_PIXELFORMAT_DXVA2_VLD，render事件的data[3]为d3d9的surface对象。
	//注：目前版本对YUV420P10LE的视频似乎不太支持
	AC_HARDWAREACCELERATETYPE_DXVA2
}ACHardwareAccelerateType;


//typedef struct
//{
//	int streamId;
//	int width;
//	int height;
//	double framerate;
//	const char* pixelFormat;
//	const char* codecName;
//	double duration;
//	int bitrate;
//	const char* description;
//}ACVideoStream;
//
//typedef struct
//{
//	int streamId;
//	int samplerate;
//	int channels;
//	int bitPerSample;
//	const char* sampleFormat;
//	const char* codecName;
//	double duration;
//	int bitrate;
//	const char* description;
//}ACAudioStream;
//
///// <summary>
///// 媒体信息
///// </summary>
//typedef struct
//{
//	int videoStreamLength;
//	int audioStreamLength;
//	ACVideoStream* videoStreams;
//	ACAudioStream* audioStreams;
//}ACMediaInfo;




/// <summary>
/// 定位基准
/// </summary>
typedef enum
{
	//总长度
	ACWENCE_SIZE = 0x10000,
	//基头部
	ACWENCE_SEEKSET = 0,
	//基于当前
	ACWENCE_SEEKCUR = 1,
	//基于尾部
	ACWENCE_SEEKEND = 2,
}ACWhence;
#define AVSEEK_SIZE 0x10000
/// <summary>
/// 停止原因
/// </summary>
typedef enum
{
	AC_STOPREASON_NONE,
	//播放结束
	AC_STOPREASON_REACHEND,
	//调用了stop方法
	AC_STOPREASON_USERCALL,
	//出现错误
	AC_STOPREASON_ERROR,
}ACStopReason;

/// <summary>
/// 时钟同步
/// </summary>
typedef enum
{
	//同步到音频
	AC_CLOCKSYNCTYPE_AUDIO,
	//同步到视频频
	AC_CLOCKSYNCTYPE_VIDEO,
	//同步到外部时钟
	AC_CLOCKSYNCTYPE_EXTERNAL,
}ACClockSyncType;

/// <summary>
/// 播放器对象
/// </summary>
typedef  void* ACPlay;
/// <summary>
/// 回调方法
/// </summary>
/// <param name="play">播放器对象</param>
typedef void(*ACPlayCallback) (void* play);
/// <summary>
/// 开始播放时回调方法
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="format">像素格式，当前视频的像素格式，可以设置想要的像素格式，在渲染回调方法中使用，比如：*format=AC_PIXELFORMAT_RGB24
///  格式为AC_PIXELFORMAT_NONE时，可能是不存在视频流、视频流打开失败、格式不支持。
///  当格式为AC_PIXELFORMAT_DXVA2_VLD时不可设置。
/// </param>
/// <param name="width">视频的宽</param>
/// <param name="height">视频的高</param>
/// <param name="duration">视频时长</param>
typedef void(*ACPlayStartedCallback) (ACPlay play, ACPixelFormat* format, int width, int height, double duration);
/// <summary>
/// 播放将要停止的回调方法
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="stopReason">停止原因</param>
typedef void(*ACPlayStoppingCallback) (ACPlay play, ACStopReason stopReason);
/// <summary>
/// 播放时间改变的回调方法
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="time">当前时间，单位秒</param>
typedef void(*ACPlayCursorTimeChangedCallback) (ACPlay play, double time);
/// <summary>
/// 渲染回调方法
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="data">视频数据</param>
/// <param name="linesize">一行大小</param>
/// <param name="width">宽</param>
/// <param name="height">高</param>
/// <param name="format">像素格式</param>
typedef void(*ACPlayDisplayCallback) (ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled);
/// <summary>
/// 自定义输入流读取回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="buf">读取缓存</param>
/// <param name="bufSize">缓存大小</param>
/// <returns>写入缓存的数据长度</returns>
typedef int(*ACPlayCustomPacketReadCallback) (ACPlay play, unsigned char* buf, int bufSize);
/// <summary>
/// 自定义输入流定位回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="offset">下标</param>
/// <param name="whence">定位基准</param>
/// <returns>定位的长度</returns>
typedef int64_t(*ACPlayCustomPacketStreamSeekCallback) (ACPlay play, int64_t offset, ACWhence whence);
/// <summary>
/// 创建播放器
/// </summary>
/// <returns>播放器对象</returns>
AC_API ACPlay ac_play_create();
/// <summary>
/// 销毁播放器
/// </summary>
/// <param name="p">播放器对象</param>
AC_API void ac_play_destroy(ACPlay p);
/// <summary>
/// 设置播放窗口，仅在Windows有效
/// 设置后将渲染到对应窗口中
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="hwnd">窗口句柄</param>
AC_API void ac_play_setWindow(ACPlay  play, void* hwnd);
/// <summary>
/// 获取播放窗口，仅在Windows有效
/// start前设置生效
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>窗口句柄</returns>
AC_API void* ac_play_getWindow(ACPlay  play);
/// <summary>
/// 设置硬件加速类型
/// start前设置生效
/// 如果与视频不兼容，则会自动切换软解
/// 与ac_play_setVideoCodecName方法冲突，指定编码器后,此选项失效。
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">硬件加速类型</param>
AC_API void ac_play_setHardwareAccelerateType(ACPlay  play, ACHardwareAccelerateType value);
/// <summary>
/// 获取硬件加速类型
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>硬件加速类型</returns>
AC_API ACHardwareAccelerateType ac_play_getHardwareAccelerateType(ACPlay  play);
/// <summary>
/// 设置指定视频解码器，可以设置硬解码器，但不可与ac_play_setHardwareAccelerateType方法同时设置。
/// start前设置生效
/// 如果与视频不兼容，则会自动切换默认编码器
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="codec">解码器名称，为ffmpeg的codec名称，通过ffmpeg -decoders命令查看</param>
AC_API void ac_play_setVideoCodecName(ACPlay  play, const char* codec);
/// <summary>
/// 获取视频解码器
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>解码器名称</returns>
AC_API const char* ac_play_getVideoCodecName(ACPlay  play);
/// <summary>
/// 设置是否打印格式信息
/// 视频开始播放时格式信息将打印在控制台
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="isDump">是否打印，1 or 0</param>
AC_API void ac_play_setIsDumpFormat(ACPlay  play, int isDump);
/// <summary>
/// 获取是否打印格式信息
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>否打印，1 or 0</returns>
AC_API int ac_play_getIsDumpFormat(ACPlay  play);
/// <summary>
/// 开始播放
/// 可以重复调用，调用后会先停止上一个播放，再开始。
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="url">播放url：文件路径、rtmp、rtsp、http、摄像头名称。
///  当前版本只支持摄像头的简单预览，无法设置格式和分辨率以及同名摄像头区分</param>
/// <param name="startTime">播放时定位的时间，0则是从头开始播放，对于可定位的视频源有效</param>
AC_API void ac_play_start(ACPlay play, const char* url, double startTime);
/// <summary>
/// 开始播放
/// 可以重复调用，调用后会先停止上一个播放，再开始。
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="url">播放url：文件路径、rtmp、rtsp、http、摄像头名称。
///  当前版本只支持摄像头的简单预览，无法设置格式和分辨率以及同名摄像头区分</param>
/// <param name="format_opts_cmdLine">ffmpeg format选项，目前只对avformat_open_input生效，格式-key1 value1 -key2 value2</param>
/// <param name="startTime">播放时定位的时间，0则是从头开始播放，对于可定位的视频源有效</param>
AC_API void ac_play_startWithOptions(ACPlay play, const char* url, const char* format_opts, double startTime);
/// <summary>
/// 开始播放
/// 可以重复调用，调用后会先停止上一个播放，再开始。
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="read">自定义输入流，读取数据时的回调</param>
/// <param name="seek">自定义输入流，定位时的回调</param>
/// <param name="format_opts_cmdLine">ffmpeg format选项，目前只对avformat_open_input生效，格式-key1 value1 -key2 value2</param>
/// <param name="startTime">播放时定位的时间，0则是从头开始播放，对于可定位的视频源有效</param>
AC_API void ac_play_startViaCustomStream(ACPlay play, ACPlayCustomPacketReadCallback read, ACPlayCustomPacketStreamSeekCallback seek, const char* format_opts, double startTime);
/// <summary>
/// 停止播放
/// </summary>
/// <param name="play">播放器对象</param>
AC_API void ac_play_stop(ACPlay play);
/// <summary>
/// 是否循环播放
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">是否循环播放，1 or 0</param>
AC_API void ac_play_setIsLoop(ACPlay play, int value);
/// <summary>
/// 获取是否循环播放
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否循环播放，1 or 0</returns>
AC_API int ac_play_getIsLoop(ACPlay play);
/// <summary>
/// 设置暂停
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="isPaused">是否暂停，1 or 0</param>
AC_API void ac_play_setIsPause(ACPlay play, int isPaused);
/// <summary>
/// 获取是否暂停
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否暂停，1 or 0</returns>
AC_API int ac_play_getIsPause(ACPlay play);
/// <summary>
/// 静音
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="isMuted">是否静音，1 or 0</param>
AC_API void ac_play_setIsMute(ACPlay play, int isMuted);
/// <summary>
/// 获取是否静音
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否静音，1 or 0</returns>
AC_API int ac_play_getIsMute(ACPlay play);
/// <summary>
/// 禁用视频
/// start前设置生效
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="isMuted">是否禁用，1 or 0</param>
AC_API void ac_play_setIsVideoDisabled(ACPlay play, int isDisable);
/// <summary>
/// 获取是否禁用视频
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否禁用，1 or 0</returns>
AC_API int ac_play_getIsVideoDisabled(ACPlay play);
/// <summary>
/// 禁用音频
/// start前设置生效
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="isMuted">是否禁用，1 or 0</param>
AC_API void ac_play_setIsAudioDisabled(ACPlay play, int isDisable);
/// <summary>
/// 获取是否禁用音频
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否禁用，1 or 0</returns>
AC_API int ac_play_getIsAudioDisabled(ACPlay play);
/// <summary>
/// 是否禁用精准定位
/// 默认非禁用
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="time">是否禁用，1 or 0</param>
AC_API void ac_play_setIsPreciseSeekDisabled(ACPlay play, int isDisable);
/// <summary>
/// 获取是否禁用精准定位
/// 默认非禁用
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>是否禁用，1 or 0</returns>
AC_API int ac_play_getIsPreciseSeekDisabled(ACPlay play);
/// <summary>
/// 设置时钟同步
/// 默认同步到音频
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="type">时钟同步类型</param>
AC_API void ac_play_setClockSyncType(ACPlay play, ACClockSyncType type);
/// <summary>
/// 获取时钟同步类型
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>时钟同步类型</returns>
AC_API ACClockSyncType ac_play_getClockSyncType(ACPlay play);
/// <summary>
/// 定位
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="time">时间，单位秒</param>
AC_API void ac_play_seek(ACPlay play, double time);
/// <summary>
/// 设置音量
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">音量大小，0-100为正常音量。超过则放大</param>
AC_API void ac_play_setVolume(ACPlay play, int value);
/// <summary>
/// 获取音量
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>音量大小</returns>
AC_API int ac_play_getVolume(ACPlay play);
/// <summary>
/// 设置播放倍速
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">倍速</param>
AC_API void ac_play_setSpeed(ACPlay play, double value);
/// <summary>
/// 获取播放倍速
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>倍速</returns>
AC_API double ac_play_getSpeed(ACPlay play);
/// <summary>
/// 设置用户数据
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="userdata">用户数据</param>
AC_API void ac_play_setUserData(ACPlay play, void* userdata);
/// <summary>
/// 获取用户数据
/// </summary>
/// <param name="play">播放器对象</param>
/// <returns>用户数据</returns>
AC_API void* ac_play_getUserData(ACPlay play);
/// <summary>
/// 设置开始播放的回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">回调方法</param>
AC_API void ac_play_setStartedCallback(ACPlay play, ACPlayStartedCallback value);
/// <summary>
/// 设置即将停止播放的回调,循环播放不会触发此事件。
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">回调方法</param>
AC_API void ac_play_setStoppingCallback(ACPlay play, ACPlayStoppingCallback value);
/// <summary>
/// 设置停止的回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">回调方法</param>
AC_API void ac_play_setStoppedCallback(ACPlay play, ACPlayCallback value);
/// <summary>
/// 设置播放位置改变的回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="value">回调方法</param>
AC_API void ac_play_setCursorTimeChangedCallback(ACPlay play, ACPlayCursorTimeChangedCallback value);
/// <summary>
/// 设置渲染的回调
/// </summary>
/// <param name="play">播放器对象</param>
/// <param name="callbask">回调方法</param>
AC_API void ac_play_setDisplayCallback(ACPlay play, ACPlayDisplayCallback value);


void ac_play_test();
#endif