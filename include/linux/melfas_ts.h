/*
 * include/linux/melfas_ts.h - platform data structure for MMS Series sensor
 *
 * Copyright (C) 2010 Melfas, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_MELFAS_TS_H
#define _LINUX_MELFAS_TS_H

#define MELFAS_TS_NAME "melfas-ts"

#if defined (CONFIG_MACH_C1_KDDI_REV00) 
struct melfas_version {
	uint8_t tsp_revision;	
	uint8_t hardware;
	uint8_t compatibility;
	uint8_t core;
	uint8_t private;
	uint8_t public;
	uint8_t product_code;
};
#endif
struct melfas_tsi_platform_data {
	int x_size;
	int y_size;
	int (*power)(int on);
#if defined (CONFIG_MACH_C1_KDDI_REV00) 
	struct melfas_version *version;
#else
		int  version;
#endif


	int gpio_int;

	void (*input_event)(void *data);

	bool mt_protocol_b;
	bool enable_btn_touch;

	void (*set_touch_i2c)(void);
	void (*set_touch_i2c_to_gpio)(void);

#ifdef CONFIG_INPUT_FBSUSPEND
	struct notifier_block fb_notif;
#endif

#if defined(CONFIG_MACH_C1CTC) || defined(CONFIG_MACH_M0_CHNOPEN) ||\
	defined(CONFIG_MACH_M0_CMCC) || defined(CONFIG_MACH_M0_CTC)
	int (*lcd_type)(void);
#endif
};

#endif /* _LINUX_MELFAS_TS_H */
