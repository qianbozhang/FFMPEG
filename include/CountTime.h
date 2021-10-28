#ifndef _COUNT_TIME_H_
#define _COUNT_TIME_H_

#include <sys/time.h>
#include <string>

class CountTime
{
public:
    CountTime(std::string msg);
    ~CountTime();
private:
    //CountTime();
private:
    timeval     m_start;
    std::string m_msg;
};


#endif