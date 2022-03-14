// SPDX-License-Identifier: GPL-2.0-only

/*
 *	Block device LED trigger
 *
 *	Copyright 2021 Ian Pilcher <arequipeno@gmail.com>
 */

#include <linux/blkdev.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/part_stat.h>
#include <linux/xarray.h>

/* Default, minimum & maximum blink duration (milliseconds) */
#define BLKDEV_TRIG_BLINK_DEF	75
#define BLKDEV_TRIG_BLINK_MIN	10
#define BLKDEV_TRIG_BLINK_MAX	86400000  /* 24 hours */

/* Default, minimum & maximum activity check interval (milliseconds) */
#define BLKDEV_TRIG_CHECK_DEF	100
#define BLKDEV_TRIG_CHECK_MIN	25
#define BLKDEV_TRIG_CHECK_MAX	86400000

/*
 * If blkdev_trig_check() can't lock the mutex, how long to wait before trying
 * again (milliseconds)
 */
#define BLKDEV_TRIG_CHECK_RETRY	5

/* Mode for blkdev_get_by_path() & blkdev_put() */
#define BLKDEV_TRIG_FMODE	0

/* Device activity type(s) that will make LED blink */
#define BLKDEV_TRIG_READ	(1 << STAT_READ)
#define BLKDEV_TRIG_WRITE	(1 << STAT_WRITE)
#define BLKDEV_TRIG_DISCARD	(1 << STAT_DISCARD)
#define BLKDEV_TRIG_FLUSH	(1 << STAT_FLUSH)

/* When unlinking a block device from an LED, is the blkdev being released? */
enum blkdev_trig_unlink_mode {
	BLKDEV_TRIG_RELEASING,
	BLKDEV_TRIG_NOT_RELEASING
};

/* Every block device linked to at least one LED gets a "BTB" */
struct blkdev_trig_bdev {
	unsigned long		last_checked;
	unsigned long		last_activity[NR_STAT_GROUPS];
	unsigned long		ios[NR_STAT_GROUPS];
	unsigned long		index;
	struct block_device	*bdev;
	struct xarray		linked_leds;
};

/* Every LED associated with the blkdev trigger gets one of these */
struct blkdev_trig_led {
	unsigned long		last_checked;
	unsigned long		index;
	unsigned long		mode;  /* must be ulong for atomic bit ops */
	struct led_classdev	*led_cdev;
	unsigned int		blink_msec;
	unsigned int		check_jiffies;
	struct xarray		linked_btbs;
	struct hlist_node	all_leds_node;
};

/* Forward declarations to make this file compile in a more readable order */
static void blkdev_trig_check(struct work_struct *work);
static struct blkdev_trig_bdev *blkdev_trig_get_btb(const char *buf,
						    size_t size);
static struct block_device *blkdev_trig_get_bdev(const char *buf, size_t size,
						 fmode_t mode);
static int blkdev_trig_link(struct blkdev_trig_led *led,
			    struct blkdev_trig_bdev *btb);
static void blkdev_trig_put_btb(struct blkdev_trig_bdev *btb);
static void blkdev_trig_btb_release(struct device *dev, void *res);
static void blkdev_trig_unlink(struct blkdev_trig_led *led,
			       struct blkdev_trig_bdev *btb,
			       enum blkdev_trig_unlink_mode unlink_mode);
static void blkdev_trig_update_btb(struct blkdev_trig_bdev *btb,
				   unsigned long now);
static bool blkdev_trig_blink(const struct blkdev_trig_led *led,
			      const struct blkdev_trig_bdev *btb);
static void blkdev_trig_sched_led(const struct blkdev_trig_led *led);

/* Index for next BTB or LED */
static unsigned long blkdev_trig_next_index;

/* Protects everything except sysfs attributes */
static DEFINE_MUTEX(blkdev_trig_mutex);

/* All LEDs associated with the trigger */
static HLIST_HEAD(blkdev_trig_all_leds);

/* Delayed work to periodically check for activity & blink LEDs */
static DECLARE_DELAYED_WORK(blkdev_trig_work, blkdev_trig_check);

/* When is the delayed work scheduled to run next (jiffies) */
static unsigned long blkdev_trig_next_check;

/* Total number of device-to-LED associations (links) */
static unsigned int blkdev_trig_link_count;

/* Empty attribute list for the linked_leds & linked_devices "groups" */
static struct attribute *blkdev_trig_attrs_empty[] = { NULL };

/* linked_leds sysfs directory for block devs linked to 1 or more LEDs */
static const struct attribute_group blkdev_trig_linked_leds = {
	.name	= "linked_leds",
	.attrs	= blkdev_trig_attrs_empty,
};

/* linked_devices sysfs directory for each LED associated with the trigger */
static const struct attribute_group blkdev_trig_linked_devs = {
	.name	= "linked_devices",
	.attrs	= blkdev_trig_attrs_empty,
};

/**
 * blkdev_trig_activate() - Called when an LED is associated with the trigger.
 * @led_cdev:	The LED
 *
 * Allocates & initializes the @blkdev_trig_led structure, adds it to the
 * @blkdev_trig_all_leds list, and sets the LED's trigger data.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 * Return:	``0`` on success, ``-errno`` on error.
 */
static int blkdev_trig_activate(struct led_classdev *led_cdev)
{
	struct blkdev_trig_led *led;
	int err;

	led = kzalloc(sizeof(*led), GFP_KERNEL);
	if (led == NULL)
		return -ENOMEM;

	err = mutex_lock_interruptible(&blkdev_trig_mutex);
	if (err)
		goto exit_free;

	if (blkdev_trig_next_index == ULONG_MAX) {
		err = -EOVERFLOW;
		goto exit_unlock;
	}

	led->index = blkdev_trig_next_index++;
	led->last_checked = jiffies;
	led->mode = -1;  /* set all bits */
	led->led_cdev = led_cdev;
	led->blink_msec = BLKDEV_TRIG_BLINK_DEF;
	led->check_jiffies = msecs_to_jiffies(BLKDEV_TRIG_CHECK_DEF);
	xa_init(&led->linked_btbs);

	hlist_add_head(&led->all_leds_node, &blkdev_trig_all_leds);
	led_set_trigger_data(led_cdev, led);

exit_unlock:
	mutex_unlock(&blkdev_trig_mutex);
exit_free:
	if (err)
		kfree(led);
	return err;
}

/**
 * link_device_store() - ``link_device`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``link_device`` attribute (@dev_attr_link_device)
 * @buf:	The value written to the attribute, which should be the path to
 *		the special file that represents the block device to be linked
 *		to the LED (e.g. /dev/sda)
 * @count:	The number of characters in @buf
 *
 * Calls blkdev_trig_get_btb() to find or create the BTB for the block device,
 * checks that the device isn't already linked to this LED, and calls
 * blkdev_trig_link() to create the link.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t link_device_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);
	struct blkdev_trig_bdev *btb;
	int err;

	err = mutex_lock_interruptible(&blkdev_trig_mutex);
	if (err)
		return err;

	btb = blkdev_trig_get_btb(buf, count);
	if (IS_ERR(btb)) {
		err = PTR_ERR(btb);
		goto exit_unlock;
	}

	if (xa_load(&btb->linked_leds, led->index) != NULL) {
		err = -EEXIST;
		goto exit_put_btb;
	}

	err = blkdev_trig_link(led, btb);

exit_put_btb:
	if (err)
		blkdev_trig_put_btb(btb);
exit_unlock:
	mutex_unlock(&blkdev_trig_mutex);
	return err ? : count;
}

/**
 * blkdev_trig_get_btb() - Find or create the BTB for a block device.
 * @buf:	The value written to the ``link_device`` attribute, which should
 *		be the path to a special file that represents a block device
 * @count:	The number of characters in @buf
 *
 * Calls blkdev_trig_get_bdev() to get the block device represented by the path
 * in @buf.  If the device already has a BTB (because it is already linked to
 * an LED), simply returns the existing BTB.
 *
 * Otherwise, allocates a new BTB (as a device resource), creates the block
 * device's ``linked_leds`` directory (attribute group), calls
 * blkdev_trig_update_btb() to set the BTB's activity counters, and adds the
 * BTB resource to the block device.
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 * Return:	Pointer to the BTB, error pointer on error.
 */
static struct blkdev_trig_bdev *blkdev_trig_get_btb(const char *buf,
						    size_t count)
{
	struct block_device *bdev;
	struct blkdev_trig_bdev *btb;
	int err;

	bdev = blkdev_trig_get_bdev(buf, count, BLKDEV_TRIG_FMODE);
	if (IS_ERR(bdev))
		return ERR_CAST(bdev);

	btb = devres_find(&bdev->bd_device, blkdev_trig_btb_release,
			  NULL, NULL);
	if (btb != NULL) {
		err = 0;
		goto exit_put_bdev;
	}

	if (blkdev_trig_next_index == ULONG_MAX) {
		err = -EOVERFLOW;
		goto exit_put_bdev;
	}

	btb = devres_alloc(blkdev_trig_btb_release, sizeof(*btb), GFP_KERNEL);
	if (btb == NULL) {
		err = -ENOMEM;
		goto exit_put_bdev;
	}

	err = sysfs_create_group(bdev_kobj(bdev), &blkdev_trig_linked_leds);
	if (err)
		goto exit_free_btb;

	btb->index = blkdev_trig_next_index++;
	btb->bdev = bdev;
	xa_init(&btb->linked_leds);
	blkdev_trig_update_btb(btb, jiffies);

	devres_add(&bdev->bd_device, btb);

exit_free_btb:
	if (err)
		devres_free(btb);
exit_put_bdev:
	blkdev_put(bdev, BLKDEV_TRIG_FMODE);
	return err ? ERR_PTR(err) : btb;
}

/**
 * blkdev_trig_get_bdev() - Get a block device by path.
 * @buf:	The value written to the ``link_device`` or ``unlink_device``
 *		attribute, which should be the path to a special file that
 *		represents a block device
 * @count:	The number of characters in @buf (not including its terminating
 *		null)
 *
 * Copies @buf to a writable buffer, trims the trailing newline (if any), and
 * calls blkdev_get_by_path() to resolve the block device.
 *
 * The caller must call blkdev_put() when finished with the device.
 *
 * Context:	Process context.
 * Return:	The block device, or an error pointer.
 */
static struct block_device *blkdev_trig_get_bdev(const char *buf, size_t count,
						 fmode_t mode)
{
	struct block_device *bdev;
	char *path;

	path = kmemdup(buf, count + 1, GFP_KERNEL);  /* +1 to include null */
	if (path == NULL)
		return ERR_PTR(-ENOMEM);

	if (path[count - 1] == '\n')
		path[count - 1] = 0;

	bdev = blkdev_get_by_path(path, mode, THIS_MODULE);
	kfree(path);
	return bdev;
}

/**
 * blkdev_trig_update_btb() - Update a BTB's activity counters.
 * @btb:	The BTB
 *
 * Checks each of the BTB's block device's I/O counters.  If the counter has
 * changed since the last check, updates the counter and its timestamp in the
 * BTB.
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 */
static void blkdev_trig_update_btb(struct blkdev_trig_bdev *btb,
				   unsigned long now)
{
	unsigned long new_ios;
	enum stat_group i;

	for (i = STAT_READ; i <= STAT_FLUSH; ++i) {

		new_ios = part_stat_read(btb->bdev, ios[i]);

		if (new_ios != btb->ios[i]) {
			btb->ios[i] = new_ios;
			btb->last_activity[i] = now;
		}
	}

	btb->last_checked = now;
}

/**
 * blkdev_trig_link() - "Link" a block device to an LED.
 * @led:	The LED
 * @btb:	The block device
 *
 * Called from link_device_store() to create the link between an LED and a
 * block device.
 *
 *   * Adds block device symlink to LED's ``linked_devices`` directory.
 *   * Adds LED symlink to block devices's ``linked_leds`` directory.
 *   * Adds the BTB to the LED's @linked_btbs and adds the LED to the BTB's
 *     @linked_leds.
 *   * If this is the first block device linked to this LED, calls
 *     blkdev_trig_new_sched() to (if needed) schedule or reschedule the delayed
 *     work which periodically checks for block device activity and blinks LEDs.
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 * Return:	0 on success, ``-errno`` on error.
 */
static int blkdev_trig_link(struct blkdev_trig_led *led,
			    struct blkdev_trig_bdev *btb)
{
	bool led_first_link;
	int err;

	led_first_link = xa_empty(&led->linked_btbs);

	err = xa_insert(&btb->linked_leds, led->index, led, GFP_KERNEL);
	if (err)
		return err;

	err = xa_insert(&led->linked_btbs, btb->index, btb, GFP_KERNEL);
	if (err)
		goto error_erase_led;

	/* Create /sys/class/block/<bdev>/linked_leds/<led> symlink */
	err = sysfs_add_link_to_group(bdev_kobj(btb->bdev),
				      blkdev_trig_linked_leds.name,
				      &led->led_cdev->dev->kobj,
				      led->led_cdev->name);
	if (err)
		goto error_erase_btb;

	/* Create /sys/class/leds/<led>/linked_devices/<bdev> symlink */
	err = sysfs_add_link_to_group(&led->led_cdev->dev->kobj,
				      blkdev_trig_linked_devs.name,
				      bdev_kobj(btb->bdev),
				      dev_name(&btb->bdev->bd_device));
	if (err)
		goto error_remove_symlink;

	/*
	 * If this isn't the first block device linked to this LED, then the
	 * delayed work schedule already reflects this LED.
	 */
	if (led_first_link)
		blkdev_trig_sched_led(led);

	++blkdev_trig_link_count;

	return 0;

error_remove_symlink:
	sysfs_remove_link_from_group(bdev_kobj(btb->bdev),
				     blkdev_trig_linked_leds.name,
				     led->led_cdev->name);
error_erase_btb:
	xa_erase(&led->linked_btbs, btb->index);
error_erase_led:
	xa_erase(&btb->linked_leds, led->index);
	return err;
}

/**
 * blkdev_trig_sched_led() - Set the schedule of the delayed work when a new
 *			     LED is added to the schedule.
 * @led:	The LED
 *
 * Called from blkdev_trig_link() to set or adjust the schedule of the delayed
 * work which periodically checks block devices for activity and blinks LEDs,
 * if necessary.
 *
 *   * If no other links exist, the delayed work is scheduled.
 *   * If the delayed work is already scheduled to run soon enough to
 *     accommodate the newly linked LED's @check_jiffies, no change is made to
 *     the delayed work's schedule.
 *   * If the delayed work is already scheduled, but it isn't scheduled to
 *     run soon enough, the schedule is modified.
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 */
static void blkdev_trig_sched_led(const struct blkdev_trig_led *led)
{
	unsigned long delay = READ_ONCE(led->check_jiffies);
	unsigned long check_by = jiffies + delay;

	if (blkdev_trig_link_count == 0) {
		WARN_ON(!schedule_delayed_work(&blkdev_trig_work, delay));
		blkdev_trig_next_check = check_by;
		return;
	}

	if (time_after_eq(check_by, blkdev_trig_next_check))
		return;

	WARN_ON(!mod_delayed_work(system_wq, &blkdev_trig_work, delay));
	blkdev_trig_next_check = check_by;
}

/**
 * unlink_device_store() - ``unlink_device`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``unlink_device`` attribute (@dev_attr_unlink_device)
 * @buf:	The value written to the attribute, which should be the path to
 *		the special file that represents the block device to be unlinked
 *		from the LED (e.g. /dev/sda)
 * @count:	The number of characters in @buf
 *
 * Block device name is written to the attribute to "unlink" the block device
 * from the LED.  I.e. the LED will no longer blink to show activity on that
 * block device.
 *
 * Calls blkdev_trig_get_bdev() to get the block device represented by the path
 * in @buf.  If the device has a BTB, searches the BTB's list of LEDs for a
 * link to this LED and (if found) calls blkdev_trig_unlink() to destroy the
 * link.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t unlink_device_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);
	struct block_device *bdev;
	struct blkdev_trig_bdev *btb;
	int err;

	bdev = blkdev_trig_get_bdev(buf, count, BLKDEV_TRIG_FMODE);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	err = mutex_lock_interruptible(&blkdev_trig_mutex);
	if (err)
		goto exit_put_bdev;

	btb = devres_find(&bdev->bd_device, blkdev_trig_btb_release,
			  NULL, NULL);
	if (btb == NULL) {
		err = -EUNATCH;  /* bdev isn't linked to any LED */
		goto exit_unlock;
	}

	if (xa_load(&btb->linked_leds, led->index) == NULL) {
		err = -EUNATCH;  /* bdev isn't linked to this LED */
		goto exit_unlock;
	}

	blkdev_trig_unlink(led, btb, BLKDEV_TRIG_NOT_RELEASING);

exit_unlock:
	mutex_unlock(&blkdev_trig_mutex);
exit_put_bdev:
	blkdev_put(bdev, BLKDEV_TRIG_FMODE);
	return err ? : count;
}

/**
 * blkdev_trig_unlink() - "Unlink" a block device from an LED.
 * @led:		The LED
 * @btb:		The block device
 * @unlink_mode:	Indicates whether the BTB is being released (because
 *			the block device has been removed)
 *
 * Removes the link between an LED and a block device.
 *
 *   * Removes the BTB from the LED's @linked_btbs and removes the LED from
 *     the BTB's @linked_leds.
 *   * Removes the block device symlink from the LED's ``linked_devices``
 *     directory.
 *
 * If the block device is **not** being released:
 *
 *   * Removes the LED symlink from the block device's ``linked_leds``
 *     directory.
 *   * Calls blkdev_trig_put_btb() to clean up the BTB, if required.
 *
 * If the removed link was the only one (i.e. there are no existing block
 * device/LED links after its removal), cancels the periodic delayed work
 * which checks for device activity.
 *
 * This function is called from multiple locations.
 *
 *   * unlink_device_store() calls this function when a block device is unlinked
 *     from an LED via the ``unlink_device`` sysfs attribute.  (@unlink_mode ==
 *     ``BLKDEV_TRIG_NOT_RELEASING``)
 *   * blkdev_trig_deactivate() calls this function for each block device linked
 *     to an LED that is being deactivated (disassociated from the trigger).
 *     (@unlink_mode == ``BLKDEV_TRIG_NOT_RELEASING``).
 *   * blkdev_trig_btb_release() calls this function for each LED linked to a
 *     block device that has been removed from the system.  (@unlink_mode ==
 *     ``BLKDEV_TRIG_RELEASING).
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 */
static void blkdev_trig_unlink(struct blkdev_trig_led *led,
			       struct blkdev_trig_bdev *btb,
			       enum blkdev_trig_unlink_mode unlink_mode)
{
	--blkdev_trig_link_count;

	if (blkdev_trig_link_count == 0)
		WARN_ON(!cancel_delayed_work_sync(&blkdev_trig_work));

	xa_erase(&btb->linked_leds, led->index);
	xa_erase(&led->linked_btbs, btb->index);

	/* Remove /sys/class/leds/<led>/linked_devices/<bdev> symlink */
	sysfs_remove_link_from_group(&led->led_cdev->dev->kobj,
				     blkdev_trig_linked_devs.name,
				     dev_name(&btb->bdev->bd_device));

	/*
	 * If the BTB is being released, the device's attribute groups have
	 * already been removed, and the BTB will be freed automatically, so
	 * only do these steps if the BTB is not being released.
	 */
	if (unlink_mode == BLKDEV_TRIG_NOT_RELEASING) {

		/* Remove /sys/class/block/<bdev>/linked_leds/<led> symlink */
		sysfs_remove_link_from_group(bdev_kobj(btb->bdev),
					     blkdev_trig_linked_leds.name,
					     led->led_cdev->name);
		blkdev_trig_put_btb(btb);
	}
}

/**
 * blkdev_trig_put_btb() - Remove and free a BTB, if it is no longer needed.
 * @btb:	The BTB
 *
 * Does nothing if the BTB (block device) is still linked to at least one LED.
 *
 * If the BTB is no longer linked to any LEDs, removes the block device's
 * ``linked_leds`` directory (attribute group), removes the BTB from the
 * block device's resource list, and frees the BTB.
 *
 * Called from blkdev_trig_unlink() (and in the link_device_store() error path).
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 */
static void blkdev_trig_put_btb(struct blkdev_trig_bdev *btb)
{
	struct block_device *bdev = btb->bdev;
	int err;

	if (xa_empty(&btb->linked_leds)) {

		sysfs_remove_group(bdev_kobj(bdev), &blkdev_trig_linked_leds);
		err = devres_destroy(&bdev->bd_device, blkdev_trig_btb_release,
				     NULL, NULL);
		WARN_ON(err);
	}
}

/**
 * blkdev_trig_deactivate() - Called when an LED is disassociated from the
 *			      trigger.
 * @led_cdev:	The LED
 *
 * Calls blkdev_trig_unlink() for each block device linked to the LED, removes
 * the LED from the @blkdevtrig_all_leds list, and frees the @blkdev_trig_led.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 */
static void blkdev_trig_deactivate(struct led_classdev *led_cdev)
{
	struct blkdev_trig_led *led = led_get_trigger_data(led_cdev);
	struct blkdev_trig_bdev *btb;
	unsigned long index;

	mutex_lock(&blkdev_trig_mutex);

	xa_for_each (&led->linked_btbs, index, btb)
		blkdev_trig_unlink(led, btb, BLKDEV_TRIG_NOT_RELEASING);

	hlist_del(&led->all_leds_node);
	kfree(led);

	mutex_unlock(&blkdev_trig_mutex);
}

/**
 * blkdev_trig_btb_release() - BTB device resource release function.
 * @dev:	The block device
 * @res:	The BTB
 *
 * Called by the driver core when a block device with a BTB is removed from
 * the system.  Calls blkdev_trig_unlink() for each LED linked to the block
 * device.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 */
static void blkdev_trig_btb_release(struct device *dev, void *res)
{
	struct blkdev_trig_bdev *btb = res;
	struct blkdev_trig_led *led;
	unsigned long index;

	mutex_lock(&blkdev_trig_mutex);

	xa_for_each (&btb->linked_leds, index, led)
		blkdev_trig_unlink(led, btb, BLKDEV_TRIG_RELEASING);

	mutex_unlock(&blkdev_trig_mutex);
}

/**
 * blkdev_trig_check() - Check linked devices for activity and blink LEDs.
 * @work:	Delayed work (@blkdev_trig_work)
 *
 * Called periodically (as delayed work) to check linked block devices for
 * activity and blink LEDs.
 *
 *   * Iterates through all LEDs associated with the trigger.
 *   * If an LED is due to be checked, iterates through the block devices (BTBs)
 *     linked to the LED.
 *   * If a block device has not already been checked during this pass, calls
 *     blkdev_trig_update_btb() to update the BTB's activity counters and
 *     timestamps.
 *   * If the LED has not already been blinked during this pass, calls
 *     blkdev_trig_blink() to blink it if the correct type of activity has
 *     occurred since the LED was last checked.
 *
 * When finished, schedules itself to run again when the next LED is due to be
 * checked.
 *
 * Context:	Process context.  Takes and releases @blkdev_trig_mutex.
 */
static void blkdev_trig_check(struct work_struct *work)
{
	struct blkdev_trig_led *led;
	struct blkdev_trig_bdev *btb;
	unsigned long index, delay, now, led_check, led_delay;
	bool blinked;

	if (!mutex_trylock(&blkdev_trig_mutex)) {
		delay = msecs_to_jiffies(BLKDEV_TRIG_CHECK_RETRY);
		goto exit_reschedule;
	}

	now = jiffies;
	delay = ULONG_MAX;

	hlist_for_each_entry (led, &blkdev_trig_all_leds, all_leds_node) {

		led_check = led->last_checked + led->check_jiffies;

		if (time_before_eq(led_check, now)) {

			blinked = false;

			xa_for_each (&led->linked_btbs, index, btb) {

				if (btb->last_checked != now)
					blkdev_trig_update_btb(btb, now);
				if (!blinked)
					blinked = blkdev_trig_blink(led, btb);
			}

			led->last_checked = now;
			led_delay = led->check_jiffies;

		} else {
			led_delay = led_check - now;
		}

		if (led_delay < delay)
			delay = led_delay;
	}

	mutex_unlock(&blkdev_trig_mutex);

exit_reschedule:
	WARN_ON_ONCE(delay == ULONG_MAX);
	WARN_ON_ONCE(!schedule_delayed_work(&blkdev_trig_work, delay));
}

/**
 * blkdev_trig_blink() - Blink an LED, if the correct type of activity has
 *			 occurred on the block device.
 * @led:	The LED
 * @btb:	The block device
 *
 * Context:	Process context.  Caller must hold @blkdev_trig_mutex.
 * Return:	``true`` if the LED is blinked, ``false`` if not.
 */
static bool blkdev_trig_blink(const struct blkdev_trig_led *led,
			      const struct blkdev_trig_bdev *btb)
{
	unsigned long delay_on, delay_off;
	enum stat_group i;
	unsigned long mode, mask;

	mode = READ_ONCE(led->mode);

	for (i = STAT_READ, mask = 1; i <= STAT_FLUSH; ++i, mask <<= 1) {

		if (!(mode & mask))
			continue;

		if (time_before_eq(btb->last_activity[i], led->last_checked))
			continue;

		delay_on = READ_ONCE(led->blink_msec);
		delay_off = 1;	/* 0 leaves LED turned on */

		led_blink_set_oneshot(led->led_cdev, &delay_on, &delay_off, 0);
		return true;
	}

	return false;
}

/**
 * blink_time_show() - ``blink_time`` device attribute show function.
 * @dev:	The LED device
 * @attr:	The ``blink_time`` attribute (@dev_attr_blink_time)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @blink_msec to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t blink_time_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	const struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);

	return sprintf(buf, "%u\n", READ_ONCE(led->blink_msec));
}

/**
 * blink_time_store() - ``blink_time`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``blink_time`` attribute (@dev_attr_blink_time)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets the LED's @blink_msec (the duration in milliseconds of one blink).
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t blink_time_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);
	unsigned int value;
	int err;

	err = kstrtouint(buf, 0, &value);
	if (err)
		return err;

	if (value < BLKDEV_TRIG_BLINK_MIN || value > BLKDEV_TRIG_BLINK_MAX)
		return -ERANGE;

	WRITE_ONCE(led->blink_msec, value);
	return count;
}

/**
 * check_interval_show() - ``check_interval`` device attribute show function.
 * @dev:	The LED device
 * @attr:	The ``check_interval`` attribute (@dev_attr_check_interval)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @check_jiffies (converted to
 * milliseconds) to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t check_interval_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);

	return sprintf(buf, "%u\n",
		       jiffies_to_msecs(READ_ONCE(led->check_jiffies)));
}

/**
 * check_interval_store() - ``check_interval`` device attribute store function
 * @dev:	The LED device
 * @attr:	The ``check_interval`` attribute (@dev_attr_check_interval)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets the LED's @check_jiffies (after converting from milliseconds).
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t check_interval_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct blkdev_trig_led *led = led_trigger_get_drvdata(dev);
	unsigned int value;
	int err;

	err = kstrtouint(buf, 0, &value);
	if (err)
		return err;

	if (value < BLKDEV_TRIG_CHECK_MIN || value > BLKDEV_TRIG_CHECK_MAX)
		return -ERANGE;

	WRITE_ONCE(led->check_jiffies, msecs_to_jiffies(value));

	return count;
}

/**
 * blkdev_trig_mode_show() - Helper for boolean attribute show functions.
 * @led:	The LED
 * @buf:	Output buffer
 * @mask:	Which bit to show (@BLKDEV_TRIG_READ, etc.)
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static int blkdev_trig_mode_show(const struct blkdev_trig_led *led, char *buf,
				 unsigned long mask)
{
	return sprintf(buf, READ_ONCE(led->mode) & mask ? "Y\n" : "N\n");
}

/**
 * blkdev_trig_mode_store() - Helper for boolean attribute store functions.
 * @led:	The LED
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 * @mask:	Which bit to set (@BLKDEV_TRIG_READ, etc.)
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static int blkdev_trig_mode_store(struct blkdev_trig_led *led,
				  const char *buf, size_t count,
				  enum stat_group bit)
{
	bool set;
	int err;

	err = kstrtobool(buf, &set);
	if (err)
		return err;

	if (set)
		set_bit(bit, &led->mode);
	else
		clear_bit(bit, &led->mode);

	return count;
}

/**
 * blink_on_read_show() - ``blink_on_read`` device attribute show function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_read`` attribute (@dev_attr_blink_on_read)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @BLKDEV_TRIG_READ @mode bit to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t blink_on_read_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return blkdev_trig_mode_show(led_trigger_get_drvdata(dev), buf,
				     BLKDEV_TRIG_READ);
}

/**
 * blink_on_read_store() - ``blink_on_read`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_read`` attribute (@dev_attr_blink_on_read)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets or clears the LED's @BLKDEV_TRIG_READ @mode bit.
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t blink_on_read_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return blkdev_trig_mode_store(led_trigger_get_drvdata(dev), buf, count,
				      STAT_READ);
}

/**
 * blink_on_write_show() - ``blink_on_write`` device attribute show function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_write`` attribute (@dev_attr_blink_on_write)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @BLKDEV_TRIG_WRITE @mode bit to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t blink_on_write_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return blkdev_trig_mode_show(led_trigger_get_drvdata(dev), buf,
				     BLKDEV_TRIG_WRITE);
}

/**
 * blink_on_write_store() - ``blink_on_write`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_write`` attribute (@dev_attr_blink_on_write)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets or clears the LED's @BLKDEV_TRIG_WRITE @mode bit.
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t blink_on_write_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return blkdev_trig_mode_store(led_trigger_get_drvdata(dev), buf, count,
				      STAT_WRITE);
}

/**
 * blink_on_flush_show() - ``blink_on_flush`` device attribute show function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_flush`` attribute (@dev_attr_blink_on_flush)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @BLKDEV_TRIG_FLUSH @mode bit to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t blink_on_flush_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return blkdev_trig_mode_show(led_trigger_get_drvdata(dev), buf,
				     BLKDEV_TRIG_FLUSH);
}

/**
 * blink_on_flush_store() - ``blink_on_flush`` device attribute store function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_flush`` attribute (@dev_attr_blink_on_flush)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets or clears the LED's @BLKDEV_TRIG_FLUSH @mode bit.
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t blink_on_flush_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return blkdev_trig_mode_store(led_trigger_get_drvdata(dev), buf, count,
				      STAT_FLUSH);
}

/**
 * blink_on_discard_show() - ``blink_on_discard`` device attribute show
 *			     function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_discard`` attribute (@dev_attr_blink_on_discard)
 * @buf:	Output buffer
 *
 * Writes the current value of the LED's @BLKDEV_TRIG_DISCARD @mode bit to @buf.
 *
 * Context:	Process context.
 * Return:	The number of characters written to @buf.
 */
static ssize_t blink_on_discard_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return blkdev_trig_mode_show(led_trigger_get_drvdata(dev), buf,
				     BLKDEV_TRIG_DISCARD);
}

/**
 * blink_on_discard_store() - ``blink_on_discard`` device attribute store
 *			      function.
 * @dev:	The LED device
 * @attr:	The ``blink_on_discard`` attribute (@dev_attr_blink_on_discard)
 * @buf:	The new value (as written to the sysfs attribute)
 * @count:	The number of characters in @buf
 *
 * Sets the LED's @BLKDEV_TRIG_DISCARD @mode bit.
 *
 * Context:	Process context.
 * Return:	@count on success, ``-errno`` on error.
 */
static ssize_t blink_on_discard_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	return blkdev_trig_mode_store(led_trigger_get_drvdata(dev), buf, count,
				      STAT_DISCARD);
}

/* Device attributes */
static DEVICE_ATTR_WO(link_device);
static DEVICE_ATTR_WO(unlink_device);
static DEVICE_ATTR_RW(blink_time);
static DEVICE_ATTR_RW(check_interval);
static DEVICE_ATTR_RW(blink_on_read);
static DEVICE_ATTR_RW(blink_on_write);
static DEVICE_ATTR_RW(blink_on_flush);
static DEVICE_ATTR_RW(blink_on_discard);

/* Device attributes in LED directory (/sys/class/leds/<led>/...) */
static struct attribute *blkdev_trig_attrs[] = {
	&dev_attr_link_device.attr,
	&dev_attr_unlink_device.attr,
	&dev_attr_blink_time.attr,
	&dev_attr_check_interval.attr,
	&dev_attr_blink_on_read.attr,
	&dev_attr_blink_on_write.attr,
	&dev_attr_blink_on_flush.attr,
	&dev_attr_blink_on_discard.attr,
	NULL
};

/* Unnamed attribute group == no subdirectory */
static const struct attribute_group blkdev_trig_attr_group = {
	.attrs	= blkdev_trig_attrs,
};

/* Attribute groups for the trigger */
static const struct attribute_group *blkdev_trig_attr_groups[] = {
	&blkdev_trig_attr_group,   /* /sys/class/leds/<led>/... */
	&blkdev_trig_linked_devs,  /* /sys/class/leds/<led>/linked_devices/ */
	NULL
};

/* Trigger registration data */
static struct led_trigger blkdev_trig_trigger = {
	.name		= "blkdev",
	.activate	= blkdev_trig_activate,
	.deactivate	= blkdev_trig_deactivate,
	.groups		= blkdev_trig_attr_groups,
};

/**
 * blkdev_trig_init() - Block device LED trigger initialization.
 *
 * Registers the LED trigger.
 *
 * Return:	0 on success, ``-errno`` on failure.
 */
static int __init blkdev_trig_init(void)
{
	return led_trigger_register(&blkdev_trig_trigger);
}
module_init(blkdev_trig_init);

/**
 * blkdev_trig_exit() - Block device LED trigger module exit.
 *
 * Unregisters the LED trigger.
 */
static void __exit blkdev_trig_exit(void)
{
	led_trigger_unregister(&blkdev_trig_trigger);
}
module_exit(blkdev_trig_exit);

MODULE_DESCRIPTION("Block device LED trigger");
MODULE_AUTHOR("Ian Pilcher <arequipeno@gmail.com>");
MODULE_LICENSE("GPL v2");
