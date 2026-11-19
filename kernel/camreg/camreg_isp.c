// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/of_platform.h>
#include <soc/mediatek/smi.h>
#include <asm/cacheflush.h>

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
/* 单块缓冲最多切几段。8 × 4 MB = 32 MB，与 alloc 上限一致。 */
#define CAMREG_MAX_CHUNK	8

struct camreg_region {
	unsigned long base;
	unsigned long len;
	const char *name;
	void __iomem *va;
};

static struct camreg_region g_regions[] = {
	/* Debian 侧 ISP / imgsys 窗口：覆盖 camsys / seninf / cam_a / camsv */
	{ 0x1A000000, 0x00100000, "isp",     NULL },
	{ 0x15000000, 0x00030000, "imgsys",  NULL },
	/* MIPI CSI-RX 模拟前端（CD-PHY / ANA）。
	 * 依据 imgsensor/src/isp6s/seninf/seninf_cfg.h：
	 *   SENINF_MAP_BASE_ANA  = 0x11C80000
	 *   SENINF_MAP_LENGTH_ANA= 0x10000
	 * 缺了它 camcap 的 ANA/DPHY 配置（供电/EQ/lane/settle）会全部
	 * EINVAL 静默失败，CSI2 pkt_cnt 恒 0 —— 这是"sensor 出流但收不到帧"
	 * 的根因。 */
	{ 0x11C80000, 0x00010000, "ana",     NULL },
	/* TOPCKGEN：CLK_CFG_2 / CLK_CFG_UPDATE 在 0x10000000 段，
	 * 用来验证 camtg 24M 是否真的被选中（SET_MCLK 是否生效）。 */
	{ 0x10000000, 0x00001000, "topckgen", NULL },
};

struct camreg_buf {
	/* 大缓冲不再要求 buddy 给出连续页：按 2^chunk_order 切成 nchunks 块，
	 * 逐块 iommu_map 到一段连续 IOVA，DMA 侧看到的仍是连续空间。 */
	struct page *pages[CAMREG_MAX_CHUNK];
	int nchunks;
	int chunk_order;
	size_t size;
	dma_addr_t iova;
	phys_addr_t paddr;	/* 首块 PA，仅用于诊断打印 */
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

/* P136: 打开 SMI larb 通道 —— camera_mem 驱动 CAM_MEM_POWER_CTRL 的等价物
 * （CamMem_EnableLarb -> smi_larbs_get -> mtk_smi_larb_get）。
 * 我们没编 camera_mem，由 camreg 代劳。不打开的话 IOMMU 层面不报错
 * （DMA_ERR 已消失），但 SMI 把流量丢掉，FBC 一个块都分不到。 */
static struct device *g_larbdev;

static int camreg_larb_get(void)
{
	struct device_node *lnp;
	struct platform_device *lpdev;
	int ret;

	if (g_larbdev)
		return 0;
	lnp = of_parse_phandle(g_camdev->of_node, "mediatek,larb", 0);
	if (!lnp) {
		pr_err("camreg: mediatek,larb 未定义\n");
		return -ENOENT;
	}
	lpdev = of_find_device_by_node(lnp);
	of_node_put(lnp);
	if (!lpdev) {
		pr_err("camreg: larb 平台设备未找到\n");
		return -ENODEV;
	}
	g_larbdev = &lpdev->dev;
	/* 防护：smi_larb 未 probe 时 drvdata 为 NULL，
	 * mtk_smi_larb_get 内部会解引用 -> oops/重启。 */
	if (!dev_get_drvdata(g_larbdev)) {
		pr_err("camreg: larb drvdata=NULL，未 probe，跳过\n");
		g_larbdev = NULL;
		return -ENODEV;
	}
	pr_info("camreg: larb dev=%s，调用 mtk_smi_larb_get\n", dev_name(g_larbdev));
	ret = mtk_smi_larb_get(g_larbdev);
	if (ret) {
		pr_err("camreg: mtk_smi_larb_get 失败 %d\n", ret);
		g_larbdev = NULL;
		return ret;
	}
	pr_info("camreg: SMI larb 已打开 (%s)\n", dev_name(g_larbdev));
	return 0;
}

static void camreg_larb_put(void)
{
	if (g_larbdev) {
		mtk_smi_larb_put(g_larbdev);
		g_larbdev = NULL;
	}
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

/* 只读诊断（20261114-B）：比对 gce 与 cam1_legacy 两个 device 的 IOMMU 域，
 * 并对给定 IOVA 窗口按 4 KB 抽样反查翻译结果。
 * 只读：不改映射、不改窗口、不写任何寄存器。 */
static void camreg_dom_probe(const char *tag, dma_addr_t iova, size_t size)
{
	struct device *d2;
	struct iommu_domain *dg = NULL, *dc = NULL;
	unsigned long off, step;

	if (!g_camdev)
		return;
	dg = iommu_get_domain_for_dev(g_camdev);
	d2 = bus_find_device_by_name(&platform_bus_type, NULL,
				     "1a030000.cam1_legacy");
	if (d2) {
		dc = iommu_get_domain_for_dev(d2);
		put_device(d2);
	}
	pr_info("camreg: domprobe[%s] gce_dom=%px cam1_dom=%px same=%d iova=0x%llx size=0x%zx\n",
		tag, dg, dc, (dg && dg == dc),
		(unsigned long long)iova, size);
	if (!size)
		return;
	step = size > (16u * PAGE_SIZE) ? (size / 16) : PAGE_SIZE;
	step = round_down(step, PAGE_SIZE);
	if (!step)
		step = PAGE_SIZE;
	for (off = 0; off < size; off += step)
		pr_info("camreg: domprobe[%s] iova=0x%llx gce->0x%llx cam1->0x%llx\n",
			tag, (unsigned long long)(iova + off),
			(unsigned long long)(dg ? iommu_iova_to_phys(dg, iova + off) : 0),
			(unsigned long long)(dc ? iommu_iova_to_phys(dc, iova + off) : 0));
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
	int i;

	if (!size || size > (32 << 20))
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

	/* CAMSV 的 IMGO 基址寄存器只有 32 位（外加一个语义不明的 MSB 寄存器
	 * 0x478），所以缓冲必须落在 4GB 以内：每块都用 GFP_DMA32 从 ZONE_DMA32
	 * 里拿页，失败再退回普通分配（并打印 PA 方便排查）。
	 *
	 * 一次 alloc_pages(order) 拿 10/12/16 MB 分别要 order 11/12/12，buddy
	 * 里没有这么大的连续块 → 直接 -ENOMEM（4 MB = order 10 能过）。所以这里
	 * 按 4 MB 切块申请，失败逐级降到 2 MB / 1 MB。 */
	for (b->chunk_order = 10; b->chunk_order >= 8; b->chunk_order--) {
		size_t chunk = PAGE_SIZE << b->chunk_order;
		int n = DIV_ROUND_UP(size, chunk);
		int k;

		if (n > CAMREG_MAX_CHUNK)
			continue;
		for (k = 0; k < n; k++) {
			b->pages[k] = alloc_pages(GFP_KERNEL | __GFP_ZERO | GFP_DMA32,
						  b->chunk_order);
			if (!b->pages[k])
				break;
		}
		if (k == n) {
			b->nchunks = n;
			break;
		}
		while (--k >= 0)
			__free_pages(b->pages[k], b->chunk_order);
		memset(b->pages, 0, sizeof(b->pages));
	}
	if (!b->nchunks)
		return -ENOMEM;
	b->size = (size_t)b->nchunks * (PAGE_SIZE << b->chunk_order);
	b->paddr = page_to_phys(b->pages[0]);

	/* 关键：alloc_pages(__GFP_ZERO) 是在**内核线性映射（cached）**里把页清零的，
	 * 会留下脏的零值缓存行。用户态 mmap 是 write-combine（uncached），
	 * memset 0xA5 直接写到 DRAM，但这些脏行稍后回写时会把 64 字节整行的 0
	 * 盖回 0xA5 上 —— 实测 4MB 缓冲会出现 ~1.4% 的 64B 对齐零块，
	 * 与相机无关（P129 隔离实验：完全不碰相机寄存器也复现）。
	 * 注意：本内核的 flush_dcache_page() 只清 PG_dcache_clean 标志，**不清缓存**
	 * （arch/arm64/mm/flush.c 实证），所以这里直接对线性别名做 dc civac。 */
	{
		int k;

		for (k = 0; k < b->nchunks; k++) {
			unsigned long st = (unsigned long)page_address(b->pages[k]);
			unsigned long en = st + (PAGE_SIZE << b->chunk_order);
			unsigned long a;

			for (a = st & ~63UL; a < en; a += 64)
				asm volatile("dc civac, %0" :: "r"(a) : "memory");
		}
		asm volatile("dsb sy" ::: "memory");
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
			size_t chunk = PAGE_SIZE << b->chunk_order;
			dma_addr_t base = 0x68000000ull;	/* 历史成功窗口 */
			int ok = 0;

			/* 从 0x68000000 起找一段 size 连续、且每一块都没被占的 IOVA。
			 * 逐块 iommu_map 到这段连续 IOVA 上，DMA 侧看到的仍是连续空间。 */
			for (; base + b->size <= 0xF0000000ull; base += chunk) {
				size_t off;

				for (off = 0; off < b->size; off += chunk)
					if (iommu_iova_to_phys(dom, base + off))
						break;
				if (off >= b->size) {
					ok = 1;
					break;
				}
			}
			if (ok) {
				int k, r = 0;

				for (k = 0; k < b->nchunks; k++) {
					size_t len = chunk;

					if (k == b->nchunks - 1)
						len = b->size - (size_t)k * chunk;
					r = iommu_map(dom,
						      (unsigned long)(base + (size_t)k * chunk),
						      page_to_phys(b->pages[k]), len,
						      IOMMU_READ | IOMMU_WRITE);
					if (r) {
						while (--k >= 0)
							iommu_unmap(dom,
							    (unsigned long)(base + (size_t)k * chunk),
							    chunk);
						break;
					}
				}
				if (!r) {
					b->iova = base;
					b->mapped = true;
					b->iommu_direct = true;
				} else {
					pr_warn("camreg: 分块 iommu_map 失败 %d（iova=0x%llx size=%zu）\n",
r, (unsigned long long)base, b->size);
				}
			} else {
				pr_warn("camreg: 找不到 %zu B 连续空闲 IOVA\n", b->size);
			}
		}
	}

	if (!b->mapped) {
		/* 没有 IOMMU 域时只能退回 dma_map_page，而它要求一次连续的页。
		 * 多块缓冲在这里无法回退，直接释放失败。 */
		if (b->nchunks != 1) {
			int k;

			pr_err("camreg: 无 IOMMU 域且缓冲为 %d 块，无法回退\n", b->nchunks);
			for (k = 0; k < b->nchunks; k++)
				__free_pages(b->pages[k], b->chunk_order);
			memset(b, 0, sizeof(*b));
			return -ENODEV;
		}
		camreg_ensure_mask();
		b->iova = dma_map_page(g_camdev, b->pages[0], 0, b->size,
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
	a->paddr = (__u64)b->paddr;
	a->size = (__u32)b->size;
	pr_info("camreg: alloc #%d %zu B = %d 块 × %lu KB\n", i, b->size,
b->nchunks, (PAGE_SIZE << b->chunk_order) >> 10);

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

		/* 只读：同一段 IOVA 在 gce 域与 cam1_legacy 域各翻译一次 */
		camreg_dom_probe("map", b->iova, b->size);

		/* 抽查整块缓冲是否每一页都真的映射好了。
		 * 只反查首地址是不够的：中间任何一页 iommu_map 失败或被别人占走，
		 * DMA 写到那里就会被静默丢弃，表现正是"写入散布、覆盖率只有两三成"。
		 * 分块后每块 PA 不同，期望值要按块算：chunk k 的 PA + 块内偏移。 */
		if (dom && b->size >= (1u << 20)) {
			size_t chunk = PAGE_SIZE << b->chunk_order;
			unsigned long off;
			char line[512];
			int n = 0;

			line[0] = 0;
			for (off = 0; off < b->size; off += (1u << 20)) {
				phys_addr_t want = page_to_phys(b->pages[off / chunk]) +
						   (off % chunk);
				phys_addr_t t = iommu_iova_to_phys(dom, b->iova + off);

				n += scnprintf(line + n, sizeof(line) - n,
					       " +0x%lx:%s", off,
					       (t == want) ? "ok" : "BAD");
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
	{
		int k;

		for (k = 0; k < b->nchunks; k++)
			if (b->pages[k])
				__free_pages(b->pages[k], b->chunk_order);
	}
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
	/* 分块缓冲在物理上不连续，必须逐块 remap；只 remap 第一块的 PA
	 * 会让用户态看到的第二块起全是别的内存。 */
	{
		size_t chunk = PAGE_SIZE << b->chunk_order;
		unsigned long va = vma->vm_start;
		size_t left = size;
		int k;

		for (k = 0; k < b->nchunks && left; k++) {
			size_t len = (left < chunk) ? left : chunk;
			int r = remap_pfn_range(vma, va, page_to_pfn(b->pages[k]),
						len, vma->vm_page_prot);

			if (r)
				return r;
			va += len;
			left -= len;
		}
	}
	return 0;
}

static const struct file_operations camreg_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = camreg_ioctl,
	.compat_ioctl = camreg_ioctl,
	.mmap = camreg_mmap,
};

static struct miscdevice camreg_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "camreg_isp",
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
	else if (0) /* P136 回退：实测引发设备重启（3 次），禁用待查 */
		camreg_larb_get();
	else
		camreg_dom_probe("init", (dma_addr_t)0x68000000ull,
				 16u * PAGE_SIZE);

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
	camreg_larb_put();
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
