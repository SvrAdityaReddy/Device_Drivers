/*
 * Example of a misc device managed by misc driver and
 * operated through the legacy /dev/goonj1 file
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ":%s " fmt, __func__

#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/miscdevice.h>

static struct {
	char buf[1024];
	loff_t written;
} goonj1;

static ssize_t read_goonj(struct file *filp, char __user *buf,
		size_t count, loff_t *ppos)
{
	ssize_t len;
	loff_t where = filp->f_pos;

	pr_info("%zu bytes at %lld\n", count, where);
	if (where >= goonj1.written)
		return 0;	/* for end of file */
	len = goonj1.written - where;
	if (count < len)
		len = count;
	if(copy_to_user(buf, &goonj1.buf[where], len))
		return -EINVAL;
	pr_info("len %zu bytes\n", len);
	*ppos += len;
	filp->f_pos += len;
	return len;
}

static ssize_t write_goonj(struct file *filp, const char __user *buf,
		size_t count, loff_t *ppos)
{
	loff_t from = filp->f_pos;
	ssize_t len;

	pr_info("%zu bytes from %lld\n", count, from);
	if (from >= sizeof(goonj1.buf))
		return -ENOSPC;
	else if (count > sizeof(goonj1.buf) - from)
		len = sizeof(goonj1.buf) - from;
	else
		len = count;

	if (copy_from_user(&goonj1.buf[from], buf, len))
		return -EINVAL;
	pr_info("len %zu bytes\n", len);
	goonj1.written = from + len;
	*ppos += len;
	filp->f_pos += len;
	return len;
}

static const struct file_operations goonj_fops = {
	.owner	= THIS_MODULE,
	.read	= read_goonj,
	.write 	= write_goonj,
};

static struct miscdevice goonjdev = {
	.name = "goonj1",
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &goonj_fops,
};

static int __init goonj_init(void)
{
	int ret;

	pr_info("\n");

	ret = misc_register(&goonjdev);
	if (ret)
		pr_err("error %d\n", ret);
	return ret;
}

static void __exit goonj_exit(void)
{
	misc_deregister(&goonjdev);
	pr_info("\n");
}

module_init(goonj_init);
module_exit(goonj_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual Echo Device");
