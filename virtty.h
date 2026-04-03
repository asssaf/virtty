#ifndef _VIRTTY_H
#define _VIRTTY_H

#include <linux/tty.h>
#include <linux/console.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

#define MAX_VIRTTY_DEVICES 8
#define VIRTTY_NAME "virtty"

/**
 * struct virtty_slave - Represents a slave TTY attached to a virtty master
 * @list:       List head for linking to virtty_device
 * @tty:        The kernel-side TTY pointer for the slave
 * @name:       Name of the slave device (e.g., "ttyS0")
 * @options:    Baud rate and other settings as a string
 * @active:     Whether the slave is currently open and usable
 */
struct virtty_slave {
	struct list_head list;
	struct tty_struct *tty;
	char *name;
	char *options;
	bool active;
	struct rcu_head rcu;
};

/**
 * struct virtty_device - The virtual TTY master device
 * @id:             Index of the device (0 for virtty0)
 * @tty_driver:     Pointer to the common virtty TTY driver
 * @slaves:         List of attached virtty_slave structures
 * @slave_lock:     Mutex protecting the slave list
 * @console:        Console structure for printk forwarding
 * @port:           TTY port for this device
 */
struct virtty_device {
	int id;
	struct tty_driver *driver;
	struct list_head slaves;
	struct mutex slave_lock;
	struct console console;
	struct tty_port port;
};

#endif /* _VIRTTY_H */
