/**
 * get yuv from video & save to output.yuv.
 * yuv数据完整性验证:ffplay -video_size wxh -i output.yuv
 **/
#include <stdio.h>
#include <string>
#include <sstream>

extern "C"
{
//编解码(最重要的库)
#include "libavcodec/avcodec.h"
//封装格式处理
#include "libavformat/avformat.h"
//工具库（大部分库都需要这个库支持）
#include "libavutil/imgutils.h"
//视频像素数据格式转换
#include "libswscale/swscale.h"
};

using namespace std;


#define OUT_WIDTH    480
#define OUT_HEIGHT   360


int savePicture(AVFrame *pFrame) {//编码保存图片
    static int index = 0;
    stringstream stream;
    stream<<"pic_";//向流中传值
    stream<<index;//向流中传值
    stream<<".jpg";//向流中传值
    std::string pic_file_name;
    stream>>pic_file_name;//向result中写入值

    index ++;
    
    int width = pFrame->width;
    int height = pFrame->height;

    printf("prapre to save jpg!!!!!\n");
    
    
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    // 设置输出文件格式
    pFormatCtx->oformat = av_guess_format("mjpeg", NULL, NULL);
 
    // 创建并初始化输出AVIOContext
    if (avio_open(&pFormatCtx->pb, pic_file_name.c_str(), AVIO_FLAG_READ_WRITE) < 0) {
        printf("Couldn't open output file.");
        return -1;
    }
 
    // 构建一个新stream
    AVStream *pAVStream = avformat_new_stream(pFormatCtx, 0);
    if (pAVStream == NULL) {
        return -1;
    }
    
    AVCodecContext *pCodeCtx;
    pAVStream->codecpar->codec_id = pFormatCtx->oformat->video_codec;
    pAVStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    pAVStream->codecpar->format = AV_PIX_FMT_YUVJ420P;
    pAVStream->codecpar->width = pFrame->width;
    pAVStream->codecpar->height = pFrame->height;
    // pAVStream->codecpar->width = OUT_WIDTH;
    // pAVStream->codecpar->height = OUT_HEIGHT;
 
    pCodeCtx = avcodec_alloc_context3(NULL);
    if (!pCodeCtx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }
 
    if ((avcodec_parameters_to_context(pCodeCtx, pAVStream->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return -1;
    }
 
    pCodeCtx->time_base = (AVRational) {1, 25};

    printf("width:%d   height:%d\n", pCodeCtx->width, pCodeCtx->height);

    const AVCodec *pCodec = avcodec_find_encoder(pCodeCtx->codec_id);
 
    if (!pCodec) {
        printf("Could not find encoder\n");
        return -1;
    }
 
    if (avcodec_open2(pCodeCtx, pCodec, NULL) < 0) {
        printf("Could not open encodec.");
        return -1;
    }
 
    int ret = avformat_write_header(pFormatCtx, NULL);
    if (ret < 0) {
        printf("write_header fail\n");
        return -1;
    }
 
    int y_size = width * height;
 
    //Encode
    // 给AVPacket分配足够大的空间
    AVPacket* pkt;
    //av_new_packet(&pkt, y_size * 3);
    pkt = av_packet_alloc();

    char errMsg[512] = {0};
 
    // 编码数据
    ret = avcodec_send_frame(pCodeCtx, pFrame);
    if (ret < 0) {
        av_strerror(ret, errMsg, 512);
        printf("Could not avcodec_send_frame(%d):%s.\n", ret, errMsg);
        return -1;
    }
 
    // 得到编码后数据
    ret = avcodec_receive_packet(pCodeCtx, pkt);
    if (ret < 0) {
        printf("Could not avcodec_receive_packet\n");
        return -1;
    }
 
    ret = av_write_frame(pFormatCtx, pkt);
 
    if (ret < 0) {
        printf("Could not av_write_frame\n");
        return -1;
    }
 
 
    //Write Trailer
    av_write_trailer(pFormatCtx);
 
    av_packet_free(&pkt);
    avcodec_close(pCodeCtx);
    avio_close(pFormatCtx->pb);
    avformat_free_context(pFormatCtx);
 
    return 0;
}


int main(int argc, char  **argv)
{
    // got filepath
    if(argc < 3)
    {
        printf("Usage:./task xxx.mp4  pos\n");
        return 0;
    }
    std::string filepath = std::string( argv[1] );   //file path
    int pos = 5;                           //pos
    printf("media file:%s, pos:%d.\n", filepath.c_str(), pos);

    //ffmpeg
    //step 1:register
    //av_register_all();
   
    AVFormatContext	*pFormatCtx;  
    AVStream        *videoStream;   
	AVCodecContext	*pCodecCtx;   
	const AVCodec	*pCodec;
    AVFrame	        *pFrame;
    AVFrame	        *pYUV;
	AVPacket        *pkg;
    FILE *fp_yuv = fopen("output.yuv", "wb+");

    //step 2:open input
    int ret = 0;
    char errMsg[512] = {0};
    ret = avformat_open_input(&pFormatCtx, filepath.c_str(), NULL, NULL);
    if(ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("open file fail(%d):%s.\n", ret, errMsg);
        goto Free;
    }

    //step 3:find info
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if(ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("find stream info fail(%d):%s.\n", ret, errMsg);
        goto Free;
    }

    //step 4:find video stream
    for(int i = 0; i < pFormatCtx->nb_streams; i ++){
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = pFormatCtx->streams[i];
            break;
        }
    }

    if(!videoStream)
    {
        printf("cannot find video stream.\n");
        goto Free;
    }

    //step 5: find decoder
    pCodecCtx = avcodec_alloc_context3(NULL);
    ret = avcodec_parameters_to_context(pCodecCtx, videoStream->codecpar);
    if(ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("video decoder open fail(%d):%s.\n", ret, errMsg);
        goto Free;
    }
    pCodec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if(!pCodec)
    {
        printf("cannot find video stream.\n");
        goto Free;
    }

    //step 6: open video decoder
    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if(ret < 0)
    {
        av_strerror(ret, errMsg, 512);
        printf("video decoder open fail(%d):%s.\n", ret, errMsg);
        goto Free;
    }

    printf("####################dump start####################\n");
    av_dump_format(pFormatCtx, videoStream->index, filepath.c_str(),0);
    printf("####################dump end####################\n");

    //step 7:we find a frame which pts = pos , decoder it & save yuv data.
    while(true)
    {
        pkg = av_packet_alloc();
        printf("read a frame!!!!.\n");
        ret = av_read_frame(pFormatCtx, pkg);
        if(ret < 0)
        {
            printf("av_read_frame fail.\n");
            break;
        }
        //debug pkg info
        printf("pkg duration:%ld.\n", pkg->duration);
        printf("pkg index:%d.\n", pkg->stream_index);
        printf("pkg pos:%ld.\n", pkg->pos);
        printf("pkg dts:%ld.\n", pkg->dts);
        printf("pkg pts:%ld.\n", pkg->pts);
        static int got_pic = 0;
        if(pkg->stream_index == videoStream->index)
        {
            if(avcodec_send_packet(pCodecCtx, pkg) != 0){
                printf("avcodec_send_packet错误!\n");
                break;
             }
            pFrame = av_frame_alloc();
            while(avcodec_receive_frame(pCodecCtx, pFrame) == 0)
            {
                //AV_PIX_FMT_YUV420P,  ///< planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples)
                printf("frame_number:%d,  width:%d,  height:%d.\n", pCodecCtx->frame_number, pFrame->width, pFrame->height);
                pYUV = av_frame_alloc();
                pYUV->width = OUT_WIDTH;
                pYUV->height = OUT_HEIGHT;

                int video_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, OUT_WIDTH, OUT_HEIGHT, 1);
                uint8_t* buf = (uint8_t*)av_malloc(video_size);
                av_image_fill_arrays(pYUV->data, pYUV->linesize, buf, AV_PIX_FMT_YUV420P, OUT_WIDTH, OUT_HEIGHT, 1);

                //视频像素数据格式转换上下文
                SwsContext *sws_ctx =
                        sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,/*INPUT  W H FMT*/
                                       OUT_WIDTH, OUT_HEIGHT, AV_PIX_FMT_YUV420P, /*OUTPUT W H FMT*/
                                       SWS_BICUBIC,NULL,NULL,NULL);

                //我要将AVFrame转成视频像素数YUV420P格式
                sws_scale(sws_ctx,
                        pFrame->data, pFrame->linesize,
                        0, pCodecCtx->height, 
                        pYUV->data, pYUV->linesize);

                
                if(fp_yuv == NULL)
                {
                    printf("fopen fail!!!!\n");
                    break;
                }

                int y_size = OUT_WIDTH  * OUT_HEIGHT;
                // //pFrame->data[0]表示Y
                // fwrite(pYUV->data[0], 1, y_size, fp_yuv);
                // //pFrame->data[1]表示U
                // fwrite(pYUV->data[1], 1, y_size/4, fp_yuv);
                // //pFrame->data[2]表示V
                // fwrite(pYUV->data[2], 1, y_size/4, fp_yuv);

                printf("yuv width:%d   height:%d\n", pYUV->width, pYUV->height);

                //pFrame->data[0]表示Y
                fwrite(buf, 1, y_size, fp_yuv);
                //pFrame->data[1]表示U
                fwrite(buf + y_size, 1, y_size/4, fp_yuv);
                //pFrame->data[2]表示V
                fwrite(buf + y_size* 5/4, 1, y_size/4, fp_yuv);

                fclose(fp_yuv);

                return 0;
                
                //savePicture(pFrame);
                
                got_pic ++;
                break;
            }
        }  
        av_packet_free(&pkg);
        if(got_pic > 10)
        {
            break;
        }
    }

Free:
    fclose(fp_yuv);
    if(pFrame)
    {
        av_frame_free(&pFrame);
    }

    if(pkg)
    {
        av_packet_free(&pkg);
    }
    //close codec
    avcodec_close(pCodecCtx);
    //close input
    avformat_close_input(&pFormatCtx);

    return 0;
}
