/*
 *  HID support for Linux
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2007-2008 Oliver Neukum
 *  Copyright (c) 2006-2012 Jiri Kosina
 *  Copyright (c) 2012 Henrik Rydberg
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>
#include <asm/byteorder.h>

#include <linux/hid.h>

struct hid_generic_device {
	struct hid_device *hdev;	/* hid_device we're attached to */
	struct input_dev *device;

	unsigned wheel_multiplier;
	unsigned hwheel_multiplier;

	/* Report ID and Field index for each Resolution Multiplier */
	unsigned report_id[2];
	unsigned field_index[2];
};

static struct hid_driver hid_generic;

static int __check_hid_generic(struct device_driver *drv, void *data)
{
	struct hid_driver *hdrv = to_hid_driver(drv);
	struct hid_device *hdev = data;

	if (hdrv == &hid_generic)
		return 0;

	return hid_match_device(hdev, hdrv) != NULL;
}

static bool hid_generic_match(struct hid_device *hdev,
			      bool ignore_special_driver)
{
	if (ignore_special_driver)
		return true;

	if (hdev->quirks & HID_QUIRK_HAVE_SPECIAL_DRIVER)
		return false;

	/*
	 * If any other driver wants the device, leave the device to this other
	 * driver.
	 */
	if (bus_for_each_drv(&hid_bus_type, NULL, hdev, __check_hid_generic))
		return false;

	return true;
}

static bool usage_in_collection(struct hid_device *hdev,
				unsigned usage_id,
				unsigned collection)
{
	struct hid_report_enum *report_enum;
	struct hid_report *rep;
	struct hid_field *field;
	struct hid_usage *usage;
	int i;

	report_enum = &hdev->report_enum[HID_INPUT_REPORT];
	list_for_each_entry(rep, &report_enum->report_list, list) {
		for (i = 0; i < rep->maxfield; i++) {
			field = rep->field[i];
			usage = field->usage;

			if (usage->hid == usage_id &&
			    usage->collection_index == collection)
				return true;
		}
	}

	return false;
}

static void handle_resolution_multiplier(struct hid_device *hdev,
					 struct hid_report *rep,
					 struct hid_field *field,
					 struct hid_usage *usage)
{
	struct hid_generic_device *dev = hid_get_drvdata(hdev);
	unsigned multiplier = 1;
	unsigned which;
	bool w, h;

	multiplier = field->physical_maximum;

	/*
	 * the multiplier only applies to usages in the
	 * same collection
	 */
	w = usage_in_collection(hdev, HID_GD_WHEEL, usage->collection_index);
	h = usage_in_collection(hdev, HID_CP_AC_PAN, usage->collection_index);

	if (!w && !h)
		return;
	if (w)
		dev->wheel_multiplier = multiplier;
	if (h)
		dev->hwheel_multiplier = multiplier;

	/*
	 * The order isn't guaranteed, but we only care about the
	 * field, not what it is mapped to.
	 */
	if (dev->report_id[0] == -1) {
		which = 0;
	} else if (dev->report_id[1] == -1) {
		which = 1;
	} else {
		/* Firmware bug, we somehow have three resolution
		 * multipliers and they're in the same collection as the
		 * wheel/hwheel.
		 */
		hid_err(hdev, "invalid Resolution Multipliers\n");
		dev->wheel_multiplier = 1;
		dev->hwheel_multiplier = 1;
		return;
	}

	dev->report_id[which] = rep->id;
	dev->field_index[which] = field->index;
}

static void hid_generic_fetch_resolution_multiplier(struct hid_device *hdev)
{
	struct hid_report_enum *report_enum;
	struct hid_report *rep;
	struct hid_field *field;
	struct hid_usage *usage;
	int i;

	report_enum = &hdev->report_enum[HID_FEATURE_REPORT];
	list_for_each_entry(rep, &report_enum->report_list, list) {
		for (i = 0; i < rep->maxfield; i++) {

			field = rep->field[i];
			usage = field->usage;

			if (usage->hid == HID_GD_RESOLUTION_MULTIPLIER)
				handle_resolution_multiplier(hdev, rep,
							     field, usage);
		}
	}
}

static void hid_generic_set_resolution_multiplier(struct hid_device *hdev)
{
	struct hid_generic_device *dev = hid_get_drvdata(hdev);
	struct hid_report_enum *report_enum;
	struct hid_report *rep;
	struct hid_field *field;
	struct hid_usage *usage;
	unsigned index;
	int i;

	if (dev->wheel_multiplier == 1 && dev->hwheel_multiplier == 1)
		return;

	/* Microsoft always sets this to the logical maximum, so let's copy
	 * that behavior. On the mice checked so far, logical min/max is
	 * always 0/1 anyway.
	 */
	report_enum = &hdev->report_enum[HID_FEATURE_REPORT];
	for (i = 0; i < 2; i++) {
		if (dev->report_id[i] == -1)
			break;

		rep = report_enum->report_id_hash[dev->report_id[i]];
		field = rep->field[dev->field_index[i]];

		usage = field->usage;
		index = usage->usage_index;
		field->value[index] = field->logical_maximum;
		hid_hw_request(hdev, rep, HID_REQ_SET_REPORT);
	}
}

static int hid_generic_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	int ret;

	struct hid_generic_device *dev;

	dev = devm_kzalloc(&hdev->dev, sizeof(struct hid_generic_device), GFP_KERNEL);
	if (!dev) {
		dev_err(&hdev->dev, "cannot allocate hid-generic data\n");
		return -ENOMEM;
	}

	dev->hdev = hdev;
	dev->wheel_multiplier = 1;
	dev->hwheel_multiplier = 1;

	dev->report_id[0] = -1;
	dev->report_id[1] = -1;
	dev->field_index[0] = -1;
	dev->field_index[1] = -1;
	hid_set_drvdata(hdev, dev);

	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	hid_generic_fetch_resolution_multiplier(hdev);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	hid_generic_set_resolution_multiplier(hdev);

	return ret;
}

static int hid_generic_event(struct hid_device *hid, struct hid_field *field,
			     struct hid_usage *usage, __s32 value)
{
	struct hid_generic_device *dev = hid_get_drvdata(hid);
	struct input_dev *device = dev->device;

	switch(field->usage->hid) {
	case HID_GD_WHEEL:
		/* FIXME: hook up hid_scroll_counter_handle_scroll */
		input_report_rel(device, REL_WHEEL_HI_RES,
				 value * dev->wheel_multiplier);
		input_report_rel(device, REL_WHEEL, value);
		input_sync(device);
		break;
	case HID_CP_AC_PAN:
		/* FIXME: hook up hid_scroll_counter_handle_scroll */
		input_report_rel(device, REL_HWHEEL_HI_RES,
				 value * dev->hwheel_multiplier);
		input_report_rel(device, REL_HWHEEL, value);
		input_sync(device);
		break;
	}

	return 1;
}

static int hid_generic_input_configured(struct hid_device *hdev,
					struct hid_input *hidinput)
{
	struct hid_generic_device *dev = hid_get_drvdata(hdev);
	struct input_dev *input = hidinput->input;

	dev->device = input;

	if (dev->wheel_multiplier > 1)
		input_set_capability(input, EV_REL, REL_WHEEL_HI_RES);
	if (dev->hwheel_multiplier > 1)
		input_set_capability(input, EV_REL, REL_HWHEEL_HI_RES);

	return 0;
}

#ifdef CONFIG_PM
static int hid_generic_reset_resume(struct hid_device *hdev)
{
	hid_generic_set_resolution_multiplier(hdev);
	return 0;
}
#endif

static const struct hid_device_id hid_table[] = {
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_ANY, HID_ANY_ID, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, hid_table);

static const struct hid_usage_id hid_generic_grabbed_usages[] = {
	{ HID_GD_WHEEL, EV_REL, REL_WHEEL },
	{ HID_CP_AC_PAN, EV_REL, REL_HWHEEL },
	{ HID_ANY_ID - 1, HID_ANY_ID - 1, HID_ANY_ID - 1}
};

static struct hid_driver hid_generic = {
	.name = "hid-generic",
	.id_table = hid_table,
	.match = hid_generic_match,
	.probe = hid_generic_probe,
	.usage_table = hid_generic_grabbed_usages,
	.event = hid_generic_event,
	.input_configured = hid_generic_input_configured,
#ifdef CONFIG_PM
	.reset_resume = hid_generic_reset_resume,
#endif
};
module_hid_driver(hid_generic);

MODULE_AUTHOR("Henrik Rydberg");
MODULE_DESCRIPTION("HID generic driver");
MODULE_LICENSE("GPL");
