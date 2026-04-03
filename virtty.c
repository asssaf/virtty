#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/console.h>
#include <linux/slab.h>
#include <linux/tty_ldisc.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/tty_driver.h>
#include <linux/serial.h>
#include <linux/ctype.h>
#include "virtty.h"

#define N_VIRTTY_SLAVE 29 /* Custom line discipline ID, we'll pick one */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Virtual TTY Multiplexer & Console");

static struct tty_driver *virtty_driver;
static struct virtty_device *virtty_devices[MAX_VIRTTY_DEVICES];

static int virtty_activate_slave(struct virtty_device *vdev, struct virtty_slave *slave, dev_t dev)
{
	struct tty_struct *tty;
	int ret;

	tty = tty_kopen_exclusive(dev);
	if (IS_ERR(tty))
		return PTR_ERR(tty);

	slave->tty = tty;
	tty->disc_data = vdev;

	/* Apply options (e.g. baud rate) */
	if (slave->options) {
		struct ktermios termios;
		speed_t baud;

		if (sscanf(slave->options, "%u", &baud) == 1) {
			termios = tty->termios;
			tty_termios_encode_baud_rate(&termios, baud, baud);
			if (tty->ops->set_termios)
				tty->ops->set_termios(tty, &termios);
			else
				tty->termios = termios;
		}
	}

	/* Force our line discipline on the slave */
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

static int virtty_add_slave(struct virtty_device *vdev, const char *name, const char *options)
{
	struct virtty_slave *slave;

	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	if (!slave)
		return -ENOMEM;

	slave->name = kstrdup(name, GFP_KERNEL);
	if (options)
		slave->options = kstrdup(options, GFP_KERNEL);

	mutex_lock(&vdev->slave_lock);
	list_add_tail_rcu(&slave->list, &vdev->slaves);
	mutex_unlock(&vdev->slave_lock);

	return 0;
}

static int tty_match_name(struct device *dev, const void *data)
{
	const char *name = data;
	return sysfs_streq(dev_name(dev), name);
}

static int virtty_check_new_device(struct device *dev, void *data)
{
	int i;
	struct virtty_slave *slave;
	const char *name = dev_name(dev);

	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		struct virtty_device *vdev = virtty_devices[i];
		if (!vdev) continue;

		mutex_lock(&vdev->slave_lock);
		list_for_each_entry(slave, &vdev->slaves, list) {
			if (!slave->active && sysfs_streq(slave->name, name)) {
				virtty_activate_slave(vdev, slave, dev->devt);
			}
		}
		mutex_unlock(&vdev->slave_lock);
	}
	return 0; /* Keep iterating */
}

static int virtty_tty_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;

	if (action == BUS_NOTIFY_ADD_DEVICE) {
		virtty_check_new_device(dev, NULL);
	}
	return NOTIFY_OK;
}

static struct notifier_block virtty_nb = {
	.notifier_call = virtty_tty_notifier,
};

/* Sysfs Interface */
static ssize_t add_slave_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct tty_struct *tty = dev_get_drvdata(dev);
	struct virtty_device *vdev = tty->driver_data;
	char name[64], *options, *tmp;

	if (count >= sizeof(name))
		return -EINVAL;

	strscpy(name, buf, sizeof(name));
	tmp = strchr(name, '\n');
	if (tmp) *tmp = '\0';

	options = strchr(name, ',');
	if (options) {
		*options = '\0';
		options++;
	}

	virtty_add_slave(vdev, name, options);

	/* Kick the notifier logic manually to see if it already exists */
	bus_for_each_dev(&tty_bus_type, NULL, (void *)name, (int (*)(struct device *, void *))virtty_check_new_device);

	return count;
}
static DEVICE_ATTR_WO(add_slave);

static ssize_t slaves_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tty_struct *tty = dev_get_drvdata(dev);
	struct virtty_device *vdev = tty->driver_data;
	struct virtty_slave *slave;
	ssize_t len = 0;

	mutex_lock(&vdev->slave_lock);
	list_for_each_entry(slave, &vdev->slaves, list) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s (%s)\n",
				 slave->name, slave->active ? "active" : "inactive");
	}
	mutex_unlock(&vdev->slave_lock);

	return len;
}
static DEVICE_ATTR_RO(slaves);

static struct attribute *virtty_attrs[] = {
	&dev_attr_add_slave.attr,
	&dev_attr_slaves.attr,
	NULL,
};
ATTRIBUTE_GROUPS(virtty);

struct virtty_cmdline_cfg {
	int id;
	char *slaves;
	bool active;
};
static struct virtty_cmdline_cfg virtty_cfg[MAX_VIRTTY_DEVICES];

/* Command Line Parsing */
static int __init virtty_setup(char *str)
{
	int id;
	char *id_str;

	if (!str)
		return 0;

	/* Format: virtty0=ttyS0,115200:ttyS1,115200 */
	id_str = strsep(&str, "=");
	if (!id_str || sscanf(id_str, "virtty%d", &id) != 1)
		return 0;

	if (id < 0 || id >= MAX_VIRTTY_DEVICES)
		return 0;

	virtty_cfg[id].id = id;
	virtty_cfg[id].slaves = kstrdup(str, GFP_KERNEL);
	virtty_cfg[id].active = true;

	return 1;
}
__setup("virtty", virtty_setup);

/* Line Discipline: Aggregate data from slave into master */
static void virtty_slave_receive_buf(struct tty_struct *tty, const unsigned char *cp,
				    const char *fp, int count)
{
	struct virtty_device *vdev = tty->disc_data;
	if (vdev && vdev->port.tty) {
		tty_insert_flip_string(vdev->port.tty, cp, count);
		tty_flip_submit(vdev->port.tty);
	}
}

static int virtty_slave_open(struct tty_struct *tty)
{
	/* Nothing special needed on open */
	return 0;
}

static void virtty_slave_close(struct tty_struct *tty)
{
	/* Clear reference on close */
	tty->disc_data = NULL;
}

static struct tty_ldisc_ops virtty_ldisc_ops = {
	.owner		= THIS_MODULE,
	.name		= "virtty_slave",
	.num		= N_VIRTTY_SLAVE,
	.open		= virtty_slave_open,
	.close		= virtty_slave_close,
	.receive_buf	= virtty_slave_receive_buf,
};

/* TTY Operations: Write to all active slaves */
static int virtty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	struct virtty_device *vdev = tty->driver_data;
	struct virtty_slave *slave;

	if (!vdev)
		return -ENODEV;

	mutex_lock(&vdev->slave_lock);
	list_for_each_entry(slave, &vdev->slaves, list) {
		if (slave->active && slave->tty && slave->tty->ops->write) {
			/* Broadcast to slave */
			slave->tty->ops->write(slave->tty, buf, count);
		}
	}
	mutex_unlock(&vdev->slave_lock);

	return count;
}

static int virtty_write_room(struct tty_struct *tty)
{
	return 2048; /* Arbitrary large buffer room */
}

static int virtty_open(struct tty_struct *tty, struct file *filp)
{
	struct virtty_device *vdev = virtty_devices[tty->index];
	tty->driver_data = vdev;
	return tty_port_open(&vdev->port, tty, filp);
}

static void virtty_close(struct tty_struct *tty, struct file *filp)
{
	struct virtty_device *vdev = tty->driver_data;
	if (vdev)
		tty_port_close(&vdev->port, tty, filp);
}

static const struct tty_operations virtty_ops = {
	.open = virtty_open,
	.close = virtty_close,
	.write = virtty_write,
	.write_room = virtty_write_room,
};

/* Console Implementation: Broadcast printk to slaves */
static void virtty_console_write(struct console *co, const char *buf, unsigned int count)
{
	struct virtty_device *vdev = virtty_devices[co->index];
	struct virtty_slave *slave;

	if (!vdev)
		return;

	/*
	 * Console writes can happen in atomic context.
	 * We use rcu_read_lock to safely traverse the list.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(slave, &vdev->slaves, list) {
		if (slave->active && slave->tty && slave->tty->ops->write) {
			/*
			 * Note: Slave TTY write might still block/sleep if not
			 * careful. For serial consoles, we'd ideally use
			 * the low-level con_write.
			 */
			slave->tty->ops->write(slave->tty, (const unsigned char *)buf, count);
		}
	}
	rcu_read_unlock();
}

static struct tty_driver *virtty_console_device(struct console *co, int *index)
{
	*index = co->index;
	return virtty_driver;
}

static int __init virtty_init_device(int index)
{
	struct virtty_device *vdev;
	struct device *dev;

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	vdev->id = index;
	vdev->driver = virtty_driver;
	INIT_LIST_HEAD(&vdev->slaves);
	mutex_init(&vdev->slave_lock);
	tty_port_init(&vdev->port);

	/* Setup console structure */
	snprintf(vdev->console.name, sizeof(vdev->console.name), "virtty");
	vdev->console.write = virtty_console_write;
	vdev->console.device = virtty_console_device;
	vdev->console.flags = CON_PRINTBUFFER;
	vdev->console.index = index;

	virtty_devices[index] = vdev;
	dev = tty_port_register_device_attr(&vdev->port, virtty_driver, index, NULL, vdev, virtty_groups);
	if (IS_ERR(dev)) {
		kfree(vdev);
		virtty_devices[index] = NULL;
		return PTR_ERR(dev);
	}

	register_console(&vdev->console);

	/* Process command line slaves if any */
	if (virtty_cfg[index].active && virtty_cfg[index].slaves) {
		char *s = kstrdup(virtty_cfg[index].slaves, GFP_KERNEL);
		char *slave_str, *p = s;
		while ((slave_str = strsep(&p, ":")) != NULL) {
			char *options = strchr(slave_str, ',');
			if (options) {
				*options = '\0';
				options++;
			}
			virtty_add_slave(vdev, slave_str, options);
		}
		kfree(s);
	}

	return 0;
}

static int __init virtty_init(void)
{
	int ret, i;

	/* Register Line Discipline */
	ret = tty_register_ldisc(N_VIRTTY_SLAVE, &virtty_ldisc_ops);
	if (ret) {
		pr_err("virtty: failed to register ldisc\n");
		return ret;
	}

	bus_register_notifier(&tty_bus_type, &virtty_nb);

	virtty_driver = tty_alloc_driver(MAX_VIRTTY_DEVICES, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(virtty_driver)) {
		tty_unregister_ldisc(N_VIRTTY_SLAVE);
		return PTR_ERR(virtty_driver);
	}

	virtty_driver->driver_name = "virtty";
	virtty_driver->name = "virtty";
	virtty_driver->major = 0;
	virtty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	virtty_driver->subtype = SERIAL_TYPE_NORMAL;
	virtty_driver->init_termios = tty_std_termios;
	tty_set_operations(virtty_driver, &virtty_ops);

	ret = tty_register_driver(virtty_driver);
	if (ret) {
		put_tty_driver(virtty_driver);
		tty_unregister_ldisc(N_VIRTTY_SLAVE);
		return ret;
	}

	/* Initialize devices specified in cmdline */
	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		if (virtty_cfg[i].active)
			virtty_init_device(i);
	}

	/* Always ensure at least virtty0 exists if nothing specified */
	if (!virtty_cfg[0].active)
		virtty_init_device(0);

	return 0;
}

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

static void __exit virtty_exit(void)
{
	int i;
	struct virtty_slave *slave, *next;

	bus_unregister_notifier(&tty_bus_type, &virtty_nb);

	for (i = 0; i < MAX_VIRTTY_DEVICES; i++) {
		struct virtty_device *vdev = virtty_devices[i];
		if (vdev) {
			unregister_console(&vdev->console);
			tty_unregister_device(virtty_driver, i);

			mutex_lock(&vdev->slave_lock);
			list_for_each_entry_safe(slave, next, &vdev->slaves, list) {
				list_del_rcu(&slave->list);
				virtty_free_slave(slave);
			}
			mutex_unlock(&vdev->slave_lock);

			tty_port_destroy(&vdev->port);
			kfree(vdev);
			virtty_devices[i] = NULL;
		}
		if (virtty_cfg[i].slaves)
			kfree(virtty_cfg[i].slaves);
	}
	tty_unregister_driver(virtty_driver);
	put_tty_driver(virtty_driver);
	tty_unregister_ldisc(N_VIRTTY_SLAVE);
}

module_init(virtty_init);
module_exit(virtty_exit);
