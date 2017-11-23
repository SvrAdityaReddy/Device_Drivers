/*
 * lirc_rpi.c
 *
 * lirc_rpi - Device driver that records pulse- and pause-lengths
 *	      (space-lengths) (just like the lirc_serial driver does)
 *	      between GPIO interrupt events on the Raspberry Pi.
 *	      Lots of code has been taken from the lirc_serial module,
 *	      so I would like say thanks to the authors.
 *
 * Copyright (C) 2012 Aron Robert Szabo <aron@reon.hu>,
 *		      Michael Bishop <cleverca22@gmail.com>
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/timekeeping.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_data/bcm2708.h>

#define LIRC_DRIVER_NAME "lirc_rpi"
#define RBUF_LEN 256
#define LIRC_TRANSMITTER_LATENCY 50

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG LIRC_DRIVER_NAME ": "	\
			       fmt, ## args);			\
	} while (0)

/* module parameters */

/* set the default GPIO input pin */
static int gpio_in_pin = 18;
/* set the default pull behaviour for input pin */
static int gpio_in_pull = BCM2708_PULL_DOWN;
/* set the default GPIO output pin */
static int gpio_out_pin = 17;
/* enable debugging messages */
static bool debug;
/* -1 = auto, 0 = active high, 1 = active low */
static int sense = -1;
/* use softcarrier by default */
static bool softcarrier = 1;
/* 0 = do not invert output, 1 = invert output */
static bool invert = 0;

static unsigned long header_pulse=3561;
static unsigned long header_space=1680;
static unsigned long one_pulse=499;
static unsigned long one_space=1234;
static unsigned long zero_pulse=499;
static unsigned long zero_space=377;
static unsigned long ptrail=500;
static char l[64]="0000000001000000000001000000000100010010000000001011110010101111";

struct gpio_chip *gpiochip;
static int irq_num;
static int auto_sense = 1;

/* forward declarations */
static long send_pulse(unsigned long length);
static void send_space(long length);
static void lirc_rpi_exit(void);
static void send_raw_codes(void);
static void send_hex_code(char *val); 
static struct platform_device *lirc_rpi_dev;
static struct timeval lasttv = { 0, 0 };
static struct lirc_buffer rbuf;
static spinlock_t lock;

/* initialized/set in init_timing_params() */
static unsigned int freq = 38000;
static unsigned int duty_cycle = 50;
static unsigned long period;
static unsigned long pulse_width;
static unsigned long space_width;

/* Creating sysfs attributes */

//#define to_lirc_rpi_dev_data(p)	((struct lirc_rpi_dev_data *)((p)->platform_data))

struct lirc_rpi_dev_data {
	struct device *dev;
	char code[1024];
	int send;
};

static ssize_t get_code(struct device *dev, struct device_attribute *attr, char *resp)
{
	struct lirc_rpi_dev_data *mydrv = dev_get_drvdata(dev);
	int len;
	printk(KERN_INFO LIRC_DRIVER_NAME ": get_code function called!\n");

    len = sprintf(resp, "%s\n", mydrv ->code);
    if (len <= 0)
        dev_err(dev, "mydrv: Invalid sprintf len: %d\n", len);

    return len;
}

static ssize_t set_code(struct device *dev, struct device_attribute *attr, const char *newval, size_t valsize)
{
	struct lirc_rpi_dev_data *mydrv = dev_get_drvdata(dev);
	
	printk(KERN_INFO LIRC_DRIVER_NAME ": set_code function called!\n");
	dev_alert(dev, "changing code from %s to %s ...\n", mydrv->code, newval);
	strncpy(mydrv->code, newval, valsize-1);
    return valsize;
}

static ssize_t get_send(struct device *dev, struct device_attribute *attr, char *resp)
{
	struct lirc_rpi_dev_data *mydrv = dev_get_drvdata(dev);
	printk(KERN_INFO LIRC_DRIVER_NAME ": get_send function called!\n");
	return snprintf(resp, 8, "%d\n", mydrv->send);
}
static ssize_t set_send(struct device *dev, struct device_attribute *attr, const char *newval, size_t valsize)
{
	struct lirc_rpi_dev_data *mydrv = dev_get_drvdata(dev);
	int newinterval;
	if (sscanf(newval, "%d", &newinterval) != 1)
		return -EINVAL;
	//dev_alert(dev, "send attribute change from %d to %d?\n", mydrv->send, newinterval);
	printk(KERN_INFO LIRC_DRIVER_NAME ": send attribute change from %d to %d\n", mydrv->send, newinterval);
	mydrv->send = newinterval;
	if(mydrv->send==1) {
		printk(KERN_INFO LIRC_DRIVER_NAME ": send_raw_codes function called!\n");
		send_raw_codes();
		//send_hex_code(mydrv->code);
	}	
	return valsize;
}

static DEVICE_ATTR(code, S_IRUGO|S_IWUSR, get_code, set_code);
static DEVICE_ATTR(send, S_IRUGO|S_IWUSR, get_send, set_send);

static struct attribute *lirc_rpi_dev_attrs[] = {
		&dev_attr_code.attr,
		&dev_attr_send.attr,
		NULL
};

static struct attribute_group lirc_rpi_dev_basic_attributes = {
		.attrs = lirc_rpi_dev_attrs,
};

/*static const struct attribute_group *lirc_rpi_dev_all_attributes[] = {
	 	&lirc_rpi_dev_basic_attributes,
		 	NULL
};*/

/*static struct lirc_rpi_dev_data lirc_rpidata={
	.code="receive"
};*/

/* sysfs attributes created */

/* Functions to send hex codes and raw codes
 * send_hex_code()
 * send_raw_codes() 
*/
static void send_raw_codes() {
	int i=0;
	long delta = 0;
	/*unsigned long l[67]={9055,    4479,     588,     545,     588,     544,
              587,    1671,     590,     542,     591,     542,
              591,     544,     589,     551,     582,    1668,
              591,     543,     590,     543,     590,    1670,
              614,     518,     590,    1676,     610,    1643,
              590,    1669,     591,    1672,     614,    1646,
              616,    1645,     615,     519,     615,    1644,
              615,     519,     616,     517,     616,     516,
              590,     544,     596,     536,     590,     542,
              592,    1668,     590,     544,     589,    1670,
              616,    1644,     622,    1636,     617,    1646,
              617}; */
    unsigned long l[115] = {3561,1680,499,377,499,1234,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,1234,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,1234,499,377,499,377,499,377,499,1234,499,377,499,377,499,1234,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,377,499,1234,499,377,499,1234,499,1234,499,1234,499,1234,499,377,499,377,499,1234,499,377,499,1234,499,377,499,1234,499,1234,499,1234,499,1234,500};     
    /*unsigned long l[115]={3525,    1744,     438,     444,     433,    1292,
              439,     438,     439,     439,     439,     438,
              439,     438,     439,     439,     439,     445,
              440,     436,     441,     437,     440,     438,
              446,     431,     441,     434,     442,    1288,
              442,     435,     442,     445,     441,     435,
              443,     436,     441,     436,     441,     436,
              441,     436,     441,     442,     435,     435,
              441,    1296,     441,     435,     442,     435,
              442,     435,     441,    1288,     441,     436,
              441,     436,     440,    1294,     436,     451,
              442,     437,     441,     436,     441,     436,
              441,     436,     441,     435,     442,     435,
              441,     436,     441,     444,     441,    1293,
              436,     436,     441,    1288,     442,    1288,
              443,    1287,     441,    1287,     444,     435,
              442,     450,     442,    1288,     442,     436,
              441,    1289,     441,     436,     442,    1288,
              442,    1289,     441,    1297,     434,    1293,
              440};*/
    /*unsigned long l[231] = {3519, 1745, 436, 439, 438, 1293, 437, 440, 438, 439, 439, 438, 439, 439, 437, 440, 439, 446, 439, 438, 439, 439, 439, 438, 439, 438, 441, 434, 441, 1289, 441, 434, 443, 445, 440, 437, 440, 437, 440, 437, 440, 437, 440, 437, 440, 436, 441, 437, 440, 1303, 435, 436, 440, 437, 440, 437, 440, 1290, 440, 435, 440, 436, 441, 1288, 442, 455, 439, 445, 432, 438, 439, 438, 439, 438, 439, 438, 439, 438, 439, 437, 439, 447, 439, 1291, 439, 438, 439, 1297, 432, 1291, 439, 1290, 440, 1292, 439, 438, 439, 451, 439, 1292, 438, 445, 432, 1290, 441, 438, 439, 1292, 437, 1293, 461, 1269, 463, 1269, 470, 74001, 3530, 1745, 438, 442, 435, 1295, 437, 440, 437, 440, 437, 440, 438, 440, 437, 440, 438, 447, 436, 441, 438, 439, 438, 447, 430, 440, 462, 413, 439, 1291, 439, 437, 440, 447, 461, 416, 464, 413, 464, 412, 465, 412, 465, 412, 472, 405, 439, 438, 439, 1299, 463, 413, 439, 438, 438, 438, 439, 1291, 468, 408, 439, 437, 440, 1296, 447, 440, 440, 438, 439, 438, 439, 438, 439, 438, 439, 438, 439, 438, 439, 438, 439, 446, 439, 1297, 433, 438, 438, 1291, 439, 1290, 440, 1291, 438, 1291, 467, 412, 466, 430, 433, 1292, 465, 412, 465, 1266, 466, 411, 465, 1266, 466, 1265, 466, 1272, 461, 1265, 438};*/             
    unsigned long flags;
    /*unsigned long l[67]={9069,    4457,     581,     549,     583,     549,
              582,    1650,     584,     547,     585,     547,
              585,     548,     584,     548,     590,    1671,
              582,     548,    584,     548,     583,    1651,
              582,     549,     610,    1652,     616,    1616,
              584,    1676,     583,    1681,     610,     495,
              637,     495,     611,    1658,     602,     523,
              609,     522,     584,     548,     583,     549,
              583,     548,     613,    1647,     583,    1649,
              585,     550,     610,    1651,     610,    1625,
              610,    1650,     611,    1632,     603,    1625,
              612};*/
    spin_lock_irqsave(&lock, flags);
    for (i = 0; i < 115; i++) {
		if (i%2)
			send_space(l[i] - delta);
		else
			delta = send_pulse(l[i]);
	}
	gpiochip->set(gpiochip, gpio_out_pin, invert);

	spin_unlock_irqrestore(&lock, flags);
	return;
}

static void send_hex_code(char *val) 
{
	long newval;
	long i=0;
	kstrtol(val, 16, &newval);
	printk(KERN_INFO LIRC_DRIVER_NAME ": hex string %s decimal value is %ld\n", val, newval);
	long delta = 0;
	// send header
	delta=send_pulse(header_pulse);
	send_space(header_space-delta);
	for(i=0;i<64;i++) {
		if(l[i]=='0') {
			delta=send_pulse(zero_pulse);
			send_space(zero_space-delta);
		}
		else {
			delta=send_pulse(one_pulse);
			send_space(one_space-delta);
		}
	}
	// send trail
	send_pulse(ptrail); 
	return;
}

static void safe_udelay(unsigned long usecs)
{
	while (usecs > MAX_UDELAY_US) {
		udelay(MAX_UDELAY_US);
		usecs -= MAX_UDELAY_US;
	}
	udelay(usecs);
}

static unsigned long read_current_us(void)
{
	struct timespec now;
	getnstimeofday(&now);
	return (now.tv_sec * 1000000) + (now.tv_nsec/1000);
}

static int init_timing_params(unsigned int new_duty_cycle,
	unsigned int new_freq)
{
	if (1000 * 1000000L / new_freq * new_duty_cycle / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	if (1000 * 1000000L / new_freq * (100 - new_duty_cycle) / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	duty_cycle = new_duty_cycle;
	freq = new_freq;
	period = 1000 * 1000000L / freq;
	pulse_width = period * duty_cycle / 100;
	space_width = period - pulse_width;
	dprintk("in init_timing_params, freq=%d pulse=%ld, "
		"space=%ld\n", freq, pulse_width, space_width);
	return 0;
}

static long send_pulse_softcarrier(unsigned long length)
{
	int flag;
	unsigned long actual, target;
	unsigned long actual_us, initial_us, target_us;

	length *= 1000;

	actual = 0; target = 0; flag = 0;
	actual_us = read_current_us();

	while (actual < length) {
		if (flag) {
			gpiochip->set(gpiochip, gpio_out_pin, invert);
			target += space_width;
		} else {
			gpiochip->set(gpiochip, gpio_out_pin, !invert);
			target += pulse_width;
		}
		initial_us = actual_us;
		target_us = actual_us + (target - actual) / 1000;
		/*
		 * Note - we've checked in ioctl that the pulse/space
		 * widths are big enough so that d is > 0
		 */
		if  ((int)(target_us - actual_us) > 0)
			udelay(target_us - actual_us);
		actual_us = read_current_us();
		actual += (actual_us - initial_us) * 1000;
		flag = !flag;
	}
	return (actual-length) / 1000;
}

static long send_pulse(unsigned long length)
{
	if (length <= 0)
		return 0;

	if (softcarrier) {
		return send_pulse_softcarrier(length);
	} else {
		gpiochip->set(gpiochip, gpio_out_pin, !invert);
		safe_udelay(length);
		return 0;
	}
}

static void send_space(long length)
{
	gpiochip->set(gpiochip, gpio_out_pin, invert);
	if (length <= 0)
		return;
	safe_udelay(length);
}

static void rbwrite(int l)
{
	if (lirc_buffer_full(&rbuf)) {
		/* no new signals will be accepted */
		dprintk("Buffer overrun\n");
		return;
	}
	lirc_buffer_write(&rbuf, (void *)&l);
}

static void frbwrite(int l)
{
	/* simple noise filter */
	static int pulse, space;
	static unsigned int ptr;

	if (ptr > 0 && (l & PULSE_BIT)) {
		pulse += l & PULSE_MASK;
		if (pulse > 250) {
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
		return;
	}
	if (!(l & PULSE_BIT)) {
		if (ptr == 0) {
			if (l > 20000) {
				space = l;
				ptr++;
				return;
			}
		} else {
			if (l > 20000) {
				space += pulse;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				space += l;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				pulse = 0;
				return;
			}
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
	}
	rbwrite(l);
}

static irqreturn_t irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	long deltv;
	int data;
	int signal;

	/* use the GPIO signal level */
	signal = gpiochip->get(gpiochip, gpio_in_pin);

	if (sense != -1) {
		/* get current time */
		do_gettimeofday(&tv);

		/* calc time since last interrupt in microseconds */
		deltv = tv.tv_sec-lasttv.tv_sec;
		if (tv.tv_sec < lasttv.tv_sec ||
		    (tv.tv_sec == lasttv.tv_sec &&
		     tv.tv_usec < lasttv.tv_usec)) {
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": AIEEEE: your clock just jumped backwards\n");
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": %d %d %lx %lx %lx %lx\n", signal, sense,
			       tv.tv_sec, lasttv.tv_sec,
			       tv.tv_usec, lasttv.tv_usec);
			data = PULSE_MASK;
		} else if (deltv > 15) {
			data = PULSE_MASK; /* really long time */
			if (!(signal^sense)) {
				/* sanity check */
				printk(KERN_DEBUG LIRC_DRIVER_NAME
				       ": AIEEEE: %d %d %lx %lx %lx %lx\n",
				       signal, sense, tv.tv_sec, lasttv.tv_sec,
				       tv.tv_usec, lasttv.tv_usec);
				/*
				 * detecting pulse while this
				 * MUST be a space!
				 */
				if (auto_sense) {
					sense = sense ? 0 : 1;
				}
			}
		} else {
			data = (int) (deltv*1000000 +
				      (tv.tv_usec - lasttv.tv_usec));
		}
		frbwrite(signal^sense ? data : (data|PULSE_BIT));
		lasttv = tv;
		wake_up_interruptible(&rbuf.wait_poll);
	}

	return IRQ_HANDLED;
}

static int is_right_chip(struct gpio_chip *chip, void *data)
{
	dprintk("is_right_chip %s %d\n", chip->label, strcmp(data, chip->label));

	if (strcmp(data, chip->label) == 0)
		return 1;
	return 0;
}

static inline int read_bool_property(const struct device_node *np,
				     const char *propname,
				     bool *out_value)
{
	u32 value = 0;
	int err = of_property_read_u32(np, propname, &value);
	if (err == 0)
		*out_value = (value != 0);
	return err;
}

static void read_pin_settings(struct device_node *node)
{
	u32 pin;
	int index;

	for (index = 0;
	     of_property_read_u32_index(
		     node,
		     "brcm,pins",
		     index,
		     &pin) == 0;
	     index++) {
		u32 function;
		int err;
		err = of_property_read_u32_index(
			node,
			"brcm,function",
			index,
			&function);
		if (err == 0) {
			if (function == 1) /* Output */
				gpio_out_pin = pin;
			else if (function == 0) /* Input */
				gpio_in_pin = pin;
		}
	}
}

static int init_port(void)
{
	int i, nlow, nhigh;
	struct device_node *node;

	node = lirc_rpi_dev->dev.of_node;

	gpiochip = gpiochip_find("bcm2708_gpio", is_right_chip);

	/*
	 * Because of the lack of a setpull function, only support
	 * pinctrl-bcm2835 if using device tree.
	*/
	if (!gpiochip && node)
		gpiochip = gpiochip_find("pinctrl-bcm2835", is_right_chip);

	if (!gpiochip) {
		pr_err(LIRC_DRIVER_NAME ": gpio chip not found!\n");
		return -ENODEV;
	}

	if (node) {
		struct device_node *pins_node;

		pins_node = of_parse_phandle(node, "pinctrl-0", 0);
		if (!pins_node) {
			printk(KERN_ERR LIRC_DRIVER_NAME
			       ": pinctrl settings not found!\n");
			return -EINVAL;
		}

		read_pin_settings(pins_node);

		of_property_read_u32(node, "rpi,sense", &sense);

		read_bool_property(node, "rpi,softcarrier", &softcarrier);

		read_bool_property(node, "rpi,invert", &invert);

		read_bool_property(node, "rpi,debug", &debug);

	} else {
		return -EINVAL;
	}

	gpiochip->set(gpiochip, gpio_out_pin, invert);

	irq_num = gpiochip->to_irq(gpiochip, gpio_in_pin);
	dprintk("to_irq %d\n", irq_num);

	/* if pin is high, then this must be an active low receiver. */
	if (sense == -1) {
		/* wait 1/2 sec for the power supply */
		msleep(500);

		/*
		 * probe 9 times every 0.04s, collect "votes" for
		 * active high/low
		 */
		nlow = 0;
		nhigh = 0;
		for (i = 0; i < 9; i++) {
			if (gpiochip->get(gpiochip, gpio_in_pin))
				nlow++;
			else
				nhigh++;
			msleep(40);
		}
		sense = (nlow >= nhigh ? 1 : 0);
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": auto-detected active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
	} else {
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": manually using active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
		auto_sense = 0;
	}

	return 0;
}

// called when the character device is opened
static int set_use_inc(void *data)
{
	int result;

	/* initialize timestamp */
	do_gettimeofday(&lasttv);

	result = request_irq(irq_num,
			     (irq_handler_t) irq_handler,
			     IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING,
			     LIRC_DRIVER_NAME, (void*) 0);

	switch (result) {
	case -EBUSY:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": IRQ %d is busy\n",
		       irq_num);
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": Bad irq number or handler\n");
		return -EINVAL;
	default:
		dprintk("Interrupt %d obtained\n",
			irq_num);
		break;
	};

	/* initialize pulse/space widths */
	init_timing_params(duty_cycle, freq);

	return 0;
}

static void set_use_dec(void *data)
{
	/* GPIO Pin Falling/Rising Edge Detect Disable */
	irq_set_irq_type(irq_num, 0);
	disable_irq(irq_num);

	free_irq(irq_num, (void *) 0);

	dprintk(KERN_INFO LIRC_DRIVER_NAME
		": freed IRQ %d\n", irq_num);
}

static ssize_t lirc_write(struct file *file, const char *buf,
	size_t n, loff_t *ppos)
{
	int i, count;
	unsigned long flags;
	long delta = 0;
	int *wbuf;

	count = n / sizeof(int);
	if (n % sizeof(int) || count % 2 == 0)
		return -EINVAL;
	wbuf = memdup_user(buf, n);
	if (IS_ERR(wbuf))
		return PTR_ERR(wbuf);
	spin_lock_irqsave(&lock, flags);

	for (i = 0; i < count; i++) {
		if (i%2)
			send_space(wbuf[i] - delta);
		else
			delta = send_pulse(wbuf[i]);
	}
	gpiochip->set(gpiochip, gpio_out_pin, invert);

	spin_unlock_irqrestore(&lock, flags);
	kfree(wbuf);
	return n;
}

static long lirc_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	int result;
	__u32 value;

	switch (cmd) {
	case LIRC_GET_SEND_MODE:
		return -ENOIOCTLCMD;
		break;

	case LIRC_SET_SEND_MODE:
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		/* only LIRC_MODE_PULSE supported */
		if (value != LIRC_MODE_PULSE)
			return -ENOSYS;
		break;

	case LIRC_GET_LENGTH:
		return -ENOSYS;
		break;

	case LIRC_SET_SEND_DUTY_CYCLE:
		dprintk("SET_SEND_DUTY_CYCLE\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value <= 0 || value > 100)
			return -EINVAL;
		return init_timing_params(value, freq);
		break;

	case LIRC_SET_SEND_CARRIER:
		dprintk("SET_SEND_CARRIER\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value > 500000 || value < 20000)
			return -EINVAL;
		return init_timing_params(duty_cycle, value);
		break;

	default:
		return lirc_dev_fop_ioctl(filep, cmd, arg);
	}
	return 0;
}

static const struct file_operations lirc_fops = {
	.owner		= THIS_MODULE,
	.write		= lirc_write,
	.unlocked_ioctl	= lirc_ioctl,
	.read		= lirc_dev_fop_read,
	.poll		= lirc_dev_fop_poll,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= no_llseek,
};

static struct lirc_driver driver = {
	.name		= LIRC_DRIVER_NAME,
	.minor		= -1,
	.code_length	= 1,
	.sample_rate	= 0,
	.data		= NULL,
	.add_to_buf	= NULL,
	.rbuf		= &rbuf,
	.set_use_inc	= set_use_inc,
	.set_use_dec	= set_use_dec,
	.fops		= &lirc_fops,
	.dev		= NULL,
	.owner		= THIS_MODULE,
};

static const struct of_device_id lirc_rpi_of_match[] = {
	{ .compatible = "rpi,lirc-rpi", },
	{},
};
MODULE_DEVICE_TABLE(of, lirc_rpi_of_match);

static int lirc_rpi_driver_probe(struct platform_device *pdev)
{
	//int result=devm_device_add_group(&pdev->dev, &lirc_rpi_dev_basic_attributes);
	//device_add_group(&pdev->dev, &lirc_rpi_dev_basic_attributes);
	struct lirc_rpi_dev_data *mydrv;
	int result;
	printk(KERN_INFO LIRC_DRIVER_NAME ": probe function called!\n");
	//mydrv->code;
    mydrv = devm_kzalloc(&pdev->dev, sizeof(*mydrv), GFP_KERNEL);
    mydrv->dev = &pdev->dev;
    platform_set_drvdata(pdev, mydrv);
	//result=device_add_groups(&pdev->dev, lirc_rpi_dev_all_attributes);
	result = sysfs_create_group(&pdev->dev.kobj, &lirc_rpi_dev_basic_attributes);
    if (result) {
        dev_err(&pdev->dev, "sysfs creation failed\n");
        return result;
    }
	return 0;
}

static int lirc_rpi_driver_remove(struct platform_device *pdev)
{
    sysfs_remove_group(&pdev->dev.kobj, &lirc_rpi_dev_basic_attributes);
    return 0;
}

static struct platform_driver lirc_rpi_driver = {
	.probe=lirc_rpi_driver_probe,
	.remove=lirc_rpi_driver_remove,
	.driver = {
		.name   = LIRC_DRIVER_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(lirc_rpi_of_match),
	},
};

static int __init lirc_rpi_init(void)
{
	struct device_node *node;
	int result;

	/* Init read buffer. */
	result = lirc_buffer_init(&rbuf, sizeof(int), RBUF_LEN);
	if (result < 0)
		return -ENOMEM;

	result = platform_driver_register(&lirc_rpi_driver);
	if (result) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": lirc register returned %d\n", result);
		goto exit_buffer_free;
	}

	node = of_find_compatible_node(NULL, NULL,
				       lirc_rpi_of_match[0].compatible);

	if (node) {
		/* DT-enabled */
		lirc_rpi_dev = of_find_device_by_node(node);
		//lirc_rpi_dev->dev.groups = lirc_rpi_dev_all_attributes;
		//lirc_rpi_dev->dev.platform_data=&lirc_rpidata;
		WARN_ON(lirc_rpi_dev->dev.of_node != node);
		of_node_put(node);
	}
	else {
		lirc_rpi_dev = platform_device_alloc(LIRC_DRIVER_NAME, 0);
		//lirc_rpi_dev->dev.groups = lirc_rpi_dev_all_attributes;
		//lirc_rpi_dev->dev.platform_data=&lirc_rpidata;
		if (!lirc_rpi_dev) {
			result = -ENOMEM;
			goto exit_driver_unregister;
		}

		result = platform_device_add(lirc_rpi_dev);
		if (result)
			goto exit_device_put;
	}

	return 0;

	exit_device_put:
	platform_device_put(lirc_rpi_dev);

	exit_driver_unregister:
	platform_driver_unregister(&lirc_rpi_driver);

	exit_buffer_free:
	lirc_buffer_free(&rbuf);

	return result;
}

static void lirc_rpi_exit(void)
{
	if (!lirc_rpi_dev->dev.of_node)
		platform_device_unregister(lirc_rpi_dev);
	platform_driver_unregister(&lirc_rpi_driver);
	lirc_buffer_free(&rbuf);
}

static int __init lirc_rpi_init_module(void)
{
	int result;

	result = lirc_rpi_init();
	if (result)
		return result;

	result = init_port();
	if (result < 0)
		goto exit_rpi;

	driver.features = LIRC_CAN_SET_SEND_DUTY_CYCLE |
			  LIRC_CAN_SET_SEND_CARRIER |
			  LIRC_CAN_SEND_PULSE |
			  LIRC_CAN_REC_MODE2;

	driver.dev = &lirc_rpi_dev->dev;
	driver.minor = lirc_register_driver(&driver);

	if (driver.minor < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": device registration failed with %d\n", result);
		result = -EIO;
		goto exit_rpi;
	}

	printk(KERN_INFO LIRC_DRIVER_NAME ": driver registered!\n");

	return 0;

	exit_rpi:
	lirc_rpi_exit();

	return result;
}

static void __exit lirc_rpi_exit_module(void)
{
	lirc_unregister_driver(driver.minor);

	gpio_free(gpio_out_pin);
	gpio_free(gpio_in_pin);

	lirc_rpi_exit();

	printk(KERN_INFO LIRC_DRIVER_NAME ": cleaned up module\n");
}

module_init(lirc_rpi_init_module);
module_exit(lirc_rpi_exit_module);

MODULE_DESCRIPTION("Infra-red receiver and blaster driver for Raspberry Pi GPIO.");
MODULE_AUTHOR("Aron Robert Szabo <aron@reon.hu>");
MODULE_AUTHOR("Michael Bishop <cleverca22@gmail.com>");
MODULE_LICENSE("GPL");

module_param(gpio_out_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_out_pin, "GPIO output/transmitter pin number of the BCM"
		 " processor. (default 17");

module_param(gpio_in_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_in_pin, "GPIO input pin number of the BCM processor."
		 " (default 18");

module_param(gpio_in_pull, int, S_IRUGO);
MODULE_PARM_DESC(gpio_in_pull, "GPIO input pin pull configuration."
		 " (0 = off, 1 = up, 2 = down, default down)");

module_param(sense, int, S_IRUGO);
MODULE_PARM_DESC(sense, "Override autodetection of IR receiver circuit"
		 " (0 = active high, 1 = active low )");

module_param(softcarrier, bool, S_IRUGO);
MODULE_PARM_DESC(softcarrier, "Software carrier (0 = off, 1 = on, default on)");

module_param(invert, bool, S_IRUGO);
MODULE_PARM_DESC(invert, "Invert output (0 = off, 1 = on, default off");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Enable debugging messages");
