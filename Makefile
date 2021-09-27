TARGET := task

FFMPEG :=$(shell pwd)

CC  := gcc
CXX := g++

CFLAGS := -g -Wall 

SOURCE := decode_to_yuv.cpp

CFLAGS += -I$(FFMPEG)/include
LIBS   := -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale

OBJS   := $(notdir  $(patsubst %.cpp,%.o,$(SOURCE)))

.PHONY : all clean
all:$(TARGET)
$(TARGET) : $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)
%.o : %.cpp
	$(CXX) $(CFLAGS) -c $< -o $@

clean:
	-@rm -rf $(OBJS) $(TARGET)