#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/console.h>
#include <linux/slab.h>
#include <linux/tty_ldisc.h>
#include <linux/device.h>
#include <linux/serial.h>
#include <linux/ctype.h>
#include <linux/workqueue.h>
#include "virtty.h"

#define N_VIRTTY_SLAVE 30

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Virtual TTY Multiplexer & Console");

static struct tty_driver *virtty_driver;
static struct virtty_device *virtty_devices[MAX_VIRTTY_DEVICES];
static char *virtty_cmdline_slaves[MAX_VIRTTY_DEVICES];
static struct delayed_work reconnect_work;

/* Forward declarations */
static void virtty_free_slave(struct virtty_slave *slave);
static int virtty_activate_slave(struct virtty_device *vdev, struct virtty_slave *slave);

/* --- Slave Management --- */

static void virtty_free_slave_rcu(struct rcu_head *head)
{
	struct virtty_slave *slave = container_of(head, struct virtty_slave, rcu);
	kfree(slave->name);
	kfree(slave->options);
	kfree(slave);
}

static void virtty_free_slave(struct virtty_slave *slave)
{
	if (slave->tty)
		tty_kclose(slave->tty);
	call_rcu(&slave->rcu, virtty_free_slave_rcu);
}

static void virtty_detach_slave(struct virtty_slave *slave)
{
	if (!slave->active)
		return;
	if (slave->tty) {
		tty_kclose(slave->tty);
		slave->tty = NULL;
	}
	slave->active = false;
}

static int virtty_activate_slave(struct virtty_device *vdev, struct virtty_slave *slave)
{
	struct tty_struct *tty;
	struct tty_driver *driver;
	int index, ret;

	if (slave->active)
		return 0;

	driver = tty_find_polling_driver(slave->name, &index);
	if (!driver)
		return -ENODEV;

	tty = tty_kopen_exclusive(MKDEV(driver->major, driver->minor_start + index));
	if (IS_ERR(tty))
		return PTR_ERR(tty);

	slave->tty = tty;
	tty->disc_data = vdev;

	if (slave->options) {
		struct ktermios termios;
		speed_t baud = 115200;
		char parity = 'n';
		int bits = 8, stop = 1;

		termios = tty->termios;
		if (sscanf(slave->options, "%u%c%d%d", &baud, &parity, &bits, &stop) >= 1) {
			tty_termios_encode_baud_rate(&termios, baud, baud);
			if (parity == 'e' || parity == 'E') { termios.c_cflag |= PARENB; termios.c_cflag &= ~PARODD; }
			else if (parity == 'o' || parity == 'O') { termios.c_cflag |= PARENB; termios.c_cflag |= PARODD; }
			else termios.c_cflag &= ~PARENB;

			termios.c_cflag &= ~CSIZE;
			if (bits == 5) termios.c_cflag |= CS5;
			else if (bits == 6) termios.c_cflag |= CS6;
			else if (bits == 7) termios.c_cflag |= CS7;
			else termios.c_cflag |= CS8;

			if (stop == 2) termios.c_cflag |= CSTOPB;
			else termios.c_cflag &= ~CSTOPB;

			if (tty->ops->set_termios) tty->ops->set_termios(tty, &termios);
			else tty->termios = termios;
		}
	}

	ret = tty_set_ldisc(tty, N_VIRTTY_SLAVE);
	if (ret) {
		tty_kclose(tty);
		slave->tty = NULL;
		return ret;
	}

	slave->active = true;
	pr_info("virtty%d: attached slave %s\n", vdev->id, slave->name);
	return 0;
}

static void virtty_reconnect_work_fn(struct work_struct *work)
{
	int i;
	struct virtty_slave *slave;
	bool still_inactive = false;

	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		struct virtty_device *vdev = virtty_devices[i];
		if (!vdev) continue;

		mutex_lock(&vdev->slave_lock);
		list_for_each_entry(slave, &vdev->slaves, list) {
			if (!slave->active) {
				if (virtty_activate_slave(vdev, slave) != 0)
					still_inactive = true;
			}
		}
		mutex_unlock(&vdev->slave_lock);
	}

	if (still_inactive)
		schedule_delayed_work(&reconnect_work, HZ * 5);
}

static int virtty_add_slave(struct virtty_device *vdev, const char *name, const char *options)
{
	struct virtty_slave *slave;
	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	if (!slave) return -ENOMEM;
	slave->name = kstrdup(name, GFP_KERNEL);
	if (options) slave->options = kstrdup(options, GFP_KERNEL);

	mutex_lock(&vdev->slave_lock);
	list_add_tail_rcu(&slave->list, &vdev->slaves);
	virtty_activate_slave(vdev, slave);
	mutex_unlock(&vdev->slave_lock);

	if (!slave->active)
		schedule_delayed_work(&reconnect_work, HZ * 2);

	return 0;
}

/* --- Sysfs --- */

static ssize_t add_slave_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tty_port *port = dev_get_drvdata(dev);
	struct virtty_device *vdev = container_of(port, struct virtty_device, port);
	char name[64], *options, *tmp;
	if (count >= sizeof(name)) return -EINVAL;
	strscpy(name, buf, sizeof(name));
	tmp = strchr(name, '\n'); if (tmp) *tmp = '\0';
	options = strchr(name, ','); if (options) { *options = '\0'; options++; }
	virtty_add_slave(vdev, name, options);
	return count;
}
static DEVICE_ATTR_WO(add_slave);

static ssize_t remove_slave_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tty_port *port = dev_get_drvdata(dev);
	struct virtty_device *vdev = container_of(port, struct virtty_device, port);
	struct virtty_slave *slave, *next;
	char name[64], *tmp;
	if (count >= sizeof(name)) return -EINVAL;
	strscpy(name, buf, sizeof(name));
	tmp = strchr(name, '\n'); if (tmp) *tmp = '\0';
	mutex_lock(&vdev->slave_lock);
	list_for_each_entry_safe(slave, next, &vdev->slaves, list) {
		if (sysfs_streq(slave->name, name)) {
			list_del_rcu(&slave->list);
			virtty_free_slave(slave);
		}
	}
	mutex_unlock(&vdev->slave_lock);
	return count;
}
static DEVICE_ATTR_WO(remove_slave);

static ssize_t slaves_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tty_port *port = dev_get_drvdata(dev);
	struct virtty_device *vdev = container_of(port, struct virtty_device, port);
	struct virtty_slave *slave;
	ssize_t len = 0;
	mutex_lock(&vdev->slave_lock);
	list_for_each_entry(slave, &vdev->slaves, list) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s (%s)\n", slave->name, slave->active ? "active" : "inactive");
	}
	mutex_unlock(&vdev->slave_lock);
	return len;
}
static DEVICE_ATTR_RO(slaves);

static struct attribute *virtty_attrs[] = { &dev_attr_add_slave.attr, &dev_attr_remove_slave.attr, &dev_attr_slaves.attr, NULL };
ATTRIBUTE_GROUPS(virtty);

/* --- Cmdline --- */

static int __init virtty_setup(char *str)
{
	int id;
	char *id_str = strsep(&str, "=");
	if (id_str && sscanf(id_str, "virtty%d", &id) == 1 && id >= 0 && id < MAX_VIRTTY_DEVICES)
		virtty_cmdline_slaves[id] = str;
	return 1;
}
__setup("virtty", virtty_setup);

/* --- Line Discipline & TTY Ops --- */

static void virtty_slave_receive_buf(struct tty_struct *tty, const u8 *cp, const u8 *fp, size_t count)
{
	struct virtty_device *vdev = tty->disc_data;
	if (vdev) {
		tty_insert_flip_string(&vdev->port, cp, count);
		tty_flip_submit(&vdev->port);
	}
}

static struct tty_ldisc_ops virtty_ldisc_ops = {
	.owner = THIS_MODULE, .name = "virtty_slave", .num = N_VIRTTY_SLAVE,
	.receive_buf = virtty_slave_receive_buf,
};

static ssize_t virtty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	struct virtty_device *vdev = tty->driver_data;
	struct virtty_slave *slave;
	if (!vdev) return -ENODEV;
	mutex_lock(&vdev->slave_lock);
	list_for_each_entry(slave, &vdev->slaves, list) {
		if (slave->active && slave->tty && slave->tty->ops->write)
			slave->tty->ops->write(slave->tty, buf, count);
	}
	mutex_unlock(&vdev->slave_lock);
	return count;
}

static unsigned int virtty_write_room(struct tty_struct *tty) { return 2048; }

static int virtty_open(struct tty_struct *tty, struct file *filp)
{
	struct virtty_device *vdev = virtty_devices[tty->index];
	tty->driver_data = vdev;
	return tty_port_open(&vdev->port, tty, filp);
}

static void virtty_close(struct tty_struct *tty, struct file *filp)
{
	struct virtty_device *vdev = tty->driver_data;
	if (vdev) tty_port_close(&vdev->port, tty, filp);
}

static const struct tty_operations virtty_ops = {
	.open = virtty_open, .close = virtty_close, .write = virtty_write, .write_room = virtty_write_room,
};

/* --- Console --- */

static void virtty_console_write(struct console *co, const char *buf, unsigned int count)
{
	struct virtty_device *vdev = virtty_devices[co->index];
	struct virtty_slave *slave;
	if (!vdev || !mutex_trylock(&vdev->slave_lock)) return;
	list_for_each_entry(slave, &vdev->slaves, list) {
		if (slave->active && slave->tty && slave->tty->ops->write) {
			if (!in_atomic() && !irqs_disabled())
				slave->tty->ops->write(slave->tty, (const u8 *)buf, count);
		}
	}
	mutex_unlock(&vdev->slave_lock);
}

static struct tty_driver *virtty_console_device(struct console *co, int *index) { *index = co->index; return virtty_driver; }

/* --- Init/Exit --- */

static int __init virtty_init_device(int index)
{
	struct virtty_device *vdev;
	struct device *dev;
	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev) return -ENOMEM;
	vdev->id = index; vdev->driver = virtty_driver;
	INIT_LIST_HEAD(&vdev->slaves); mutex_init(&vdev->slave_lock); tty_port_init(&vdev->port);
	snprintf(vdev->console.name, sizeof(vdev->console.name), "virtty");
	vdev->console.write = virtty_console_write; vdev->console.device = virtty_console_device;
	vdev->console.flags = CON_PRINTBUFFER; vdev->console.index = index;
	virtty_devices[index] = vdev;
	dev = tty_port_register_device_attr(&vdev->port, virtty_driver, index, NULL, &vdev->port, virtty_groups);
	if (IS_ERR(dev)) { kfree(vdev); virtty_devices[index] = NULL; return PTR_ERR(dev); }
	register_console(&vdev->console);
	if (virtty_cmdline_slaves[index]) {
		char *s = kstrdup(virtty_cmdline_slaves[index], GFP_KERNEL), *slave_str, *p = s;
		if (s) {
			while ((slave_str = strsep(&p, ":")) != NULL) {
				char *opts = strchr(slave_str, ','); if (opts) { *opts = '\0'; opts++; }
				virtty_add_slave(vdev, slave_str, opts);
			}
			kfree(s);
		}
	}
	return 0;
}

static int __init virtty_init(void)
{
	int ret, i;
	INIT_DELAYED_WORK(&reconnect_work, virtty_reconnect_work_fn);
	ret = tty_register_ldisc(&virtty_ldisc_ops);
	if (ret) return ret;
	virtty_driver = tty_alloc_driver(MAX_VIRTTY_DEVICES, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(virtty_driver)) { tty_unregister_ldisc(&virtty_ldisc_ops); return PTR_ERR(virtty_driver); }
	virtty_driver->driver_name = "virtty"; virtty_driver->name = "virtty"; virtty_driver->major = 0;
	virtty_driver->type = TTY_DRIVER_TYPE_SERIAL; virtty_driver->subtype = SERIAL_TYPE_NORMAL;
	virtty_driver->init_termios = tty_std_termios; tty_set_operations(virtty_driver, &virtty_ops);
	ret = tty_register_driver(virtty_driver);
	if (ret) { tty_driver_kref_put(virtty_driver); tty_unregister_ldisc(&virtty_ldisc_ops); return ret; }
	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		if (virtty_cmdline_slaves[i] || i == 0)
			virtty_init_device(i);
	}
	return 0;
}

static void __exit virtty_exit(void)
{
	int i; struct virtty_slave *slave, *next;
	cancel_delayed_work_sync(&reconnect_work);
	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		struct virtty_device *vdev = virtty_devices[i];
		if (vdev) {
			unregister_console(&vdev->console);
			tty_unregister_device(virtty_driver, i);
			mutex_lock(&vdev->slave_lock);
			list_for_each_entry_safe(slave, next, &vdev->slaves, list) { list_del_rcu(&slave->list); virtty_free_slave(slave); }
			mutex_unlock(&vdev->slave_lock);
			tty_port_destroy(&vdev->port); kfree(vdev);
		}
	}
	tty_unregister_driver(virtty_driver);
	tty_driver_kref_put(virtty_driver); tty_unregister_ldisc(&virtty_ldisc_ops);
}

module_init(virtty_init);
module_exit(virtty_exit);
