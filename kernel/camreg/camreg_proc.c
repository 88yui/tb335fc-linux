// camreg - 相机寄存器直通 + DMA 缓冲分配
//
// 背景：厂商 camera_isp.ko 的 ISP_READ_REGISTER 实现对内核指针用了 get_user()，
// arm64 上必然返回 -EFAULT，读寄存器这条路在驱动层面是坏的。
// 本模块绕过 camera_isp，直接 ioremap 各相机子系统的寄存器窗口。
//
// 时钟/电源仍由 /dev/camera-isp 的 ISP_POWER_CTRL 管理：
//   上电传 CAMSV 用户索引 10..15（驱动内部 +2 → hw 12..17）
// 未上电就读取被门控的模块会导致总线挂死，使用者自负。
//
// DMA 缓冲挂在 camsv2 平台设备上分配，这样拿到的是该 larb 的 IOMMU IOVA，
// 可以直接写进 CAMSV 的 IMGO_BASE_ADDR。mmap 出去的页面是 write-combine，
// 用户态读到的就是设备写入 DRAM 的内容，不需要额外 cache 维护。
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/cacheflush.h>

#define CAMREG_IOC_MAGIC 'r'

struct camreg_req {
	__u32 addr;	/* 物理地址（必须是下列窗口之一） */
	__u32 val;
};

struct camreg_alloc {
	__u32 size;	/* in: 请求字节数 */
	__u32 handle;	/* out: 缓冲号（也用于 mmap 的 offset） */
	__u64 iova;	/* out: 给设备用的 DMA 地址 */
	__u64 paddr;	/* out: CPU 物理地址 */
};

struct camreg_pa {
	__u64 addr;	/* in: PA(READPA) 或 IOVA(TRANS) */
	__u64 val;	/* out: 32 位值 或 翻译后的 PA */
};

#define CAMREG_RD	_IOWR(CAMREG_IOC_MAGIC, 1, struct camreg_req)
#define CAMREG_WR	_IOW(CAMREG_IOC_MAGIC, 2, struct camreg_req)
#define CAMREG_ALLOC	_IOWR(CAMREG_IOC_MAGIC, 3, struct camreg_alloc)
#define CAMREG_FREE	_IOW(CAMREG_IOC_MAGIC, 4, struct camreg_alloc)
#define CAMREG_READPA	_IOWR(CAMREG_IOC_MAGIC, 5, struct camreg_pa)
#define CAMREG_TRANS	_IOWR(CAMREG_IOC_MAGIC, 6, struct camreg_pa)

#define CAMREG_MAX_BUF	8

struct camreg_region {
	unsigned long base;
	unsigned long len;
	const char *name;
	void __iomem *va;
};

static struct camreg_region g_regions[] = {
	/* SMI 块必须排在 seninf 之前：seninf 的 0xA000 窗口覆盖了
	 * 0x1A00C000/0x1A00D000，靠前匹配才能拿到正确的名字。 */
	{ 0x1A001000, 0x00001000, "smi_larb13", NULL },
	{ 0x1A00C000, 0x00001000, "smi_comm2",  NULL },
	{ 0x1A00D000, 0x00001000, "smi_comm0",  NULL },
	{ 0x1A00F000, 0x00001000, "smi_larb16", NULL },
	{ 0x1A010000, 0x00001000, "smi_larb17", NULL },
	{ 0x1A000000, 0x00001000, "camsys",  NULL },
	{ 0x1A004000, 0x0000A000, "seninf",  NULL },
	{ 0x1A030000, 0x00008000, "cam_a",   NULL },
	{ 0x1A038000, 0x00008000, "cam_a_inner", NULL },
	{ 0x1A04F000, 0x00001000, "camsys_rawa", NULL },
	{ 0x1A050000, 0x00008000, "cam_b",   NULL },
	{ 0x1A070000, 0x00008000, "cam_c",   NULL },
	{ 0x1A090000, 0x00008000, "camsv",   NULL },
	{ 0x11C80000, 0x00010000, "mipi_ana", NULL },
};

struct camreg_buf {
	struct page *page;
	size_t size;
	dma_addr_t iova;
	phys_addr_t paddr;
	bool mapped;
	bool iommu_direct;	/* true = 用 iommu_map() 自己建的映射 */
	bool used;
};

static struct device *g_camdev;
static struct camreg_buf g_bufs[CAMREG_MAX_BUF];

static int camreg_get_dev(void);

/* 读 DRAM 用的单页缓存映射（只读，ioremap_cache） */
static void __iomem *g_mp_va;
static phys_addr_t g_mp_pa;

#define CAMREG_PA_MIN	0x40000000ULL
#define CAMREG_PA_MAX	0x400000000ULL

static int camreg_read_pa(struct camreg_pa *p)
{
	phys_addr_t pa = (phys_addr_t)p->addr;
	unsigned long off;

	if (pa < CAMREG_PA_MIN || pa >= CAMREG_PA_MAX || (pa & 3))
		return -EINVAL;
	off = pa & (PAGE_SIZE - 1);
	if (!g_mp_va || g_mp_pa != (pa & PAGE_MASK)) {
		if (g_mp_va) {
			iounmap(g_mp_va);
			g_mp_va = NULL;
		}
		g_mp_va = ioremap_cache(pa & PAGE_MASK, PAGE_SIZE);
		if (!g_mp_va)
			return -ENOMEM;
		g_mp_pa = pa & PAGE_MASK;
	}
	p->val = (__u64)readl(g_mp_va + off);
	return 0;
}

static int camreg_match_all(struct device *dev, const void *data)
{
	return 1;
}

static int camreg_trans(struct camreg_pa *p)
{
	static const char *devs[] = {
		"cam_mem",
		"1a030000.cam1_legacy",
		"1a04f000.camsys_rawa_legacy",
		"1a000000.camisp_legacy",
		"1a092000.camsv2_legacy",
	};
	struct device *d = NULL, *n;
	int i;

	/* 先按已知名字试 */
	for (i = 0; i < ARRAY_SIZE(devs); i++) {
		struct device *c;
		struct iommu_domain *dom;
		phys_addr_t pa;

		c = bus_find_device_by_name(&platform_bus_type, NULL, devs[i]);
		if (!c)
			continue;
		dom = iommu_get_domain_for_dev(c);
		if (dom) {
			pa = iommu_iova_to_phys(dom, (dma_addr_t)p->addr);
			if (pa) {
				put_device(c);
				p->val = (__u64)pa;
				return 0;
			}
		}
		put_device(c);
	}

	/* 再暴力枚举所有 platform device 的域 */
	while ((n = bus_find_device(&platform_bus_type, d, NULL,
				    camreg_match_all)) != NULL) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(n);
		phys_addr_t pa = 0;

		if (dom)
			pa = iommu_iova_to_phys(dom, (dma_addr_t)p->addr);
		if (pa) {
			p->val = (__u64)pa;
			put_device(n);
			if (d)
				put_device(d);
			return 0;
		}
		if (d)
			put_device(d);
		d = n;
	}
	if (d)
		put_device(d);
	return -ENOENT;
}

/* DMA 缓冲挂在哪个平台设备上分配。MTK 的 IOMMU 按子系统分域：
 *   cam_main（seninf/camsv）、cam_suba（CAMSYS_RAWA → CAM_A）、cam_subb（RAWB）。
 * 只有分配它的那个域的 IOMMU 页表里才有这条映射，用错域的 IOVA/PA 写过去
 * 会被 IOMMU 静默丢弃 —— 表现就是「DMA 使能了、TG 也在跑、但缓冲一个字节不变」。
 * insmod camreg.ko devcamsys=1 时改用 1a04f000.camsys_rawa_legacy（CAM_A 域）。 */
static int g_devcamsys;		/* 0=camsv2（CAMSV 域）, 1=camsys_rawa（CAM_A 域） */
module_param_named(devcamsys, g_devcamsys, int, 0444);

/* 直接指定分配缓冲用的平台设备名。Android 的相机 DMA 全部走 cam_mem 域（IOVA），
 * 而 Linux DT 里相机没有任何 iommus。这里允许挂到别的 IOMMU client 上分配，
 * 拿到一个真正经过 IOMMU 翻译的 IOVA，用它验证「相机端口和别的 domain 共享
 * 同一套页表」这个假设。例：devname=14000000.dispsys_config */
static char g_devname[64];
module_param_string(devname, g_devname, sizeof(g_devname), 0444);

static const char *camreg_devname(void)
{
	if (g_devname[0])
		return g_devname;
	if (g_devcamsys)
		return "1a04f000.camsys_rawa_legacy";
	return "1a092000.camsv2_legacy";
}

static void __iomem *camreg_lookup(__u32 addr, __u32 *off)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(g_regions); i++) {
		if (addr >= g_regions[i].base &&
		    addr < g_regions[i].base + g_regions[i].len) {
			*off = addr - (__u32)g_regions[i].base;
			return g_regions[i].va;
		}
	}
	return NULL;
}

static int camreg_get_dev(void)
{
	struct device *d;

	if (g_camdev)
		return 0;

	d = bus_find_device_by_name(&platform_bus_type, NULL,
				    camreg_devname());
	if (!d) {
		pr_err("camreg: 平台设备 %s 未找到\n", camreg_devname());
		return -ENODEV;
	}
	/* 注意：这里**不要**改设备的 dma_mask。
	 *
	 * 踩过的坑：之前在图省事，直接 dma_set_mask_and_coherent(dispsys_config,
	 * 32 位)。card0 就是 14000000.dispsys_config，掩码被改成 32 位后，
	 * 显示驱动再申请 IOVA 就必然失败（该域 aperture 从 8GB 起）：
	 *     mtk_iova_dbg_alloc fail! dev:14000000.dispsys_config, size:0xf000
	 *     DDP_COMPONENT_DSI0: buffer underrun / OVL0: frame underflow
	 * 表现就是整块屏幕灰屏。而且 rmmod 之后掩码不会自己恢复，只能重启。
	 *
	 * 现在走 iommu_map() 根本不需要 dma_mask，所以一律不动它。 */
	g_camdev = d;
	return 0;
}

/* 只有在没有 IOMMU 域、退化成 dma_map_page 时才需要掩码（改的是相机自己的
 * camsv/cam1 节点，不会影响显示）。 */
static int camreg_ensure_mask(void)
{
	if (g_camdev->dma_mask && *g_camdev->dma_mask)
		return 0;
	if (dma_set_mask_and_coherent(g_camdev, DMA_BIT_MASK(32)) &&
	    dma_set_mask_and_coherent(g_camdev, DMA_BIT_MASK(36)))
		return -EIO;
	return 0;
}

static int camreg_buf_alloc(struct camreg_alloc *a)
{
	struct camreg_buf *b = NULL;
	size_t size = a->size;
	int i, order;

	if (!size || size > (16 << 20))
		return -EINVAL;
	if (camreg_get_dev())
		return -ENODEV;

	for (i = 0; i < CAMREG_MAX_BUF; i++) {
		if (!g_bufs[i].used) {
			b = &g_bufs[i];
			break;
		}
	}
	if (!b)
		return -ENOSPC;

	size = PAGE_ALIGN(size);

	order = get_order(size);
	/* CAMSV 的 IMGO 基址寄存器只有 32 位（外加一个语义不明的 MSB 寄存器
	 * 0x478），所以缓冲必须落在 4GB 以内。实测普通 GFP_KERNEL 分配会拿到
	 * pa=0x164800000（5.7GB）这种高位页，dma_map_page 因 32 位 mask 直接
	 * 失败。这里优先用 GFP_DMA32 从 ZONE_DMA32（<4GB）里拿页，失败再退回
	 * 普通分配（并打印 PA 方便排查）。 */
	b->page = alloc_pages(GFP_KERNEL | __GFP_ZERO | GFP_DMA32, order);
	if (!b->page) {
		pr_warn("camreg: GFP_DMA32 分配 %zu B 失败，退回普通分配\n", size);
		b->page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	}
	if (!b->page)
		return -ENOMEM;
	b->size = PAGE_SIZE << order;
	b->paddr = page_to_phys(b->page);

	/* 关键：alloc_pages(__GFP_ZERO) 是在**内核线性映射（cached）**里把页清零的，
	 * 会留下脏的零值缓存行。用户态 mmap 是 write-combine（uncached），
	 * memset 0xA5 直接写到 DRAM，但这些脏行稍后回写时会把 64 字节整行的 0
	 * 盖回 0xA5 上 —— 实测 4MB 缓冲会出现 ~1.4% 的 64B 对齐零块，
	 * 与相机无关（P129 隔离实验：完全不碰相机寄存器也复现）。
	 * 这里把内核别名刷干净，后续用户态填充的内容才不会被覆盖。 */
	{
		unsigned long np = (PAGE_SIZE << order) >> PAGE_SHIFT;
		unsigned long pi;

		for (pi = 0; pi < np; pi++)
			flush_dcache_page(b->page + pi);
	}

	/* 优先直接往 mtk-iommu 域里建映射。
	 *
	 * 背景：相机 LARB16 port0(IMGO) 处在这个 MM IOMMU 页表里（fault 报
	 * pgtable=0x68810000）。Linux DT 里相机节点没有 iommus，所以内核从来没给
	 * 相机分配过 IOVA；以前的 dma_map_page 在某些测试节点上 dma_ops 是 NULL，
	 * 直接退化成恒等映射（iova==pa），页表里什么都没有，写事务被丢弃。
	 *
	 * 实测只有 dispsys_config / gce 这类设备真有 IOMMU 域（aperture
	 * 8GB..16GB）。而整个 1e802000 IOMMU 只有一张共享页表，所以这里直接用
	 * iommu_map() 把这块页按「恒等映射」写进那张表：PA 一定 <4GB（GFP_DMA32
	 * 保证），正好能放进 32 位的 IMGO_BASE 寄存器。 */
	{
		struct iommu_domain *dom = iommu_get_domain_for_dev(g_camdev);

		if (dom) {
			dma_addr_t cand = (dma_addr_t)b->paddr;
			int r;

			/* 恒等地址若已被别的设备占走，往上找一段空闲的 */
			if (iommu_iova_to_phys(dom, cand))
				cand = 0xF0000000ull - b->size;
			r = iommu_map(dom, (unsigned long)cand, b->paddr, b->size,
				      IOMMU_READ | IOMMU_WRITE);
			if (!r) {
				b->iova = cand;
				b->mapped = true;
				b->iommu_direct = true;
			} else {
				pr_warn("camreg: iommu_map(0x%llx<-0x%llx %zuB) 失败 %d\n",
					(unsigned long long)cand,
					(unsigned long long)b->paddr, b->size, r);
			}
		}
	}

	if (!b->mapped) {
		camreg_ensure_mask();
		b->iova = dma_map_page(g_camdev, b->page, 0, b->size,
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(g_camdev, b->iova)) {
			/* 该平台设备没有挂在 IOMMU 上，dma_map_page 会因 mask 检查失败。
			 * 这时 DMA 地址就是物理地址（恒等映射），直接用 PA。
			 */
			pr_warn("camreg: dma_map_page 失败(pa=0x%llx mask=0x%llx)，回退物理地址\n",
				(unsigned long long)b->paddr,
				(unsigned long long)(g_camdev->dma_mask ?
						     *g_camdev->dma_mask : 0));
			b->iova = (dma_addr_t)b->paddr;
			b->mapped = false;
		} else {
			b->mapped = true;
		}
	}

	b->used = true;
	a->handle = i;
	a->iova = (__u64)b->iova;
	a->paddr = (__u64)page_to_phys(b->page);
	a->size = (__u32)b->size;

	/* 诊断：这块 IOVA 到底进了哪个域、反查能不能翻译回 PA。
	 * bank 是 SMI 端口侧 4GB 窗口号，硬件会把 bank<<32 加进 IOVA 再查表。 */
	{
		struct iommu_domain *dom = iommu_get_domain_for_dev(g_camdev);
		phys_addr_t back = dom ? iommu_iova_to_phys(dom, b->iova) : 0;

		if (dom)
			pr_info("camreg: map #%d iova=0x%llx pa=0x%llx trans=0x%llx aperture=0x%llx..0x%llx dma_ops=%s\n",
				i, (unsigned long long)b->iova,
				(unsigned long long)b->paddr,
				(unsigned long long)back,
				(unsigned long long)dom->geometry.aperture_start,
				(unsigned long long)dom->geometry.aperture_end,
				g_camdev->dma_ops ? "非空" : "NULL");
		else
			pr_info("camreg: map #%d iova=0x%llx pa=0x%llx 该设备没有 IOMMU 域，dma_ops=%s\n",
				i, (unsigned long long)b->iova,
				(unsigned long long)b->paddr,
				g_camdev->dma_ops ? "非空" : "NULL");

		/* 抽查整块缓冲是否每一页都真的映射好了。
		 * 只反查首地址是不够的：4MB = 1024 个 4KB 页，中间任何一页
		 * iommu_map 失败或被别人占走，DMA 写到那里就会被静默丢弃，
		 * 表现正是"写入散布、覆盖率只有两三成"。 */
		if (dom && b->size >= (1u << 20)) {
			unsigned long off;
			char line[160];
			int n = 0;

			line[0] = 0;
			for (off = 0; off < b->size; off += (1u << 20)) {
				phys_addr_t t = iommu_iova_to_phys(dom, b->iova + off);

				n += scnprintf(line + n, sizeof(line) - n,
					       " +0x%lx:%s", off,
					       (t == b->paddr + off) ? "ok" : "BAD");
			}
			pr_info("camreg: 整块抽查 #%d%s\n", i, line);
		}
	}
	return 0;
}

static void camreg_buf_free(struct camreg_buf *b)
{
	if (b->iommu_direct) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(g_camdev);

		if (dom)
			iommu_unmap(dom, (unsigned long)b->iova, b->size);
	} else if (b->mapped) {
		dma_unmap_page(g_camdev, b->iova, b->size, DMA_FROM_DEVICE);
	}
	__free_pages(b->page, get_order(b->size));
	memset(b, 0, sizeof(*b));
}

static long camreg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct camreg_req r;
	struct camreg_alloc a;
	void __iomem *va;
	__u32 off = 0;
	int ret;

	switch (cmd) {
	case CAMREG_RD:
	case CAMREG_WR:
		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;
		va = camreg_lookup(r.addr, &off);
		if (!va)
			return -EINVAL;
		if (cmd == CAMREG_RD) {
			r.val = readl(va + off);
			if (copy_to_user((void __user *)arg, &r, sizeof(r)))
				return -EFAULT;
		} else {
			writel(r.val, va + off);
			wmb();
		}
		return 0;

	case CAMREG_ALLOC:
		if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
			return -EFAULT;
		ret = camreg_buf_alloc(&a);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &a, sizeof(a)))
			return -EFAULT;
		pr_info("camreg: alloc #%u %u B iova=0x%llx pa=0x%llx\n",
			a.handle, a.size, a.iova, a.paddr);
		return 0;

	case CAMREG_FREE:
		if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
			return -EFAULT;
		if (a.handle >= CAMREG_MAX_BUF || !g_bufs[a.handle].used)
			return -EINVAL;
		camreg_buf_free(&g_bufs[a.handle]);
		return 0;

	case CAMREG_READPA:
	case CAMREG_TRANS:
	{
		struct camreg_pa p;

		if (copy_from_user(&p, (void __user *)arg, sizeof(p)))
			return -EFAULT;
		ret = (cmd == CAMREG_READPA) ? camreg_read_pa(&p) : camreg_trans(&p);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &p, sizeof(p)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static int camreg_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long idx = vma->vm_pgoff;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct camreg_buf *b;

	if (idx >= CAMREG_MAX_BUF || !g_bufs[idx].used)
		return -EINVAL;
	b = &g_bufs[idx];
	if (size > b->size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, page_to_pfn(b->page),
			       size, vma->vm_page_prot);
}

static const struct file_operations camreg_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = camreg_ioctl,
	.compat_ioctl = camreg_ioctl,
	.mmap = camreg_mmap,
};

static struct miscdevice camreg_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "camreg",
	.fops = &camreg_fops,
};

static int __init camreg_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(g_regions); i++) {
		g_regions[i].va = ioremap(g_regions[i].base, g_regions[i].len);
		if (!g_regions[i].va) {
			pr_err("camreg: ioremap %s @0x%lx failed\n",
			       g_regions[i].name, g_regions[i].base);
			while (--i >= 0)
				iounmap(g_regions[i].va);
			return -ENOMEM;
		}
	}

	ret = camreg_get_dev();
	if (ret)
		pr_warn("camreg: DMA 不可用（%d），寄存器功能仍可用\n", ret);

	ret = misc_register(&camreg_misc);
	if (ret) {
		pr_err("camreg: misc_register failed %d\n", ret);
		for (i = 0; i < ARRAY_SIZE(g_regions); i++)
			iounmap(g_regions[i].va);
		return ret;
	}
	pr_info("camreg: ready (dma dev %s)\n",
		g_camdev ? dev_name(g_camdev) : "none");
	return 0;
}

static void __exit camreg_exit(void)
{
	int i;

	misc_deregister(&camreg_misc);
	if (g_mp_va) {
		iounmap(g_mp_va);
		g_mp_va = NULL;
	}
	for (i = 0; i < CAMREG_MAX_BUF; i++)
		if (g_bufs[i].used)
			camreg_buf_free(&g_bufs[i]);
	for (i = 0; i < ARRAY_SIZE(g_regions); i++)
		iounmap(g_regions[i].va);
	pr_info("camreg: bye\n");
}

module_init(camreg_init);
module_exit(camreg_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6835 camera register window + DMA buffer");
