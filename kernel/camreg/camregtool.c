// camregtool - 通过 /dev/camreg 读写相机寄存器窗口 + 管理 DMA 缓冲
//   camregtool rd  <addr> [count]
//   camregtool wr  <addr> <val>
//   camregtool alloc <size>
//   camregtool dump <handle> <bytes> [outfile]
//   camregtool free <handle>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define CAMREG_IOC_MAGIC 'r'
struct camreg_req {
	uint32_t addr;
	uint32_t val;
};
struct camreg_alloc {
	uint32_t size;
	uint32_t handle;
	uint64_t iova;
	uint64_t paddr;
};
#define CAMREG_RD	_IOWR(CAMREG_IOC_MAGIC, 1, struct camreg_req)
#define CAMREG_WR	_IOW(CAMREG_IOC_MAGIC, 2, struct camreg_req)
#define CAMREG_ALLOC	_IOWR(CAMREG_IOC_MAGIC, 3, struct camreg_alloc)
#define CAMREG_FREE	_IOW(CAMREG_IOC_MAGIC, 4, struct camreg_alloc)
struct camreg_pa {
	uint64_t addr;
	uint64_t val;
};
#define CAMREG_READPA	_IOWR(CAMREG_IOC_MAGIC, 5, struct camreg_pa)
#define CAMREG_TRANS	_IOWR(CAMREG_IOC_MAGIC, 6, struct camreg_pa)

static int fd = -1;

static int open_dev(void)
{
	fd = open("/dev/camreg", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open /dev/camreg: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/* 返回 handle，-1 失败；iova/paddr/size 回填 */
static int do_alloc(unsigned int size, uint64_t *iova, uint64_t *paddr,
		    unsigned int *real)
{
	struct camreg_alloc a = { 0 };

	a.size = size;
	if (ioctl(fd, CAMREG_ALLOC, &a) < 0) {
		fprintf(stderr, "alloc: %s\n", strerror(errno));
		return -1;
	}
	if (iova)
		*iova = a.iova;
	if (paddr)
		*paddr = a.paddr;
	if (real)
		*real = a.size;
	return (int)a.handle;
}

int main(int argc, char **argv)
{
	struct camreg_req r;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s rd <addr> [count]\n"
			"       %s wr <addr> <val>\n"
			"       %s alloc <size>\n"
			"       %s dump <handle> <bytes> [outfile]\n"
			"       %s free <handle>\n"
			"       %s pa <iova>        # IOVA -> PA（遍历 cam 相关域）\n"
			"       %s rp <pa>          # 读物理地址 32 位\n", argv[0], argv[0], argv[0],
			argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}
	if (open_dev())
		return 1;

	if (!strcmp(argv[1], "pa")) {
		struct camreg_pa p = { 0 };

		p.addr = strtoull(argv[2], NULL, 0);
		if (ioctl(fd, CAMREG_TRANS, &p) < 0) {
			printf("TRANS iova=0x%llx -> 未映射（errno=%d %s）\n",
			       (unsigned long long)p.addr, errno, strerror(errno));
			return 0;
		}
		printf("TRANS iova=0x%llx -> pa=0x%llx\n",
		       (unsigned long long)p.addr, (unsigned long long)p.val);
		return 0;
	}

	if (!strcmp(argv[1], "rp")) {
		struct camreg_pa p = { 0 };

		p.addr = strtoull(argv[2], NULL, 0);
		if (ioctl(fd, CAMREG_READPA, &p) < 0) {
			printf("READPA pa=0x%llx 失败（errno=%d %s）\n",
			       (unsigned long long)p.addr, errno, strerror(errno));
			return 0;
		}
		printf("READPA pa=0x%llx val=0x%08x\n",
		       (unsigned long long)p.addr, (unsigned int)p.val);
		return 0;
	}

	if (!strcmp(argv[1], "rd")) {
		unsigned long base = strtoul(argv[2], NULL, 0);
		int i, n = argc > 3 ? atoi(argv[3]) : 1;

		for (i = 0; i < n; i++) {
			r.addr = (uint32_t)(base + i * 4);
			r.val = 0;
			if (ioctl(fd, CAMREG_RD, &r) < 0) {
				fprintf(stderr, "rd 0x%08x: %s\n", r.addr,
					strerror(errno));
				return 1;
			}
			printf("0x%08x = 0x%08x\n", r.addr, r.val);
		}
		return 0;
	}

	if (!strcmp(argv[1], "wr") && argc > 3) {
		r.addr = (uint32_t)strtoul(argv[2], NULL, 0);
		r.val = (uint32_t)strtoul(argv[3], NULL, 0);
		if (ioctl(fd, CAMREG_WR, &r) < 0) {
			fprintf(stderr, "wr 0x%08x: %s\n", r.addr,
				strerror(errno));
			return 1;
		}
		printf("0x%08x <= 0x%08x\n", r.addr, r.val);
		return 0;
	}

	if (!strcmp(argv[1], "alloc")) {
		uint64_t iova = 0, paddr = 0;
		unsigned int real = 0;
		int h = do_alloc((unsigned int)strtoul(argv[2], NULL, 0), &iova,
				 &paddr, &real);

		if (h < 0)
			return 1;
		printf("handle=%d size=%u iova=0x%llx paddr=0x%llx\n", h, real,
		       (unsigned long long)iova, (unsigned long long)paddr);
		return 0;
	}

	if (!strcmp(argv[1], "dump") && argc > 3) {
		int h = atoi(argv[2]);
		unsigned long n = strtoul(argv[3], NULL, 0);
		unsigned char *p;
		unsigned long i, nz = 0;

		if (n == 0)
			n = 4096;
		p = mmap(NULL, n, PROT_READ, MAP_SHARED, fd,
			 (off_t)h * 4096);
		if (p == MAP_FAILED) {
			fprintf(stderr, "mmap: %s\n", strerror(errno));
			return 1;
		}
		for (i = 0; i < n; i++)
			if (p[i])
				nz++;
		printf("handle=%d bytes=%lu nonzero=%lu (%.1f%%)\n", h, n, nz,
		       n ? 100.0 * nz / n : 0.0);
		printf("head:");
		for (i = 0; i < 64 && i < n; i++)
			printf(" %02x", p[i]);
		printf("\n");
		if (argc > 4) {
			FILE *f = fopen(argv[4], "wb");

			if (!f) {
				fprintf(stderr, "open %s: %s\n", argv[4],
					strerror(errno));
				return 1;
			}
			fwrite(p, 1, n, f);
			fclose(f);
			printf("written %lu bytes to %s\n", n, argv[4]);
		}
		munmap(p, n);
		return 0;
	}

	if (!strcmp(argv[1], "free") && argc > 2) {
		struct camreg_alloc a = { 0 };

		a.handle = (uint32_t)strtoul(argv[2], NULL, 0);
		if (ioctl(fd, CAMREG_FREE, &a) < 0) {
			fprintf(stderr, "free: %s\n", strerror(errno));
			return 1;
		}
		printf("freed %u\n", a.handle);
		return 0;
	}

	fprintf(stderr, "bad args\n");
	return 1;
}
