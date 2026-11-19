# 05 · V4L2 代理：让普通应用当摄像头用

源文件：kernel/camv4l2/camv4l2.c、userspace/{camvid.c, camdaemon.sh, cammirror}

## 1. 为什么要自己写一个 V4L2 模块

设备上没有可用的相机 V4L2 驱动（MTK 的 ISP 驱动是 Android 专用），
而 v4l2loopback 这类现成方案在无外网环境拉不到，且需求其实很小：
应用要一个 /dev/videoN，能 S_FMT、能 mmap 取 YUYV/RGB24 帧即可。

所以：用户态 camvid 采集+处理，写好的一帧 YUYV 通过 /dev/camfeed 的 write 送进来，内核模块用 vb2_vmalloc 把它变成标准 V4L2 流。

    应用 --(VIDIOC_S_FMT/REQBUFS/STREAMON)--> camv4l2.ko
    应用 <--(mmap DQBUF)-------------------- camv4l2.ko <--(write /dev/camfeed)-- camvid

## 2. camfeed 私有 ABI

    struct camfeed_fmt   { u32 width, height, fourcc, bpl, sizeimage; };   /* 20 B */
    struct camfeed_stats { u64 fed, dropped, seq;
                           u32 streaming, queued; };                      /* 32 B */

    CAMFEED_SET_FMT = _IOW ('F', 1, struct camfeed_fmt)
    CAMFEED_GET_FMT = _IOR ('F', 2, struct camfeed_fmt)
    CAMFEED_STATS   = _IOR ('F', 3, struct camfeed_stats)

流程：camvid 先 GET_FMT 学到应用当前要什么（由 S_FMT 决定），再按需缩放/裁剪/转格式，
然后每帧 write 到 /dev/camfeed（一个 write = 一帧，长度必须 == sizeimage）。

## 3. 模块能力

* 像素格式：V4L2_PIX_FMT_YUYV、V4L2_PIX_FMT_RGB24。
* 尺寸（只做 4:3）：640×480、800×600、1280×960、1632×1224。
  **刻意不支持 16:9**：源是 4:3，做 1280×720 会裁掉 25% 视野（微信里问过这个问题）。
* 帧间隔：1/30、1/15、1/10、1/5。
* module_param video_nr（默认 0 = 自动分配）；本项目实际用 video_nr=3 + 软链 /dev/video3 -> /dev/video0。
* 双节点版（camv4l2_dual）：camv4l2 后摄 / camv4l2-front 前置，用 camv_owner 记录最后 STREAMON 的节点。

## 4. 三个必踩的坑

1. **v4l2_device_register 之前必须 strscpy(v4l2_dev.name)**，否则内核 WARN_ON 后返回 -EINVAL。
2. **必须实现 enum_input / g_input / s_input**，否则 ffmpeg 直接报
   `ioctl(VIDIOC_G_INPUT): Inappropriate ioctl for device`。
3. **不要在 ioctl handler 里再锁 vdev->lock**：video_ioctl2() 已经持锁，再 mutex_lock 同一个锁会让进程永久 D 状态，
   kill -9 无效、rmmod refcount 不降，只能重启。

另外：喂帧速度必须跟应用的消费节奏匹配（应用慢就丢帧，不能无限堆积）。

## 5. camdaemon

userspace/camdaemon.sh 负责「有应用要流才启动管线」：

* 轮询 /dev/camfeed 的 STATS，streaming=1 → 起 camvid；=0 → SIGTERM 收流。
* 给 camvid 传一组稳定的画质/曝光参数（见脚本内 CAMREC_* 变量）。
* 双节点版会按前/后摄分别拉起 camfront / camvid。

当前生产参数大致是：

    CAMREC_VFAST=1 CAMREC_TNR_ALPHA=0.30 CAMREC_HI_FLOOR=175
    CAMREC_AE_VTSMIN=2500 CAMREC_AE_VTSMAX=6000 CAMREC_VTHREADS=2
    camvid -n 100000 -L /dev/camfeed -F 30 -v 6000 -e 2400 -g 0 -A -t 330
           --vcontrast 1.25 --vgamma 2.0

（早期用 -v 2500 保 30 fps，但夜间被帧长卡死曝光；改成 6000 后夜间 12~15 fps、白天仍 30 fps。）

镜像：前置自拍需要水平翻转，`/persist/cam_flip`（默认 front）+ cammirror 控制，相关 0.9186。

## 6. 验证过的应用

| 工具 | 结果 |
| --- | --- |
| v4l2-ctl | 60 帧连续取流 OK |
| ffmpeg -f v4l2 | h264 编码 OK |
| GStreamer | 25 帧 JPEG OK |
| Chromium getUserMedia | 640×480 与 1280×720（早期档位）OK |
| 微信 / QQ | 能出图；视野 16:9 被裁与拖影问题已修（删 16:9 档 + 固定 VTS + TNR α0.30） |

## 7. 测试工具

userspace/tools/{camfeed_test.py, camfeedfmt.c, camfeedstat.c, v4ltest.c} —— 分别用于直接 write 测试、格式读写、统计读取、最小 V4L2 取流。