// SPDX-License-Identifier: GPL-2.0-only
// afreg - VCM(AF) i2c 直通：读/写对焦执行器寄存器并扫描
// 用法：insmod afreg.ko bus=8 addr=0x18
//   echo "r8 00" > /proc/afreg ; cat /proc/afreg
//   echo "w8 00 12" > /proc/afreg
//   echo "scan8 0 16" > /proc/afreg
//   echo "r16 0000" / "w16 0000 12" (16 位寄存器地址 + 8 位数据)
//   echo "r16d16 0000" / "w16d16 0000 0123" (16 位地址 + 16 位数据)
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>

static int bus = 8;
module_param(bus, int, 0644);
static int addr = 0x18;
module_param(addr, int, 0644);

static struct i2c_adapter *adap;
static char outbuf[8192];
static int outlen;

static int i2c_w(struct i2c_adapter *a, u16 reg, int regbits, u32 val, int dlen)
{
	u8 buf[4];
	int n = regbits / 8, i;
	if (regbits == 16) { buf[0] = (reg >> 8) & 0xff; buf[1] = reg & 0xff; }
	else buf[0] = reg & 0xff;
	for (i = 0; i < dlen; i++)
		buf[n + i] = (val >> (8 * (dlen - 1 - i))) & 0xff;
	{
		struct i2c_msg m = { .addr = addr, .flags = 0, .len = n + dlen, .buf = buf };
		return i2c_transfer(a, &m, 1);
	}
}

static int i2c_r(struct i2c_adapter *a, u16 reg, int regbits, u32 *val, int dlen)
{
	u8 rb[2];
	u8 data[2] = {0, 0};
	struct i2c_msg m[2];
	int n = regbits / 8, ret, i;
	if (regbits == 16) { rb[0] = (reg >> 8) & 0xff; rb[1] = reg & 0xff; }
	else rb[0] = reg & 0xff;
	m[0].addr = addr; m[0].flags = 0; m[0].len = n; m[0].buf = rb;
	m[1].addr = addr; m[1].flags = I2C_M_RD; m[1].len = dlen; m[1].buf = data;
	ret = i2c_transfer(a, m, 2);
	if (ret != 2) return ret < 0 ? ret : -EIO;
	*val = 0;
	for (i = 0; i < dlen; i++) *val = (*val << 8) | data[i];
	return 0;
}

static void app(const char *fmt, ...)
{
	va_list ap;
	int n;
	if (outlen >= (int)sizeof(outbuf) - 2) return;
	va_start(ap, fmt);
	n = vscnprintf(outbuf + outlen, sizeof(outbuf) - outlen, fmt, ap);
	va_end(ap);
	if (n > 0) outlen += n;
}

static ssize_t af_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	char *kbuf, op[16];
	unsigned int a1 = 0, a2 = 0;
	int ret = 0;

	if (len == 0 || len > 4096) return -EINVAL;
	kbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(kbuf)) return PTR_ERR(kbuf);
	outlen = 0;
	if (!adap) { app("ERR no adapter (bus=%d)\n", bus); kfree(kbuf); return len; }

	if (sscanf(kbuf, "%15s %x %x", op, &a1, &a2) < 1) {
		app("ERR parse\n"); kfree(kbuf); return -EINVAL;
	}
	app("CMD %s %x %x\n", op, a1, a2);
	if (!strcmp(op, "r8")) {
		u32 v = 0; ret = i2c_r(adap, a1, 8, &v, 1);
		app("r8 %02x = %02x ret=%d\n", a1, v, ret);
	} else if (!strcmp(op, "w8")) {
		ret = i2c_w(adap, a1, 8, a2, 1);
		app("w8 %02x <- %02x ret=%d\n", a1, a2, ret);
	} else if (!strcmp(op, "r16")) {
		u32 v = 0; ret = i2c_r(adap, a1, 16, &v, 1);
		app("r16 %04x = %02x ret=%d\n", a1, v, ret);
	} else if (!strcmp(op, "w16")) {
		ret = i2c_w(adap, a1, 16, a2, 1);
		app("w16 %04x <- %02x ret=%d\n", a1, a2, ret);
	} else if (!strcmp(op, "r16d16")) {
		u32 v = 0; ret = i2c_r(adap, a1, 16, &v, 2);
		app("r16d16 %04x = %04x ret=%d\n", a1, v, ret);
	} else if (!strcmp(op, "w16d16")) {
		ret = i2c_w(adap, a1, 16, a2, 2);
		app("w16d16 %04x <- %04x ret=%d\n", a1, a2, ret);
	} else if (!strcmp(op, "scan8")) {
		unsigned int i;
		if (!a2) a2 = 16;
		for (i = 0; i < a2; i++) {
			u32 v = 0xdeadbeef; ret = i2c_r(adap, a1 + i, 8, &v, 1);
			app("%02x=%02x%s", a1 + i, v & 0xff, ret ? "(E)" : " ");
			if ((i % 8) == 7) app("\n");
		}
		app("\n");
	} else if (!strcmp(op, "scan16")) {
		unsigned int i;
		if (!a2) a2 = 16;
		for (i = 0; i < a2; i++) {
			u32 v = 0xdeadbeef; ret = i2c_r(adap, a1 + i, 16, &v, 1);
			app("%04x=%02x%s", a1 + i, v & 0xff, ret ? "(E)" : " ");
			if ((i % 8) == 7) app("\n");
		}
		app("\n");
	} else if (!strcmp(op, "ww")) {
		/* 8 位寄存器 + 2 字节数据（VCM DAC: reg=0x03, val=10bit） */
		u8 b[3];
		struct i2c_msg m;
		b[0] = a1 & 0xff; b[1] = (a2 >> 8) & 0xff; b[2] = a2 & 0xff;
		m.addr = (u16)addr; m.flags = 0; m.len = 3; m.buf = b;
		ret = i2c_transfer(adap, &m, 1);
		app("ww %02x <- %04x ret=%d\n", a1, a2, ret);
	} else if (!strcmp(op, "ping")) {
		u8 b = 0;
		struct i2c_msg m = { .addr = (u16)a1, .flags = 0, .len = 1, .buf = &b };
		ret = i2c_transfer(adap, &m, 1);
		app("ping %02x ret=%d %s\n", a1, ret, ret == 1 ? "ACK" : "NAK");
	} else if (!strcmp(op, "busscan")) {
		unsigned int i;
		app("busscan %s:", adap->name);
		for (i = 0x08; i < 0x78; i++) {
			u8 b = 0;
			struct i2c_msg m = { .addr = (u16)i, .flags = 0, .len = 1, .buf = &b };
			if (i2c_transfer(adap, &m, 1) == 1) app(" %02x", i);
		}
		app("\n");
	} else if (!strcmp(op, "info")) {
		app("bus=%d addr=0x%02x adapter=%s\n", bus, addr, adap->name);
	} else {
		app("ERR unknown op\n");
	}
	kfree(kbuf);
	return len;
}

static ssize_t af_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	ssize_t n = outlen;
	if (*off >= outlen) return 0;
	if (n > (ssize_t)len) n = len;
	if (copy_to_user(ubuf, outbuf + *off, n)) return -EFAULT;
	*off += n;
	return n;
}

static const struct proc_ops af_fops = {
	.proc_write = af_write,
	.proc_read  = af_read,
};

static int __init af_init(void)
{
	adap = i2c_get_adapter(bus);
	if (!adap) return -ENODEV;
	proc_create("afreg", 0666, NULL, &af_fops);
	pr_info("afreg: bus=%d addr=0x%02x adapter=%s\n", bus, addr, adap->name);
	return 0;
}
static void __exit af_exit(void)
{
	remove_proc_entry("afreg", NULL);
	if (adap) i2c_put_adapter(adap);
}
module_init(af_init);
module_exit(af_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VCM i2c direct access");
