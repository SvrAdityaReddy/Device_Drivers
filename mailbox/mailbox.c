#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct edid_tag{
	u32 block_number;
	u32 status;
	unsigned char edid_block[128];
}edid_tag1;

static int __init mailbox_init(void){
	int error;
	printk(KERN_INFO "init\n");
	struct device_node *fwr = NULL;
	//fw = of_find_compatible_node(NULL,);
	struct rpi_firmware *fw = rpi_firmware_get(NULL);
	printk(KERN_INFO "rpi_firmware_get := %p",fw);
	edid_tag1.block_number = 0;
	edid_tag1.status = 0;
	for(error=0;error<128;error++)
		edid_tag1.edid_block[error] = 0;
	error = rpi_firmware_property(fw,RPI_FIRMWARE_GET_EDID_BLOCK,&edid_tag1,sizeof(edid_tag1));
	printk(KERN_INFO "Block No:%x, status:%x, error:%d",edid_tag1.block_number,edid_tag1.status,error);
	for(error=0;error<128;error++){
		printk("edid_block[%d] := %c\n",error,edid_tag1.edid_block[error]);
	}
	return 1;
}
static void __init mailbox_exit(void){
	printk(KERN_INFO "exit\n");
}
module_init(mailbox_init);
module_exit(mailbox_exit);

MODULE_DESCRIPTION("rpi driver for mailbox hdmi read");
MODULE_LICENSE("GPL");
