/**
 * Copyright (c) 2012-2013, NVIDIA Corporation.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property
 * and proprietary rights in and to this software and related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA Corporation is strictly prohibited.
 */

#ifndef __IMX219_H__
#define __IMX219_H__

#include <linux/ioctl.h>  /* For IOCTL macros */
#include <media/nvc.h>
#include <media/nvc_image.h>

#define IMX219_IOCTL_SET_MODE           _IOW('o', 1, struct imx219_mode)
#define IMX219_IOCTL_GET_STATUS         _IOR('o', 2, __u8)
#define IMX219_IOCTL_SET_FRAME_LENGTH   _IOW('o', 3, __u32)
#define IMX219_IOCTL_SET_COARSE_TIME    _IOW('o', 4, __u32)
#define IMX219_IOCTL_SET_GAIN           _IOW('o', 5, __u16)
#define IMX219_IOCTL_GET_FUSEID         _IOR('o', 6, struct nvc_fuseid)
#define IMX219_IOCTL_SET_GROUP_HOLD     _IOW('o', 7, struct imx219_ae)
#define IMX219_IOCTL_GET_AFDAT            _IOR('o', 8, __u32)
#define IMX219_IOCTL_SET_POWER		_IOW('o', 20, __u32)
#define IMX219_IOCTL_GET_FLASH_CAP	_IOR('o', 30, __u32)
#define IMX219_IOCTL_SET_FLASH_MODE _IOW('o', 31, struct imx219_flash_control)

struct imx219_gain {
	__u16 again;
	__u8 dgain_upper;
	__u8 dgain_lower;
};

struct imx219_mode {
	int xres;
	int yres;
	__u32 frame_length;
	__u32 coarse_time;
	struct imx219_gain gain;
};

struct imx219_ae {
	__u32 frame_length;
	__u8  frame_length_enable;
	__u32 coarse_time;
	__u8  coarse_time_enable;
	struct imx219_gain gain;
	__u8  gain_enable;
};

struct imx219_flash_control {
	u8 enable;
	u8 edge_trig_en;
	u8 start_edge;
	u8 repeat;
	u16 delay_frm;
};

#ifdef __KERNEL__
struct imx219_power_rail {
	struct regulator *dvdd;
	struct regulator *avdd;
	struct regulator *iovdd;
};

struct imx219_platform_data {
	struct imx219_flash_control flash_cap;
	const char *mclk_name; /* NULL for default default_mclk */
	int (*power_on)(struct imx219_power_rail *pw);
	int (*power_off)(struct imx219_power_rail *pw);
};
#endif /* __KERNEL__ */

#endif  /* __IMX219_H__ */
