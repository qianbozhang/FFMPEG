#ifndef _THUMBNAIL_H_
#define _THUMBNAIL_H_

#include <string>

typedef enum _ThumbEvent
{
	EVENT_UNKNOWN = 0,
	EVENT_THUMBDONE,
	EVENT_THUMBERROR,
	EVENT_ABORT
}ThumbEvent_e;

typedef enum _ThumbError
{
	ERROR_UNKNOWN = 0,
	ERROR_INITFAIL,
	ERROR_OPENFAIL,
	ERROR_SEEKFAIL,
	ERROR_DECODERFAIL,
	ERROR_EDITFAIL,
	ERROR_SAVEFAIL
}ThumbError_e;

typedef void (*CallBackFun)(ThumbEvent_e event, ThumbError_e error);

typedef struct AVFormatContext;
typedef struct AVPacket;

class BuliderThumbnail
{
public:
	/*param init*/
	virtual int Init(const std::string& uri, int width, int height, int pos, CallBackFun cb) { return -1000; }
	/*open file & find video track info */
	virtual int OPenFile() { return -1000; }
	/*seek to pos & get urrent pos's I frame*/
	virtual int SeekToPos() { return -1000; }
	/*decoder current frame.*/
	virtual int DecoderFrame() { return -1000; }
	/*edit yuv data*/
	virtual int EditYvuData() { return -1000; }
	/*save yuv to jpeg*/
    virtual int SavePicture() { return -1000; }
};

class Thumbnail :public BuliderThumbnail
{

public:
    Thumbnail();
    ~Thumbnail();


    int Stop();

    int getThumbnail(const std::string& uri, int width, int height, int pos, CallBackFun cb);

	/*param init*/
	virtual int Init(const std::string& uri, int width, int height, int pos, CallBackFun cb);
	/*open file & find video track info */
	virtual int OPenFile();
	/*seek to pos & get urrent pos's I frame*/
	virtual int SeekToPos();
	/*decoder current frame.*/
	virtual int DecoderFrame();
	/*edit yuv data*/
	virtual int EditYvuData();
	/*save yuv to jpeg*/
	virtual int SavePicture(const std::string thumbUrl, const unsigned char *buf, int w, int h);

private:
    bool WidthAndHeight_Equal();
    bool Width_Equal();
	bool Height_Equal();
	bool WidthAndHeight_NoEqual();

	//copy unsigned char
	bool CopyUnsignedChar(unsigned char *dest, int &pos, unsigned char *src, int start, int len);

private:
	//ffmpeg
    AVFormatContext  *m_FmtCtx;
    unsigned char    *m_picture_buf;
    unsigned char    *m_picture_buf2;
	long long         m_Duration;//ms
	int               m_VideoTrack;
	int               m_VideoWidth;
	int               m_VideoHeight;
	int               m_SwsWidth;
	int               m_SwsHeight;

    //user data
    std::string       m_FilePath;
    int               m_width;/*user*/
    int               m_height;/*user*/
	int               m_Pos;//ms
    CallBackFun       m_Cb;

    //thumbnail state
    ThumbEvent_e      m_Event;
    ThumbError_e      m_Error;

    //stop flag
    bool              m_Stop;
};



#endif