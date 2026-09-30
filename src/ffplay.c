#include "acplay.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
// #include "libavcodec/avfft.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
// #include"Collection/acf_queue.h"
#ifdef _WIN32
#include "SDL.h"
#include "SDL_thread.h"
#include<windows.h> 
#include "D3DRender.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
#endif // WIN32
#include"sonic.h"
#include "ffmpeg_dxva2.h"
#include <SDL_syswm.h>
//#include <Collection/acf_array.h>
#include "cSoundTouch.h"


#define  CONFIG_SDLWINDOW 0
#define  CONFIG_AVFILTER 0
typedef struct VideoState VideoState;
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)
/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10
/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20
/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)
//#define CURSOR_HIDE_DELAY 1000000
#define USE_ONEPASS_SUBTITLE_RENDER 1
static unsigned sws_flags = SWS_BICUBIC;

#define AUDIOD_EVICE_FORMAT AUDIO_F32SYS

typedef struct MyAVPacketList {
	AVPacket pkt;
	struct MyAVPacketList* next;
	int serial;
} MyAVPacketList;

typedef struct PacketQueue {
	MyAVPacketList* first_pkt, * last_pkt;
	int nb_packets;
	int size;
	int64_t duration;
	int abort_request;
	int serial;
	SDL_mutex* mutex;
	SDL_cond* cond;
	int is_cond_waited;//添加的参数,用于解决精准定位时判断解码线程未在解码过程中
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
	int freq;
	int channels;
	int64_t channel_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
} AudioParams;

typedef struct Clock {
	double pts;           /* clock base */
	double pts_drift;     /* clock base minus time at which we updated the clock */
	double last_updated;
	double speed;
	int serial;           /* clock is based on a packet with this serial */
	int paused;
	int* queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
	AVFrame* frame;
	AVFrame** scaled_frames;
	AVFrame** scaled_frames_length;
	AVSubtitle sub;
	int serial;
	double pts;           /* presentation timestamp for the frame */
	double duration;      /* estimated duration of the frame */
	int64_t pos;          /* byte position of the frame in the input file */
	int width;
	int height;
	int format;
	AVRational sar;
	int uploaded;
	int flip_v;
} Frame;

typedef struct FrameQueue {
	Frame queue[FRAME_QUEUE_SIZE];
	int rindex;
	int windex;
	int size;
	int max_size;
	int keep_last;
	int rindex_shown;
	SDL_mutex* mutex;
	SDL_cond* cond;
	PacketQueue* pktq;
} FrameQueue;

enum {
	AV_SYNC_AUDIO_MASTER, /* default choice */
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
	AVPacket pkt;
	PacketQueue* queue;
	AVCodecContext* avctx;
	int pkt_serial;
	int finished;
	int packet_pending;
	SDL_cond* empty_queue_cond;
	int64_t start_pts;
	AVRational start_pts_tb;
	int64_t next_pts;
	AVRational next_pts_tb;
	SDL_Thread* decoder_tid;
} Decoder;


typedef struct VideoScale {
	int align;
	int width;
	int height;
	enum AVPixelFormat format;
	struct SwsContext* ctx;
	AVFrame* frame;
} VideoScale;


typedef struct VideoState {
	SDL_Thread* read_tid;
	AVInputFormat* iformat;
	int abort_request;
	int force_refresh;
	int paused;
	int last_paused;
	int queue_attachments_req;
	int seek_req;
	int seek_flags;
	int64_t seek_pos;
	int64_t seek_rel;
	int read_pause_return;
	AVFormatContext* ic;
	int realtime;
	Clock audclk;
	Clock vidclk;
	Clock extclk;
	FrameQueue pictq;
	FrameQueue subpq;
	FrameQueue sampq;
	Decoder auddec;
	Decoder viddec;
	Decoder subdec;
	int audio_stream;
	int av_sync_type;
	double audio_clock;
	int audio_clock_serial;
	double audio_diff_cum; /* used for AV difference average computation */
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	int audio_diff_avg_count;
	AVStream* audio_st;
	PacketQueue audioq;
	int audio_hw_buf_size;
	uint8_t* audio_buf;
	uint8_t* audio_buf1;
	unsigned int audio_buf_size; /* in bytes */
	unsigned int audio_buf1_size;
	int audio_buf_index; /* in bytes */
	int audio_write_buf_size;
	int audio_volume;
	int muted;
	struct AudioParams audio_src;
	struct AudioParams audio_tgt;
	struct SwrContext* swr_ctx;
	int frame_drops_early;
	int frame_drops_late;
	enum ShowMode {
		SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
	} show_mode;
	//int16_t sample_array[SAMPLE_ARRAY_SIZE];
	//int sample_array_index;
	//int last_i_start;
	// RDFTContext* rdft;
	// int rdft_bits;
	// FFTSample* rdft_data;
	int xpos;
	double last_vis_time;
	SDL_Texture* vis_texture;
	SDL_Texture* sub_texture;
	SDL_Texture* vid_texture;
	int subtitle_stream;
	AVStream* subtitle_st;
	PacketQueue subtitleq;
	double frame_timer;
	double frame_last_returned_time;
	double frame_last_filter_delay;
	int video_stream;
	AVStream* video_st;
	PacketQueue videoq;
	double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
	struct SwsContext* img_convert_ctx;
	struct SwsContext* sub_convert_ctx;
	int eof;
	char* filename;
	int width, height, xleft, ytop;
	int step;
	int last_video_stream, last_audio_stream, last_subtitle_stream;
	SDL_cond* continue_read_thread;
#if CONFIG_SDLWINDOW
	SDL_Window* window;//sdl窗口
	SDL_Renderer* renderer;
#endif
	void* hwnd;
	//AVInputFormat* file_iformat;
	int audio_disable;
	int video_disable;
	int subtitle_disable;
	const char* wanted_stream_spec[AVMEDIA_TYPE_NB];
	int seek_by_bytes;
	int display_disable;
	int show_status;
	int64_t start_time;
	int64_t duration;
	int fast;
	int genpts;
	int lowres;
	int decoder_reorder_pts;
	int autoexit;
	int loop;
	int framedrop;
	int infinite_buffer;
	const char* audio_codec_name;
	const char* subtitle_codec_name;
	const char* video_codec_name;
	double rdftspeed;
	//int64_t cursor_last_shown;
	//int cursor_hidden;
	//int autorotate;
	int find_stream_info;
	/* current context */
	int64_t audio_callback_time;
	AVPacket flush_pkt;
	SDL_Thread* event_tid;
#if CONFIG_AVFILTER
	struct AudioParams audio_filter_src;
	int vfilter_idx;
	AVFilterContext* in_video_filter;   // the first filter in the video chain
	AVFilterContext* out_video_filter;  // the last filter in the video chain
	AVFilterContext* in_audio_filter;   // the first filter in the audio chain
	AVFilterContext* out_audio_filter;  // the last filter in the audio chain
	AVFilterGraph* agraph;              // audio filter graph
	const char** vfilters_list;
	int nb_vfilters;
	char* afilters;
	int req_afilter_reconfigure;
#endif
	void* userdata;
	/*unsigned char* render_buf;
	int render_buf_size;*/
	int audio_callback_index;
	//struct SwsContext* render_convert_ctx;
	enum AVPixelFormat render_format;
	ACPlayDisplayCallback render_callback;
	ACPlayStartedCallback begin_callback;
	ACPlayStoppingCallback end_callback;
	ACPlayCallback stopped_callback;
	ACPlayCursorTimeChangedCallback pos_changed_callback;
	//SDL_AudioDeviceID dev;
	double speed;
	InputStream* ist;
	ACHardwareAccelerateType hwaccel;
	//double cursorTime;
	int isDisablePreciseSeek;
	int audioVolume100;
	AVDictionary* format_opts;
	AVIOContext* avio;
	sonicStream sncStream;
	char* speed_buf;
	int    speed_buf_size;
	cSoundTouch soundTouch;
	D3DRender _d3dRender;
	VideoScale* video_scales;
	enum AVPixelFormat d3d_render_format;
} VideoState;
SDL_AudioDeviceID devId;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static const struct TextureFormatEntry {
	enum AVPixelFormat format;
	int texture_fmt;
} sdl_texture_format_map[] = {
	{ AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332 },
	{ AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444 },
	{ AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555 },
	{ AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555 },
	{ AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565 },
	{ AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565 },
	{ AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24 },
	{ AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24 },
	{ AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888 },
	{ AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888 },
	{ AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
	{ AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
	{ AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888 },
	{ AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888 },
	{ AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888 },
	{ AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888 },
	{ AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV },
	{ AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2 },
	{ AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY },
	{ AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN },
};

#define MUTI_OPEN_NUM 64 //支持多开数
static SDL_mutex* audio_streams_mutex = NULL;
static SDL_mutex* initial_mutex = NULL;
static VideoState* open_audio_streams[MUTI_OPEN_NUM];
static int is_init_audio = 0;
static struct AudioParams audio_params;
static int audio_buf_size = 0;
static int is_init_global = 0;
static int   event_loop(void* lpParameter);
static double get_master_clock(VideoState* is);


typedef struct Slice {
	int length;
	int capacity;
	int elementSize;
}Slice;


#define make(t,cap)slice_make(sizeof(t),cap)
#define unmake(t)slice_umake(t);t=NULL;
#define append(...)_ACF_COUNT_ARG(__VA_ARGS__)
#define len(array) slice_len( array)
#define cap(array) slice_cap( array)





#define _ACF_ARG_T(t)  t 
#define _ACF_ARG_N(a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,N,...)  N
#define _ARG_N_HELPER(...)  _ACF_ARG_T(_ACF_ARG_N(__VA_ARGS__))  
#define _ACF_COUNT_ARG(...)  _ARG_N_HELPER(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,_APPEND_ARRAY(__VA_ARGS__),_APPEND(__VA_ARGS__),1 ,0) 
#define _APPEND(a,e)slice_append(a,&e,sizeof(e))
#define _APPEND_ARRAY(a,e,l)slice_appendArray(a,sizeof(*e),e,l)


static inline uint64_t av_get_default_channel_layout(int nb_channels)
{
    AVChannelLayout layout = { 0 };
    av_channel_layout_default(&layout, nb_channels);
    uint64_t mask = layout.u.mask;
    av_channel_layout_uninit(&layout);
    return mask;
}

void* slice_make(size_t elementSize, size_t sliceCap) {
	Slice* slice = malloc(elementSize * sliceCap + sizeof(Slice));
	if (slice)
	{
		slice->capacity = sliceCap;
		slice->elementSize = elementSize;
		slice->length = 0;
		return slice + 1;
	}
	return NULL;
}


void* slice_append(void* array, void* element, size_t elementSize) {
	Slice* slice = (array ? (Slice*)array : (Slice*)slice_make(elementSize, 4)) - 1;
	if (slice->capacity == slice->length) {
		slice->capacity = slice->capacity == 0 ? 4 : slice->capacity * 2;
		if ((slice = realloc(slice, slice->capacity * slice->elementSize + sizeof(Slice))) == NULL)return NULL;
	}
	char* p = slice + 1;
	memcpy(p + slice->elementSize * slice->length++, element, slice->elementSize);
	return  slice + 1;
}

void* slice_appendArray(void* array, size_t elementSize, void* array2, size_t array2Size) {
	Slice* slice = (array ? (Slice*)array : (Slice*)slice_make(elementSize, array2Size)) - 1;
	int newCap = slice->capacity;
	while (newCap < array2Size) {
		newCap << 1;
	}
	if (slice->capacity < newCap) {
		slice->capacity = newCap;
		if ((slice = realloc(slice, slice->capacity * slice->elementSize + sizeof(Slice))) == NULL)return NULL;
	}
	char* p = slice + 1;
	memcpy(p + slice->elementSize * slice->length++, array2, slice->elementSize * array2Size);
	slice->length += array2Size;
	return  slice + 1;
}

size_t slice_len(void* array) {
	if (!array)return 0;
	Slice* slice = (Slice*)array - 1;
	return slice->length;
}

size_t slice_cap(void* array) {
	if (!array)return 0;
	Slice* slice = (Slice*)array - 1;
	return slice->capacity;
}

void slice_umake(void* array) {
	if (array)
	{
		Slice* slice = (Slice*)array - 1;
		free(slice);
	}
}

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2)
{
	/* If channel count == 1, planar and non-planar formats are the same */
	if (channel_count1 == 1 && channel_count2 == 1)
		return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
	else
		return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline int av_get_channel_layout_nb_channels(uint64_t mask)
{
    AVChannelLayout layout = { 0 };
    av_channel_layout_from_mask(&layout, mask);
    int nb = layout.nb_channels;
    av_channel_layout_uninit(&layout);
    return nb;
}
static inline struct SwrContext *swr_alloc_set_opts(struct SwrContext *s,
    int64_t out_ch_layout, enum AVSampleFormat out_sample_fmt, int out_sample_rate,
    int64_t in_ch_layout, enum AVSampleFormat in_sample_fmt, int in_sample_rate,
    int log_offset, void *log_ctx)
{
    AVChannelLayout out_layout = { 0 };
    AVChannelLayout in_layout = { 0 };
    av_channel_layout_from_mask(&out_layout, (uint64_t)out_ch_layout);
    av_channel_layout_from_mask(&in_layout, (uint64_t)in_ch_layout);

    struct SwrContext *ctx = s;
    int ret = swr_alloc_set_opts2(&ctx,
        &out_layout, out_sample_fmt, out_sample_rate,
        &in_layout, in_sample_fmt, in_sample_rate,
        log_offset, log_ctx);

    av_channel_layout_uninit(&out_layout);
    av_channel_layout_uninit(&in_layout);

    if (ret < 0) {
        if (!s)
            swr_free(&ctx);
        return NULL;
    }
    return ctx;
}
// static inline int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
// {
// 	if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
// 		return channel_layout;
// 	else
// 		return 0;
// }

static int packet_queue_put_private(VideoState* is, PacketQueue* q, AVPacket* pkt)
{
	MyAVPacketList* pkt1;
	if (q->abort_request)
		return -1;
	pkt1 = av_malloc(sizeof(MyAVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;
	if (pkt == &is->flush_pkt)
		q->serial++;
	pkt1->serial = q->serial;
	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size + sizeof(*pkt1);
	q->duration += pkt1->pkt.duration;
	/* XXX: should duplicate packet data in DV case */
	SDL_CondSignal(q->cond);
	return 0;
}

static int packet_queue_put(VideoState* is, PacketQueue* q, AVPacket* pkt)
{
	int ret;
	SDL_LockMutex(q->mutex);
	ret = packet_queue_put_private(is, q, pkt);
	SDL_UnlockMutex(q->mutex);
	if (pkt != &is->flush_pkt && ret < 0)
		av_packet_unref(pkt);
	return ret;
}

static int packet_queue_put_nullpacket(VideoState* is, PacketQueue* q, int stream_index)
{
	AVPacket pkt1, * pkt = &pkt1;
	av_init_packet(pkt);
	pkt->data = NULL;
	pkt->size = 0;
	pkt->stream_index = stream_index;
	return packet_queue_put(is, q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue* q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	if (!q->mutex) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->cond = SDL_CreateCond();
	if (!q->cond) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->abort_request = 1;
	return 0;
}

static void packet_queue_flush(PacketQueue* q)
{
	MyAVPacketList* pkt, * pkt1;
	SDL_LockMutex(q->mutex);
	for (pkt = q->first_pkt; pkt; pkt = pkt1) {
		pkt1 = pkt->next;
		av_packet_unref(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue* q)
{
	if (q->mutex)
	{
		packet_queue_flush(q);
	}
	if (q->mutex)
	{
		SDL_DestroyMutex(q->mutex);
		q->mutex = NULL;
	}
	if (q->cond)
	{
		SDL_DestroyCond(q->cond);
		q->cond = NULL;
	}
}

static void packet_queue_abort(PacketQueue* q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 1;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(VideoState* is, PacketQueue* q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 0;
	packet_queue_put_private(is, q, &is->flush_pkt);
	SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial)
{
	MyAVPacketList* pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	for (;;) {
		if (q->abort_request) {
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size + sizeof(*pkt1);
			q->duration -= pkt1->pkt.duration;
			*pkt = pkt1->pkt;
			if (serial)
				*serial = pkt1->serial;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			q->is_cond_waited = 1;
			SDL_CondWait(q->cond, q->mutex);
			q->is_cond_waited = 0;
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

static void decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond) {
	memset(d, 0, sizeof(Decoder));
	d->avctx = avctx;
	d->queue = queue;
	d->empty_queue_cond = empty_queue_cond;
	d->start_pts = AV_NOPTS_VALUE;
	d->pkt_serial = -1;
}

static int decoder_decode_frame(VideoState* is, Decoder* d, AVFrame* frame, AVSubtitle* sub) {
	int ret = AVERROR(EAGAIN);
	for (;;) {
		AVPacket pkt;
		if (d->queue->serial == d->pkt_serial) {
			do {
				if (d->queue->abort_request)
					return -1;
				switch (d->avctx->codec_type) {
				case AVMEDIA_TYPE_VIDEO:
					ret = avcodec_receive_frame(d->avctx, frame);
					/*static int64_t del = 0;
					printf("decode cost %lf  \n", (av_gettime_relative() - del)/ 1000000.0);
					del = av_gettime_relative();*/

					if (ret >= 0) {
						if (is->decoder_reorder_pts == -1) {
							frame->pts = frame->best_effort_timestamp;
						}
						else if (!is->decoder_reorder_pts) {
							frame->pts = frame->pkt_dts;
						}

					}
					break;
				case AVMEDIA_TYPE_AUDIO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0) {
						AVRational tb = (AVRational){ 1, frame->sample_rate };
						if (frame->pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase/*av_codec_get_pkt_timebase(d->avctx)*/, tb);
						else if (d->next_pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);

						if (frame->pts != AV_NOPTS_VALUE) {
							d->next_pts = frame->pts + frame->nb_samples;
							d->next_pts_tb = tb;
						}
					}
					break;
				}
				if (ret == AVERROR_EOF) {
					d->finished = d->pkt_serial;
					avcodec_flush_buffers(d->avctx);
					return 0;
				}
				if (ret >= 0)
					return 1;
			} while (ret != AVERROR(EAGAIN));
		}

		do {
			if (d->queue->nb_packets == 0)
				SDL_CondSignal(d->empty_queue_cond);
			if (d->packet_pending) {
				av_packet_move_ref(&pkt, &d->pkt);
				d->packet_pending = 0;
			}
			else {
				if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
					return -1;
			}
		} while (d->queue->serial != d->pkt_serial);

		if (pkt.data == is->flush_pkt.data) {
			avcodec_flush_buffers(d->avctx);
			d->finished = 0;
			d->next_pts = d->start_pts;
			d->next_pts_tb = d->start_pts_tb;
		}
		else {
			if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
				int got_frame = 0;
				ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
				if (ret < 0) {
					ret = AVERROR(EAGAIN);
				}
				else {
					if (got_frame && !pkt.data) {
						d->packet_pending = 1;
						av_packet_move_ref(&d->pkt, &pkt);
					}
					ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
				}
			}
			else {
				if (!pkt.opaque_ref) {
					pkt.opaque_ref = av_buffer_allocz(sizeof(pkt.pos));
					if (!pkt.opaque_ref)
						return AVERROR(ENOMEM);
					*((int64_t*)pkt.opaque_ref->data) = pkt.pos;
				}
				if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
					av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
					d->packet_pending = 1;
					av_packet_move_ref(&d->pkt, &pkt);
				}
			}
			av_packet_unref(&pkt);
		}
	}
}

static void decoder_destroy(Decoder* d) {
	av_packet_unref(&d->pkt);
	avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame* vp)
{
	av_frame_unref(vp->frame);
	avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
{
	int i;
	memset(f, 0, sizeof(FrameQueue));
	if (!(f->mutex = SDL_CreateMutex())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	if (!(f->cond = SDL_CreateCond())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	f->pktq = pktq;
	f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
	f->keep_last = !!keep_last;
	for (i = 0; i < f->max_size; i++)
		if (!(f->queue[i].frame = av_frame_alloc()))
			return AVERROR(ENOMEM);
	return 0;
}

static void frame_queue_destory(FrameQueue* f)
{
	if (f->mutex)
	{
		int i;
		for (i = 0; i < f->max_size; i++) {
			Frame* vp = &f->queue[i];
			frame_queue_unref_item(vp);
			av_frame_free(&vp->frame);
		}
	}
	if (f->mutex)
	{
		SDL_DestroyMutex(f->mutex);
		f->mutex = NULL;
	}
	if (f->cond)
	{
		SDL_DestroyCond(f->cond);
		f->cond = NULL;
	}
}

static void frame_queue_signal(FrameQueue* f)
{
	SDL_LockMutex(f->mutex);
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static Frame* frame_queue_peek(FrameQueue* f)
{
	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame* frame_queue_peek_next(FrameQueue* f)
{
	return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame* frame_queue_peek_last(FrameQueue* f)
{
	return &f->queue[f->rindex];
}

static Frame* frame_queue_peek_writable(FrameQueue* f)
{
	/* wait until we have space to put a new frame */
	SDL_LockMutex(f->mutex);
	while (f->size >= f->max_size &&
		!f->pktq->abort_request) {
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[f->windex];
}

static Frame* frame_queue_peek_readable(FrameQueue* f)
{
	/* wait until we have a readable a new frame */
	SDL_LockMutex(f->mutex);
	while (f->size - f->rindex_shown <= 0 &&
		!f->pktq->abort_request) {
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue* f)
{
	if (++f->windex == f->max_size)
		f->windex = 0;
	SDL_LockMutex(f->mutex);
	f->size++;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue* f)
{
	if (f->keep_last && !f->rindex_shown) {
		f->rindex_shown = 1;
		return;
	}
	frame_queue_unref_item(&f->queue[f->rindex]);
	if (++f->rindex == f->max_size)
		f->rindex = 0;
	SDL_LockMutex(f->mutex);
	f->size--;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue* f)
{
	return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue* f)
{
	Frame* fp = &f->queue[f->rindex];
	if (f->rindex_shown && fp->serial == f->pktq->serial)
		return fp->pos;
	else
		return -1;
}

static void decoder_abort(Decoder* d, FrameQueue* fq)
{
	if (d->queue)
		packet_queue_abort(d->queue);
	frame_queue_signal(fq);
	if (d->decoder_tid)
	{
		SDL_WaitThread(d->decoder_tid, NULL);
		d->decoder_tid = NULL;
	}
	if (d->queue)
		packet_queue_flush(d->queue);
}

static inline void fill_rectangle(SDL_Renderer* renderer, int  x, int y, int w, int h)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	if (w && h)
		SDL_RenderFillRect(renderer, &rect);
}

static int realloc_texture(SDL_Renderer* renderer, SDL_Texture** texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
	Uint32 format;
	int access, w, h;
	if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
		void* pixels;
		int pitch;
		SDL_LockMutex(initial_mutex);
		SDL_DestroyTexture(*texture);
		if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height))) {
			SDL_UnlockMutex(initial_mutex);
			return -1;
		}
		SDL_UnlockMutex(initial_mutex);

		if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
			return -1;
		if (init_texture) {
			if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
				return -1;
			memset(pixels, 0, pitch * new_height);
			SDL_UnlockTexture(*texture);
		}
		av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
	}
	return 0;
}

static void calculate_display_rect(SDL_Rect* rect,
	int scr_xleft, int scr_ytop, int scr_width, int scr_height,
	int pic_width, int pic_height, AVRational pic_sar)
{
	float aspect_ratio;
	int width, height, x, y;

	if (pic_sar.num == 0)
		aspect_ratio = 0;
	else
		aspect_ratio = (float)av_q2d(pic_sar);

	if (aspect_ratio <= 0.0)
		aspect_ratio = 1.0;
	aspect_ratio *= (float)pic_width / (float)pic_height;

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	height = scr_height;
	width = lrint(height * aspect_ratio) & ~1;
	if (width > scr_width) {
		width = scr_width;
		height = lrint(width / aspect_ratio) & ~1;
	}
	x = (scr_width - width) / 2;
	y = (scr_height - height) / 2;
	rect->x = scr_xleft + x;
	rect->y = scr_ytop + y;
	rect->w = FFMAX(width, 1);
	rect->h = FFMAX(height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode)
{
	int i;
	*sdl_blendmode = SDL_BLENDMODE_NONE;
	*sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
	if (format == AV_PIX_FMT_RGB32 ||
		format == AV_PIX_FMT_RGB32_1 ||
		format == AV_PIX_FMT_BGR32 ||
		format == AV_PIX_FMT_BGR32_1)
		*sdl_blendmode = SDL_BLENDMODE_BLEND;
	for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
		if (format == sdl_texture_format_map[i].format) {
			*sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
			return;
		}
	}
}

static int upload_texture(SDL_Renderer* renderer, SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx) {
	int ret = 0;
	Uint32 sdl_pix_fmt;
	SDL_BlendMode sdl_blendmode;
	get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
	if (realloc_texture(renderer, tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
		return -1;
	switch (sdl_pix_fmt) {
	case SDL_PIXELFORMAT_UNKNOWN:
		/* This should only happen if we are not using avfilter... */
		*img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
			frame->width, frame->height, frame->format, frame->width, frame->height,
			AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
		if (*img_convert_ctx != NULL) {
			uint8_t* pixels[4];
			int pitch[4];
			if (!SDL_LockTexture(*tex, NULL, (void**)pixels, pitch)) {
				sws_scale(*img_convert_ctx, (const uint8_t* const*)frame->data, frame->linesize,
					0, frame->height, pixels, pitch);
				SDL_UnlockTexture(*tex);
			}
		}
		else {
			av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
			ret = -1;
		}
		break;
	case SDL_PIXELFORMAT_IYUV:

		if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
			ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
				frame->data[1], frame->linesize[1],
				frame->data[2], frame->linesize[2]);
		}
		else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
			ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
				frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
				frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
		}
		else {
			av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
			return -1;
		}
		break;
	default:
		if (frame->linesize[0] < 0) {
			ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
		}
		else {
			ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
		}
		break;
	}
	return ret;
}
#if CONFIG_SDLWINDOW
static void video_image_display(VideoState* is)
{
	Frame* vp;
	Frame* sp = NULL;
	SDL_Rect rect;
	vp = frame_queue_peek_last(&is->pictq);


	if (is->subtitle_st) {
		if (frame_queue_nb_remaining(&is->subpq) > 0) {
			sp = frame_queue_peek(&is->subpq);

			if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
				if (!sp->uploaded) {
					uint8_t* pixels[4];
					int pitch[4];
					unsigned	int i;
					if (!sp->width || !sp->height) {
						sp->width = vp->width;
						sp->height = vp->height;
					}
					if (realloc_texture(is->renderer, &is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
						return;

					for (i = 0; i < sp->sub.num_rects; i++) {
						AVSubtitleRect* sub_rect = sp->sub.rects[i];

						sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
						sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
						sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
						sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

						is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
							sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
							sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
							0, NULL, NULL, NULL);
						if (!is->sub_convert_ctx) {
							av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
							return;
						}
						if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)pixels, pitch)) {
							sws_scale(is->sub_convert_ctx, (const uint8_t* const*)sub_rect->data, sub_rect->linesize,
								0, sub_rect->h, pixels, pitch);
							SDL_UnlockTexture(is->sub_texture);
						}
					}
					sp->uploaded = 1;
				}
			}
			else
				sp = NULL;
		}
	}


	int width, height;
	SDL_GetWindowSize(is->window, &width, &height);
	if (is->width != width || is->height != height)
	{
		SDL_LockMutex(initial_mutex);
		is->width = width;
		is->height = height;
		if (is->renderer)
		{
			SDL_DestroyRenderer(is->renderer);
		}
		SDL_RendererInfo info;
		is->renderer = SDL_CreateRenderer(is->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!is->renderer) {
			av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
			is->renderer = SDL_CreateRenderer(is->window, -1, 0);
		}
		if (is->renderer) {
			if (!SDL_GetRendererInfo(is->renderer, &info))
				av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
		}
		SDL_UnlockMutex(initial_mutex);
	}

	rect.x = 0;
	rect.y = 0;
	rect.w = is->width;
	rect.h = is->height;


	//calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

	if (!vp->uploaded) {
		if (upload_texture(is->renderer, &is->vid_texture, vp->frame, &is->img_convert_ctx) < 0)
		{
			return;
		}
		vp->uploaded = 1;
		vp->flip_v = vp->frame->linesize[0] < 0;
	}
	SDL_RenderCopyEx(is->renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
	if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
		SDL_RenderCopy(is->renderer, is->sub_texture, NULL, &rect);
#else
		int i;
		double xratio = (double)rect.w / (double)sp->width;
		double yratio = (double)rect.h / (double)sp->height;
		for (i = 0; i < sp->sub.num_rects; i++) {
			SDL_Rect* sub_rect = (SDL_Rect*)sp->sub.rects[i];
			SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
				.y = rect.y + sub_rect->y * yratio,
				.w = sub_rect->w * xratio,
				.h = sub_rect->h * yratio };
			SDL_RenderCopy(is->renderer, is->sub_texture, sub_rect, &target);
		}
#endif
	}
}
#endif

static inline int compute_mod(int a, int b)
{
	return a < 0 ? a % b + b : a % b;
}

static void stream_component_close(VideoState* is, int stream_index)
{
	AVFormatContext* ic = is->ic;
	AVCodecParameters* codecpar;
	if (!is->ic)
		return;
	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return;
	codecpar = ic->streams[stream_index]->codecpar;
	switch (codecpar->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		decoder_abort(&is->auddec, &is->sampq);
		SDL_LockMutex(audio_streams_mutex);
		int n = 0;
		for (int i = 0; i < MUTI_OPEN_NUM; i++)
		{
			if (open_audio_streams[i] == is)
			{
				open_audio_streams[i] = NULL;
			}
			if (open_audio_streams[i] == NULL)
				n++;
		}
		if (n == MUTI_OPEN_NUM)
		{
			/*SDL_CloseAudio();*/

			SDL_CloseAudioDevice(devId);
			devId = 0;
			is_init_audio = 0;
		}
		SDL_UnlockMutex(audio_streams_mutex);
		decoder_destroy(&is->auddec);
		swr_free(&is->swr_ctx);
		av_freep(&is->audio_buf1);
		is->audio_buf1_size = 0;
		is->audio_buf = NULL;
		// if (is->rdft) {
		// 	av_rdft_end(is->rdft);
		// 	av_freep(&is->rdft_data);
		// 	is->rdft = NULL;
		// 	is->rdft_bits = 0;
		// }
		break;
	case AVMEDIA_TYPE_VIDEO:
		decoder_abort(&is->viddec, &is->pictq);
		decoder_destroy(&is->viddec);
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		decoder_abort(&is->subdec, &is->subpq);
		decoder_destroy(&is->subdec);
		break;
	default:
		break;
	}

	ic->streams[stream_index]->discard = AVDISCARD_ALL;
	switch (codecpar->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		is->audio_st = NULL;
		is->audio_stream = -1;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_st = NULL;
		is->video_stream = -1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_st = NULL;
		is->subtitle_stream = -1;
		break;
	default:
		break;
	}
}
static void set_default_param(VideoState* s) {
#if CONFIG_SDLWINDOW
	s->window = 0;
	s->renderer = 0;
#endif
	s->hwnd = 0;
	//s->file_iformat = 0;
	s->audio_disable = 0;
	s->video_disable = 0;
	s->subtitle_disable = 0;
	s->wanted_stream_spec[0] = 0;
	s->seek_by_bytes = -1;
	s->display_disable = 0;
	s->audio_volume = SDL_MIX_MAXVOLUME;
	s->audioVolume100 = 100;
	s->show_status = 0;
	s->av_sync_type = AV_SYNC_AUDIO_MASTER;
	s->start_time = AV_NOPTS_VALUE;
	s->duration = AV_NOPTS_VALUE;
	s->fast = 0;
	s->genpts = 0;
	s->lowres = 0;
	s->decoder_reorder_pts = -1;
	s->autoexit = 1;
	s->loop = 0;//修改为是否循环播放，原来是循环的次数。
	s->framedrop = -1;
	s->infinite_buffer = -1;
	s->show_mode = SHOW_MODE_NONE;
	s->audio_codec_name = 0;
	s->subtitle_codec_name = 0;
	s->video_codec_name = 0; //"h264_cuvid";
	s->rdftspeed = 0.02;
	//s->cursor_last_shown = 0;
	s->find_stream_info = 1;
	s->audio_callback_time = 0;
	av_init_packet(&s->flush_pkt);
	s->flush_pkt.data = (uint8_t*)&s->flush_pkt;
	s->speed = 1;
}






static void stream_close(VideoState* is)
{

	//if (is->hwnd && invoke(is->hwnd, stream_close, is)) {
	//	return;
	//}	


	/* XXX: use a special url_shutdown call to abort parse cleanly */
	//int64_t del = 0;
	//del = av_gettime_relative();
	is->abort_request = 1;
	if (is->read_tid)
	{
		SDL_WaitThread(is->read_tid, NULL);
		is->read_tid = NULL;
	}
	//printf("read close cost %lf  \n", (av_gettime_relative() - del) / 1000000.0);

	if (is->event_tid)
	{
		SDL_WaitThread(is->event_tid, NULL);
		is->event_tid = NULL;
	}

	/* close each stream */
	if (is->audio_stream >= 0)
		stream_component_close(is, is->audio_stream);
	if (is->video_stream >= 0)
		stream_component_close(is, is->video_stream);
	if (is->subtitle_stream >= 0)
		stream_component_close(is, is->subtitle_stream);



	if (is->avio != NULL)
	{
		if (is->avio->buffer != NULL)
		{
			av_free(is->avio->buffer);
		}
		avio_context_free(&is->avio);
		is->avio = NULL;
	}

	if (is->ic)
	{
		avformat_close_input(&is->ic);

		if (is->ic != NULL)
		{
			printf("avformat close input error\n");
		}
	}
	packet_queue_destroy(&is->videoq);
	packet_queue_destroy(&is->audioq);
	packet_queue_destroy(&is->subtitleq);
	/* free all pictures */
	frame_queue_destory(&is->pictq);
	frame_queue_destory(&is->sampq);
	frame_queue_destory(&is->subpq);

	if (is->continue_read_thread)
	{
		SDL_DestroyCond(is->continue_read_thread);
		is->continue_read_thread = NULL;
	}
	if (is->img_convert_ctx)
	{
		sws_freeContext(is->img_convert_ctx);
		is->img_convert_ctx = NULL;
	}
	if (is->sub_convert_ctx)
	{
		sws_freeContext(is->sub_convert_ctx);
		is->sub_convert_ctx = NULL;
	}
	if (is->filename)
	{
		av_free(is->filename);
		is->filename = NULL;
	}


#if CONFIG_SDLWINDOW
	SDL_LockMutex(initial_mutex);

	if (is->renderer)
	{
		SDL_DestroyRenderer(is->renderer);
		is->renderer = NULL;
	}

	if (is->window)
	{

#if defined(_WIN32)
		int bVisible = (GetWindowLong(is->hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
		SDL_DestroyWindow(is->window);
		if (bVisible)
			ShowWindow(is->hwnd, SW_SHOW);
#else
		SDL_DestroyWindow(is->window);
#endif // ! _WIn32
		is->window = NULL;
	}

	if (is->vis_texture)
	{
		SDL_DestroyTexture(is->vis_texture);
		is->vis_texture = NULL;
	}
	if (is->vid_texture)
	{
		SDL_DestroyTexture(is->vid_texture);
		is->vid_texture = NULL;
	}
	if (is->sub_texture)
	{
		SDL_DestroyTexture(is->sub_texture);
		is->sub_texture = NULL;
	}
	SDL_UnlockMutex(initial_mutex);
#endif
	if (is->ist)
	{
		dxva2_uninit2(is->ist);
		av_free(is->ist);
		is->ist = NULL;
	}
	av_dict_free(&is->format_opts);
#if	CONFIG_AVFILTER
	if (is->vfilters_list)
	{
		av_free(is->vfilters_list);
		is->vfilters_list = NULL;
	}
	avfilter_graph_free(&is->agraph);
#endif
	//if (is->render_buf)
	//{
	//	av_free(is->render_buf);
	//	is->render_buf = NULL;
	//}
	//if (is->render_convert_ctx)
	//{
	//	sws_freeContext(is->render_convert_ctx);
	//	is->render_convert_ctx = NULL;
	//}

	if (is->speed_buf)
	{
		av_free(is->speed_buf);
		is->speed_buf = NULL;
	}
	if (is->sncStream) {
		sonicDestroyStream(is->sncStream);
		is->sncStream = NULL;
	}
	if (is->soundTouch)
	{
		cSoundTouch_destroy(is->soundTouch);
		is->soundTouch = NULL;
	}

	for (int i = 0; i < len(is->video_scales); i++)
	{
		av_frame_free(&is->video_scales->frame);
		if (is->video_scales->ctx)
			sws_freeContext(is->video_scales->ctx);
	}
	unmake(is->video_scales);

	is->abort_request = 0;
	//重置所有字段值，但需要保留用户设置的字段值，由于字段过多，暂时使用下列方法。
	int hwnd = is->hwnd;
	int loop = is->loop;
	int hwaccel = is->hwaccel;
	int paused = is->paused;
	int muted = is->muted;
	int audio_volume = is->audio_volume;
	int audioVolume100 = is->audioVolume100;
	double speed = is->speed;
	void* userdata = is->userdata;
	void* begin_callback = is->begin_callback;
	void* end_callback = is->end_callback;
	void* stopped_callback = is->stopped_callback;
	void* pos_changed_callback = is->pos_changed_callback;
	void* render_callback = is->render_callback;
	const char* video_codec_name = is->video_codec_name;
	const char* audio_codec_name = is->audio_codec_name;
#if CONFIG_AVFILTER
	const char* afilters = is->afilters;
#endif
	int	video_disable = is->video_disable;
	int audio_disable = is->audio_disable;
	int	isDisablePreciseSeek = is->isDisablePreciseSeek;
	int av_sync_type = is->av_sync_type;
	//int dev = is->dev;
	int show_status = is->show_status;
	memset(is, 0, sizeof(VideoState));
	set_default_param(is);
	is->hwnd = hwnd;
	is->loop = loop;
	is->hwaccel = hwaccel;
	is->muted = muted;
	is->audio_volume = audio_volume;
	is->paused = paused;
	is->audioVolume100 = audioVolume100;
	is->speed = speed;
	is->userdata = userdata;
	is->begin_callback = begin_callback;
	is->end_callback = end_callback;
	is->stopped_callback = stopped_callback;
	is->pos_changed_callback = pos_changed_callback;
	is->render_callback = render_callback;
	is->video_codec_name = video_codec_name;
	is->audio_codec_name = audio_codec_name;
#if CONFIG_AVFILTER
	is->afilters = afilters;
#endif
	is->video_disable = video_disable;
	is->audio_disable = audio_disable;
	is->isDisablePreciseSeek = isDisablePreciseSeek;
	is->av_sync_type = av_sync_type;
	is->show_status = show_status;
	/*is->dev = dev;*/
	if (is->stopped_callback)
	{
		is->stopped_callback(is);
	}

}
#if CONFIG_SDLWINDOW
static int video_open(VideoState* is)
{
	//if (!is->window) {
	//	if (is->hwnd)
	//	{
	//		is->window = SDL_CreateWindowFrom(is->hwnd);
	//	}
	//	else
	//		return -1;
	//}

	if (is->window) {
		SDL_RendererInfo info;
		is->renderer = SDL_CreateRenderer(is->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!is->renderer) {
			av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
			is->renderer = SDL_CreateRenderer(is->window, -1, 0);
		}
		if (is->renderer) {
			if (!SDL_GetRendererInfo(is->renderer, &info))
				av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
		}
		SDL_GetWindowSize(is->window, &is->width, &is->height);
	}

	if (!is->window || !is->renderer) {
		av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
		//do_exit(is);
	}

	return 0;
}
#endif



static AVFrame* video_frame_scale(VideoState* is, Frame* vp, int width, int height, enum AVPixelFormat format, int align) {

	VideoScale* scale = NULL;
	for (int i = 0; i < len(is->video_scales); i++) {
		if (is->video_scales[i].format == format && is->video_scales[i].width == width && is->video_scales[i].height == height && is->video_scales[i].align == align)
		{
			scale = &is->video_scales[i]; break;
		}
	}
	if (!scale) {
		VideoScale t;
		is->video_scales = append(is->video_scales, t);
		scale = &is->video_scales[len(is->video_scales) - 1];
		memset(scale, 0, sizeof(VideoScale));
		scale->format = format;
		scale->width = width;
		scale->height = height;
		scale->align = align;
		scale->ctx = sws_getCachedContext(scale->ctx,
			vp->frame->width, vp->frame->height, vp->frame->format, width, height,
			format, sws_flags, NULL, NULL, NULL);
		if (!scale->ctx) {
			av_log(NULL, AV_LOG_ERROR, "sws_getCachedContext fail\n");
			return NULL;
		}
		scale->frame = av_frame_alloc();
		scale->frame->width = width;
		scale->frame->height = height;
		scale->frame->format = format;
		av_frame_get_buffer(scale->frame, align);
	}
	if (scale->ctx && scale->frame)
	{
		int ret = sws_scale(scale->ctx, vp->frame->data, vp->frame->linesize, 0, vp->frame->height, scale->frame->data, scale->frame->linesize);
		if (ret < 1)
		{
			av_log(NULL, AV_LOG_ERROR, "sws_scale fail\n");
			return NULL;
		}
		return scale->frame;
	}
	return NULL;
}




static void video_display(VideoState* is)
{
	/*static int64_t del = 0;
	printf("display cost %lf  \n", (av_gettime_relative() - del) / 1000000.0);
	del = av_gettime_relative();*/

	//回调当前播放时间
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER || is->auddec.avctx == NULL) {
		if (is->pos_changed_callback != NULL)
		{
			is->pos_changed_callback(is, get_master_clock(is));
		}
	}

	Frame* vp;
	vp = frame_queue_peek_last(&is->pictq);
	if (is->render_callback)
	{
		int isHandled = FALSE;
		if (is->render_format != vp->frame->format)
		{
			AVFrame* frame = video_frame_scale(is, vp, vp->width, vp->height, is->render_format, 0);
			if (frame)
				is->render_callback(is, frame->data, frame->linesize, frame->width, frame->height, is->render_format, &isHandled);
		}
		else
		{
			is->render_callback(is, vp->frame->data, vp->frame->linesize, vp->width, vp->height, is->render_format, &isHandled);
		}
		if (isHandled)
			return;
	}

	if (vp->format == AV_PIX_FMT_DXVA2_VLD)
	{
		dxva2_retrieve_data_call(is->viddec.avctx, vp->frame);
		return;
	}
	if (is->hwnd) {
#if CONFIG_SDLWINDOW
		if (!is->window)
		{
			if (!is->hwnd && !is->render_callback) {
				is->window = SDL_CreateWindow(is->hwnd, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 360, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
			}
		}
#endif
		if (!is->_d3dRender) {
			is->d3d_render_format = vp->format;
			is->_d3dRender = d3dRender_create(is->hwnd, vp->width, vp->height, is->d3d_render_format);
			if (!is->_d3dRender)
			{
				is->d3d_render_format = AV_PIX_FMT_BGRA;
				is->_d3dRender = d3dRender_create(is->hwnd, vp->width, vp->height, is->d3d_render_format);
			}
		}
		if (!is->_d3dRender) {
			av_log(NULL, AV_LOG_ERROR, "create d3d renderer fail\n");
			return;
		}
		if (is->d3d_render_format == vp->format)
		{
			d3dRender_present(is->_d3dRender, vp->frame->data, vp->frame->linesize);
		}
		else {
			AVFrame* frame = video_frame_scale(is, vp, vp->width, vp->height, is->d3d_render_format, 64);
			if (frame) {
				d3dRender_present(is->_d3dRender, frame->data, frame->linesize);
			}
		}

#if CONFIG_SDLWINDOW
		if (!is->window)
			return;
		//if (!is->hwnd&& !is->render_callback)
		//{
		//	SDL_Event event;
		//	SDL_PollEvent(&event);
		//}


		SDL_LockMutex(initial_mutex);
		if (!is->renderer)
		{
			if (video_open(is) != 0) {
				SDL_UnlockMutex(initial_mutex);
				return;
			}
		}
		SDL_UnlockMutex(initial_mutex);
		SDL_RenderClear(is->renderer);
		if (is->video_st)
		{
			video_image_display(is);
		}
		SDL_RenderPresent(is->renderer);
#endif
	}
}

static double get_clock(Clock* c)
{
	if (c->queue_serial && *c->queue_serial != c->serial)
		return NAN;
	if (c->paused) {
		return c->pts;
	}
	else {
		double time = av_gettime_relative() / 1000000.0;
		return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
	}
}

static void set_clock_at(Clock* c, double pts, int serial, double time)
{
	c->pts = pts;
	c->last_updated = time;
	c->pts_drift = c->pts - time;
	c->serial = serial;
}

static void set_clock(Clock* c, double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock* c, double speed)
{
	set_clock(c, get_clock(c), c->serial);
	c->speed = speed;
}

static void init_clock(Clock* c, int* queue_serial)
{
	c->speed = 1.0;
	c->paused = 0;
	c->queue_serial = queue_serial;
	set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock* c, Clock* slave)
{
	double clock = get_clock(c);
	double slave_clock = get_clock(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState* is) {
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		if (is->video_st)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		if (is->audio_st)
			return AV_SYNC_AUDIO_MASTER;
		else
		{
			return AV_SYNC_EXTERNAL_CLOCK;
		}
	}
	else {
		return AV_SYNC_EXTERNAL_CLOCK;
	}
}

/* get the current master clock value */
static double get_master_clock(VideoState* is)
{
	double val;

	switch (get_master_sync_type(is)) {
	case AV_SYNC_VIDEO_MASTER:
		val = get_clock(&is->vidclk);
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = get_clock(&is->audclk);
		break;
	default:
		val = get_clock(&is->extclk);
		break;
	}
	return val;
}

static void check_external_clock_speed(VideoState* is) {
	if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
		is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
		set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
	}
	else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
		(is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
		set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
	}
	else {
		double speed = is->extclk.speed;
		if (speed != 1.0)
			set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
	}
}

/* seek in the stream */
static void stream_seek(VideoState* is, int64_t pos, int64_t rel, int seek_by_bytes)
{
	//if (!is->seek_req)
	{
		is->seek_pos = pos;
		is->seek_rel = rel;
		is->seek_flags &= ~AVSEEK_FLAG_BYTE;
		if (seek_by_bytes)
			is->seek_flags |= AVSEEK_FLAG_BYTE;
		is->seek_req = 1;
		if (is->continue_read_thread)
			SDL_CondSignal(is->continue_read_thread);
	}
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState* is)
{
	if (is->paused) {
		is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
		if (is->read_pause_return != AVERROR(ENOSYS)) {
			is->vidclk.paused = 0;
		}
		set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
	}
	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState* is)
{
	stream_toggle_pause(is);
	is->step = 0;
}

static void toggle_mute(VideoState* is)
{
	is->muted = !is->muted;
}

static void step_to_next_frame(VideoState* is)
{
	/* if the stream is paused unpause it, then step */
	if (is->paused)
		stream_toggle_pause(is);
	is->step = 1;
}

static double compute_target_delay(double delay, VideoState* is)
{
	double sync_threshold, diff = 0;
	/* update delay to follow master synchronisation source */
	if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
		/* if video is slave, we try to correct big delays by
		duplicating or deleting a frame */
		diff = get_clock(&is->vidclk) - get_master_clock(is);
		/* skip or repeat frame. We take into account the
		delay to compute the threshold. I still don't know
		if it is the best guess */
		sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
			if (diff <= -sync_threshold)
				delay = FFMAX(0, delay + diff);
			else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
				delay = delay + diff;
			else if (diff >= sync_threshold)
				delay = 2 * delay;
		}
	}
	av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
		delay, -diff);
	return delay;
}

static double vp_duration(VideoState* is, Frame* vp, Frame* nextvp) {
	if (vp->serial == nextvp->serial) {
		double duration = nextvp->pts - vp->pts;
		if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
			return vp->duration;
		else
			return duration;
	}
	else {
		return 0.0;
	}
}

static void update_video_pts(VideoState* is, double pts, int64_t pos, int serial) {
	/* update current video pts */
	set_clock(&is->vidclk, pts, serial);
	sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void* opaque, double* remaining_time)
{

	VideoState* is = opaque;

	double time;
	Frame* sp, * sp2;
	if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
		check_external_clock_speed(is);

	if (!is->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
		time = av_gettime_relative() / 1000000.0;
		if (is->force_refresh || is->last_vis_time + is->rdftspeed < time) {
			video_display(is);
			is->last_vis_time = time;
		}
		*remaining_time = FFMIN(*remaining_time, is->last_vis_time + is->rdftspeed - time);
	}

	if (is->video_st) {
	retry:
		if (frame_queue_nb_remaining(&is->pictq) == 0) {
			// nothing to do, no picture to display in the queue
		}
		else {
			double last_duration, duration, delay;
			Frame* vp, * lastvp;

			/* dequeue the picture */
			lastvp = frame_queue_peek_last(&is->pictq);
			vp = frame_queue_peek(&is->pictq);

			if (vp->serial != is->videoq.serial) {
				frame_queue_next(&is->pictq);
				goto retry;
			}

			if (lastvp->serial != vp->serial)
				is->frame_timer = av_gettime_relative() / 1000000.0;

			if (is->paused)
				goto display;

			/* compute nominal last_duration */
			last_duration = vp_duration(is, lastvp, vp);
			delay = compute_target_delay(last_duration, is);

			time = av_gettime_relative() / 1000000.0;
			if (time < is->frame_timer + delay) {
				*remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
				goto display;
			}

			is->frame_timer += delay;
			if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
				is->frame_timer = time;

			SDL_LockMutex(is->pictq.mutex);
			if (!isnan(vp->pts))
				update_video_pts(is, vp->pts, vp->pos, vp->serial);
			SDL_UnlockMutex(is->pictq.mutex);

			if (frame_queue_nb_remaining(&is->pictq) > 1) {
				Frame* nextvp = frame_queue_peek_next(&is->pictq);
				duration = vp_duration(is, vp, nextvp);
				if (!is->step && (is->framedrop > 0 || (is->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
					is->frame_drops_late++;
					frame_queue_next(&is->pictq);
					goto retry;
				}
			}

			if (is->subtitle_st) {
				while (frame_queue_nb_remaining(&is->subpq) > 0) {
					sp = frame_queue_peek(&is->subpq);

					if (frame_queue_nb_remaining(&is->subpq) > 1)
						sp2 = frame_queue_peek_next(&is->subpq);
					else
						sp2 = NULL;

					if (sp->serial != is->subtitleq.serial
						|| (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
						|| (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
					{
						if (sp->uploaded) {
							unsigned	int i;
							for (i = 0; i < sp->sub.num_rects; i++) {
								AVSubtitleRect* sub_rect = sp->sub.rects[i];
								uint8_t* pixels;
								int pitch, j;

								if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
									for (j = 0; j < sub_rect->h; j++, pixels += pitch)
										memset(pixels, 0, sub_rect->w << 2);
									SDL_UnlockTexture(is->sub_texture);
								}
							}
						}
						frame_queue_next(&is->subpq);
					}
					else {
						break;
					}
				}
			}
			frame_queue_next(&is->pictq);
			is->force_refresh = 1;

			if (is->step && !is->paused)
				stream_toggle_pause(is);
		}

	display:
		/* display picture */
		if (!is->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
			video_display(is);
	}
	is->force_refresh = 0;
}

static int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
	Frame* vp;
#if defined(DEBUG_SYNC)
	printf("frame_type=%c pts=%0.3f\n",
		av_get_picture_type_char(src_frame->pict_type), pts);
#endif
	if (!(vp = frame_queue_peek_writable(&is->pictq)))
		return -1;
	vp->sar = src_frame->sample_aspect_ratio;
	vp->uploaded = 0;
	vp->width = src_frame->width;
	vp->height = src_frame->height;
	vp->format = src_frame->format;
	vp->pts = pts;
	vp->duration = duration;
	vp->pos = pos;
	vp->serial = serial;
	//deleted next line by xin
	//set_default_window_size(vp->width, vp->height, vp->sar);
	av_frame_move_ref(vp->frame, src_frame);
	frame_queue_push(&is->pictq);
	return 0;
}

static int get_video_frame(VideoState* is, AVFrame* frame)
{
	int got_picture;

	if ((got_picture = decoder_decode_frame(is, &is->viddec, frame, NULL)) < 0)
		return -1;
	if (got_picture) {

		frame->pkt_dts /= 2;
		double dpts = NAN;
		if (frame->pts != AV_NOPTS_VALUE)
			dpts = av_q2d(is->video_st->time_base) * frame->pts;
		frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

		if (is->framedrop > 0 || (is->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
			if (frame->pts != AV_NOPTS_VALUE) {
				double diff = dpts - get_master_clock(is);
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
					diff - is->frame_last_filter_delay < 0 &&
					is->viddec.pkt_serial == is->vidclk.serial &&
					is->videoq.nb_packets) {
					is->frame_drops_early++;
					av_frame_unref(frame);
					got_picture = 0;
				}
			}
		}
	}
	return got_picture;
}

#if CONFIG_AVFILTER

static int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph,
	AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
{
	int ret, i;
	int nb_filters = graph->nb_filters;
	AVFilterInOut* outputs = NULL, * inputs = NULL;

	if (filtergraph) {
		outputs = avfilter_inout_alloc();
		inputs = avfilter_inout_alloc();
		if (!outputs || !inputs) {
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		outputs->name = av_strdup("in");
		outputs->filter_ctx = source_ctx;
		outputs->pad_idx = 0;
		outputs->next = NULL;

		inputs->name = av_strdup("out");
		inputs->filter_ctx = sink_ctx;
		inputs->pad_idx = 0;
		inputs->next = NULL;

		if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
			goto fail;
	}
	else {
		if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
			goto fail;
	}

	/* Reorder the filters to ensure that inputs of the custom filters are merged first */
	for (i = 0; i < graph->nb_filters - nb_filters; i++)
		FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

	ret = avfilter_graph_config(graph, NULL);
fail:
	avfilter_inout_free(&outputs);
	avfilter_inout_free(&inputs);
	return ret;
}


static int autorotate = 1;


static int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
{
	enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
	char sws_flags_str[512] = "";
	char buffersrc_args[256];
	AVDictionary* sws_dict = NULL;
	int ret;
	AVFilterContext* filt_src = NULL, * filt_out = NULL, * last_filter = NULL;
	AVCodecParameters* codecpar = is->video_st->codecpar;
	AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
	AVDictionaryEntry* e = NULL;
	int i;

	for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts); i++)
		pix_fmts[i] = sdl_texture_format_map[i].format;

	while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
		if (!strcmp(e->key, "sws_flags")) {
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
		}
		else
			av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
	}
	if (strlen(sws_flags_str))
		sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

	graph->scale_sws_opts = av_strdup(sws_flags_str);

	snprintf(buffersrc_args, sizeof(buffersrc_args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		frame->width, frame->height, frame->format,
		is->video_st->time_base.num, is->video_st->time_base.den,
		codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
	if (fr.num && fr.den)
		av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

	if ((ret = avfilter_graph_create_filter(&filt_src,
		avfilter_get_by_name("buffer"),
		"ffplay_buffer", buffersrc_args, NULL,
		graph)) < 0)
		goto fail;

	ret = avfilter_graph_create_filter(&filt_out,
		avfilter_get_by_name("buffersink"),
		"ffplay_buffersink", NULL, NULL, graph);
	if (ret < 0)
		goto fail;

	if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto fail;

	last_filter = filt_out;

	/* Note: this macro adds a filter before the lastly added filter, so the
	* processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
	AVFilterContext *filt_ctx;                                               \
	\
	ret = avfilter_graph_create_filter(&filt_ctx, \
	avfilter_get_by_name(name), \
	"ffplay_" name, arg, NULL, graph);    \
	if (ret < 0)                                                             \
	goto fail;                                                           \
	\
	ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
	if (ret < 0)                                                             \
	goto fail;                                                           \
	\
	last_filter = filt_ctx;                                                  \
	} while (0)

	if (autorotate) {
		/*double theta = get_rotation(is->video_st);
		if (fabs(theta - 90) < 1.0) {
			INSERT_FILT("transpose", "clock");
		}
		else if (fabs(theta - 180) < 1.0) {
			INSERT_FILT("hflip", NULL);
			INSERT_FILT("vflip", NULL);
		}
		else if (fabs(theta - 270) < 1.0) {
			INSERT_FILT("transpose", "cclock");
		}
		else if (fabs(theta) > 1.0) {
			char rotate_buf[64];
			snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
			INSERT_FILT("rotate", rotate_buf);
		}*/
	}

	if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
		goto fail;

	is->in_video_filter = filt_src;
	is->out_video_filter = filt_out;

fail:
	av_dict_free(&sws_dict);
	return ret;
}

static int configure_audio_filters(VideoState* is, const char* afilters, int force_output_format)
{
	static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
	int sample_rates[2] = { 0, -1 };
	int64_t channel_layouts[2] = { 0, -1 };
	int channels[2] = { 0, -1 };
	AVFilterContext* filt_asrc = NULL, * filt_asink = NULL;
	char aresample_swr_opts[512] = "";
	AVDictionaryEntry* e = NULL;
	char asrc_args[256] = { 0 };
	int ret;
	AVDictionary* swr_opts = NULL;

	avfilter_graph_free(&is->agraph);

	///以一下代码会内存泄露，完全是局部代码，结合整体流程就会内存泄露，如果添加while死循环调用则不会泄露，原因未找到。
	/*AVFilterGraph* agraph= avfilter_graph_alloc();

	ret = snprintf(asrc_args, sizeof(asrc_args),
		"sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
		is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
		is->audio_filter_src.channels,
		1, is->audio_filter_src.freq);

	ret = avfilter_graph_create_filter(&filt_asrc,
		avfilter_get_by_name("abuffer"), "ffplay_abuffer",
		asrc_args, NULL, agraph);
	if (ret < 0)
		goto end;



	ret = avfilter_graph_create_filter(&filt_asink,
		avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
		NULL, NULL, agraph);
	if (ret < 0)
		goto end;
	avfilter_graph_free(&agraph);
	return -1;*/

	if (!(is->agraph = avfilter_graph_alloc()))
		return AVERROR(ENOMEM);

	while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
		av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
	if (strlen(aresample_swr_opts))
		aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
	av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

	ret = snprintf(asrc_args, sizeof(asrc_args),
		"sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
		is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
		is->audio_filter_src.channels,
		1, is->audio_filter_src.freq);
	if (is->audio_filter_src.channel_layout)
		snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
			":channel_layout=0x%"PRIx64, is->audio_filter_src.channel_layout);


	ret = avfilter_graph_create_filter(&filt_asrc,
		avfilter_get_by_name("abuffer"), "ffplay_abuffer",
		asrc_args, NULL, is->agraph);
	if (ret < 0)
		goto end;



	ret = avfilter_graph_create_filter(&filt_asink,
		avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
		NULL, NULL, is->agraph);
	if (ret < 0)
		goto end;

	//此处存在内存泄露，原因未找到。
	/*ret = -1;
	goto end;*/


	if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto end;
	if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
		goto end;

	if (force_output_format) {
		channel_layouts[0] = is->audio_tgt.channel_layout;
		channels[0] = is->audio_tgt.channels;
		sample_rates[0] = is->audio_tgt.freq;
		if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
		if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
			goto end;
	}



	if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
		goto end;

	is->in_audio_filter = filt_asrc;
	is->out_audio_filter = filt_asink;

end:
	if (ret < 0)
		avfilter_graph_free(&is->agraph);
	av_dict_free(&swr_opts);
	return ret;
}

#endif


static int audio_thread(void* arg)
{

	VideoState* is = arg;
	AVFrame* frame = av_frame_alloc();
	Frame* af;
	int got_frame = 0;
	AVRational tb;
	int ret = 0;
	//ACStopReason reason = AC_STOPREASON_ERROR;
#if CONFIG_AVFILTER
	int last_serial = -1;
	int64_t dec_channel_layout;
	int reconfigure;
#endif

	//enterThread(is);

	if (!frame)
	{
		//exitThread(is);
		return AVERROR(ENOMEM);
	}
	do {
		if ((got_frame = decoder_decode_frame(is, &is->auddec, frame, NULL)) < 0)
		{

			goto the_end;
		}

		if (got_frame) {
			tb = (AVRational){ 1, frame->sample_rate };
			//使用原来的pts
			double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
			int64_t pos = *((int64_t*)frame->opaque_ref->data);
			int serial = is->auddec.pkt_serial;
			double duration = av_q2d((AVRational) { frame->nb_samples, frame->sample_rate });
			//使用原来的pts --end
#if CONFIG_AVFILTER
			dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);
			reconfigure =
				cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
					frame->format, frame->channels) ||
				is->audio_filter_src.channel_layout != dec_channel_layout ||
				is->audio_filter_src.freq != frame->sample_rate ||
				is->auddec.pkt_serial != last_serial;
			if (reconfigure || is->req_afilter_reconfigure) {
				is->req_afilter_reconfigure = 0;
				char buf1[1024], buf2[1024];
				av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
				av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
				av_log(NULL, AV_LOG_DEBUG,
					"Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
					is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
					frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);
				is->audio_filter_src.fmt = frame->format;
				is->audio_filter_src.channels = frame->channels;
				is->audio_filter_src.channel_layout = dec_channel_layout;
				is->audio_filter_src.freq = frame->sample_rate;
				last_serial = is->auddec.pkt_serial;
				if ((ret = configure_audio_filters(is, is->afilters, 1)) < 0)
					goto the_end;
			}

			if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
				goto the_end;
			while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
				tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
				if (!(af = frame_queue_peek_writable(&is->sampq)))
					goto the_end;
				//使用原来的pts
				af->pts = pts;// (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
				af->pos = pos;//frame->pkt_pos;
				af->serial = serial;// is->auddec.pkt_serial;
				af->duration = duration;// av_q2d((AVRational) { frame->nb_samples, frame->sample_rate });
				//使用原来的pts --end
				av_frame_move_ref(af->frame, frame);
				frame_queue_push(&is->sampq);
#if CONFIG_AVFILTER
				if (is->audioq.serial != is->auddec.pkt_serial)
					break;
			}
			if (ret == AVERROR_EOF)
				is->auddec.finished = is->auddec.pkt_serial;
#endif
		}
	} while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
#if CONFIG_AVFILTER
	avfilter_graph_free(&is->agraph);
#endif
	av_frame_free(&frame);
	//exitThread(is);
	return ret;
}

static int decoder_start(VideoState* is, Decoder* d, int(*fn)(void*), void* arg)
{
	packet_queue_start(is, d->queue);
	d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
	if (!d->decoder_tid) {
		av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread() 1: %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	return 0;
}

static int video_thread(void* arg)
{
	VideoState* is = arg;
	AVFrame* frame = av_frame_alloc();
	double pts;
	double duration;
	int ret;
	AVRational tb = is->video_st->time_base;
	AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
	//enterThread(is);
	//#if CONFIG_AVFILTER
	//	AVFilterGraph* graph = avfilter_graph_alloc();
	//	AVFilterContext* filt_out = NULL, * filt_in = NULL;
	//	int last_w = 0;
	//	int last_h = 0;
	//	enum AVPixelFormat last_format = -2;
	//	int last_serial = -1;
	//	int last_vfilter_idx = 0;
	//	if (!graph) {
	//		av_frame_free(&frame);
	//		return AVERROR(ENOMEM);
	//	}
	//
	//#endif
	if (!frame) {
		//#if CONFIG_AVFILTER
		//		avfilter_graph_free(&graph);
		//#endif

		//exitThread(is);
		return AVERROR(ENOMEM);
	}
	for (;;) {
		ret = get_video_frame(is, frame);
		if (ret < 0)
			goto the_end;
		if (!ret)
			continue;
		//#if CONFIG_AVFILTER
		//		if (last_w != frame->width
		//			|| last_h != frame->height
		//			|| last_format != frame->format
		//			|| last_serial != is->viddec.pkt_serial
		//			|| last_vfilter_idx != is->vfilter_idx) {
		//			av_log(NULL, AV_LOG_DEBUG,
		//				"Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
		//				last_w, last_h,
		//				(const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
		//				frame->width, frame->height,
		//				(const char*)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
		//			avfilter_graph_free(&graph);
		//			graph = avfilter_graph_alloc();
		//			if ((ret = configure_video_filters(graph, is, is->vfilters_list ? is->vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
		//				SDL_Event event;
		//				event.type = FF_QUIT_EVENT;
		//				event.user.data1 = is;
		//				SDL_PushEvent(&event);
		//				goto the_end;
		//			}
		//			filt_in = is->in_video_filter;
		//			filt_out = is->out_video_filter;
		//			last_w = frame->width;
		//			last_h = frame->height;
		//			last_format = frame->format;
		//			last_serial = is->viddec.pkt_serial;
		//			last_vfilter_idx = is->vfilter_idx;
		//			frame_rate = av_buffersink_get_frame_rate(filt_out);
		//		}
		//
		//		ret = av_buffersrc_add_frame(filt_in, frame);
		//		if (ret < 0)
		//			goto the_end;
		//
		//		while (ret >= 0) {
		//			is->frame_last_returned_time = av_gettime_relative() / 1000000.0;
		//
		//			ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
		//			if (ret < 0) {
		//				if (ret == AVERROR_EOF)
		//					is->viddec.finished = is->viddec.pkt_serial;
		//				ret = 0;
		//				break;
		//			}
		//
		//			is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
		//			if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
		//				is->frame_last_filter_delay = 0;
		//			tb = av_buffersink_get_time_base(filt_out);
		//#endif
		duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) { frame_rate.den, frame_rate.num }) : 0);
		pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
		ret = queue_picture(is, frame, pts, duration, /*frame->pkt_pos */ *((int64_t*)frame->opaque_ref->data), is->viddec.pkt_serial);
		av_frame_unref(frame);
		//#if CONFIG_AVFILTER
		//		}
		//#endif
		if (ret < 0)
			goto the_end;
	}
the_end:
	//#if CONFIG_AVFILTER
	//	avfilter_graph_free(&graph);
	//#endif
	av_frame_free(&frame);
	//exitThread(is);
	return 0;
}

static int subtitle_thread(void* arg)
{
	VideoState* is = arg;
	Frame* sp;
	int got_subtitle;
	double pts;
	//enterThread(is);
	for (;;) {
		if (!(sp = frame_queue_peek_writable(&is->subpq)))
		{
			//exitThread(is);
			return 0;

		}
		if ((got_subtitle = decoder_decode_frame(is, &is->subdec, NULL, &sp->sub)) < 0)
		{

			break;
		}
		pts = 0;
		if (got_subtitle && sp->sub.format == 0) {
			if (sp->sub.pts != AV_NOPTS_VALUE)
				pts = sp->sub.pts / (double)AV_TIME_BASE;
			sp->pts = pts;
			sp->serial = is->subdec.pkt_serial;
			sp->width = is->subdec.avctx->width;
			sp->height = is->subdec.avctx->height;
			sp->uploaded = 0;
			/* now we can update the picture count */
			frame_queue_push(&is->subpq);
		}
		else if (got_subtitle) {
			avsubtitle_free(&sp->sub);
		}
	}
	//exitThread(is);
	return 0;
}

///* copy samples for viewing in editor window */
//static void update_sample_display(VideoState* is, short* samples, int samples_size)
//{
//	int size, len;
//	size = samples_size / sizeof(short);
//	while (size > 0) {
//		len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
//		if (len > size)
//			len = size;
//		memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
//		samples += len;
//		is->sample_array_index += len;
//		if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
//			is->sample_array_index = 0;
//		size -= len;
//	}
//}

/* return the wanted number of samples to get better sync if sync_type is video
* or external master clock */
static int synchronize_audio(VideoState* is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;
	/* if not master, then we try to remove or add samples to correct the clock */
	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;
		diff = get_clock(&is->audclk) - get_master_clock(is);
		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				/* not enough measures to have a correct estimate */
				is->audio_diff_avg_count++;
			}
			else {
				/* estimate the A-V difference */
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if (fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
					min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
				}
				av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
					diff, avg_diff, wanted_nb_samples - nb_samples,
					is->audio_clock, is->audio_diff_threshold);
			}
		}
		else {
			/* too big difference : may be initial PTS errors, so
			reset A-V filter */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return wanted_nb_samples;
}

/**
* Decode one audio frame and return its uncompressed size.
*
* The processed audio frame is decoded, converted if required, and
* stored in is->audio_buf, with size in bytes given by the return
* value.
*/
static int audio_decode_frame(VideoState* is)
{
	int data_size, resampled_data_size;
	int64_t dec_channel_layout;
	av_unused double audio_clock0;
	int wanted_nb_samples;
	Frame* af;
	if (is->paused)
		return -1;
	do {
#if defined(_WIN32)
		while (frame_queue_nb_remaining(&is->sampq) == 0) {
			if ((av_gettime_relative() - is->audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
				return -1;
			av_usleep(1000);
		}
#endif
		if (!(af = frame_queue_peek_readable(&is->sampq)))
			return -1;
		frame_queue_next(&is->sampq);
	} while (af->serial != is->audioq.serial);

	data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
		af->frame->nb_samples,
		af->frame->format, 1);
	dec_channel_layout =
		(af->frame->ch_layout.u.mask && af->frame->ch_layout.nb_channels == av_get_channel_layout_nb_channels(af->frame->ch_layout.u.mask)) ?
		af->frame->ch_layout.u.mask : av_get_default_channel_layout(af->frame->ch_layout.nb_channels);
	wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

	if (af->frame->format != is->audio_src.fmt ||
		dec_channel_layout != is->audio_src.channel_layout ||
		af->frame->sample_rate != is->audio_src.freq ||
		(wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
		swr_free(&is->swr_ctx);
		is->swr_ctx = swr_alloc_set_opts(NULL,
			is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
			dec_channel_layout, af->frame->format, af->frame->sample_rate,
			0, NULL);
		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
			av_log(NULL, AV_LOG_ERROR,
				"Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
				af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
				is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
			swr_free(&is->swr_ctx);
			return -1;
		}
		is->audio_src.channel_layout = dec_channel_layout;
		is->audio_src.channels = af->frame->ch_layout.nb_channels;
		is->audio_src.freq = af->frame->sample_rate;
		is->audio_src.fmt = af->frame->format;
	}
	if (is->swr_ctx) {
		const uint8_t** in = (const uint8_t**)af->frame->extended_data;
		uint8_t** out = &is->audio_buf1;
		int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
		int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
		int len2;
		if (out_size < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->frame->nb_samples) {
			if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
				wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
				av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
		if (!is->audio_buf1)
			return AVERROR(ENOMEM);
		len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
		if (len2 < 0) {
			av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count) {
			av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(is->swr_ctx) < 0)
				swr_free(&is->swr_ctx);
		}
		is->audio_buf = is->audio_buf1;
		resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	}
	else {
		is->audio_buf = af->frame->data[0];
		resampled_data_size = data_size;
	}


	double speed = is->speed;
	if (speed != 1)
	{
		//设置倍速
		cSoundTouch_setTempo(is->soundTouch, speed);
		//写入音频数据
		cSoundTouch_putSamples(is->soundTouch, is->audio_buf, af->frame->nb_samples);
		int numSamples = 2 * af->frame->nb_samples / speed;
		//if (speed < 1)
			//倍速小于1时使用自己的缓冲区
		{
			int size = numSamples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
			if (is->speed_buf_size < size)
			{
				is->speed_buf = av_realloc(is->speed_buf, size);
				is->speed_buf_size = size;
			}
			is->audio_buf = is->speed_buf;
		}
		//读取处理后的数据
		int new_nb_samples = cSoundTouch_receiveSamples(is->soundTouch, is->audio_buf, numSamples);
		//更新参数
		resampled_data_size = new_nb_samples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
		af->frame->nb_samples = numSamples;
	}



	//double speed = is->speed;
	//if (speed != 1)
	//{
	//	//设置倍速
	//	sonicSetSpeed(is->sncStream, speed);
	//	sonicSetQuality(is->sncStream, 0);
	//	//写入音频数据
	//	int ret = sonicWriteFloatToStream(is->sncStream, is->audio_buf, af->frame->nb_samples);
	//	if (ret) {
	//		//计算新的nb_samples,乘以2是为了保证sonicReadShortFromStream能一次读取全部的数据，否则可能造成累计延迟。
	//		int numSamples = 2 * af->frame->nb_samples / speed;
	//		//if (speed < 1)
	//		//	//倍速小于1时使用自己的缓冲区
	//		{
	//			//计算缓冲区大小
	//			int size = numSamples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	//			if (is->speed_buf_size < size)
	//			{
	//				is->speed_buf = av_realloc(is->speed_buf, size);
	//				is->speed_buf_size = size;
	//			}
	//			is->audio_buf = is->speed_buf;
	//		}
	//		//读取处理后的数据
	//		int new_nb_samples = sonicReadFloatFromStream(is->sncStream, is->audio_buf, numSamples);
	//		//重新计算数据大小
	//		resampled_data_size = new_nb_samples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	//		//设置新的nb_samples
	//		af->frame->nb_samples = new_nb_samples;
	//	}
	//}



	audio_clock0 = is->audio_clock;
	/* update the audio clock with the pts */
	if (!isnan(af->pts))
		is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
	else
		is->audio_clock = NAN;
	is->audio_clock_serial = af->serial;
#ifdef DEBUG
	{
		static double last_clock;
		printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
			is->audio_clock - last_clock,
			is->audio_clock, audio_clock0);
		last_clock = is->audio_clock;
	}
#endif
	return resampled_data_size;
}
static void setVolume(char* buf, UINT32 size, UINT32 uRepeat, double vol)
{
	if (!size)
	{
		return;
	}
	for (int i = 0; i < size; i += 2)
	{
		short wData;
		wData = MAKEWORD(buf[i], buf[i + 1]);
		long dwData = wData;
		for (int j = 0; j < uRepeat; j++)
		{
			dwData = dwData * vol;
			if (dwData < -0x8000)
			{
				dwData = -0x8000;
			}
			else if (dwData > 0x7FFF)
			{
				dwData = 0x7FFF;
			}
		}
		wData = LOWORD(dwData);
		buf[i] = LOBYTE(wData);
		buf[i + 1] = HIBYTE(wData);
	}
}

//static int adjustmentVolume(short* samples, int numSamples, float factor)
//{
//	int tmpValue;
//	if (0 == factor)
//	{
//		memset((void*)samples, 0, numSamples * sizeof(short));
//		return numSamples;
//	}
//	else if (1.0 == factor)
//	{
//		return numSamples;
//	}
//
//	for (int i = 0; i < numSamples; i++)
//	{
//		tmpValue = samples[i] * factor; //这样运算出来仍然是整数
//		if (tmpValue < -32768)
//		{
//			tmpValue = -32768;
//		}
//		else if (tmpValue > 32767)
//		{
//			tmpValue = 32767;
//		}
//		samples[i] = tmpValue;
//	}
//	return numSamples;
//}
int adjustmentVolume(short* samples, int numSamples, int factor) {
	const short MIND = -0x8000;
	const short MAXD = 0x7FFF;
	short data = 0, maxData = 0, minData = 0;
	//获取一个音频帧中的最大值`max`和最小值`min`
	for (int i = 0; i < numSamples; i++) {
		data = samples[i];
		maxData = maxData > data ? maxData : data;
		minData = minData < data ? minData : data;
	}
	//根据获取到的最大值和最小值分别计算出在不失真的情况下，允许的放大倍数`maxfactor`和`minfactor`
	short maxfactor = maxData != 0 ? MAXD / maxData : 1;
	short minfactor = minData != 0 ? MIND / minData : 1;

	//取其最小值为允许的放大倍数`allowfactor`
	short allowfactor = maxfactor > minfactor ? minfactor : maxfactor;
	//选择合适的振幅的系数
	factor = factor > allowfactor ? allowfactor : factor;

	if (factor == 1) {
		return numSamples;
	}
	else if (0 == factor)
	{
		memset((void*)samples, 0, numSamples * sizeof(short));
		return numSamples;
	}
	//对PCM数据放大
	long newData = 0;
	for (int i = 0; i < numSamples; i++) {
		data = samples[i];
		newData = data * factor;
		//边界值溢出处理
		if (newData < MIND) {
			newData = MIND;
		}
		else if (newData > MAXD) {
			newData = MAXD;
		}
		data = newData & 0xffff;
		samples[i] = data;
	}

	return numSamples;
}
/* prepare a new audio buffer */
static void sdl_audio_callback(void* opaque, Uint8* stream, int len)
{
	VideoState* is = opaque;
	int audio_size, len1;
	is->audio_callback_time = av_gettime_relative();
	if (is->av_sync_type == AV_SYNC_AUDIO_MASTER || is->av_sync_type == AV_SYNC_EXTERNAL_CLOCK)
	{
		if (is->pos_changed_callback != NULL)
		{

			is->pos_changed_callback(is, get_master_clock(is));
		}
	}

	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			audio_size = audio_decode_frame(is);
			if (audio_size < 0) {
				/* if error, just output silence */
				is->audio_buf = NULL;
				is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
			}
			else {
				/*if (is->show_mode != SHOW_MODE_VIDEO)
					update_sample_display(is, (int16_t*)is->audio_buf, audio_size);*/
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		/*	if (!is->muted && is->audio_buf)
			{
				if(is->audioVolume100 !=100)
				setVolume(is->audio_buf + is->audio_buf_index, len1,1,is->audioVolume100 /100.0);
				if (is->audio_callback_index == 0)
				{
					memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
				}
				else
				{

					SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIOD_EVICE_FORMAT, len1, SDL_MIX_MAXVOLUME);
				}
			}*/

		if (is->audio_callback_index == 0)
		{

			if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
			{
				//setVolume(is->audio_buf + is->audio_buf_index, len1, 1, is->audioVolume100 / 100.0);
				//adjustmentVolume(is->audio_buf + is->audio_buf_index, len1, is->audioVolume100 / 100.0);
				memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
			}
			else {
				memset(stream, 0, len1);
				if (!is->muted && is->audio_buf)
					SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIOD_EVICE_FORMAT, len1, is->audio_volume);
			}
		}
		else
		{
			if (!is->muted && is->audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIOD_EVICE_FORMAT, len1, is->audio_volume);
		}

		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
	is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
	/* Let's assume the audio driver that is used by SDL has two periods. */
	if (!isnan(is->audio_clock)) {
		set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, is->audio_callback_time / 1000000.0);
		sync_clock_to_slave(&is->extclk, &is->audclk);
	}
}

static void sdl_audio_callback_sum(void* opaque, Uint8* stream, int len)
{
	int n = 0;
	VideoState* is = opaque;
	while (SDL_TryLockMutex(audio_streams_mutex) != 0)
	{
		if (is->abort_request)
			return;
		SDL_Delay(10);
	}
	for (int i = 0; i < MUTI_OPEN_NUM; i++)
	{
		if (open_audio_streams[i])
		{
			open_audio_streams[i]->audio_callback_index = n++;
			sdl_audio_callback(open_audio_streams[i], stream, len);
		}
	}
	SDL_UnlockMutex(audio_streams_mutex);
}

static int audio_open(void* opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
	VideoState* is = opaque;
	SDL_LockMutex(audio_streams_mutex);
	if (!is_init_audio)
	{
		SDL_AudioSpec wanted_spec, spec;
		const char* env;
		static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
		static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
		int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
		env = SDL_getenv("SDL_AUDIO_CHANNELS");
		if (env) {
			wanted_nb_channels = atoi(env);
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		}
		if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
			wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
		}
		wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
		wanted_spec.channels = wanted_nb_channels;
		wanted_spec.freq = wanted_sample_rate;
		if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
			av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
			SDL_UnlockMutex(audio_streams_mutex);
			return -1;
		}
		while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
			next_sample_rate_idx--;
		wanted_spec.format = AUDIOD_EVICE_FORMAT;
		wanted_spec.silence = 0;
		wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
		wanted_spec.callback = sdl_audio_callback_sum;
		wanted_spec.userdata = opaque;

		//#ifdef WIN32
		//		CoInitialize(NULL);
		//#endif // WIN32


		while (/*SDL_OpenAudio(&wanted_spec, &spec) */(devId = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 1)) < 2) {
			av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
				wanted_spec.channels, wanted_spec.freq, SDL_GetError());
			wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
			if (!wanted_spec.channels) {
				wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
				wanted_spec.channels = wanted_nb_channels;
				if (!wanted_spec.freq) {
					av_log(NULL, AV_LOG_ERROR,
						"No more combinations to try, audio open failed\n");
					SDL_UnlockMutex(audio_streams_mutex);
					return -1;
				}
			}
			wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
		}
		if (spec.format != AUDIOD_EVICE_FORMAT) {
			av_log(NULL, AV_LOG_ERROR,
				"SDL advised audio format %d is not supported!\n", spec.format);
			SDL_UnlockMutex(audio_streams_mutex);
			return -1;
		}
		if (spec.channels != wanted_spec.channels) {
			wanted_channel_layout = av_get_default_channel_layout(spec.channels);
			if (!wanted_channel_layout) {
				av_log(NULL, AV_LOG_ERROR,
					"SDL advised channel count %d is not supported!\n", spec.channels);
				SDL_UnlockMutex(audio_streams_mutex);
				return -1;
			}
		}
		audio_params.fmt = AV_SAMPLE_FMT_FLT;
		audio_params.freq = spec.freq;
		audio_params.channel_layout = wanted_channel_layout;
		audio_params.channels = spec.channels;
		audio_params.frame_size = av_samples_get_buffer_size(NULL, audio_params.channels, 1, audio_params.fmt, 1);
		audio_params.bytes_per_sec = av_samples_get_buffer_size(NULL, audio_params.channels, audio_params.freq, audio_params.fmt, 1);
		if (audio_params.bytes_per_sec <= 0 || audio_params.frame_size <= 0) {
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
			SDL_UnlockMutex(audio_streams_mutex);
			return -1;
		}
		is_init_audio = 1;
		audio_buf_size = spec.size;
	}
	for (int i = 0; i < MUTI_OPEN_NUM; i++)
	{
		if (open_audio_streams[i] == NULL)
		{
			open_audio_streams[i] = opaque;
			break;
		}
	}
	SDL_UnlockMutex(audio_streams_mutex);
	*audio_hw_params = audio_params;
	return audio_buf_size;
}
static enum AVPixelFormat GetHwFormat(AVCodecContext* s, const enum AVPixelFormat* pix_fmts)
{
	InputStream* ist = (InputStream*)s->opaque;
	ist->active_hwaccel_id = HWACCEL_DXVA2;
	ist->hwaccel_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
	return ist->hwaccel_pix_fmt;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState* is, int stream_index)
{

	//AVDictionary* codec_opts = NULL;
	AVFormatContext* ic = is->ic;
	AVCodecContext* avctx;
	AVCodec* codec;
	const char* forced_codec_name = NULL;
	AVDictionary* opts = NULL;
	AVDictionaryEntry* t = NULL;
	int sample_rate, nb_channels;
	int64_t channel_layout;
	int ret = 0;
	int stream_lowres = is->lowres;
	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;
	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
		return AVERROR(ENOMEM);

	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	if (ret < 0)
		goto fail;

	//av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);
	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	codec = avcodec_find_decoder(avctx->codec_id);
	avctx->pix_fmt = 0;
	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO: is->last_audio_stream = stream_index; forced_codec_name = is->audio_codec_name; break;
	case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = is->subtitle_codec_name; break;
	case AVMEDIA_TYPE_VIDEO: is->last_video_stream = stream_index; forced_codec_name = is->video_codec_name; break;
	}

	if (forced_codec_name)
	{

		codec = avcodec_find_decoder_by_name(forced_codec_name);
		if (!codec)
		{
			av_log(NULL, AV_LOG_WARNING,
				"No codec could be found with name '%s'.Setting a default codec.\n", forced_codec_name);

			codec = avcodec_find_decoder(avctx->codec_id);
		}
	}
	if (!codec) {
		if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
			"No codec could be found with name '%s'\n", forced_codec_name);
		else                   av_log(NULL, AV_LOG_WARNING,
			"No codec could be found with id %d\n", avctx->codec_id);

		ret = AVERROR(EINVAL);
		goto fail;
	}
	//avctx->codec_id = codec->id;
	if (stream_lowres > codec->max_lowres  /*av_codec_get_max_lowres(codec)*/) {
		av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
			codec->max_lowres/* av_codec_get_max_lowres(codec)*/);
		stream_lowres = codec->max_lowres /* av_codec_get_max_lowres(codec)*/;
	}
	avctx->lowres = stream_lowres;
	/*av_codec_set_lowres(avctx, stream_lowres);*/
#if FF_API_EMU_EDGE
	if (stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
	if (is->fast)
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
	if (codec->capabilities & AV_CODEC_CAP_DR1)
		avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
	//delete next line by xin
	//opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);

	//if (!av_dict_get(opts, "threads", NULL, 0))
	//	av_dict_set(&opts, "threads", "auto", 0);
	if (stream_lowres)
		av_dict_set_int(&opts, "lowres", stream_lowres, 0);
	//if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
	//	av_dict_set(&opts, "refcounted_frames", "1", 0);

	if (!forced_codec_name && avctx->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		if (is->hwaccel == AC_HARDWAREACCELERATETYPE_AUTO || is->hwaccel == AC_HARDWAREACCELERATETYPE_DXVA2)
		{
			switch (codec->id)
			{
			case AV_CODEC_ID_MPEG2VIDEO:
			case AV_CODEC_ID_H264:
			case AV_CODEC_ID_VC1:
			case AV_CODEC_ID_WMV3:
			case AV_CODEC_ID_HEVC:
			case AV_CODEC_ID_VP9:
				//while (1)
			{
				//const AVCodecHWConfig* config = avcodec_get_hw_config(codec, 0);
				avctx->thread_count = 1;  // Multithreading is apparently not compatible with hardware decoding
				is->ist = av_mallocz(sizeof(InputStream));
				is->ist->hwaccel_id = HWACCEL_AUTO;
				is->ist->active_hwaccel_id = HWACCEL_AUTO;
				is->ist->hwaccel_device = "dxva2";
				is->ist->dec = codec;
				is->ist->dec_ctx = avctx;
				avctx->opaque = is->ist;
				if (dxva2_init(avctx, is->hwnd) == 0)
				{
					avctx->get_buffer2 = is->ist->hwaccel_get_buffer;
					avctx->get_format = GetHwFormat;
					//avctx->thread_safe_callbacks = 1;
					avctx->pix_fmt = AV_PIX_FMT_DXVA2_VLD;
				}
				else
				{
					av_free(is->ist);
					is->ist = NULL;
				}
			}
			break;
			}
		}
	}

	avctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
		goto fail;
	}

	if (is->ist)
	{
		avctx->pix_fmt = AV_PIX_FMT_DXVA2_VLD;
	}
	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}
	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
	{
		AVFilterContext* sink;
		is->audio_filter_src.freq = avctx->sample_rate;
		is->audio_filter_src.channels = avctx->channels;
		is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
		is->audio_filter_src.fmt = avctx->sample_fmt;
		if ((ret = configure_audio_filters(is, is->afilters, 0)) < 0)
			goto fail;
		sink = is->out_audio_filter;
		sample_rate = av_buffersink_get_sample_rate(sink);
		nb_channels = av_buffersink_get_channels(sink);
		channel_layout = av_buffersink_get_channel_layout(sink);
	}
#else
		sample_rate = avctx->sample_rate;
		nb_channels = avctx->ch_layout.nb_channels;
		channel_layout = avctx->ch_layout.u.mask;
#endif
		is->soundTouch = cSoundTouch_create();
		cSoundTouch_setSampleRate(is->soundTouch, sample_rate);
		cSoundTouch_setChannels(is->soundTouch, nb_channels);
		is->sncStream = sonicCreateStream(sample_rate, nb_channels);
		/* prepare audio output */
		if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
			goto fail;
		is->audio_hw_buf_size = ret;
		is->audio_src = is->audio_tgt;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;
		/* init averaging filter */
		is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		is->audio_diff_avg_count = 0;
		/* since we do not have a precise anough audio FIFO fullness,
		we correct audio sync only if larger than this threshold */
		is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;
		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];
		decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
		if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) /*&& !is->ic->iformat->read_seek*/) {
			is->auddec.start_pts = is->audio_st->start_time;
			is->auddec.start_pts_tb = is->audio_st->time_base;
		}
		if ((ret = decoder_start(is, &is->auddec, audio_thread, is)) < 0)
			goto out;
		SDL_PauseAudioDevice(devId, 0);


		break;
	case AVMEDIA_TYPE_VIDEO:
		is->event_tid = SDL_CreateThread(event_loop, "event_loop", is);
		if (!is->event_tid) {
			av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread() 3: %s\n", SDL_GetError());
			ret = -1;
			goto fail;
		}
		is->video_stream = stream_index;
		is->video_st = ic->streams[stream_index];
		decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
		if ((ret = decoder_start(is, &is->viddec, video_thread, is)) < 0)
			goto out;
		is->queue_attachments_req = 1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_stream = stream_index;
		is->subtitle_st = ic->streams[stream_index];
		decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
		if ((ret = decoder_start(is, &is->subdec, subtitle_thread, is)) < 0)
			goto out;
		break;
	default:
		break;
	}
	goto out;
fail:
	avcodec_free_context(&avctx);
out:
	av_dict_free(&opts);
	return ret;
}

static int decode_interrupt_cb(void* ctx)
{
	VideoState* is = ctx;
	return is->abort_request;
}

static int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue) {
	return stream_id < 0 ||
		queue->abort_request ||
		(st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext* s)
{
	if (!strcmp(s->iformat->name, "rtp")
		|| !strcmp(s->iformat->name, "rtsp")
		|| !strcmp(s->iformat->name, "sdp")
		)
		return 1;
	if (s->pb && (!strncmp(s->url, "rtp:", 4)
		|| !strncmp(s->url, "udp:", 4)
		)
		)
		return 1;
	return 0;
}

enum {
	FLV_TAG_TYPE_AUDIO = 0x08,
	FLV_TAG_TYPE_VIDEO = 0x09,
	FLV_TAG_TYPE_META = 0x12,
};

//static AVStream* create_stream(AVFormatContext* s, int codec_type)
//{
//	AVStream* st = avformat_new_stream(s, NULL);
//	if (!st)
//		return NULL;
//	st->codec->codec_type = codec_type;
//	return st;
//}



/* this thread gets the stream from the disk or the network */
static int read_thread(void* arg)
{

	//AVFrame* picture = av_frame_alloc();
	//AVDictionary* format_opts = NULL;
	//AVDictionary* codec_opts = NULL;
	VideoState* is = arg;
	AVFormatContext* ic = NULL;
	int err, i, ret = 0;
	int st_index[AVMEDIA_TYPE_NB];
	AVPacket pkt1, * pkt = &pkt1;
	int64_t stream_start_time;
	int pkt_in_play_range = 0;
	/*AVDictionaryEntry* t;*/
	SDL_mutex* wait_mutex = SDL_CreateMutex();
	int scan_all_pmts_set = 0;
	int64_t pkt_ts;
	double seek_time = 0;
	double pkt_time;
	ACStopReason stopReason = AC_STOPREASON_ERROR;
	if (!wait_mutex) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	memset(st_index, -1, sizeof(st_index));
	is->last_video_stream = is->video_stream = -1;
	is->last_audio_stream = is->audio_stream = -1;
	is->last_subtitle_stream = is->subtitle_stream = -1;
	is->eof = 0;


	ic = avformat_alloc_context();
	if (!ic) {
		av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = is;
	if (!av_dict_get(is->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
		av_dict_set(&is->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scan_all_pmts_set = 1;
	}
	//av_dict_set(&is->format_opts, "decryption_key", "76a6c65c5ea762046bd749a2e632ccbb", AV_DICT_DONT_OVERWRITE);

	if (is->avio)
	{
		ic->pb = is->avio;
		ic->flags = AVFMT_FLAG_CUSTOM_IO;
	}

	err = avformat_open_input(&ic, is->filename, is->iformat, &is->format_opts);
	if (err < 0) {
#ifdef _WIN32  	 
		if (!is->abort_request)
		{
			avdevice_register_all();
			is->iformat = av_find_input_format("dshow");
			char camera[512];
			sprintf(camera, "video=%s", is->filename);
			av_dict_set(&is->format_opts, "pixel_format", "yuv420p", 0);
			ic = avformat_alloc_context();
			ic->interrupt_callback.callback = decode_interrupt_cb;
			ic->interrupt_callback.opaque = is;
			if (avformat_open_input(&ic, camera, is->iformat, &is->format_opts) < 0) {
				ic = avformat_alloc_context();
				ic->interrupt_callback.callback = decode_interrupt_cb;
				ic->interrupt_callback.opaque = is;
				if (avformat_open_input(&ic, camera, is->iformat, NULL) < 0) {
					av_log(NULL, AV_LOG_FATAL, "avformat_open_input(): %s\n", SDL_GetError());
					ret = -1;
					goto fail;
				}
			}
		}
		else
		{
			goto fail;
		}
#else
		ret = -1;
		av_log(NULL, AV_LOG_FATAL, "avformat_open_input(): %s\n", SDL_GetError());
		if (!is->abort_request)
			reason = AC_STOPREASON_ERROR;
		goto fail;
#endif  	
	}

	if (scan_all_pmts_set)
		av_dict_set(&is->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
	is->ic = ic;

	if (is->genpts)
		ic->flags |= AVFMT_FLAG_GENPTS;
	// av_format_inject_global_side_data(ic);
	if (is->find_stream_info) {
		int orig_nb_streams = ic->nb_streams;
		err = avformat_find_stream_info(ic, NULL);
		if (err < 0) {
			av_log(NULL, AV_LOG_WARNING,
				"%s: could not find codec parameters\n", is->filename);
			ret = -1;
			goto fail;
		}

	}

	av_dict_free(&is->format_opts);
	if (ic->pb)
		ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
	if (is->seek_by_bytes < 0)
		is->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
	is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
	/* if seeking requested, we execute it */
	if (is->start_time != 0/*AV_NOPTS_VALUE*/) {
		//int64_t timestamp;
		//timestamp = is->start_time;
		//is->start_time = 50000000;
		///* add the stream start time */
		//if (ic->start_time != AV_NOPTS_VALUE)
		//	timestamp += ic->start_time;
		//ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
		//if (ret < 0) {
		//	av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
		//		is->filename, (double)timestamp / AV_TIME_BASE);
		//}
		//
		is->seek_req = 1;
		is->seek_pos = is->start_time;
		is->start_time = 0;
	}
	is->realtime = is_realtime(ic);
	if (is->show_status)
		av_dump_format(ic, 0, is->filename, 0);

	for (i = 0; i < ic->nb_streams; i++) {
		AVStream* st = ic->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && is->wanted_stream_spec[type] && st_index[type] == -1)
			if (avformat_match_stream_specifier(ic, st, is->wanted_stream_spec[type]) > 0)
				st_index[type] = i;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
		if (is->wanted_stream_spec[i] && st_index[i] == -1) {
			av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", is->wanted_stream_spec[i], av_get_media_type_string(i));
			st_index[i] = INT_MAX;
		}
	}

	if (!is->video_disable)
		st_index[AVMEDIA_TYPE_VIDEO] =
		av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
			st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
	if (!is->audio_disable)
		st_index[AVMEDIA_TYPE_AUDIO] =
		av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
			st_index[AVMEDIA_TYPE_AUDIO],
			st_index[AVMEDIA_TYPE_VIDEO],
			NULL, 0);
	if (!is->video_disable && !is->subtitle_disable)
		st_index[AVMEDIA_TYPE_SUBTITLE] =
		av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
			st_index[AVMEDIA_TYPE_SUBTITLE],
			(st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
				st_index[AVMEDIA_TYPE_AUDIO] :
				st_index[AVMEDIA_TYPE_VIDEO]),
			NULL, 0);
	///* open the streams */
	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
	}

	//ret = -1;
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
	}

	if (is->show_mode == SHOW_MODE_NONE)
		is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

	if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
		stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
	}

	if (is->video_stream < 0 && is->audio_stream < 0) {
		av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
			is->filename);
		ret = -1;
		goto fail;
	}

	if (is->infinite_buffer < 0 && is->realtime)
		is->infinite_buffer = 1;

	ACPixelFormat format = AV_PIX_FMT_NONE;
	if (is->viddec.avctx)
	{
		if (is->viddec.avctx->codec->pix_fmts) {
			format = is->viddec.avctx->codec->pix_fmts[0];
		}
		else
			format = is->viddec.avctx->pix_fmt;
	}
	if (is->begin_callback)
	{
		int width = is->viddec.avctx ? is->viddec.avctx->width : 0;
		int height = is->viddec.avctx ? is->viddec.avctx->height : 0;
		double duration = is->ic->duration / (double)AV_TIME_BASE;
		is->begin_callback(is, &format, width, height, duration);
	}

	is->render_format = format;
	if (is->render_format == AV_PIX_FMT_NONE)
	{
		is->render_format = AV_PIX_FMT_YUV420P;
	}
	if (is->render_format == AV_PIX_FMT_DXVA2_VLD && is->viddec.avctx && is->viddec.avctx->pix_fmt != AV_PIX_FMT_DXVA2_VLD)
	{
		is->render_format = AV_PIX_FMT_YUV420P;
	}

	for (;;) {
		if (is->abort_request)
			break;
		if (is->paused != is->last_paused) {
			is->last_paused = is->paused;
			if (is->paused)
			{
				is->read_pause_return = av_read_pause(ic);
			}
			else
				av_read_play(ic);
		}
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
		if (is->paused &&
			(!strcmp(ic->iformat->name, "rtsp") ||
				(ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
			/* wait 10 ms to avoid trying to get another packet */
			/* XXX: horrible */
			SDL_Delay(10);
			continue;
		}
#endif

		if (is->seek_req) {
			int64_t seek_target = is->seek_pos;
			int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
			int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
			// FIXME the +-2 is due to rounding being not done in the correct direction in generation
			//      of the seek_pos/seek_rel variables
			/*ret = av_seek_frame(is->ic, -1, seek_target, AVSEEK_FLAG_BACKWARD); */
			ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
			//if(is->seek_req==2)
			seek_time = is->seek_pos / (double)AV_TIME_BASE;
			//is->seek_pos = 0;
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR,
					"%s: error while seeking\n", is->filename);
			}
			else {
				if (is->audio_stream >= 0) {
					packet_queue_flush(&is->audioq);
					packet_queue_put(is, &is->audioq, &is->flush_pkt);
				}
				if (is->subtitle_stream >= 0) {
					packet_queue_flush(&is->subtitleq);
					packet_queue_put(is, &is->subtitleq, &is->flush_pkt);
				}
				if (is->video_stream >= 0) {
					packet_queue_flush(&is->videoq);
					packet_queue_put(is, &is->videoq, &is->flush_pkt);
				}
				if (is->seek_flags & AVSEEK_FLAG_BYTE) {
					set_clock(&is->extclk, NAN, 0);
				}
				else {
					set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
				}
			}
			is->seek_req = 0;
			is->queue_attachments_req = 1;
			is->eof = 0;
			if (is->paused)
			{
				step_to_next_frame(is);
				if (is->pos_changed_callback != NULL)
				{
					is->pos_changed_callback(is, seek_time);
				}
			}
			if (is->pos_changed_callback != NULL)
			{
				is->pos_changed_callback(is, seek_time);
			}
		}

		if (is->queue_attachments_req) {
			if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
				AVPacket copy;

				if ((ret = /*av_copy_packet(&copy, &is->video_st->attached_pic)*/av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
					goto fail;
				packet_queue_put(is, &is->videoq, &copy);
				packet_queue_put_nullpacket(is, &is->videoq, is->video_stream);
			}
			is->queue_attachments_req = 0;
		}
		/* if the queue are full, no need to read more */
		if (is->infinite_buffer < 1 &&
			(is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
				|| (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
					stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
					stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
			/* wait 10 ms */
			SDL_LockMutex(wait_mutex);
			SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			SDL_UnlockMutex(wait_mutex);
			continue;
		}
		if (!is->paused &&
			(!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
			(!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
			if (is->loop/*loop != 1 && (!loop || --loop)*/) {
				stream_seek(is, is->start_time != AV_NOPTS_VALUE ? is->start_time : 0, 0, 0);
			}
			else if (is->autoexit) {

				if (is->pos_changed_callback)
				{
					is->pos_changed_callback(is, get_master_clock(is));
				}

				stopReason = AC_STOPREASON_REACHEND;

				ret = AVERROR_EOF;
				goto fail;
			}
		}
		ret = av_read_frame(ic, pkt);

		if (ret < 0) {
			if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
				if (is->video_stream >= 0)
					packet_queue_put_nullpacket(is, &is->videoq, is->video_stream);
				if (is->audio_stream >= 0)
					packet_queue_put_nullpacket(is, &is->audioq, is->audio_stream);
				if (is->subtitle_stream >= 0)
					packet_queue_put_nullpacket(is, &is->subtitleq, is->subtitle_stream);
				is->eof = 1;
			}
			if (ic->pb && ic->pb->error)
				break;
			SDL_LockMutex(wait_mutex);
			SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			SDL_UnlockMutex(wait_mutex);
			continue;
		}
		else {
			is->eof = 0;
		}
		pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

		//if (!is->isDisablePreciseSeek)
		//{
		//	if (seek_time > 0)
		//	{
		//		int isVReached = is->video_stream < 0;
		//		int isAReached = is->audio_stream < 0;
		//		pkt_time = pkt_ts * av_q2d(is->ic->streams[is->video_stream]->time_base);
		//		acf_array videoQueue;
		//		acf_array_init(&videoQueue, sizeof(AVPacket*));
		//		while (!isVReached&& !isAReached)
		//		{
		//			if (pkt->stream_index == is->video_stream)
		//			{
		//				while (1)
		//				{
		//					SDL_LockMutex(is->videoq.mutex);
		//					if (is->videoq.nb_packets < 1 && is->videoq.is_cond_waited)
		//					{
		//						SDL_UnlockMutex(is->videoq.mutex);
		//						break;
		//					}
		//					SDL_UnlockMutex(is->videoq.mutex);
		//					SDL_Delay(5);
		//				}		
		//				int got_picture = 0;
		//				if (!isVReached)
		//				{
		//					AVFrame* frame = av_frame_alloc();
		//					avcodec_send_packet(is->viddec.avctx, pkt);
		//					got_picture = avcodec_receive_frame(is->viddec.avctx, frame) == 0;
		//					if (got_picture)
		//					{
		//						pkt_time = (frame->pkt_pts == AV_NOPTS_VALUE ? frame->pkt_dts : frame->pkt_pts) * av_q2d(is->ic->streams[is->video_stream]->time_base);
		//						isVReached = fabs(seek_time - pkt_time < 0.04);
		//					}
		//					av_frame_unref(frame);
		//					av_packet_unref(pkt);
		//				}
		//				else
		//				{
		//					acf_array_add_ptr(&videoQueue, &pkt);
		//				}									
		//			}

		//			//while (is->audio_stream > -1)
		//			//{
		//			//	SDL_LockMutex(is->audioq.mutex);
		//			//	if (is->audioq.nb_packets < 1 && is->audioq.is_cond_waited)
		//			//	{
		//			//		SDL_UnlockMutex(is->audioq.mutex);
		//			//		break;
		//			//	}
		//			//	SDL_UnlockMutex(is->audioq.mutex);
		//			//	SDL_Delay(5);
		//			//}



		//			ret = av_read_frame(ic, pkt);



		//		}
		//	}
		//}


		//static int64_t del = 0;
		//printf("read cost %lf  \n", (av_gettime_relative() - del) / 1000000.0);
		//del = av_gettime_relative();

		//gop seek	
		if (!is->isDisablePreciseSeek)
		{


			if (seek_time > 0 && pkt->stream_index == is->video_stream && (pkt_time = pkt_ts * av_q2d(is->ic->streams[is->video_stream]->time_base)) < seek_time) {
				while (1)
				{
					SDL_LockMutex(is->videoq.mutex);
					if (is->videoq.nb_packets < 1 && is->videoq.is_cond_waited)
					{
						SDL_UnlockMutex(is->videoq.mutex);
						break;
					}
					SDL_UnlockMutex(is->videoq.mutex);
					SDL_Delay(10);
				}
				AVFrame* frame = av_frame_alloc();
				int got_picture = 0;
				avcodec_send_packet(is->viddec.avctx, pkt);
				avcodec_receive_frame(is->viddec.avctx, frame);
				av_packet_unref(pkt);
				av_frame_unref(frame);
				while (fabs(seek_time - pkt_time > 0.04) && pkt_time >= 0) {
					ret = av_read_frame(ic, pkt);
					if (ret >= 0)
					{
						if (pkt->stream_index == is->video_stream)
						{
							ret = avcodec_send_packet(is->viddec.avctx, pkt);
							if (ret == 0)
							{
								got_picture = avcodec_receive_frame(is->viddec.avctx, frame) == 0;
								if (got_picture)
								{
									pkt_time = (frame->pts == AV_NOPTS_VALUE ? frame->pkt_dts : frame->pts) * av_q2d(is->ic->streams[is->video_stream]->time_base);
									av_frame_unref(frame);
								}
							}
						}
						av_packet_unref(pkt);
					}
					else
					{
						if (is->pos_changed_callback != NULL)
						{
							is->pos_changed_callback(is, seek_time);
						}
						break;
					}
				}
				av_frame_free(&frame);
				seek_time = 0;
				continue;
			}
		}

		//gop seek -end

		/* check if packet is in play range specified by user, then queue, otherwise discard */
		//stream_start_time = ic->streams[pkt->stream_index]->start_time;
		stream_start_time = ic->streams[pkt->stream_index]->start_time;
		pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		pkt_in_play_range = is->duration == AV_NOPTS_VALUE ||
			(pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
			av_q2d(ic->streams[pkt->stream_index]->time_base) -
			(double)(is->start_time != AV_NOPTS_VALUE ? is->start_time : 0) / 1000000
			<= ((double)is->duration / 1000000);
		if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
			packet_queue_put(is, &is->audioq, pkt);
		}
		else if (pkt->stream_index == is->video_stream && pkt_in_play_range
			&& !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			packet_queue_put(is, &is->videoq, pkt);
		}
		else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
			packet_queue_put(is, &is->subtitleq, pkt);
		}
		else {
			av_packet_unref(pkt);
		}
	}
	ret = 0;
fail:

	if (ic && !is->ic)
	{
		avformat_close_input(&ic);
		//avformat_free_context(ic);
	}

	SDL_DestroyMutex(wait_mutex);

	if (is->end_callback)
	{
		if (is->abort_request)
			stopReason = AC_STOPREASON_USERCALL;
		is->end_callback(is, stopReason);
	}
	//
	///* close each stream */
	//if (is->audio_stream >= 0)
	//	stream_component_close(is, is->audio_stream);
	//if (is->video_stream >= 0)
	//	stream_component_close(is, is->video_stream);
	//if (is->subtitle_stream >= 0)
	//	stream_component_close(is, is->subtitle_stream);
	//if (is->event_tid)
	//{
	//	SDL_WaitThread(is->event_tid, NULL);
	//	is->event_tid = NULL;
	//}

	//exitThread(is);


	return 0;
}

//事件循环
static int  event_loop(void* lpParameter)
{

	double remaining_time = 0.0;
	VideoState* is = lpParameter;
	//enterThread(is);
	int i = 0;
	for (; ;) {
		if (is->abort_request)
		{
			//exitThread(is);
			break;
		}
		if (remaining_time > 0.0)
			av_usleep((int64_t)(remaining_time * 1000000.0));
		remaining_time = REFRESH_RATE;
		if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
			video_refresh(is, &remaining_time);
	}
	//exitThread(is);
	if (is->_d3dRender)
	{
		d3dRender_destory(is->_d3dRender);
		is->_d3dRender = NULL;
	}
	return 0;
}

static void stream_open(VideoState* is/*, const char* filename, AVInputFormat* iformat*/)
{
	//if (is->hwnd && invoke(is->hwnd, stream_open, is)) {
	//	return;
	//}
	//SDL_LockMutex(initial_mutex);



	if (!is->filename && !is->avio)
		goto fail;
	//if (!filename && !is->avio)
	//	goto fail;
	//if (filename)
	//	is->filename = av_strdup(filename);
	////is->iformat = iformat;
	is->iformat = NULL;
	is->ytop = 0;
	is->xleft = 0;
	//is->paused = 0;
	//is->stopCountMutex = SDL_CreateMutex();
	//if (is->speed != 1)
	//	ac_play_setSpeed(is, is->speed);
	/*if (is->speed != 1)
		is->req_afilter_reconfigure = 1;*/
		/* start video display */
	if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
		goto fail;
	if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
		goto fail;
	if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
		goto fail;
	if (packet_queue_init(&is->videoq) < 0 ||
		packet_queue_init(&is->audioq) < 0 ||
		packet_queue_init(&is->subtitleq) < 0)
		goto fail;
	if (!(is->continue_read_thread = SDL_CreateCond())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		goto fail;
	}

	init_clock(&is->vidclk, &is->videoq.serial);
	init_clock(&is->audclk, &is->audioq.serial);
	init_clock(&is->extclk, &is->extclk.serial);
	is->audio_clock_serial = -1;
	is->audclk.paused = is->vidclk.paused = is->extclk.paused = is->paused;
	set_clock_speed(&is->extclk, is->speed);
#if CONFIG_SDLWINDOW
	SDL_LockMutex(initial_mutex);
	//video_open(is);
	if (!is->display_disable) {
		if (!is->window) {
			if (is->hwnd)
			{
				is->window = SDL_CreateWindowFrom(is->hwnd);
			}
		}
	}
	SDL_UnlockMutex(initial_mutex);
#endif
	is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
	if (!is->read_tid) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread() 2: %s\n", SDL_GetError());
	fail:
		stream_close(is);
		return;
	}
	return;
}

static void stream_cycle_channel(VideoState* is, int codec_type)
{
	AVFormatContext* ic = is->ic;
	int start_index, stream_index;
	int old_index;
	AVStream* st;
	AVProgram* p = NULL;
	int nb_streams = is->ic->nb_streams;
	if (codec_type == AVMEDIA_TYPE_VIDEO) {
		start_index = is->last_video_stream;
		old_index = is->video_stream;
	}
	else if (codec_type == AVMEDIA_TYPE_AUDIO) {
		start_index = is->last_audio_stream;
		old_index = is->audio_stream;
	}
	else {
		start_index = is->last_subtitle_stream;
		old_index = is->subtitle_stream;
	}
	stream_index = start_index;
	if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
		p = av_find_program_from_stream(ic, NULL, is->video_stream);
		if (p) {
			nb_streams = p->nb_stream_indexes;
			for (start_index = 0; start_index < nb_streams; start_index++)
				if (p->stream_index[start_index] == stream_index)
					break;
			if (start_index == nb_streams)
				start_index = -1;
			stream_index = start_index;
		}
	}
	for (;;) {
		if (++stream_index >= nb_streams)
		{
			if (codec_type == AVMEDIA_TYPE_SUBTITLE)
			{
				stream_index = -1;
				is->last_subtitle_stream = -1;
				goto the_end;
			}
			if (start_index == -1)
				return;
			stream_index = 0;
		}
		if (stream_index == start_index)
			return;
		st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
		if (st->codecpar->codec_type == codec_type) {
			/* check that parameters are OK */
			switch (codec_type) {
			case AVMEDIA_TYPE_AUDIO:
				if (st->codecpar->sample_rate != 0 &&
					st->codecpar->ch_layout.nb_channels != 0)
					goto the_end;
				break;
			case AVMEDIA_TYPE_VIDEO:
			case AVMEDIA_TYPE_SUBTITLE:
				goto the_end;
			default:
				break;
			}
		}
	}
the_end:
	if (p && stream_index != -1)
		stream_index = p->stream_index[stream_index];
	av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
		av_get_media_type_string(codec_type),
		old_index,
		stream_index);
	stream_component_close(is, old_index);
	stream_component_open(is, stream_index);
}



static void init_global() {
	if (!is_init_global)
	{
		if (initial_mutex)
		{
			SDL_LockMutex(initial_mutex);
		}
		if (!is_init_global) {
			avformat_network_init();
			int	flags = SDL_INIT_EVERYTHING;//SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
			if (SDL_Init(flags)) {
				av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
				av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
				SDL_Quit();
				av_log(NULL, AV_LOG_QUIET, "%s", "");
				return NULL;
			}
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
			if (audio_streams_mutex == NULL)
			{
				audio_streams_mutex = SDL_CreateMutex();
			}
			memset(open_audio_streams, 0, sizeof(VideoState*) * MUTI_OPEN_NUM);
			is_init_global = 1;
		}
		if (initial_mutex)
		{
			SDL_UnlockMutex(initial_mutex);
		}
	}
}


#ifdef _WIN32
BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,  // handle to DLL module
	DWORD fdwReason,     // reason for calling function
	LPVOID lpvReserved)  // reserved
{
	// Perform actions based on the reason for calling.
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		// Initialize once for each new process.
		// Return FALSE to fail DLL load.
		initial_mutex = SDL_CreateMutex();
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;

	case DLL_PROCESS_DETACH:

		if (lpvReserved != NULL)
		{
			break; // do not do cleanup if process termination scenario
		}

		// Perform any necessary cleanup.
		break;
	}
	return TRUE;  // Successful DLL_PROCESS_ATTACH.
}
#endif


ACPlay ac_play_create() {


	init_global();

	VideoState* s = av_mallocz(sizeof(VideoState));
	set_default_param(s);
	return s;
}

void ac_play_destroy(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	ac_play_stop(play);
	if (s->video_codec_name)
	{
		av_free(s->video_codec_name);
	}
	if (s->audio_codec_name)
	{
		av_free(s->audio_codec_name);
	}
#if CONFIG_AVFILTER
	if (s->afilters)
	{
		av_free(s->afilters);
	}
#endif

	av_free(s);
}


void ac_play_setWindow(ACPlay play, void* hwnd)
{
	VideoState* s = (VideoState*)play;
	s->hwnd = hwnd;

}

void* ac_play_getWindow(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->hwnd;
}

void ac_play_setHardwareAccelerateType(ACPlay  play, ACHardwareAccelerateType value) {
	VideoState* s = (VideoState*)play;
	s->hwaccel = value;
}
ACHardwareAccelerateType ac_play_getHardwareAccelerateType(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return  s->hwaccel;
}
void ac_play_setVideoCodecName(ACPlay  play, const char* codec) {
	VideoState* s = (VideoState*)play;
	if (s->video_codec_name)
		av_free(s->video_codec_name);
	if (codec)
	{

		s->video_codec_name = av_strdup(codec);
	}
	else
	{
		s->video_codec_name = NULL;
	}
}

const char* ac_play_getVideoCodecName(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->video_codec_name;
}

void ac_play_setIsDumpFormat(ACPlay play, int isDump)
{
	VideoState* s = (VideoState*)play;
	s->show_status = isDump;
}

int ac_play_getIsDumpFormat(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->show_status;
}

void ac_play_setACodecName(ACPlay play, const char* codec)
{
	VideoState* s = (VideoState*)play;
	if (s->audio_codec_name)
		av_free(s->audio_codec_name);
	if (codec)
	{
		s->audio_codec_name = av_strdup(codec);
	}
	else
	{
		s->audio_codec_name = NULL;
	}
}
static void ac_play_startInternal(ACPlay play, const char* url, ACPlayCustomPacketReadCallback read, ACPlayCustomPacketStreamSeekCallback seek, const char* format_opts, double startTime)
{
	VideoState* s = (VideoState*)play;
	ac_play_stop(play);
	if (read)
		s->avio = avio_alloc_context((unsigned char*)av_malloc(1024 * 1024), 1024 * 1024, 0, s, read, NULL, seek);
	s->start_time = (int64_t)(startTime * AV_TIME_BASE);
	if (format_opts)
	{
		char* temp = av_strdup(format_opts);
		int i = 0;
		char* pKey = NULL;
		char* pValue = NULL;
		int state = 0;
		while (temp[i])
		{
			switch (state)
			{
			case 0:
				if (temp[i] == '-')
				{
					pKey = &temp[i + 1];
					state = 1;
				}
				break;
			case 1:
				if (temp[i] == ' ')
				{
					temp[i] = 0;
					state = 2;
				}
				break;
			case 2:
				if (temp[i] != ' ')
				{
					pValue = &temp[i];
					state = 3;
				}
				break;
			case 3:
				if (temp[i] == ' ')
				{
					temp[i] = 0;
					state = 0;
					av_dict_set(&s->format_opts, pKey, pValue, AV_DICT_DONT_OVERWRITE);
				}
				else if (temp[i + 1] == 0)
				{
					av_dict_set(&s->format_opts, pKey, pValue, AV_DICT_DONT_OVERWRITE);
				}

				break;
			default:
				break;
			}
			i++;
		}
		av_free(temp);
	}
	if (url)
		s->filename = av_strdup(url);
	stream_open(s/*, url, NULL*/);
}


void ac_play_start(ACPlay play, const char* url, double startTime)
{
	ac_play_startWithOptions(play, url, NULL, startTime);
}

void ac_play_startWithOptions(ACPlay play, const char* url, const char* format_opts, double startTime)
{
	ac_play_startInternal(play, url, NULL, NULL, format_opts, startTime);
}

void ac_play_startViaCustomStream(ACPlay play, ACPlayCustomPacketReadCallback read, ACPlayCustomPacketStreamSeekCallback seek, const char* format_opts, double startTime)
{
	ac_play_startInternal(play, "", read, seek, format_opts, startTime);
}

void ac_play_stop(ACPlay p)
{
	VideoState* s = (VideoState*)p;
	if (s->filename || s->avio)
	{

		stream_close((VideoState*)p);


	}
}



void ac_play_setIsLoop(ACPlay play, int value)
{
	VideoState* s = (VideoState*)play;
	s->loop = value;
}

int ac_play_getIsLoop(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->loop;
}

void ac_play_setIsPause(ACPlay play, int isPaused)
{
	VideoState* s = (VideoState*)play;
	if (s->paused != isPaused)
		toggle_pause(s);
}

int ac_play_getIsPause(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->paused;
}

void ac_play_setIsMute(ACPlay play, int isMuted)
{
	VideoState* s = (VideoState*)play;
	s->muted = isMuted;
}

int ac_play_getIsMute(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return s->muted;
}

void ac_play_setIsVideoDisabled(ACPlay play, int isDisable)
{
	VideoState* s = (VideoState*)play;
	s->video_disable = isDisable;
}

int ac_play_getIsVideoDisabled(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return	s->video_disable;
}

void ac_play_setIsAudioDisabled(ACPlay play, int isDisable)
{
	VideoState* s = (VideoState*)play;
	s->audio_disable = isDisable;
}

int ac_play_getIsAudioDisabled(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return	s->audio_disable;
}

void ac_play_setDisableSubtitle(ACPlay play, int isDisable)
{
	VideoState* s = (VideoState*)play;
	s->subtitle_disable = isDisable;
}

void ac_play_setIsPreciseSeekDisabled(ACPlay play, int isDisable)
{
	VideoState* s = (VideoState*)play;
	s->isDisablePreciseSeek = isDisable;
}

int ac_play_getIsPreciseSeekDisabled(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return	 s->isDisablePreciseSeek;

}

void ac_play_setClockSyncType(ACPlay play, ACClockSyncType sync)
{
	VideoState* s = (VideoState*)play;
	s->av_sync_type = sync;
}

ACClockSyncType ac_play_getClockSyncType(ACPlay play)
{
	VideoState* s = (VideoState*)play;
	return	 s->av_sync_type;
}

void ac_play_seek(ACPlay play, double time)
{
	VideoState* is = (VideoState*)play;
	stream_seek(is, (int64_t)(time * AV_TIME_BASE), 0, 0);
}


void ac_play_setVolume(ACPlay play, int value)
{
	VideoState* is = (VideoState*)play;
	if (value < 0)
		value = 0;
	//if (value > 100)
	//	value = 100;
	is->audio_volume = (int)(value * SDL_MIX_MAXVOLUME / 100.0);
	is->audioVolume100 = value;
}

int ac_play_getVolume(ACPlay play)
{
	VideoState* is = (VideoState*)play;
	return is->audioVolume100;
}

void ac_play_setSpeed(ACPlay play, double value)
{
	//if (value < 0.5 || value>2)
	//	return;
	if (value < 0)value = 0;
	VideoState* is = (VideoState*)play;
#if CONFIG_AVFILTER
	if (value < 0.5)
		value = 0.5;
	if (value > 2)
		value = 2;
	if (!is->afilters)
	{
		is->afilters = av_malloc(32);
	}
	sprintf(is->afilters, "atempo=%lf", is->speed);
	is->req_afilter_reconfigure = 1;
#endif
	set_clock_speed(&is->extclk, value);
	is->speed = value;
}

double ac_play_getSpeed(ACPlay play)
{
	VideoState* is = (VideoState*)play;
	return	is->speed;
}

double Np_GetDuration(ACPlay play)
{
	VideoState* is = (VideoState*)play;
	return  (double)is->duration / AV_TIME_BASE;
}

void ac_play_setUserData(ACPlay play, void* userdata)
{
	VideoState* is = (VideoState*)play;
	is->userdata = userdata;
}
void* ac_play_getUserData(ACPlay play) {
	VideoState* is = (VideoState*)play;
	return is->userdata;
}

void ac_play_setStartedCallback(ACPlay play, ACPlayStartedCallback value)
{
	VideoState* is = (VideoState*)play;
	is->begin_callback = value;
}

//AC_API void ac_play_setReachEndCallback(ACPlay play, ACPlayCallback value)
//{
//	VideoState* is = (VideoState*)play;
//	is->end_callback = value;
//}



void ac_play_setStoppingCallback(ACPlay play, ACPlayStoppingCallback value)
{
	VideoState* is = (VideoState*)play;
	is->end_callback = value;
}

void ac_play_setStoppedCallback(ACPlay play, ACPlayCallback value)
{
	VideoState* is = (VideoState*)play;
	is->stopped_callback = value;
}

void ac_play_setCursorTimeChangedCallback(ACPlay play, ACPlayCursorTimeChangedCallback value)
{
	VideoState* is = (VideoState*)play;
	is->pos_changed_callback = value;
}

void ac_play_setDisplayCallback(ACPlay play, ACPlayDisplayCallback value)
{
	VideoState* is = (VideoState*)play;
	is->render_callback = value;
}




static void  beginCallback(void* play, ACPixelFormat* format, int width, int height, double duration)
{
	*format = AC_PIXELFORMAT_YU12;
	SDL_Window* screen = ac_play_getUserData(play);
	SDL_Renderer* render = SDL_GetRenderer(screen);
	SDL_Texture* sdlTexture = SDL_CreateTexture(render, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_TARGET, width, height);
	SDL_SetWindowData(screen, "texture", sdlTexture);
}


static void renderCallback(void* play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format)
{
	//渲染到SDL Window中
	SDL_Window* screen = ac_play_getUserData(play);
	SDL_Renderer* render = SDL_GetRenderer(screen);
	SDL_Texture* sdlTexture = SDL_GetWindowData(screen, "texture");
	SDL_Rect sdlRect, sdlRect2;
	sdlRect2.x = 0;
	sdlRect2.y = 0;
	sdlRect2.w = width;
	sdlRect2.h = height;
	sdlRect.x = 0;
	sdlRect.y = 0;
	SDL_GetWindowSize(screen, &sdlRect.w, &sdlRect.h);
	SDL_UpdateYUVTexture(sdlTexture, &sdlRect2,
		data[0], linesize[0],
		data[1], linesize[1],
		data[2], linesize[2]);
	SDL_RenderCopy(render, sdlTexture, 0, &sdlRect);

	int n = SDL_RenderReadPixels(render, &sdlRect, SDL_PIXELFORMAT_IYUV, data[0], linesize[0]);

	SDL_RenderPresent(render);
}

/*测试程序*/

//冒烟测试
static int test0() {

	SDL_Window* screen;
	SDL_Renderer* sdlRenderer;
	SDL_Texture* sdlTexture;
	SDL_Rect sdlRect;
	SDL_mutex* sdlMutex;
	int screen_w = 640, screen_h = 360;
	ACPlay play = ac_play_create();
	screen = SDL_CreateWindow("acplayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		screen_w, screen_h,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!screen) {
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}
	sdlRenderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

	ac_play_setStartedCallback(play, beginCallback);
	ac_play_setDisplayCallback(play, renderCallback);
	ac_play_setUserData(play, screen);
	SDL_Event sdl_event;
	ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);
	for (int i = 0; i < 50; i++) {
		SDL_Delay(30);
		SDL_PollEvent(&sdl_event);
	}
	sdlTexture = SDL_GetWindowData(screen, "texture");
	ac_play_destroy(play);
	SDL_DestroyWindow(screen);
	SDL_DestroyRenderer(sdlRenderer);
	SDL_DestroyTexture(sdlTexture);
	//SDL_Quit();
	printf("test0 passed!\n");
	return 0;
}

//初始化
static int  test1() {

	ACPlay play = ac_play_create();
	ac_play_destroy(play);
	printf("test1 passed!\n");
	return 0;
}


//反初始化
BOOL isTest2stoped = FALSE;
static int test2Stoped(void* play)
{

	isTest2stoped = TRUE;
	return 0;
}
static int test2() {
	isTest2stoped = FALSE;
	ACPlay play = ac_play_create();
	ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);
	ac_play_setStoppedCallback(play, test2Stoped);
	ac_play_destroy(play);
	if (!isTest2stoped)
	{
		printf("test2 failed:Missing stoping callback line:%d\n", __LINE__);
		return -1;
	}
	printf("test2 passed!\n");
	return 0;
}

//开始播放
BOOL isTest3started = FALSE;
BOOL test3SetFormat1 = FALSE;
ACPixelFormat test3Format1 = AC_PIXELFORMAT_NONE;
ACPixelFormat test3FormatSetValue = AC_PIXELFORMAT_NONE;
ACPixelFormat test3Format2 = AC_PIXELFORMAT_NONE;
double test3Time = 0;
static void test3Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest3started = TRUE;
	test3Format1 = *format;
	if (test3SetFormat1)
		*format = test3FormatSetValue;
}

static void test3Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test3Format2 = format;
}

static void test3cursorChanged(ACPlay play, double time) {
	test3Time = time;
}

FILE* test3file;

int64_t test3fileSize = 0;
static int test3_avio_read(ACPlay play, uint8_t* buf, int bufsize)
{
	return fread(buf, 1, bufsize, test3file);
}


static int64_t test3_avio_seek(ACPlay play, int64_t offset, int whence)
{
	switch (whence)
	{
	case AVSEEK_SIZE:
		return test3fileSize;
		break;
	case SEEK_CUR:
		fseek(test3file, offset, whence);
		break;
	case SEEK_SET:
		fseek(test3file, offset, whence);
		break;
	case SEEK_END:
		fseek(test3file, offset, whence);
		break;
	default:
		break;
	}
	return  ftell(test3file);
}



static int test3() {

	ACPlay play = ac_play_create();

	ac_play_setStartedCallback(play, test3Started);
	ac_play_setDisplayCallback(play, test3Display);
	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3FormatSetValue = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (isTest3started)
			break;
	}
	if (!isTest3started)
	{
		printf("test3 failed:Missing started callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	isTest3started = FALSE;
	ac_play_start(play, "2321321334321", 0);
	for (int i = 0; i < 50; i++) {
		SDL_Delay(10);
		if (isTest3started)
			break;
	}
	if (isTest3started)
	{
		printf("test3 failed:Illegal started callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest3started = FALSE;
	ac_play_start(play, "XiaoMi USB 2.0 Webcam", 0);
	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (isTest3started)
			break;
	}
	if (!isTest3started)
	{
		printf("test3 failed:Missing started callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);

	//ACPlay play2 = ac_play_create();
	//ACPlay play3 = ac_play_create();
	////ac_play_setHardwareAccelerateType(play2, AC_HARDWAREACCELERATETYPE_DXVA2);
	////ac_play_setHardwareAccelerateType(play3, AC_HARDWAREACCELERATETYPE_DXVA2);

	////ac_play_setVideoCodecName(play3, "h264_qsv");
	//ac_play_setIsLoop(play2, TRUE);
	//ac_play_setIsLoop(play3, TRUE);


	//SDL_Window* screen;
	//screen = SDL_CreateWindow("acplay", 640, 360, 640, 360, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	//SDL_SysWMinfo wmInfo;
	//SDL_VERSION(&wmInfo.version);
	//SDL_GetWindowWMInfo(screen, &wmInfo);
	//HWND hwnd = wmInfo.info.win.window;
	//ac_play_setWindow(play2, hwnd);

	//SDL_Window* screen2;
	//screen2 = SDL_CreateWindow("acplay", 0, 0, 640, 360, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	//SDL_SysWMinfo wmInfo2;
	//SDL_VERSION(&wmInfo2.version);
	//SDL_GetWindowWMInfo(screen2, &wmInfo2);
	//HWND hwnd2 = wmInfo2.info.win.window;
	//ac_play_setWindow(play3, hwnd2);
	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);
	//SDL_Event sdl_event;
	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}
	//ac_play_stop(play2);
	//ac_play_stop(play3);


	//ac_play_setVideoCodecName(play2, "h264_qsv");
	//ac_play_setVideoCodecName(play3, "h264_qsv");
	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}





	//ac_play_setVideoCodecName(play2, "h264_qsv");
	//ac_play_setVideoCodecName(play3, "h264_qsv");
	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}


	//ac_play_setVideoCodecName(play2, "h264_cuvid");
	//ac_play_setVideoCodecName(play3, "h264_cuvid");
	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}


	//ac_play_setVideoCodecName(play2, "h264_cuvid");
	//ac_play_setVideoCodecName(play3, "h264_qsv");


	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}

	//ac_play_setVideoCodecName(play2, NULL);
	//ac_play_setVideoCodecName(play3, "h264_qsv");


	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}



	//ac_play_setVideoCodecName(play2, NULL);
	//ac_play_setVideoCodecName(play3, "h264_cuvid");


	//ac_play_start(play2, "D:\\FFmpeg\\test.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\test.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}




	//ac_play_setVideoCodecName(play2, "hevc_qsv");
	//ac_play_setVideoCodecName(play3, "hevc_qsv");


	//ac_play_start(play2, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	//ac_play_start(play3, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	//for (int i = 0; i < 100; i++) {
	//	SDL_Delay(30);
	//	SDL_PollEvent(&sdl_event);
	//}




	//ac_play_destroy(play2);
	//ac_play_destroy(play3);

	//SDL_DestroyWindow(screen);
	//SDL_DestroyWindow(screen2);



	play = ac_play_create();
	ac_play_setStartedCallback(play, test3Started);
	ac_play_setDisplayCallback(play, test3Display);

	int pixfmts[] = {
		AC_PIXELFORMAT_YU12 ,
		AC_PIXELFORMAT_YUY2 ,
		AC_PIXELFORMAT_RGB24 ,
		AC_PIXELFORMAT_NV12 ,
		AC_PIXELFORMAT_ARGB32 ,
		AC_PIXELFORMAT_BGRA32 ,
	};
	for (int i = 0; i < 6; i++)
	{
		int fmt = pixfmts[i];
		test3FormatSetValue = fmt;
		isTest3started = FALSE;
		test3SetFormat1 = TRUE;
		test3Format1 = AC_PIXELFORMAT_NONE;
		test3Format2 = AC_PIXELFORMAT_NONE;
		ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);
		for (int i = 0; i < 5000; i++) {
			SDL_Delay(10);
			if (test3Format2 == fmt)
				break;
		}
		if (test3Format2 != fmt)
		{
			printf("test3 failed:can not set format line:%d\n", __LINE__);
			ac_play_destroy(play);
			return -1;
		}
		ac_play_stop(play);
	}

	test3FormatSetValue = AC_PIXELFORMAT_NONE;
	isTest3started = FALSE;
	test3SetFormat1 = TRUE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}
	if (test3Format2 == AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed:invalid set format  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);





	test3FormatSetValue = AC_PIXELFORMAT_DXVA2_VLD;
	isTest3started = FALSE;
	test3SetFormat1 = TRUE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}
	if (test3Format2 == AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed:invalid set format  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	SDL_Window* screen3;
	screen3 = SDL_CreateWindow("acplay", 0, 0, 640, 360, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	SDL_SysWMinfo wmInfo3;
	SDL_VERSION(&wmInfo3.version);
	SDL_GetWindowWMInfo(screen3, &wmInfo3);
	HWND hwnd3 = wmInfo3.info.win.window;
	ac_play_setWindow(play, hwnd3);
	ac_play_startWithOptions(play, "D:\\FFmpeg\\cross_road.cenc.mp4", "-decryption_key 76a6c65c5ea762046bd749a2e632ccbb -vcodec libx264", 0);

	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}

	if (test3Format2 == AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed: can not set options  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_start(play, "D:\\FFmpeg\\cross_road.cenc.mp4", 0);

	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}

	if (test3Format2 != AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed: can not set options  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_startWithOptions(play, "D:\\FFmpeg\\cross_road.cenc.mp4", "", 0);
	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}

	if (test3Format2 != AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed: can not set options  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}




	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;

	test3file = fopen("D:\\FFmpeg\\hevc4k60fpscross_road.mp4", "rb+");
	fseek(test3file, 0, SEEK_END);//定位到文件的最后面
	test3fileSize = ftell(test3file);
	fseek(test3file, 0, SEEK_SET);
	ac_play_startViaCustomStream(play, test3_avio_read, test3_avio_seek, "", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Format2 != AC_PIXELFORMAT_NONE)
			break;
	}

	if (test3Format2 == AC_PIXELFORMAT_NONE)
	{
		printf("test3 failed: cant not play custom stream    line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	fclose(test3file);

	isTest3started = FALSE;
	test3SetFormat1 = FALSE;
	test3Format1 = AC_PIXELFORMAT_NONE;
	test3Format2 = AC_PIXELFORMAT_NONE;
	test3Time = 0;
	test3file = fopen("D:\\FFmpeg\\hevc4k60fpscross_road.mp4", "rb+");
	fseek(test3file, 0, SEEK_END);//定位到文件的最后面
	test3fileSize = ftell(test3file);
	fseek(test3file, 0, SEEK_SET);
	ac_play_setCursorTimeChangedCallback(play, test3cursorChanged);
	ac_play_startViaCustomStream(play, test3_avio_read, test3_avio_seek, "", 30);

	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test3Time > 30)
			break;
	}

	if (test3Time < 30)
	{
		printf("test3 failed:custom stream can not seek   line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_stop(play);
	fclose(test3file);




	SDL_DestroyWindow(screen3);


	ac_play_destroy(play);
	printf("test3 passed!\n");
	return 0;
}


//停止播放
BOOL isTest4stopping = FALSE;
BOOL isTest4stopped = FALSE;
ACStopReason test4StopReason;
static void test4Stopping(ACPlay play, ACStopReason stopReason)
{
	test4StopReason = stopReason;

	isTest4stopping = TRUE;
}

static void test4Stopped(void* play)
{

	isTest4stopped = TRUE;
}


static int test4() {
	isTest4stopping = FALSE;
	isTest4stopped = FALSE;
	ACPlay play = ac_play_create();
	ac_play_setStoppingCallback(play, test4Stopping);
	ac_play_setStoppedCallback(play, test4Stopped);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	SDL_Delay(200);
	ac_play_stop(play);
	if (!isTest4stopping)
	{
		printf("test4 failed: Missing stoping callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	if (test4StopReason != AC_STOPREASON_USERCALL)
	{
		printf("test4 failed: stop reason incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	if (!isTest4stopped)
	{
		printf("test4 failed: Missing stopped callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	isTest4stopping = FALSE;
	isTest4stopped = FALSE;
	test4StopReason = AC_STOPREASON_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	ac_play_start(play, "", 0);

	if (!isTest4stopped)
	{
		printf("test4 failed: Missing stopped callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest4stopping = FALSE;
	for (int i = 0; i < 50; i++) {
		SDL_Delay(10);
		if (isTest4stopping)
			break;
	}
	if (!isTest4stopping)
	{
		printf("test4 failed: Missing stoping callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	for (int i = 0; i < 50; i++) {
		SDL_Delay(10);
		if (test4StopReason == AC_STOPREASON_ERROR)
			break;
	}

	if (test4StopReason != AC_STOPREASON_ERROR)
	{
		printf("test4 failed: stop reason incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest4stopped = FALSE;
	ac_play_stop(play);
	if (!isTest4stopped)
	{
		printf("test4 failed: Missing stopped callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest4stopping = FALSE;
	isTest4stopped = FALSE;
	test4StopReason = AC_STOPREASON_NONE;
	ac_play_setIsLoop(play, 0);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 130);

	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (isTest4stopping)
			break;
	}
	if (!isTest4stopping)
	{
		printf("test4 failed: Missing stoping callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	if (test4StopReason != AC_STOPREASON_REACHEND)
	{
		printf("test4 failed: stop reason incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest4stopped = FALSE;
	ac_play_stop(play);
	if (!isTest4stopped)
	{
		printf("test4 failed: Missing stopped callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest4stopped = FALSE;
	ac_play_stop(play);
	if (isTest4stopped)
	{
		printf("test4 failed:Illegal stopped callback line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);
	printf("test4 passed!\n");
	return 0;
}



//窗口句柄
BOOL isTest5stopping = FALSE;
BOOL isTest5stopped = FALSE;
ACStopReason test5StopReason;
static void test5Stopping(ACPlay play, ACStopReason stopReason)
{
	test5StopReason = stopReason;

	isTest5stopping = TRUE;
}

static void test5Stopped(void* play)
{

	isTest5stopped = TRUE;
}


static int test5() {

	SDL_Window* screen;
	int screen_w = 640, screen_h = 360;
	ACPlay play = ac_play_create();
	VideoState* is = play;
	screen = SDL_CreateWindow("acplayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		screen_w, screen_h,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!screen) {
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(screen, &wmInfo);
	HWND hwnd = wmInfo.info.win.window;
	ac_play_setWindow(play, hwnd);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	SDL_Event sdl_event;

	for (int i = 0; i < 50; i++) {
		SDL_Delay(30);
		SDL_PollEvent(&sdl_event);
	}

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 50; i++) {
		SDL_Delay(30);
		SDL_PollEvent(&sdl_event);
	}
	if (ac_play_getWindow(play) != hwnd)
	{
		printf("test5 failed:hwnd han been changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_destroy(play);
	SDL_DestroyWindow(screen);
	printf("test5 passed!\n");
	return 0;
}



//硬件加速
BOOL isTest6started = FALSE;
BOOL test6SetFormat1 = FALSE;
ACPixelFormat test6Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test6Format2 = AC_PIXELFORMAT_NONE;
ACPixelFormat test6FormatSetValue = AC_PIXELFORMAT_NONE;


static void test6Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest6started = TRUE;
	test6Format = *format;
	if (test6SetFormat1)
		*format = test6FormatSetValue;
}

static void test6Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test6Format2 = format;
}
static int test6() {

	ACPlay play = ac_play_create();
	isTest6started = FALSE;
	test6SetFormat1 = FALSE;
	test6Format = AC_PIXELFORMAT_NONE;
	test6Format2 = AC_PIXELFORMAT_NONE;
	test6FormatSetValue = AC_PIXELFORMAT_NONE;

#ifdef _WIN32
	ac_play_setHardwareAccelerateType(play, AC_HARDWAREACCELERATETYPE_DXVA2);
	ac_play_setStartedCallback(play, test6Started);
	ac_play_setDisplayCallback(play, test6Display);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (test6Format == AC_PIXELFORMAT_DXVA2_VLD)
			break;
	}
	if (test6Format != AC_PIXELFORMAT_DXVA2_VLD)
	{
		printf("test6 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (test6Format2 == AC_PIXELFORMAT_DXVA2_VLD)
			break;
	}
	if (test6Format2 != AC_PIXELFORMAT_DXVA2_VLD)
	{
		printf("test6 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	if (ac_play_getHardwareAccelerateType(play) != AC_HARDWAREACCELERATETYPE_DXVA2)
	{
		printf("test6 failed:Hwaccel changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	test6Format = AC_PIXELFORMAT_NONE;
	test6Format2 = AC_PIXELFORMAT_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 5000; i++) {
		SDL_Delay(10);
		if (test6Format == AC_PIXELFORMAT_DXVA2_VLD)
			break;
	}
	if (test6Format != AC_PIXELFORMAT_DXVA2_VLD)
	{
		printf("test6 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (test6Format2 == AC_PIXELFORMAT_DXVA2_VLD)
			break;
	}
	if (test6Format2 != AC_PIXELFORMAT_DXVA2_VLD)
	{
		printf("test6 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);

	isTest6started = FALSE;
	test6SetFormat1 = TRUE;
	test6Format = AC_PIXELFORMAT_NONE;
	test6Format2 = AC_PIXELFORMAT_NONE;
	test6FormatSetValue = AC_PIXELFORMAT_NONE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (test6Format == AC_PIXELFORMAT_DXVA2_VLD)
			break;
	}
	if (test6Format != AC_PIXELFORMAT_DXVA2_VLD)
	{
		printf("test6 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}




#endif
	ac_play_destroy(play);
	printf("test6 passed!\n");
	return 0;
}


//设置视频解码器
BOOL isTest7started = FALSE;
ACPixelFormat test7Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test7Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest7stopping = FALSE;
BOOL isTest7stopped = FALSE;
ACStopReason test7StopReason = AC_STOPREASON_NONE;
static void test7Stopping(ACPlay play, ACStopReason stopReason)
{
	test7StopReason = stopReason;
	isTest7stopping = TRUE;
}

static void test7Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest7started = TRUE;
	test7Format = *format;
}

static void test7Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test7Format2 = format;
}

static int test7() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test7Started);
	ac_play_setDisplayCallback(play, test7Display);
	ac_play_setStoppingCallback(play, test7Stopping);
	ac_play_setVideoCodecName(play, "h264_qsv");
	ac_play_start(play, "D:\\FFmpeg\\test.mp4", 0);

	for (int i = 0; i < 500; i++) {
		SDL_Delay(10);
		if (test7Format == AC_PIXELFORMAT_NV12)
			break;
	}
	if (test7Format != AC_PIXELFORMAT_NV12)
	{
		printf("test7 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_stop(play);

	VideoState* s = (VideoState*)play;
	if (strcmp(ac_play_getVideoCodecName(play), "h264_qsv") != 0)
	{
		printf("test7 failed:video codec name changed:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	test7Format = AC_PIXELFORMAT_NONE;
	test7Format2 = AC_PIXELFORMAT_NONE;
	ac_play_setVideoCodecName(play, "hevc_qsv");
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 5000; i++) {
		SDL_Delay(10);
		if (test7Format == AC_PIXELFORMAT_NV12)
			break;
	}
	if (test7Format != AC_PIXELFORMAT_NV12)
	{
		printf("test7 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	for (int i = 0; i < 5000; i++) {
		SDL_Delay(10);
		if (test7Format2 == AC_PIXELFORMAT_NV12)
			break;
	}
	if (test7Format2 != AC_PIXELFORMAT_NV12)
	{
		printf("test7 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	test7Format = AC_PIXELFORMAT_NONE;
	test7Format2 = AC_PIXELFORMAT_NONE;
	ACStopReason test7StopReason = AC_STOPREASON_NONE;
	ac_play_setVideoCodecName(play, "hevc_qsv2");
	if (strcmp(ac_play_getVideoCodecName(play), "hevc_qsv2") != 0)
	{
		printf("test7 failed:video codec name incorrect:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test7Format2 != AC_PIXELFORMAT_NONE)
			break;
	}
	if (test7Format2 == AC_PIXELFORMAT_NONE)
	{
		printf("test7 failed:Missing a default codec line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);




	test7Format = AC_PIXELFORMAT_NONE;
	test7Format2 = AC_PIXELFORMAT_NONE;

	ac_play_setVideoCodecName(play, "h264_qsv");
	if (strcmp(ac_play_getVideoCodecName(play), "h264_qsv") != 0)
	{
		printf("test7 failed:video codec name incorrect:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test7Format2 != AC_PIXELFORMAT_NONE)
			break;
	}
	if (test7Format2 != AC_PIXELFORMAT_NONE)
	{
		printf("test7 failed:Missing a default codec line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);



	test7Format = AC_PIXELFORMAT_NONE;
	test7Format2 = AC_PIXELFORMAT_NONE;

	ac_play_setVideoCodecName(play, "h264_qsv");
	if (strcmp(ac_play_getVideoCodecName(play), "h264_qsv") != 0)
	{
		printf("test7 failed:video codec name incorrect:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test7Format2 != AC_PIXELFORMAT_NONE)
			break;
	}
	if (test7Format2 != AC_PIXELFORMAT_NONE)
	{
		printf("test7 failed:Wrong format line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	ac_play_destroy(play);
	printf("test7 passed!\n");
	return 0;
}






//循环播放
BOOL isTest8started = FALSE;
ACPixelFormat test8Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test8Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest8stopping = FALSE;
BOOL isTest8stopped = FALSE;
ACStopReason test8StopReason = AC_STOPREASON_NONE;
static void test8Stopping(ACPlay play, ACStopReason stopReason)
{
	test8StopReason = stopReason;
	isTest8stopping = TRUE;
}


static void test8Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest8started = TRUE;
	test8Format = *format;
}

static void test8Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test8Format2 = format;
}

static int test8() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test8Started);
	ac_play_setDisplayCallback(play, test8Display);
	ac_play_setStoppingCallback(play, test8Stopping);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 120);
	test8Format = AC_PIXELFORMAT_NONE;
	test8Format2 = AC_PIXELFORMAT_NONE;
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test8StopReason == AC_STOPREASON_REACHEND)
			break;
	}
	if (test8StopReason != AC_STOPREASON_REACHEND)
	{
		printf("test8 failed:Missing reach end line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);

	ac_play_setIsLoop(play, 1);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 120);
	test8Format = AC_PIXELFORMAT_NONE;
	test8Format2 = AC_PIXELFORMAT_NONE;
	test8StopReason = AC_STOPREASON_NONE;
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test8StopReason != AC_STOPREASON_NONE)
			break;
	}
	if (test8StopReason != AC_STOPREASON_NONE)
	{
		printf("test8 failed:abnormal stopping line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	if (!ac_play_getIsLoop(play))
	{
		printf("test8 failed:Loop changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 120);
	test8Format = AC_PIXELFORMAT_NONE;
	test8Format2 = AC_PIXELFORMAT_NONE;
	test8StopReason = AC_STOPREASON_NONE;
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (test8StopReason != AC_STOPREASON_NONE)
			break;
	}
	if (test8StopReason != AC_STOPREASON_NONE)
	{
		printf("test8 failed:abnormal stopping line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}





	ac_play_destroy(play);
	printf("test8 passed!\n");
	return 0;
}



//暂停
BOOL isTest9started = FALSE;
ACPixelFormat test9Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test9Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest9stopping = FALSE;
BOOL isTest9stopped = FALSE;
BOOL isTest9display = FALSE;
ACStopReason test9StopReason = AC_STOPREASON_NONE;
static void test9Stopping(ACPlay play, ACStopReason stopReason)
{
	test9StopReason = stopReason;
	isTest9stopping = TRUE;
}


static void test9Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest9started = TRUE;
	test9Format = *format;
}

static void test9Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test9Format2 = format;
	isTest9display = TRUE;
}

static int test9() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test9Started);
	ac_play_setDisplayCallback(play, test9Display);
	ac_play_setStoppingCallback(play, test9Stopping);
	isTest9display = FALSE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	ac_play_setIsPause(play, TRUE);

	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (isTest9display)
			break;
	}
	if (isTest9display)
	{
		printf("test9 failed:Error display when paused line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (!ac_play_getIsPause(play))
	{
		printf("test9 failed:Paused changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest9display = FALSE;
	ac_play_setIsPause(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (isTest9display)
			break;
	}
	if (!ac_play_getIsPause(play))
	{
		printf("test9 failed:Paused changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	if (isTest9display)
	{
		printf("test9 failed:Error paused  line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);
	printf("test9 passed!\n");
	return 0;
}





//静音
BOOL isTest10started = FALSE;
ACPixelFormat test10Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test10Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest10stopping = FALSE;
BOOL isTest10stopped = FALSE;
BOOL isTest10display = FALSE;
ACStopReason test10StopReason = AC_STOPREASON_NONE;
static void test10Stopping(ACPlay play, ACStopReason stopReason)
{
	test10StopReason = stopReason;
	isTest10stopping = TRUE;
}


static void test10Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest10started = TRUE;
	test10Format = *format;
}

static void test10Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test10Format2 = format;
	isTest10display = TRUE;
}

static int test10() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test10Started);
	ac_play_setDisplayCallback(play, test10Display);
	ac_play_setStoppingCallback(play, test10Stopping);
	isTest10display = FALSE;
	ac_play_setIsMute(play, TRUE);
	VideoState* is = (VideoState*)play;

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	if (!is->muted)
	{
		printf("test10 failed:mute value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);

	if (!ac_play_getIsMute(play))
	{
		printf("test10 failed:mute value incorrect after stop line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);
	printf("test10 passed!\n");
	return 0;
}


//禁用视频
BOOL isTest11started = FALSE;
ACPixelFormat test11Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test11Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest11stopping = FALSE;
BOOL isTest11stopped = FALSE;
BOOL isTest11display = FALSE;
ACStopReason test11StopReason = AC_STOPREASON_NONE;
static void test11Stopping(ACPlay play, ACStopReason stopReason)
{
	test11StopReason = stopReason;
	isTest11stopping = TRUE;
}


static void test11Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest11started = TRUE;
	test11Format = *format;
}

static void test11Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test11Format2 = format;
	isTest11display = TRUE;
}

static int test11() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test11Started);
	ac_play_setDisplayCallback(play, test11Display);
	ac_play_setStoppingCallback(play, test11Stopping);
	isTest11display = FALSE;

	VideoState* is = (VideoState*)play;
	ac_play_setIsVideoDisabled(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	if (!is->video_disable)
	{
		printf("test11 failed:video disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (isTest11display)
			break;
	}
	if (isTest11display)
	{
		printf("test11 failed:Still display when disabled line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);

	if (!ac_play_getIsVideoDisabled(play))
	{
		printf("test11 failed:video disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_setIsVideoDisabled(play, FALSE);

	if (ac_play_getIsVideoDisabled(play))
	{
		printf("test11 failed:video disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 300; i++) {
		SDL_Delay(10);
		if (isTest11display)
			break;
	}
	if (!isTest11display)
	{
		printf("test11 failed:Missing display when enabled line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);
	printf("test11 passed!\n");
	return 0;
}



//禁用音频
BOOL isTest12started = FALSE;
ACPixelFormat test12Format = AC_PIXELFORMAT_NONE;
ACPixelFormat test12Format2 = AC_PIXELFORMAT_NONE;
BOOL isTest12stopping = FALSE;
BOOL isTest12stopped = FALSE;
BOOL isTest12display = FALSE;
ACStopReason test12StopReason = AC_STOPREASON_NONE;
static void test12Stopping(ACPlay play, ACStopReason stopReason)
{
	test12StopReason = stopReason;
	isTest12stopping = TRUE;
}


static void test12Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest12started = TRUE;
	test12Format = *format;
}

static void test12Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {
	test12Format2 = format;
	isTest12display = TRUE;
}

static int test12() {

	ACPlay play = ac_play_create();
	ac_play_setStartedCallback(play, test12Started);
	ac_play_setDisplayCallback(play, test12Display);
	ac_play_setStoppingCallback(play, test12Stopping);
	isTest12display = FALSE;
	VideoState* is = (VideoState*)play;
	ac_play_setIsAudioDisabled(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	if (!ac_play_getIsAudioDisabled(play))
	{
		printf("test12 failed:audio disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (!ac_play_getIsAudioDisabled(play))
	{
		printf("test12 failed:audio disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setIsAudioDisabled(play, FALSE);
	if (ac_play_getIsAudioDisabled(play))
	{
		printf("test12 failed:audio disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);
	printf("test12 passed!\n");
	return 0;
}


//禁用精准定位

static int test13() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	ac_play_setIsPreciseSeekDisabled(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	if (!ac_play_getIsPreciseSeekDisabled(play))
	{
		printf("test13 failed:preciseSeek disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (!ac_play_getIsPreciseSeekDisabled(play))
	{
		printf("test13 failed:preciseSeek disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setIsPreciseSeekDisabled(play, FALSE);
	if (ac_play_getIsPreciseSeekDisabled(play))
	{
		printf("test13 failed:preciseSeek disable value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);
	printf("test13 passed!\n");
	return 0;
}


//时钟同步
static int test14() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	ac_play_setClockSyncType(play, AC_CLOCKSYNCTYPE_VIDEO);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	if (ac_play_getClockSyncType(play) != AC_CLOCKSYNCTYPE_VIDEO)
	{
		printf("test14 failed:clock sync type value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (ac_play_getClockSyncType(play) != AC_CLOCKSYNCTYPE_VIDEO)
	{
		printf("test14 failed:clock sync type value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setClockSyncType(play, AC_CLOCKSYNCTYPE_EXTERNAL);
	if (ac_play_getClockSyncType(play) != AC_CLOCKSYNCTYPE_EXTERNAL)
	{
		printf("test14 failed:clock sync type value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);
	printf("test14 passed!\n");
	return 0;
}


//定位
double test15Time = 0;

BOOL isTest15stopping = FALSE;
ACStopReason test15StopReason = AC_STOPREASON_NONE;
static void test15Stopping(ACPlay play, ACStopReason stopReason)
{
	test15StopReason = stopReason;
	isTest15stopping = TRUE;
}

static void test15cursorChanged(ACPlay play, double time) {
	test15Time = time;
}

static int test15() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest15stopping = FALSE;
	test15Time = 0;
	ac_play_setCursorTimeChangedCallback(play, test15cursorChanged);
	ac_play_setStoppingCallback(play, test15Stopping);
	ac_play_seek(play, 30);
	ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test15Time >= 30)
			break;
	}

	if (test15Time < 30)
	{
		printf("test15 failed:seek cursor time incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_seek(play, 60);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test15Time >= 60)
			break;
	}

	if (test15Time < 60)
	{
		printf("test15 failed:seek cursor time incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_seek(play, 0);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test15Time < 5)
			break;
	}

	if (test15Time > 10)
	{
		printf("test15 failed:seek cursor time incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_seek(play, 3600);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest15stopping)
			break;
	}

	if (test15StopReason != AC_STOPREASON_REACHEND)
	{
		printf("test15 failed:seek end incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_destroy(play);
	printf("test15 passed!\n");
	return 0;
}

//音量
static int test16() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	ac_play_setVolume(play, 2);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	if (ac_play_getVolume(play) != 2)
	{
		printf("test16 failed:audio_volume value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (ac_play_getVolume(play) != 2)
	{
		printf("test16 failed:audio_volume value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_setVolume(play, 1000);

	if (ac_play_getVolume(play) != 1000)
	{
		printf("test16 failed:audio_volume value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_setVolume(play, 0);

	if (ac_play_getVolume(play) != 0)
	{
		printf("test16 failed:audio_volume value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_setVolume(play, -2321);

	if (ac_play_getVolume(play) != 0)
	{
		printf("test16 failed:audio_volume value incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);
	printf("test16 passed!\n");
	return 0;
}


//播放速度
double test17Time = 0;

BOOL isTest17stopping = FALSE;
ACStopReason test17StopReason = AC_STOPREASON_NONE;
BOOL isTest17started = FALSE;
BOOL isTest17Display = FALSE;

static void test17Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest17started = TRUE;
}
static void test17Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest17Display = TRUE;
}

static void test17Stopping(ACPlay play, ACStopReason stopReason)
{
	test17StopReason = stopReason;
	isTest17stopping = TRUE;
}

static void test17cursorChanged(ACPlay play, double time) {
	test17Time = time;
}

static int test17() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest17stopping = FALSE;
	isTest17started = FALSE;
	isTest17Display = FALSE;
	test17Time = 0;
	ac_play_setStartedCallback(play, test17Started);
	ac_play_setDisplayCallback(play, test17Display);
	ac_play_setCursorTimeChangedCallback(play, test17cursorChanged);
	ac_play_setStoppingCallback(play, test17Stopping);
	ac_play_setSpeed(play, 2);
	ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest17Display)
			break;
	}

	SDL_Delay(1200);

	if (test17Time < 2)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_seek(play, 0);
	ac_play_setSpeed(play, 0.5);

	SDL_Delay(500);

	if (test17Time > 0.5)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (ac_play_getSpeed(play) != 0.5)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	test17Time = 0;
	isTest17started = FALSE;
	isTest17Display = FALSE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest17Display)
			break;
	}
	SDL_Delay(500);

	if (test17Time > 0.5)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_setSpeed(play, 0);

	if (ac_play_getSpeed(play) != 0)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setSpeed(play, 2312);
	if (ac_play_getSpeed(play) != 2312)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	//禁用视频倍速
	isTest17stopping = FALSE;
	isTest17started = FALSE;
	isTest17Display = FALSE;
	test17Time = 0;

	ac_play_setIsVideoDisabled(play, TRUE);
	ac_play_setSpeed(play, 2);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);


	//for (int i = 0; i < 200; i++) {
	//	SDL_Delay(10);
	//	if (isTest17Display)
	//		break;
	//}

	SDL_Delay(1300);

	if (test17Time < 2)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_seek(play, 0);
	ac_play_setSpeed(play, 0.5);



	SDL_Delay(1000);

	if (test17Time > 0.6)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	//禁用音频频倍速
	isTest17stopping = FALSE;
	isTest17started = FALSE;
	isTest17Display = FALSE;
	test17Time = 0;

	ac_play_setIsVideoDisabled(play, FALSE);
	ac_play_setIsAudioDisabled(play, TRUE);

	ac_play_setSpeed(play, 2);
	ac_play_start(play, "D:\\FFmpeg\\cross_road.mp4", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest17Display)
			break;
	}

	SDL_Delay(1000);

	if (test17Time < 2)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_seek(play, 0);
	ac_play_setSpeed(play, 0.5);

	SDL_Delay(500);

	if (test17Time > 0.5)
	{
		printf("test17 failed:speed incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);


	ac_play_destroy(play);
	printf("test17 passed!\n");
	return 0;
}

//用户数据
static int test18() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	ac_play_setUserData(play, play);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	if (ac_play_getUserData(play) != play)
	{
		printf("test18 failed:userdata incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	if (ac_play_getUserData(play) != play)
	{
		printf("test18 failed:userdata incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setUserData(play, NULL);
	if (ac_play_getUserData(play) != NULL)
	{
		printf("test18 failed:userdata incorrect line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);
	printf("test18 passed!\n");
	return 0;
}



//开始播放事件
double test19Time = 0;
BOOL isTest19stopping = FALSE;
ACStopReason test19StopReason = AC_STOPREASON_NONE;
BOOL isTest19started = FALSE;
BOOL isTest19Display = FALSE;

static void test19Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest19started = TRUE;
}
static void test19Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest19Display = TRUE;
}

static void test19Stopping(ACPlay play, ACStopReason stopReason)
{
	test19StopReason = stopReason;
	isTest19stopping = TRUE;
}

static void test19cursorChanged(ACPlay play, double time) {
	test19Time = time;
}

static int test19() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest19stopping = FALSE;
	isTest19started = FALSE;
	isTest19Display = FALSE;
	test19Time = 0;
	ac_play_setStartedCallback(play, test19Started);
	ac_play_setDisplayCallback(play, test19Display);
	ac_play_setCursorTimeChangedCallback(play, test19cursorChanged);
	ac_play_setStoppingCallback(play, test19Stopping);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest19started)
			break;
	}


	if (!isTest19started)
	{
		printf("test19 failed:Missing started call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);
	isTest19started = FALSE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest19started)
			break;
	}


	if (!isTest19started)
	{
		printf("test19 failed:Missing started call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);

	isTest19started = FALSE;
	ac_play_start(play, "", 0);

	for (int i = 0; i < 50; i++) {
		SDL_Delay(10);
		if (isTest19started)
			break;
	}


	if (isTest19started)
	{
		printf("test19 failed:Invalid started call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_setStartedCallback(play, NULL);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 50; i++) {
		SDL_Delay(10);
		if (isTest19started)
			break;
	}


	if (isTest19started)
	{
		printf("test19 failed:Invalid started call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_destroy(play);
	printf("test19 passed!\n");
	return 0;
}



//停止中事件
double test20Time = 0;

BOOL isTest20stopping = FALSE;
ACStopReason test20StopReason = AC_STOPREASON_NONE;
BOOL isTest20started = FALSE;
BOOL isTest20Display = FALSE;

static void test20Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest20started = TRUE;
}
static void test20Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest20Display = TRUE;
}

static void test20Stopping(ACPlay play, ACStopReason stopReason)
{
	test20StopReason = stopReason;
	isTest20stopping = TRUE;
}

static void test20cursorChanged(ACPlay play, double time) {
	test20Time = time;
}

static int test20() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest20stopping = FALSE;
	isTest20started = FALSE;
	isTest20Display = FALSE;
	test20StopReason = AC_STOPREASON_NONE;
	test20Time = 0;
	ac_play_setStartedCallback(play, test20Started);
	ac_play_setDisplayCallback(play, test20Display);
	ac_play_setCursorTimeChangedCallback(play, test20cursorChanged);
	ac_play_setStoppingCallback(play, test20Stopping);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	ac_play_stop(play);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test20StopReason == AC_STOPREASON_USERCALL)
			break;
	}

	if (test20StopReason != AC_STOPREASON_USERCALL)
	{
		printf("test20 failed:Wrong stop reason line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	test20StopReason = AC_STOPREASON_NONE;
	ac_play_start(play, "", 0);




	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test20StopReason == AC_STOPREASON_ERROR)
			break;
	}

	if (test20StopReason != AC_STOPREASON_ERROR)
	{
		printf("test20 failed:Wrong stop reason line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	test20StopReason = AC_STOPREASON_NONE;

	ac_play_stop(play);


	ac_play_setStoppingCallback(play, NULL);

	ac_play_start(play, "", 0);
	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (test20StopReason != AC_STOPREASON_NONE)
			break;
	}

	if (test20StopReason != AC_STOPREASON_NONE)
	{
		printf("test20 failed:Wrong stopping call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}



	ac_play_destroy(play);




	printf("test20 passed!\n");
	return 0;
}




//停止事件
double test21Time = 0;

BOOL isTest21stopping = FALSE;
BOOL isTest21stopped = FALSE;
ACStopReason test21StopReason = AC_STOPREASON_NONE;
BOOL isTest21started = FALSE;
BOOL isTest21Display = FALSE;

static void test21Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest21started = TRUE;
}
static void test21Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest21Display = TRUE;
}

static void test21Stopping(ACPlay play, ACStopReason stopReason)
{
	test21StopReason = stopReason;
	isTest21stopping = TRUE;
}

static void test21Stopped(ACPlay play)
{

	isTest21stopped = TRUE;
}



static void test21cursorChanged(ACPlay play, double time) {
	test21Time = time;
}

static int test21() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest21stopping = FALSE;
	isTest21started = FALSE;
	isTest21Display = FALSE;
	isTest21stopped = FALSE;
	test21StopReason = AC_STOPREASON_NONE;
	test21Time = 0;
	ac_play_setStartedCallback(play, test21Started);
	ac_play_setDisplayCallback(play, test21Display);
	ac_play_setCursorTimeChangedCallback(play, test21cursorChanged);
	ac_play_setStoppingCallback(play, test21Stopping);
	ac_play_setStoppedCallback(play, test21Stopped);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	ac_play_stop(play);


	if (!isTest21stopped)
	{
		printf("test21 failed:Missing stopped call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest21stopped = FALSE;
	ac_play_start(play, "", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest21stopped)
			break;
	}

	if (isTest21stopped)
	{
		printf("test21 failed:Wrong stopped call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);


	if (!isTest21stopped)
	{
		printf("test21 failed:Missing stopped call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	isTest21stopped = FALSE;
	ac_play_setStoppedCallback(play, NULL);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	ac_play_stop(play);

	if (isTest21stopped)
	{
		printf("test21 failed:Wrong stopped call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	isTest21stopped = FALSE;
	ac_play_setStoppedCallback(play, test21Stopped);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	ac_play_destroy(play);

	if (!isTest21stopped)
	{
		printf("test21 failed:Missing stopped call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	printf("test21 passed!\n");
	return 0;
}








//游标时间改变事件
double test22Time = 0;

BOOL isTest22stopping = FALSE;
BOOL isTest22stopped = FALSE;
ACStopReason test22StopReason = AC_STOPREASON_NONE;
BOOL isTest22started = FALSE;
BOOL isTest22Display = FALSE;

static void test22Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest22started = TRUE;
}
static void test22Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest22Display = TRUE;
}

static void test22Stopping(ACPlay play, ACStopReason stopReason)
{
	test22StopReason = stopReason;
	isTest22stopping = TRUE;
}

static void test22Stopped(ACPlay play)
{

	isTest22stopped = TRUE;
}



static void test22cursorChanged(ACPlay play, double time) {
	test22Time = time;
}

static int test22() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest22stopping = FALSE;
	isTest22started = FALSE;
	isTest22Display = FALSE;
	isTest22stopped = FALSE;
	test22StopReason = AC_STOPREASON_NONE;
	test22Time = 0;
	ac_play_setStartedCallback(play, test22Started);
	ac_play_setDisplayCallback(play, test22Display);
	ac_play_setCursorTimeChangedCallback(play, test22cursorChanged);
	ac_play_setStoppingCallback(play, test22Stopping);
	ac_play_setStoppedCallback(play, test22Stopped);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	SDL_Delay(500);
	ac_play_stop(play);



	if (test22Time == 0)
	{
		printf("test22 failed:Missing curor time changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	test22Time = 0;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	SDL_Delay(500);
	ac_play_stop(play);



	if (test22Time == 0)
	{
		printf("test22 failed:Missing curor time changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}




	test22Time = 0;
	ac_play_setCursorTimeChangedCallback(play, NULL);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	SDL_Delay(500);
	ac_play_stop(play);

	if (test22Time != 0)
	{
		printf("test22 failed:Wrong curor time changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_destroy(play);

	printf("test22 passed!\n");
	return 0;
}


//渲染事件
double test23Time = 0;
BOOL isTest23stopping = FALSE;
BOOL isTest23stopped = FALSE;
ACStopReason test23StopReason = AC_STOPREASON_NONE;
BOOL isTest23started = FALSE;
BOOL isTest23Display = FALSE;

static void test23Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest23started = TRUE;
}
static void test23Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest23Display = TRUE;
}

static void test23Stopping(ACPlay play, ACStopReason stopReason)
{
	test23StopReason = stopReason;
	isTest23stopping = TRUE;
}

static void test23Stopped(ACPlay play)
{

	isTest23stopped = TRUE;
}



static void test23cursorChanged(ACPlay play, double time) {
	test23Time = time;
}

static int test23() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest23stopping = FALSE;
	isTest23started = FALSE;
	isTest23Display = FALSE;
	isTest23stopped = FALSE;
	test23StopReason = AC_STOPREASON_NONE;
	test23Time = 0;
	ac_play_setStartedCallback(play, test23Started);
	ac_play_setDisplayCallback(play, test23Display);
	ac_play_setCursorTimeChangedCallback(play, test23cursorChanged);
	ac_play_setStoppingCallback(play, test23Stopping);
	ac_play_setStoppedCallback(play, test23Stopped);

	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest23Display)
			break;
	}

	if (!isTest23Display)
	{
		printf("test23 failed:Missing display call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);

	isTest23Display = FALSE;
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest23Display)
			break;
	}

	if (!isTest23Display)
	{
		printf("test23 failed:Missing display call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);



	isTest23Display = FALSE;
	ac_play_start(play, "", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest23Display)
			break;
	}

	if (isTest23Display)
	{
		printf("test23 failed:Wrong display call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);



	ac_play_setDisplayCallback(play, NULL);



	isTest23Display = FALSE;
	ac_play_start(play, "", 0);


	for (int i = 0; i < 200; i++) {
		SDL_Delay(10);
		if (isTest23Display)
			break;
	}

	if (isTest23Display)
	{
		printf("test23 failed:Wrong display call back line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}


	ac_play_destroy(play);


	printf("test23 passed!\n");
	return 0;
}




//打印格式
double test24Time = 0;
BOOL isTest24stopping = FALSE;
BOOL isTest24stopped = FALSE;
ACStopReason test24StopReason = AC_STOPREASON_NONE;
BOOL isTest24started = FALSE;
BOOL isTest24Display = FALSE;

static void test24Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest24started = TRUE;
}
static void test24Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest24Display = TRUE;
}

static void test24Stopping(ACPlay play, ACStopReason stopReason)
{
	test24StopReason = stopReason;
	isTest24stopping = TRUE;
}

static void test24Stopped(ACPlay play)
{

	isTest24stopped = TRUE;
}



static void test24cursorChanged(ACPlay play, double time) {
	test24Time = time;
}

static int test24() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	isTest24stopping = FALSE;
	isTest24started = FALSE;
	isTest24Display = FALSE;
	isTest24stopped = FALSE;
	test24StopReason = AC_STOPREASON_NONE;
	test24Time = 0;
	ac_play_setStartedCallback(play, test24Started);
	ac_play_setDisplayCallback(play, test24Display);
	ac_play_setCursorTimeChangedCallback(play, test24cursorChanged);
	ac_play_setStoppingCallback(play, test24Stopping);
	ac_play_setStoppedCallback(play, test24Stopped);
	ac_play_setIsDumpFormat(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	ac_play_stop(play);

	if (!ac_play_getIsDumpFormat(play))
	{
		printf("test24 failed:isDumpFormat changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_setIsDumpFormat(play, FALSE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);
	ac_play_stop(play);

	if (ac_play_getIsDumpFormat(play))
	{
		printf("test24 failed:isDumpFormat changed line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}
	ac_play_stop(play);
	ac_play_destroy(play);
	printf("test24 passed!\n");
	return 0;
}




//组合流程
double test25Time = 0;
BOOL isTest25stopping = FALSE;
BOOL isTest25stopped = FALSE;
ACStopReason test25StopReason = AC_STOPREASON_NONE;
BOOL isTest25started = FALSE;
BOOL isTest25Display = FALSE;

static void test25Started(ACPlay play, ACPixelFormat* format, int width, int height, double duration)
{
	isTest25started = TRUE;
}
static void test25Display(ACPlay play, unsigned char* data[8], int linesize[8], int width, int height, ACPixelFormat format, int* isHandled) {

	isTest25Display = TRUE;
}

static void test25Stopping(ACPlay play, ACStopReason stopReason)
{
	test25StopReason = stopReason;
	isTest25stopping = TRUE;
}

static void test25Stopped(ACPlay play)
{

	isTest25stopped = TRUE;
}



static void test25cursorChanged(ACPlay play, double time) {
	test25Time = time;
}

static int test25() {
	ACPlay play = ac_play_create();
	VideoState* is = (VideoState*)play;
	ac_play_setStartedCallback(play, test25Started);
	ac_play_setDisplayCallback(play, test25Display);
	ac_play_setCursorTimeChangedCallback(play, test25cursorChanged);
	ac_play_setStoppingCallback(play, test25Stopping);
	ac_play_setStoppedCallback(play, test25Stopped);

	//禁用视频和音频
	isTest25stopping = FALSE;
	isTest25started = FALSE;
	isTest25Display = FALSE;
	isTest25stopped = FALSE;
	test25StopReason = AC_STOPREASON_NONE;
	test25Time = 0;
	ac_play_setIsAudioDisabled(play, TRUE);
	ac_play_setIsVideoDisabled(play, TRUE);
	ac_play_start(play, "D:\\FFmpeg\\hevc4k60fpscross_road.mp4", 0);

	for (int i = 0; i < 200; i++)
	{
		SDL_Delay(50);
		if (isTest25stopping)
		{
			break;
		}
	}

	if (test25StopReason != AC_STOPREASON_ERROR)
	{
		printf("test25 failed:wrong stop reason line:%d\n", __LINE__);
		ac_play_destroy(play);
		return -1;
	}

	ac_play_stop(play);




	ac_play_destroy(play);
	printf("test25 passed!\n");
	return 0;
}




void test26Thread(void* s) {
	ACPlay* play = ac_play_create();
	ac_play_setIsVideoDisabled(play, 1);
	//ac_play_setIsAudioDisabled(play, 1);
	while (1)
	{
		ac_play_start(play, "D:\\test.mp4", 0);
		Sleep(1000);
		//ac_play_stop(play);
	}
}


int test26() {

	ACPlay* play = ac_play_create();
	ac_play_destroy(play);
	printf("test26 was running.Checking memory leak by youself.And stopping by youself in a right time!\n");
	for (int i = 0; i < 32; i++)
	{
		SDL_CreateThread(test26Thread, "", 0);
	}
	while (1)
	{
		Sleep(100);
	}
	printf("test26 passed!\n");
}





void ac_play_test()
{
	if (test0() != 0)return -1;
	if (test2() != 0)return -1;
	if (test3() != 0)return -1;
	if (test4() != 0)return -1;
	if (test5() != 0)return -1;
	if (test6() != 0)return -1;
	//if (test7() != 0)return -1; 
	if (test8() != 0)return -1;
	if (test9() != 0)return -1;
	if (test10() != 0)return -1;
	if (test11() != 0)return -1;
	if (test12() != 0)return -1;
	if (test13() != 0)return -1;
	if (test14() != 0)return -1;
	if (test15() != 0)return -1;
	if (test16() != 0)return -1;
	if (test17() != 0)return -1;
	if (test18() != 0)return -1;
	if (test19() != 0)return -1;
	if (test20() != 0)return -1;
	if (test21() != 0)return -1;
	if (test22() != 0)return -1;
	if (test23() != 0)return -1;
	if (test24() != 0)return -1;
	if (test25() != 0)return -1;
	//test26();
}
