// Authored codec/colour/resampling fixture, using synthetic generated media.
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <assert.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const enum AVCodecID required[] = {AV_CODEC_ID_VP9, AV_CODEC_ID_VP8, AV_CODEC_ID_H264,
        AV_CODEC_ID_VORBIS, AV_CODEC_ID_AAC, AV_CODEC_ID_OPUS};
    for (unsigned i=0;i<sizeof required/sizeof *required;i++) assert(avcodec_find_decoder(required[i]));
    assert(argc == 2);
    AVFormatContext *format=NULL;
    assert(avformat_open_input(&format,argv[1],NULL,NULL)==0);
    assert(avformat_find_stream_info(format,NULL)>=0);
    AVCodecContext *decoders[8]={0}; int video=0,audio=0;
    for (unsigned i=0;i<format->nb_streams;i++) {
        assert(i<8);
        AVCodecParameters *p=format->streams[i]->codecpar;
        if (p->codec_type!=AVMEDIA_TYPE_VIDEO && p->codec_type!=AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec *codec=avcodec_find_decoder(p->codec_id); assert(codec);
        decoders[i]=avcodec_alloc_context3(codec); assert(decoders[i]);
        assert(avcodec_parameters_to_context(decoders[i],p)==0);
        assert(avcodec_open2(decoders[i],codec,NULL)==0);
    }
    AVPacket *packet=av_packet_alloc(); AVFrame *frame=av_frame_alloc(); assert(packet&&frame);
    while (av_read_frame(format,packet)>=0) {
        AVCodecContext *c=decoders[packet->stream_index];
        if (c) {
            assert(avcodec_send_packet(c,packet)>=0);
            while (avcodec_receive_frame(c,frame)==0) {
                if (c->codec_type==AVMEDIA_TYPE_VIDEO) {
                    assert(frame->width==32 && frame->height==32);
                    struct SwsContext *s=sws_getContext(32,32,frame->format,32,32,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL); assert(s);
                    uint8_t rgba[32*32*4]; uint8_t *out[]={rgba}; int stride[]={32*4};
                    assert(sws_scale(s,(const uint8_t *const*)frame->data,frame->linesize,0,32,out,stride)==32);
                    sws_freeContext(s); video++;
                } else {
                    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
                    SwrContext *s=NULL;
                    assert(swr_alloc_set_opts2(&s,&stereo,AV_SAMPLE_FMT_FLT,48000,&frame->ch_layout,frame->format,frame->sample_rate,0,NULL)==0);
                    assert(swr_init(s)==0);
                    float output[16384]; uint8_t *samples[]={(uint8_t*)output};
                    assert(swr_convert(s,samples,8192,(const uint8_t**)frame->extended_data,frame->nb_samples)>0);
                    swr_free(&s); audio++;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    for (int i=0;i<8;i++) avcodec_free_context(&decoders[i]);
    av_packet_free(&packet); av_frame_free(&frame); avformat_close_input(&format);
    assert(video>=6 && audio>=2);
    return 0;
}
