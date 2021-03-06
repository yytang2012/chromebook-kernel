/*
 * exynos_thermal.c - Samsung EXYNOS TMU (Thermal Management Unit)
 *
 *  Copyright (C) 2011 Samsung Electronics
 *  Donggeun Kim <dg77.kim@samsung.com>
 *  Amit Daniel Kachhap <amit.kachhap@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/platform_data/exynos_thermal.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include <plat/cpu.h>
#include <mach/regs-pmu.h>	/* for EXYNOS5_PS_HOLD_CONTROL */

/* Exynos generic registers */
#define EXYNOS_TMU_REG_TRIMINFO		0x0
#define EXYNOS_TMU_REG_CONTROL		0x20
#define EXYNOS_TMU_REG_STATUS		0x28
#define EXYNOS_TMU_REG_CURRENT_TEMP	0x40
#define EXYNOS_TMU_REG_INTEN		0x70
#define EXYNOS_TMU_REG_INTSTAT		0x74
#define EXYNOS_TMU_REG_INTCLEAR		0x78

#define EXYNOS_TMU_TRIM_TEMP_MASK	0xff
#define EXYNOS_TMU_GAIN_SHIFT		8
#define EXYNOS_TMU_REF_VOLTAGE_SHIFT	24
#define EXYNOS_TMU_TRIP_EN		(1 << 12)
#define EXYNOS_TMU_CORE_ON		3
#define EXYNOS_TMU_CORE_OFF		2
#define EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET	50

/* Exynos4210 specific registers */
#define EXYNOS4210_TMU_REG_THRESHOLD_TEMP	0x44
#define EXYNOS4210_TMU_REG_TRIG_LEVEL0	0x50
#define EXYNOS4210_TMU_REG_TRIG_LEVEL1	0x54
#define EXYNOS4210_TMU_REG_TRIG_LEVEL2	0x58
#define EXYNOS4210_TMU_REG_TRIG_LEVEL3	0x5C
#define EXYNOS4210_TMU_REG_PAST_TEMP0	0x60
#define EXYNOS4210_TMU_REG_PAST_TEMP1	0x64
#define EXYNOS4210_TMU_REG_PAST_TEMP2	0x68
#define EXYNOS4210_TMU_REG_PAST_TEMP3	0x6C

#define EXYNOS4210_TMU_TRIG_LEVEL0_MASK	0x1
#define EXYNOS4210_TMU_TRIG_LEVEL1_MASK	0x10
#define EXYNOS4210_TMU_TRIG_LEVEL2_MASK	0x100
#define EXYNOS4210_TMU_TRIG_LEVEL3_MASK	0x1000
#define EXYNOS4210_TMU_INTCLEAR_VAL	0x1111

/* Exynos5250 and Exynos4412 specific registers */
#define EXYNOS_TMU_TRIMINFO_CON	0x14
#define EXYNOS_THD_TEMP_RISE		0x50
#define EXYNOS_THD_TEMP_FALL		0x54
#define EXYNOS_EMUL_CON		0x80

#define EXYNOS_TRIMINFO_RELOAD		0x1
#define EXYNOS_TMU_CLEAR_RISE_INT	0x111
#define EXYNOS_TMU_CLEAR_FALL_INT	(0x111 << 12)

/* Register bit change for EXYNOS5420 */
#define EXYNOS5420_TMU_CLEAR_FALL_INT	(0x111 << 16)

#define EXYNOS_TMU_HWTRIG_ENABLE	(1 << 31)
#define EXYNOS_MUX_ADDR_VALUE		6
#define EXYNOS_MUX_ADDR_SHIFT		20
#define EXYNOS_TMU_TRIP_MODE_SHIFT	13

#define EFUSE_MIN_VALUE 40
#define EFUSE_MAX_VALUE 100

/* In-kernel thermal framework related macros & definations */
#define SENSOR_NAME_LEN	16
#define MAX_TRIP_COUNT	8
#define MAX_COOLING_DEVICE 4
#define MAX_THRESHOLD_LEVS 4

#define ACTIVE_INTERVAL 500
#define IDLE_INTERVAL 10000
#define MCELSIUS	1000

/* CPU Zone information */
#define PANIC_ZONE      4
#define WARN_ZONE       3
#define MONITOR_ZONE    2
#define SAFE_ZONE       1

#define GET_ZONE(trip) (trip + 2)
#define GET_TRIP(zone) (zone - 2)

#define EXYNOS_ZONE_COUNT	3

struct exynos_thermal_zone;

struct exynos_tmu_data {
	struct exynos_tmu_platform_data *pdata;
	struct resource *mem;
	void __iomem *base, *triminfo_base;
	int irq;
	enum soc_type soc;
	struct work_struct irq_work;
	struct mutex lock;
	struct clk *clk;
	struct clk *triminfo_clk;
	u32 thd_temp_rise, thd_temp_fall;	/* exynos5 resume support */
	struct exynos_thermal_zone *th_zone;
	u8 temp_error1, temp_error2;
};

struct	thermal_trip_point_conf {
	int trip_val[MAX_TRIP_COUNT];
	int trip_count;
	u8 trigger_falling;
};

struct	thermal_cooling_conf {
	struct freq_clip_table freq_data[MAX_TRIP_COUNT];
	int freq_clip_count;
};

static int thermal_sensor_count;

struct thermal_sensor_conf {
	char name[SENSOR_NAME_LEN];
	int (*read_temperature)(void *data);
	struct thermal_trip_point_conf trip_data;
	struct thermal_cooling_conf cooling_data;
	void *private_data;
};

struct exynos_thermal_zone {
	enum thermal_device_mode mode;
	struct thermal_zone_device *therm_dev;
	struct thermal_cooling_device *cool_dev[MAX_COOLING_DEVICE];
	unsigned int cool_dev_size;
	struct platform_device *exynos4_dev;
	struct thermal_sensor_conf *sensor_conf;
	bool bind;
};

static void exynos_unregister_thermal(struct exynos_tmu_data *data);
static int exynos_register_thermal(struct thermal_sensor_conf *sensor_conf,
				   struct exynos_tmu_data *data);

/* Get mode callback functions for thermal zone */
static int exynos_get_mode(struct thermal_zone_device *thermal,
			enum thermal_device_mode *mode)
{
	struct exynos_tmu_data *data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = data->th_zone;

	if (th_zone)
		*mode = th_zone->mode;
	return 0;
}

/* Set mode callback functions for thermal zone */
static int exynos_set_mode(struct thermal_zone_device *thermal,
			enum thermal_device_mode mode)
{
	struct exynos_tmu_data *data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = data->th_zone;

	if (!th_zone->therm_dev) {
		pr_notice("thermal zone not registered\n");
		return 0;
	}

	mutex_lock(&th_zone->therm_dev->lock);

	if (mode == THERMAL_DEVICE_ENABLED &&
		!th_zone->sensor_conf->trip_data.trigger_falling)
		th_zone->therm_dev->polling_delay = IDLE_INTERVAL;
	else
		th_zone->therm_dev->polling_delay = 0;

	mutex_unlock(&th_zone->therm_dev->lock);

	th_zone->mode = mode;
	thermal_zone_device_update(th_zone->therm_dev);
	pr_info("thermal polling set for duration=%d msec\n",
				th_zone->therm_dev->polling_delay);
	return 0;
}


/* Get trip type callback functions for thermal zone */
static int exynos_get_trip_type(struct thermal_zone_device *thermal, int trip,
				 enum thermal_trip_type *type)
{
	switch (GET_ZONE(trip)) {
	case MONITOR_ZONE:
	case WARN_ZONE:
		*type = THERMAL_TRIP_ACTIVE;
		break;
	case PANIC_ZONE:
		*type = THERMAL_TRIP_CRITICAL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Get trip temperature callback functions for thermal zone */
static int exynos_get_trip_temp(struct thermal_zone_device *thermal, int trip,
				unsigned long *temp)
{
	struct exynos_tmu_data *data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = data->th_zone;

	if (trip < GET_TRIP(MONITOR_ZONE) || trip > GET_TRIP(PANIC_ZONE))
		return -EINVAL;

	*temp = th_zone->sensor_conf->trip_data.trip_val[trip];
	/* convert the temperature into millicelsius */
	*temp = *temp * MCELSIUS;

	return 0;
}

/* Get critical temperature callback functions for thermal zone */
static int exynos_get_crit_temp(struct thermal_zone_device *thermal,
				unsigned long *temp)
{
	int ret;
	/* Panic zone */
	ret = exynos_get_trip_temp(thermal, GET_TRIP(PANIC_ZONE), temp);
	return ret;
}

/* Bind callback functions for thermal zone */
static int exynos_bind(struct thermal_zone_device *thermal,
			struct thermal_cooling_device *cdev)
{
	int ret = 0, i, tab_size, level;
	struct freq_clip_table *tab_ptr, *clip_data;
	struct exynos_tmu_data *tmu_data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = tmu_data->th_zone;
	struct thermal_sensor_conf *data = th_zone->sensor_conf;


	tab_ptr = (struct freq_clip_table *)data->cooling_data.freq_data;
	tab_size = data->cooling_data.freq_clip_count;

	if (tab_ptr == NULL || tab_size == 0)
		return -EINVAL;

	/* find the cooling device registered*/
	for (i = 0; i < th_zone->cool_dev_size; i++)
		if (cdev == th_zone->cool_dev[i])
			break;

	/* No matching cooling device */
	if (i == th_zone->cool_dev_size)
		return 0;

	/* Bind the thermal zone to the cpufreq cooling device */
	for (i = 0; i < tab_size; i++) {
		clip_data = (struct freq_clip_table *)&(tab_ptr[i]);
		level = cpufreq_cooling_get_level(0, clip_data->freq_clip_max);
		if (level == THERMAL_CSTATE_INVALID)
			return 0;
		switch (GET_ZONE(i)) {
		case MONITOR_ZONE:
		case WARN_ZONE:
			if (thermal_zone_bind_cooling_device(thermal, i, cdev,
								level, 0)) {
				pr_err("error binding cdev inst %d\n", i);
				ret = -EINVAL;
			}
			th_zone->bind = true;
			break;
		default:
			ret = -EINVAL;
		}
	}

	return ret;
}

/* Unbind callback functions for thermal zone */
static int exynos_unbind(struct thermal_zone_device *thermal,
			struct thermal_cooling_device *cdev)
{
	int ret = 0, i, tab_size;
	struct exynos_tmu_data *tmu_data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = tmu_data->th_zone;
	struct thermal_sensor_conf *data = th_zone->sensor_conf;

	if (th_zone->bind == false)
		return 0;

	tab_size = data->cooling_data.freq_clip_count;

	if (tab_size == 0)
		return -EINVAL;

	/* find the cooling device registered*/
	for (i = 0; i < th_zone->cool_dev_size; i++)
		if (cdev == th_zone->cool_dev[i])
			break;

	/* No matching cooling device */
	if (i == th_zone->cool_dev_size)
		return 0;

	/* Bind the thermal zone to the cpufreq cooling device */
	for (i = 0; i < tab_size; i++) {
		switch (GET_ZONE(i)) {
		case MONITOR_ZONE:
		case WARN_ZONE:
			if (thermal_zone_unbind_cooling_device(thermal, i,
								cdev)) {
				pr_err("error unbinding cdev inst=%d\n", i);
				ret = -EINVAL;
			}
			th_zone->bind = false;
			break;
		default:
			ret = -EINVAL;
		}
	}
	return ret;
}

/* Get temperature callback functions for thermal zone */
static int exynos_get_temp(struct thermal_zone_device *thermal,
			unsigned long *temp)
{
	struct exynos_tmu_data *tmu_data = thermal->devdata;
	struct exynos_thermal_zone *th_zone = tmu_data->th_zone;
	void *data;

	if (!th_zone->sensor_conf) {
		pr_info("Temperature sensor not initialised\n");
		return -EINVAL;
	}
	data = th_zone->sensor_conf->private_data;
	*temp = th_zone->sensor_conf->read_temperature(data);
	/* convert the temperature into millicelsius */
	*temp = *temp * MCELSIUS;
	return 0;
}

/* Get the temperature trend */
static int exynos_get_trend(struct thermal_zone_device *thermal,
			int trip, enum thermal_trend *trend)
{
	int ret;
	unsigned long trip_temp;

	ret = exynos_get_trip_temp(thermal, trip, &trip_temp);
	if (ret < 0)
		return ret;

	if (thermal->temperature >= trip_temp)
		*trend = THERMAL_TREND_RAISE_FULL;
	else
		*trend = THERMAL_TREND_DROP_FULL;

	return 0;
}
/* Operation callback functions for thermal zone */
static struct thermal_zone_device_ops const exynos_dev_ops = {
	.bind = exynos_bind,
	.unbind = exynos_unbind,
	.get_temp = exynos_get_temp,
	.get_trend = exynos_get_trend,
	.get_mode = exynos_get_mode,
	.set_mode = exynos_set_mode,
	.get_trip_type = exynos_get_trip_type,
	.get_trip_temp = exynos_get_trip_temp,
	.get_crit_temp = exynos_get_crit_temp,
};

/*
 * This function may be called from interrupt based temperature sensor
 * when threshold is changed.
 */
static void exynos_report_trigger(struct exynos_thermal_zone *th_zone)
{
	unsigned int i;
	char data[10];
	char *envp[] = { data, NULL };

	if (!th_zone || !th_zone->therm_dev)
		return;
	if (th_zone->bind == false) {
		for (i = 0; i < th_zone->cool_dev_size; i++) {
			if (!th_zone->cool_dev[i])
				continue;
			exynos_bind(th_zone->therm_dev,
					th_zone->cool_dev[i]);
		}
	}

	thermal_zone_device_update(th_zone->therm_dev);

	mutex_lock(&th_zone->therm_dev->lock);
	/* Find the level for which trip happened */
	for (i = 0; i < th_zone->sensor_conf->trip_data.trip_count; i++) {
		if (th_zone->therm_dev->last_temperature <
			th_zone->sensor_conf->trip_data.trip_val[i] * MCELSIUS)
			break;
	}

	if (th_zone->mode == THERMAL_DEVICE_ENABLED &&
		!th_zone->sensor_conf->trip_data.trigger_falling) {
		if (i > 0)
			th_zone->therm_dev->polling_delay = ACTIVE_INTERVAL;
		else
			th_zone->therm_dev->polling_delay = IDLE_INTERVAL;
	}

	snprintf(data, sizeof(data), "%u", i);
	kobject_uevent_env(&th_zone->therm_dev->device.kobj, KOBJ_CHANGE, envp);
	mutex_unlock(&th_zone->therm_dev->lock);
}

/* Register with the in-kernel thermal management */
static int exynos_register_thermal(struct thermal_sensor_conf *sensor_conf,
				   struct exynos_tmu_data *data)
{
	int ret;
	struct cpumask mask_val;
	struct exynos_thermal_zone *th_zone;

	if (!sensor_conf || !sensor_conf->read_temperature) {
		pr_err("Temperature sensor not initialised\n");
		return -EINVAL;
	}

	th_zone = kzalloc(sizeof(struct exynos_thermal_zone), GFP_KERNEL);
	if (!th_zone)
		return -ENOMEM;

	th_zone->sensor_conf = sensor_conf;
	cpumask_setall(&mask_val);
	th_zone->cool_dev[0] = cpufreq_cooling_register(&mask_val);
	if (IS_ERR(th_zone->cool_dev[0])) {
		pr_err("Failed to register cpufreq cooling device\n");
		ret = -EINVAL;
		goto err_unregister;
	}
	th_zone->cool_dev_size++;
	data->th_zone = th_zone;

	th_zone->therm_dev = thermal_zone_device_register(sensor_conf->name,
			EXYNOS_ZONE_COUNT, 0, data, &exynos_dev_ops, NULL, 0,
			sensor_conf->trip_data.trigger_falling ?
			0 : IDLE_INTERVAL);

	if (IS_ERR(th_zone->therm_dev)) {
		pr_err("Failed to register thermal zone device\n");
		ret = -EINVAL;
		goto err_unregister;
	}
	th_zone->mode = THERMAL_DEVICE_ENABLED;

	pr_info("Exynos: Kernel Thermal management registered\n");
	return 0;

err_unregister:
	exynos_unregister_thermal(data);
	return ret;
}

/* Un-Register with the in-kernel thermal management */
static void exynos_unregister_thermal(struct exynos_tmu_data *data)
{
	struct exynos_thermal_zone *th_zone = data->th_zone;
	int i;

	if (!th_zone)
		return;

	if (th_zone->therm_dev)
		thermal_zone_device_unregister(th_zone->therm_dev);

	for (i = 0; i < th_zone->cool_dev_size; i++) {
		if (th_zone->cool_dev[i])
			cpufreq_cooling_unregister(th_zone->cool_dev[i]);
	}

	kfree(th_zone);
	pr_info("Exynos: Kernel Thermal management unregistered\n");
}

/*
 * TMU treats temperature as a mapped temperature code.
 * The temperature is converted differently depending on the calibration type.
 */
static int temp_to_code(struct exynos_tmu_data *data, u8 temp)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp_code;

	if (data->soc == SOC_ARCH_EXYNOS4210)
		/* temp should range between 25 and 125 */
		if (temp < 25 || temp > 125) {
			temp_code = -EINVAL;
			goto out;
		}

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp_code = (temp - 25) *
		    (data->temp_error2 - data->temp_error1) /
		    (85 - 25) + data->temp_error1;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp_code = temp + data->temp_error1 - 25;
		break;
	default:
		temp_code = temp + EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET;
		break;
	}
out:
	return temp_code;
}

/*
 * Calculate a temperature value from a temperature code.
 * The unit of the temperature is degree Celsius.
 */
static int code_to_temp(struct exynos_tmu_data *data, u8 temp_code)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp;

	if (data->soc == SOC_ARCH_EXYNOS4210)
		/* temp_code should range between 75 and 175 */
		if (temp_code < 75 || temp_code > 175) {
			temp = -ENODATA;
			goto out;
		}

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp = (temp_code - data->temp_error1) * (85 - 25) /
		    (data->temp_error2 - data->temp_error1) + 25;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp = temp_code - data->temp_error1 + 25;
		break;
	default:
		temp = temp_code - EXYNOS_TMU_DEF_CODE_TO_TEMP_OFFSET;
		break;
	}
out:
	return temp;
}

static int exynos5_setup_temp(struct platform_device *pdev, int trigger_levs)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;

	u8 hwtrig_code, hwtrig_temp;
	u32 rising_threshold = 0, falling_threshold = 0;
	int threshold_code, i;
	bool was_enabled;

	/* Write temperature code for rising and falling threshold */
	for (i = 0; i < trigger_levs; i++) {
		threshold_code = temp_to_code(data,
					      pdata->trigger_levels[i]);
		if (threshold_code < 0)
			return threshold_code;
		rising_threshold |= threshold_code << 8 * i;
		if (pdata->threshold_falling) {
			threshold_code = temp_to_code(data,
						      pdata->trigger_levels[i] -
						      pdata->threshold_falling);
			if (threshold_code > 0)
				falling_threshold |=
					threshold_code << 8 * i;
		}
	}

	was_enabled = (readl(data->base + EXYNOS_TMU_REG_CONTROL) &
		       EXYNOS_TMU_TRIP_EN) == EXYNOS_TMU_TRIP_EN;

	/* On "warm" and "cold" reset, hwtrig_enable remains set
	 * but TEMP_RISE gets cleared. :(
	 * Value ranges copied from "62.3 Temperature Code Table"
	 * of "S5PC520 User's Manual" version 0.01 Oct 2011.
	 */
	hwtrig_code = readl(data->base + EXYNOS_THD_TEMP_RISE) >> 24;
	hwtrig_temp = code_to_temp(data, hwtrig_code);
	threshold_code = temp_to_code(data, pdata->trigger_levels[2]);
	if (was_enabled && hwtrig_temp >= 25 && hwtrig_temp <= 125
			&& (hwtrig_code < threshold_code)) {
		/* Boot FW set the HW trig value.  */
		pr_warn("Thermal: HW poweroff threshold (%d) "
			"< Kernel shutdown threshold (%d). "
			"Changing Kernel shutdown to %d C.\n",
			hwtrig_code, threshold_code,
			code_to_temp(data, hwtrig_code - 2));

		/* ckear previous code */
		rising_threshold &= ~(0xffff << 16);
		if (pdata->threshold_falling)
			falling_threshold &= ~(0xffff << 16);

		/* don't have to use temp. 1:1 mapping of temp:code */
		rising_threshold |= ((hwtrig_code - 2) << 16) |
			(hwtrig_code << 24);
		if (pdata->threshold_falling) {
			hwtrig_code -= pdata->threshold_falling;
			falling_threshold |= ((hwtrig_code - 2) << 16) |
				(hwtrig_code << 24);
		}
	}

	data->thd_temp_rise = rising_threshold;
	data->thd_temp_fall = falling_threshold;
	return 0;
}


static int exynos_tmu_initialize(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int status, trim_info;
	int ret = 0, threshold_code, i, trigger_levs = 0;

	mutex_lock(&data->lock);
	clk_enable(data->clk);
	if (!IS_ERR(data->triminfo_clk))
		clk_enable(data->triminfo_clk);

	status = readb(data->base + EXYNOS_TMU_REG_STATUS);
	if (!status) {
		ret = -EBUSY;
		goto out;
	}

	if (data->soc >= SOC_ARCH_EXYNOS) {
		__raw_writel(EXYNOS_TRIMINFO_RELOAD,
				data->base + EXYNOS_TMU_TRIMINFO_CON);
	}

	/* Save trimming info in order to perform calibration */
	if (data->triminfo_base)
		/* On exynos5420 TRIMINFO is misplaced for some channels */
		trim_info = readl(data->triminfo_base);
	else
		trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO);

	data->temp_error1 = trim_info & EXYNOS_TMU_TRIM_TEMP_MASK;
	data->temp_error2 = ((trim_info >> 8) & EXYNOS_TMU_TRIM_TEMP_MASK);

	/*
	 * TRIMINFO values should always be valid on Exynos5420
	 */
	if (!soc_is_exynos542x()) {
		if ((EFUSE_MIN_VALUE > data->temp_error1) ||
				(data->temp_error1 > EFUSE_MAX_VALUE) ||
				(data->temp_error2 != 0))
			data->temp_error1 = pdata->efuse_value;
	}

	/* Count trigger levels to be enabled */
	for (i = 0; i < MAX_THRESHOLD_LEVS; i++)
		if (pdata->trigger_levels[i])
			trigger_levs++;

	/* Disable all interrupts */
	writel(0, data->base + EXYNOS_TMU_REG_INTEN);

	if (data->soc == SOC_ARCH_EXYNOS4210) {
		/* Write temperature code for threshold */
		threshold_code = temp_to_code(data, pdata->threshold);
		if (threshold_code < 0) {
			ret = threshold_code;
			goto out;
		}
		writeb(threshold_code,
			data->base + EXYNOS4210_TMU_REG_THRESHOLD_TEMP);
		for (i = 0; i < trigger_levs; i++)
			writeb(pdata->trigger_levels[i],
			data->base + EXYNOS4210_TMU_REG_TRIG_LEVEL0 + i * 4);

		writel(EXYNOS4210_TMU_INTCLEAR_VAL,
			data->base + EXYNOS_TMU_REG_INTCLEAR);
	} else if (data->soc >= SOC_ARCH_EXYNOS) {
		/*
		 * thd_temp_{rise,fall} are nonzero on resume. Don't compute
		 * again.
		 */
		if (!data->thd_temp_rise || (pdata->threshold_falling &&
					     !data->thd_temp_fall)) {
			ret = exynos5_setup_temp(pdev, trigger_levs);
			if (ret < 0)
 				goto out;
		}

		writel(data->thd_temp_rise,
				data->base + EXYNOS_THD_TEMP_RISE);
		writel(data->thd_temp_fall,
				data->base + EXYNOS_THD_TEMP_FALL);

		/* clear all interrupt status bits regardless of which ones
		 * we will enable. HW Trigger seems to depend on REISE_LEVEL3
		 * being cleared.
		 */
		writel(~0, data->base + EXYNOS_TMU_REG_INTCLEAR);
	}

out:
	clk_disable(data->clk);
	if (!IS_ERR(data->triminfo_clk))
		clk_disable(data->triminfo_clk);
	mutex_unlock(&data->lock);

	return ret;
}

static void exynos_tmu_control(struct platform_device *pdev, bool on)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int con, interrupt_en;

	mutex_lock(&data->lock);
	clk_enable(data->clk);

	con = pdata->reference_voltage << EXYNOS_TMU_REF_VOLTAGE_SHIFT |
		pdata->gain << EXYNOS_TMU_GAIN_SHIFT;

	if (data->soc >= SOC_ARCH_EXYNOS) {
		con |= pdata->noise_cancel_mode << EXYNOS_TMU_TRIP_MODE_SHIFT;
		con |= EXYNOS_MUX_ADDR_VALUE << EXYNOS_MUX_ADDR_SHIFT;

		/* enable HW Trigger (must set multiple bits to enable) */
		con |= EXYNOS_TMU_TRIP_EN;
	}

	if (on) {
		con |= EXYNOS_TMU_CORE_ON;
		interrupt_en = pdata->trigger_level3_en << 12 |
			pdata->trigger_level2_en << 8 |
			pdata->trigger_level1_en << 4 |
			pdata->trigger_level0_en;
		if (pdata->threshold_falling)
			interrupt_en |= interrupt_en << 16;
	} else {
		con |= EXYNOS_TMU_CORE_OFF;
		interrupt_en = 0; /* Disable all interrupts */
	}

	writel(interrupt_en, data->base + EXYNOS_TMU_REG_INTEN);
	writel(con, data->base + EXYNOS_TMU_REG_CONTROL);

	if (data->soc >= SOC_ARCH_EXYNOS) {
		/* enable overtemp HW poweroff.
		 * Enable this *after* thresholds have been set.
		 */
		con = readl(EXYNOS5_PS_HOLD_CONTROL);
		con |= EXYNOS_TMU_HWTRIG_ENABLE;
		writel(con, EXYNOS5_PS_HOLD_CONTROL);
	}

	clk_disable(data->clk);
	mutex_unlock(&data->lock);
}

static int exynos_tmu_read(struct exynos_tmu_data *data)
{
	u8 temp_code;
	int temp;

	mutex_lock(&data->lock);
	clk_enable(data->clk);

	temp_code = readb(data->base + EXYNOS_TMU_REG_CURRENT_TEMP);
	temp = code_to_temp(data, temp_code);

	clk_disable(data->clk);
	mutex_unlock(&data->lock);

	return temp;
}

static void exynos_tmu_work(struct work_struct *work)
{
	struct exynos_tmu_data *data = container_of(work,
			struct exynos_tmu_data, irq_work);

	exynos_report_trigger(data->th_zone);
	mutex_lock(&data->lock);
	clk_enable(data->clk);
	if (data->soc == SOC_ARCH_EXYNOS)
		writel(EXYNOS_TMU_CLEAR_RISE_INT |
				EXYNOS_TMU_CLEAR_FALL_INT,
				data->base + EXYNOS_TMU_REG_INTCLEAR);
	else if (data->soc == SOC_ARCH_EXYNOS5420)
		writel(EXYNOS_TMU_CLEAR_RISE_INT |
				EXYNOS5420_TMU_CLEAR_FALL_INT,
				data->base + EXYNOS_TMU_REG_INTCLEAR);
	else
		writel(EXYNOS4210_TMU_INTCLEAR_VAL,
				data->base + EXYNOS_TMU_REG_INTCLEAR);
	clk_disable(data->clk);
	mutex_unlock(&data->lock);
	enable_irq(data->irq);
}

static irqreturn_t exynos_tmu_irq(int irq, void *id)
{
	struct exynos_tmu_data *data = id;

	disable_irq_nosync(irq);
	schedule_work(&data->irq_work);

	return IRQ_HANDLED;
}

#if defined(CONFIG_CPU_EXYNOS4210)
static struct exynos_tmu_platform_data const exynos4210_default_tmu_data = {
	.threshold = 80,
	.trigger_levels[0] = 5,
	.trigger_levels[1] = 20,
	.trigger_levels[2] = 30,
	.trigger_level0_en = 1,
	.trigger_level1_en = 1,
	.trigger_level2_en = 1,
	.trigger_level3_en = 0,
	.gain = 15,
	.reference_voltage = 7,
	.cal_type = TYPE_ONE_POINT_TRIMMING,
	.freq_tab[0] = {
		.freq_clip_max = 800 * 1000,
		.temp_level = 85,
	},
	.freq_tab[1] = {
		.freq_clip_max = 200 * 1000,
		.temp_level = 100,
	},
	.freq_tab_count = 2,
	.type = SOC_ARCH_EXYNOS4210,
};
#define EXYNOS4210_TMU_DRV_DATA (&exynos4210_default_tmu_data)
#else
#define EXYNOS4210_TMU_DRV_DATA (NULL)
#endif

#if defined(CONFIG_SOC_EXYNOS5250) || defined(CONFIG_SOC_EXYNOS4412) || \
	defined(CONFIG_SOC_EXYNOS5420)
static struct exynos_tmu_platform_data const exynos_default_tmu_data = {
	.trigger_levels[0] = 85,	/* slow e.g. 800Mhz */
	.trigger_levels[1] = 103,	/* very slow e.g. 200Mhz */
	.trigger_levels[2] = 108,	/* kernel issues shutdown */
	.trigger_levels[3] = 110,	/* HW poweroff trigger */
	.threshold_falling = 10,
	.trigger_level0_en = 1,
	.trigger_level1_en = 1,
	.trigger_level2_en = 1,
	.trigger_level3_en = 0,		/* HW Trig is enabled differently */
	.gain = 8,
	.reference_voltage = 16,
	.noise_cancel_mode = 4,
	.cal_type = TYPE_ONE_POINT_TRIMMING,
	.efuse_value = 55,
	.freq_tab[0] = {
		.freq_clip_max = 800 * 1000,
		.temp_level = 85,
	},
	.freq_tab[1] = {
		.freq_clip_max = 200 * 1000,
		.temp_level = 103,
	},
	.freq_tab_count = 2,
	.type = SOC_ARCH_EXYNOS,
};
#define EXYNOS_TMU_DRV_DATA (&exynos_default_tmu_data)
static struct exynos_tmu_platform_data const exynos5420_default_tmu_data = {
	.trigger_levels[0] = 85,	/* slow e.g. 800Mhz */
	.trigger_levels[1] = 103,	/* very slow e.g. 200Mhz */
	.trigger_levels[2] = 108,	/* kernel issues shutdown */
	.trigger_levels[3] = 110,	/* HW poweroff trigger */
	.threshold_falling = 10,
	.trigger_level0_en = 1,
	.trigger_level1_en = 1,
	.trigger_level2_en = 1,
	.trigger_level3_en = 0,		/* HW Trig is enabled differently */
	.gain = 8,
	.reference_voltage = 16,
	.noise_cancel_mode = 4,
	.cal_type = TYPE_ONE_POINT_TRIMMING,
	.efuse_value = 55,
	.freq_tab[0] = {
		.freq_clip_max = 700 * 1000,
		.temp_level = 85,
	},
	.freq_tab[1] = {
		.freq_clip_max = 300 * 1000,
		.temp_level = 103,
	},
	.freq_tab_count = 2,
	.type = SOC_ARCH_EXYNOS5420,
};
#define EXYNOS5420_TMU_DRV_DATA (&exynos5420_default_tmu_data)
#else
#define EXYNOS_TMU_DRV_DATA (NULL)
#define EXYNOS5420_TMU_DRV_DATA (NULL)
#endif

#ifdef CONFIG_OF
static const struct of_device_id exynos_tmu_match[] = {
	{
		.compatible = "samsung,exynos4210-tmu",
		.data = (void *)EXYNOS4210_TMU_DRV_DATA,
	},
	{
		.compatible = "samsung,exynos5250-tmu",
		.data = (void *)EXYNOS_TMU_DRV_DATA,
	},
	{
		.compatible = "samsung,exynos5420-tmu",
		.data = (void *)EXYNOS5420_TMU_DRV_DATA,
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_tmu_match);
#else
#define  exynos_tmu_match NULL
#endif

static struct platform_device_id exynos_tmu_driver_ids[] = {
	{
		.name		= "exynos4210-tmu",
		.driver_data    = (kernel_ulong_t)EXYNOS4210_TMU_DRV_DATA,
	},
	{
		.name		= "exynos5250-tmu",
		.driver_data    = (kernel_ulong_t)EXYNOS_TMU_DRV_DATA,
	},
	{
		.name		= "exynos5420-tmu",
		.driver_data    = (kernel_ulong_t)EXYNOS5420_TMU_DRV_DATA,
	},
	{ },
};
MODULE_DEVICE_TABLE(platform, exynos_tmu_driver_ids);

static inline struct  exynos_tmu_platform_data *exynos_get_driver_data(
			struct platform_device *pdev)
{
#ifdef CONFIG_OF
	if (pdev->dev.of_node) {
		const struct of_device_id *match;
		match = of_match_node(exynos_tmu_match, pdev->dev.of_node);
		if (!match)
			return NULL;
		return (struct exynos_tmu_platform_data *) match->data;
	}
#endif
	return (struct exynos_tmu_platform_data *)
			platform_get_device_id(pdev)->driver_data;
}
static int exynos_tmu_probe(struct platform_device *pdev)
{
	struct exynos_tmu_data *data;
	struct exynos_tmu_platform_data *pdata = pdev->dev.platform_data;
	struct thermal_sensor_conf *sensor_conf;
	int ret, i;

	if (!pdata)
		pdata = exynos_get_driver_data(pdev);

	if (!pdata) {
		dev_err(&pdev->dev, "No platform init data supplied.\n");
		return -ENODEV;
	}
	data = devm_kzalloc(&pdev->dev, sizeof(struct exynos_tmu_data),
					GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "Failed to allocate driver structure\n");
		return -ENOMEM;
	}

	sensor_conf = devm_kzalloc(&pdev->dev, sizeof(*sensor_conf),
					GFP_KERNEL);
	if (!sensor_conf) {
		dev_err(&pdev->dev, "Failed to allocate sensor config\n");
		return -ENOMEM;
	}

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0) {
		dev_err(&pdev->dev, "Failed to get platform irq\n");
		return data->irq;
	}

	INIT_WORK(&data->irq_work, exynos_tmu_work);

	data->mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!data->mem) {
		dev_err(&pdev->dev, "Failed to get platform resource\n");
		return -ENOENT;
	}

	data->base = devm_request_and_ioremap(&pdev->dev, data->mem);
	if (!data->base) {
		dev_err(&pdev->dev, "Failed to ioremap memory\n");
		return -ENODEV;
	}

	/* For Exynos5420 The misplaced TERMINFO register address will be
	 * passed from device tree node.
	 *
	 * We cannot use devm_request_and_ioremap, as the base address
	 * over laps with the address space of the other TMU channel.
	 * Check Documentation for details
	 */
	data->triminfo_base = of_iomap(pdev->dev.of_node, 1);

	data->clk = devm_clk_get(&pdev->dev, "tmu_apbif");
	if (IS_ERR(data->clk)) {
		dev_err(&pdev->dev, "Failed to get clock\n");
		ret = PTR_ERR(data->clk);
		goto err_iomap;
	}

	ret = clk_prepare(data->clk);
	if (ret)
		goto err_iomap;

	/*
	 * The misplaced TRIMINFO register may be located on a different
	 * TMU, requiring a seprate clock to be un-gated in order to access
	 * it.
	 */
	data->triminfo_clk = devm_clk_get(&pdev->dev, "tmu_apbif_triminfo");
	if (!IS_ERR(data->triminfo_clk)) {
		ret = clk_prepare(data->triminfo_clk);
		if (ret)
			goto err_clk_1;
	}

	if (pdata->type >= SOC_ARCH_EXYNOS ||
				pdata->type == SOC_ARCH_EXYNOS4210)
		data->soc = pdata->type;
	else {
		ret = -EINVAL;
		dev_err(&pdev->dev, "Platform not supported\n");
		goto err_clk_2;
	}

	data->pdata = pdata;
	platform_set_drvdata(pdev, data);
	mutex_init(&data->lock);

	ret = exynos_tmu_initialize(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize TMU\n");
		goto err_clk_2;
	}

	/* Register the sensor with thermal management interface */
	snprintf(sensor_conf->name, sizeof(sensor_conf->name),
			"exynos_therm%d", thermal_sensor_count++);
	sensor_conf->read_temperature = (int (*)(void *))exynos_tmu_read;
	sensor_conf->private_data = data;
	sensor_conf->trip_data.trip_count = pdata->trigger_level0_en +
			pdata->trigger_level1_en + pdata->trigger_level2_en +
			pdata->trigger_level3_en;

	for (i = 0; i < sensor_conf->trip_data.trip_count; i++)
		sensor_conf->trip_data.trip_val[i] =
			pdata->threshold + pdata->trigger_levels[i];

	sensor_conf->trip_data.trigger_falling = pdata->threshold_falling;

	sensor_conf->cooling_data.freq_clip_count = pdata->freq_tab_count;
	for (i = 0; i < pdata->freq_tab_count; i++) {
		sensor_conf->cooling_data.freq_data[i].freq_clip_max =
					pdata->freq_tab[i].freq_clip_max;
		sensor_conf->cooling_data.freq_data[i].temp_level =
					pdata->freq_tab[i].temp_level;
	}

	exynos_tmu_control(pdev, true);

	ret = exynos_register_thermal(sensor_conf, data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register thermal interface\n");
		goto err_clk_2;
	}

	ret = request_irq(data->irq, exynos_tmu_irq, IRQF_TRIGGER_RISING,
			  "exynos-tmu", data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq: %d\n", data->irq);
		goto err_irq;
	}

	return 0;

err_irq:
	exynos_unregister_thermal(data);
err_clk_2:
	if (!IS_ERR(data->triminfo_clk))
		clk_unprepare(data->triminfo_clk);
err_clk_1:
	clk_unprepare(data->clk);
err_iomap:
	if (data->triminfo_base)
		iounmap(data->triminfo_base);

	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int exynos_tmu_remove(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);

	free_irq(data->irq, data);
	exynos_tmu_control(pdev, false);

	exynos_unregister_thermal(data);

	if (data->triminfo_base)
		iounmap(data->triminfo_base);

	clk_unprepare(data->clk);
	if (!IS_ERR(data->triminfo_clk))
		clk_unprepare(data->triminfo_clk);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int exynos_tmu_suspend(struct device *dev)
{
	exynos_tmu_control(to_platform_device(dev), false);

	return 0;
}

static int exynos_tmu_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	exynos_tmu_initialize(pdev);
	exynos_tmu_control(pdev, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(exynos_tmu_pm,
			 exynos_tmu_suspend, exynos_tmu_resume);
#define EXYNOS_TMU_PM	(&exynos_tmu_pm)
#else
#define EXYNOS_TMU_PM	NULL
#endif

static struct platform_driver exynos_tmu_driver = {
	.driver = {
		.name   = "exynos-tmu",
		.owner  = THIS_MODULE,
		.pm     = EXYNOS_TMU_PM,
		.of_match_table = exynos_tmu_match,
	},
	.probe = exynos_tmu_probe,
	.remove	= exynos_tmu_remove,
	.id_table = exynos_tmu_driver_ids,
};

module_platform_driver(exynos_tmu_driver);

MODULE_DESCRIPTION("EXYNOS TMU Driver");
MODULE_AUTHOR("Donggeun Kim <dg77.kim@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:exynos-tmu");
