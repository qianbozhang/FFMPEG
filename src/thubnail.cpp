#include "thumbnail.h"

#include <stdio.h>
#include <string>
#include <string.h>
#include <sstream>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
};

#define DUMP_YUV  false

#define CHECKEQUAL(value, except)             \
    do                                        \
    {                                         \
        if (value == except)                  \
        {                                     \
            m_Cb(EVENT_ABORT, ERROR_UNKNOWN); \
            return -1;                        \
        }                                     \
    } while (0);

/**
 *thumbnail
 */
Thumbnail::Thumbnail(/* args */)
{
    //ffmpeg
    m_FmtCtx      = NULL;
    m_picture_buf = NULL;
    m_Duration    = 0;
    m_VideoTrack  = -1;
    m_VideoWidth  = -1;
    m_VideoHeight = -1;
    m_SwsWidth    = -1;
    m_SwsHeight   = -1;

    //user data
    m_FilePath  = "";
    m_width     = -1; /*user*/
    m_height    = -1; /*user*/
    m_Pos       = 0;
    m_Cb        = NULL;

    //thumb state
    m_Event = EVENT_UNKNOWN;
    m_Error = ERROR_UNKNOWN;

    //stop flag
    m_Stop = false;
}

Thumbnail::~Thumbnail()
{
    if (m_FmtCtx)
    {
        avformat_close_input(&m_FmtCtx);
        avformat_free_context(m_FmtCtx);
    }

    if(m_picture_buf)
    {
        av_free(m_picture_buf);
    }

    // if (m_Cb)
    // {
    //     m_Cb(m_Event, m_Error);
    // }
}

/*stop getthumbnail action*/
int Thumbnail::Stop()
{
    m_Stop  = true;
    m_Event = EVENT_ABORT;
    m_Error = ERROR_UNKNOWN;
    return 0;
}

//get thumb
int Thumbnail::getThumbnail(const std::string &uri, int width, int height, int pos, CallBackFun cb)
{
    int ret = 0;
    ret = Init(uri, width, height, pos, cb);
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = OPenFile();
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = SeekToPos();
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = DecoderFrame();
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = EditYvuData();
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = SavePicture();
    if (ret < 0)
    {
        return -1;
    }

    return 0;
}

/*param init*/
int Thumbnail::Init(const std::string &uri, int width, int height, int pos, CallBackFun cb)
{
    if (uri.empty() || width <= 0 || height <= 0 || cb == NULL)
    {
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_INITFAIL;
        return -1;
    }
    m_width    = width;
    m_height   = height;
    m_FilePath = uri;
    m_Cb       = cb;
    m_Pos      = pos;

    return 0;
}

/*open file & find video track info */
int Thumbnail::OPenFile()
{
    int  ret = 0;
    char errMsg[512] = {0};
    ret = avformat_open_input(&m_FmtCtx, m_FilePath.c_str(), NULL, NULL);
    if (ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("open file fail(%d):%s.\n", ret, errMsg);
        goto FAIL;
    }

    ret = avformat_find_stream_info(m_FmtCtx, NULL);
    if (ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("find stream info fail(%d):%s.\n", ret, errMsg);
        goto FAIL;
    }

    m_Duration = (m_FmtCtx->duration)/AV_TIME_BASE;

    for (int i = 0; i < m_FmtCtx->nb_streams; i++)
    {
        if (m_FmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            m_VideoTrack = i;
            break;
        }
    }

    if (m_VideoTrack < 0)
    {
        printf("cannot find video track.\n");
        goto FAIL;
    }

    return 0;

FAIL:
    m_Event = EVENT_THUMBERROR;
    m_Error = ERROR_OPENFAIL;
    return -1;
}

/*seek to pos & get urrent pos's I frame*/
int Thumbnail::SeekToPos()
{
    //defalut
    if (m_Pos <= 0)
    {
        /*
         *if duration > 3 min, m_Pos = 10 s
         *else m_Pos = duration * (10/100)  [10%]
        */
        if(m_Duration > 1000 * 60 * 3)
        {
            m_Pos = 10 * 1000;
        }else
        {
            m_Pos = m_Duration / 10;
        }
    }
    printf("seek to %d ms.\n", m_Pos);
    int ret = av_seek_frame(m_FmtCtx, -1, m_Pos * 1000/*us*/, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
    {
        printf("seek error.");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SEEKFAIL;
        return -1;
    }
    return 0;
}
/*decoder current frame.*/
int Thumbnail::DecoderFrame()
{
    AVCodecContext *pCodecCtx;
    const AVCodec  *pCodec;
    AVFrame        *pFrame;
    AVFrame        *pYuv;
    AVPacket       *pkg;

#if DUMP_YUV
    FILE           *fp_yuv;
#endif
    int ret = 0;
    char errMsg[512] = {0};
    bool isGot = false;

    if (m_FmtCtx == NULL || m_VideoTrack == -1)
    {
        goto FAIL;
    }
#if DUMP_YUV
    fp_yuv = fopen("output.yuv", "wb+");
    if(!fp_yuv)
    {
        printf("fopen fail.\n");
    }
#endif
    //find CodecCtx
    pCodecCtx = avcodec_alloc_context3(NULL);
    ret = avcodec_parameters_to_context(pCodecCtx, m_FmtCtx->streams[m_VideoTrack]->codecpar);
    if (ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("video decoder open fail(%d):%s.\n", ret, errMsg);
        goto FAIL;
    }

    pCodec = avcodec_find_decoder(m_FmtCtx->streams[m_VideoTrack]->codecpar->codec_id);
    if (!pCodec)
    {
        printf("cannot find video stream.\n");
        goto FAIL;
    }

    //step 6: open video decoder
    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("video decoder open fail(%d):%s.\n", ret, errMsg);
        goto FAIL;
    }

    //av_dump_format(m_FmtCtx, m_VideoTrack, m_FilePath.c_str(), 0);
    m_VideoWidth = pCodecCtx->width;
    m_VideoHeight = pCodecCtx->height;

    //calculate
    /*
     *1.w <= m_width && h <= m_height
    */
    if (m_VideoWidth <= m_width && m_VideoHeight <= m_height)
    {
        m_SwsWidth  = m_VideoWidth;
        m_SwsHeight = m_VideoHeight;
    }else if(m_VideoWidth <= m_width && m_VideoHeight > m_height)
    {
        m_SwsHeight = m_height;
        m_SwsWidth  = m_VideoWidth / (m_VideoHeight / m_height);
    }else if(m_VideoWidth > m_width && m_VideoHeight <= m_height)
    {
        m_SwsWidth  = m_width;
        m_SwsHeight = m_VideoHeight / (m_VideoWidth / m_width);
    }else{//m_VideoWidth > m_width && m_VideoHeight > m_height
        float cc_w = (double)m_VideoWidth / m_width;
        float cc_h = (double)m_VideoHeight / m_height;

        if(cc_w >= cc_h)
        {
            m_SwsWidth  = m_width;
            m_SwsHeight = m_VideoHeight / cc_w;
        }else
        {
            m_SwsHeight = m_height;
            m_SwsWidth  = m_VideoWidth / cc_h;
        }
    }

    printf("video:width = %d, height = %d.\n", m_VideoWidth, m_VideoHeight);
    printf("SWS:width = %d, height = %d.\n", m_SwsWidth, m_SwsHeight);

    while (true)
    {
        pkg = av_packet_alloc();
        printf("read a frame!!!!.\n");
        ret = av_read_frame(m_FmtCtx, pkg);
        if (ret < 0)
        {
            printf("av_read_frame fail.\n");
            goto CLOSE;
        }

        if(pkg->stream_index == m_VideoTrack)
        {
            printf("prepare to decode frame.\n");
            if (avcodec_send_packet(pCodecCtx, pkg) != 0)
            {
                printf("avcodec_send_packet fail!\n");
                goto CLOSE;
            }
            
            pFrame = av_frame_alloc();
            while (avcodec_receive_frame(pCodecCtx, pFrame) == 0)
            {
                printf("got data.\n");
                if(isGot == false)
                {
                    printf("save first frame.\n");
                    pYuv = av_frame_alloc();

                    int video_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_SwsWidth, m_SwsHeight, 1);
                    m_picture_buf = (uint8_t *)av_malloc(video_size);
                    av_image_fill_arrays(pYuv->data, pYuv->linesize, m_picture_buf, AV_PIX_FMT_YUV420P, m_SwsWidth, m_SwsHeight, 1);
                    //视频像素数据格式转换上下文
                    SwsContext *sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, /*INPUT  W H FMT*/
                                                        m_SwsWidth, m_SwsHeight, AV_PIX_FMT_YUV420P,              /*OUTPUT W H FMT*/
                                                        SWS_BICUBIC,                                              /*拉伸算法*/
                                                        NULL, NULL, NULL);                                        /*其他参数*/
                    //将AVFrame转成视频像素数YUV420P格式
                    sws_scale(sws_ctx, (const uint8_t * const *)pFrame->data, pFrame->linesize,
                                        0, pCodecCtx->height, 
                                        pYuv->data, pYuv->linesize);
#if DUMP_YUV
                    if(fp_yuv)
                    {
                        int y_size = m_width  * m_height;
                        //pFrame->data[0]Y
                        fwrite(pYuv->data[0], 1, y_size, fp_yuv);
                        //pFrame->data[1]U
                        fwrite(pYuv->data[1], 1, y_size/4, fp_yuv);
                        //pFrame->data[2]V
                        fwrite(pYuv->data[2], 1, y_size/4, fp_yuv);
                    }
#endif
                    sws_freeContext(sws_ctx);
                    av_frame_free(&pYuv);
                    isGot = true;
                }
            }
            av_frame_free(&pFrame);
        }
        av_packet_free(&pkg);
        if(isGot)
        {
            printf("we got data.\n");
            break;
        }
    }

#if DUMP_YUV
    if(fp_yuv)
        fclose(fp_yuv);
#endif

    avcodec_close(pCodecCtx);
    avcodec_free_context(&pCodecCtx);

    return 0;

CLOSE:
    avcodec_close(pCodecCtx);

FAIL:
    if (pkg)
    {
        av_packet_free(&pkg);
    }

    if (pFrame)
    {
        av_frame_free(&pFrame);
    }

    if (pYuv)
    {
        av_frame_free(&pYuv);
    }

    if(pCodecCtx)
    {
        avcodec_free_context(&pCodecCtx);
    }
    m_Event = EVENT_THUMBERROR;
    m_Error = ERROR_DECODERFAIL;
    return -1;
}
/*edit yuv data*/
int Thumbnail::EditYvuData()
{
    // int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_width, m_height, 1);
    // m_picture_buf = (uint8_t*)av_malloc(size);

    // int y_size = m_yuv420p->width * m_yuv420p->height;
    // memcpy(m_picture_buf, m_yuv420p.data[0], )

    return 0;
}
/*save yuv to jpeg*/
int Thumbnail::SavePicture()
{
    std::string     pic_file_name = "test.jpg";
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    // 设置输出文件格式
    pFormatCtx->oformat = av_guess_format("mjpeg", NULL, NULL);
    if (avio_open(&pFormatCtx->pb, pic_file_name.c_str(), AVIO_FLAG_READ_WRITE) < 0)
    {
        printf("Couldn't open output file.");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    // 构建一个新stream
    AVStream *pAVStream = avformat_new_stream(pFormatCtx, 0);
    if (pAVStream == NULL)
    {
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    AVCodecContext *pCodeCtx;
    pAVStream->codecpar->codec_id = pFormatCtx->oformat->video_codec;
    pAVStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    pAVStream->codecpar->format = AV_PIX_FMT_YUVJ420P;
    pAVStream->codecpar->width = m_SwsWidth;
    pAVStream->codecpar->height = m_SwsHeight;

    pCodeCtx = avcodec_alloc_context3(NULL);
    if (!pCodeCtx)
    {
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        fprintf(stderr, "Could not allocate video codec context\n");
        return -1;
    }

    if ((avcodec_parameters_to_context(pCodeCtx, pAVStream->codecpar)) < 0)
    {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }
    pCodeCtx->time_base = (AVRational) {1, 25};
    const AVCodec *pCodec = avcodec_find_encoder(pCodeCtx->codec_id);

    if (!pCodec)
    {
        printf("Could not find encoder\n");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    if (avcodec_open2(pCodeCtx, pCodec, NULL) < 0)
    {
        printf("Could not open encodec.");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    AVFrame *picture = av_frame_alloc();
    picture->width = pCodeCtx->width;
    picture->height = pCodeCtx->height;
    picture->format = AV_PIX_FMT_YUV420P;

    av_image_fill_arrays(picture->data, picture->linesize, m_picture_buf, pCodeCtx->pix_fmt, pCodeCtx->width, pCodeCtx->height, 1);

    int ret = avformat_write_header(pFormatCtx, NULL);
    if (ret < 0)
    {
        printf("write_header fail\n");
        return -1;
    }

    //Encode
    AVPacket *pkt = av_packet_alloc();

    char errMsg[512] = {0};

    // 编码数据
    ret = avcodec_send_frame(pCodeCtx, picture);
    if (ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("Could not avcodec_send_frame(%d):%s.\n", ret, errMsg);
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    // 得到编码后数据
    ret = avcodec_receive_packet(pCodeCtx, pkt);
    if (ret < 0)
    {
        printf("Could not avcodec_receive_packet\n");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    ret = av_write_frame(pFormatCtx, pkt);
    if (ret < 0)
    {
        printf("Could not av_write_frame\n");
        m_Event = EVENT_THUMBERROR;
        m_Error = ERROR_SAVEFAIL;
        return -1;
    }

    //Write Trailer
    av_write_trailer(pFormatCtx);

    av_packet_free(&pkt);
    avcodec_close(pCodeCtx);
    avio_close(pFormatCtx->pb);
    avformat_free_context(pFormatCtx);

    //ok
    m_Event = EVENT_THUMBDONE;
    m_Error = ERROR_UNKNOWN;

    return 0;
}