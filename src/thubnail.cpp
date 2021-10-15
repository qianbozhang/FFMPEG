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

//Y
#undef BLACK
#define BLACK  0
//UV
#undef TRANSPARENCY
#define TRANSPARENCY  128

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
    ret = EditYvuData();
    if (ret < 0)
    {
        return -1;
    }

    CHECKEQUAL(m_Stop, true);
    ret = SavePicture("pic.jpg", m_picture_buf2, m_width, m_height);
    ret = SavePicture("original_pic.jpg", m_picture_buf, m_SwsWidth, m_SwsHeight);
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
                //printf("got data.\n");
                if(isGot == false)
                {
                    //printf("save first frame.\n");
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

bool Thumbnail::WidthAndHeight_Equal()
{
    if(!m_picture_buf || !m_picture_buf2)
    {
        return false;
    }
    //copy 
    memcpy(m_picture_buf2, m_picture_buf, m_width * m_height * 3 / 2);
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
#if 1
        int diff_h     = m_height - m_SwsHeight;
        int diff_t     = diff_h/2;
        //Make sure it's a multiple of 2
        if(diff_t % 2 != 0)
        {
            diff_t += 1;  
        }
        int diff_b     = diff_h - diff_t;
        if(diff_b % 2 != 0)
        {
            diff_b -= 1;  
        }
        int uv_h_t     = diff_t/2;
        int uv_h_b     = diff_b/2;

        //top
        int top_s    = m_width * diff_t;
        int top_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_width, diff_t, 1);
        uint8_t *top = (uint8_t*)av_malloc(top_size);
        memset(top, BLACK, m_width * diff_t);
        memset(top + m_width * diff_t, TRANSPARENCY, m_width * diff_t / 2);

        //buttom
        int buttom_s    = m_width * diff_b;
        int buttom_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_width, diff_b, 1);
        uint8_t *buttom = (uint8_t*)av_malloc(buttom_size);
        memset(buttom, BLACK, m_width * diff_b);
        memset(buttom + m_width * diff_b, TRANSPARENCY, m_width * diff_b / 2);

        //pic
        int pic_s = m_width * m_SwsHeight;

        int pos = 0;
        //Y
        //top
        CopyUnsignedChar(m_picture_buf2, pos, top, 0, top_s);
        //pic
        CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, 0, pic_s);
        //buttom
        CopyUnsignedChar(m_picture_buf2, pos, buttom, 0, buttom_s);
        printf("pos = %d, total:%d.\n", pos, m_width * m_height);

        //U
        pos = m_width * m_height;
        //top
        CopyUnsignedChar(m_picture_buf2, pos, top, top_s, top_s/4);
        printf("pos = %d.\n", pos);
        //pic
        CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, pic_s, pic_s/4);
        //buttom
        CopyUnsignedChar(m_picture_buf2, pos, buttom, buttom_s, buttom_s/4);
        printf("pos = %d, total:%d.\n", pos, m_width * m_height * 5/4);

        //V
        pos = m_width * m_height * 5/4;
        //top
        CopyUnsignedChar(m_picture_buf2, pos, top, top_s * 5 / 4, top_s/4);
        //pic
        CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, pic_s * 5 / 4, pic_s/4);
        //buttom
        CopyUnsignedChar(m_picture_buf2, pos, buttom, buttom_s * 5 / 4, buttom_s/4);
        printf("pos = %d, total:%d.\n", pos, m_width * m_height * 3/2);

        av_free(top);
        av_free(buttom);

        //test
        // for(int i = m_width * m_height * 5/4; i < m_width * m_height * 3/2; i ++)
        // {
        //     m_picture_buf2[i] = 128;
        // }
#else 
        int diff_h        = m_height - m_SwsHeight;
        int diff_h_top    = diff_h % 2 == 0 ? diff_h/2 : diff_h/2 +1;
        int diff_h_buttom = m_SwsHeight + diff_h_top;
        int uv_w          = m_width / 2;
        int uv_h          = m_height / 2;
        int uv_diff_top   = diff_h % 4 == 0 ? diff_h/4 : diff_h/4 +1;
        int uv_diff_buttom = m_SwsHeight / 2 + uv_diff_top;
        int row            = 0;//m_picture_buf flag

        //for Y
        row = 0;
        uint8_t* data_y = (uint8_t*)av_malloc(m_width * m_height);
        for(int i = 0; i < m_height ; i++)
        {
            for(int j = 0; j < m_width; j ++)
            {
                if(i < diff_h_top || i > diff_h_buttom)
                {
                    data_y[i * m_width + j] = BLACK;
                }
                else
                {
                    data_y[i * m_width + j] = m_picture_buf[row ++];
                }
            }
        }

        //for U
        uint8_t* data_u = (uint8_t*)av_malloc(uv_w * uv_h);
        row = m_SwsWidth * m_SwsHeight;
        for(int i = 0; i < uv_h; i ++)
        {
            for(int j = 0; j < uv_w; j ++)
            {
                if(i < uv_diff_top || i > uv_diff_buttom)
                {
                    data_u[i * uv_w + j] = TRANSPARENCY;
                }
                else
                {
                    static bool p = true;
                    if(p)
                    {
                        printf("pos = %d.\n", m_width * m_height + i * uv_w + j);
                        p =false;
                    }
                    data_u[i * uv_w + j] = m_picture_buf[row ++];
                }
            }
        }
        // //for V
        uint8_t* data_v = (uint8_t*)av_malloc(uv_w * uv_h);
        row = m_SwsWidth * m_SwsHeight * 5 / 4;
        for(int i = 0; i < uv_h; i ++)
        {
            for(int j = 0; j < uv_w; j ++)
            {
                if(i < uv_diff_top || i > uv_diff_buttom)
                {
                    data_v[i * uv_w + j] = TRANSPARENCY;
                }
                else
                {
                    data_v[i * uv_w + j] = m_picture_buf[row ++];
                }
            }
        }
        //copy to m_picture_buf2
        memcpy(m_picture_buf2, data_y, m_width * m_height);
        memcpy(m_picture_buf2 + m_width * m_height, data_u, m_width * m_height / 4);
        memcpy(m_picture_buf2 + m_width * m_height * 5 / 4, data_v, m_width * m_height / 4);
        //free
        av_free(data_y);
        av_free(data_u);
        av_free(data_v);
#endif
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
        int diff_w    = m_width - m_SwsWidth;
        int diff_l    = diff_w/2;
        int diff_r    = diff_w - diff_l;

        //left
        int left_uv_w = diff_l/2;
        int left_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, diff_l, m_height, 1);
        uint8_t *left = (uint8_t*)av_malloc(left_size);
        memset(left, BLACK, diff_l * m_height);
        memset(left + diff_l * m_height, TRANSPARENCY, diff_l * m_height / 2);

        //right
        int right_uv_w = diff_r/2;
        int right_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, diff_r, m_height, 1);
        uint8_t *right = (uint8_t*)av_malloc(right_size);
        memset(right, BLACK, diff_r * m_height);
        memset(right + diff_r * m_height, TRANSPARENCY, diff_r * m_height / 2);

        //combination:left==>>m_picture==>>right
        //Y
        int pos = 0;
        for(int i = 0; i < m_height; i ++)
        {
            //left
            CopyUnsignedChar(m_picture_buf2, pos, left, i * diff_l, diff_l);
            //m_picture_buf
            CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, i * m_SwsWidth, m_SwsWidth);
            //right
            CopyUnsignedChar(m_picture_buf2, pos, right, i * diff_r, diff_r);
        }

        printf("pos = %d, total:%d.\n", pos, m_width * m_height);

        //U
        for(int i = 0; i < m_height / 2; i ++)
        {
            //left
            CopyUnsignedChar(m_picture_buf2, pos, left, i * left_uv_w + diff_l * m_height, left_uv_w);
            //m_picture_buf
            CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, i * m_SwsWidth/2 + m_SwsWidth * m_height, m_SwsWidth/2);
            //right
            CopyUnsignedChar(m_picture_buf2, pos, right, i * right_uv_w + diff_r * m_height, right_uv_w);
        }
        printf("pos = %d, total:%d.\n", pos, m_width * m_height * 5 / 4);
        //V
        for(int i = 0; i < m_height / 2; i ++)
        {
            //left
            CopyUnsignedChar(m_picture_buf2, pos, left, i * left_uv_w + diff_l * m_height * 5 / 4, left_uv_w);
            //m_picture_buf
            CopyUnsignedChar(m_picture_buf2, pos, m_picture_buf, i * m_SwsWidth/2 + m_SwsWidth * m_height * 5 / 4, m_SwsWidth/2);
            //right
            CopyUnsignedChar(m_picture_buf2, pos, right, i * right_uv_w + diff_r * m_height * 5 / 4, right_uv_w);
        }
        printf("pos = %d, total:%d.\n", pos, m_width * m_height * 3 / 2);

        av_free(left);
        av_free(right);
#if 0
        int diff_w         = m_width - m_SwsWidth;
        int uv_diff_w      = diff_w/2;
        int harf_diff_w    = diff_w/2;
        int harf_uv_diff_w = uv_diff_w/2;
        int uv_w           = m_width/2;
        int uv_h           = m_height/2;
        int row            = 0;//buf flag

        //for Y
        uint8_t* data_y = (uint8_t*)av_malloc(m_width * m_height);
        row = 0;
        for(int i = 0; i < m_height; i ++)
        {
            for(int j = 0; j < m_width; j++)
            {
                if( j < harf_diff_w || j > (harf_diff_w + m_SwsWidth))
                {
                    data_y[i * m_width + j] = BLACK;
                }else
                {
                    data_y[i * m_width + j] = m_picture_buf[row ++];
                }
            }
        }

        printf("row:%d,  total:%d.\n", row, m_SwsWidth * m_SwsHeight);

        //for U
        row = m_SwsWidth * m_SwsHeight;
        uint8_t *data_u = (uint8_t*)av_malloc(uv_w * uv_h);
        for(int i = 0; i < uv_h; i ++)
        {
            for(int j = 0 ; j < uv_w; j ++)
            {
                if(j < harf_uv_diff_w || j > (harf_uv_diff_w + m_SwsWidth/2))
                {
                    data_u[i * uv_w + j] = TRANSPARENCY;
                }else
                {
                    data_u[i * uv_w + j] = m_picture_buf[row ++];
                }
            }
        }

        //for V
        row = m_SwsWidth * m_height * 5 / 4;
        uint8_t *data_v = (uint8_t*)av_malloc(uv_w * uv_h);
        for(int i = 0; i < uv_h; i ++)
        {
            for(int j = 0 ; j < uv_w; j ++)
            {
                if(j < harf_uv_diff_w || j > (harf_uv_diff_w + m_SwsWidth/2))
                {
                    data_v[i * uv_w + j] = TRANSPARENCY;
                }else
                {
                    data_v[i * uv_w + j] = m_picture_buf[row ++];
                }
            }
        }

        //copy to m_picture_buf2
        memcpy(m_picture_buf2, data_y, m_width * m_height);
        memcpy(m_picture_buf2 + m_width * m_height, data_u, m_width * m_height / 4);
        memcpy(m_picture_buf2 + m_width * m_height * 5 / 4, data_v, m_width * m_height / 4);
        //free
        av_free(data_y);
        av_free(data_u);
        av_free(data_v);
#endif
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
        int diff_top    = diff_h % 4 == 0 ? diff_h/2 : diff_h/2 + 1;
        int diff_buttom = diff_h - diff_top;
        //left & right 's width
        int diff_w        = m_width - m_SwsWidth;
        int diff_left_w   = diff_w % 4 == 0 ? diff_w/2 : diff_w/2 + 1;
        int diff_right_w  = diff_w - diff_left_w;
        //left & right 's height
        int diff_left_h = m_SwsHeight;
        int diff_right_h = m_SwsHeight;

        int row = 0;//pic_buf flag

        //for Y
        uint8_t* data_y = (uint8_t*)av_malloc(m_width * m_height);
        row = 0;
        for(int i = 0; i < m_height; i ++)
        {
            for(int j = 0; j < m_width; j++)
            {
                if( j < diff_left_w || j >= (diff_left_w + m_SwsWidth) || i < diff_top || i >= (diff_top + m_SwsHeight))
                {
                    data_y[i * m_width + j] = BLACK;
                }else
                {
                    data_y[i * m_width + j] = m_picture_buf[row ++];
                }
            }
        }

        //for U
        row = m_SwsWidth * m_SwsHeight;
        uint8_t *data_u = (uint8_t*)av_malloc(m_width * m_height/4);
        for(int i = 0; i < m_height/2; i ++)
        {
            for(int j = 0 ; j < m_width/2; j ++)
            {
                if(j < diff_left_w/2 || j >= (diff_left_w/2 + m_SwsWidth/2) || i < diff_top/2 || i >= (diff_top/2 + m_SwsHeight/2))
                {
                    data_u[i * m_width/2 + j] = TRANSPARENCY;
                }else
                {
                    data_u[i * m_width/2 + j] = m_picture_buf[row ++];
                }
            }
        }

        //for V
        row = m_SwsWidth * m_SwsHeight * 5 / 4;
        uint8_t *data_v = (uint8_t*)av_malloc(m_width * m_height/4);
        for(int i = 0; i < m_height/2; i ++)
        {
            for(int j = 0 ; j < m_width/2; j ++)
            {
                if(j < diff_left_w/2 || j >= (diff_left_w/2 + m_SwsWidth/2) || i < diff_top/2 || i >= (diff_top/2 + m_SwsHeight/2))
                {
                    data_v[i * m_width/2 + j] = TRANSPARENCY;
                }else
                {
                    data_v[i * m_width/2 + j] = m_picture_buf[row ++];
                }
            }
        }

        //copy to m_picture_buf2
        memcpy(m_picture_buf2, data_y, m_width * m_height);
        memcpy(m_picture_buf2 + m_width * m_height, data_u, m_width * m_height / 4);
        memcpy(m_picture_buf2 + m_width * m_height * 5 / 4, data_v, m_width * m_height / 4);
        //free
        av_free(data_y);
        av_free(data_u);
        av_free(data_v);
    }
    return true;
}

/*edit yuv data*/
int Thumbnail::EditYvuData()
{
    int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_width, m_height, 1);
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
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    // 设置输出文件格式
    pFormatCtx->oformat = av_guess_format("mjpeg", NULL, NULL);
    if (avio_open(&pFormatCtx->pb, thumbUrl.c_str(), AVIO_FLAG_READ_WRITE) < 0)
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
    pAVStream->codecpar->width = w;
    pAVStream->codecpar->height = h;

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

    av_image_fill_arrays(picture->data, picture->linesize, buf, pCodeCtx->pix_fmt, pCodeCtx->width, pCodeCtx->height, 1);

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