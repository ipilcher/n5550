.. SPDX-License-Identifier: GPL-2.0

=================================
Block Device (blkdev) LED Trigger
=================================

Available when ``CONFIG_LEDS_TRIGGER_BLKDEV=y`` or
``CONFIG_LEDS_TRIGGER_BLKDEV=m``.

See also:

* ``Documentation/ABI/testing/sysfs-class-led-trigger-blkdev``
* ``Documentation/ABI/stable/sysfs-block`` (``/sys/block/<disk>/linked_leds``)

Overview
========

.. note::
	The examples below use ``<LED>`` to refer to the name of a
	system-specific LED.  If no suitable LED is available on a test
	system (in a virtual machine, for example), it is possible to
	use a userspace LED.  (See ``Documentation/leds/uleds.rst``.)

Verify that the ``blkdev`` LED trigger is available::

	# grep blkdev /sys/class/leds/<LED>/trigger
	... rfkill-none blkdev

(If the previous command produces no output, you may need to load the trigger
module - ``modprobe ledtrig_blkdev``.  If the module is not available, check
the value of ``CONFIG_LEDS_TRIGGER_BLKDEV`` in your kernel configuration.)

Associate the LED with the ``blkdev`` LED trigger::

	# echo blkdev > /sys/class/leds/<LED>/trigger

	# cat /sys/class/leds/<LED>/trigger
	... rfkill-none [blkdev]

Note that several new device attributes are available in the
``/sys/class/leds/<LED>`` directory.

* ``link_device`` and ``unlink_device`` are used to manage the set of block
  devices associated with this LED.  The LED will blink in response to read or
  write activity on its linked devices.

* ``blink_on_read``, ``blink_on_write``, ``blink_on_discard``, and
  ``blink_on_flush`` are boolean values that determine whether the LED will
  blink when a particular type of activity is detected on one of its linked
  block devices.

* ``blink_time`` is the duration (in milliseconds) of each blink of this LED.
  (The minimum value is 10 milliseconds.)

* ``check_interval`` is the frequency (in milliseconds) with which block devices
  linked to this LED will be checked for activity and the LED blinked (if the
  correct type of activity has occurred).

* The ``linked_devices`` directory will contain a symbolic link to every device
  that is associated with this LED.

Link a block device to the LED::

	# echo /dev/sda > /sys/class/leds/<LED>/link_device

	# ls /sys/class/leds/<LED>/linked_devices
	sda

(The value written to ``link_device`` must be the path of the device special
file, such as ``/dev/sda``, that represents the block device - or the path of a
symbolic link to such a device special file.)

Activity on the device should cause the LED to blink.  The duration of each
blink (in milliseconds) can be adjusted by setting
``/sys/class/leds/<LED>/blink_time``.  (But see **check_interval and
blink_time** below.)

Associate a second device with the LED::

	# echo /dev/sdb > /sys/class/leds/<LED>/link_device

	# ls /sys/class/leds/<LED>/linked_devices
	sda  sdb

When a block device is linked to one or more LEDs, the LEDs are linked from
the device's ``linked_leds`` directory::

	# ls /sys/class/block/sd{a,b}/linked_leds
	/sys/class/block/sda/linked_leds:
	<LED>

	/sys/class/block/sdb/linked_leds:
	<LED>

(The ``linked_leds`` directory only exists when the block device is linked to
at least one LED.)

``check_interval`` and ``blink_time``
=====================================

* By default, linked block devices are checked for activity every 100
  milliseconds.  This frequency can be changed for an LED via the
  ``/sys/class/leds/<led>/check_interval`` attribute.  (The minimum value is 25
  milliseconds.)

* All block devices associated with an LED are checked for activity every
  ``check_interval`` milliseconds, and a blink is triggered if the correct type
  of activity (as determined by the LED's ``blink_on_*`` attributes) is
  detected.  The duration of an LED's blink is determined by its ``blink_time``
  attribute.  Thus (when the correct type of activity is detected), the LED will
  be on for ``blink_time`` milliseconds and off for ``check_interval -
  blink_time`` milliseconds.

* The LED subsystem ignores new blink requests for an LED that is already in
  in the process of blinking, so setting a ``blink_time`` greater than or equal
  to ``check_interval`` will cause some blinks to be missed.

* Because of processing times, scheduling latencies, etc., avoiding missed
  blinks actually requires a difference of at least a few milliseconds between
  the ``blink_time`` and ``check_interval``.  The required difference is likely
  to vary from system to system.  As a  reference, a Thecus N5550 NAS requires a
  difference of 7 milliseconds (``check_interval == 100``, ``blink_time ==
  93``).

* The default values (``check_interval == 100``, ``blink_time == 75``) cause the
  LED associated with a continuously active device to blink rapidly.  For a more
  "always on" effect, increase the ``blink_time`` (but not too much; see the
  previous bullet).

Other Notes
===========

* Many (possibly all) types of block devices work with this trigger, including:

  * SCSI (including SATA and USB) hard disk drives and SSDs
  * SCSI (including SATA and USB) optical drives
  * NVMe SSDs
  * SD cards
  * loopback block devices (``/dev/loop*``)
  * device mapper devices, such as LVM logical volumes
  * MD RAID devices
  * zRAM compressed RAM-disks
  * partitions on block devics that support them

* The names of the symbolic links in ``/sys/class/leds/<LED>/linked_devices``
  are **kernel** names, which may not match the paths used for
  ``link_device`` and ``unlink_device``.  This is most likely when a symbolic
  link is used to refer to the device (as is common with logical volumes), but
  it can be true for any device, because nothing prevents the creation of
  device special files with arbitrary names (e.g. ``sudo mknod /foo b 8 0``).

* The ``blkdev`` LED trigger supports many-to-many device/LED associations.
  A device can be associated with multiple LEDs, and an LED can be associated
  with multiple devices.
