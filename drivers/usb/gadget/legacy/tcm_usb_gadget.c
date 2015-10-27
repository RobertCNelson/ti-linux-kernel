/* Target based USB-Gadget
 *
 * UAS protocol handling, target callbacks, configfs handling,
 * BBB (USB Mass Storage Class Bulk-Only (BBB) and Transport protocol handling.
 *
 * Author: Sebastian Andrzej Siewior <bigeasy at linutronix dot de>
 * License: GPLv2 as published by FSF.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/configfs.h>
#include <linux/ctype.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/usb/storage.h>
#include <scsi/scsi_tcq.h>
#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <asm/unaligned.h>

USB_GADGET_COMPOSITE_OPTIONS();

/* #include to be removed when new function registration interface is used  */
#define USBF_TCM_INCLUDED
#include "../function/f_tcm.c"

#define UAS_VENDOR_ID	0x0525	/* NetChip */
#define UAS_PRODUCT_ID	0xa4a5	/* Linux-USB File-backed Storage Gadget */

static struct usb_device_descriptor usbg_device_desc = {
	.bLength =		sizeof(usbg_device_desc),
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		USB_CLASS_PER_INTERFACE,
	.idVendor =		cpu_to_le16(UAS_VENDOR_ID),
	.idProduct =		cpu_to_le16(UAS_PRODUCT_ID),
	.bNumConfigurations =   1,
};

#define USB_G_STR_CONFIG USB_GADGET_FIRST_AVAIL_IDX

static struct usb_string	usbg_us_strings[] = {
	[USB_GADGET_MANUFACTURER_IDX].s	= "Target Manufactor",
	[USB_GADGET_PRODUCT_IDX].s	= "Target Product",
	[USB_GADGET_SERIAL_IDX].s	= "000000000001",
	[USB_G_STR_CONFIG].s		= "default config",
	{ },
};

static struct usb_gadget_strings usbg_stringtab = {
	.language = 0x0409,
	.strings = usbg_us_strings,
};

static struct usb_gadget_strings *usbg_strings[] = {
	&usbg_stringtab,
	NULL,
};

static int guas_unbind(struct usb_composite_dev *cdev)
{
	return 0;
}

static struct usb_configuration usbg_config_driver = {
	.label                  = "Linux Target",
	.bConfigurationValue    = 1,
	.bmAttributes           = USB_CONFIG_ATT_SELFPOWER,
};

static void give_back_ep(struct usb_ep **pep)
{
	struct usb_ep *ep = *pep;
	if (!ep)
		return;
	ep->driver_data = NULL;
}

static int tcm_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_uas		*fu = to_f_uas(f);
	struct usb_gadget	*gadget = c->cdev->gadget;
	struct usb_ep		*ep;
	int			iface;
	int			ret;

	iface = usb_interface_id(c, f);
	if (iface < 0)
		return iface;

	bot_intf_desc.bInterfaceNumber = iface;
	uasp_intf_desc.bInterfaceNumber = iface;
	fu->iface = iface;
	ep = usb_ep_autoconfig_ss(gadget, &uasp_ss_bi_desc,
			&uasp_bi_ep_comp_desc);
	if (!ep)
		goto ep_fail;
	fu->ep_in = ep;

	ep = usb_ep_autoconfig_ss(gadget, &uasp_ss_bo_desc,
			&uasp_bo_ep_comp_desc);
	if (!ep)
		goto ep_fail;
	fu->ep_out = ep;

	ep = usb_ep_autoconfig_ss(gadget, &uasp_ss_status_desc,
			&uasp_status_in_ep_comp_desc);
	if (!ep)
		goto ep_fail;
	fu->ep_status = ep;

	ep = usb_ep_autoconfig_ss(gadget, &uasp_ss_cmd_desc,
			&uasp_cmd_comp_desc);
	if (!ep)
		goto ep_fail;
	fu->ep_cmd = ep;

	/* Assume endpoint addresses are the same for both speeds */
	uasp_bi_desc.bEndpointAddress =	uasp_ss_bi_desc.bEndpointAddress;
	uasp_bo_desc.bEndpointAddress = uasp_ss_bo_desc.bEndpointAddress;
	uasp_status_desc.bEndpointAddress =
		uasp_ss_status_desc.bEndpointAddress;
	uasp_cmd_desc.bEndpointAddress = uasp_ss_cmd_desc.bEndpointAddress;

	uasp_fs_bi_desc.bEndpointAddress = uasp_ss_bi_desc.bEndpointAddress;
	uasp_fs_bo_desc.bEndpointAddress = uasp_ss_bo_desc.bEndpointAddress;
	uasp_fs_status_desc.bEndpointAddress =
		uasp_ss_status_desc.bEndpointAddress;
	uasp_fs_cmd_desc.bEndpointAddress = uasp_ss_cmd_desc.bEndpointAddress;

	ret = usb_assign_descriptors(f, uasp_fs_function_desc,
			uasp_hs_function_desc, uasp_ss_function_desc);
	if (ret)
		goto ep_fail;

	return 0;
ep_fail:
	pr_err("Can't claim all required eps\n");
	return -ENOTSUPP;
}

static void tcm_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_uas *fu = to_f_uas(f);

	usb_free_all_descriptors(f);
	kfree(fu);
}

struct guas_setup_wq {
	struct work_struct work;
	struct f_uas *fu;
	unsigned int alt;
};

static void tcm_delayed_set_alt(struct work_struct *wq)
{
	struct guas_setup_wq *work = container_of(wq, struct guas_setup_wq,
			work);
	struct f_uas *fu = work->fu;
	int alt = work->alt;

	kfree(work);

	if (fu->flags & USBG_IS_BOT)
		bot_cleanup_old_alt(fu);
	if (fu->flags & USBG_IS_UAS)
		uasp_cleanup_old_alt(fu);

	if (alt == USB_G_ALT_INT_BBB)
		bot_set_alt(fu);
	else if (alt == USB_G_ALT_INT_UAS)
		uasp_set_alt(fu);
	usb_composite_setup_continue(fu->function.config->cdev);
}

static int tcm_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_uas *fu = to_f_uas(f);

	if ((alt == USB_G_ALT_INT_BBB) || (alt == USB_G_ALT_INT_UAS)) {
		struct guas_setup_wq *work;

		work = kmalloc(sizeof(*work), GFP_ATOMIC);
		if (!work)
			return -ENOMEM;
		INIT_WORK(&work->work, tcm_delayed_set_alt);
		work->fu = fu;
		work->alt = alt;
		schedule_work(&work->work);
		return USB_GADGET_DELAYED_STATUS;
	}
	return -EOPNOTSUPP;
}

static void tcm_disable(struct usb_function *f)
{
	struct f_uas *fu = to_f_uas(f);

	if (fu->flags & USBG_IS_UAS)
		uasp_cleanup_old_alt(fu);
	else if (fu->flags & USBG_IS_BOT)
		bot_cleanup_old_alt(fu);
	fu->flags = 0;
}

static int tcm_setup(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct f_uas *fu = to_f_uas(f);

	if (!(fu->flags & USBG_IS_BOT))
		return -EOPNOTSUPP;

	return usbg_bot_setup(f, ctrl);
}

static int tcm_bind_config(struct usb_configuration *c)
{
	struct f_uas *fu;
	int ret;

	fu = kzalloc(sizeof(*fu), GFP_KERNEL);
	if (!fu)
		return -ENOMEM;
	fu->function.name = "Target Function";
	fu->function.bind = tcm_bind;
	fu->function.unbind = tcm_unbind;
	fu->function.set_alt = tcm_set_alt;
	fu->function.setup = tcm_setup;
	fu->function.disable = tcm_disable;
	fu->function.strings = tcm_strings;
	fu->tpg = the_only_tpg_I_currently_have;

	bot_intf_desc.iInterface = tcm_us_strings[USB_G_STR_INT_BBB].id;
	uasp_intf_desc.iInterface = tcm_us_strings[USB_G_STR_INT_UAS].id;

	ret = usb_add_function(c, &fu->function);
	if (ret)
		goto err;

	return 0;
err:
	kfree(fu);
	return ret;
}

static int usb_target_bind(struct usb_composite_dev *cdev)
{
	int ret;

	ret = usb_string_ids_tab(cdev, usbg_us_strings);
	if (ret)
		return ret;

	usbg_device_desc.iManufacturer =
		usbg_us_strings[USB_GADGET_MANUFACTURER_IDX].id;
	usbg_device_desc.iProduct = usbg_us_strings[USB_GADGET_PRODUCT_IDX].id;
	usbg_device_desc.iSerialNumber =
		usbg_us_strings[USB_GADGET_SERIAL_IDX].id;
	usbg_config_driver.iConfiguration =
		usbg_us_strings[USB_G_STR_CONFIG].id;

	ret = usb_add_config(cdev, &usbg_config_driver,
			tcm_bind_config);
	if (ret)
		return ret;
	usb_composite_overwrite_options(cdev, &coverwrite);
	return 0;
}

static struct usb_composite_driver usbg_driver = {
	.name           = "g_target",
	.dev            = &usbg_device_desc,
	.strings        = usbg_strings,
	.max_speed      = USB_SPEED_SUPER,
	.bind		= usb_target_bind,
	.unbind         = guas_unbind,
};

static int usbg_attach(struct usbg_tpg *tpg)
{
	return usb_composite_probe(&usbg_driver);
}

static void usbg_detach(struct usbg_tpg *tpg)
{
	usb_composite_unregister(&usbg_driver);
}

static int __init usb_target_gadget_init(void)
{
	return target_register_template(&usbg_ops);
}
module_init(usb_target_gadget_init);

static void __exit usb_target_gadget_exit(void)
{
	target_unregister_template(&usbg_ops);
}
module_exit(usb_target_gadget_exit);

MODULE_AUTHOR("Sebastian Andrzej Siewior <bigeasy@linutronix.de>");
MODULE_DESCRIPTION("usb-gadget fabric");
MODULE_LICENSE("GPL v2");
