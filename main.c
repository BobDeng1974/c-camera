#include <fcntl.h> //open 函数
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL/SDL.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h> //struct v4l2_capability 结构体
#include <sys/mman.h>


#define DEV_CAMERA "/dev/video0"
#define REQUBUF_COUNT 4
#define handle_err(msg , id)\
    do{ char buf[100];sprintf(buf , "error:%s , line:%d" , msg , id); perror(buf); exit(EXIT_FAILURE);} while(0);

#define FREE(fp) \
    while(fp){free(fp); fp = NULL;}

typedef struct {
    void* start;
    unsigned int length;
}BUFFER;

//包含用于软件blitting的像素集合的结构。
SDL_Surface *pscreen = NULL;

//YUV视频覆盖
SDL_Overlay *overlay = NULL;

//矩形定义的结构，原点在左上角。
SDL_Rect drect;

//包含不同事件类型的结构的联合
SDL_Event sdlevent;

//互斥锁
SDL_mutex *affmutex = NULL;
unsigned char *p = NULL;


int process_image(BUFFER *buf , int width , int height);

int main()
{
    if (SDL_Init(SDL_INIT_EVERYTHING) != 0){ //使用SDL库前初始化
        fprintf(stderr , "Can not initialize SDL: %s\n" , SDL_GetError());
        return -1;
    } 

    //linux video dev struct
    struct v4l2_capability cap;
    int fd;
    fd = open(DEV_CAMERA , O_RDWR);
    if (fd < 0){
        printf("Can not open %s\n" , DEV_CAMERA);
        return -2;
    }

    //VIDIOC_QUERYCAP -->查询设备功能
    ioctl(fd , VIDIOC_QUERYCAP , &cap );//用于设备输入输出操作的系统调用,调用的功能完全取决于请求码
    printf("version: %u\n" , cap.version);
    printf("DirverName: %s\nCard Name: %s\nBus info:%s\nDriverVersion:%u.%u.%u\n"
            , cap.driver , cap.card , cap.bus_info , (cap.version >> 16) & 0xFF , 
            cap.version >> 8 & 0xFF , cap.version & 0xFF);

    // format
    struct v4l2_fmtdesc fmtdesc;
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    printf("support format:\n");
    while(ioctl(fd , VIDIOC_ENUM_FMT , &fmtdesc) != -1){
        printf("\t%d.%s\n" , 1+ fmtdesc.index++ , fmtdesc.description);
    }

    struct v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd , VIDIOC_G_FMT , &fmt);
    printf("Current format information:\n\twidth:%d\n\theight:%d\n" , fmt.fmt.pix.width , fmt.fmt.pix.height);

    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while(ioctl(fd , VIDIOC_ENUM_FMT , &fmtdesc) != -1){
        if (fmtdesc.pixelformat & fmt.fmt.pix.pixelformat){
            printf("\tformat:%s\n" , fmtdesc.description);
            break;
        }
        fmtdesc.index++;
    }

    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
    if (ioctl(fd , VIDIOC_TRY_FMT , &fmt) == -1){
        perror("ioctl err");
        if (errno == EINVAL)
            printf(" no support format RGB32\n");
    }else{
        printf("RGB32 is supported\n");
    }

    //向驱动申请帧缓冲的请求，里面包含申请的个数
    struct v4l2_requestbuffers reqbuf;
    reqbuf.count = REQUBUF_COUNT;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    ioctl(fd , VIDIOC_REQBUFS , &reqbuf);

    BUFFER *buffers;
    buffers = (BUFFER*)calloc(reqbuf.count , sizeof(*buffers));
    if (!buffers){
        handle_err("calloc" , __LINE__);
    }

    for (unsigned int n_buf = 0; n_buf < reqbuf.count; ++n_buf){
        //代表驱动中的一帧
        struct v4l2_buffer buf;
        memset(&buf , 0 , sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buf;
        if (-1 == ioctl(fd , VIDIOC_QUERYBUF , &buf)){
            handle_err("ioctl" , __LINE__);
        }
        buffers[n_buf].length = buf.length;
        printf("buf.length = %d , buf.m.o.offset = %d\n" , buf.length ,
                buf.m.offset);
        buffers[n_buf].start = mmap(NULL , buf.length , PROT_READ|PROT_WRITE ,
               MAP_SHARED , fd , buf.m.offset );
        if (MAP_FAILED == buffers[n_buf].start){
            handle_err("mmap" , __LINE__);
        }
    }

    //get camera data
    unsigned int i = 0;
    enum v4l2_buf_type type;
    for (i = 0; i <reqbuf.count; ++i){
        struct v4l2_buffer buf;
        memset(&buf , 0 , sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ioctl(fd , VIDIOC_QBUF , &buf);
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd , VIDIOC_STREAMON , &type);

    int width = fmt.fmt.pix.width;
    int height = fmt.fmt.pix.height;
    //设置具有指定宽度，高度和每像素位数的视频模式。 
    //如果bpp为0，则将其视为每个像素的当前显示位数。
    pscreen = SDL_SetVideoMode(width , height , 0 , SDL_SWSURFACE);

    // 创建一个YUV视频覆盖
    overlay = SDL_CreateYUVOverlay(width , height , SDL_YUY2_OVERLAY , pscreen); 

    p = (unsigned char *)overlay->pixels[0];
    drect.x = 0;
    drect.y = 0;
    drect.w = pscreen->w;
    drect.h = pscreen->h;
    int quit = 0;
    while(quit == 0){
        //使用此函数轮询当前未决事件。
        while (SDL_PollEvent(&sdlevent)){
            if (sdlevent.type == SDL_QUIT){
                quit = 1;
                break;
            }
        }

        for (i = 0; i < reqbuf.count; ++i){
            struct v4l2_buffer buf;
            memset(&buf , 0 , sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            ioctl(fd , VIDIOC_DQBUF , &buf);

            //deal with data
            process_image(&buffers[i] , fmt.fmt.pix.width , fmt.fmt.pix.height);
            ioctl(fd , VIDIOC_QBUF , &buf); 
        }
    }

    //销毁使用SDL_CreateMutex（）创建的互斥锁
    SDL_DestroyMutex(affmutex);    
    //释放YUV视频覆盖
    SDL_FreeYUVOverlay(overlay); 
    printf("Clean Up done Quit \n");
    
    //清理所有SDL初始化的子系统
    SDL_Quit();

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd , VIDIOC_STREAMON , &type);
    for (i = 0; i < reqbuf.count; ++i){
        if (-1 == munmap(buffers[i].start , buffers[i].length)){
            handle_err("munmap" , __LINE__);
        }
    }
    
    FREE(buffers);
    close(fd);
}

int process_image(BUFFER *buf , int width , int height)
{
    affmutex = SDL_CreateMutex();
    SDL_LockSurface(pscreen);
    SDL_LockYUVOverlay(overlay);
    memcpy(p , buf->start , width * height * 2);
    SDL_UnlockYUVOverlay(overlay);
    SDL_UnlockSurface(pscreen);

    //位块传输覆盖显示
    SDL_DisplayYUVOverlay(overlay , &drect);
    
    //设置窗口图块和图标名称。
    SDL_WM_SetCaption("Yun's camera" , NULL);
    //在返回之前等待指定的毫秒数
    SDL_Delay(40);
    return 0;
}
