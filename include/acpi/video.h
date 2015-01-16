#ifndef __ACPI_VIDEO_H
#define __ACPI_VIDEO_H

#include <linux/errno.h> /* for ENODEV */

struct acpi_video_brightness_flags {
	u8 _BCL_no_ac_battery_levels:1;	/* no AC/Battery levels in _BCL */
	u8 _BCL_reversed:1;		/* _BCL package is in a reversed order */
	u8 _BQC_use_index:1;		/* _BQC returns an index value */
};

struct acpi_video_device_brightness {
	int curr;
	int count;
	int *levels;
	struct acpi_video_brightness_flags flags;
};

struct acpi_device;

#define ACPI_VIDEO_CLASS	"video"

#define ACPI_VIDEO_DISPLAY_CRT  1
#define ACPI_VIDEO_DISPLAY_TV   2
#define ACPI_VIDEO_DISPLAY_DVI  3
#define ACPI_VIDEO_DISPLAY_LCD  4

#define ACPI_VIDEO_DISPLAY_LEGACY_MONITOR 0x0100
#define ACPI_VIDEO_DISPLAY_LEGACY_PANEL   0x0110
#define ACPI_VIDEO_DISPLAY_LEGACY_TV      0x0200

#if (defined CONFIG_ACPI_VIDEO || defined CONFIG_ACPI_VIDEO_MODULE)
extern int acpi_video_register(void);
extern void acpi_video_unregister(void);
extern void acpi_video_unregister_backlight(void);
extern int acpi_video_get_edid(struct acpi_device *device, int type,
			       int device_id, void **edid);
extern int acpi_video_get_levels(struct acpi_device *device,
				struct acpi_video_device_brightness **dev_br);
extern bool acpi_video_verify_backlight_support(void);
#else
static inline int acpi_video_register(void) { return 0; }
static inline void acpi_video_unregister(void) { return; }
static inline void acpi_video_unregister_backlight(void) { return; }
static inline int acpi_video_get_edid(struct acpi_device *device, int type,
				      int device_id, void **edid)
{
	return -ENODEV;
}
static int acpi_video_get_levels(struct acpi_device *device,
			struct acpi_video_device_brightness **dev_br)
{
	return -ENODEV;
}
static inline bool acpi_video_verify_backlight_support(void) { return false; }
#endif

#endif
