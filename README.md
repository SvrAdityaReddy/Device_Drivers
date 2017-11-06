# Device_Drivers
Looked at lirc user utiliy available <br>
Looking at lirc_dev.c located in /linux/drivers/media/rc which is a base driver <br>
We need to use this base driver file lirc_dev.c to modify driver to receive/send ir signals through gpio pins using gpio subsystem <br>
Started writing driver code by looking at deepa module<br>
What is third parameter in module_param. It is to set permissions. If it is zero then no sysfs entry is made. In order to make sysfs entry we may use S_IRUGO|S_IWUSR as third parameter, this allows any one to read but only sudo to write. [Source](http://elixir.free-electrons.com/linux/latest/source/include/linux/moduleparam.h#L127)<br>
