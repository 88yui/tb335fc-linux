// SPDX-License-Identifier: GPL-2.0-only
// regwin - map arbitrary physical window and dump registers via /proc
// insmod regwin.ko base=0x15021000 size=0x1000   (hex values)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>

static unsigned long base = 0x15021000UL;
static unsigned long size = 0x1000UL;
module_param(base, ulong, 0444);
module_param(size, ulong, 0444);

static void __iomem *va;

static int rw_show(struct seq_file *m, void *v)
{
	unsigned long i;
	for (i = 0; i + 4 <= size; i += 4)
		seq_printf(m, "0x%08lx=0x%08x\n", i, readl(va + i));
	return 0;
}

static int rw_open(struct inode *in, struct file *f)
{
	return single_open(f, rw_show, NULL);
}

static const struct proc_ops rw_ops = {
	.proc_open = rw_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init rw_init(void)
{
	va = ioremap(base, size);
	if (!va) {
		pr_err("regwin: ioremap failed\n");
		return -ENOMEM;
	}
	if (!proc_create("regwin", 0444, NULL, &rw_ops)) {
		iounmap(va);
		return -ENOMEM;
	}
	pr_info("regwin: ready base=0x%lx size=0x%lx\n", base, size);
	return 0;
}

static void __exit rw_exit(void)
{
	remove_proc_entry("regwin", NULL);
	iounmap(va);
	pr_info("regwin: exit\n");
}

module_init(rw_init);
module_exit(rw_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("register window dump");
