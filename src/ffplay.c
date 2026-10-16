#include "acplay.h"
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "libavutil/mathematics.h"
#include "libavutil/dict.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "utils/array.h"
#ifdef _WIN32
#include "SDL.h"
#include "SDL_thread.h"
#include <windows.h>
#include "D3DRender.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
#endif
#include "cSoundTouch.h"

typedef struct VideoState VideoState;
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
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
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20
/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
static unsigned sws_flags = SWS_BICUBIC;

#define AUDIOD_EVICE_FORMAT AUDIO_F32SYS

typedef struct MyAVPacketList
{
	AVPacket pkt;
	struct MyAVPacketList *next;
	int serial;
} MyAVPacketList;

typedef struct PacketQueue
{
	MyAVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	int64_t duration;
	int abort_request;
	int serial;
	SDL_mutex *mutex;
	SDL_cond *cond;
	int is_cond_waited;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams
{
	int freq;
	int channels;
	int64_t channel_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
} AudioParams;

typedef struct Clock
{
	double pts;		  /* clock base */
	double pts_drift; /* clock base minus time at which we updated the clock */
	double last_updated;
	double speed;
	int serial; /* clock is based on a packet with this serial */
	int paused;
	int *queue_serial; /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame
{
	AVFrame *frame;
	AVSubtitle sub;
	int serial;
	double pts;		 /* presentation timestamp for the frame */
	double duration; /* estimated duration of the frame */
	int width;
	int height;
	int format;
} Frame;

typedef struct FrameQueue
{
	Frame queue[FRAME_QUEUE_SIZE];
	int rindex;
	int windex;
	int size;
	int max_size;
	int keep_last;
	int rindex_shown;
	SDL_mutex *mutex;
	SDL_cond *cond;
	PacketQueue *pktq;
} FrameQueue;

enum
{
	AV_SYNC_AUDIO_MASTER, /* default choice */
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder
{
	AVPacket pkt;
	PacketQueue *queue;
	AVCodecContext *avctx;
	int pkt_serial;
	int finished;
	int packet_pending;
	SDL_cond *empty_queue_cond;
	int64_t start_pts;
	AVRational start_pts_tb;
	int64_t next_pts;
	AVRational next_pts_tb;
	SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoScale
{
	int align;
	int width;
	int height;
	enum AVPixelFormat format;
	struct SwsContext *ctx;
	AVFrame *frame;
} VideoScale;

typedef struct VideoState
{
	SDL_Thread *read_tid;
	AVInputFormat *iformat;
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
	AVFormatContext *ic;
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
	AVStream *audio_st;
	PacketQueue audioq;
	int audio_hw_buf_size;
	uint8_t *audio_buf;
	uint8_t *audio_buf1;
	unsigned int audio_buf_size; /* in bytes */
	unsigned int audio_buf1_size;
	int audio_buf_index; /* in bytes */
	int audio_write_buf_size;
	int audio_volume;
	int muted;
	struct AudioParams audio_src;
	struct AudioParams audio_tgt;
	struct SwrContext *swr_ctx;
	enum ShowMode
	{
		SHOW_MODE_NONE = -1,
		SHOW_MODE_VIDEO = 0,
		SHOW_MODE_WAVES,
		SHOW_MODE_RDFT,
		SHOW_MODE_NB
	} show_mode;
	double last_vis_time;
	int subtitle_stream;
	AVStream *subtitle_st;
	PacketQueue subtitleq;
	double frame_timer;
	double frame_last_filter_delay;
	int video_stream;
	AVStream *video_st;
	PacketQueue videoq;
	double max_frame_duration;
	int eof;
	char *filename;
	int step;
	int last_video_stream, last_audio_stream, last_subtitle_stream;
	SDL_cond *continue_read_thread;
	void *hwnd;
	int audio_disable;
	int video_disable;
	int subtitle_disable;
	const char *wanted_stream_spec[AVMEDIA_TYPE_NB];
	int seek_by_bytes;
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
	const char *audio_codec_name;
	const char *subtitle_codec_name;
	const char *video_codec_name;
	double rdftspeed;
	int find_stream_info;
	/* current context */
	int64_t audio_callback_time;
	AVPacket flush_pkt;
	SDL_Thread *event_tid;
	void *userdata;
	int audio_callback_index;
	enum AVPixelFormat render_format;
	ACPlayDisplayCallback render_callback;
	ACPlayStartedCallback begin_callback;
	ACPlayStoppingCallback end_callback;
	ACPlayCallback stopped_callback;
	ACPlayCursorTimeChangedCallback pos_changed_callback;
	double speed;

	ACHardwareAccelerateType hwaccel;
	int isDisablePreciseSeek;
	int audioVolume100;
	AVDictionary *format_opts;
	AVIOContext *avio;
	char *speed_buf;
	int speed_buf_size;
	cSoundTouch soundTouch;

	VideoScale *video_scales;
	enum AVPixelFormat d3d_render_format;

#ifdef _WIN32
	D3DRender _d3dRender;
#endif

} VideoState;
SDL_AudioDeviceID devId;

#define MUTI_OPEN_NUM 64
static SDL_mutex *audio_streams_mutex = NULL;
static SDL_mutex *initial_mutex = NULL;
static VideoState *open_audio_streams[MUTI_OPEN_NUM];
static struct AudioParams audio_params;
static int audio_buf_size = 0;
static int is_init_audio = 0;
static int event_loop(void *lpParameter);
static double get_master_clock(VideoState *is);

static inline uint64_t av_get_default_channel_layout(int nb_channels)
{
	AVChannelLayout layout = {0};
	av_channel_layout_default(&layout, nb_channels);
	uint64_t mask = layout.u.mask;
	av_channel_layout_uninit(&layout);
	return mask;
}

static inline int av_get_channel_layout_nb_channels(uint64_t mask)
{
	AVChannelLayout layout = {0};
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
	AVChannelLayout out_layout = {0};
	AVChannelLayout in_layout = {0};
	av_channel_layout_from_mask(&out_layout, (uint64_t)out_ch_layout);
	av_channel_layout_from_mask(&in_layout, (uint64_t)in_ch_layout);

	struct SwrContext *ctx = s;
	int ret = swr_alloc_set_opts2(&ctx,
								  &out_layout, out_sample_fmt, out_sample_rate,
								  &in_layout, in_sample_fmt, in_sample_rate,
								  log_offset, log_ctx);

	av_channel_layout_uninit(&out_layout);
	av_channel_layout_uninit(&in_layout);

	if (ret < 0)
	{
		if (!s)
			swr_free(&ctx);
		return NULL;
	}
	return ctx;
}

static int packet_queue_put_private(VideoState *is, PacketQueue *q, AVPacket *pkt)
{
	MyAVPacketList *pkt1;
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
	SDL_CondSignal(q->cond);
	return 0;
}

static int packet_queue_put(VideoState *is, PacketQueue *q, AVPacket *pkt)
{
	int ret;
	SDL_LockMutex(q->mutex);
	ret = packet_queue_put_private(is, q, pkt);
	SDL_UnlockMutex(q->mutex);
	if (pkt != &is->flush_pkt && ret < 0)
		av_packet_unref(pkt);
	return ret;
}

static int packet_queue_put_nullpacket(VideoState *is, PacketQueue *q, int stream_index)
{
	AVPacket pkt1, *pkt = &pkt1;
	av_init_packet(pkt);
	pkt->data = NULL;
	pkt->size = 0;
	pkt->stream_index = stream_index;
	return packet_queue_put(is, q, pkt);
}

static int packet_queue_init(PacketQueue *q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->cond = SDL_CreateCond();
	if (!q->cond)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->abort_request = 1;
	return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
	MyAVPacketList *pkt, *pkt1;
	SDL_LockMutex(q->mutex);
	for (pkt = q->first_pkt; pkt; pkt = pkt1)
	{
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

static void packet_queue_destroy(PacketQueue *q)
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

static void packet_queue_abort(PacketQueue *q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 1;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(VideoState *is, PacketQueue *q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 0;
	packet_queue_put_private(is, q, &is->flush_pkt);
	SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
	MyAVPacketList *pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	for (;;)
	{
		if (q->abort_request)
		{
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if (pkt1)
		{
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
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			q->is_cond_waited = 1;
			SDL_CondWait(q->cond, q->mutex);
			q->is_cond_waited = 0;
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond)
{
	memset(d, 0, sizeof(Decoder));
	d->avctx = avctx;
	d->queue = queue;
	d->empty_queue_cond = empty_queue_cond;
	d->start_pts = AV_NOPTS_VALUE;
	d->pkt_serial = -1;
}

static int decoder_decode_frame(VideoState *is, Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
	int ret = AVERROR(EAGAIN);
	for (;;)
	{
		AVPacket pkt;
		if (d->queue->serial == d->pkt_serial)
		{
			do
			{
				if (d->queue->abort_request)
					return -1;
				switch (d->avctx->codec_type)
				{
				case AVMEDIA_TYPE_VIDEO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0)
					{
						if (is->decoder_reorder_pts == -1)
						{
							frame->pts = frame->best_effort_timestamp;
						}
						else if (!is->decoder_reorder_pts)
						{
							frame->pts = frame->pkt_dts;
						}
					}
					break;
				case AVMEDIA_TYPE_AUDIO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0)
					{
						AVRational tb = (AVRational){1, frame->sample_rate};
						if (frame->pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
						else if (d->next_pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);

						if (frame->pts != AV_NOPTS_VALUE)
						{
							d->next_pts = frame->pts + frame->nb_samples;
							d->next_pts_tb = tb;
						}
					}
					break;
				}
				if (ret == AVERROR_EOF)
				{
					d->finished = d->pkt_serial;
					avcodec_flush_buffers(d->avctx);
					return 0;
				}
				if (ret >= 0)
					return 1;
			} while (ret != AVERROR(EAGAIN));
		}

		do
		{
			if (d->queue->nb_packets == 0)
				SDL_CondSignal(d->empty_queue_cond);
			if (d->packet_pending)
			{
				av_packet_move_ref(&pkt, &d->pkt);
				d->packet_pending = 0;
			}
			else
			{
				if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
					return -1;
			}
		} while (d->queue->serial != d->pkt_serial);

		if (pkt.data == is->flush_pkt.data)
		{
			avcodec_flush_buffers(d->avctx);
			d->finished = 0;
			d->next_pts = d->start_pts;
			d->next_pts_tb = d->start_pts_tb;
		}
		else
		{
			if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
			{
				int got_frame = 0;
				ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
				if (ret < 0)
				{
					ret = AVERROR(EAGAIN);
				}
				else
				{
					if (got_frame && !pkt.data)
					{
						d->packet_pending = 1;
						av_packet_move_ref(&d->pkt, &pkt);
					}
					ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
				}
			}
			else
			{
				if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN))
				{
					av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
					d->packet_pending = 1;
					av_packet_move_ref(&d->pkt, &pkt);
				}
			}
			av_packet_unref(&pkt);
		}
	}
}

static void decoder_destroy(Decoder *d)
{
	av_packet_unref(&d->pkt);
	avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
	av_frame_unref(vp->frame);
	avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
	int i;
	memset(f, 0, sizeof(FrameQueue));
	if (!(f->mutex = SDL_CreateMutex()))
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	if (!(f->cond = SDL_CreateCond()))
	{
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

static void frame_queue_destory(FrameQueue *f)
{
	if (f->mutex)
	{
		int i;
		for (i = 0; i < f->max_size; i++)
		{
			Frame *vp = &f->queue[i];
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

static void frame_queue_signal(FrameQueue *f)
{
	SDL_LockMutex(f->mutex);
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
	return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
	return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
	SDL_LockMutex(f->mutex);
	while (f->size >= f->max_size &&
		   !f->pktq->abort_request)
	{
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
	SDL_LockMutex(f->mutex);
	while (f->size - f->rindex_shown <= 0 &&
		   !f->pktq->abort_request)
	{
		SDL_CondWait(f->cond, f->mutex);
	}
	SDL_UnlockMutex(f->mutex);

	if (f->pktq->abort_request)
		return NULL;

	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
	if (++f->windex == f->max_size)
		f->windex = 0;
	SDL_LockMutex(f->mutex);
	f->size++;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
	if (f->keep_last && !f->rindex_shown)
	{
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

static int frame_queue_nb_remaining(FrameQueue *f)
{
	return f->size - f->rindex_shown;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
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

static void stream_component_close(VideoState *is, int stream_index)
{
	AVFormatContext *ic = is->ic;
	AVCodecParameters *codecpar;
	if (!is->ic)
		return;
	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return;
	codecpar = ic->streams[stream_index]->codecpar;
	switch (codecpar->codec_type)
	{
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
	switch (codecpar->codec_type)
	{
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

static void set_default_param(VideoState *s)
{
	s->hwnd = 0;
	s->audio_disable = 0;
	s->video_disable = 0;
	s->subtitle_disable = 0;
	s->wanted_stream_spec[0] = 0;
	s->seek_by_bytes = -1;
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
	s->loop = 0;
	s->framedrop = -1;
	s->infinite_buffer = -1;
	s->show_mode = SHOW_MODE_NONE;
	s->audio_codec_name = 0;
	s->subtitle_codec_name = 0;
	s->video_codec_name = 0;
	s->rdftspeed = 0.02;
	s->find_stream_info = 1;
	s->audio_callback_time = 0;
	av_init_packet(&s->flush_pkt);
	s->flush_pkt.data = (uint8_t *)&s->flush_pkt;
	s->speed = 1;
}

static void stream_close(VideoState *is)
{
	is->abort_request = 1;
	if (is->read_tid)
	{
		SDL_WaitThread(is->read_tid, NULL);
		is->read_tid = NULL;
	}

	if (is->event_tid)
	{
		SDL_WaitThread(is->event_tid, NULL);
		is->event_tid = NULL;
	}

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
	frame_queue_destory(&is->pictq);
	frame_queue_destory(&is->sampq);
	frame_queue_destory(&is->subpq);

	if (is->continue_read_thread)
	{
		SDL_DestroyCond(is->continue_read_thread);
		is->continue_read_thread = NULL;
	}
	if (is->filename)
	{
		av_free(is->filename);
		is->filename = NULL;
	}

	av_dict_free(&is->format_opts);

	if (is->speed_buf)
	{
		av_free(is->speed_buf);
		is->speed_buf = NULL;
	}
	if (is->soundTouch)
	{
		cSoundTouch_destroy(is->soundTouch);
		is->soundTouch = NULL;
	}
	if (is->video_scales)
	{
		for (int i = 0; i < array_len(is->video_scales); i++)
		{
			av_frame_free(&is->video_scales[i].frame);
			if (is->video_scales[i].ctx)
				sws_freeContext(is->video_scales[i].ctx);
		}
		array_unmake(is->video_scales);
	}

	is->abort_request = 0;
	void *hwnd = is->hwnd;
	int loop = is->loop;
	int hwaccel = is->hwaccel;
	int paused = is->paused;
	int muted = is->muted;
	int audio_volume = is->audio_volume;
	int audioVolume100 = is->audioVolume100;
	double speed = is->speed;
	void *userdata = is->userdata;
	void *begin_callback = is->begin_callback;
	void *end_callback = is->end_callback;
	void *stopped_callback = is->stopped_callback;
	void *pos_changed_callback = is->pos_changed_callback;
	void *render_callback = is->render_callback;
	const char *video_codec_name = is->video_codec_name;
	const char *audio_codec_name = is->audio_codec_name;
	int video_disable = is->video_disable;
	int audio_disable = is->audio_disable;
	int isDisablePreciseSeek = is->isDisablePreciseSeek;
	int av_sync_type = is->av_sync_type;
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
	is->video_disable = video_disable;
	is->audio_disable = audio_disable;
	is->isDisablePreciseSeek = isDisablePreciseSeek;
	is->av_sync_type = av_sync_type;
	is->show_status = show_status;
	if (is->stopped_callback)
	{
		is->stopped_callback(is);
	}
}

static AVFrame *video_frame_scale(VideoState *is, Frame *vp, int width, int height, enum AVPixelFormat format, int align)
{
	VideoScale *scale = NULL;
	if (!is->video_scales)
	{
		is->video_scales = array_make(VideoScale, 0, 4);
	}
	for (int i = 0; i < array_len(is->video_scales); i++)
	{
		if (is->video_scales[i].format == format && is->video_scales[i].width == width && is->video_scales[i].height == height && is->video_scales[i].align == align)
		{
			scale = &is->video_scales[i];
			break;
		}
	}
	if (!scale)
	{
		VideoScale t = {0};
		array_append(is->video_scales, t);
		scale = &is->video_scales[array_len(is->video_scales) - 1];
		memset(scale, 0, sizeof(VideoScale));
		scale->format = format;
		scale->width = width;
		scale->height = height;
		scale->align = align;
		scale->ctx = sws_getCachedContext(scale->ctx,
										  vp->frame->width, vp->frame->height, vp->frame->format, width, height,
										  format, sws_flags, NULL, NULL, NULL);
		if (!scale->ctx)
		{
			av_log(NULL, AV_LOG_ERROR, "sws_getCachedContext fail\n");
			return NULL;
		}
		scale->frame = av_frame_alloc();
		if (!scale->frame)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to allocate video frame\n");
			return NULL;
		}
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

static void video_display(VideoState *is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER || is->auddec.avctx == NULL)
	{
		if (is->pos_changed_callback != NULL)
		{
			is->pos_changed_callback(is, get_master_clock(is));
		}
	}

	Frame *vp;
	vp = frame_queue_peek_last(&is->pictq);
	if (is->render_callback)
	{
		int isHandled = 0;
		if (is->render_format != vp->frame->format)
		{
			AVFrame *frame = video_frame_scale(is, vp, vp->width, vp->height, is->render_format, 0);
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
	if (is->hwnd)
	{

#ifdef _WIN32
		if (!is->_d3dRender)
		{
			is->d3d_render_format = vp->format;
			is->_d3dRender = d3dRender_create(is->hwnd, vp->width, vp->height, is->d3d_render_format);
			if (!is->_d3dRender)
			{
				is->d3d_render_format = AV_PIX_FMT_BGRA;
				is->_d3dRender = d3dRender_create(is->hwnd, vp->width, vp->height, is->d3d_render_format);
			}
		}
		if (!is->_d3dRender)
		{
			av_log(NULL, AV_LOG_ERROR, "create d3d renderer fail\n");
			return;
		}
		if (is->d3d_render_format == vp->format)
		{
			d3dRender_present(is->_d3dRender, vp->frame->data, vp->frame->linesize);
		}
		else
		{
			AVFrame *frame = video_frame_scale(is, vp, vp->width, vp->height, is->d3d_render_format, 64);
			if (frame)
			{
				d3dRender_present(is->_d3dRender, frame->data, frame->linesize);
			}
		}
#endif
	}
}

static double get_clock(Clock *c)
{
	if (c->queue_serial && *c->queue_serial != c->serial)
		return NAN;
	if (c->paused)
	{
		return c->pts;
	}
	else
	{
		double time = av_gettime_relative() / 1000000.0;
		return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
	}
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
	c->pts = pts;
	c->last_updated = time;
	c->pts_drift = c->pts - time;
	c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
	set_clock(c, get_clock(c), c->serial);
	c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
	c->speed = 1.0;
	c->paused = 0;
	c->queue_serial = queue_serial;
	set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
	double clock = get_clock(c);
	double slave_clock = get_clock(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
	{
		if (is->video_st)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
	{
		if (is->audio_st)
			return AV_SYNC_AUDIO_MASTER;
		else
		{
			return AV_SYNC_EXTERNAL_CLOCK;
		}
	}
	else
	{
		return AV_SYNC_EXTERNAL_CLOCK;
	}
}

static double get_master_clock(VideoState *is)
{
	double val;

	switch (get_master_sync_type(is))
	{
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

static void check_external_clock_speed(VideoState *is)
{
	if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
		is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
	{
		set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
	}
	else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
			 (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
	{
		set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
	}
	else
	{
		double speed = is->extclk.speed;
		if (speed != 1.0)
			set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
	}
}

static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
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

static void stream_toggle_pause(VideoState *is)
{
	if (is->paused)
	{
		is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
		if (is->read_pause_return != AVERROR(ENOSYS))
		{
			is->vidclk.paused = 0;
		}
		set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
	}
	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is)
{
	stream_toggle_pause(is);
	is->step = 0;
}

static void step_to_next_frame(VideoState *is)
{
	if (is->paused)
		stream_toggle_pause(is);
	is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
	double sync_threshold, diff = 0;
	if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
	{
		diff = get_clock(&is->vidclk) - get_master_clock(is);
		sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
		{
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

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
	if (vp->serial == nextvp->serial)
	{
		double duration = nextvp->pts - vp->pts;
		if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
			return vp->duration;
		else
			return duration;
	}
	else
	{
		return 0.0;
	}
}

static void update_video_pts(VideoState *is, double pts, int serial)
{
	set_clock(&is->vidclk, pts, serial);
	sync_clock_to_slave(&is->extclk, &is->vidclk);
}

static void video_refresh(void *opaque, double *remaining_time)
{
	VideoState *is = opaque;

	double time;
	Frame *sp, *sp2;
	if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
		check_external_clock_speed(is);

	if (is->show_mode != SHOW_MODE_VIDEO && is->audio_st)
	{
		time = av_gettime_relative() / 1000000.0;
		if (is->force_refresh || is->last_vis_time + is->rdftspeed < time)
		{
			video_display(is);
			is->last_vis_time = time;
		}
		*remaining_time = FFMIN(*remaining_time, is->last_vis_time + is->rdftspeed - time);
	}

	if (is->video_st)
	{
	retry:
		if (frame_queue_nb_remaining(&is->pictq) == 0)
		{
		}
		else
		{
			double last_duration, duration, delay;
			Frame *vp, *lastvp;

			lastvp = frame_queue_peek_last(&is->pictq);
			vp = frame_queue_peek(&is->pictq);

			if (vp->serial != is->videoq.serial)
			{
				frame_queue_next(&is->pictq);
				goto retry;
			}

			if (lastvp->serial != vp->serial)
				is->frame_timer = av_gettime_relative() / 1000000.0;

			if (is->paused)
				goto display;

			last_duration = vp_duration(is, lastvp, vp);
			delay = compute_target_delay(last_duration, is);

			time = av_gettime_relative() / 1000000.0;
			if (time < is->frame_timer + delay)
			{
				*remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
				goto display;
			}

			is->frame_timer += delay;
			if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
				is->frame_timer = time;

			SDL_LockMutex(is->pictq.mutex);
			if (!isnan(vp->pts))
				update_video_pts(is, vp->pts, vp->serial);
			SDL_UnlockMutex(is->pictq.mutex);

			if (frame_queue_nb_remaining(&is->pictq) > 1)
			{
				Frame *nextvp = frame_queue_peek_next(&is->pictq);
				duration = vp_duration(is, vp, nextvp);
				if (!is->step && (is->framedrop > 0 || (is->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
				{
					frame_queue_next(&is->pictq);
					goto retry;
				}
			}

			if (is->subtitle_st)
			{
				while (frame_queue_nb_remaining(&is->subpq) > 0)
				{
					sp = frame_queue_peek(&is->subpq);

					if (frame_queue_nb_remaining(&is->subpq) > 1)
						sp2 = frame_queue_peek_next(&is->subpq);
					else
						sp2 = NULL;

					if (sp->serial != is->subtitleq.serial || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
					{
						frame_queue_next(&is->subpq);
					}
					else
					{
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
		if (is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
			video_display(is);
	}
	is->force_refresh = 0;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int serial)
{
	Frame *vp;
	if (!(vp = frame_queue_peek_writable(&is->pictq)))
		return -1;
	vp->width = src_frame->width;
	vp->height = src_frame->height;
	vp->format = src_frame->format;
	vp->pts = pts;
	vp->duration = duration;
	vp->serial = serial;
	av_frame_move_ref(vp->frame, src_frame);
	frame_queue_push(&is->pictq);
	return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
	int got_picture;

	if ((got_picture = decoder_decode_frame(is, &is->viddec, frame, NULL)) < 0)
		return -1;
	if (got_picture)
	{
		double dpts = NAN;
		if (frame->pts != AV_NOPTS_VALUE)
			dpts = av_q2d(is->video_st->time_base) * frame->pts;
		frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

		if (is->framedrop > 0 || (is->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
		{
			if (frame->pts != AV_NOPTS_VALUE)
			{
				double diff = dpts - get_master_clock(is);
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
					diff - is->frame_last_filter_delay < 0 &&
					is->viddec.pkt_serial == is->vidclk.serial &&
					is->videoq.nb_packets)
				{
					av_frame_unref(frame);
					got_picture = 0;
				}
			}
		}
	}
	return got_picture;
}

static int audio_thread(void *arg)
{
	VideoState *is = arg;
	AVFrame *frame = av_frame_alloc();
	Frame *af;
	int got_frame = 0;
	AVRational tb;
	int ret = 0;

	if (!frame)
	{
		return AVERROR(ENOMEM);
	}
	do
	{
		if ((got_frame = decoder_decode_frame(is, &is->auddec, frame, NULL)) < 0)
		{
			goto the_end;
		}

		if (got_frame)
		{
			tb = (AVRational){1, frame->sample_rate};
			double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
			int serial = is->auddec.pkt_serial;
			double duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});
			if (!(af = frame_queue_peek_writable(&is->sampq)))
				goto the_end;
			af->pts = pts;
			af->serial = serial;
			af->duration = duration;
			av_frame_move_ref(af->frame, frame);
			frame_queue_push(&is->sampq);
		}
	} while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
	av_frame_free(&frame);
	return ret;
}

static int decoder_start(VideoState *is, Decoder *d, int (*fn)(void *), void *arg)
{
	packet_queue_start(is, d->queue);
	d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
	if (!d->decoder_tid)
	{
		av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread() 1: %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	return 0;
}

static int video_thread(void *arg)
{
	VideoState *is = arg;
	AVFrame *frame = av_frame_alloc();
	double pts;
	double duration;
	int ret;
	AVRational tb = is->video_st->time_base;
	AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
	if (!frame)
	{
		return AVERROR(ENOMEM);
	}
	for (;;)
	{
		ret = get_video_frame(is, frame);
		if (ret < 0)
			goto the_end;
		if (!ret)
			continue;
		duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
		pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
		ret = queue_picture(is, frame, pts, duration, is->viddec.pkt_serial);
		av_frame_unref(frame);
		if (ret < 0)
			goto the_end;
	}
the_end:
	av_frame_free(&frame);
	return 0;
}

static int subtitle_thread(void *arg)
{
	VideoState *is = arg;
	Frame *sp;
	int got_subtitle;
	double pts;
	for (;;)
	{
		if (!(sp = frame_queue_peek_writable(&is->subpq)))
		{
			return 0;
		}
		if ((got_subtitle = decoder_decode_frame(is, &is->subdec, NULL, &sp->sub)) < 0)
		{
			break;
		}
		pts = 0;
		if (got_subtitle && sp->sub.format == 0)
		{
			if (sp->sub.pts != AV_NOPTS_VALUE)
				pts = sp->sub.pts / (double)AV_TIME_BASE;
			sp->pts = pts;
			sp->serial = is->subdec.pkt_serial;
			sp->width = is->subdec.avctx->width;
			sp->height = is->subdec.avctx->height;
			frame_queue_push(&is->subpq);
		}
		else if (got_subtitle)
		{
			avsubtitle_free(&sp->sub);
		}
	}
	return 0;
}

static int synchronize_audio(VideoState *is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;
	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER)
	{
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;
		diff = get_clock(&is->audclk) - get_master_clock(is);
		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD)
		{
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
			{
				is->audio_diff_avg_count++;
			}
			else
			{
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if (fabs(avg_diff) >= is->audio_diff_threshold)
				{
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
		else
		{
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return wanted_nb_samples;
}

static int audio_decode_frame(VideoState *is)
{
	int data_size, resampled_data_size;
	int64_t dec_channel_layout;
	int wanted_nb_samples;
	Frame *af;
	if (is->paused)
		return -1;
	do
	{
#if defined(_WIN32)
		while (frame_queue_nb_remaining(&is->sampq) == 0)
		{
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
		(af->frame->ch_layout.u.mask && af->frame->ch_layout.nb_channels == av_get_channel_layout_nb_channels(af->frame->ch_layout.u.mask)) ? af->frame->ch_layout.u.mask : av_get_default_channel_layout(af->frame->ch_layout.nb_channels);
	wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

	if (af->frame->format != is->audio_src.fmt ||
		dec_channel_layout != is->audio_src.channel_layout ||
		af->frame->sample_rate != is->audio_src.freq ||
		(wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx))
	{
		swr_free(&is->swr_ctx);
		is->swr_ctx = swr_alloc_set_opts(NULL,
										 is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
										 dec_channel_layout, af->frame->format, af->frame->sample_rate,
										 0, NULL);
		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0)
		{
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
	if (is->swr_ctx)
	{
		const uint8_t **in = (const uint8_t **)af->frame->extended_data;
		uint8_t **out = &is->audio_buf1;
		int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
		int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
		int len2;
		if (out_size < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->frame->nb_samples)
		{
			if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
									 wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
		if (!is->audio_buf1)
			return AVERROR(ENOMEM);
		len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
		if (len2 < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count)
		{
			av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(is->swr_ctx) < 0)
				swr_free(&is->swr_ctx);
		}
		is->audio_buf = is->audio_buf1;
		resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	}
	else
	{
		is->audio_buf = af->frame->data[0];
		resampled_data_size = data_size;
	}

	double speed = is->speed;
	if (speed != 1)
	{
		cSoundTouch_setTempo(is->soundTouch, speed);
		cSoundTouch_putSamples(is->soundTouch, is->audio_buf, af->frame->nb_samples);
		int numSamples = 2 * af->frame->nb_samples / speed;
		{
			int size = numSamples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
			if (is->speed_buf_size < size)
			{
				is->speed_buf = av_realloc(is->speed_buf, size);
				is->speed_buf_size = size;
			}
			is->audio_buf = is->speed_buf;
		}
		int new_nb_samples = cSoundTouch_receiveSamples(is->soundTouch, is->audio_buf, numSamples);
		resampled_data_size = new_nb_samples * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
		af->frame->nb_samples = numSamples;
	}

	if (!isnan(af->pts))
		is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
	else
		is->audio_clock = NAN;
	is->audio_clock_serial = af->serial;
	return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
	VideoState *is = opaque;
	int audio_size, len1;
	is->audio_callback_time = av_gettime_relative();
	if (is->av_sync_type == AV_SYNC_AUDIO_MASTER || is->av_sync_type == AV_SYNC_EXTERNAL_CLOCK)
	{
		if (is->pos_changed_callback != NULL)
		{
			is->pos_changed_callback(is, get_master_clock(is));
		}
	}

	while (len > 0)
	{
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			audio_size = audio_decode_frame(is);
			if (audio_size < 0)
			{
				is->audio_buf = NULL;
				is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
			}
			else
			{
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;

		if (is->audio_callback_index == 0)
		{
			if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
			{
				memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
			}
			else
			{
				memset(stream, 0, len1);
				if (!is->muted && is->audio_buf)
					SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIOD_EVICE_FORMAT, len1, is->audio_volume);
			}
		}
		else
		{
			if (!is->muted && is->audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIOD_EVICE_FORMAT, len1, is->audio_volume);
		}

		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
	is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
	if (!isnan(is->audio_clock))
	{
		set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, is->audio_callback_time / 1000000.0);
		sync_clock_to_slave(&is->extclk, &is->audclk);
	}
}

static void sdl_audio_callback_sum(void *opaque, Uint8 *stream, int len)
{
	int n = 0;
	VideoState *is = opaque;
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

static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
	VideoState *is = opaque;
	SDL_LockMutex(audio_streams_mutex);
	if (!is_init_audio)
	{
		SDL_AudioSpec wanted_spec, spec;
		const char *env;
		static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
		static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
		int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
		env = SDL_getenv("SDL_AUDIO_CHANNELS");
		if (env)
		{
			wanted_nb_channels = atoi(env);
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		}
		if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout))
		{
			wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
			wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
		}
		wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
		wanted_spec.channels = wanted_nb_channels;
		wanted_spec.freq = wanted_sample_rate;
		if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0)
		{
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

		while ((devId = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 1)) < 2)
		{
			av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
				   wanted_spec.channels, wanted_spec.freq, SDL_GetError());
			wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
			if (!wanted_spec.channels)
			{
				wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
				wanted_spec.channels = wanted_nb_channels;
				if (!wanted_spec.freq)
				{
					av_log(NULL, AV_LOG_ERROR,
						   "No more combinations to try, audio open failed\n");
					SDL_UnlockMutex(audio_streams_mutex);
					return -1;
				}
			}
			wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
		}
		if (spec.format != AUDIOD_EVICE_FORMAT)
		{
			av_log(NULL, AV_LOG_ERROR,
				   "SDL advised audio format %d is not supported!\n", spec.format);
			SDL_UnlockMutex(audio_streams_mutex);
			return -1;
		}
		if (spec.channels != wanted_spec.channels)
		{
			wanted_channel_layout = av_get_default_channel_layout(spec.channels);
			if (!wanted_channel_layout)
			{
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
		if (audio_params.bytes_per_sec <= 0 || audio_params.frame_size <= 0)
		{
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

static int stream_component_open(VideoState *is, int stream_index)
{
	AVFormatContext *ic = is->ic;
	AVCodecContext *avctx;
	AVCodec *codec;
	const char *forced_codec_name = NULL;
	AVDictionary *opts = NULL;
	AVDictionaryEntry *t = NULL;
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

	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	codec = avcodec_find_decoder(avctx->codec_id);
	avctx->pix_fmt = 0;
	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->last_audio_stream = stream_index;
		forced_codec_name = is->audio_codec_name;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->last_subtitle_stream = stream_index;
		forced_codec_name = is->subtitle_codec_name;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->last_video_stream = stream_index;
		forced_codec_name = is->video_codec_name;
		break;
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
	if (!codec)
	{
		if (forced_codec_name)
			av_log(NULL, AV_LOG_WARNING,
				   "No codec could be found with name '%s'\n", forced_codec_name);
		else
			av_log(NULL, AV_LOG_WARNING,
				   "No codec could be found with id %d\n", avctx->codec_id);
		ret = AVERROR(EINVAL);
		goto fail;
	}
	if (stream_lowres > codec->max_lowres)
	{
		av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
			   codec->max_lowres);
		stream_lowres = codec->max_lowres;
	}
	avctx->lowres = stream_lowres;
	if (is->fast)
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;
	if (stream_lowres)
		av_dict_set_int(&opts, "lowres", stream_lowres, 0);

	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
	{
		goto fail;
	}
	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)))
	{
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}
	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		sample_rate = avctx->sample_rate;
		nb_channels = avctx->ch_layout.nb_channels;
		channel_layout = avctx->ch_layout.u.mask;
		is->soundTouch = cSoundTouch_create();
		cSoundTouch_setSampleRate(is->soundTouch, sample_rate);
		cSoundTouch_setChannels(is->soundTouch, nb_channels);
		if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
			goto fail;
		is->audio_hw_buf_size = ret;
		is->audio_src = is->audio_tgt;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;
		is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		is->audio_diff_avg_count = 0;
		is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;
		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];
		decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
		if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)))
		{
			is->auddec.start_pts = is->audio_st->start_time;
			is->auddec.start_pts_tb = is->audio_st->time_base;
		}
		if ((ret = decoder_start(is, &is->auddec, audio_thread, is)) < 0)
			goto out;
		SDL_PauseAudioDevice(devId, 0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->event_tid = SDL_CreateThread(event_loop, "event_loop", is);
		if (!is->event_tid)
		{
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

static int decode_interrupt_cb(void *ctx)
{
	VideoState *is = ctx;
	return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
	return stream_id < 0 ||
		   queue->abort_request ||
		   (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		   queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s)
{
	if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
		return 1;
	if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
		return 1;
	return 0;
}

static int read_thread(void *arg)
{
	VideoState *is = arg;
	AVFormatContext *ic = NULL;
	int err, i, ret = 0;
	int st_index[AVMEDIA_TYPE_NB];
	AVPacket pkt1, *pkt = &pkt1;
	int64_t stream_start_time;
	int pkt_in_play_range = 0;
	SDL_mutex *wait_mutex = SDL_CreateMutex();
	int scan_all_pmts_set = 0;
	int64_t pkt_ts;
	double seek_time = 0;
	double pkt_time;
	ACStopReason stopReason = AC_STOPREASON_ERROR;
	if (!wait_mutex)
	{
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
	if (!ic)
	{
		av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = is;
	if (!av_dict_get(is->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
	{
		av_dict_set(&is->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scan_all_pmts_set = 1;
	}

	if (is->avio)
	{
		ic->pb = is->avio;
		ic->flags = AVFMT_FLAG_CUSTOM_IO;
	}

	err = avformat_open_input(&ic, is->filename, is->iformat, &is->format_opts);
	if (err < 0)
	{
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
			if (avformat_open_input(&ic, camera, is->iformat, &is->format_opts) < 0)
			{
				ic = avformat_alloc_context();
				ic->interrupt_callback.callback = decode_interrupt_cb;
				ic->interrupt_callback.opaque = is;
				if (avformat_open_input(&ic, camera, is->iformat, NULL) < 0)
				{
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
		goto fail;
#endif
	}

	if (scan_all_pmts_set)
		av_dict_set(&is->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
	is->ic = ic;

	if (is->genpts)
		ic->flags |= AVFMT_FLAG_GENPTS;
	if (is->find_stream_info)
	{
		int orig_nb_streams = ic->nb_streams;
		err = avformat_find_stream_info(ic, NULL);
		if (err < 0)
		{
			av_log(NULL, AV_LOG_WARNING,
				   "%s: could not find codec parameters\n", is->filename);
			ret = -1;
			goto fail;
		}
	}

	av_dict_free(&is->format_opts);
	if (ic->pb)
		ic->pb->eof_reached = 0;
	if (is->seek_by_bytes < 0)
		is->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
	is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
	if (is->start_time != 0)
	{
		is->seek_req = 1;
		is->seek_pos = is->start_time;
		is->start_time = 0;
	}
	is->realtime = is_realtime(ic);
	if (is->show_status)
		av_dump_format(ic, 0, is->filename, 0);

	for (i = 0; i < ic->nb_streams; i++)
	{
		AVStream *st = ic->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && is->wanted_stream_spec[type] && st_index[type] == -1)
			if (avformat_match_stream_specifier(ic, st, is->wanted_stream_spec[type]) > 0)
				st_index[type] = i;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; i++)
	{
		if (is->wanted_stream_spec[i] && st_index[i] == -1)
		{
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
								(st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
								NULL, 0);
	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
	{
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
	}
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
	{
		ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
	}

	if (is->show_mode == SHOW_MODE_NONE)
		is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

	if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
	{
		stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
	}

	if (is->video_stream < 0 && is->audio_stream < 0)
	{
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
		if (is->viddec.avctx->codec->pix_fmts)
		{
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

	for (;;)
	{
		if (is->abort_request)
			break;
		if (is->paused != is->last_paused)
		{
			is->last_paused = is->paused;
			if (is->paused)
			{
				is->read_pause_return = av_read_pause(ic);
			}
			else
				av_read_play(ic);
		}
		if (is->seek_req)
		{
			int64_t seek_target = is->seek_pos;
			int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
			int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
			ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
			seek_time = is->seek_pos / (double)AV_TIME_BASE;
			if (ret < 0)
			{
				av_log(NULL, AV_LOG_ERROR,
					   "%s: error while seeking\n", is->filename);
			}
			else
			{
				if (is->audio_stream >= 0)
				{
					packet_queue_flush(&is->audioq);
					packet_queue_put(is, &is->audioq, &is->flush_pkt);
				}
				if (is->subtitle_stream >= 0)
				{
					packet_queue_flush(&is->subtitleq);
					packet_queue_put(is, &is->subtitleq, &is->flush_pkt);
				}
				if (is->video_stream >= 0)
				{
					packet_queue_flush(&is->videoq);
					packet_queue_put(is, &is->videoq, &is->flush_pkt);
				}
				if (is->seek_flags & AVSEEK_FLAG_BYTE)
				{
					set_clock(&is->extclk, NAN, 0);
				}
				else
				{
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

		if (is->queue_attachments_req)
		{
			if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
			{
				AVPacket copy;

				if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
					goto fail;
				packet_queue_put(is, &is->videoq, &copy);
				packet_queue_put_nullpacket(is, &is->videoq, is->video_stream);
			}
			is->queue_attachments_req = 0;
		}
		if (is->infinite_buffer < 1 &&
			(is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
																						 stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
																						 stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq))))
		{
			SDL_LockMutex(wait_mutex);
			SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			SDL_UnlockMutex(wait_mutex);
			continue;
		}
		if (!is->paused &&
			(!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
			(!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0)))
		{
			if (is->loop)
			{
				stream_seek(is, is->start_time != AV_NOPTS_VALUE ? is->start_time : 0, 0, 0);
			}
			else if (is->autoexit)
			{
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

		if (ret < 0)
		{
			if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
			{
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
		else
		{
			is->eof = 0;
		}
		pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

		if (!is->isDisablePreciseSeek)
		{
			if (seek_time > 0 && pkt->stream_index == is->video_stream && (pkt_time = pkt_ts * av_q2d(is->ic->streams[is->video_stream]->time_base)) < seek_time)
			{
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
				AVFrame *frame = av_frame_alloc();
				int got_picture = 0;
				avcodec_send_packet(is->viddec.avctx, pkt);
				avcodec_receive_frame(is->viddec.avctx, frame);
				av_packet_unref(pkt);
				av_frame_unref(frame);
				while (fabs(seek_time - pkt_time) > 0.04 && pkt_time >= 0)
				{
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

		stream_start_time = ic->streams[pkt->stream_index]->start_time;
		pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		pkt_in_play_range = is->duration == AV_NOPTS_VALUE ||
							(pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
										av_q2d(ic->streams[pkt->stream_index]->time_base) -
									(double)(is->start_time != AV_NOPTS_VALUE ? is->start_time : 0) / 1000000 <=
								((double)is->duration / 1000000);
		if (pkt->stream_index == is->audio_stream && pkt_in_play_range)
		{
			packet_queue_put(is, &is->audioq, pkt);
		}
		else if (pkt->stream_index == is->video_stream && pkt_in_play_range && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
		{
			packet_queue_put(is, &is->videoq, pkt);
		}
		else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range)
		{
			packet_queue_put(is, &is->subtitleq, pkt);
		}
		else
		{
			av_packet_unref(pkt);
		}
	}
	ret = 0;
fail:
	if (ic && !is->ic)
	{
		avformat_close_input(&ic);
	}
	SDL_DestroyMutex(wait_mutex);
	if (is->end_callback)
	{
		if (is->abort_request)
			stopReason = AC_STOPREASON_USERCALL;
		is->end_callback(is, stopReason);
	}
	return 0;
}

static int event_loop(void *lpParameter)
{
	double remaining_time = 0.0;
	VideoState *is = lpParameter;
	for (;;)
	{
		if (is->abort_request)
		{
			break;
		}
		if (remaining_time > 0.0)
			av_usleep((int64_t)(remaining_time * 1000000.0));
		remaining_time = REFRESH_RATE;
		if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
			video_refresh(is, &remaining_time);
	}
#ifdef _WIN32
	if (is->_d3dRender)
	{
		d3dRender_destory(is->_d3dRender);
		is->_d3dRender = NULL;
	}
#endif
	return 0;
}

static void stream_open(VideoState *is)
{
	if (!is->filename && !is->avio)
		goto fail;
	is->iformat = NULL;
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
	if (!(is->continue_read_thread = SDL_CreateCond()))
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		goto fail;
	}

	init_clock(&is->vidclk, &is->videoq.serial);
	init_clock(&is->audclk, &is->audioq.serial);
	init_clock(&is->extclk, &is->extclk.serial);
	is->audio_clock_serial = -1;
	is->audclk.paused = is->vidclk.paused = is->extclk.paused = is->paused;
	set_clock_speed(&is->extclk, is->speed);
	is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
	if (!is->read_tid)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread() 2: %s\n", SDL_GetError());
	fail:
		stream_close(is);
		return;
	}
	return;
}

static void init_global()
{
	static int is_init_global = 0;
	if (!is_init_global)
	{
		if (initial_mutex)
		{
			SDL_LockMutex(initial_mutex);
		}
		if (!is_init_global)
		{
			avformat_network_init();
			int flags = SDL_INIT_EVERYTHING;
			if (SDL_Init(flags))
			{
				av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
				av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
				SDL_Quit();
				av_log(NULL, AV_LOG_QUIET, "%s", "");
				return;
			}
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
			if (audio_streams_mutex == NULL)
			{
				audio_streams_mutex = SDL_CreateMutex();
			}
			memset(open_audio_streams, 0, sizeof(VideoState *) * MUTI_OPEN_NUM);
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
	HINSTANCE hinstDLL,
	DWORD fdwReason,
	LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		initial_mutex = SDL_CreateMutex();
		break;
	case DLL_PROCESS_DETACH:
		if (lpvReserved != NULL)
		{
			break;
		}
		break;
	}
	return TRUE;
}
#endif

ACPlay ac_play_create()
{
	init_global();
	VideoState *s = av_mallocz(sizeof(VideoState));
	set_default_param(s);
	return s;
}

void ac_play_destroy(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	ac_play_stop(play);
	if (s->video_codec_name)
	{
		av_free(s->video_codec_name);
	}
	if (s->audio_codec_name)
	{
		av_free(s->audio_codec_name);
	}
	av_free(s);
}

void ac_play_setWindow(ACPlay play, void *hwnd)
{
	VideoState *s = (VideoState *)play;
	s->hwnd = hwnd;
}

void *ac_play_getWindow(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->hwnd;
}

void ac_play_setHardwareAccelerateType(ACPlay play, ACHardwareAccelerateType value)
{
	VideoState *s = (VideoState *)play;
	s->hwaccel = value;
}

ACHardwareAccelerateType ac_play_getHardwareAccelerateType(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->hwaccel;
}

void ac_play_setVideoCodecName(ACPlay play, const char *codec)
{
	VideoState *s = (VideoState *)play;
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

const char *ac_play_getVideoCodecName(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->video_codec_name;
}

void ac_play_setIsDumpFormat(ACPlay play, int isDump)
{
	VideoState *s = (VideoState *)play;
	s->show_status = isDump;
}

int ac_play_getIsDumpFormat(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->show_status;
}

void ac_play_setACodecName(ACPlay play, const char *codec)
{
	VideoState *s = (VideoState *)play;
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

static void ac_play_startInternal(ACPlay play, const char *url, ACPlayCustomPacketReadCallback read, ACPlayCustomPacketStreamSeekCallback seek, const char *format_opts, double startTime)
{
	VideoState *s = (VideoState *)play;
	ac_play_stop(play);
	if (read)
		s->avio = avio_alloc_context((unsigned char *)av_malloc(1024 * 1024), 1024 * 1024, 0, s, read, NULL, (int64_t (*)(void *opaque, int64_t offset, int whence))seek);
	s->start_time = (int64_t)(startTime * AV_TIME_BASE);
	if (format_opts)
	{
		char *temp = av_strdup(format_opts);
		int i = 0;
		char *pKey = NULL;
		char *pValue = NULL;
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
	stream_open(s);
}

void ac_play_start(ACPlay play, const char *url, double startTime)
{
	ac_play_startWithOptions(play, url, NULL, startTime);
}

void ac_play_startWithOptions(ACPlay play, const char *url, const char *format_opts, double startTime)
{
	ac_play_startInternal(play, url, NULL, NULL, format_opts, startTime);
}

void ac_play_startViaCustomStream(ACPlay play, ACPlayCustomPacketReadCallback read, ACPlayCustomPacketStreamSeekCallback seek, const char *format_opts, double startTime)
{
	ac_play_startInternal(play, "", read, seek, format_opts, startTime);
}

void ac_play_stop(ACPlay p)
{
	VideoState *s = (VideoState *)p;
	if (s->filename || s->avio)
	{
		stream_close((VideoState *)p);
	}
}

void ac_play_setIsLoop(ACPlay play, int value)
{
	VideoState *s = (VideoState *)play;
	s->loop = value;
}

int ac_play_getIsLoop(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->loop;
}

void ac_play_setIsPause(ACPlay play, int isPaused)
{
	VideoState *s = (VideoState *)play;
	if (s->paused != isPaused)
		toggle_pause(s);
}

int ac_play_getIsPause(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->paused;
}

void ac_play_setIsMute(ACPlay play, int isMuted)
{
	VideoState *s = (VideoState *)play;
	s->muted = isMuted;
}

int ac_play_getIsMute(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->muted;
}

void ac_play_setIsVideoDisabled(ACPlay play, int isDisable)
{
	VideoState *s = (VideoState *)play;
	s->video_disable = isDisable;
}

int ac_play_getIsVideoDisabled(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->video_disable;
}

void ac_play_setIsAudioDisabled(ACPlay play, int isDisable)
{
	VideoState *s = (VideoState *)play;
	s->audio_disable = isDisable;
}

int ac_play_getIsAudioDisabled(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->audio_disable;
}

void ac_play_setDisableSubtitle(ACPlay play, int isDisable)
{
	VideoState *s = (VideoState *)play;
	s->subtitle_disable = isDisable;
}

void ac_play_setIsPreciseSeekDisabled(ACPlay play, int isDisable)
{
	VideoState *s = (VideoState *)play;
	s->isDisablePreciseSeek = isDisable;
}

int ac_play_getIsPreciseSeekDisabled(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->isDisablePreciseSeek;
}

void ac_play_setClockSyncType(ACPlay play, ACClockSyncType sync)
{
	VideoState *s = (VideoState *)play;
	s->av_sync_type = sync;
}

ACClockSyncType ac_play_getClockSyncType(ACPlay play)
{
	VideoState *s = (VideoState *)play;
	return s->av_sync_type;
}

void ac_play_seek(ACPlay play, double time)
{
	VideoState *is = (VideoState *)play;
	stream_seek(is, (int64_t)(time * AV_TIME_BASE), 0, 0);
}

void ac_play_setVolume(ACPlay play, int value)
{
	VideoState *is = (VideoState *)play;
	if (value < 0)
		value = 0;
	is->audio_volume = (int)(value * SDL_MIX_MAXVOLUME / 100.0);
	is->audioVolume100 = value;
}

int ac_play_getVolume(ACPlay play)
{
	VideoState *is = (VideoState *)play;
	return is->audioVolume100;
}

void ac_play_setSpeed(ACPlay play, double value)
{
	if (value <= 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Speed must be greater than 0\n");
		return;
	}
	VideoState *is = (VideoState *)play;
	set_clock_speed(&is->extclk, value);
	is->speed = value;
}

double ac_play_getSpeed(ACPlay play)
{
	VideoState *is = (VideoState *)play;
	return is->speed;
}

void ac_play_setUserData(ACPlay play, void *userdata)
{
	VideoState *is = (VideoState *)play;
	is->userdata = userdata;
}

void *ac_play_getUserData(ACPlay play)
{
	VideoState *is = (VideoState *)play;
	return is->userdata;
}

void ac_play_setStartedCallback(ACPlay play, ACPlayStartedCallback value)
{
	VideoState *is = (VideoState *)play;
	is->begin_callback = value;
}

void ac_play_setStoppingCallback(ACPlay play, ACPlayStoppingCallback value)
{
	VideoState *is = (VideoState *)play;
	is->end_callback = value;
}

void ac_play_setStoppedCallback(ACPlay play, ACPlayCallback value)
{
	VideoState *is = (VideoState *)play;
	is->stopped_callback = value;
}

void ac_play_setCursorTimeChangedCallback(ACPlay play, ACPlayCursorTimeChangedCallback value)
{
	VideoState *is = (VideoState *)play;
	is->pos_changed_callback = value;
}

void ac_play_setDisplayCallback(ACPlay play, ACPlayDisplayCallback value)
{
	VideoState *is = (VideoState *)play;
	is->render_callback = value;
}