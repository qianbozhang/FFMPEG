#环境配置:
官网下载ffmpeg:git clone https://git.ffmpeg.org/ffmpeg.git
#编译安装ffmpeg
./configure --prefix=自定义目录 --enable-shared
sudo make
sudo make install


decode_to_yuv:
编译:make all
运行环境配置:
export LD_LIBRARY_PATH=../lib:$LD_LIBRARY_PATH
运行:./task 4k.mp4 5


