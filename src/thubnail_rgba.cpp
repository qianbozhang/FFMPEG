#include "thumbnail.h"
#include "CountTime.h"

#include <stdio.h>
#include <string>
#include <string.h>
#include <sstream>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libavutil/display.h"
#include "libswscale/swscale.h"
};

//Y
#undef BLACK
#define BLACK  0
//UV
#undef TRANSPARENCY
#define TRANSPARENCY  128

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
    m_Rotate      = 0;
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
    if(m_picture_buf2)
    {
        av_free(m_picture_buf2);
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
    ret = EditData();
    if (ret < 0)
    {
        return -1;
    }

    // CHECKEQUAL(m_Stop, true);
    ret = SavePicture("thumbnail", m_picture_buf2, m_width, m_height);
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
    //count
    CountTime ct("OPenFile");

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

    m_Duration = (m_FmtCtx->duration)/AV_TIME_BASE;
    printf("duration:%d s.\n", m_Duration);

    return 0;

FAIL:
    m_Event = EVENT_THUMBERROR;
    m_Error = ERROR_OPENFAIL;
    return -1;
}

/*seek to pos & get urrent pos's I frame*/
int Thumbnail::SeekToPos()
{
    //count
    CountTime ct("SeekToPos");
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
    //count
    CountTime ct("DecoderFrame");

    AVCodecContext *pCodecCtx;
    const AVCodec  *pCodec;
    AVFrame        *pFrame;
    AVPacket       *pkg;
    int32_t        *displaymatrix;

    int ret = 0;
    char errMsg[512] = {0};
    bool isGot = false;

    if (m_FmtCtx == NULL || m_VideoTrack == -1)
    {
        goto FAIL;
    }
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

    m_VideoTrack = av_find_best_stream(m_FmtCtx, AVMEDIA_TYPE_VIDEO, -1,-1, NULL, 0);
    if(m_VideoTrack < 0)
    {
        goto FAIL;
    }

    displaymatrix = (int32_t *)av_stream_get_side_data(m_FmtCtx->streams[m_VideoTrack], AV_PKT_DATA_DISPLAYMATRIX, NULL);
    if(displaymatrix)
    {
        int rotate = av_display_rotation_get((int32_t*) displaymatrix);
        if(rotate == -90) //It needs to be rotated 90 degrees clockwise
        {
            m_Rotate = 90;
        }else if(rotate == -180) //clockwise 180
        {
            m_Rotate = 180;
        }else if(rotate == 90) //clockwise 270
        {
            m_Rotate = 270;
        }
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
    m_Duration = m_FmtCtx->duration / 1000;
    m_VideoWidth = pCodecCtx->width;
    m_VideoHeight = pCodecCtx->height;
    printf("videoinfo:width:%d, height:%d, duration:%lld, need rotated %d degrees clockwise.\n",
        m_VideoWidth, m_VideoHeight, m_Duration, m_Rotate);

    while (true)
    {
        pkg = av_packet_alloc();
        //printf("read a frame!!!!.\n");
        ret = av_read_frame(m_FmtCtx, pkg);
        if (ret < 0)
        {
            printf("av_read_frame fail.\n");
            goto CLOSE;
        }

        if(pkg->stream_index == m_VideoTrack)
        {
            //printf("prepare to decode frame.\n");
            if (avcodec_send_packet(pCodecCtx, pkg) != 0)
            {
                printf("avcodec_send_packet fail!\n");
                goto CLOSE;
            }
            
            pFrame = av_frame_alloc();
            while (avcodec_receive_frame(pCodecCtx, pFrame) == 0)
            {
                if(isGot == false)
                {
                    //convert
                    AVFrame* rgbaF = av_frame_alloc();
                    ConvertFmtToRGBA(pFrame, rgbaF);

                    //need rotate first.
                    if(m_Rotate != 0)
                    {
                        int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_VideoWidth, m_VideoHeight, 1);
                        uint8_t* outBuf = (uint8_t*)av_malloc(size);

                        switch(m_Rotate)
                        {
                            case 90:
                            {
                                Rotate_90(m_VideoWidth, m_VideoHeight, rgbaF->data[0], outBuf);
                                memset(rgbaF->data[0], 0, size);
                                memcpy(rgbaF->data[0], outBuf, size);
                                rgbaF->width = m_VideoHeight;
                                rgbaF->height = m_VideoWidth;
                                rgbaF->linesize[0] = rgbaF->width * 4;
                                break;
                            }
                            case 180:
                            {
                                Rotate_180(m_VideoWidth, m_VideoHeight, rgbaF->data[0], outBuf);
                                memset(rgbaF->data[0], 0, size);
                                memcpy(rgbaF->data[0], outBuf, size);
                                rgbaF->width = m_VideoWidth;
                                rgbaF->height = m_VideoHeight;
                                rgbaF->linesize[0] = rgbaF->width * 4;
                                break;
                            }
                            case 270:
                            {
                                Rotate_270(m_VideoWidth, m_VideoHeight, rgbaF->data[0], outBuf);
                                memset(rgbaF->data[0], 0, size);
                                memcpy(rgbaF->data[0], outBuf, size);
                                rgbaF->width = m_VideoHeight;
                                rgbaF->height = m_VideoWidth;
                                rgbaF->linesize[0] = rgbaF->width * 4;
                                break;
                            }
                        }
                        av_free(outBuf);

                        //update
                        m_VideoWidth = rgbaF->width;
                        m_VideoHeight = rgbaF->height;
                    }

                    //calculate
                    if (m_VideoWidth <= m_width && m_VideoHeight <= m_height)
                    {
                        m_SwsWidth  = m_VideoWidth;
                        m_SwsHeight = m_VideoHeight;
                    }else if(m_VideoWidth <= m_width && m_VideoHeight > m_height)
                    {
                        float cc_h = (double)m_VideoHeight / m_height;
                        m_SwsHeight = m_height;
                        m_SwsWidth  = m_VideoWidth / cc_h;
                    }else if(m_VideoWidth > m_width && m_VideoHeight <= m_height)
                    {
                        float cc_w = (double)m_VideoWidth / m_width;
                        m_SwsWidth  = m_width;
                        m_SwsHeight = m_VideoHeight / cc_w;
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
                    printf("user:width = %d, height = %d.\n", m_width, m_height);
                    printf("SWS:width = %d, height = %d.\n", m_SwsWidth, m_SwsHeight);

                    //scale
                    m_picture_buf = (uint8_t*)av_malloc(m_SwsWidth * m_SwsHeight * 4);
                    ScaleFrame(rgbaF, m_SwsWidth, m_SwsHeight, m_picture_buf);
                    // SavePicture("m_picture_buf", m_picture_buf, m_SwsWidth, m_SwsHeight);
                    //free
                    av_frame_free(&rgbaF);
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

    if(pCodecCtx)
    {
        avcodec_free_context(&pCodecCtx);
    }
    m_Event = EVENT_THUMBERROR;
    m_Error = ERROR_DECODERFAIL;
    return -1;
}

bool Thumbnail::WidthAndHeight_Equal()
{
    if(!m_picture_buf || !m_picture_buf2)
    {
        return false;
    }
    //copy 
    memcpy(m_picture_buf2, m_picture_buf, m_width * m_height * 4);
    return true;
}


bool Thumbnail::CopyUnsignedChar(unsigned char *dest, int &pos, unsigned char *src, int start, int len)
{
    if(start < 0 || len < 0)
    {
        printf("invalue.\n");
        return false;
    }
    for(int i = start; i < start + len; i ++)
    {
        dest[pos ++] = src[i];
    }
    return true;
}
/**
 * -------------
 * |    top    |
 * -------------
 * -------------
 * |    pic    |
 * -------------
 * -------------
 * |   buttom  |
 * -------------
 * 
 */ 
bool Thumbnail::Width_Equal()
{
    if(!m_picture_buf || !m_picture_buf2)
    {
        return false;
    }
    //calculate
    if(m_SwsWidth == m_width && m_SwsHeight < m_height)
    {
        int diff_h       = m_height - m_SwsHeight;
        int diff_top     = diff_h/2;
        int diff_buttom  = diff_h - diff_top;

        printf("diff_top:%d, diff_buttom:%d.\n", diff_top, diff_buttom);

        int pos = 0;
        //create top
        for(int i = 0; i < diff_top; i++)
        {
            pos = i * m_width * 4;
            for(int j  = 0; j < m_width; j++)
            {
                m_picture_buf2[pos + j * 4 + 0] = 235;//R
                m_picture_buf2[pos + j * 4 + 1] = 142;//G
                m_picture_buf2[pos + j * 4 + 2] = 85;//B
                m_picture_buf2[pos + j * 4 + 3] = 0;//A 0:完全透明  ===>>  1：完全不透明
            }
        }
        //original buf
        memcpy(m_picture_buf2 + diff_top * m_width * 4, m_picture_buf, m_SwsWidth * m_SwsHeight * 4);

        //create buttom
        for(int i = diff_top + m_SwsHeight; i < m_height; i++)
        {
            pos = i * m_width * 4;
            for(int j = 0; j < m_width; j++)
            {
                m_picture_buf2[pos + j * 4 + 0] = 235;//R
                m_picture_buf2[pos + j * 4 + 1] = 142;//G
                m_picture_buf2[pos + j * 4 + 2] = 85;//B
                m_picture_buf2[pos + j * 4 + 3] = 0;//A 0:完全透明  ===>>  1：完全不透明
            }
        }
    }
    else
    {
        //do nothing
        return false;
    }
    return true;
}

/**
 * |--left--||--pic--||--right--|
 */ 
bool Thumbnail::Height_Equal()
{
    if(!m_picture_buf || !m_picture_buf2)
    {
        return false;
    }

    if(m_width > m_SwsWidth && m_height == m_SwsHeight)
    {
        int diff_w      = m_width - m_SwsWidth;
        int diff_left   = diff_w/2;
        int diff_right  = diff_w - diff_left;
        int pos         = 0;
        int pos_line    = 0;

        for(int i = 0; i < m_height; i++)
        {
            pos_line = i * m_width * 4;
            for(int j = 0; j < m_width; j++)
            {
                if(j < diff_left || j >= (diff_left + m_SwsWidth))
                {
                    m_picture_buf2[pos_line + j * 4 + 0] = 235; //R
                    m_picture_buf2[pos_line + j * 4 + 1] = 142; //G
                    m_picture_buf2[pos_line + j * 4 + 2] = 85;  //B
                    m_picture_buf2[pos_line + j * 4 + 3] = 0;   //A
                }else
                {
                    m_picture_buf2[pos_line + j * 4 + 0] = m_picture_buf[pos ++];//R
                    m_picture_buf2[pos_line + j * 4 + 1] = m_picture_buf[pos ++];//G
                    m_picture_buf2[pos_line + j * 4 + 2] = m_picture_buf[pos ++];//B
                    m_picture_buf2[pos_line + j * 4 + 3] = m_picture_buf[pos ++];//A
                }
            }
        }
    }
    return true;
}

/**
 * |----------------------top-----------------------|
 * |--left--||------------pic------------||--right--|
 * |-------------------buttom-----------------------|
 */ 
bool Thumbnail::WidthAndHeight_NoEqual()
{
    if(!m_picture_buf || !m_picture_buf2)
    {
        return false;
    }

    if(m_width > m_SwsWidth && m_height > m_SwsHeight)
    {   
        //top & buttom 's height
        int diff_h      = m_height - m_SwsHeight;
        int diff_top    = diff_h/2;
        int diff_buttom = diff_h - diff_top;
        //left & right 's width
        int diff_w      = m_width - m_SwsWidth;
        int diff_left   = diff_w/2;
        int diff_right  = diff_w - diff_left;
        int pos         = 0;
        int pos_line    = 0;

        for(int i = 0; i < m_height; i++)
        {
            pos_line = i * m_width * 4;
            for(int j = 0; j < m_width; j++)
            {
                if(j < diff_left || j >= (diff_left + m_SwsWidth) || i < diff_top || i >= (diff_top + m_SwsHeight))
                {
                    m_picture_buf2[pos_line + j * 4 + 0] = 235; //R
                    m_picture_buf2[pos_line + j * 4 + 1] = 142; //G
                    m_picture_buf2[pos_line + j * 4 + 2] = 85;  //B
                    m_picture_buf2[pos_line + j * 4 + 3] = 0;   //A
                }else
                {
                    m_picture_buf2[pos_line + j * 4 + 0] = m_picture_buf[pos ++];//R
                    m_picture_buf2[pos_line + j * 4 + 1] = m_picture_buf[pos ++];//G
                    m_picture_buf2[pos_line + j * 4 + 2] = m_picture_buf[pos ++];//B
                    m_picture_buf2[pos_line + j * 4 + 3] = m_picture_buf[pos ++];//A
                }
            }
        }
    }
    return true;
}

void Thumbnail::Rotate_90(int iw, int ih, uint8_t* inbuf, uint8_t* &outBuf)
{
    if(!outBuf)
    {
        printf("invalid outbuf.\n");
        return;
    }
    int pos = 0;
    //for 90
    for(int col = ih - 1; col >= 0; col --)
    {
        for(int row = 0; row < iw; row ++ )
        {
            outBuf[row * ih * 4 + col * 4 + 0] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 1] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 2] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 3] = inbuf[pos ++];
        }
    }

    //SavePicture("Rotate_90", outBuf, ih, iw);
}

void Thumbnail::Rotate_180(int iw, int ih, uint8_t* inbuf, uint8_t* &outBuf)
{
    if(!outBuf)
    {
        printf("invalid outbuf.\n");
        return;
    }
    int pos = 0;

    for(int row = ih - 1; row >= 0; row --)
    {
        for(int col = iw - 1; col >= 0; col --)
        {
            outBuf[row * iw * 4 + col * 4 + 0] = inbuf[pos ++];
            outBuf[row * iw * 4 + col * 4 + 1] = inbuf[pos ++];
            outBuf[row * iw * 4 + col * 4 + 2] = inbuf[pos ++];
            outBuf[row * iw * 4 + col * 4 + 3] = inbuf[pos ++];
        }
    }
    //SavePicture("Rotate_180", outBuf, iw, ih);
}

void Thumbnail::Rotate_270(int iw, int ih, uint8_t* inbuf, uint8_t* &outBuf)
{
    if(!outBuf)
    {
        printf("invalid outbuf.\n");
        return;
    }
    int pos = 0;
    for(int col = 0; col < ih; col ++)
    {
        for(int row = iw - 1; row >= 0; row --)
        {
            outBuf[row * ih * 4 + col * 4 + 0] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 1] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 2] = inbuf[pos ++];
            outBuf[row * ih * 4 + col * 4 + 3] = inbuf[pos ++];
        }
    }
    // SavePicture("Rotate_270", outBuf, ih, iw);
}

/**
 * convert to rgba
 */ 
bool Thumbnail::ConvertFmtToRGBA(AVFrame* inFrame, AVFrame* outFrame)
{
    //fill outframe
    if(av_image_alloc(outFrame->data, outFrame->linesize,inFrame->width, inFrame->height, AV_PIX_FMT_RGBA, 1) < 0)
    {
        printf("av_image_alloc fail.\n");
        return false;
    }
    outFrame->width = inFrame->width;
    outFrame->height = inFrame->height;
    outFrame->format = AV_PIX_FMT_RGBA;

    //convert
    SwsContext *sws_ctx = sws_getContext(inFrame->width, inFrame->height, (AVPixelFormat)inFrame->format,   /*INPUT  W H FMT*/
                                         inFrame->width, inFrame->height, AV_PIX_FMT_RGBA,                 /*OUTPUT W H FMT*/
                                         SWS_BICUBIC, NULL, NULL, NULL);          /*default*/
    if (!sws_ctx) {
        printf("sws_getContext fail.\n");
        return false;
    }
    sws_scale(sws_ctx, (const uint8_t * const *)inFrame->data, inFrame->linesize, 0, inFrame->height, 
                                        outFrame->data, outFrame->linesize);
    //free
    sws_freeContext(sws_ctx);
    return true;
}

/**
 * scale
 */ 
bool Thumbnail::ScaleFrame(AVFrame* inFrame, int scaleW, int scaleH, uint8_t* &outBuf)
{
    if(!outBuf)
    {
        printf("invalid outbuf!!!.\n");
        return false;
    }
    printf("w:%d, h:%d, fmt:%d.\n", inFrame->width, inFrame->height, inFrame->format);
    //tmp frame
    AVFrame *tmp = av_frame_alloc();
    //fill tmp
    if(av_image_fill_arrays(tmp->data, tmp->linesize, outBuf, AV_PIX_FMT_RGBA, scaleW, scaleH, 1) < 0)
    {
        printf("fill array fail.\n");
        return false;
    }
    //convert
    SwsContext *sws_ctx = sws_getContext(inFrame->width, inFrame->height, (AVPixelFormat)inFrame->format,   /*INPUT  W H FMT*/
                                         scaleW, scaleH, AV_PIX_FMT_RGBA,                 /*OUTPUT W H FMT*/
                                         SWS_BICUBIC, NULL, NULL, NULL);          /*default*/
    if (!sws_ctx) {
        printf("sws_getContext fail.\n");
        return false;
    }
    sws_scale(sws_ctx, (const uint8_t * const *)inFrame->data, inFrame->linesize, 0, inFrame->height, 
                                        tmp->data, tmp->linesize);
    //free
    av_frame_free(&tmp);
    sws_freeContext(sws_ctx);

    return true;
}

/* get rotate */
int Thumbnail::getRotateFromVideoStream(AVDictionary *metadata)
{
    int rotate = -1;
    AVDictionaryEntry *tag = NULL;
    tag = av_dict_get(metadata, "rotate", tag, 0);
    if (tag == NULL)
    {
        printf("tag == NULL\n");
        rotate = 0;
    }
    else
    {
        int angle = atoi(tag->value);
        printf("angle == %d\n", angle);
        angle %= 360;
        if (angle == 90)
        {
            rotate = 90;
        }
        else if (angle == 180)
        {
            rotate = 180;
        }
        else if (angle == 270)
        {
            rotate = 270;
        }
        else
        {
            rotate = 0;
        }
    }

    return rotate;
}

/*edit yuv data*/
int Thumbnail::EditData()
{
    CountTime ct("EditrgbaData");
    int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_width, m_height, 1);
    m_picture_buf2 = (uint8_t*)av_malloc(size);

    bool isOk = false;
    if(m_width == m_SwsWidth && m_height == m_SwsHeight)
    {
        isOk = WidthAndHeight_Equal();
    }else if(m_width == m_SwsWidth && m_height > m_SwsHeight)
    {
        isOk = Width_Equal();
    }else if(m_width > m_SwsWidth && m_height == m_SwsHeight)
    {
        isOk = Height_Equal();
    }else
    {
        isOk = WidthAndHeight_NoEqual();
    }

    return 0;
}
/*save yuv to jpeg*/
int Thumbnail::SavePicture(const std::string thumbUrl, const unsigned char *buf, int w, int h)
{
    char name[128] = "";
    snprintf(name, sizeof(name), "%s_%dx%d.rgba", thumbUrl.c_str(), w, h);
    printf("save rgba to %s.\n", name);

    FILE *fp = fopen(name, "wb+");

    fwrite(buf, 1, w * h * 4, fp);

    fclose(fp);

    return 0;
}