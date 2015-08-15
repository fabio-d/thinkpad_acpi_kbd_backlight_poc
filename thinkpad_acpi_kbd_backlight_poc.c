/*
 *  thinkpad_acpi_kbd_backlight_poc.c - Proof of concept keyboard backlight
 *  support code for thinkpad_acpi (tpacpi::kbd_backlight)
 *
 *  Based on thinkpad_acpi.c - ThinkPad ACPI Extras
 *
 *  Copyright (C) 2004-2005 Borislav Deianov <borislav@users.sf.net>
 *  Copyright (C) 2006-2009 Henrique de Moraes Holschuh <hmh@hmh.eng.br>
 *
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/workqueue.h>

/* ----- BEGIN CUT HERE ----------------------- Imported from thinkpad_acpi.c */

static struct workqueue_struct *tpacpi_wq;

#define TPACPI_ACPI_EC_HID				"PNP0C09"
#define TPACPI_MAX_ACPI_ARGS				3

#define vdbg_printk(a_dbg_level, format, arg...)	printk(format, ##arg)

#define dbg_printk(a_dbg_level, format, arg...)				\
					printk(KERN_DEBUG		\
					       pr_fmt("%s: " format),	\
					       __func__, ##arg)

static acpi_handle ec_handle;

#define TPACPI_HANDLE(object, parent, paths...)			\
	static acpi_handle  object##_handle;			\
	static const acpi_handle * const object##_parent __initconst =	\
						&parent##_handle; \
	static char *object##_paths[] __initdata = { paths }

TPACPI_HANDLE(hkey, ec, "\\_SB.HKEY",	/* 600e/x, 770e, 770x */
	   "^HKEY",		/* R30, R31 */
	   "HKEY",		/* all others */
	   );			/* 570 */

static int acpi_evalf(acpi_handle handle,
		      int *res, char *method, char *fmt, ...)
{
	char *fmt0 = fmt;
	struct acpi_object_list params;
	union acpi_object in_objs[TPACPI_MAX_ACPI_ARGS];
	struct acpi_buffer result, *resultp;
	union acpi_object out_obj;
	acpi_status status;
	va_list ap;
	char res_type;
	int success;
	int quiet;

	if (!*fmt) {
		pr_err("acpi_evalf() called with empty format\n");
		return 0;
	}

	if (*fmt == 'q') {
		quiet = 1;
		fmt++;
	} else
		quiet = 0;

	res_type = *(fmt++);

	params.count = 0;
	params.pointer = &in_objs[0];

	va_start(ap, fmt);
	while (*fmt) {
		char c = *(fmt++);
		switch (c) {
		case 'd':	/* int */
			in_objs[params.count].integer.value = va_arg(ap, int);
			in_objs[params.count++].type = ACPI_TYPE_INTEGER;
			break;
			/* add more types as needed */
		default:
			pr_err("acpi_evalf() called "
			       "with invalid format character '%c'\n", c);
			va_end(ap);
			return 0;
		}
	}
	va_end(ap);

	if (res_type != 'v') {
		result.length = sizeof(out_obj);
		result.pointer = &out_obj;
		resultp = &result;
	} else
		resultp = NULL;

	status = acpi_evaluate_object(handle, method, &params, resultp);

	switch (res_type) {
	case 'd':		/* int */
		success = (status == AE_OK &&
			   out_obj.type == ACPI_TYPE_INTEGER);
		if (success && res)
			*res = out_obj.integer.value;
		break;
	case 'v':		/* void */
		success = status == AE_OK;
		break;
		/* add more types as needed */
	default:
		pr_err("acpi_evalf() called "
		       "with invalid format character '%c'\n", res_type);
		return 0;
	}

	if (!success && !quiet)
		pr_err("acpi_evalf(%s, %s, ...) failed: %s\n",
		       method, fmt0, acpi_format_exception(status));

	return success;
}

#define TPACPI_ACPIHANDLE_INIT(object) \
	drv_acpi_handle_init(#object, &object##_handle, *object##_parent, \
		object##_paths, ARRAY_SIZE(object##_paths))

static void __init drv_acpi_handle_init(const char *name,
			   acpi_handle *handle, const acpi_handle parent,
			   char **paths, const int num_paths)
{
	int i;
	acpi_status status;

	vdbg_printk(TPACPI_DBG_INIT, "trying to locate ACPI handle for %s\n",
		name);

	for (i = 0; i < num_paths; i++) {
		status = acpi_get_handle(parent, paths[i], handle);
		if (ACPI_SUCCESS(status)) {
			dbg_printk(TPACPI_DBG_INIT,
				   "Found ACPI handle %s for %s\n",
				   paths[i], name);
			return;
		}
	}

	vdbg_printk(TPACPI_DBG_INIT, "ACPI handle for %s not found\n",
		    name);
	*handle = NULL;
}

static acpi_status __init tpacpi_acpi_handle_locate_callback(acpi_handle handle,
			u32 level, void *context, void **return_value)
{
	struct acpi_device *dev;
	if (!strcmp(context, "video")) {
		if (acpi_bus_get_device(handle, &dev))
			return AE_OK;
		if (strcmp(ACPI_VIDEO_HID, acpi_device_hid(dev)))
			return AE_OK;
	}

	*(acpi_handle *)return_value = handle;

	return AE_CTRL_TERMINATE;
}

static void __init tpacpi_acpi_handle_locate(const char *name,
		const char *hid,
		acpi_handle *handle)
{
	acpi_status status;
	acpi_handle device_found;

	BUG_ON(!name || !handle);
	vdbg_printk(TPACPI_DBG_INIT,
			"trying to locate ACPI handle for %s, using HID %s\n",
			name, hid ? hid : "NULL");

	memset(&device_found, 0, sizeof(device_found));
	status = acpi_get_devices(hid, tpacpi_acpi_handle_locate_callback,
				  (void *)name, &device_found);

	*handle = NULL;

	if (ACPI_SUCCESS(status)) {
		*handle = device_found;
		dbg_printk(TPACPI_DBG_INIT,
			   "Found ACPI handle for %s\n", name);
	} else {
		vdbg_printk(TPACPI_DBG_INIT,
			    "Could not locate an ACPI handle for %s: %s\n",
			    name, acpi_format_exception(status));
	}
}

/* -------- END CUT HERE ---------------------- Imported from thinkpad_acpi.c */

static acpi_handle mlcg_handle, mlcs_handle;

struct tpacpikbdpoc_backlight_classdev {
	struct led_classdev led_classdev;
	struct work_struct work;
	int next_value;
};

static int tpacpikbdpoc_call_mlcg(void)
{
	int res;

	if (!acpi_evalf(mlcg_handle, &res, NULL, "dd", 0 /* dummy */)) {
		WARN_ONCE(1, "failed to obtain current keyboard backlight "
			     "brightness\n");
		res = 0;
	}

	/* Expect 0x50200, 0x50201 or 0x50202 */
	WARN_ONCE(res < 0x50200 || res > 0x50202,
		  "MLCG returned unexpected 0x%x value\n", res);

	return res;
}

static void backlight_worker(struct work_struct *work)
{
	struct tpacpikbdpoc_backlight_classdev *data =
		container_of(work,
			     struct tpacpikbdpoc_backlight_classdev,
			     work);

	int res = acpi_evalf(mlcs_handle, NULL, NULL, "vd", data->next_value);

	WARN_ONCE(res == 0, "failed to set keyboard backlight brightness\n");
}

static void backlight_sysfs_set(struct led_classdev *led_cdev,
				enum led_brightness brightness)
{
	struct tpacpikbdpoc_backlight_classdev *data =
		container_of(led_cdev,
			     struct tpacpikbdpoc_backlight_classdev,
			     led_classdev);

	data->next_value = brightness;
	queue_work(tpacpi_wq, &data->work);
}

static enum led_brightness backlight_sysfs_get(struct led_classdev *led_cdev)
{
	return tpacpikbdpoc_call_mlcg() & 3;
}

static struct tpacpikbdpoc_backlight_classdev backlight_led = {
	.led_classdev = {
		.name		= "tpacpi::kbd_backlight",
		.max_brightness	= 2,
		.brightness_set	= &backlight_sysfs_set,
		.brightness_get	= &backlight_sysfs_get,
	}
};

static int __init tpacpikbdpoc_init(void)
{
	int rc;

	dbg_printk(TPACPI_DBG_INIT, "loading\n");

	if (acpi_disabled)
		return -ENODEV;

	/* The EC handler is required */
	tpacpi_acpi_handle_locate("ec", TPACPI_ACPI_EC_HID, &ec_handle);
	if (!ec_handle)
		return -ENODEV;

	/* The HKEY handler is required for backlight control */
	TPACPI_ACPIHANDLE_INIT(hkey);
	if (!hkey_handle)
		return -ENODEV;

	/* Create workqueue for pending commands */
	tpacpi_wq = create_singlethread_workqueue("ktpacpikbdpocd");
	if (!tpacpi_wq) {
		dbg_printk(TPACPI_DBG_INIT, "cannot create workqueue\n");
		return -ENOMEM;
	}

	INIT_WORK(&backlight_led.work, backlight_worker);

	/* Locate MLCG */
	if (!ACPI_SUCCESS(acpi_get_handle(hkey_handle, "MLCG", &mlcg_handle))) {
		dbg_printk(TPACPI_DBG_INIT, "cannot locate MLCG handle\n");
		destroy_workqueue(tpacpi_wq);
		return -ENODEV;
	}

	/* We have found MLCG, let's do a test run */
	dbg_printk(TPACPI_DBG_INIT, "Found ACPI handle MLCG\n");
	dbg_printk(TPACPI_DBG_INIT, "MLCG initially returns 0x%x\n",
		    tpacpikbdpoc_call_mlcg());

	/* Locate MLCS */
	if (!ACPI_SUCCESS(acpi_get_handle(hkey_handle, "MLCS", &mlcs_handle))) {
		dbg_printk(TPACPI_DBG_INIT, "cannot locate MLCS handle\n");
		destroy_workqueue(tpacpi_wq);
		return -ENODEV;
	}

	dbg_printk(TPACPI_DBG_INIT, "Found ACPI handle MLCS\n");

	rc = led_classdev_register(NULL, &backlight_led.led_classdev);
	if (rc < 0) {
		dbg_printk(TPACPI_DBG_INIT, "cannot register %s led\n",
			    backlight_led.led_classdev.name);
		destroy_workqueue(tpacpi_wq);
		return rc;
	}

	dbg_printk(TPACPI_DBG_INIT, "Both MLCG and MLCS were found, led %s "
	       "initialized successfully\n", backlight_led.led_classdev.name);
	return 0;
}

static void __exit tpacpikbdpoc_exit(void)
{
	led_classdev_unregister(&backlight_led.led_classdev);

	flush_workqueue(tpacpi_wq);
	destroy_workqueue(tpacpi_wq);

	dbg_printk(TPACPI_DBG_INIT, "unloaded\n");
}

module_init(tpacpikbdpoc_init);
module_exit(tpacpikbdpoc_exit);

MODULE_AUTHOR("Fabio D'Urso <fabiodurso@hotmail.it>");
MODULE_DESCRIPTION("Proof-of-concept ThinkPad keyboard backlight control");
MODULE_LICENSE("GPL");
