#include "tilefinch/host_media.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <lexbor/dom/interfaces/element.h>

#ifdef TILEFINCH_HAVE_SDL_AUDIO
#include <SDL.h>
#endif

#include "tilefinch/document.h"
#include "tilefinch/resources.h"
#include "tilefinch/url.h"
#include "tilefinch/youtube_resolver.h"
#include "host_media_timing.h"

#define HOST_MEDIA_NETWORK_UA \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " \
    "AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/136.0.0.0 Safari/537.36"

struct HostMediaPlayer {
    Budget *budget;
    AVFormatContext *format;
    AVCodecContext *decoder;
    AVCodecContext *audio_decoder;
    AVFrame *frame;
    AVFrame *audio_frame;
    AVPacket *packet;
    AVPacket *queued_video[256];
    struct SwsContext *scaler;
    SwrContext *audio_resampler;
    unsigned char *pixels;
    unsigned char *audio_buffer;
    size_t audio_buffer_bytes;
    lxb_dom_node_t *node;
    uint64_t generation;
    int stream_index;
    int audio_stream_index;
    int output_width;
    int output_height;
    int scaler_pixel_format;
    AVRational time_base;
    AVRational audio_time_base;
    AVRational frame_rate;
    int64_t first_timestamp;
    int64_t pending_timestamp;
    uint64_t clock_us;
    uint64_t presented_video_time_us;
    uint64_t duration_us;
    size_t decoded_frames;
    size_t dropped_frames;
    size_t video_packets;
    size_t queued_video_head;
    size_t queued_video_count;
    size_t queued_video_bytes;
    size_t audio_packets;
    size_t audio_streams;
    uint64_t decoded_audio_samples;
    uint64_t queued_audio_samples;
    uint64_t audio_clock_origin_us;
    uint64_t audio_seek_floor_us;
    int64_t audio_stream_end_us;
    uint64_t dropped_audio_samples;
    unsigned audio_sample_rate;
    unsigned audio_channels;
    size_t working_set_bytes;
    bool have_pending;
    bool input_eof;
    bool decoder_flushed;
    bool video_ended;
    bool playing;
    bool ended;
    bool surface_adopted;
    bool native_player;
    bool audio_output_active;
    bool audio_clock_initialized;
    bool audio_device_started;
    bool audio_seek_pending;
    bool audio_seek_demuxing;
    bool audio_decoder_drained;
    bool audio_stream_end_known;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    SDL_AudioDeviceID audio_device;
#endif
    char video_codec[32];
    char audio_codec[32];
    char source_kind[24];
    char title[256];
    int resolved_itag;
    size_t resolver_bytes;
};

#define HOST_MEDIA_AUDIO_RATE 48000
#define HOST_MEDIA_AUDIO_CHANNELS 2
#define HOST_MEDIA_AUDIO_BUFFER_BYTES (32u * 1024u)
#define HOST_MEDIA_AUDIO_MAXIMUM_QUEUED_MS 500u
#define HOST_MEDIA_AUDIO_READ_AHEAD_MS 200u
#define HOST_MEDIA_VIDEO_PACKET_QUEUE 256u
#define HOST_MEDIA_VIDEO_PACKET_MAXIMUM_BYTES (256u * 1024u)
#define HOST_MEDIA_VIDEO_QUEUE_MAXIMUM_BYTES (512u * 1024u)

static void media_error(char *error, size_t error_size,
                        const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool media_node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
        && strncasecmp(name, wanted, length) == 0;
}

static uint64_t media_presented_audio_samples(const HostMediaPlayer *player)
{
    if (player == NULL || !player->audio_output_active
        || player->audio_sample_rate == 0 || player->audio_channels == 0)
        return 0;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    uint64_t bytes_per_frame = (uint64_t) player->audio_channels
                             * sizeof(int16_t);
    uint64_t queued = SDL_GetQueuedAudioSize(player->audio_device);
    uint64_t queued_samples = bytes_per_frame == 0
        ? 0 : queued / bytes_per_frame;
    return queued_samples < player->queued_audio_samples
        ? player->queued_audio_samples - queued_samples : 0;
#else
    return 0;
#endif
}

static bool media_receive_audio_frames(HostMediaPlayer *player,
                                       char *error, size_t error_size)
{
    if (player->audio_decoder == NULL) return true;
    while (true) {
        int status = avcodec_receive_frame(
            player->audio_decoder, player->audio_frame);
        if (status == AVERROR(EAGAIN)) return true;
        if (status == AVERROR_EOF) {
            player->audio_decoder_drained = true;
            return true;
        }
        if (status < 0) {
            media_error(error, error_size,
                        "audio decoder rejected the stream (%d)", status);
            return false;
        }
        int64_t timestamp = player->audio_frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE)
            timestamp = player->audio_frame->pts;
        int output_capacity = (int) (
            player->audio_buffer_bytes
            / (sizeof(int16_t) * player->audio_channels));
        int wanted = swr_get_out_samples(
            player->audio_resampler, player->audio_frame->nb_samples);
        if (wanted < 0 || wanted > output_capacity) {
            media_error(error, error_size,
                        "decoded audio frame exceeded PCM bound (%d/%d)",
                        wanted, output_capacity);
            av_frame_unref(player->audio_frame);
            return false;
        }
        unsigned char *outputs[] = {player->audio_buffer};
        int samples = swr_convert(
            player->audio_resampler, outputs, output_capacity,
            (const unsigned char **) player->audio_frame->extended_data,
            player->audio_frame->nb_samples);
        av_frame_unref(player->audio_frame);
        if (samples < 0) {
            media_error(error, error_size,
                        "audio resampling failed (%d)", samples);
            return false;
        }
        size_t skipped = 0;
        uint64_t frame_start_us = 0;
        bool have_frame_time =
            player->audio_time_base.num > 0
            && player->audio_time_base.den > 0
            && player->time_base.num > 0 && player->time_base.den > 0
            && timestamp != AV_NOPTS_VALUE;
        if (have_frame_time) {
            int64_t audio_us = av_rescale_q(
                timestamp, player->audio_time_base,
                (AVRational) {1, 1000000});
            int64_t video_origin_us = av_rescale_q(
                player->first_timestamp, player->time_base,
                (AVRational) {1, 1000000});
            frame_start_us = audio_us > video_origin_us
                ? (uint64_t) (audio_us - video_origin_us) : 0;
        }
        if (player->audio_seek_pending && samples > 0) {
            if (have_frame_time
                && frame_start_us < player->audio_seek_floor_us) {
                uint64_t delta =
                    player->audio_seek_floor_us - frame_start_us;
                int64_t wanted_skip = delta > (uint64_t) INT64_MAX
                    ? INT64_MAX
                    : av_rescale_rnd(
                        (int64_t) delta, player->audio_sample_rate,
                        1000000, AV_ROUND_UP);
                skipped = wanted_skip <= 0 ? 0
                    : (uint64_t) wanted_skip >= (uint64_t) samples
                        ? (size_t) samples : (size_t) wanted_skip;
            } else if (!have_frame_time && player->audio_seek_demuxing) {
                skipped = (size_t) samples;
            }
            player->dropped_audio_samples += skipped;
            if (skipped == (size_t) samples) continue;
            player->audio_clock_origin_us = have_frame_time
                ? frame_start_us
                  + (uint64_t) skipped * UINT64_C(1000000)
                    / player->audio_sample_rate
                : player->audio_seek_floor_us;
            player->audio_clock_initialized = true;
            player->audio_seek_pending = false;
        }
        size_t retained = (size_t) samples - skipped;
        if (!player->audio_clock_initialized) {
            player->audio_clock_origin_us = 0;
            player->audio_clock_initialized = true;
        }
        player->decoded_audio_samples += retained;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
        if (player->audio_output_active && retained > 0) {
            size_t bytes = retained * player->audio_channels
                         * sizeof(int16_t);
            const unsigned char *pcm = player->audio_buffer
                + skipped * player->audio_channels * sizeof(int16_t);
            uint64_t maximum = (uint64_t) player->audio_sample_rate
                             * player->audio_channels * sizeof(int16_t)
                             * HOST_MEDIA_AUDIO_MAXIMUM_QUEUED_MS / 1000u;
            uint64_t queued = SDL_GetQueuedAudioSize(player->audio_device);
            if (!host_media_audio_queue_admits(
                    queued, bytes, maximum)) {
                media_error(
                    error, error_size,
                    "audio output queue exceeded its %u ms bound",
                    HOST_MEDIA_AUDIO_MAXIMUM_QUEUED_MS);
                return false;
            }
            if (SDL_QueueAudio(
                    player->audio_device, pcm, (Uint32) bytes) != 0) {
                media_error(error, error_size,
                            "audio output queue failed: %s",
                            SDL_GetError());
                return false;
            }
            player->queued_audio_samples += retained;
        }
#endif
    }
}

static bool media_decode_audio_packet(HostMediaPlayer *player,
                                      const AVPacket *packet,
                                      char *error, size_t error_size)
{
    if (player->audio_decoder == NULL) return true;
    int status = avcodec_send_packet(player->audio_decoder, packet);
    if (status == AVERROR(EAGAIN)) {
        if (!media_receive_audio_frames(player, error, error_size))
            return false;
        status = avcodec_send_packet(player->audio_decoder, packet);
    }
    if (status < 0 && status != AVERROR_EOF) {
        media_error(error, error_size,
                    "audio packet admission failed (%d)", status);
        return false;
    }
    return media_receive_audio_frames(player, error, error_size);
}

static bool media_finish_audio(HostMediaPlayer *player,
                               char *error, size_t error_size)
{
    if (player->audio_decoder == NULL || player->audio_decoder_drained) {
        return true;
    }
    for (unsigned attempt = 0; attempt < 2u; attempt++) {
        int status = avcodec_send_packet(player->audio_decoder, NULL);
        if (status < 0 && status != AVERROR_EOF
            && status != AVERROR(EAGAIN)) {
            media_error(error, error_size,
                        "audio decoder flush failed (%d)", status);
            return false;
        }
        if (!media_receive_audio_frames(
                player, error, error_size)) return false;
        if (status != AVERROR(EAGAIN)
            || player->audio_decoder_drained) return true;
    }
    media_error(error, error_size,
                "audio decoder flush made no bounded progress");
    return false;
}

static bool media_queue_video_packet(HostMediaPlayer *player,
                                     const AVPacket *packet)
{
    if (packet->size <= 0
        || (size_t) packet->size > HOST_MEDIA_VIDEO_PACKET_MAXIMUM_BYTES
        || player->queued_video_count >= HOST_MEDIA_VIDEO_PACKET_QUEUE
        || (size_t) packet->size
           > HOST_MEDIA_VIDEO_QUEUE_MAXIMUM_BYTES
             - player->queued_video_bytes) return false;
    AVPacket *copy = av_packet_clone(packet);
    if (copy == NULL) return false;
    size_t tail = (player->queued_video_head
                 + player->queued_video_count)
                % HOST_MEDIA_VIDEO_PACKET_QUEUE;
    player->queued_video[tail] = copy;
    player->queued_video_count++;
    player->queued_video_bytes += (size_t) copy->size;
    return true;
}

static AVPacket *media_take_video_packet(HostMediaPlayer *player)
{
    if (player->queued_video_count == 0) return NULL;
    AVPacket *packet = player->queued_video[player->queued_video_head];
    player->queued_video[player->queued_video_head] = NULL;
    player->queued_video_head =
        (player->queued_video_head + 1u) % HOST_MEDIA_VIDEO_PACKET_QUEUE;
    player->queued_video_count--;
    player->queued_video_bytes -= (size_t) packet->size;
    return packet;
}

static bool media_prefetch_audio(HostMediaPlayer *player,
                                 uint64_t target_samples,
                                 char *error, size_t error_size)
{
    if (!player->audio_output_active || player->audio_decoder == NULL)
        return true;
    while (!player->input_eof
           && host_media_audio_prefetch_needed(
                  player->queued_audio_samples, target_samples)
           && player->queued_video_count < HOST_MEDIA_VIDEO_PACKET_QUEUE) {
        int status = av_read_frame(player->format, player->packet);
        if (status == AVERROR_EOF) {
            player->input_eof = true;
            return media_finish_audio(player, error, error_size);
        }
        if (status < 0) {
            media_error(error, error_size,
                        "media read-ahead failed (%d)", status);
            return false;
        }
        if (player->packet->stream_index == player->audio_stream_index) {
            player->audio_packets++;
            bool decoded = media_decode_audio_packet(
                player, player->packet, error, error_size);
            av_packet_unref(player->packet);
            if (!decoded) return false;
        } else if (player->packet->stream_index == player->stream_index) {
            if (!media_queue_video_packet(player, player->packet)) {
                av_packet_unref(player->packet);
                media_error(error, error_size,
                            "compressed video read-ahead exceeded bound");
                return false;
            }
            player->video_packets++;
            av_packet_unref(player->packet);
        } else {
            av_packet_unref(player->packet);
        }
    }
    return true;
}

static bool media_initialize_audio(HostMediaPlayer *player,
                                   bool enable_output,
                                   char *error, size_t error_size)
{
    const AVCodec *codec = NULL;
    int stream_index = av_find_best_stream(
        player->format, AVMEDIA_TYPE_AUDIO, -1, player->stream_index,
        &codec, 0);
    if (stream_index < 0 || codec == NULL) return true;
    AVStream *stream = player->format->streams[stream_index];
    player->audio_decoder = avcodec_alloc_context3(codec);
    if (player->audio_decoder == NULL
        || avcodec_parameters_to_context(
            player->audio_decoder, stream->codecpar) < 0
        || avcodec_open2(player->audio_decoder, codec, NULL) < 0) {
        media_error(error, error_size, "could not initialize audio decoder");
        return false;
    }
    player->audio_frame = av_frame_alloc();
    player->audio_buffer_bytes = HOST_MEDIA_AUDIO_BUFFER_BYTES;
    player->audio_buffer = budget_malloc_category(
        player->budget, BUDGET_CATEGORY_RESOURCE,
        player->audio_buffer_bytes);
    if (player->audio_frame == NULL || player->audio_buffer == NULL) {
        media_error(error, error_size,
                    "audio decode state exceeded page memory budget");
        return false;
    }
    player->audio_sample_rate = HOST_MEDIA_AUDIO_RATE;
    player->audio_channels = HOST_MEDIA_AUDIO_CHANNELS;
    AVChannelLayout output_layout;
    av_channel_layout_default(
        &output_layout, (int) player->audio_channels);
    int status = swr_alloc_set_opts2(
        &player->audio_resampler,
        &output_layout, AV_SAMPLE_FMT_S16,
        (int) player->audio_sample_rate,
        &player->audio_decoder->ch_layout,
        player->audio_decoder->sample_fmt,
        player->audio_decoder->sample_rate, 0, NULL);
    av_channel_layout_uninit(&output_layout);
    if (status < 0 || player->audio_resampler == NULL
        || swr_init(player->audio_resampler) < 0) {
        media_error(error, error_size, "could not initialize audio resampler");
        return false;
    }
    player->audio_stream_index = stream_index;
    player->audio_time_base = stream->time_base;
    if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
        int64_t start = stream->start_time == AV_NOPTS_VALUE
            ? 0 : stream->start_time;
        if (stream->duration <= INT64_MAX - start) {
            player->audio_stream_end_us = av_rescale_q(
                start + stream->duration, stream->time_base,
                (AVRational) {1, 1000000});
            player->audio_stream_end_known =
                player->audio_stream_end_us >= 0;
        }
    }
    snprintf(player->audio_codec, sizeof(player->audio_codec), "%s",
             codec->name == NULL ? "unknown" : codec->name);
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    if (enable_output) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            media_error(error, error_size, "SDL audio initialization failed: %s",
                        SDL_GetError());
            return false;
        }
        SDL_AudioSpec wanted = {0};
        SDL_AudioSpec obtained = {0};
        wanted.freq = (int) player->audio_sample_rate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = (Uint8) player->audio_channels;
        wanted.samples = 1024;
        player->audio_device = SDL_OpenAudioDevice(
            NULL, 0, &wanted, &obtained, 0);
        if (player->audio_device == 0) {
            media_error(error, error_size, "SDL audio output failed: %s",
                        SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        player->audio_output_active = true;
        /*
         * Keep the device paused until the first video frame exists and a
         * timestamp-trimmed PCM prefix has been queued. Opening it here used
         * to let audio encountered during first-frame decode escape before
         * the caller could pause the newly created player.
         */
        SDL_PauseAudioDevice(player->audio_device, 1);
    }
#else
    (void) enable_output;
#endif
    return true;
}

static lxb_dom_node_t *media_find_first_video(const PocDocument *document)
{
    if (document == NULL || document->html == NULL) return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (media_node_name_is(node, "video")) return node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return NULL;
}

static bool media_set_native_style(HostMediaPlayer *player,
                                   const ViewportContext *viewport,
                                   bool visible)
{
    if (player == NULL || player->node == NULL
        || player->node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    char style[384];
    int written;
    if (!visible) {
        written = snprintf(style, sizeof(style), "display:none");
    } else {
        int viewport_width = viewport == NULL ? 480 : viewport->css_width;
        int viewport_height = viewport == NULL ? 272 : viewport->css_height;
        int width = player->output_width;
        int height = player->output_height;
        if (width <= 0 || height <= 0
            || viewport_width <= 0 || viewport_height <= 0) return false;
        if ((int64_t) width * viewport_height
            > (int64_t) height * viewport_width) {
            height = (int) ((int64_t) height * viewport_width / width);
            width = viewport_width;
        } else {
            width = (int) ((int64_t) width * viewport_height / height);
            height = viewport_height;
        }
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        int left = (viewport_width - width) / 2;
        int top = (viewport_height - height) / 2;
        written = snprintf(
            style, sizeof(style),
            "position:fixed;left:%dpx;top:%dpx;width:%dpx;height:%dpx;"
            "display:block;background:#000;z-index:2147483647",
            left, top, width, height);
    }
    return written > 0 && (size_t) written < sizeof(style)
        && lxb_dom_element_set_attribute(
               lxb_dom_interface_element(player->node),
               (const lxb_char_t *) "style", 5,
               (const lxb_char_t *) style, (size_t) written) != NULL;
}

static lxb_dom_node_t *media_create_native_video(HostMediaPlayer *player,
                                                 PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    if (player == NULL || document == NULL || document->html == NULL
        || body == NULL) return NULL;
    static const lxb_char_t video_name[] = "video";
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, video_name, sizeof(video_name) - 1,
        NULL);
    lxb_dom_node_t *node = element == NULL ? NULL
        : lxb_dom_interface_node(element);
    player->node = node;
    if (node == NULL
        || lxb_dom_node_append_child(body, node) != LXB_DOM_EXCEPTION_OK
        || !document_refresh(document)) {
        if (node != NULL && node->parent == NULL) {
            lxb_dom_node_destroy_deep(node);
        }
        player->node = NULL;
        return NULL;
    }
    return node;
}

static uint64_t media_timestamp_us(const HostMediaPlayer *player,
                                   int64_t timestamp)
{
    if (timestamp == AV_NOPTS_VALUE) return player->clock_us;
    int64_t relative = timestamp - player->first_timestamp;
    if (relative <= 0) return 0;
    return (uint64_t) av_rescale_q(
        relative, player->time_base, (AVRational) {1, 1000000});
}

static bool media_receive_frame(HostMediaPlayer *player,
                                char *error, size_t error_size)
{
    while (true) {
        int status = avcodec_receive_frame(player->decoder, player->frame);
        if (status == 0) {
            if (player->frame->width != player->output_width
                || player->frame->height != player->output_height
                || player->frame->format != player->scaler_pixel_format) {
                media_error(
                    error, error_size,
                    "video geometry/format changed mid-stream "
                    "(%dx%d/%d, expected %dx%d/%d)",
                    player->frame->width, player->frame->height,
                    player->frame->format, player->output_width,
                    player->output_height, player->scaler_pixel_format);
                av_frame_unref(player->frame);
                return false;
            }
            int64_t timestamp = player->frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) timestamp = player->frame->pts;
            if (player->decoded_frames == 0) {
                player->first_timestamp = timestamp == AV_NOPTS_VALUE
                    ? 0 : timestamp;
            }
            player->pending_timestamp = timestamp;
            player->have_pending = true;
            return true;
        }
        if (status == AVERROR_EOF) {
            player->video_ended = true;
            return true;
        }
        if (status != AVERROR(EAGAIN)) {
            media_error(error, error_size,
                        "video decoder rejected the stream (%d)", status);
            return false;
        }
        AVPacket *queued = media_take_video_packet(player);
        if (queued != NULL) {
            status = avcodec_send_packet(player->decoder, queued);
            av_packet_free(&queued);
            if (status == AVERROR(EAGAIN)) continue;
            if (status < 0) {
                media_error(error, error_size,
                            "queued video packet admission failed (%d)",
                            status);
                return false;
            }
            continue;
        }
        if (player->input_eof) {
            if (!player->decoder_flushed) {
                status = avcodec_send_packet(player->decoder, NULL);
                if (status < 0 && status != AVERROR_EOF) {
                    media_error(error, error_size,
                                "video decoder flush failed (%d)", status);
                    return false;
                }
                player->decoder_flushed = true;
                continue;
            }
            player->video_ended = true;
            return true;
        }
        while ((status = av_read_frame(player->format, player->packet)) >= 0) {
            if (player->packet->stream_index != player->stream_index) {
                if (player->packet->stream_index
                    == player->audio_stream_index) {
                    player->audio_packets++;
                    bool decoded = media_decode_audio_packet(
                        player, player->packet, error, error_size);
                    av_packet_unref(player->packet);
                    if (!decoded) return false;
                    continue;
                }
                av_packet_unref(player->packet);
                continue;
            }
            player->video_packets++;
            status = avcodec_send_packet(player->decoder, player->packet);
            av_packet_unref(player->packet);
            if (status == AVERROR(EAGAIN)) break;
            if (status < 0) {
                media_error(error, error_size,
                            "video packet admission failed (%d)", status);
                return false;
            }
            break;
        }
        if (status == AVERROR_EOF) {
            player->input_eof = true;
            if (!media_finish_audio(
                    player, error, error_size)) return false;
        } else if (status < 0 && status != AVERROR(EAGAIN)) {
            media_error(error, error_size,
                        "media demux failed (%d)", status);
            return false;
        }
    }
}

static bool media_present_pending(HostMediaPlayer *player,
                                  char *error, size_t error_size)
{
    if (!player->have_pending) return true;
    uint64_t frame_time = media_timestamp_us(
        player, player->pending_timestamp);
    uint8_t *planes[4] = {player->pixels, NULL, NULL, NULL};
    int strides[4] = {player->output_width * 4, 0, 0, 0};
    int rows = sws_scale(
        player->scaler,
        (const uint8_t *const *) player->frame->data,
        player->frame->linesize, 0, player->frame->height,
        planes, strides);
    if (rows != player->output_height) {
        media_error(error, error_size,
                    "video color conversion produced %d/%d rows",
                    rows, player->output_height);
        return false;
    }
    player->decoded_frames++;
    player->presented_video_time_us = frame_time;
    player->have_pending = false;
    av_frame_unref(player->frame);
    return true;
}

static void media_discard_pending(HostMediaPlayer *player)
{
    if (player == NULL || !player->have_pending) return;
    player->decoded_frames++;
    player->dropped_frames++;
    player->have_pending = false;
    av_frame_unref(player->frame);
}

static uint64_t media_nominal_frame_duration_us(
    const HostMediaPlayer *player)
{
    if (player == NULL || player->frame_rate.num <= 0
        || player->frame_rate.den <= 0) return 0;
    return (uint64_t) av_rescale_q(
        1, (AVRational) {
            player->frame_rate.den, player->frame_rate.num
        }, (AVRational) {1, 1000000});
}

static bool media_audio_clock_master_active(
    const HostMediaPlayer *player)
{
    if (player == NULL || !player->audio_output_active
        || player->audio_decoder == NULL) return false;
    bool output_pending = false;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    output_pending = SDL_GetQueuedAudioSize(player->audio_device) != 0;
#endif
    bool declared_end_reached = false;
    if (player->audio_stream_end_known) {
        int64_t video_origin_us = av_rescale_q(
            player->first_timestamp, player->time_base,
            (AVRational) {1, 1000000});
        uint64_t relative_end = player->audio_stream_end_us > video_origin_us
            ? (uint64_t) (player->audio_stream_end_us - video_origin_us) : 0;
        declared_end_reached = player->clock_us >= relative_end;
    }
    return host_media_audio_clock_is_master(
        true, player->audio_decoder_drained,
        declared_end_reached, output_pending);
}

HostMediaPlayer *host_media_create(Budget *budget, BrowserSession *session,
                                   const char *source,
                                   int maximum_width, int maximum_height,
                                   bool enable_audio_output,
                                   char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || source == NULL || source[0] == '\0'
        || maximum_width <= 0 || maximum_height <= 0) {
        media_error(error, error_size, "invalid host media request");
        return NULL;
    }
    HostMediaPlayer *player = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*player));
    if (player == NULL) {
        media_error(error, error_size, "media state exceeded page budget");
        return NULL;
    }
    player->budget = budget;
    player->stream_index = -1;
    player->audio_stream_index = -1;
    const char *input = source;
    bool network_input = false;
    bool youtube_input = false;
    YoutubeStream youtube = {0};
    if (strstr(source, "youtube.com/") != NULL
        || strstr(source, "youtu.be/") != NULL) {
        if (session == NULL
            || !youtube_resolve_progressive_mp4(
                budget, session, source, maximum_height, 30000,
                &youtube, error, error_size)) {
            if (error != NULL && error_size != 0 && error[0] == '\0')
                media_error(error, error_size, "YouTube resolution failed");
            host_media_destroy(player);
            return NULL;
        }
        input = youtube.media_url;
        network_input = true;
        youtube_input = true;
        player->resolved_itag = youtube.itag;
        player->resolver_bytes = youtube.watch_bytes + youtube.player_bytes;
        snprintf(player->source_kind, sizeof(player->source_kind), "youtube");
        snprintf(player->title, sizeof(player->title), "%s", youtube.title);
    } else {
        TilefinchUrl network_url;
        if (tilefinch_url_parse(source, &network_url)) {
            network_input = true;
        } else if (strchr(source, ':') != NULL) {
            media_error(error, error_size,
                        "unsupported or malformed media URL");
            host_media_destroy(player);
            return NULL;
        }
        if (network_input) {
        snprintf(player->source_kind, sizeof(player->source_kind), "network");
        snprintf(player->title, sizeof(player->title), "Network video");
        } else {
            snprintf(player->source_kind, sizeof(player->source_kind), "file");
            const char *name = strrchr(source, '/');
            snprintf(player->title, sizeof(player->title), "%s",
                     name == NULL ? source : name + 1);
        }
    }
    AVDictionary *options = NULL;
    TilefinchUrl input_url = {0};
    if (network_input) {
        if (!tilefinch_url_parse(input, &input_url)) {
            media_error(error, error_size, "invalid network media URL");
            host_media_destroy(player);
            return NULL;
        }
        av_dict_set(
            &options, "protocol_whitelist",
            input_url.scheme == TILEFINCH_URL_SCHEME_HTTPS
                ? "https,tcp,tls,httpproxy"
                : "http,tcp,httpproxy", 0);
        av_dict_set(&options, "user_agent", HOST_MEDIA_NETWORK_UA, 0);
        if (youtube_input)
            av_dict_set(
                &options, "referer", "https://www.youtube.com/", 0);
        av_dict_set(
            &options, "max_redirects",
            youtube_input ? "0" : "3", 0);
        av_dict_set(&options, "rw_timeout", "15000000", 0);
    }
    int open_status = avformat_open_input(
        &player->format, input, NULL, &options);
    av_dict_free(&options);
    if (open_status < 0) {
        media_error(error, error_size, "could not open MP4 media");
        host_media_destroy(player);
        return NULL;
    }
    TilefinchUrl effective_url = {0};
    if (network_input
        && (player->format->url == NULL
            || !tilefinch_url_parse(
                player->format->url, &effective_url)
            || effective_url.scheme != input_url.scheme
            || (youtube_input
                && !youtube_media_url_supported(
                    player->format->url)))) {
        media_error(error, error_size,
                    "media redirect left its trusted HTTPS origin class");
        host_media_destroy(player);
        return NULL;
    }
    if (avformat_find_stream_info(player->format, NULL) < 0) {
        media_error(error, error_size, "could not inspect MP4 media");
        host_media_destroy(player);
        return NULL;
    }
    const AVCodec *codec = NULL;
    int stream_index = av_find_best_stream(
        player->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_index < 0 || codec == NULL) {
        media_error(error, error_size, "MP4 has no decodable video stream");
        host_media_destroy(player);
        return NULL;
    }
    AVStream *stream = player->format->streams[stream_index];
    int source_width = stream->codecpar->width;
    int source_height = stream->codecpar->height;
    if (source_width <= 0 || source_height <= 0
        || source_width > maximum_width || source_height > maximum_height) {
        media_error(error, error_size,
                    "video geometry %dx%d exceeds lab bound %dx%d",
                    source_width, source_height,
                    maximum_width, maximum_height);
        host_media_destroy(player);
        return NULL;
    }
    player->decoder = avcodec_alloc_context3(codec);
    if (player->decoder == NULL
        || avcodec_parameters_to_context(
               player->decoder, stream->codecpar) < 0
        || avcodec_open2(player->decoder, codec, NULL) < 0) {
        media_error(error, error_size, "could not initialize video decoder");
        host_media_destroy(player);
        return NULL;
    }
    player->frame = av_frame_alloc();
    player->packet = av_packet_alloc();
    player->output_width = source_width;
    player->output_height = source_height;
    player->scaler_pixel_format = player->decoder->pix_fmt;
    player->scaler = sws_getContext(
        source_width, source_height, player->decoder->pix_fmt,
        player->output_width, player->output_height, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    size_t pixels = (size_t) player->output_width
                  * (size_t) player->output_height;
    if (player->frame == NULL || player->packet == NULL
        || player->scaler == NULL || pixels > SIZE_MAX / 4u) {
        media_error(error, error_size, "could not allocate decode state");
        host_media_destroy(player);
        return NULL;
    }
    player->pixels = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, pixels * 4u);
    if (player->pixels == NULL) {
        media_error(error, error_size,
                    "video surface exceeded page memory budget");
        host_media_destroy(player);
        return NULL;
    }
    player->stream_index = stream_index;
    player->time_base = stream->time_base;
    player->frame_rate = av_guess_frame_rate(
        player->format, stream, NULL);
    player->duration_us = player->format->duration > 0
        ? (uint64_t) player->format->duration : 0;
    snprintf(player->video_codec, sizeof(player->video_codec), "%s",
             codec->name == NULL ? "unknown" : codec->name);
    for (unsigned i = 0; i < player->format->nb_streams; i++) {
        AVCodecParameters *parameters = player->format->streams[i]->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        player->audio_streams++;
    }
    if (!media_initialize_audio(
            player, enable_audio_output, error, error_size)) {
        host_media_destroy(player);
        return NULL;
    }
    int decoded_buffer_bytes = av_image_get_buffer_size(
        player->decoder->pix_fmt, source_width, source_height, 1);
    player->working_set_bytes = pixels * 4u
        + (decoded_buffer_bytes > 0 ? (size_t) decoded_buffer_bytes : 0)
        + player->audio_buffer_bytes;
    player->playing = true;
    if (!media_receive_frame(player, error, error_size)
        || player->video_ended || !player->have_pending
        || !media_present_pending(player, error, error_size)) {
        if (error != NULL && error_size != 0 && error[0] == '\0') {
            media_error(error, error_size, "video contains no decoded frame");
        }
        host_media_destroy(player);
        return NULL;
    }
    return player;
}

static bool media_attach_video(HostMediaPlayer *player,
                               NavigationSession *navigation,
                               bool create_native,
                               char *error, size_t error_size)
{
    if (player == NULL || navigation == NULL || !navigation->page.loaded
        || player->surface_adopted) {
        media_error(error, error_size, "media cannot attach to this page");
        return false;
    }
    lxb_dom_node_t *node = media_find_first_video(
        &navigation->page.document);
    if (node == NULL && create_native) {
        node = media_create_native_video(
            player, &navigation->page.document);
    }
    if (node == NULL) {
        media_error(error, error_size, "page has no video element");
        return false;
    }
    player->node = node;
    if (create_native
        && !media_set_native_style(
               player, &navigation->viewport, true)) {
        media_error(error, error_size, "could not style native video surface");
        return false;
    }
    if (create_native
        && !document_refresh(&navigation->page.document)) {
        media_error(error, error_size,
                    "could not refresh native video document");
        return false;
    }
    if (!images_replace_with_decoded_surface(
            &navigation->page.images, navigation->budget, node,
            player->pixels, player->output_width, player->output_height)) {
        media_error(error, error_size,
                    "could not adopt the video surface");
        return false;
    }
    player->surface_adopted = true;
    player->node = node;
    player->native_player = create_native;
    player->generation = navigation->generation;
    navigation->images = &navigation->page.images;
    layout_reuse_cache_update_images(
        navigation->page.layout_reuse, &navigation->page.images);
    if (!navigation_relayout(navigation)) {
        media_error(error, error_size,
                    "video surface relayout failed");
        return false;
    }
    return true;
}

bool host_media_attach_first_video(HostMediaPlayer *player,
                                   NavigationSession *navigation,
                                   char *error, size_t error_size)
{
    return media_attach_video(
        player, navigation, false, error, error_size);
}

bool host_media_attach_native_player(HostMediaPlayer *player,
                                     NavigationSession *navigation,
                                     char *error, size_t error_size)
{
    return media_attach_video(
        player, navigation, true, error, error_size);
}

bool host_media_reattach_native_player(HostMediaPlayer *player,
                                       NavigationSession *navigation,
                                       char *error, size_t error_size)
{
    if (player == NULL || navigation == NULL || !player->native_player
        || !player->surface_adopted
        || player->generation == navigation->generation
        || player->output_width <= 0 || player->output_height <= 0
        || (size_t) player->output_width
           > SIZE_MAX / (size_t) player->output_height
        || (size_t) player->output_width * (size_t) player->output_height
           > SIZE_MAX / 4u) {
        media_error(error, error_size,
                    "media cannot reattach to this page");
        return false;
    }

    /*
     * The retired document owned the previously adopted RGBA surface and
     * released it during navigation. Keep the demux/decoder state, but never
     * reuse or inspect that retired pointer. A replacement surface has the
     * same fixed bound as the original player allocation.
     */
    size_t surface_bytes = (size_t) player->output_width
                         * (size_t) player->output_height * 4u;
    player->pixels = NULL;
    player->node = NULL;
    player->surface_adopted = false;
    player->generation = 0;
    unsigned char *pixels = budget_malloc_category(
        player->budget, BUDGET_CATEGORY_RESOURCE, surface_bytes);
    if (pixels == NULL) {
        media_error(error, error_size,
                    "video reattachment exceeded page memory budget");
        return false;
    }
    memset(pixels, 0, surface_bytes);
    player->pixels = pixels;

    uint64_t restore_time_us = player->clock_us;
    if (!media_attach_video(
            player, navigation, true, error, error_size)) {
        if (!player->surface_adopted) {
            budget_free(player->budget, player->pixels);
            player->pixels = NULL;
        }
        return false;
    }
    bool frame_changed = false;
    if (!host_media_seek(
            player, navigation, restore_time_us, &frame_changed,
            error, error_size)) {
        return false;
    }
    return true;
}

bool host_media_set_native_player_visible(HostMediaPlayer *player,
                                          NavigationSession *navigation,
                                          bool visible,
                                          char *error, size_t error_size)
{
    if (player == NULL || navigation == NULL || !player->native_player
        || !host_media_is_attached(player, navigation)
        || !media_set_native_style(
               player, &navigation->viewport, visible)
        || !document_refresh(&navigation->page.document)
        || !navigation_relayout(navigation)) {
        media_error(error, error_size,
                    "could not update native media visibility");
        return false;
    }
    return true;
}

bool host_media_is_attached(const HostMediaPlayer *player,
                            const NavigationSession *navigation)
{
    if (player == NULL || navigation == NULL || !player->surface_adopted
        || player->generation != navigation->generation) return false;
    const ImageResource *surface = images_find_node(
        &navigation->page.images, player->node);
    return surface != NULL && surface->pixels == player->pixels;
}

const void *host_media_surface_identity(
    const HostMediaPlayer *player, const NavigationSession *navigation)
{
    return host_media_is_attached(player, navigation)
        ? player->pixels : NULL;
}

bool host_media_copy_current_frame_rgb565(
    const HostMediaPlayer *player, uint16_t *pixels,
    int width, int height, int stride)
{
    if (player == NULL || player->pixels == NULL || pixels == NULL
        || player->output_width <= 0 || player->output_height <= 0
        || width <= 0 || height <= 0 || stride < width) return false;
    for (int y = 0; y < height; y++) {
        int source_y = y * player->output_height / height;
        uint16_t *destination = pixels + (size_t) y * stride;
        const unsigned char *source = player->pixels
            + (size_t) source_y * player->output_width * 4u;
        for (int x = 0; x < width; x++) {
            const unsigned char *rgba = source
                + (size_t) (x * player->output_width / width) * 4u;
            destination[x] = (uint16_t) (
                ((uint16_t) (rgba[0] >> 3) << 11)
                | ((uint16_t) (rgba[1] >> 2) << 5)
                | (uint16_t) (rgba[2] >> 3));
        }
    }
    return true;
}

bool host_media_advance(HostMediaPlayer *player,
                        NavigationSession *navigation,
                        unsigned elapsed_ms, bool *frame_changed,
                        char *error, size_t error_size)
{
    if (frame_changed != NULL) *frame_changed = false;
    if (player == NULL || navigation == NULL
        || !host_media_is_attached(player, navigation)) {
        media_error(error, error_size, "media is detached from the page");
        return false;
    }
    if (!player->playing || player->ended || elapsed_ms == 0) return true;
    uint64_t delta = (uint64_t) elapsed_ms * UINT64_C(1000);
    player->clock_us = delta > UINT64_MAX - player->clock_us
        ? UINT64_MAX : player->clock_us + delta;
    if (player->audio_output_active && player->audio_sample_rate != 0) {
        uint64_t presented_samples =
            media_presented_audio_samples(player);
        uint64_t target_samples =
            host_media_audio_prefetch_target_samples(
                presented_samples, player->audio_sample_rate,
                HOST_MEDIA_AUDIO_READ_AHEAD_MS);
        if (!media_prefetch_audio(
                player, target_samples, error, error_size)) return false;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
        if (!player->audio_device_started
            && player->audio_clock_initialized
            && SDL_GetQueuedAudioSize(player->audio_device) != 0) {
            SDL_PauseAudioDevice(player->audio_device, 0);
            player->audio_device_started = true;
        }
#endif
    }
    /*
     * Range/demux read-ahead retains only compressed video packets, so the
     * hardware PCM cursor can safely be the presentation master without
     * stalling at the next video packet boundary.
     */
    uint64_t presented_audio = player->audio_clock_origin_us;
    if (player->audio_sample_rate != 0) {
        presented_audio +=
            media_presented_audio_samples(player) * UINT64_C(1000000)
            / player->audio_sample_rate;
    }
    HostMediaTiming timing = host_media_timing_snapshot(
        player->clock_us, player->presented_video_time_us,
        presented_audio,
        media_audio_clock_master_active(player),
        player->audio_clock_initialized);
    uint64_t presentation_clock = timing.presentation_limit_us;
    uint64_t frame_duration_us =
        media_nominal_frame_duration_us(player);
    bool changed = false;
    while (!player->video_ended) {
        if (!player->have_pending
            && !media_receive_frame(player, error, error_size)) return false;
        if (player->video_ended || !player->have_pending) break;
        uint64_t frame_time = media_timestamp_us(
            player, player->pending_timestamp);
        if (host_media_timing_frame_is_stale(
                frame_time, presentation_clock, frame_duration_us)) {
            media_discard_pending(player);
            continue;
        }
        if (frame_time > presentation_clock
            && !(player->audio_output_active
                 && player->audio_decoder != NULL
                 && player->audio_clock_initialized
                 && host_media_timing_should_present(
                        player->presented_video_time_us,
                        presentation_clock, frame_time))) break;
        if (changed) player->dropped_frames++;
        if (!media_present_pending(player, error, error_size)) return false;
        changed = true;
    }
    if (player->video_ended) {
#ifdef TILEFINCH_HAVE_SDL_AUDIO
        player->ended = !player->audio_output_active
            || SDL_GetQueuedAudioSize(player->audio_device) == 0;
#else
        player->ended = true;
#endif
    }
    if (frame_changed != NULL) *frame_changed = changed;
    return true;
}

static void media_clear_video_queue(HostMediaPlayer *player)
{
    if (player == NULL) return;
    for (size_t i = 0; i < HOST_MEDIA_VIDEO_PACKET_QUEUE; i++) {
        av_packet_free(&player->queued_video[i]);
    }
    player->queued_video_head = 0;
    player->queued_video_count = 0;
    player->queued_video_bytes = 0;
}

bool host_media_seek(HostMediaPlayer *player,
                     NavigationSession *navigation,
                     uint64_t target_time_us, bool *frame_changed,
                     char *error, size_t error_size)
{
    if (frame_changed != NULL) *frame_changed = false;
    if (player == NULL || navigation == NULL
        || !host_media_is_attached(player, navigation)) {
        media_error(error, error_size, "media is detached from the page");
        return false;
    }
    if (player->duration_us != 0 && target_time_us > player->duration_us) {
        target_time_us = player->duration_us;
    }
    uint64_t seek_time = target_time_us;
    if (player->format->start_time != AV_NOPTS_VALUE
        && player->format->start_time > 0
        && seek_time <= UINT64_MAX - (uint64_t) player->format->start_time) {
        seek_time += (uint64_t) player->format->start_time;
    }
    int64_t timestamp = seek_time > (uint64_t) INT64_MAX
        ? INT64_MAX : (int64_t) seek_time;
    int status = avformat_seek_file(
        player->format, -1, INT64_MIN, timestamp, INT64_MAX,
        AVSEEK_FLAG_BACKWARD);
    if (status < 0) {
        media_error(error, error_size, "media seek failed (%d)", status);
        return false;
    }
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    /*
     * Do not destroy audible state until the demuxer accepts the seek. A
     * rejected seek must leave ordinary playback running instead of silently
     * pausing a player whose UI still says it is playing.
     */
    if (player->audio_output_active) {
        SDL_PauseAudioDevice(player->audio_device, 1);
        SDL_ClearQueuedAudio(player->audio_device);
        player->audio_device_started = false;
    }
#endif
    avcodec_flush_buffers(player->decoder);
    if (player->audio_decoder != NULL) {
        avcodec_flush_buffers(player->audio_decoder);
    }
    if (player->audio_resampler != NULL) {
        swr_close(player->audio_resampler);
        if (swr_init(player->audio_resampler) < 0) {
            media_error(error, error_size,
                        "audio resampler reset failed after seek");
            goto seek_failed;
        }
    }
    av_frame_unref(player->frame);
    if (player->audio_frame != NULL) av_frame_unref(player->audio_frame);
    av_packet_unref(player->packet);
    media_clear_video_queue(player);
    player->have_pending = false;
    player->input_eof = false;
    player->decoder_flushed = false;
    player->video_ended = false;
    player->ended = false;
    player->clock_us = target_time_us;
    player->audio_clock_origin_us = target_time_us;
    player->audio_seek_floor_us = target_time_us;
    player->decoded_audio_samples = 0;
    player->queued_audio_samples = 0;
    player->audio_clock_initialized = false;
    player->audio_seek_pending = player->audio_decoder != NULL;
    player->audio_seek_demuxing = player->audio_decoder != NULL;
    player->audio_decoder_drained = false;
    bool changed = false;
    while (!player->video_ended) {
        if (!media_receive_frame(player, error, error_size))
            goto seek_failed;
        if (player->video_ended || !player->have_pending) break;
        uint64_t frame_time = media_timestamp_us(
            player, player->pending_timestamp);
        if (frame_time > target_time_us
            && !host_media_timing_should_present_seek(
                   changed, player->presented_video_time_us,
                   target_time_us, frame_time)) break;
        if (!media_present_pending(player, error, error_size))
            goto seek_failed;
        changed = true;
        if (frame_time > target_time_us) break;
    }
    player->audio_seek_demuxing = false;
    if (frame_changed != NULL) *frame_changed = changed;
    return true;

seek_failed:
    player->audio_seek_demuxing = false;
    return false;
}

void host_media_set_playing(HostMediaPlayer *player, bool playing)
{
    if (player == NULL || player->ended) return;
    player->playing = playing;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    if (player->audio_output_active) {
        bool start = playing && player->audio_clock_initialized
            && SDL_GetQueuedAudioSize(player->audio_device) != 0;
        SDL_PauseAudioDevice(player->audio_device, start ? 0 : 1);
        player->audio_device_started = start;
    }
#endif
}

bool host_media_get_stats(const HostMediaPlayer *player,
                          HostMediaStats *stats)
{
    if (player == NULL || stats == NULL) return false;
    uint64_t presented_audio = media_presented_audio_samples(player);
    uint64_t clock_samples = player->audio_output_active
        ? presented_audio : player->decoded_audio_samples;
    uint64_t audio_time = player->audio_sample_rate == 0
        ? player->audio_clock_origin_us
        : player->audio_clock_origin_us
          + clock_samples * UINT64_C(1000000) / player->audio_sample_rate;
    HostMediaTiming timing = host_media_timing_snapshot(
        player->clock_us, player->presented_video_time_us, audio_time,
        media_audio_clock_master_active(player),
        player->audio_clock_initialized);
    *stats = (HostMediaStats) {
        .source_width = player->decoder->width,
        .source_height = player->decoder->height,
        .output_width = player->output_width,
        .output_height = player->output_height,
        .frame_rate_numerator = player->frame_rate.num > 0
            ? (unsigned) player->frame_rate.num : 0,
        .frame_rate_denominator = player->frame_rate.den > 0
            ? (unsigned) player->frame_rate.den : 1,
        .duration_us = player->duration_us,
        .current_time_us = timing.current_time_us,
        .audio_time_us = audio_time,
        .audio_video_skew_us = timing.audio_video_skew_us,
        .decoded_frames = player->decoded_frames,
        .dropped_frames = player->dropped_frames,
        .video_packets = player->video_packets,
        .audio_packets = player->audio_packets,
        .audio_streams = player->audio_streams,
        .decoded_audio_samples = player->decoded_audio_samples,
        .presented_audio_samples = presented_audio,
        .dropped_audio_samples = player->dropped_audio_samples,
        .audio_sample_rate = player->audio_sample_rate,
        .audio_channels = player->audio_channels,
        .working_set_bytes = player->working_set_bytes
                           + player->queued_video_bytes,
        .audio_output_active = player->audio_output_active,
        .playing = player->playing,
        .ended = player->ended,
        .resolved_itag = player->resolved_itag,
        .resolver_bytes = player->resolver_bytes
    };
    snprintf(stats->source_kind, sizeof(stats->source_kind), "%s",
             player->source_kind);
    snprintf(stats->video_codec, sizeof(stats->video_codec), "%s",
             player->video_codec);
    snprintf(stats->audio_codec, sizeof(stats->audio_codec), "%s",
             player->audio_codec);
    snprintf(stats->title, sizeof(stats->title), "%s", player->title);
    return true;
}

void host_media_destroy(HostMediaPlayer *player)
{
    if (player == NULL) return;
#ifdef TILEFINCH_HAVE_SDL_AUDIO
    if (player->audio_device != 0) {
        SDL_ClearQueuedAudio(player->audio_device);
        SDL_CloseAudioDevice(player->audio_device);
        player->audio_device = 0;
    }
    if (player->audio_output_active) SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
    if (!player->surface_adopted) {
        budget_free(player->budget, player->pixels);
    }
    budget_free(player->budget, player->audio_buffer);
    media_clear_video_queue(player);
    swr_free(&player->audio_resampler);
    av_frame_free(&player->audio_frame);
    avcodec_free_context(&player->audio_decoder);
    sws_freeContext(player->scaler);
    av_packet_free(&player->packet);
    av_frame_free(&player->frame);
    avcodec_free_context(&player->decoder);
    avformat_close_input(&player->format);
    Budget *budget = player->budget;
    memset(player, 0, sizeof(*player));
    budget_free(budget, player);
}
