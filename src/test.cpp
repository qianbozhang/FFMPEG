#include <stdio.h>
#include <string>
#include <sstream>
#include "thumbnail.h"

static void CB(ThumbEvent_e event, ThumbError_e error)
{
	printf("rev event:%d, error:%d.\n", event, error);

}


//thread
void getthumb(std::string file, int width, int height)
{
	Thumbnail thu;

	thu.getThumbnail(file, width, height, 1000 * 1.5  /* ms */, CB);
}

int main(int argc, char const *argv[])
{
    std::string filepath = std::string( argv[1] );
    
    getthumb(filepath, 480, 360);
    
    return 0;
}
