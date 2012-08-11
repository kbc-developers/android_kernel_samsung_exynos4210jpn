/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2012 sakuramilk <c.sakuramilk@gmail.com>
 * Copyright 2005 Phil Blundell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <mach/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/earlysuspend.h>
#include <linux/io.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include "issp_extern.h"
#ifdef CONFIG_TOUCHSCREEN_ATMEL_MXT540E
#include <linux/i2c/mxt540e.h>
#else
#include <linux/i2c/mxt224_u1.h>
#endif
#include <linux/i2c/touchkey_i2c.h>

/* target check */
#if defined(TK_USE_GENERAL_SMBUS)
#error not support TK_USE_GENERAL_SMBUS.
#endif
#if defined(TK_HAS_AUTOCAL)
#error not support TK_HAS_AUTOCAL.
#endif
#if defined(TK_HAS_FIRMWARE_UPDATE)
#error not support TK_HAS_FIRMWARE_UPDATE.
#endif
#if defined(CONFIG_TARGET_LOCALE_NAATT_TEMP) ||\
    defined(CONFIG_TARGET_LOCALE_NAATT) ||\
    defined(CONFIG_TARGET_LOCALE_NA)
#error not support target local NA ot NAATT
#endif

/* M0 Touchkey temporary setting */

#if defined(CONFIG_MACH_M0) || defined(CONFIG_MACH_C1VZW)
#define CONFIG_MACH_Q1_BD
#elif defined(CONFIG_MACH_C1) && !defined(CONFIG_TARGET_LOCALE_KOR)
#define CONFIG_MACH_Q1_BD
#elif defined(CONFIG_MACH_C1) && defined(CONFIG_TARGET_LOCALE_KOR)
/* C1 KOR doesn't use Q1_BD */
#endif

#if defined(CONFIG_MACH_M0) || defined(CONFIG_MACH_C1) || defined(CONFIG_MACH_C1VZW)
static int touchkey_keycode[3] = { 0, KEY_BACK, KEY_MENU,};
#else
static int touchkey_keycode[3] = { 0, KEY_MENU, KEY_BACK };
#endif
static const int touchkey_count = sizeof(touchkey_keycode) / sizeof(int);

#if defined(CONFIG_TARGET_LOCALE_KOR)
#ifndef TRUE
#define TRUE	1
#endif

#ifndef FALSE
#define FALSE	0
#endif

#if defined(SEC_TKEY_EVENT_DEBUG)
static bool g_debug_tkey = TRUE;
#else
static bool g_debug_tkey = FALSE;
#endif
#endif /* defined(CONFIG_TARGET_LOCALE_KOR) */

#if defined(CONFIG_GENERIC_BLN)
#include <linux/bln.h>
#endif
#if defined(CONFIG_GENERIC_BLN) || defined(CONFIG_CM_BLN)
#include <linux/mutex.h>
#include <linux/wakelock.h>
struct touchkey_i2c* bln_tkey_i2c;
#endif

/*
 * Generic LED Notification functionality.
 */
#ifdef CONFIG_GENERIC_BLN
struct wake_lock bln_wake_lock;
bool bln_enabled = false;
static DEFINE_MUTEX(bln_sem);
#endif

static int touchkey_i2c_check(struct touchkey_i2c *tkey_i2c);

static u8 menu_sensitivity;
static u8 back_sensitivity;

static int touchkey_enable;
static bool touchkey_probe = true;

static const struct i2c_device_id sec_touchkey_id[] = {
	{"sec_touchkey", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, sec_touchkey_id);

extern int get_touchkey_firmware(char *version);
static int touchkey_led_status;
static int touchled_cmd_reversed;

static int touchkey_debug_count;
static char touchkey_debug[104];
static int touchkey_update_status;

/* led i2c write value convert helper */
static int inline touchkey_conv_led_data__(int data, const char* func_name) {
#if defined(CONFIG_MACH_Q1_BD) || defined(CONFIG_MACH_C1)
	if (data == 1)
		data = 0x10;
	else if (data == 2)
		data = 0x20;
#endif
	printk("[TouchKey] %s led %s\n", func_name, (data == 0x1 || data == 0x10) ? "on" : "off");
	return data;
}
#define touchkey_conv_led_data(v) touchkey_conv_led_data__(v, __func__)

static void change_touch_key_led_voltage(int vol_mv)
{
	struct regulator *tled_regulator;

	tled_regulator = regulator_get(NULL, "touch_led");
	if (IS_ERR(tled_regulator)) {
		pr_err("%s: failed to get resource %s\n", __func__, "touch_led");
		return;
	}
	regulator_set_voltage(tled_regulator, vol_mv * 1000, vol_mv * 1000);
	regulator_put(tled_regulator);
}

static void get_touch_key_led_voltage(struct touchkey_i2c *tkey_i2c)
{
	struct regulator *tled_regulator;

	tled_regulator = regulator_get(NULL, "touch_led");
	if (IS_ERR(tled_regulator)) {
		pr_err("%s: failed to get resource %s\n", __func__, "touch_led");
		return;
	}

	tkey_i2c->brightness = regulator_get_voltage(tled_regulator) / 1000;

}

static ssize_t brightness_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	return sprintf(buf,"%d\n", tkey_i2c->brightness);
}

static ssize_t brightness_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int data;

	if (sscanf(buf, "%d\n", &data) == 1) {
		printk(KERN_ERR "[TouchKey] touch_led_brightness: %d\n", data);
		change_touch_key_led_voltage(data);
		tkey_i2c->brightness = data;
	} else {
		printk(KERN_ERR "[TouchKey] touch_led_brightness Error\n");
	}

	return size;
}

static void set_touchkey_debug(char value)
{
	if (touchkey_debug_count == 100)
		touchkey_debug_count = 0;

	touchkey_debug[touchkey_debug_count] = value;
	touchkey_debug_count++;
}

static int i2c_touchkey_read(struct i2c_client *client, u8 reg, u8 *val, unsigned int len)
{
	int err = 0;
	int retry = 2;
	struct i2c_msg msg[1];

	if ((client == NULL) || !(touchkey_enable == 1) || !touchkey_probe) {
		printk(KERN_ERR "[TouchKey] touchkey is not enabled. %d\n", __LINE__);
		return -ENODEV;
	}

	while (retry--) {
		msg->addr = client->addr;
		msg->flags = I2C_M_RD;
		msg->len = len;
		msg->buf = val;
		err = i2c_transfer(client->adapter, msg, 1);

		if (err >= 0)
			return 0;
		printk(KERN_ERR "[TouchKey] %s %d i2c transfer error\n", __func__, __LINE__);
		mdelay(10);
	}
	return err;

}

static int i2c_touchkey_write(struct i2c_client *client, u8 *val, unsigned int len)
{
	int err = 0;
	struct i2c_msg msg[1];
	int retry = 2;

	if ((client == NULL) || !(touchkey_enable == 1) || !touchkey_probe) {
		printk(KERN_ERR "[TouchKey] touchkey is not enabled. %d\n", __LINE__);
		return -ENODEV;
	}

	while (retry--) {
		msg->addr = client->addr;
		msg->flags = I2C_M_WR;
		msg->len = len;
		msg->buf = val;
		err = i2c_transfer(client->adapter, msg, 1);

		if (err >= 0)
			return 0;

		printk(KERN_DEBUG "[TouchKey] %s %d i2c transfer error\n", __func__, __LINE__);
		mdelay(10);
	}
	return err;
}

void touchkey_firmware_update(struct touchkey_i2c *tkey_i2c)
{
	char data[3];
	int retry;
	int ret = 0;

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_DEBUG "[TouchKey] i2c read fail. do not excute firm update.\n");
		return;
	}

	printk(KERN_ERR "%s F/W version: 0x%x, Module version:0x%x\n", __func__, data[1], data[2]);
	retry = 3;

	tkey_i2c->firmware_ver = data[1];
	tkey_i2c->module_ver = data[2];

	if (tkey_i2c->firmware_ver < 0x0A) {
		touchkey_update_status = 1;
		while (retry--) {
			if (ISSP_main(tkey_i2c) == 0) {
				printk(KERN_ERR "[TouchKey] Touchkey_update succeeded\n");
				touchkey_update_status = 0;
				break;
			}
			printk(KERN_ERR "[TouchKey] touchkey_update failed...retry...\n");
		}
		if (retry <= 0) {
			tkey_i2c->pdata->power_on(0);
			touchkey_update_status = -1;
		}
	} else {
		if (tkey_i2c->firmware_ver >= 0x0A) {
			printk(KERN_ERR "[TouchKey] Not F/W update. Cypess touch-key F/W version is latest\n");
		} else {
			printk(KERN_ERR "[TouchKey] Not F/W update. Cypess touch-key version(module or F/W) is not valid\n");
		}
	}
}

static irqreturn_t touchkey_interrupt(int irq, void *dev_id)
{
	struct touchkey_i2c *tkey_i2c = dev_id;
	u8 data[3];
	int ret;
	int retry = 10;
	int keycode_type = 0;
	int pressed;

	set_touchkey_debug('a');

	retry = 3;
	while (retry--) {
		ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
		if (!ret) {
			break;
		} else {
			printk(KERN_DEBUG "[TouchKey] i2c read failed, ret:%d, retry: %d\n", ret, retry);
			continue;
		}
	}
	if (ret < 0)
		return IRQ_HANDLED;

	set_touchkey_debug(data[0]);

	keycode_type = (data[0] & TK_BIT_KEYCODE);
	pressed = !(data[0] & TK_BIT_PRESS_EV);

	if (keycode_type <= 0 || keycode_type >= touchkey_count) {
		printk(KERN_DEBUG "[Touchkey] keycode_type err\n");
		return IRQ_HANDLED;
	}

	if (pressed)
		set_touchkey_debug('P');

	if (get_tsp_status() && pressed)
		printk(KERN_DEBUG "[TouchKey] touchkey pressed but don't send event because touch is pressed.\n");
	else {
		input_report_key(tkey_i2c->input_dev, touchkey_keycode[keycode_type], pressed);
		input_sync(tkey_i2c->input_dev);
		/* printk(KERN_DEBUG "[TouchKey] keycode:%d pressed:%d\n", touchkey_keycode[keycode_index], pressed); */
	}
	set_touchkey_debug('A');
	return IRQ_HANDLED;
}

#ifdef CONFIG_GENERIC_BLN
static void touchkey_bln_enable(void)
{
	int data;

	mutex_lock(&bln_sem);
	if (bln_enabled)
	{
		if (touchkey_enable == 0) {
			if (!wake_lock_active(&bln_wake_lock)) {
				wake_lock(&bln_wake_lock);
			}
			bln_tkey_i2c->pdata->power_on(1);
			msleep(50);
			bln_tkey_i2c->pdata->led_power_on(1);
			touchkey_enable = 1;
		}
		change_touch_key_led_voltage(bln_tkey_i2c->brightness);
		data = touchkey_conv_led_data(1);
		i2c_touchkey_write(bln_tkey_i2c->client, (u8 *)&data, 1);
		touchkey_led_status = 2;
		touchled_cmd_reversed = 1;
	}
	mutex_unlock(&bln_sem);
}

static void touchkey_bln_disable(void)
{
	int data;

	mutex_lock(&bln_sem);
	if (bln_enabled)
	{
		data = touchkey_conv_led_data(2);
		i2c_touchkey_write(bln_tkey_i2c->client, (u8 *)&data, 1);
		if (touchkey_enable == 1) {
			bln_tkey_i2c->pdata->led_power_on(0);
			bln_tkey_i2c->pdata->power_on(0);
			touchkey_enable = 0;
			wake_unlock(&bln_wake_lock);
		}
		touchkey_led_status = 1;
		touchled_cmd_reversed = 1;
	}
	mutex_unlock(&bln_sem);
}

static struct bln_implementation cypress_touchkey_bln = {
	.enable = touchkey_bln_enable,
	.disable = touchkey_bln_disable,
};
#endif // CONFIG_GENERIC_BLN

#ifdef CONFIG_HAS_EARLYSUSPEND
static int sec_touchkey_early_suspend(struct early_suspend *h)
{
	struct touchkey_i2c *tkey_i2c = container_of(h, struct touchkey_i2c, early_suspend);
	int ret;
	int i;

	disable_irq(tkey_i2c->irq);
	ret = cancel_work_sync(&tkey_i2c->update_work);
	if (ret) {
		printk(KERN_DEBUG "[Touchkey] enable_irq ret=%d\n", ret);
		enable_irq(tkey_i2c->irq);
	}

	/* release keys */
	for (i = 1; i < touchkey_count; ++i) {
		input_report_key(tkey_i2c->input_dev, touchkey_keycode[i], 0);
	}

#ifdef CONFIG_GENERIC_BLN
	mutex_lock(&bln_sem);
	bln_enabled = true;
#endif

	touchkey_enable = 0;
	set_touchkey_debug('S');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_early_suspend\n");
	if (touchkey_enable < 0) {
		printk(KERN_DEBUG "[TouchKey] ---%s---touchkey_enable: %d\n", __func__, touchkey_enable);
		goto out;
	}

	/* disable ldo18 */
	tkey_i2c->pdata->led_power_on(0);

	/* disable ldo11 */
	tkey_i2c->pdata->power_on(0);

out:
#ifdef CONFIG_GENERIC_BLN
	mutex_unlock(&bln_sem);
#endif
	return 0;
}

static int sec_touchkey_late_resume(struct early_suspend *h)
{
	struct touchkey_i2c *tkey_i2c = container_of(h, struct touchkey_i2c, early_suspend);

	set_touchkey_debug('R');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_late_resume\n");

#ifdef CONFIG_GENERIC_BLN
	mutex_lock(&bln_sem);
#endif

	/* enable ldo11 */
	tkey_i2c->pdata->power_on(1);

	if (touchkey_enable < 0) {
		printk(KERN_DEBUG "[TouchKey] ---%s---touchkey_enable: %d\n", __func__, touchkey_enable);
		goto out;
	}
	msleep(50);
	tkey_i2c->pdata->led_power_on(1);

	touchkey_enable = 1;

	if (touchled_cmd_reversed) {
		touchled_cmd_reversed = 0;
		i2c_touchkey_write(tkey_i2c->client, (u8 *) &touchkey_led_status, 1);
		printk(KERN_DEBUG "LED returned on\n");
	}

	enable_irq(tkey_i2c->irq);

out:
#ifdef CONFIG_GENERIC_BLN
	bln_enabled = false;
	wake_unlock(&bln_wake_lock);
	mutex_unlock(&bln_sem);
#endif
	return 0;
}
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int touchkey_i2c_check(struct touchkey_i2c *tkey_i2c)
{
	char data[3] = { 0, };
	int ret = 0;

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_ERR "[TouchKey] module version read fail\n");
		return ret;
	}

	tkey_i2c->firmware_ver = data[1];
	tkey_i2c->module_ver = data[2];

	return ret;
}

ssize_t touchkey_update_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	char data[3] = { 0, };

	get_touchkey_firmware(data);
	put_user(data[1], buf);

	return 1;
}

static ssize_t touch_version_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	char data[3] = { 0, };
	int count;

	i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);

	count = sprintf(buf, "0x%x\n", data[1]);

	printk(KERN_DEBUG "[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk(KERN_DEBUG "[TouchKey] module_version_read 0x%x\n", data[2]);

	return count;
}

static ssize_t touch_version_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	printk(KERN_DEBUG "[TouchKey] input data --> %s\n", buf);

	return size;
}

void touchkey_update_func(struct work_struct *work)
{
	struct touchkey_i2c *tkey_i2c = container_of(work, struct touchkey_i2c, update_work);
	int retry = 3;
	touchkey_update_status = 1;
	printk(KERN_DEBUG "[TouchKey] %s start\n", __func__);
	touchkey_enable = 0;
	while (retry--) {
		if (ISSP_main(tkey_i2c) == 0) {
			printk(KERN_DEBUG "[TouchKey] touchkey_update succeeded\n");
			msleep(50);
			touchkey_enable = 1;
			touchkey_update_status = 0;
			enable_irq(tkey_i2c->irq);
			return;
		}
		tkey_i2c->pdata->power_on(0);
	}
	enable_irq(tkey_i2c->irq);
	touchkey_update_status = -1;
	printk(KERN_DEBUG "[TouchKey] touchkey_update failed\n");
	return;
}

static ssize_t touch_update_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	printk(KERN_DEBUG "[TouchKey] touchkey firmware update\n");

	if (*buf == 'S') {
		disable_irq(tkey_i2c->irq);
		schedule_work(&tkey_i2c->update_work);
	}
	return size;
}

static ssize_t touch_update_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count = 0;

	printk(KERN_DEBUG "[TouchKey] touch_update_read: touchkey_update_status %d\n", touchkey_update_status);

	if (touchkey_update_status == 0)
		count = sprintf(buf, "PASS\n");
	else if (touchkey_update_status == 1)
		count = sprintf(buf, "Downloading\n");
	else if (touchkey_update_status == -1)
		count = sprintf(buf, "Fail\n");

	return count;
}

static ssize_t touchkey_led_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int data;
	int errnum;

	if (sscanf(buf, "%d\n", &data) == 1) {
		data = touchkey_conv_led_data(data);
		errnum = i2c_touchkey_write(tkey_i2c->client, (u8 *) &data, 1);
		if (errnum == -ENODEV)
			touchled_cmd_reversed = 1;

		touchkey_led_status = data;
	} else {
		printk(KERN_DEBUG "[TouchKey] touchkey_led_control Error\n");
	}

	return size;
}

static ssize_t touchkey_menu_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
#if defined(CONFIG_MACH_Q1_BD) || (defined(CONFIG_MACH_C1) && defined(CONFIG_TARGET_LOCALE_KOR))
	u8 data[14] = { 0, };
	int ret;

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 14);

	printk(KERN_DEBUG "called %s data[13] =%d\n", __func__, data[13]);
	menu_sensitivity = data[13];
#else
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	menu_sensitivity = data[7];
#endif
	return sprintf(buf, "%d\n", menu_sensitivity);
}

static ssize_t touchkey_back_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
#if defined(CONFIG_MACH_Q1_BD) || (defined(CONFIG_MACH_C1) && defined(CONFIG_TARGET_LOCALE_KOR))
	u8 data[14] = { 0, };
	int ret;

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 14);

	printk(KERN_DEBUG "called %s data[11] =%d\n", __func__, data[11]);
	back_sensitivity = data[11];
#else
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	back_sensitivity = data[9];
#endif
	return sprintf(buf, "%d\n", back_sensitivity);
}

static ssize_t touch_sensitivity_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	unsigned char data = 0x40;
	i2c_touchkey_write(tkey_i2c->client, &data, 1);
	return size;
}

static ssize_t set_touchkey_firm_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", TK_FIRMWARE_VER);
}

static ssize_t set_touchkey_update_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int count = 0;
	int retry = 3;
	touchkey_update_status = 1;

	disable_irq(tkey_i2c->irq);

	while (retry--) {
		if (ISSP_main(tkey_i2c) == 0) {
			printk(KERN_ERR "[TouchKey]Touchkey_update succeeded\n");
			touchkey_update_status = 0;
			count = 1;
			msleep(50);
			break;
		}
		printk(KERN_ERR "[TouchKey] touchkey_update failed... retry...\n");
	}
	if (retry <= 0) {
		/* disable ldo11 */
		tkey_i2c->pdata->power_on(0);
		count = 0;
		printk(KERN_ERR "[TouchKey]Touchkey_update fail\n");
		touchkey_update_status = -1;
		enable_irq(tkey_i2c->irq);
		return count;
	}

	enable_irq(tkey_i2c->irq);

	return count;
}

static ssize_t set_touchkey_firm_version_read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	char data[3] = { 0, };
	int count;

	i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	count = sprintf(buf, "0x%x\n", data[1]);

	printk(KERN_DEBUG "[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk(KERN_DEBUG "[TouchKey] module_version_read 0x%x\n", data[2]);
	return count;
}

static ssize_t set_touchkey_firm_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count = 0;

	printk(KERN_DEBUG "[TouchKey] touch_update_read: touchkey_update_status %d\n", touchkey_update_status);

	if (touchkey_update_status == 0)
		count = sprintf(buf, "PASS\n");
	else if (touchkey_update_status == 1)
		count = sprintf(buf, "Downloading\n");
	else if (touchkey_update_status == -1)
		count = sprintf(buf, "Fail\n");

	return count;
}

#ifdef CONFIG_GENERIC_BLN
static ssize_t touchkey_bln_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int data, errnum;

	if (sscanf(buf, "%d\n", &data) == 1) {
		printk(KERN_ERR "[TouchKey] %s: %d \n", __func__, data);
		data = touchkey_conv_led_data(data);
		errnum = i2c_touchkey_write(tkey_i2c->client, (u8 *)&data, 1);
		if (errnum == -ENODEV)
			touchled_cmd_reversed = 1;
		touchkey_led_status = data;
	} else {
		printk(KERN_ERR "[TouchKey] touchkey_bln_control Error\n");
	}

	return size;
}
#endif

static DEVICE_ATTR(recommended_version, S_IRUGO | S_IWUSR | S_IWGRP, touch_version_read, touch_version_write);
static DEVICE_ATTR(updated_version, S_IRUGO | S_IWUSR | S_IWGRP, touch_update_read, touch_update_write);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL, touchkey_led_control);
static DEVICE_ATTR(touchkey_menu, S_IRUGO | S_IWUSR | S_IWGRP, touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO | S_IWUSR | S_IWGRP, touchkey_back_show, NULL);
static DEVICE_ATTR(touch_sensitivity, S_IRUGO | S_IWUSR | S_IWGRP, NULL, touch_sensitivity_control);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_update_show, NULL);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_status_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_version_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_version_read_show, NULL);
static DEVICE_ATTR(touchkey_brightness, S_IRUGO | S_IWUSR | S_IWGRP, brightness_read, brightness_control);
#ifdef CONFIG_GENERIC_BLN
static DEVICE_ATTR(touchkey_bln_control, S_IWUGO, NULL, touchkey_bln_control);
#endif

static struct attribute *touchkey_attributes[] = {
	&dev_attr_recommended_version.attr,
	&dev_attr_updated_version.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_menu.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touch_sensitivity.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_brightness.attr,
#ifdef CONFIG_GENERIC_BLN
	&dev_attr_touchkey_bln_control.attr,
#endif
	NULL,
};

static struct attribute_group touchkey_attr_group = {
	.attrs = touchkey_attributes,
};

static int i2c_touchkey_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct touchkey_platform_data *pdata = client->dev.platform_data;
	struct touchkey_i2c *tkey_i2c;

	struct input_dev *input_dev;
	int err = 0;
	unsigned char data;
	int i;
	int ret;

	printk(KERN_DEBUG "[TouchKey] i2c_touchkey_probe\n");

	if (pdata == NULL) {
		printk(KERN_ERR "%s: no pdata\n", __func__);
		return -ENODEV;
	}

	/*Check I2C functionality */
	ret = i2c_check_functionality(client->adapter, I2C_FUNC_I2C);
	if (ret == 0) {
		printk(KERN_ERR "[Touchkey] No I2C functionality found\n");
		ret = -ENODEV;
		return ret;
	}

	/*Obtain kernel memory space for touchkey i2c */
	tkey_i2c = kzalloc(sizeof(struct touchkey_i2c), GFP_KERNEL);
	if (NULL == tkey_i2c) {
		printk(KERN_ERR "[Touchkey] failed to allocate tkey_i2c.\n");
		return -ENOMEM;
	}

	input_dev = input_allocate_device();

	if (!input_dev) {
		printk(KERN_ERR"[Touchkey] failed to allocate input device\n");
		kfree(tkey_i2c);
		return -ENOMEM;
	}

	input_dev->name = "sec_touchkey";
	input_dev->phys = "sec_touchkey/input0";
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &client->dev;

	/*tkey_i2c*/
	tkey_i2c->pdata = pdata;
	tkey_i2c->input_dev = input_dev;
	tkey_i2c->client = client;
	tkey_i2c->irq = client->irq;
	tkey_i2c->name = "sec_touchkey";

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	set_bit(EV_KEY, input_dev->evbit);

	for (i = 1; i < touchkey_count; i++)
		set_bit(touchkey_keycode[i], input_dev->keybit);

	input_set_drvdata(input_dev, tkey_i2c);

	ret = input_register_device(input_dev);
	if (ret) {
		printk(KERN_ERR"[Touchkey] failed to register input device\n");
		input_free_device(input_dev);
		kfree(tkey_i2c);
		return err;
	}

	INIT_WORK(&tkey_i2c->update_work, touchkey_update_func);

	tkey_i2c->pdata->power_on(1);
	msleep(50);

	/* read key led voltage */
	get_touch_key_led_voltage(tkey_i2c);

	touchkey_enable = 1;
	data = 1;

	/*sysfs*/
	tkey_i2c->dev = device_create(sec_class, NULL, 0, NULL, "sec_touchkey");

	if (IS_ERR(tkey_i2c->dev)) {
		printk(KERN_ERR "Failed to create device(tkey_i2c->dev)!\n");
		input_unregister_device(input_dev);
	} else {
		dev_set_drvdata(tkey_i2c->dev, tkey_i2c);
		ret = sysfs_create_group(&tkey_i2c->dev->kobj, &touchkey_attr_group);
		if (ret) {
			printk(KERN_ERR "[TouchKey]: failed to create sysfs group\n");
		}
	}

	ret = touchkey_i2c_check(tkey_i2c);
	if (ret < 0) {
		printk(KERN_ERR "[TouchKey] Probe fail\n");
		input_unregister_device(input_dev);
		touchkey_probe = false;
		return -ENODEV;
	}

	ret = request_threaded_irq(tkey_i2c->irq, NULL, touchkey_interrupt,
				IRQF_DISABLED | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, tkey_i2c->name, tkey_i2c);
	if (ret < 0) {
		printk(KERN_ERR "[Touchkey]: failed to request irq(%d) - %d\n", tkey_i2c->irq, ret);
		input_unregister_device(input_dev);
		return -EBUSY;
	}

#ifdef CONFIG_GENERIC_BLN
	bln_tkey_i2c = tkey_i2c;
	wake_lock_init(&bln_wake_lock, WAKE_LOCK_SUSPEND, "bln_wake_lock");
	register_bln_implementation(&cypress_touchkey_bln);
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	tkey_i2c->early_suspend.suspend = (void *)sec_touchkey_early_suspend;
	tkey_i2c->early_suspend.resume = (void *)sec_touchkey_late_resume;
	register_early_suspend(&tkey_i2c->early_suspend);
#endif

	tkey_i2c->pdata->led_power_on(1);

	set_touchkey_debug('K');
	return 0;
}

struct i2c_driver touchkey_i2c_driver = {
	.driver = {
		.name = "sec_touchkey_driver",
	},
	.id_table = sec_touchkey_id,
	.probe = i2c_touchkey_probe,
};

static int __init touchkey_init(void)
{
	int ret = 0;

#if defined(CONFIG_MACH_M0) || defined(CONFIG_MACH_C1VZW)
	if (system_rev < TOUCHKEY_FW_UPDATEABLE_HW_REV) {
		printk(KERN_DEBUG "[Touchkey] Doesn't support this board rev %d\n", system_rev);
		return 0;
	}
#elif defined(CONFIG_MACH_C1) && !defined(CONFIG_TARGET_LOCALE_KOR)
	if (system_rev < TOUCHKEY_FW_UPDATEABLE_HW_REV) {
		printk(KERN_DEBUG "[Touchkey] Doesn't support this board rev %d\n", system_rev);
		return 0;
	}
#endif

	ret = i2c_add_driver(&touchkey_i2c_driver);

	if (ret) {
		printk(KERN_ERR "[TouchKey] registration failed, module not inserted.ret= %d\n", ret);
	}

	return ret;
}

static void __exit touchkey_exit(void)
{
	printk(KERN_DEBUG "[TouchKey] %s\n", __func__);
	i2c_del_driver(&touchkey_i2c_driver);

#ifdef CONFIG_GENERIC_BLN
	wake_lock_destroy(&bln_wake_lock);
#endif
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@@@");
MODULE_DESCRIPTION("touch keypad");
