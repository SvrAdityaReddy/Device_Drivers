#include <linux/module.h>

// Default gpio input pin
static int gpio_in=18;

// Default gpio output pin
static int gpio_out=17;

module_param(gpio_in, int, S_IRUGO|S_IWUSR);
module_param(gpio_out, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(gpio_in, "GPIO INPUT PIN");
MODULE_PARM_DESC(gpio_out, "GPIO OUTPUT PIN");

static int __init lirc_rpi_init(void) {
	pr_info("lirc_rpi module registered\n");
	return 0;
}

static void __exit lirc_rpi_exit(void) {
	pr_info("lirc_rpi module unloaded\n");
}

module_init(lirc_rpi_init);
module_exit(lirc_rpi_exit);
MODULE_DESCRIPTION("LIRC IR Transceiver driver module");
MODULE_AUTHOR("Svr Aditya Reddy");
MODULE_LICENSE("GPL");
