/*
 * Copyright 2013, 2021 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/libata.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/leds.h>

/*
 * Exported by libahci, but declared only in drivers/ata/libahci.h.  Contains
 * pointers to low-level functions used by libahci "client" drivers -- ahci,
 * acard_ahci, ahci_platform, sata_highbank, and possibly others.
 */
extern struct ata_port_operations ahci_ops;

/*
 * Each "blink" of an LED will turn it on for blink_on_ms and then turn it
 * off for blink_off_ms.
 */
static unsigned long blink_on_ms = 75;
module_param(blink_on_ms, ulong, 0644);
MODULE_PARM_DESC(blink_on_ms, "LED blink on ms (default 75, max 1000)");
static unsigned long blink_off_ms = 25;
module_param(blink_off_ms, ulong, 0644);
MODULE_PARM_DESC(blink_off_ms, "LED blink off ms (default 25, max 1000)");

/*
 * Everything related to "hooking" (or unhooking) libahci's qc_issue function
 * and registering/unregistering the LED triggers is protected by this mutex.
 */
DEFINE_MUTEX(n5550_ahci_leds_hook_mutex);
static int n5550_ahci_leds_hook_active = 0;
/* Original value of ahci_ops.qc_issue */
static unsigned int (*libahci_qc_issue)(struct ata_queued_cmd *);

static struct led_trigger n5550_ahci_led_triggers[5] = {
	{ .name = "n5550-ahci-0" },
	{ .name = "n5550-ahci-1" },
	{ .name = "n5550-ahci-2" },
	{ .name = "n5550-ahci-3" },
	{ .name = "n5550-ahci-4" },
};

static struct class *n5550_ahci_leds_sysfs_class;

static ssize_t enabled_show(struct class *const class,
			    struct class_attribute *const attr, char *const buf)
{
	int i;

	i = mutex_lock_interruptible(&n5550_ahci_leds_hook_mutex);
	if (i != 0) {
		pr_warn("n5550_ahci_leds: "
			"Couldn't lock n5550_ahci_leds_hook_mutex\n");
		return i;
	}

	i = n5550_ahci_leds_hook_active;
	mutex_unlock(&n5550_ahci_leds_hook_mutex);
	return sprintf(buf, "%d\n", i);
}

static ssize_t enabled_store(struct class *const class,
			     struct class_attribute *const attr,
			     const char *const buf, const size_t count)
{
	struct module *libahci;
	int ret, i;

	ret = kstrtoint(buf, 0, &i);
	if (ret != 0) {
		pr_warn("n5550_ahci_leds: "
			"Couldn't parse write to 'enabled' attribute\n");
		return ret;
	}
	if (i != 0) {
		pr_warn("n5550_ahci_leds: "
			"Non-zero value written to 'enabled' attribute\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&n5550_ahci_leds_hook_mutex);
	if (ret != 0) {
		pr_warn("n5550_ahci_leds: "
			"Couldn't lock n5550_ahci_leds_hook_mutex\n");
		return ret;
	}

	if (!n5550_ahci_leds_hook_active) {
		pr_info("n5550_ahci_leds: LED hook already disabled\n");
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return (ssize_t)count;
	}

	ret = mutex_lock_interruptible(&module_mutex);
	if (ret != 0) {
		pr_warn("n5550_ahci_leds: Couldn't lock module_mutex\n");
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return ret;
	}

	libahci = find_module("libahci");
	if (libahci == NULL) {
		/* Should never happen; this module depends on libahci */
		pr_warn("n5550_ahci_leds: "
			"Couldn't get reference to libahci module\n");
		mutex_unlock(&module_mutex);
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return ret;
	}

	if (module_refcount(libahci) > 1) {
		pr_warn("n5550_ahci_leds: "
			"libahci module in use; cannot disable LED hook\n");
		mutex_unlock(&module_mutex);
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return -EBUSY;
	}

	ahci_ops.qc_issue = libahci_qc_issue;
	for (i = 0; i < 5; ++i)
		led_trigger_unregister(&n5550_ahci_led_triggers[i]);
	n5550_ahci_leds_hook_active = 0;
	module_put(THIS_MODULE);
	mutex_unlock(&module_mutex);
	mutex_unlock(&n5550_ahci_leds_hook_mutex);
	pr_info("n5550_ahci_leds: Successfully disabled LED hook\n");

	return (ssize_t)count;
}

static CLASS_ATTR_RW(enabled);

static int n5550_create_sysfs_file(void)
{
	int ret;

	n5550_ahci_leds_sysfs_class = class_create(THIS_MODULE,
						   "n5550_ahci_leds");
	if (IS_ERR(n5550_ahci_leds_sysfs_class))
		return PTR_ERR(n5550_ahci_leds_sysfs_class);

	ret = class_create_file(n5550_ahci_leds_sysfs_class,
				&class_attr_enabled);
	if (ret != 0) {
		class_destroy(n5550_ahci_leds_sysfs_class);
		return ret;
	}

	return 0;
}

static void n5550_destroy_sysfs_file(void)
{
	class_remove_file(n5550_ahci_leds_sysfs_class, &class_attr_enabled);
	class_destroy(n5550_ahci_leds_sysfs_class);
}

static unsigned int n5550_ahci_leds_qc_issue(struct ata_queued_cmd *qc)
{
	unsigned long delay_on, delay_off;
	int port = qc->ap->port_no;
	int ret;

	/*
	 * No locking around blink_{on,off}_ms.  Waiting on a lock in the disk
	 * I/O path would be "bad", and it isn't critical that every CPU pick up
	 * a change immediately.  Limiting each to 1000 ms ensures that even
	 * garbage from an incomplete non-atomic change to one of the parameters
	 * won't have any long-term effect.
	 */

	delay_on = blink_on_ms;
	if (delay_on > 1000)
		delay_on = 1000;

	delay_off = blink_off_ms;
	if (delay_off > 1000)
		delay_off = 1000;

	if ((ret = libahci_qc_issue(qc)) == 0 && port >= 1 && port <= 5) {
		led_trigger_blink_oneshot(&n5550_ahci_led_triggers[port - 1],
					  &delay_on, &delay_off, 0);
	}

	return ret;
}

static int __init n5550_ahci_leds_init(void)
{
	struct module *libahci;
	int i, ret;

	ret = mutex_lock_interruptible(&n5550_ahci_leds_hook_mutex);
	if (ret != 0) {
		pr_warn("n5550_ahci_leds: "
			"Couldn't lock n5550_ahci_leds_hook_mutex\n");
		return ret;
	}

	ret = n5550_create_sysfs_file();
	if (ret != 0) {
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return ret;
	}

	/*
	 * Lock module_mutex while checking that no other libahci clients are
   	 * already loaded and then doing our dirty work.
	 */
	ret = mutex_lock_interruptible(&module_mutex);
	if (ret != 0) {
		pr_warn("n5550_ahci_leds: Couldn't lock module_mutex\n");
		n5550_destroy_sysfs_file();
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return ret;
	}

	libahci = find_module("libahci");
	if (libahci == NULL) {
		/* Should never happen; this module depends on libahci */
		pr_warn("n5550_ahci_leds: "
			"Couldn't get reference to libahci module\n");
		mutex_unlock(&module_mutex);
		n5550_destroy_sysfs_file();
		mutex_unlock(&n5550_ahci_leds_hook_mutex);
		return -ENOENT;
	}

	/*
	 * If we get this far, we're definitely going to load the module, even
	 * if we don't actually hook the qc_issue function.
	 */

	if (module_refcount(libahci) > 1) {
		pr_warn("n5550_ahci_leds: "
			"libahci module already in use; LED hook disabled\n");
		n5550_ahci_leds_hook_active = 0;
		goto done;
	}

	if (!try_module_get(THIS_MODULE)) {
		pr_warn("n5550_ahci_leds: Couldn't increment module use count; "
			"LED hook disabled\n");
		n5550_ahci_leds_hook_active = 0;
		goto done;
	}

        libahci_qc_issue = ahci_ops.qc_issue;
        ahci_ops.qc_issue = n5550_ahci_leds_qc_issue;
	n5550_ahci_leds_hook_active = 1;

	for (i = 0; i < 5; ++i) {
		led_trigger_register(&n5550_ahci_led_triggers[i]);
	}

done:
	mutex_unlock(&module_mutex);
	mutex_unlock(&n5550_ahci_leds_hook_mutex);
	return ret;
}

static void __exit n5550_ahci_leds_exit(void)
{
	n5550_destroy_sysfs_file();
}

MODULE_AUTHOR("Ian Pilcher <arequipeno@gmail.com>");
MODULE_DESCRIPTION("AHCI driver \"hook\" for Thecus N5550 drive LEDs");
MODULE_LICENSE("GPL v2");

module_init(n5550_ahci_leds_init);
module_exit(n5550_ahci_leds_exit);
