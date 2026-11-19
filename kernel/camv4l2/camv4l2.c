// SPDX-License-Identifier: GPL-2.0
/*
 * camv4l2 - proxy V4L2 capture node fed from userspace (/dev/camfeed)
 *
 * Rationale: the MT6835 ISP6s camera pipeline used in this project is driven
 * entirely from userspace (golden CQ ring replay + ISP post-processing in C).
 * This module exposes a *standard Linux V4L2 capture device* whose frames are
 * pushed in by that userspace pipeline, so any V4L2 application (ffmpeg,
 * OpenCV, GStreamer, v4l2-ctl) can consume the camera as a normal /dev/videoN.
 *
 * Feed protocol (/dev/camfeed):
 *   ioctl(CAMFEED_SET_FMT) once with geometry/fourcc/bpl/sizeimage
 *   write() frames of exactly sizeimage bytes (non-blocking: returns -EAGAIN
 *   when the consumer has not returned a buffer yet)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define CAMV_DRV "camv4l2"
#define CAMV_MAXDIM 4096
#define CAMV_MAXSIZE (32 * 1024 * 1024)

/* 对普通应用（GStreamer / Chromium WebRTC / 微信 / QQ）暴露的格式与尺寸。
 * YUYV 是事实标准（GStreamer v4l2src、Chromium、各视频通话 SDK 的默认首选），
 * RGB24 保留给我们自己的工具链与调试。数据由 /dev/camfeed 端按请求的尺寸/格式产出。 */
static const u32 camv_pixfmts[] = {
	V4L2_PIX_FMT_YUYV,
	V4L2_PIX_FMT_RGB24,
};

static const char *camv_pixfmt_name(u32 fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_YUYV:  return "YUYV 4:2:2";
	case V4L2_PIX_FMT_RGB24: return "RGB24";
	default:                 return "unknown";
	}
}

static const struct { u32 w, h; } camv_sizes[] = {
	{ 640,  480 },
	{ 800,  600 },
	{ 1280, 960 },
	{ 1632, 1224 },
};

static int camv_is_pixfmt(u32 fourcc)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(camv_pixfmts); i++)
		if (camv_pixfmts[i] == fourcc)
			return 1;
	return 0;
}

static u32 camv_bpl(u32 fourcc, u32 w)
{
	return fourcc == V4L2_PIX_FMT_YUYV ? w * 2 : w * 3;
}

static u32 camv_sizeimage(u32 fourcc, u32 w, u32 h)
{
	return camv_bpl(fourcc, w) * h;
}

static int video_nr = 0;
module_param(video_nr, int, 0644);
MODULE_PARM_DESC(video_nr, "V4L2 video node number (default 0 -> /dev/video0)");

struct camfeed_fmt {
	__u32 width;
	__u32 height;
	__u32 fourcc;
	__u32 bpl;
	__u32 sizeimage;
};

struct camfeed_stats {
	__u64 fed;
	__u64 dropped;
	__u64 seq;
	__u32 streaming;
	__u32 queued;
};

#define CAMFEED_SET_FMT _IOW('F', 1, struct camfeed_fmt)
#define CAMFEED_GET_FMT _IOR('F', 2, struct camfeed_fmt)
#define CAMFEED_STATS _IOR('F', 3, struct camfeed_stats)

struct camv_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct camv_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;		/* serializes fmt changes */
	spinlock_t qlock;		/* protects bufs list */
	struct list_head bufs;
	wait_queue_head_t feed_wq;
	struct camfeed_fmt fmt;
	unsigned int fps;
	atomic_t streaming;
	u64 fed;
	u64 dropped;
	u64 seq;
	int index;		/* 0 = rear node, 1 = front node */
	const char *card;	/* v4l2_capability.card */
};

/* 双摄像头代理：两个 V4L2 节点（后摄 / 前置）共享同一个 /dev/camfeed 推流口。
 * 应用选中哪个节点起了流，推流数据就进入那个节点的 VB2 队列；同一时刻只服务
 * 一个节点（camv_owner 记录最后开始 streaming 的节点）。 */
static struct camv_dev *camv_devs[2];
static struct camv_dev *camv_owner;

static struct camv_dev *camfeed_dev(void)
{
	struct camv_dev *d = READ_ONCE(camv_owner);

	if (d && atomic_read(&d->streaming))
		return d;
	return camv_devs[0];
}

/* ---------------- videobuf2 ops ---------------- */

static int camv_queue_setup(struct vb2_queue *q, unsigned int *nbufs,
			    unsigned int *num_planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct camv_dev *d = vb2_get_drv_priv(q);
	unsigned int size = d->fmt.sizeimage;

	if (!size || size > CAMV_MAXSIZE)
		return -EINVAL;
	if (*num_planes) {
		if (sizes[0] < size)
			return -EINVAL;
		return 0;
	}
	*num_planes = 1;
	sizes[0] = size;
	if (*nbufs < 2)
		*nbufs = 4;
	if (*nbufs > 8)
		*nbufs = 8;
	return 0;
}

static int camv_buf_prepare(struct vb2_buffer *vb)
{
	struct camv_dev *d = vb2_get_drv_priv(vb->vb2_queue);

	vb2_set_plane_payload(vb, 0, d->fmt.sizeimage);
	return 0;
}

static void camv_buf_queue(struct vb2_buffer *vb)
{
	struct camv_dev *d = vb2_get_drv_priv(vb->vb2_queue);
	struct camv_buf *b = container_of(vb, struct camv_buf, vb.vb2_buf);
	unsigned long flags;

	spin_lock_irqsave(&d->qlock, flags);
	list_add_tail(&b->list, &d->bufs);
	spin_unlock_irqrestore(&d->qlock, flags);
	wake_up_interruptible_all(&d->feed_wq);
}

static int camv_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct camv_dev *d = vb2_get_drv_priv(q);

	atomic_set(&d->streaming, 1);
	WRITE_ONCE(camv_owner, d);
	pr_info(CAMV_DRV ": [%s] streaming start, %u buffers\n",
		d->card, count);
	return 0;
}

static void camv_stop_streaming(struct vb2_queue *q)
{
	struct camv_dev *d = vb2_get_drv_priv(q);
	struct camv_buf *b, *tmp;
	LIST_HEAD(local);
	unsigned long flags;

	atomic_set(&d->streaming, 0);
	if (READ_ONCE(camv_owner) == d)
		WRITE_ONCE(camv_owner, NULL);
	spin_lock_irqsave(&d->qlock, flags);
	list_splice_init(&d->bufs, &local);
	spin_unlock_irqrestore(&d->qlock, flags);
	list_for_each_entry_safe(b, tmp, &local, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	wake_up_interruptible_all(&d->feed_wq);
	pr_info(CAMV_DRV ": [%s] streaming stop\n", d->card);
}

static void camv_wait_prepare(struct vb2_queue *q)
{
	mutex_unlock(q->lock);
}

static void camv_wait_finish(struct vb2_queue *q)
{
	mutex_lock(q->lock);
}

static const struct vb2_ops camv_vb2_ops = {
	.queue_setup = camv_queue_setup,
	.buf_prepare = camv_buf_prepare,
	.buf_queue = camv_buf_queue,
	.wait_prepare = camv_wait_prepare,
	.wait_finish = camv_wait_finish,
	.start_streaming = camv_start_streaming,
	.stop_streaming = camv_stop_streaming,
};

/* ---------------- V4L2 ioctls ---------------- */

static void camv_fill_pix(struct camv_dev *d, struct v4l2_format *f)
{
	memset(&f->fmt.pix, 0, sizeof(f->fmt.pix));
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f->fmt.pix.width = d->fmt.width;
	f->fmt.pix.height = d->fmt.height;
	f->fmt.pix.pixelformat = d->fmt.fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = d->fmt.bpl;
	f->fmt.pix.sizeimage = d->fmt.sizeimage;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
}

static int camv_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	struct camv_dev *d = video_drvdata(file);

	strscpy(cap->driver, CAMV_DRV, sizeof(cap->driver));
	strscpy(cap->card, d->card, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:camfeed", sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int camv_enum_fmt(struct file *file, void *priv,
			 struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(camv_pixfmts))
		return -EINVAL;
	f->pixelformat = camv_pixfmts[f->index];
	f->flags = 0;
	strscpy(f->description, camv_pixfmt_name(f->pixelformat),
		sizeof(f->description));
	return 0;
}

static int camv_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fs)
{
	int i, n = 0;

	if (!camv_is_pixfmt(fs->pixel_format))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(camv_sizes); i++) {
		if (n != fs->index) {
			n++;
			continue;
		}
		fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs->discrete.width = camv_sizes[i].w;
		fs->discrete.height = camv_sizes[i].h;
		return 0;
	}
	return -EINVAL;
}

static const struct { u32 num, den; } camv_intervals[] = {
	{ 1, 30 }, { 1, 15 }, { 1, 10 }, { 1, 5 },
};

static int camv_enum_frameintervals(struct file *file, void *priv,
				    struct v4l2_frmivalenum *fi)
{
	if (!camv_is_pixfmt(fi->pixel_format))
		return -EINVAL;
	if (fi->index >= ARRAY_SIZE(camv_intervals))
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = camv_intervals[fi->index].num;
	fi->discrete.denominator = camv_intervals[fi->index].den;
	return 0;
}

/* 把应用请求的格式/尺寸规整到我们真正支持的组合 */
static void camv_normalize(struct v4l2_format *f)
{
	struct v4l2_pix_format *p = &f->fmt.pix;
	u32 bestw = camv_sizes[0].w, besth = camv_sizes[0].h;
	int i;

	if (!camv_is_pixfmt(p->pixelformat))
		p->pixelformat = V4L2_PIX_FMT_YUYV;
	if (!p->width || !p->height) {
		p->width = camv_sizes[0].w;
		p->height = camv_sizes[0].h;
	}
	/* 选最接近的支持尺寸 */
	for (i = 1; i < ARRAY_SIZE(camv_sizes); i++) {
		long dcur = abs((long)p->width - bestw) +
			    abs((long)p->height - besth);
		long dnew = abs((long)p->width - camv_sizes[i].w) +
			    abs((long)p->height - camv_sizes[i].h);
		if (dnew < dcur) {
			bestw = camv_sizes[i].w;
			besth = camv_sizes[i].h;
		}
	}
	p->width = bestw;
	p->height = besth;
	p->field = V4L2_FIELD_NONE;
	p->colorspace = V4L2_COLORSPACE_SRGB;
	p->bytesperline = camv_bpl(p->pixelformat, p->width);
	p->sizeimage = camv_sizeimage(p->pixelformat, p->width, p->height);
}

static int camv_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct camv_dev *d = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	camv_fill_pix(d, f);
	return 0;
}

static int camv_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct camv_dev *d = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	camv_normalize(f);
	(void)d;
	return 0;
}

static int camv_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct camv_dev *d = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	/* video_ioctl2() already holds vdev->lock (== d->lock) here */
	if (vb2_is_busy(&d->queue))
		return -EBUSY;
	camv_normalize(f);
	d->fmt.width = f->fmt.pix.width;
	d->fmt.height = f->fmt.pix.height;
	d->fmt.fourcc = f->fmt.pix.pixelformat;
	d->fmt.bpl = f->fmt.pix.bytesperline;
	d->fmt.sizeimage = f->fmt.pix.sizeimage;
	pr_info(CAMV_DRV ": app s_fmt %ux%u fourcc=%08x size=%u\n",
		d->fmt.width, d->fmt.height, d->fmt.fourcc, d->fmt.sizeimage);
	return 0;
}

static int camv_enum_input(struct file *file, void *priv,
			   struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;
	memset(inp, 0, sizeof(*inp));
	inp->index = 0;
	strscpy(inp->name, "camfeed", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int camv_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int camv_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int camv_g_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *p)
{
	struct camv_dev *d = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&p->parm.capture, 0, sizeof(p->parm.capture));
	p->parm.capture.timeperframe.numerator = 1;
	p->parm.capture.timeperframe.denominator = d->fps ? d->fps : 10;
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	return 0;
}

static int camv_s_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *p)
{
	struct camv_dev *d = video_drvdata(file);
	unsigned int num, den, fps;

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	num = p->parm.capture.timeperframe.numerator;
	den = p->parm.capture.timeperframe.denominator;
	if (num && den) {
		fps = den / num;
		if (!fps)
			fps = 1;
		if (fps > 30)
			fps = 30;
		d->fps = fps;
	}
	camv_g_parm(file, priv, p);
	return 0;
}

static const struct v4l2_ioctl_ops camv_ioctl_ops = {
	.vidioc_querycap = camv_querycap,
	.vidioc_enum_fmt_vid_cap = camv_enum_fmt,
	.vidioc_enum_framesizes = camv_enum_framesizes,
	.vidioc_enum_frameintervals = camv_enum_frameintervals,
	.vidioc_g_fmt_vid_cap = camv_g_fmt,
	.vidioc_try_fmt_vid_cap = camv_try_fmt,
	.vidioc_s_fmt_vid_cap = camv_s_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_input = camv_enum_input,
	.vidioc_g_input = camv_g_input,
	.vidioc_s_input = camv_s_input,
	.vidioc_g_parm = camv_g_parm,
	.vidioc_s_parm = camv_s_parm,
};

static const struct v4l2_file_operations camv_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

/* ---------------- /dev/camfeed ---------------- */

static int camfeed_ioctl_set_fmt(struct camfeed_fmt *f)
{
	struct camv_dev *d = camfeed_dev();

	if (!f->width || !f->height || f->width > CAMV_MAXDIM ||
	    f->height > CAMV_MAXDIM)
		return -EINVAL;
	if (!f->fourcc || !f->bpl || !f->sizeimage ||
	    f->sizeimage > CAMV_MAXSIZE)
		return -EINVAL;
	if (atomic_read(&d->streaming))
		return -EBUSY;
	mutex_lock(&d->lock);
	d->fmt = *f;
	mutex_unlock(&d->lock);
	pr_info(CAMV_DRV ": fmt set %ux%u fourcc=%08x bpl=%u size=%u\n",
		f->width, f->height, f->fourcc, f->bpl, f->sizeimage);
	return 0;
}

static long camfeed_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct camv_dev *d = camfeed_dev();

	switch (cmd) {
	case CAMFEED_SET_FMT: {
		struct camfeed_fmt f;

		if (copy_from_user(&f, (void __user *)arg, sizeof(f)))
			return -EFAULT;
		return camfeed_ioctl_set_fmt(&f);
	}
	case CAMFEED_GET_FMT: {
		struct camfeed_fmt f;
		int ret = 0;

		mutex_lock(&d->lock);
		f = d->fmt;
		mutex_unlock(&d->lock);
		if (copy_to_user((void __user *)arg, &f, sizeof(f)))
			ret = -EFAULT;
		return ret;
	}
	case CAMFEED_STATS: {
		struct camfeed_stats s;
		unsigned long flags;
		int ret = 0;

		memset(&s, 0, sizeof(s));
		s.fed = d->fed;
		s.dropped = d->dropped;
		s.seq = d->seq;
		/* 1 = 后摄节点在收流，2 = 前置节点在收流（/root/camfeedstat 第一字段） */
		s.streaming = atomic_read(&d->streaming) ? (__u32)(d->index + 1) : 0;
		spin_lock_irqsave(&d->qlock, flags);
		s.queued = 0;
		{
			struct camv_buf *b;

			list_for_each_entry(b, &d->bufs, list)
				s.queued++;
		}
		spin_unlock_irqrestore(&d->qlock, flags);
		if (copy_to_user((void __user *)arg, &s, sizeof(s)))
			ret = -EFAULT;
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static ssize_t camfeed_write(struct file *file, const char __user *udata,
			     size_t count, loff_t *ppos)
{
	struct camv_dev *d = camfeed_dev();
	struct camv_buf *b;
	void *vaddr;
	unsigned long flags;

	if (!d)
		return -ENODEV;
	if (!d->fmt.sizeimage)
		return -EINVAL;
	if (count != d->fmt.sizeimage)
		return -EINVAL;
	if (!atomic_read(&d->streaming)) {
		d->dropped++;
		return -EAGAIN;
	}
	spin_lock_irqsave(&d->qlock, flags);
	if (list_empty(&d->bufs)) {
		spin_unlock_irqrestore(&d->qlock, flags);
		d->dropped++;
		return -EAGAIN;
	}
	b = list_first_entry(&d->bufs, struct camv_buf, list);
	list_del(&b->list);
	spin_unlock_irqrestore(&d->qlock, flags);

	vaddr = vb2_plane_vaddr(&b->vb.vb2_buf, 0);
	if (!vaddr) {
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return -EFAULT;
	}
	if (copy_from_user(vaddr, udata, count)) {
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return -EFAULT;
	}
	memset(vaddr + count, 0, 0);
	vb2_set_plane_payload(&b->vb.vb2_buf, 0, count);
	b->vb.vb2_buf.timestamp = ktime_get_ns();
	b->vb.sequence = (__u32)d->seq;
	d->seq++;
	vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_DONE);
	d->fed++;
	return count;
}

static int camfeed_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int camfeed_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations camfeed_fops = {
	.owner = THIS_MODULE,
	.open = camfeed_open,
	.release = camfeed_release,
	.write = camfeed_write,
	.unlocked_ioctl = camfeed_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice camfeed_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "camfeed",
	.fops = &camfeed_fops,
	.mode = 0666,
};

/* ---------------- init / exit ---------------- */

static void camv_vdev_release(struct video_device *vdev)
{
}

static int camv_alloc_dev(int idx, const char *vname,
	const char *card, int nr)
{
	struct camv_dev *d;
	int ret;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	mutex_init(&d->lock);
	spin_lock_init(&d->qlock);
	INIT_LIST_HEAD(&d->bufs);
	init_waitqueue_head(&d->feed_wq);
	d->fps = 15;
	d->index = idx;
	d->card = card;
	d->fmt.width = 640;
	d->fmt.height = 480;
	d->fmt.fourcc = V4L2_PIX_FMT_YUYV;
	d->fmt.bpl = camv_bpl(d->fmt.fourcc, d->fmt.width);
	d->fmt.sizeimage = camv_sizeimage(d->fmt.fourcc, d->fmt.width,
			  d->fmt.height);
	camv_devs[idx] = d;

	strscpy(d->v4l2_dev.name, vname, sizeof(d->v4l2_dev.name));
	ret = v4l2_device_register(NULL, &d->v4l2_dev);
	if (ret) {
		pr_err(CAMV_DRV ": v4l2_device_register(%s) failed %d\n",
		       vname, ret);
		goto err_free;
	}

	d->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	d->queue.io_modes = VB2_MMAP;
	d->queue.mem_ops = &vb2_vmalloc_memops;
	d->queue.drv_priv = d;
	d->queue.buf_struct_size = sizeof(struct camv_buf);
	d->queue.ops = &camv_vb2_ops;
	d->queue.lock = &d->lock;
	d->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	d->queue.allow_zero_bytesused = 1;
	ret = vb2_queue_init(&d->queue);
	if (ret) {
		pr_err(CAMV_DRV ": vb2_queue_init failed %d\n", ret);
		goto err_v4l2;
	}

	strscpy(d->vdev.name, vname, sizeof(d->vdev.name));
	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.fops = &camv_fops;
	d->vdev.ioctl_ops = &camv_ioctl_ops;
	d->vdev.lock = &d->lock;
	d->vdev.queue = &d->queue;
	d->vdev.release = camv_vdev_release;
	d->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(&d->vdev, d);

	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO, nr);
	if (ret) {
		pr_err(CAMV_DRV ": video_register_device(%s) failed %d\n",
		       vname, ret);
		goto err_queue;
	}

	pr_info(CAMV_DRV ": registered %s as /dev/video%d (%s)\n",
	vname, d->vdev.num, card);
	return 0;

err_queue:
	vb2_queue_release(&d->queue);
err_v4l2:
	v4l2_device_unregister(&d->v4l2_dev);
err_free:
	camv_devs[idx] = NULL;
	kfree(d);
	return ret;
}

static void camv_free_dev(struct camv_dev *d)
{
	if (!d)
		return;
	video_unregister_device(&d->vdev);
	vb2_queue_release(&d->queue);
	v4l2_device_unregister(&d->v4l2_dev);
	kfree(d);
}

static int __init camv_init(void)
{
	static const char *const names[2] = { "camv4l2", "camv4l2-front" };
	static const char *const cards[2] = { "MT6835 CAM_A rear",
			      "MT6835 CAM_A front" };
	int i, ret;

	for (i = 0; i < 2; i++) {
		int nr = video_nr < 0 ? -1 : video_nr + i;

		ret = camv_alloc_dev(i, names[i], cards[i], nr);
		if (ret) {
			pr_err(CAMV_DRV ": init node %d failed %d\n", i, ret);
			goto err;
		}
	}
	ret = misc_register(&camfeed_misc);
	if (ret) {
		pr_err(CAMV_DRV ": misc_register failed %d\n", ret);
		goto err;
	}
	pr_info(CAMV_DRV ": dual video nodes ready (rear+front), feed /dev/camfeed\n");
	return 0;

err:
	for (i = 0; i < 2; i++) {
		camv_free_dev(camv_devs[i]);
		camv_devs[i] = NULL;
	}
	return ret;
}

static void __exit camv_exit(void)
{
	int i;

	misc_deregister(&camfeed_misc);
	for (i = 0; i < 2; i++) {
		camv_free_dev(camv_devs[i]);
		camv_devs[i] = NULL;
	}
	WRITE_ONCE(camv_owner, NULL);
	pr_info(CAMV_DRV ": exited\n");
}

module_init(camv_init);
module_exit(camv_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Proxy V4L2 capture node for MT6835 CAM_A pipeline");
MODULE_AUTHOR("project");
