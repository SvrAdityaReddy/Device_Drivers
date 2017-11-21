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
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <soc/bcm2835/raspberrypi-firmware.h>
#include <linux/timer.h>

#define secs_to_jiffies(i) (msecs_to_jiffies((i)*1000))
#define interval 5
struct miscdevice;
static int hdmi_pin;
static unsigned int hdmi_gpio = 46;
static int flag=0;


struct edid_tag{
	u32 block_number;
	u32 status;
	unsigned char edid_block[128];
}edid_tag1;

static struct timer_list my_timer;
void my_timer_callback(unsigned long data);

static void get_projector_id(struct device_node *fwr, struct rpi_firmware *fw){
	
	int error;
	printk(KERN_INFO "init\n");
	printk(KERN_INFO "rpi_firmware_get := %p",fw);
	edid_tag1.block_number = 0;
	edid_tag1.status = 0;
	for(error=0;error<128;error++)
		edid_tag1.edid_block[error] = 0;
	error = rpi_firmware_property(fw,RPI_FIRMWARE_GET_EDID_BLOCK,&edid_tag1,sizeof(edid_tag1));
	printk(KERN_INFO "Block No:%x, status:%x, error:%d\n",edid_tag1.block_number,edid_tag1.status,error);
	for(error=0;error<128;error++){
		printk("%c",edid_tag1.edid_block[error]);
	}
	printk("\n");
}


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
	int error = 0;
	pr_info("\n");
	
	error = misc_register(&hdmi_dev);
	if (error)
		pr_err("error %d\n", error);
	
	struct device *dev = hdmi_dev.this_device;

	error = kobject_uevent_env(&dev->kobj,KOBJ_CHANGE,envp);
	if (error){
		kobject_put(&dev->kobj);
		pr_err("No kobject_uevent %d\n", error);
	}

	if(!gpio_is_valid(hdmi_gpio)){
		printk(KERN_INFO "hdmi hotplug gpio invalid");
		return -ENODEV;
	}

	gpio_request(hdmi_gpio,"sysfs");
	gpio_direction_input(hdmi_gpio);
	gpio_export(hdmi_gpio,false);


	setup_timer(&my_timer,my_timer_callback,0);
	error = mod_timer(&my_timer,secs_to_jiffies(interval));
	if(error)
		printk(KERN_INFO "timer error");

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
	return 0;
}

static void __exit module_end(void)
{
	misc_deregister(&hdmi_dev);
	gpio_unexport(hdmi_gpio);
	gpio_free(hdmi_gpio);
	del_timer(&my_timer);
	pr_info("\n");
}


void my_timer_callback(unsigned long data){
	printk(KERN_INFO "inside timer routine\n");
	int error;
	static int count=0;
	//static int flag=1;
	struct device *dev = hdmi_dev.this_device;
	struct device_node *fwr = NULL;
//	fw = of_find_compatible_node(NULL,);
	struct rpi_firmware *fw = rpi_firmware_get(NULL);
	count++;
	printk(KERN_INFO "gpio46 val := %d\n",gpio_get_value(hdmi_gpio));
	//if(count==interval) {
	if(gpio_get_value(hdmi_gpio)){
		printk(KERN_INFO "inside inner loop\n");
		//get_projector_id(fwr,fw);
		//flag=1;
		if(flag==0){
			printk("before event\n");
			error = kobject_uevent_env(&dev->kobj,KOBJ_CHANGE,envp);
			if (error){
				kobject_put(&dev->kobj);
				pr_err("No kobject_uevent %d\n", error);
			}
			printk("after event");
		}
		flag=1;
		//flag=0;
	}
	else {
		flag=0;
	}
	//count=0;
	//}
	mod_timer(&my_timer,jiffies+secs_to_jiffies((interval)));
}
module_init(module_start);
module_exit(module_end);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HDMI Device");
