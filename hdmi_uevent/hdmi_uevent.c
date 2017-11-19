#define pr_fmt(fmt)	KBUILD_MODNAME ":%s " fmt, __func__
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>

struct miscdevice;
static int hdmi_pin;
static unsigned int hdmi_gpio = 46;
static unsigned int irqNumber;

static irq_handler_t hdmigpio_irq_handler(unsigned int irq, void *dev_id,struct pt_regs *regs);

static ssize_t read_hdmi_status (struct device *dev, struct device_attribute *attr,char *buf){
	return sprintf(buf,"hdmi_status=%d\n",hdmi_pin);
}
static ssize_t change_hdmi_status (struct device *dev, 
						struct device_attribute *attr,char *buf){
	//if(count)
	//	errorurn sscanf(buf,"%d",&type);
		return sprintf(buf,"hdmi_status=%d\n",hdmi_pin);	
}
static DEVICE_ATTR(read, S_IRUSR|S_IRGRP, read_hdmi_status, NULL);
static DEVICE_ATTR(change, S_IRUSR|S_IRGRP, change_hdmi_status, NULL);

static struct attribute *dev_attrs[] = {
	&dev_attr_read.attr,
	&dev_attr_change.attr,
	NULL
};
static struct attribute_group dev_attr_group = {
	.attrs = dev_attrs,NULL
};
static const struct attribute_group *dev_attr_groups[] = {
	&dev_attr_group,
	NULL
};
static struct miscdevice hdmi_dev = {
	.name = "hdmi_device",
	.minor = MISC_DYNAMIC_MINOR,
};
static char *envp[]={"SUBSYSTEM=HDMI",NULL};
static int __init module_start(void)
{
	int error=0;
	static int hdmi_gpio_val = 0;
	pr_info("\n");

	error = misc_register(&hdmi_dev);
	if (error)
		pr_err("error %d\n", error);
	
	if(!gpio_is_valid(hdmi_gpio)){
		printk(KERN_INFO "hdmi hotplug gpio invalid");
		return -ENODEV;
	}
	gpio_request(hdmi_gpio,"sysfs");
	gpio_direction_input(hdmi_gpio);
	gpio_export(hdmi_gpio,false);
	irqNumber = gpio_to_irq(hdmi_gpio);
	error = request_irq(irqNumber,(irq_handler_t)hdmigpio_irq_handler,IRQF_TRIGGER_RISING,"hdmi_gpio_handler",NULL);
while(1){
	hdmi_gpio_val = gpio_get_value(hdmi_gpio);
	while(hdmi_gpio_val){
		hdmi_gpio_val = gpio_get_value(hdmi_gpio);
		msleep(1000);	
	}
	while(!hdmi_gpio_val){
		hdmi_gpio_val = gpio_get_value(hdmi_gpio);
		msleep(1000);	
	}
	printk(KERN_INFO "hotplug removed and value = %d\n",hdmi_gpio_val);
#if 0
	error = device_create_file(dev,&dev_attr_read);
	if (error){
		put_device(dev);
		pr_err("No Read device attribute Added%d\n", error);
	}
	error = device_create_file(dev,&dev_attr_change);
	if (error){
		put_device(dev);
		pr_err("No Change device attribute Added%d\n", error);
	}
#endif
	struct device *dev = hdmi_dev.this_device;
	error = kobject_uevent_env(&dev->kobj,KOBJ_CHANGE,envp);
	if (error){
		kobject_put(&dev->kobj);
		pr_err("No kobject_uevent %d\n", error);
	}
	msleep(1000);
}
	return error;
}

static void __exit module_end(void)
{
	misc_deregister(&hdmi_dev);
	gpio_unexport(hdmi_gpio);
	free_irq(irqNumber,NULL);
	gpio_free(hdmi_gpio);
	pr_info("\n");
}

static irq_handler_t hdmigpio_irq_handler(unsigned int irq, void *dev_id, struct pt_regs *regs){
	int error;
	printk(KERN_INFO "\nDetected hdmi hotplug event and inside IRQ\n");
	struct device *dev = hdmi_dev.this_device;
	error = kobject_uevent_env(&dev->kobj,KOBJ_CHANGE,envp);
	if (error){
		kobject_put(&dev->kobj);
		pr_err("No kobject_uevent %d\n", error);
	}
	return (irq_handler_t) IRQ_HANDLED;
}

module_init(module_start);
module_exit(module_end);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HDMI Device");

