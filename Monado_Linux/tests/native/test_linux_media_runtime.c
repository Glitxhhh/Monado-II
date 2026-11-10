// [XCDE] LINUX-042: authored CPU media fixture. No game files or external media.
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <opus.h>

enum { WIDTH=32, HEIGHT=32, FRAMES=3, SAMPLES=960, CHANNELS=2 };
static void checked(int result, const char *operation) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof message);
        fprintf(stderr, "%s: %s\n", operation, message);
        abort();
    }
}
#define CHECK(operation) checked((operation), #operation)
static void gradient(AVFrame *frame, unsigned index) {
    CHECK(av_frame_make_writable(frame));
    for (unsigned y=0;y<HEIGHT;++y)
        for (unsigned x=0;x<WIDTH;++x)
            frame->data[0][y*frame->linesize[0]+x]=(uint8_t)(32+x*4+y*2+index*5);
    for (unsigned y=0;y<HEIGHT/2;++y)
        for (unsigned x=0;x<WIDTH/2;++x) {
            frame->data[1][y*frame->linesize[1]+x]=128;
            frame->data[2][y*frame->linesize[2]+x]=128;
        }
}
static void write_packets(AVCodecContext *codec, AVFormatContext *format,
                          AVStream *stream, AVPacket *packet) {
    int result;
    while ((result=avcodec_receive_packet(codec,packet))==0) {
        av_packet_rescale_ts(packet,codec->time_base,stream->time_base);
        packet->stream_index=stream->index;
        CHECK(av_interleaved_write_frame(format,packet));av_packet_unref(packet);
    }
    assert(result==AVERROR(EAGAIN)||result==AVERROR_EOF);
}
static void encode_movie(const char *path, const char *container, const char *encoder) {
    const AVCodec *implementation=avcodec_find_encoder_by_name(encoder);assert(implementation);
    AVFormatContext *format=NULL;
    CHECK(avformat_alloc_output_context2(&format,NULL,container,path));assert(format);
    AVCodecContext *codec=avcodec_alloc_context3(implementation);assert(codec);
    codec->width=WIDTH;codec->height=HEIGHT;codec->pix_fmt=AV_PIX_FMT_YUV420P;
    codec->time_base=(AVRational){1,30};codec->framerate=(AVRational){30,1};
    codec->thread_count=1;codec->bit_rate=500000;codec->gop_size=FRAMES;codec->max_b_frames=0;
    if (format->oformat->flags&AVFMT_GLOBALHEADER)codec->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options=NULL;
    if (!strcmp(encoder,"libvpx-vp9")) {
        CHECK(av_dict_set(&options,"deadline","realtime",0));
        CHECK(av_dict_set(&options,"cpu-used","8",0));
        CHECK(av_dict_set(&options,"lag-in-frames","0",0));
    } else {
        CHECK(av_dict_set(&options,"preset","ultrafast",0));
        CHECK(av_dict_set(&options,"tune","zerolatency",0));
    }
    CHECK(avcodec_open2(codec,implementation,&options));
    assert(av_dict_count(options)==0);av_dict_free(&options);
    AVStream *stream=avformat_new_stream(format,NULL);assert(stream);
    stream->time_base=codec->time_base;CHECK(avcodec_parameters_from_context(stream->codecpar,codec));
    CHECK(avio_open(&format->pb,path,AVIO_FLAG_WRITE));CHECK(avformat_write_header(format,NULL));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();assert(frame&&packet);
    frame->format=codec->pix_fmt;frame->width=WIDTH;frame->height=HEIGHT;CHECK(av_frame_get_buffer(frame,32));
    for (unsigned i=0;i<FRAMES;++i) {
        gradient(frame,i);frame->pts=i;CHECK(avcodec_send_frame(codec,frame));write_packets(codec,format,stream,packet);
    }
    CHECK(avcodec_send_frame(codec,NULL));write_packets(codec,format,stream,packet);CHECK(av_write_trailer(format));
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&codec);
    CHECK(avio_closep(&format->pb));avformat_free_context(format);
}
static void receive_frames(AVCodecContext *codec, AVFrame *frame, unsigned *count) {
    int result;
    while ((result=avcodec_receive_frame(codec,frame))==0) {
        assert(frame->width==WIDTH&&frame->height==HEIGHT&&frame->format==AV_PIX_FMT_YUV420P);
        unsigned sum=0;
        for (unsigned y=0;y<HEIGHT;++y)
            for (unsigned x=0;x<WIDTH;++x)sum+=frame->data[0][y*frame->linesize[0]+x];
        // Lossy codecs: verify useful luminance/gradient rather than byte identity.
        int average=(int)(sum/(WIDTH*HEIGHT));int expected=32+31*3+(int)*count*5;
        assert(abs(average-expected)<12);assert(frame->data[0][WIDTH-1]>frame->data[0][0]+70);
        ++*count;av_frame_unref(frame);
    }
    assert(result==AVERROR(EAGAIN)||result==AVERROR_EOF);
}
static unsigned decode_movie(const char *path, enum AVCodecID expected) {
    AVFormatContext *format=NULL;
    CHECK(avformat_open_input(&format,path,NULL,NULL));CHECK(avformat_find_stream_info(format,NULL));
    int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);CHECK(stream);
    assert(format->streams[stream]->codecpar->codec_id==expected);
    const AVCodec *implementation=avcodec_find_decoder(expected);assert(implementation);
    AVCodecContext *codec=avcodec_alloc_context3(implementation);assert(codec);
    CHECK(avcodec_parameters_to_context(codec,format->streams[stream]->codecpar));
    codec->thread_count=1;CHECK(avcodec_open2(codec,implementation,NULL));
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();assert(frame&&packet);
    unsigned count=0;int result;
    while ((result=av_read_frame(format,packet))==0) {
        if (packet->stream_index==stream) {
            CHECK(avcodec_send_packet(codec,packet));receive_frames(codec,frame,&count);
        }
        av_packet_unref(packet);
    }
    assert(result==AVERROR_EOF);CHECK(avcodec_send_packet(codec,NULL));receive_frames(codec,frame,&count);assert(count==FRAMES);
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&codec);avformat_close_input(&format);return count;
}
static void scale_gradient(void) {
    AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_YUV420P;
    frame->width=WIDTH;frame->height=HEIGHT;CHECK(av_frame_get_buffer(frame,32));gradient(frame,0);
    struct SwsContext *scale=sws_getContext(WIDTH,HEIGHT,AV_PIX_FMT_YUV420P,
        WIDTH,HEIGHT,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);assert(scale);
    uint8_t rgba[WIDTH*HEIGHT*4];uint8_t *planes[]={rgba,NULL,NULL,NULL};int strides[]={WIDTH*4,0,0,0};
    assert(sws_scale(scale,(const uint8_t *const *)frame->data,frame->linesize,0,HEIGHT,planes,strides)==HEIGHT);
    for (unsigned i=0;i<WIDTH*HEIGHT;++i) {
        assert(rgba[i*4+3]==255);assert(abs((int)rgba[i*4]-(int)rgba[i*4+1])<=2);
        assert(abs((int)rgba[i*4+1]-(int)rgba[i*4+2])<=2);
    }
    assert(rgba[(WIDTH-1)*4]>rgba[0]+80);sws_freeContext(scale);av_frame_free(&frame);
}
static unsigned opus_and_resample(void) {
    int16_t source[SAMPLES*CHANNELS],decoded[SAMPLES*CHANNELS];
    for (unsigned i=0;i<SAMPLES;++i) {
        int16_t value=(int16_t)(((int)(i%96)-48)*150);source[i*2]=value;source[i*2+1]=(int16_t)-value;
    }
    int error=0;OpusEncoder *encoder=opus_encoder_create(48000,CHANNELS,OPUS_APPLICATION_AUDIO,&error);
    assert(encoder&&error==OPUS_OK);assert(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(64000))==OPUS_OK);
    assert(opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(1))==OPUS_OK);
    unsigned char packet[4096];int bytes=opus_encode(encoder,source,SAMPLES,packet,sizeof packet);assert(bytes>0);
    OpusDecoder *decoder=opus_decoder_create(48000,CHANNELS,&error);assert(decoder&&error==OPUS_OK);
    int samples=opus_decode(decoder,packet,bytes,decoded,SAMPLES,0);assert(samples==SAMPLES);
    uint64_t energy=0;for (unsigned i=0;i<SAMPLES*CHANNELS;++i)energy+=(uint64_t)((int64_t)decoded[i]*decoded[i]);
    assert(energy>1000000);opus_decoder_destroy(decoder);opus_encoder_destroy(encoder);
    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;SwrContext *resample=NULL;
    CHECK(swr_alloc_set_opts2(&resample,&stereo,AV_SAMPLE_FMT_FLT,44100,
        &stereo,AV_SAMPLE_FMT_S16,48000,0,NULL));assert(resample);CHECK(swr_init(resample));
    float output[2048*CHANNELS];const uint8_t *input[]={(const uint8_t *)source};
    uint8_t *destination=(uint8_t *)output;int total=swr_convert(resample,&destination,2048,input,SAMPLES);CHECK(total);
    for (unsigned flush=0;flush<8;++flush) {
        destination=(uint8_t *)(output+total*CHANNELS);int count=swr_convert(resample,&destination,2048-total,NULL,0);
        CHECK(count);total+=count;if (!count)break;
    }
    assert(total==882);double resampled_energy=0;
    for (int i=0;i<total;++i) {
        double left=output[i*2],right=output[i*2+1];assert(left>-1&&left<1&&right>-1&&right<1);
        double opposite=left+right;assert(opposite>-0.00001&&opposite<0.00001);resampled_energy+=left*left+right*right;
    }
    assert(resampled_energy>0.01);swr_free(&resample);return (unsigned)total;
}
int main(int argc,char **argv) {
    assert(argc==2);av_log_set_level(AV_LOG_ERROR);char webm[4096],h264[4096];
    int a=snprintf(webm,sizeof webm,"%s/synthetic-vp9.webm",argv[1]);int b=snprintf(h264,sizeof h264,"%s/synthetic-h264.mkv",argv[1]);
    assert(a>0&&(size_t)a<sizeof webm&&b>0&&(size_t)b<sizeof h264);
    encode_movie(webm,"webm","libvpx-vp9");unsigned vp9=decode_movie(webm,AV_CODEC_ID_VP9);
    encode_movie(h264,"matroska","libx264");unsigned avc=decode_movie(h264,AV_CODEC_ID_H264);
    scale_gradient();unsigned resampled=opus_and_resample();
    printf("native media runtime: PASS vp9=%u h264=%u opus=%u resampled=%u\n",vp9,avc,SAMPLES,resampled);return 0;
}
