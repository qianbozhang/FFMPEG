#include "CountTime.h"

CountTime::CountTime(std::string msg)
{
    gettimeofday(&m_start, nullptr);
    m_msg = msg;
}

CountTime::~CountTime()
{
    timeval t_end;
    gettimeofday(&t_end, nullptr);
   
    int cost = (t_end.tv_sec - m_start.tv_sec) * 1000 + (t_end.tv_usec - m_start.tv_usec)/1000;
    printf("%s cost %d ms.\n", m_msg.c_str(), cost);
}