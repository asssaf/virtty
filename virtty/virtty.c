#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define DRIVER_NAME "virtty"
#define DEVICE_NAME "virtty"
#define MAX_DEVICES 8
#define N_VIRTTY 29 // N_DEVELOPMENT

struct virtty_instance {
    char slave_name[64];
    int baud_rate;
    int index;
};

struct virtty_port {
    struct tty_port port;
    struct virtty_instance *instance;
    struct tty_struct *slave_tty;
    struct file *slave_file;
    struct mutex lock;
    struct console *slave_console;
};

static struct virtty_port *virtty_ports[MAX_DEVICES];
static struct virtty_instance *virtty_instances[MAX_DEVICES];
static struct tty_driver *virtty_driver;

static void __init virtty_parse_cmdline(void) {
    char *p, *token, *eq, *comma;
    int index, baud;
    static char cmdline[COMMAND_LINE_SIZE];

    strscpy(cmdline, boot_command_line, COMMAND_LINE_SIZE);
    p = cmdline;

    while ((p = strstr(p, "virtty")) != NULL) {
        token = strsep(&p, " ");
        if (!token) break;

        eq = strchr(token, '=');
        if (!eq) continue;
        *eq = '\0';

        if (sscanf(token, "virtty%d", &index) == 1 && index >= 0 && index < MAX_DEVICES) {
            char *slave = eq + 1;
            comma = strchr(slave, ',');
            if (comma) {
                *comma = '\0';
                baud = simple_strtoul(comma + 1, NULL, 10);
            } else {
                baud = 115200;
            }

            if (!virtty_instances[index]) {
                virtty_instances[index] = kzalloc(sizeof(struct virtty_instance), GFP_KERNEL);
                if (virtty_instances[index]) {
                    strscpy(virtty_instances[index]->slave_name, slave, 64);
                    virtty_instances[index]->baud_rate = baud;
                    virtty_instances[index]->index = index;
                    pr_info("virtty: instance %d configured for slave %s at %d baud\n", index, slave, baud);
                }
            }
        }
    }
}

static int virtty_install(struct tty_driver *driver, struct tty_struct *tty) {
    int index = tty->index;
    if (index >= MAX_DEVICES || !virtty_ports[index])
        return -ENODEV;
    tty->port = &virtty_ports[index]->port;
    return tty_standard_install(driver, tty);
}

static void virtty_receive_buf(struct tty_struct *tty, const u8 *cp, const u8 *fp, size_t count) {
    struct virtty_port *vport = tty->disc_data;
    if (vport && vport->port.tty) {
        tty_insert_flip_string(&vport->port, cp, count);
        tty_flip_buffer_push(&vport->port);
    }
}

static int virtty_ldisc_open(struct tty_struct *tty) {
    return 0;
}

static void virtty_ldisc_close(struct tty_struct *tty) {}

static struct tty_ldisc_ops virtty_ldisc_ops = {
    .owner = THIS_MODULE,
    .num = N_VIRTTY,
    .name = "virtty_ldisc",
    .open = virtty_ldisc_open,
    .close = virtty_ldisc_close,
    .receive_buf = virtty_receive_buf,
};

static int virtty_open_slave(struct virtty_port *vport) {
    char path[128];
    struct file *slave_file;
    struct tty_struct *slave_tty;
    int ret = 0;

    mutex_lock(&vport->lock);
    if (vport->slave_tty) {
        mutex_unlock(&vport->lock);
        return 0;
    }

    snprintf(path, sizeof(path), "/dev/%s", vport->instance->slave_name);
    slave_file = filp_open(path, O_RDWR | O_NOCTTY, 0);
    if (IS_ERR(slave_file)) {
        ret = PTR_ERR(slave_file);
        goto out;
    }
    vport->slave_file = slave_file;

    slave_tty = tty_kopen_exclusive(vport->slave_file->f_path.dentry->d_inode->i_rdev);
    if (IS_ERR(slave_tty)) {
        filp_close(vport->slave_file, NULL);
        vport->slave_file = NULL;
        ret = PTR_ERR(slave_tty);
        goto out;
    }
    vport->slave_tty = slave_tty;

    ret = tty_set_ldisc(slave_tty, N_VIRTTY);
    if (ret) {
        tty_kclose(vport->slave_tty);
        vport->slave_tty = NULL;
        filp_close(vport->slave_file, NULL);
        vport->slave_file = NULL;
        goto out;
    }
    slave_tty->disc_data = vport;

out:
    mutex_unlock(&vport->lock);
    return ret;
}

static int virtty_open(struct tty_struct *tty, struct file *file) {
    struct virtty_port *vport = container_of(tty->port, struct virtty_port, port);
    int ret;

    ret = tty_port_open(tty->port, tty, file);
    if (ret) return ret;

    ret = virtty_open_slave(vport);
    if (ret) {
        tty_port_close(tty->port, tty, file);
        return ret;
    }

    return 0;
}

static void virtty_close(struct tty_struct *tty, struct file *file) {
    tty_port_close(tty->port, tty, file);
}

static ssize_t virtty_write(struct tty_struct *tty, const u8 *buffer, size_t count) {
    struct virtty_port *vport = container_of(tty->port, struct virtty_port, port);
    if (vport->slave_tty && vport->slave_tty->ops->write) {
        return vport->slave_tty->ops->write(vport->slave_tty, buffer, count);
    }
    return 0;
}

static unsigned int virtty_write_room(struct tty_struct *tty) {
    struct virtty_port *vport = container_of(tty->port, struct virtty_port, port);
    if (vport->slave_tty && vport->slave_tty->ops->write_room) {
        return vport->slave_tty->ops->write_room(vport->slave_tty);
    }
    return 2048;
}

static void virtty_set_termios(struct tty_struct *tty, const struct ktermios *old) {
    struct virtty_port *vport = container_of(tty->port, struct virtty_port, port);
    if (vport->slave_tty && vport->slave_tty->ops->set_termios) {
        vport->slave_tty->ops->set_termios(vport->slave_tty, &tty->termios);
    }
}

static const struct tty_operations virtty_ops = {
    .install = virtty_install,
    .open = virtty_open,
    .close = virtty_close,
    .write = virtty_write,
    .write_room = virtty_write_room,
    .set_termios = virtty_set_termios,
};

static void virtty_console_write(struct console *co, const char *s, unsigned int count) {
    struct virtty_port *vport = (co->index >= 0 && co->index < MAX_DEVICES) ? virtty_ports[co->index] : NULL;

    if (vport && vport->slave_console && vport->slave_console->write) {
        // Safe direct call to slave console write.
        // We assume the caller handles the console lock.
        vport->slave_console->write(vport->slave_console, s, count);
    }
}

static struct tty_driver *virtty_console_device(struct console *co, int *index) {
    *index = co->index;
    return virtty_driver;
}

static int virtty_console_setup(struct console *co, char *options) {
    if (co->index < 0 || co->index >= MAX_DEVICES || !virtty_ports[co->index])
        return -ENODEV;
    return 0;
}

static struct console virtty_console = {
    .name = DEVICE_NAME,
    .write = virtty_console_write,
    .device = virtty_console_device,
    .setup = virtty_console_setup,
    .flags = CON_PRINTBUFFER | CON_ANYTIME,
    .index = -1,
};

static int __init virtty_init(void) {
    int i, ret;
    struct console *c;

    virtty_parse_cmdline();

    ret = tty_register_ldisc(&virtty_ldisc_ops);
    if (ret) {
        pr_err("virtty: failed to register ldisc\n");
        goto err_out;
    }

    virtty_driver = tty_alloc_driver(MAX_DEVICES, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
    if (IS_ERR(virtty_driver)) {
        ret = PTR_ERR(virtty_driver);
        goto err_ldisc;
    }

    virtty_driver->driver_name = DRIVER_NAME;
    virtty_driver->name = DEVICE_NAME;
    virtty_driver->major = 0;
    virtty_driver->minor_start = 0;
    virtty_driver->type = TTY_DRIVER_TYPE_SERIAL;
    virtty_driver->subtype = SERIAL_TYPE_NORMAL;
    virtty_driver->init_termios = tty_std_termios;
    virtty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
    tty_set_operations(virtty_driver, &virtty_ops);

    ret = tty_register_driver(virtty_driver);
    if (ret) goto err_put;

    for (i = 0; i < MAX_DEVICES; i++) {
        if (virtty_instances[i]) {
            virtty_ports[i] = kzalloc(sizeof(struct virtty_port), GFP_KERNEL);
            if (!virtty_ports[i]) {
                ret = -ENOMEM;
                goto err_ports;
            }
            virtty_ports[i]->instance = virtty_instances[i];
            tty_port_init(&virtty_ports[i]->port);
            mutex_init(&virtty_ports[i]->lock);
            tty_port_register_device(&virtty_ports[i]->port, virtty_driver, i, NULL);

            // Locate slave console
            console_lock();
            for_each_console(c) {
                if (strncmp(virtty_instances[i]->slave_name, c->name, strlen(c->name)) == 0) {
                    virtty_ports[i]->slave_console = c;
                    break;
                }
            }
            console_unlock();
        }
    }

    register_console(&virtty_console);
    pr_info("virtty: driver initialized\n");
    return 0;

err_ports:
    for (i = 0; i < MAX_DEVICES; i++) {
        if (virtty_ports[i]) {
            tty_unregister_device(virtty_driver, i);
            tty_port_destroy(&virtty_ports[i]->port);
            kfree(virtty_ports[i]);
        }
    }
    tty_unregister_driver(virtty_driver);
err_put:
    put_tty_driver(virtty_driver);
err_ldisc:
    tty_unregister_ldisc(&virtty_ldisc_ops);
err_out:
    for (i = 0; i < MAX_DEVICES; i++) {
        if (virtty_instances[i]) kfree(virtty_instances[i]);
    }
    return ret;
}

static void __exit virtty_exit(void) {
    int i;

    unregister_console(&virtty_console);
    tty_unregister_ldisc(&virtty_ldisc_ops);

    for (i = 0; i < MAX_DEVICES; i++) {
        if (virtty_ports[i]) {
            if (virtty_ports[i]->slave_tty) {
                tty_kclose(virtty_ports[i]->slave_tty);
            }
            if (virtty_ports[i]->slave_file) {
                filp_close(virtty_ports[i]->slave_file, NULL);
            }
            tty_unregister_device(virtty_driver, i);
            tty_port_destroy(&virtty_ports[i]->port);
            kfree(virtty_ports[i]);
        }
        if (virtty_instances[i]) {
            kfree(virtty_instances[i]);
        }
    }
    tty_unregister_driver(virtty_driver);
    put_tty_driver(virtty_driver);
    pr_info("virtty: driver exited\n");
}

module_init(virtty_init);
module_exit(virtty_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jules");
MODULE_DESCRIPTION("Virtual TTY device multiplexer/proxy");
