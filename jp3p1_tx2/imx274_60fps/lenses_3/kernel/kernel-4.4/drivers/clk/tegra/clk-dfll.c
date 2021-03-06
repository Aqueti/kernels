/*
 * clk-dfll.c - Tegra DFLL clock source common code
 *
 * Copyright (C) 2012-2017 NVIDIA Corporation. All rights reserved.
 *
 * Aleksandr Frid <afrid@nvidia.com>
 * Paul Walmsley <pwalmsley@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * This library is for the DVCO and DFLL IP blocks on the Tegra124
 * SoC. These IP blocks together are also known at NVIDIA as
 * "CL-DVFS". To try to avoid confusion, this code refers to them
 * collectively as the "DFLL."
 *
 * The DFLL is a root clocksource which tolerates some amount of
 * supply voltage noise. Tegra124 uses it to clock the fast CPU
 * complex when the target CPU speed is above a particular rate. The
 * DFLL can be operated in either open-loop mode or closed-loop mode.
 * In open-loop mode, the DFLL generates an output clock appropriate
 * to the supply voltage. In closed-loop mode, when configured with a
 * target frequency, the DFLL minimizes supply voltage while
 * delivering an average frequency equal to the target.
 *
 * Devices clocked by the DFLL must be able to tolerate frequency
 * variation. In the case of the CPU, it's important to note that the
 * CPU cycle time will vary. This has implications for
 * performance-measurement code and any code that relies on the CPU
 * cycle time to delay for a certain length of time.
 *
 */

#include <dt-bindings/thermal/tegra210-dfll-trips.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <soc/tegra/cvb.h>
#include <soc/tegra/pwm-tegra-dfll.h>
#include <soc/tegra/tegra-dfll.h>
#include <soc/tegra/tegra-dvfs.h>

#include "clk-dfll.h"

/*
 * DFLL control registers - access via dfll_{readl,writel}
 */

/* DFLL_CTRL: DFLL control register */
#define DFLL_CTRL			0x00
#define DFLL_CTRL_MODE_MASK		0x03

/* DFLL_CONFIG: DFLL sample rate control */
#define DFLL_CONFIG			0x04
#define DFLL_CONFIG_DIV_MASK		0xff
#define DFLL_CONFIG_DIV_PRESCALE	32

/* DFLL_PARAMS: tuning coefficients for closed loop integrator */
#define DFLL_PARAMS			0x08
#define DFLL_PARAMS_CG_SCALE		(0x1 << 24)
#define DFLL_PARAMS_FORCE_MODE_SHIFT	22
#define DFLL_PARAMS_FORCE_MODE_MASK	(0x3 << DFLL_PARAMS_FORCE_MODE_SHIFT)
#define DFLL_PARAMS_CF_PARAM_SHIFT	16
#define DFLL_PARAMS_CF_PARAM_MASK	(0x3f << DFLL_PARAMS_CF_PARAM_SHIFT)
#define DFLL_PARAMS_CI_PARAM_SHIFT	8
#define DFLL_PARAMS_CI_PARAM_MASK	(0x7 << DFLL_PARAMS_CI_PARAM_SHIFT)
#define DFLL_PARAMS_CG_PARAM_SHIFT	0
#define DFLL_PARAMS_CG_PARAM_MASK	(0xff << DFLL_PARAMS_CG_PARAM_SHIFT)

/* DFLL_TUNE0: delay line configuration register 0 */
#define DFLL_TUNE0			0x0c

/* DFLL_TUNE1: delay line configuration register 1 */
#define DFLL_TUNE1			0x10

/* DFLL_FREQ_REQ: target DFLL frequency control */
#define DFLL_FREQ_REQ			0x14
#define DFLL_FREQ_REQ_FORCE_ENABLE	(0x1 << 28)
#define DFLL_FREQ_REQ_FORCE_SHIFT	16
#define DFLL_FREQ_REQ_FORCE_MASK	(0xfff << DFLL_FREQ_REQ_FORCE_SHIFT)
#define FORCE_MAX			2047
#define FORCE_MIN			-2048
#define DFLL_FREQ_REQ_SCALE_SHIFT	8
#define DFLL_FREQ_REQ_SCALE_MASK	(0xff << DFLL_FREQ_REQ_SCALE_SHIFT)
#define DFLL_FREQ_REQ_SCALE_MAX		256
#define DFLL_FREQ_REQ_FREQ_VALID	(0x1 << 7)
#define DFLL_FREQ_REQ_MULT_SHIFT	0
#define DFLL_FREQ_REQ_MULT_MASK		(0x7f << DFLL_FREQ_REQ_MULT_SHIFT)
#define FREQ_MAX			127

/* DFLL_DROOP_CTRL: droop prevention control */
#define DFLL_DROOP_CTRL			0x1c

/* DFLL_OUTPUT_CFG: closed loop mode control registers */
/* NOTE: access via dfll_i2c_{readl,writel} */
#define DFLL_OUTPUT_CFG			0x20
#define DFLL_OUTPUT_CFG_I2C_ENABLE	(0x1 << 30)
#define OUT_MASK			0x3f
#define DFLL_OUTPUT_CFG_SAFE_SHIFT	24
#define DFLL_OUTPUT_CFG_SAFE_MASK	\
		(OUT_MASK << DFLL_OUTPUT_CFG_SAFE_SHIFT)
#define DFLL_OUTPUT_CFG_MAX_SHIFT	16
#define DFLL_OUTPUT_CFG_MAX_MASK	\
		(OUT_MASK << DFLL_OUTPUT_CFG_MAX_SHIFT)
#define DFLL_OUTPUT_CFG_MIN_SHIFT	8
#define DFLL_OUTPUT_CFG_MIN_MASK	\
		(OUT_MASK << DFLL_OUTPUT_CFG_MIN_SHIFT)
#define DFLL_OUTPUT_CFG_PWM_DELTA	(0x1 << 7)
#define DFLL_OUTPUT_CFG_PWM_ENABLE	(0x1 << 6)
#define DFLL_OUTPUT_CFG_PWM_DIV_SHIFT	0
#define DFLL_OUTPUT_CFG_PWM_DIV_MASK	\
		(OUT_MASK << DFLL_OUTPUT_CFG_PWM_DIV_SHIFT)

/* DFLL_OUTPUT_FORCE: closed loop mode voltage forcing control */
#define DFLL_OUTPUT_FORCE		0x24
#define DFLL_OUTPUT_FORCE_ENABLE	(0x1 << 6)
#define DFLL_OUTPUT_FORCE_VALUE_SHIFT	0
#define DFLL_OUTPUT_FORCE_VALUE_MASK	\
		(OUT_MASK << DFLL_OUTPUT_FORCE_VALUE_SHIFT)

/* DFLL_MONITOR_CTRL: internal monitor data source control */
#define DFLL_MONITOR_CTRL		0x28

/* DFLL_MONITOR_DATA: internal monitor data output */
#define DFLL_MONITOR_DATA		0x2c
#define DFLL_MONITOR_DATA_NEW_MASK	(0x1 << 16)
#define DFLL_MONITOR_DATA_VAL_SHIFT	0
#define DFLL_MONITOR_DATA_VAL_MASK	(0xFFFF << DFLL_MONITOR_DATA_VAL_SHIFT)

/*
 * I2C output control registers - access via dfll_i2c_{readl,writel}
 */

/* DFLL_I2C_CFG: I2C controller configuration register */
#define DFLL_I2C_CFG			0x40
#define DFLL_I2C_CFG_ARB_ENABLE		(0x1 << 20)
#define DFLL_I2C_CFG_HS_CODE_SHIFT	16
#define DFLL_I2C_CFG_HS_CODE_MASK	(0x7 << DFLL_I2C_CFG_HS_CODE_SHIFT)
#define DFLL_I2C_CFG_PACKET_ENABLE	(0x1 << 15)
#define DFLL_I2C_CFG_SIZE_SHIFT		12
#define DFLL_I2C_CFG_SIZE_MASK		(0x7 << DFLL_I2C_CFG_SIZE_SHIFT)
#define DFLL_I2C_CFG_SLAVE_ADDR_10	(0x1 << 10)
#define DFLL_I2C_CFG_SLAVE_ADDR_SHIFT_7BIT	1
#define DFLL_I2C_CFG_SLAVE_ADDR_SHIFT_10BIT	0

/* DFLL_I2C_VDD_REG_ADDR: PMIC I2C address for closed loop mode */
#define DFLL_I2C_VDD_REG_ADDR		0x44

/* DFLL_I2C_STS: I2C controller status */
#define DFLL_I2C_STS			0x48
#define DFLL_I2C_STS_I2C_LAST_SHIFT	1
#define DFLL_I2C_STS_I2C_REQ_PENDING	0x1

/* DFLL_INTR_STS: DFLL interrupt status register */
#define DFLL_INTR_STS			0x5c

/* DFLL_INTR_EN: DFLL interrupt enable register */
#define DFLL_INTR_EN			0x60
#define DFLL_INTR_MIN_MASK		0x1
#define DFLL_INTR_MAX_MASK		0x2

#define DFLL_CC4_HVC			0x74
#define DFLL_CC4_HVC_CTRL_SHIFT		0
#define DFLL_CC4_HVC_CTRL_MASK		(0x3 << DFLL_CC4_HVC_CTRL_SHIFT)
#define DFLL_CC4_HVC_FORCE_VAL_SHIFT	2
#define DFLL_CC4_HVC_FORCE_VAL_MASK \
	(OUT_MASK << DFLL_CC4_HVC_FORCE_VAL_SHIFT)
#define DFLL_CC4_HVC_FORCE_EN		(0x1 << 8)

/*
 * Integrated I2C controller registers - relative to td->i2c_controller_base
 */

/* DFLL_I2C_CLK_DIVISOR: I2C controller clock divisor */
#define DFLL_I2C_CLK_DIVISOR		0x6c
#define DFLL_I2C_CLK_DIVISOR_MASK	0xffff
#define DFLL_I2C_CLK_DIVISOR_FS_SHIFT	16
#define DFLL_I2C_CLK_DIVISOR_HS_SHIFT	0
#define DFLL_I2C_CLK_DIVISOR_PREDIV	8
#define DFLL_I2C_CLK_DIVISOR_HSMODE_PREDIV	12

/*
 * Other constants
 */

/* MAX_DFLL_VOLTAGES: number of LUT entries in the DFLL IP block */
#define MAX_DFLL_VOLTAGES		33

/*
 * REF_CLK_CYC_PER_DVCO_SAMPLE: the number of ref_clk cycles that the hardware
 *    integrates the DVCO counter over - used for debug rate monitoring and
 *    droop control
 */
#define REF_CLK_CYC_PER_DVCO_SAMPLE	4

/*
 * DFLL_OUTPUT_RAMP_DELAY: after the DFLL's I2C PMIC output starts
 * requesting the minimum TUNE_HIGH voltage, the minimum number of
 * microseconds to wait for the PMIC's voltage output to finish
 * slewing
 */
#define DFLL_OUTPUT_RAMP_DELAY		100

/*
 * DFLL_TUNE_HIGH_DELAY: number of microseconds to wait between tests
 * to see if the high voltage has been reached yet, during a
 * transition from the low-voltage range to the high-voltage range
 */
#define DFLL_TUNE_HIGH_DELAY		2000

/*
 * DFLL_TUNE_HIGH_MARGIN_STEPS: attempt to initially program the DFLL
 * voltage target to a (DFLL_TUNE_HIGH_MARGIN_STEPS * 10 millivolt)
 * margin above the high voltage floor, in closed-loop mode in the
 * high-voltage range
 */
#define DFLL_TUNE_HIGH_MARGIN_STEPS	3

/*
 * DFLL_CAP_GUARD_BAND_STEPS: the minimum volage is at least
 * DFLL_CAP_GUARD_BAND_STEPS below maximum voltage
 */
#define DFLL_CAP_GUARD_BAND_STEPS	2

/*
 * I2C_OUTPUT_ACTIVE_TEST_US: mandatory minimum interval (in
 * microseconds) between testing whether the I2C controller is
 * currently sending a voltage-set command.  Some comments list this
 * as being a worst-case margin for "disable propagation."
 */
#define I2C_OUTPUT_ACTIVE_TEST_US	2

/*
 * REF_CLOCK_RATE: the DFLL reference clock rate currently supported by this
 * driver, in Hz
 */
#define REF_CLOCK_RATE			51000000UL

/*
 * DFLL_CALIBR_TIME: number of microseconds to wait between tests
 * to see if the DVCO rate at Vmin changed.
 */
#define DFLL_CALIBR_TIME		40000

/*
 * DFLL_ONE_SHOT_CALIBR_DELAY: number of microseconds to wait after target
 * voltage is set before starting one-shot calibration
 */
#define DFLL_ONE_SHOT_CALIBR_DELAY	200

/* DFLL_ONE_SHOT_AVG_SAMPLES: number of samples for one-shot calibration */
#define DFLL_ONE_SHOT_AVG_SAMPLES	5

#define DVCO_RATE_TO_MULT(rate, ref_rate)	((rate) / ((ref_rate) / 2))
#define MULT_TO_DVCO_RATE(mult, ref_rate)	((mult) * ((ref_rate) / 2))
#define ROUND_DVCO_MIN_RATE(rate, ref_rate)	\
	(DIV_ROUND_UP(rate, (ref_rate) / 2) * ((ref_rate) / 2))

/*
 * DT configuration flags
 */
#define DFLL_CALIBRATE_FORCE_VMIN	BIT(0)
#define DFLL_DEFER_FORCE_CALIBRATE	BIT(1)
#define DFLL_ONE_SHOT_CALIBRATE		BIT(2)
#define DFLL_HAS_IDLE_OVERRIDE		BIT(3)

/**
 * enum dfll_ctrl_mode - DFLL hardware operating mode
 * @DFLL_UNINITIALIZED: (uninitialized state - not in hardware bitfield)
 * @DFLL_DISABLED: DFLL not generating an output clock
 * @DFLL_OPEN_LOOP: DVCO running, but DFLL not adjusting voltage
 * @DFLL_CLOSED_LOOP: DVCO running, and DFLL adjusting voltage to match
 *		      the requested rate
 *
 * The integer corresponding to the last two states, minus one, is
 * written to the DFLL hardware to change operating modes.
 */
enum dfll_ctrl_mode {
	DFLL_UNINITIALIZED = 0,
	DFLL_DISABLED = 1,
	DFLL_OPEN_LOOP = 2,
	DFLL_CLOSED_LOOP = 3,
};

/**
 * enum dfll_tune_range - voltage range that the driver believes it's in
 * @DFLL_TUNE_UNINITIALIZED: DFLL tuning not yet programmed
 * @DFLL_TUNE_LOW: DFLL in the low-voltage range (or open-loop mode)
 * @DFLL_TUNE_WAIT_DFLL: waiting for DFLL voltage output to reach high
 * @DFLL_TUNE_WAIT_PMIC: waiting for PMIC to react to DFLL output
 * @DFLL_TUNE_HIGH: DFLL in the high-voltage range
 *
 * Some DFLL tuning parameters may need to change depending on the
 * DVCO's voltage; these states represent the ranges that the driver
 * supports. These are software states; these values are never
 * written into registers.
 */
enum dfll_tune_range {
	DFLL_TUNE_UNINITIALIZED = 0,
	DFLL_TUNE_LOW,
	DFLL_TUNE_WAIT_DFLL,
	DFLL_TUNE_WAIT_PMIC,
	DFLL_TUNE_HIGH,
};

enum tegra_dfll_pmu_if {
	TEGRA_DFLL_PMU_I2C = 0,
	TEGRA_DFLL_PMU_PWM = 1,
};

/**
 * struct dfll_rate_req - target DFLL rate request data
 * @rate: target frequency, after the postscaling
 * @dvco_target_rate: target frequency, after the postscaling
 * @lut_index: LUT index at which voltage the dvco_target_rate will be reached
 * @mult_bits: value to program to the MULT bits of the DFLL_FREQ_REQ register
 * @scale_bits: value to program to the SCALE bits of the DFLL_FREQ_REQ register
 */
struct dfll_rate_req {
	unsigned long rate;
	unsigned long dvco_target_rate;
	int lut_index;
	u8 mult_bits;
	u8 scale_bits;
};

struct tegra_dfll {
	struct device			*dev;
	struct tegra_dfll_soc_data	*soc;

	void __iomem			*base;
	void __iomem			*i2c_base;
	void __iomem			*i2c_controller_base;
	void __iomem			*lut_base;

	struct regulator		*vdd_reg;
	struct clk			*soc_clk;
	struct clk			*ref_clk;
	struct clk			*i2c_clk;
	struct clk			*dfll_clk;
	struct clk			*cclk_g_clk;
	struct reset_control		*dvco_rst;
	unsigned long			ref_rate;
	unsigned long			i2c_clk_rate;
	unsigned long			dvco_rate_min;
	unsigned long			out_rate_min;
	unsigned long			out_rate_max;

	enum dfll_ctrl_mode		mode;
	enum dfll_ctrl_mode		resume_mode;
	enum dfll_tune_range		tune_range;
	struct dentry			*debugfs_dir;
	struct clk_hw			dfll_clk_hw;
	const char			*output_clock_name;
	struct dfll_rate_req		last_req;
	unsigned long			last_unrounded_rate;

	struct timer_list		tune_timer;
	unsigned long			tune_delay;

	/* Parameters from DT */
	u32				droop_ctrl;
	u32				sample_rate;
	u32				force_mode;
	u32				cf;
	u32				ci;
	s32				cg;
	bool				cg_scale;
	u32				reg_init_uV;
	u32				cfg_flags;

	/* I2C interface parameters */
	u32				i2c_fs_rate;
	u32				i2c_reg;
	u32				i2c_slave_addr;

	/* i2c_lut array entries are regulator framework selectors */
	unsigned			lut[MAX_DFLL_VOLTAGES];
	unsigned			lut_uv[MAX_DFLL_VOLTAGES];
	int				lut_size;
	u8				lut_bottom, lut_min, lut_max, lut_safe;
	u8				lut_force_min;

	/* tuning parameters */
	u8				tune_high_out_start;
	u8				tune_high_out_min;
	u8				tune_out_last;
	unsigned long			tune_high_dvco_rate_min;
	unsigned long 			tune_high_target_rate_min;
	bool				tune_high_calibrated;

	/* PWM interface */
	enum tegra_dfll_pmu_if		pmu_if;

	/* Thermal parameters */
	unsigned int			thermal_floor_output;
	unsigned int			thermal_floor_index;
	unsigned int			thermal_cap_output;
	unsigned int			thermal_cap_index;
	unsigned long			*dvco_rate_floors;

	/* spinlock protecting register accesses */
	spinlock_t			lock;

	/* PMIC undershoot */
	int				pmu_undershoot_gb;

	/* Vmin calibration */
	struct timer_list		calibration_timer;
	unsigned long			calibration_delay;
	ktime_t				last_calibration;
	unsigned long			calibration_range_min;
	unsigned long			calibration_range_max;

	/* Child cclk_g rate change notifier */
	struct notifier_block		cclk_g_parent_nb;
};

enum dfll_monitor_mode {
	DFLL_OUTPUT_VALUE = 5,
	DFLL_FREQ = 6,
};

static struct tegra_dfll *tegra_dfll_dev;

#define clk_hw_to_dfll(_hw) container_of(_hw, struct tegra_dfll, dfll_clk_hw)
#define cclk_g_nb_to_dfll(_nb) \
	container_of(_nb, struct tegra_dfll, cclk_g_parent_nb)

/* mode_name: map numeric DFLL modes to names for friendly console messages */
static const char * const mode_name[] = {
	[DFLL_UNINITIALIZED] = "uninitialized",
	[DFLL_DISABLED] = "disabled",
	[DFLL_OPEN_LOOP] = "open_loop",
	[DFLL_CLOSED_LOOP] = "closed_loop",
};

static void dfll_set_output_limits(struct tegra_dfll *td,
			u32 out_min, u32 out_max);
static void dfll_load_i2c_lut(struct tegra_dfll *td);
static u8 find_mv_out_cap(struct tegra_dfll *td, int mv);
static u8 find_mv_out_floor(struct tegra_dfll *td, int mv);

/*
 * Register accessors
 */

static inline u32 dfll_readl(struct tegra_dfll *td, u32 offs)
{
	return __raw_readl(td->base + offs);
}

static inline void dfll_writel(struct tegra_dfll *td, u32 val, u32 offs)
{
	__raw_writel(val, td->base + offs);
}

static inline void dfll_wmb(struct tegra_dfll *td)
{
	dfll_readl(td, DFLL_CTRL);
}

/* I2C output control registers - for addresses above DFLL_I2C_CFG */

static inline u32 dfll_i2c_readl(struct tegra_dfll *td, u32 offs)
{
	return __raw_readl(td->i2c_base + offs);
}

static inline void dfll_i2c_writel(struct tegra_dfll *td, u32 val, u32 offs)
{
	__raw_writel(val, td->i2c_base + offs);
}

static inline void dfll_i2c_wmb(struct tegra_dfll *td)
{
	dfll_i2c_readl(td, DFLL_I2C_CFG);
}

static inline void dfll_set_monitor_mode(struct tegra_dfll *td,
					 enum dfll_monitor_mode mode)
{
	dfll_writel(td, mode, DFLL_MONITOR_CTRL);
	dfll_wmb(td);
	udelay(1);
}

/**
 * is_output_i2c_req_pending - is an I2C voltage-set command in progress?
 * @pdev: DFLL instance
 *
 * Returns 1 if an I2C request is in progress, or 0 if not.  The DFLL
 * IP block requires two back-to-back reads of the I2C_REQ_PENDING
 * field to return 0 before the software can be sure that no I2C
 * request is currently pending.  Also, a minimum time interval
 * between DFLL_I2C_STS reads is required by the IP block.
 */
static int is_output_i2c_req_pending(struct tegra_dfll *td)
{
	u32 sts;

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
		return 0;

	sts = dfll_i2c_readl(td, DFLL_I2C_STS);
	if (sts & DFLL_I2C_STS_I2C_REQ_PENDING)
		return 1;

	udelay(I2C_OUTPUT_ACTIVE_TEST_US);

	sts = dfll_i2c_readl(td, DFLL_I2C_STS);
	if (sts & DFLL_I2C_STS_I2C_REQ_PENDING)
		return 1;

	return 0;
}

static u8 dfll_get_output_min(struct tegra_dfll *td)
{
	u32 tune_min;

	tune_min = td->tune_range == DFLL_TUNE_LOW ?
			td->lut_bottom : td->tune_high_out_min;
	return max(tune_min, td->thermal_floor_output);
}

static void set_force_out_min(struct tegra_dfll *td)
{
	u8 lut_force_min;
	int force_mv_min = td->pmu_undershoot_gb;

	if (!force_mv_min)
		return;

	lut_force_min = dfll_get_output_min(td);
	force_mv_min += td->lut_uv[lut_force_min];
	lut_force_min = find_mv_out_cap(td, force_mv_min);
	if (lut_force_min == td->lut_safe)
		lut_force_min++;
	td->lut_force_min = lut_force_min;
}

/**
 * dfll_is_running - is the DFLL currently generating a clock?
 * @td: DFLL instance
 *
 * If the DFLL is currently generating an output clock signal, return
 * true; otherwise return false.
 */
static bool dfll_is_running(struct tegra_dfll *td)
{
	return td->mode >= DFLL_OPEN_LOOP;
}

/*
 * Runtime PM suspend/resume callbacks
 */

/**
 * tegra_dfll_runtime_resume - enable all clocks needed by the DFLL
 * @dev: DFLL device *
 *
 * Enable all clocks needed by the DFLL. Assumes that clk_prepare()
 * has already been called on all the clocks.
 *
 * XXX Should also handle context restore when returning from off.
 */
int tegra_dfll_runtime_resume(struct device *dev)
{
	struct tegra_dfll *td = dev_get_drvdata(dev);
	int ret;

	ret = clk_enable(td->ref_clk);
	if (ret) {
		dev_err(dev, "could not enable ref clock: %d\n", ret);
		return ret;
	}

	ret = clk_enable(td->soc_clk);
	if (ret) {
		dev_err(dev, "could not enable register clock: %d\n", ret);
		clk_disable(td->ref_clk);
		return ret;
	}

	ret = clk_enable(td->i2c_clk);
	if (ret) {
		dev_err(dev, "could not enable i2c clock: %d\n", ret);
		clk_disable(td->soc_clk);
		clk_disable(td->ref_clk);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(tegra_dfll_runtime_resume);

/**
 * tegra_dfll_runtime_suspend - disable all clocks needed by the DFLL
 * @dev: DFLL device *
 *
 * Disable all clocks needed by the DFLL. Assumes that other code
 * will later call clk_unprepare().
 */
int tegra_dfll_runtime_suspend(struct device *dev)
{
	struct tegra_dfll *td = dev_get_drvdata(dev);

	clk_disable(td->ref_clk);
	clk_disable(td->soc_clk);
	clk_disable(td->i2c_clk);

	return 0;
}
EXPORT_SYMBOL(tegra_dfll_runtime_suspend);

/*
 * DFLL tuning operations (per-voltage-range tuning settings)
 */

/**
 * dfll_tune_low - tune to DFLL and CPU settings valid for any voltage
 * @td: DFLL instance
 *
 * Tune the DFLL oscillator parameters and the CPU clock shaper for
 * the low-voltage range. These settings are valid for any voltage,
 * but may not be optimal.
 */
static void dfll_tune_low(struct tegra_dfll *td)
{
	td->tune_range = DFLL_TUNE_LOW;

	dfll_writel(td, td->soc->tune0_low, DFLL_TUNE0);
	dfll_writel(td, td->soc->tune1, DFLL_TUNE1);
	dfll_wmb(td);

	if (td->soc->set_clock_trimmers_low)
		td->soc->set_clock_trimmers_low();
}

/**
 * dfll_tune_high - tune DFLL and CPU clock shaper for high voltages
 * @td: DFLL instance
 *
 * Tune the DFLL oscillator parameters and the CPU clock shaper
 * for the high-voltage range.  The bottom end of the high-voltage
 * range is represented by the index td->soc->tune_high_min_millivolts.
 * Used in closed-loop mode.  No return value.
 */
static void dfll_tune_high(struct tegra_dfll *td)
{
	td->tune_range = DFLL_TUNE_HIGH;

	dfll_writel(td, td->soc->tune0_high, DFLL_TUNE0);
	dfll_writel(td, td->soc->tune1, DFLL_TUNE1);
	dfll_wmb(td);

	if (td->soc->set_clock_trimmers_high)
		td->soc->set_clock_trimmers_high();
}

static enum dfll_tune_range dfll_tune_target(struct tegra_dfll *td,
					     unsigned long rate)
{
	if (rate >= td->tune_high_target_rate_min)
		return DFLL_TUNE_HIGH;

	return DFLL_TUNE_LOW;
}

/**
 * dfll_set_close_loop_config - prepare to switch to closed-loop mode
 * @pdev: DFLL instance
 * @req: requested output rate
 *
 * Prepare to switch the DFLL to closed-loop mode.  This involves
 * switching the DFLL's tuning voltage regime (if necessary), and
 * rewriting the LUT to restrict the minimum and maximum voltages.  No
 * return value.
 */
static void dfll_set_close_loop_config(struct tegra_dfll *td,
			  struct dfll_rate_req *req)
{
	bool sample_tune_out_last = false;
	u8 out_min, out_max;

	switch (td->tune_range) {
	case DFLL_TUNE_LOW:
		if (dfll_tune_target(td, req->rate) > DFLL_TUNE_LOW) {
			td->tune_range = DFLL_TUNE_WAIT_DFLL;
			mod_timer(&td->tune_timer, jiffies + td->tune_delay);
			set_force_out_min(td);
		}
		sample_tune_out_last = true;
		break;

	case DFLL_TUNE_HIGH:
	case DFLL_TUNE_WAIT_DFLL:
	case DFLL_TUNE_WAIT_PMIC:
		if (dfll_tune_target(td, req->rate) == DFLL_TUNE_LOW)
			dfll_tune_low(td);
		set_force_out_min(td);
		break;
	default:
		BUG();
	}

	out_min = dfll_get_output_min(td);

	if (td->thermal_cap_output > out_min + DFLL_CAP_GUARD_BAND_STEPS)
		out_max = td->thermal_cap_output;
	else
		out_max = out_min + DFLL_CAP_GUARD_BAND_STEPS;
	out_max = max(out_max, td->lut_force_min);

	if ((td->lut_min != out_min) || (td->lut_max != out_max)) {
		dfll_set_output_limits(td, out_min, out_max);
		dfll_load_i2c_lut(td);
	}
}

/**
 * dfll_set_open_loop_config - prepare to switch to open-loop mode
 * @td: DFLL instance
 *
 * Prepare to switch the DFLL to open-loop mode. This switches the
 * DFLL to the low-voltage tuning range, ensures that I2C output
 * forcing is disabled, and disables the output clock rate scaler.
 * The DFLL's low-voltage tuning range parameters must be
 * characterized to keep the downstream device stable at any DVCO
 * input voltage. No return value.
 */
static void dfll_set_open_loop_config(struct tegra_dfll *td)
{
	u32 val;
	u32 out_min, out_max;

	out_min = td->lut_min;
	out_max = td->lut_max;
	/* always tune low (safe) in open loop */
	if (td->tune_range != DFLL_TUNE_LOW) {
		dfll_tune_low(td);
		out_min = dfll_get_output_min(td);
	}

	dfll_set_output_limits(td, out_min, out_max);

	val = dfll_readl(td, DFLL_FREQ_REQ);
	val |= DFLL_FREQ_REQ_SCALE_MASK;
	val &= ~DFLL_FREQ_REQ_FORCE_ENABLE;
	dfll_writel(td, val, DFLL_FREQ_REQ);
	dfll_wmb(td);
}

/**
 * dfll_tune_timer_cb - timer callback while tuning low to high
 * @data: struct platform_device * of the DFLL instance
 *
 * Timer callback, used when switching from TUNE_LOW to TUNE_HIGH in
 * closed-loop mode.  Waits for DFLL I2C voltage command output to
 * reach tune_high_out_min, then waits for the PMIC to react to the
 * command.  No return value.
 */
static void dfll_tune_timer_cb(unsigned long data)
{
	struct tegra_dfll *td = (struct tegra_dfll *)data;
	u32 val, out_min, out_last;
	bool use_ramp_delay;

	if (td->tune_range == DFLL_TUNE_WAIT_DFLL) {
		out_min = td->lut_min;

		if (td->pmu_if == TEGRA_DFLL_PMU_I2C) {
			val = dfll_i2c_readl(td, DFLL_I2C_STS);
			out_last = val >> DFLL_I2C_STS_I2C_LAST_SHIFT;
			out_last &= OUT_MASK;
		} else {
			out_last = out_min;
		}

		use_ramp_delay = !is_output_i2c_req_pending(td);
		use_ramp_delay |= td->tune_out_last != out_last;

		if (use_ramp_delay &&
		    (out_last >= td->tune_high_out_min) &&
		    (out_min >= td->tune_high_out_min)) {
			td->tune_range = DFLL_TUNE_WAIT_PMIC;
			mod_timer(&td->tune_timer, jiffies +
				  usecs_to_jiffies(DFLL_OUTPUT_RAMP_DELAY));
		} else {
			mod_timer(&td->tune_timer, jiffies + td->tune_delay);
		}
	} else if (td->tune_range == DFLL_TUNE_WAIT_PMIC) {
		dfll_tune_high(td);
	}
}

/*
 * DVCO rate control
 */

static unsigned long get_dvco_rate_below(struct tegra_dfll *td, u8 out_min)
{
	struct dev_pm_opp *opp;
	unsigned long rate, prev_rate;
	int uv, min_uv;

	min_uv = td->lut_uv[out_min];
	for (rate = 0, prev_rate = 0; ; rate++) {
		rcu_read_lock();
		opp = dev_pm_opp_find_freq_ceil(td->soc->dev, &rate);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			break;
		}
		uv = dev_pm_opp_get_voltage(opp);
		rcu_read_unlock();

		if (uv && uv > min_uv)
			return prev_rate;

		prev_rate = rate;
	}

	return prev_rate;
}

static unsigned long get_dvco_rate_above(struct tegra_dfll *td, u8 out_min)
{
	struct dev_pm_opp *opp;
	unsigned long rate;
	int uv, min_uv;

	min_uv = td->lut_uv[out_min];
	for (rate = 0; ; rate++) {
		rcu_read_lock();
		opp = dev_pm_opp_find_freq_ceil(td->soc->dev, &rate);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			break;
		}
		uv = dev_pm_opp_get_voltage(opp);
		rcu_read_unlock();

		if (uv && uv > min_uv)
			return rate;
	}

	return rate;
}

/**
 * set_dvco_rate_min - set the minimum DVCO output rate & interval
 * @td: DFLL instance
 *
 * Find and cache the "minimum" DVCO output frequency. No return value.
 */
static void set_dvco_rate_min(struct tegra_dfll *td, struct dfll_rate_req *req)
{
	unsigned long rate;
	unsigned long tune_high_range_min = 0;
	unsigned long range = 32 * (td->ref_rate / 2);

	rate = td->dvco_rate_floors[td->thermal_floor_index];
	if (!rate) {
		if (td->thermal_floor_index
			< td->soc->thermal_floor_table_size)
			rate = get_dvco_rate_below(td,
						   td->thermal_floor_output);
		else
			rate = td->out_rate_min;
	}

	if (dfll_tune_target(td, req->rate) > DFLL_TUNE_LOW) {
		rate = max(rate, td->tune_high_dvco_rate_min);
		tune_high_range_min = td->tune_high_target_rate_min;
	}

	/* round minimum rate to request unit (ref_rate/2) boundary */
	td->dvco_rate_min = ROUND_DVCO_MIN_RATE(rate, td->ref_rate);

	/* set symmetrical calibration boundaries */
	td->calibration_range_min = td->dvco_rate_min > range ?
		td->dvco_rate_min - range : 0;
	if (td->calibration_range_min < tune_high_range_min)
		td->calibration_range_min = tune_high_range_min;
	if (td->calibration_range_min < td->out_rate_min)
		td->calibration_range_min = td->out_rate_min;

	td->calibration_range_max = td->dvco_rate_min + range;
	if (td->calibration_range_max > td->out_rate_max)
		td->calibration_range_max = td->out_rate_max;
}

/*
 * DFLL mode switching
 */

/**
 * dfll_set_mode - change the DFLL control mode
 * @td: DFLL instance
 * @mode: DFLL control mode (see enum dfll_ctrl_mode)
 *
 * Change the DFLL's operating mode between disabled, open-loop mode,
 * and closed-loop mode, or vice versa.
 */
static void dfll_set_mode(struct tegra_dfll *td,
			  enum dfll_ctrl_mode mode)
{
	td->mode = mode;
	dfll_writel(td, mode - 1, DFLL_CTRL);

	if (td->cfg_flags & DFLL_HAS_IDLE_OVERRIDE) {
		/* Override mode follows active mode up to open loop */
		u32 val = dfll_readl(td, DFLL_CC4_HVC);
		val &= ~(DFLL_CC4_HVC_CTRL_MASK | DFLL_CC4_HVC_FORCE_EN);
		if (mode >= DFLL_OPEN_LOOP) {
			val |= DFLL_OPEN_LOOP - 1;
			val |= DFLL_CC4_HVC_FORCE_EN;
		}
		dfll_writel(td, val, DFLL_CC4_HVC);
	}
	dfll_wmb(td);
}

/*
 * DFLL-to-I2C controller interface
 */

/**
 * dfll_i2c_set_output_enabled - enable/disable I2C PMIC voltage requests
 * @td: DFLL instance
 * @enable: whether to enable or disable the I2C voltage requests
 *
 * Set the master enable control for I2C control value updates. If disabled,
 * then I2C control messages are inhibited, regardless of the DFLL mode.
 */
static int dfll_i2c_set_output_enabled(struct tegra_dfll *td, bool enable)
{
	u32 val;

	val = dfll_i2c_readl(td, DFLL_OUTPUT_CFG);

	if (enable)
		val |= DFLL_OUTPUT_CFG_I2C_ENABLE;
	else
		val &= ~DFLL_OUTPUT_CFG_I2C_ENABLE;

	dfll_i2c_writel(td, val, DFLL_OUTPUT_CFG);
	dfll_i2c_wmb(td);

	return 0;
}

/**
 * dfll_set_output_limits - set DFLL output min and max limits
 * @td: DFLL instance
 * @out_min: min lut index
 * @out_max: max lut index
 *
 * Set the minimum and maximum lut index into DFLL output config
 * register.
 */
static void dfll_set_output_limits(struct tegra_dfll *td,
			u32 out_min, u32 out_max)
{
	u32 val;

	td->lut_min = out_min;
	td->lut_max = out_max;

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
		val = dfll_readl(td, DFLL_OUTPUT_CFG);
	else
		val = dfll_i2c_readl(td, DFLL_OUTPUT_CFG);

	val &= ~DFLL_OUTPUT_CFG_MAX_MASK &
		~DFLL_OUTPUT_CFG_MIN_MASK;

	val |= (td->lut_max << DFLL_OUTPUT_CFG_MAX_SHIFT) |
		(td->lut_min << DFLL_OUTPUT_CFG_MIN_SHIFT);

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM) {
		dfll_writel(td, val, DFLL_OUTPUT_CFG);
		dfll_wmb(td);
	} else {
		dfll_i2c_writel(td, val, DFLL_OUTPUT_CFG);
		dfll_i2c_wmb(td);
	}

	if (td->cfg_flags & DFLL_HAS_IDLE_OVERRIDE) {
		/* Override mode force value follows active mode Vmin */
		val = dfll_readl(td, DFLL_CC4_HVC);
		val &= ~DFLL_CC4_HVC_FORCE_VAL_MASK;
		val |= td->lut_min << DFLL_CC4_HVC_FORCE_VAL_SHIFT;
		dfll_writel(td, val, DFLL_CC4_HVC);
		dfll_wmb(td);
	}
}

/**
 * dfll_load_i2c_lut - load the voltage lookup table
 * @td: struct tegra_dfll *
 *
 * Load the voltage-to-PMIC register value lookup table into the DFLL
 * IP block memory. Look-up tables can be loaded at any time.
 */
static void dfll_load_i2c_lut(struct tegra_dfll *td)
{
	int i, lut_index;
	u32 val;

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
		return;

	for (i = 0; i < MAX_DFLL_VOLTAGES; i++) {
		if (i < td->lut_min)
			lut_index = td->lut_min;
		else if (i > td->lut_max)
			lut_index = td->lut_max;
		else
			lut_index = i;

		val = regulator_list_hardware_vsel(td->vdd_reg,
						     td->lut[lut_index]);
		__raw_writel(val, td->lut_base + i * 4);
	}

	dfll_i2c_wmb(td);
}

/**
 * dfll_init_i2c_if - set up the DFLL's DFLL-I2C interface
 * @td: DFLL instance
 *
 * During DFLL driver initialization, program the DFLL-I2C interface
 * with the PMU slave address, vdd register offset, and transfer mode.
 * This data is used by the DFLL to automatically construct I2C
 * voltage-set commands, which are then passed to the DFLL's internal
 * I2C controller.
 */
static void dfll_init_i2c_if(struct tegra_dfll *td)
{
	u32 val;

	if (td->i2c_slave_addr > 0x7f) {
		val = td->i2c_slave_addr << DFLL_I2C_CFG_SLAVE_ADDR_SHIFT_10BIT;
		val |= DFLL_I2C_CFG_SLAVE_ADDR_10;
	} else {
		val = td->i2c_slave_addr << DFLL_I2C_CFG_SLAVE_ADDR_SHIFT_7BIT;
	}
	val |= DFLL_I2C_CFG_SIZE_MASK;
	val |= DFLL_I2C_CFG_ARB_ENABLE;
	dfll_i2c_writel(td, val, DFLL_I2C_CFG);

	dfll_i2c_writel(td, td->i2c_reg, DFLL_I2C_VDD_REG_ADDR);

	val = DIV_ROUND_UP(td->i2c_clk_rate, td->i2c_fs_rate * 8);
	BUG_ON(!val || (val > DFLL_I2C_CLK_DIVISOR_MASK));
	val = (val - 1) << DFLL_I2C_CLK_DIVISOR_FS_SHIFT;

	/* default hs divisor just in case */
	val |= 1 << DFLL_I2C_CLK_DIVISOR_HS_SHIFT;
	__raw_writel(val, td->i2c_controller_base + DFLL_I2C_CLK_DIVISOR);
	dfll_i2c_wmb(td);
}

/*
 * DFLL-to-PWM controller interface
 */

/**
 * dfll_set_force_output_value - set fixed value for force output
 * @td: DFLL instance
 * @out_val: value to force output
 *
 * Set the fixec value for force output, DFLL will output this value when
 * force output is enabled.
 */
static u32 dfll_set_force_output_value(struct tegra_dfll *td, u8 out_val)
{
	u32 val = dfll_readl(td, DFLL_OUTPUT_FORCE);

	val = (val & DFLL_OUTPUT_FORCE_ENABLE) | (out_val & OUT_MASK);
	dfll_writel(td, val, DFLL_OUTPUT_FORCE);
	dfll_wmb(td);

	return dfll_readl(td, DFLL_OUTPUT_FORCE);
}

/**
 * dfll_set_force_output_enabled - enable/disable force output
 * @td: DFLL instance
 * @enable: whether to enable or disable the force output
 *
 * Set the enable control for fouce output with fixed value.
 */
static void dfll_set_force_output_enabled(struct tegra_dfll *td, bool enable)
{
	u32 val = dfll_readl(td, DFLL_OUTPUT_FORCE);

	if (enable)
		val |= DFLL_OUTPUT_FORCE_ENABLE;
	else
		val &= ~DFLL_OUTPUT_FORCE_ENABLE;

	dfll_writel(td, val, DFLL_OUTPUT_FORCE);
	dfll_wmb(td);
}

/**
 * dfll_i2c_set_output_enabled - enable/disable I2C PMIC voltage requests
 * @td: DFLL instance
 * @enable: whether to enable or disable the I2C voltage requests
 *
 * Set the master enable control for I2C control value updates. If disabled,
 * then I2C control messages are inhibited, regardless of the DFLL mode.
 */
static int dfll_force_output(struct tegra_dfll *td, unsigned int out_sel)
{
	u32 val;

	if (out_sel > OUT_MASK)
		return -EINVAL;

	val = dfll_set_force_output_value(td, out_sel);
	if ((td->mode < DFLL_CLOSED_LOOP) &&
	    !(val & DFLL_OUTPUT_FORCE_ENABLE)) {
		dfll_set_force_output_enabled(td, true);
	}

	return 0;
}

/*
 * Reading monitor data concurrently with the update may render intermediate
 * (neither "old" nor "new") values. Synchronization with the "rising edge"
 * of DATA_NEW makes it very unlikely, but still possible. Use simple filter:
 * compare 2 consecutive readings for data consistency within 2 LSb range.
 * Return error otherwise. On the platform that does not allow to use DATA_NEW
 * at all check for consistency of consecutive reads is the only protection.
 */

static int dfll_wait_monitor_data(struct tegra_dfll *td, u32 *reg)
{
	int sample_period;

	sample_period = DIV_ROUND_UP(1000000, td->sample_rate);

	return readl_relaxed_poll_timeout_atomic(td->base + DFLL_MONITOR_DATA,
					  *reg,
					  *reg & DFLL_MONITOR_DATA_NEW_MASK, 1,
					  sample_period * 2);
}

static int dfll_get_monitor_data(struct tegra_dfll *td, u32 *reg)
{
	u32 val;

	dfll_wait_monitor_data(td, reg);
	*reg &= DFLL_MONITOR_DATA_VAL_MASK;

	val = dfll_readl(td, DFLL_MONITOR_DATA) & DFLL_MONITOR_DATA_VAL_MASK;
	if (abs(*reg - val) <= 2)
		return 0;

	*reg = dfll_readl(td, DFLL_MONITOR_DATA) & DFLL_MONITOR_DATA_VAL_MASK;
	if (abs(*reg - val) <= 2)
		return 0;

	return -EINVAL;
}

/*
 * Calibrate DFLL minimum rate
 */

/**
 * dfll_calc_monitored_rate - convert DFLL_MONITOR_DATA_VAL rate into real freq
 * @monitor_data: value read from the DFLL_MONITOR_DATA_VAL bitfield
 * @ref_rate: DFLL reference clock rate
 *
 * Convert @monitor_data from DFLL_MONITOR_DATA_VAL units into cycles
 * per second. Returns the converted value.
 */
static u64 dfll_calc_monitored_rate(u32 monitor_data,
				    unsigned long ref_rate)
{
	return monitor_data * (ref_rate / REF_CLK_CYC_PER_DVCO_SAMPLE);
}

static inline void calibration_timer_update(struct tegra_dfll *td)
{
	/*
	 * Forced output must be disabled in closed loop mode outside of
	 * calibration. It may be temporarily enabled during calibration;
	 * use timer update to clean up.
	 */
	dfll_set_force_output_enabled(td, false);

	if (td->calibration_delay)
		mod_timer(&td->calibration_timer,
			  jiffies + td->calibration_delay + 1);
}

/*
 * Opportunistic calibrate implements s/w closed loop that updates calibrate
 * targets if DFLL is already at floor voltage because of low frequency request.
 */
static void dfll_calibrate(struct tegra_dfll *td)
{
	u32 val, data;
	ktime_t now;
	unsigned long rate;
	unsigned long step = td->ref_rate / 2;
	unsigned long rate_min = td->dvco_rate_min;
	u8 out_min = dfll_get_output_min(td);

	if (!td->calibration_delay)
		return;
	/*
	 *  Enter calibration procedure only if
	 *  - closed loop operations
	 *  - last request engaged clock skipper
	 *  - at least specified time after the last calibration attempt
	 */
	if ((td->mode != DFLL_CLOSED_LOOP) ||
	    (td->last_req.dvco_target_rate > rate_min))
		return;

	now = ktime_get();
	if (ktime_us_delta(now, td->last_calibration) <
	    jiffies_to_usecs(td->calibration_delay))
		return;

	td->last_calibration = now;

	/* Defer calibration if in the middle of tuning transition */
	if ((td->tune_range > DFLL_TUNE_LOW) &&
	    (td->tune_range < DFLL_TUNE_HIGH)) {
		calibration_timer_update(td);
		return;
	}

	/* Defer calibration if forced output was left enabled */
	val = dfll_readl(td, DFLL_OUTPUT_FORCE);
	if (val & DFLL_OUTPUT_FORCE_ENABLE) {
		calibration_timer_update(td);
		return;
	}

	/*
	 * Check if we need to force minimum output during calibration.
	 *
	 * Considerations for selecting TEGRA_CL_DVFS_CALIBRATE_FORCE_VMIN.
	 * - if there is no voltage enforcement underneath this driver, no need
	 * to select defer option.
	 *
	 *  - if SoC has internal pm controller that controls voltage while CPU
	 * cluster is idle, and restores force_val on idle exit, the following
	 * trade-offs applied:
	 *
	 * a) force: DVCO calibration is accurate, but calibration time is
	 * increased by 2 sample periods and target module maybe under-clocked
	 * during that time,
	 * b) don't force: calibration results depend on whether flag
	 * TEGRA_CL_DVFS_DEFER_FORCE_CALIBRATE is set -- see description below.
	 */
	if (td->cfg_flags & DFLL_CALIBRATE_FORCE_VMIN) {
		int delay = 2 * DIV_ROUND_UP(1000000, td->sample_rate);
		dfll_set_force_output_value(td, out_min);
		dfll_set_force_output_enabled(td, true);
		udelay(delay);
	}

	/* Synchronize with sample period, and get rate measurements */
	dfll_set_monitor_mode(td, DFLL_FREQ);

	/* Defer calibration if data reading is not consistent */
	dfll_get_monitor_data(td, &data);
	if (dfll_get_monitor_data(td, &data) < 0) {
		calibration_timer_update(td);
		return;
	}

	/* Get output (voltage) measurements */
	if (td->pmu_if == TEGRA_DFLL_PMU_I2C) {
		/* Defer calibration if I2C transaction is pending */
		val = dfll_i2c_readl(td, DFLL_I2C_STS);
		if (val & DFLL_I2C_STS_I2C_REQ_PENDING) {
			calibration_timer_update(td);
			return;
		}
		val = (val >> DFLL_I2C_STS_I2C_LAST_SHIFT) & OUT_MASK;
	} else if (td->cfg_flags & DFLL_CALIBRATE_FORCE_VMIN) {
		/* Use forced value (cannot read it back from PWM interface) */
		val = out_min;
	} else {
		/* Get last output (there is no such thing as pending PWM) */
		/* Defer calibration if data reading is not consistent */
		dfll_set_monitor_mode(td, DFLL_OUTPUT_VALUE);
		if (dfll_get_monitor_data(td, &val) < 0) {
			calibration_timer_update(td);
			return;
		}
	}

	if (td->cfg_flags & DFLL_CALIBRATE_FORCE_VMIN) {
		/* Defer calibration if forced and read outputs do not match */
		if (val != out_min) {
			calibration_timer_update(td);
			return;
		}
		dfll_set_force_output_enabled(td, false);
	}

	/*
	 * Check if we need to defer calibration when voltage is matching
	 * request force_val.
	 *
	 * Considerations for selecting TEGRA_CL_DVFS_DEFER_FORCE_CALIBRATE.
	 * - if there is no voltage enforcement underneath this driver, no need
	 * to select defer option.
	 *
	 *  - if SoC has internal pm controller that controls voltage while CPU
	 * cluster is idle, and restores force_val on idle exit, the following
	 * trade-offs applied:
	 *
	 * a) defer: DVCO minimum maybe slightly over-estimated, all frequencies
	 * below DVCO minimum are skipped-to accurately, but voltage at low
	 * frequencies would fluctuate between Vmin and Vmin + 1 LUT/PWM step.
	 * b) don't defer: DVCO minimum rate is underestimated, maybe down to
	 * calibration_range_min, respectively actual frequencies below DVCO
	 * minimum are configured higher than requested, but voltage at low
	 * frequencies is saturated at Vmin.
	 */
	if ((val == td->last_req.lut_index) &&
	    (td->cfg_flags & DFLL_DEFER_FORCE_CALIBRATE)) {
		calibration_timer_update(td);
		return;
	}

	/* Adjust minimum rate */
	rate = dfll_calc_monitored_rate(data, td->ref_rate);
	if ((val > out_min) || (rate < (rate_min - step)))
		rate_min -= step;
	else if (rate > (rate_min + step))
		rate_min += step;
	else {
		int t_floor_out, t_floor_idx;
		struct thermal_tv tv;

		if ((td->tune_range == DFLL_TUNE_HIGH) &&
		    (td->tune_high_out_min == out_min)) {
			td->tune_high_dvco_rate_min = rate_min;
			td->tune_high_calibrated = true;
			return;
		}

		t_floor_idx = td->thermal_floor_index;
		tv = td->soc->thermal_floor_table[t_floor_idx];
		t_floor_out = find_mv_out_cap(td, tv.millivolts);
		if (t_floor_out == out_min) {
			td->dvco_rate_floors[t_floor_idx] = rate_min;
			return;
		}
		calibration_timer_update(td);
		return;
	}

	td->dvco_rate_min = clamp(rate_min,
			td->calibration_range_min, td->calibration_range_max);
	calibration_timer_update(td);
	pr_debug("%s: calibrated dvco_rate_min %lu (%lu), measured %lu\n",
		 __func__, td->dvco_rate_min, rate_min, rate);
}

/*
 * One-shot calibrate forces and calibrates each target floor once when DFLL is
 * locked or when temperature crosses thermal range threshold.
 */
static unsigned long dfll_one_shot_calibrate_mv(struct tegra_dfll *td, int mv)
{
	int i, n = 0;
	u32 data, avg_data = 0;
	unsigned long rate;
	u8 out_min = find_mv_out_cap(td, mv);

	/* Set calibration target voltage */
	dfll_set_force_output_value(td, out_min);
	dfll_set_force_output_enabled(td, true);

	/* Switch to rate measurement, and synchronize with sample period */
	dfll_set_monitor_mode(td, DFLL_FREQ);
	dfll_get_monitor_data(td, &data);
	udelay(DFLL_ONE_SHOT_CALIBR_DELAY);

	/* Average measurements. Take the last one "as is" if all unstable */
	for (i = 0; i < DFLL_ONE_SHOT_AVG_SAMPLES; i++) {
		if (dfll_get_monitor_data(td, &data) < 0) {
			if ((i + 1 < DFLL_ONE_SHOT_AVG_SAMPLES) || n)
				continue;
			dev_err(td->dev, "%s: use unstable monitor output %u\n",
				 __func__, data);
		}
		avg_data += data;
		n++;
	}

	/* Get average monitor rate rounded to request unit (=2*monitor unit) */
	avg_data = DIV_ROUND_CLOSEST(avg_data, 2 * n);
	rate = MULT_TO_DVCO_RATE(avg_data, td->ref_rate);
	pr_debug("%s: calibrated dvco_rate_min %lu at %d mV over %d samples\n",
		 __func__,  rate, mv, n);

	dfll_set_force_output_enabled(td, false);
	return rate;
}

static bool dfll_one_shot_calibrate_floors(struct tegra_dfll *td)
{
	bool ret = false;
	int mv, therm_mv = 0;
	int i = td->thermal_floor_index;

	if (!(td->cfg_flags & DFLL_ONE_SHOT_CALIBRATE) ||
	    (td->mode != DFLL_CLOSED_LOOP) || timekeeping_suspended)
		return ret;

	/* Thermal floors */
	if (td->soc->thermal_floor_table_size &&
	    (i < td->soc->thermal_floor_table_size)) {
		therm_mv = td->soc->thermal_floor_table[i].millivolts;
		if (!td->dvco_rate_floors[i]) {
			td->dvco_rate_floors[i] =
				dfll_one_shot_calibrate_mv(td, therm_mv);
			ret = true;
		}
	}

	/* Tune high Vmin if specified */
	if (!td->tune_high_calibrated &&
	    (td->tune_high_target_rate_min != ULONG_MAX)) {
		mv = td->lut_uv[td->tune_high_out_min] / 1000;
		if (mv > therm_mv) {
			td->tune_high_dvco_rate_min =
				dfll_one_shot_calibrate_mv(td, mv);
			td->tune_high_calibrated = true;
			ret = true;
		}
	}

	/* Absolute Vmin matters only when no thermal floors */
	if (!therm_mv) {
		i = td->soc->thermal_floor_table_size;
		if (!td->dvco_rate_floors[i]) {
			mv = td->lut_uv[td->lut_bottom] / 1000;
			td->out_rate_min = dfll_one_shot_calibrate_mv(td, mv);
			td->dvco_rate_floors[i] = td->out_rate_min;
			ret = true;
		}
	}

	return ret;
}

/**
 * dfll_init_out_if - prepare DFLL-to-PMIC interface
 * @td: DFLL instance
 *
 * During DFLL driver initialization or resume from context loss,
 * it needs to set safe voltage and output limits, and disable and
 * clear limit interrupts. For PWM interface, it needs to set
 * initial voltage for force output to avoid CPU voltage glitch
 * during DFLL is from open to closed loop transition.
 */
static void dfll_init_out_if(struct tegra_dfll *td)
{
	u32 val = 0;
	int index, mv;

	td->thermal_floor_output = 0;
	if (td->soc->thermal_floor_table_size) {
		index = 0;
		mv = td->soc->thermal_floor_table[index].millivolts;
		td->thermal_floor_output = find_mv_out_cap(td, mv);
		td->thermal_floor_index = index;
	}

	td->thermal_cap_output = td->lut_size - 1;
	if (td->soc->thermal_cap_table_size) {
		index = td->soc->thermal_cap_table_size - 1;
		mv = td->soc->thermal_cap_table[index].millivolts;
		td->thermal_cap_output = find_mv_out_floor(td, mv);
		td->thermal_cap_index = index;
	}

	set_dvco_rate_min(td, &td->last_req);
	set_force_out_min(td);

	td->lut_min = td->thermal_floor_output;
	td->lut_max = td->thermal_cap_output;
	td->lut_safe = td->lut_min + 1;

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM) {
		int vinit = td->reg_init_uV;
		int vstep = td->soc->alignment.step_uv;
		int vmin = regulator_list_voltage_unlocked(td->vdd_reg, 0);

		/* clear DFLL_OUTPUT_CFG before setting new value */
		dfll_writel(td, 0, DFLL_OUTPUT_CFG);
		dfll_wmb(td);

		val = dfll_readl(td, DFLL_OUTPUT_CFG);
		val |= (td->lut_safe << DFLL_OUTPUT_CFG_SAFE_SHIFT) |
		       (td->lut_max << DFLL_OUTPUT_CFG_MAX_SHIFT) |
		       (td->lut_min << DFLL_OUTPUT_CFG_MIN_SHIFT);
		dfll_writel(td, val, DFLL_OUTPUT_CFG);
		dfll_wmb(td);

		dfll_writel(td, 0, DFLL_OUTPUT_FORCE);
		dfll_writel(td, 0, DFLL_INTR_EN);
		dfll_writel(td, DFLL_INTR_MAX_MASK | DFLL_INTR_MIN_MASK,
				DFLL_INTR_STS);

		/* set initial voltage */
		if ((vinit >= vmin) && vstep) {
			unsigned int vsel;

			vsel = DIV_ROUND_UP((vinit - vmin), vstep);
			dfll_force_output(td, vsel);
		}
	} else {
		dfll_i2c_writel(td, 0, DFLL_OUTPUT_CFG);
		val |= (td->lut_safe << DFLL_OUTPUT_CFG_SAFE_SHIFT) |
		       (td->lut_max << DFLL_OUTPUT_CFG_MAX_SHIFT) |
		       (td->lut_min << DFLL_OUTPUT_CFG_MIN_SHIFT);
		dfll_i2c_writel(td, val, DFLL_OUTPUT_CFG);
		dfll_i2c_wmb(td);

		dfll_i2c_writel(td, 0, DFLL_OUTPUT_FORCE);
		dfll_i2c_writel(td, 0, DFLL_INTR_EN);
		dfll_i2c_writel(td, DFLL_INTR_MAX_MASK | DFLL_INTR_MIN_MASK,
				DFLL_INTR_STS);

		dfll_load_i2c_lut(td);
		dfll_init_i2c_if(td);
	}

	if (td->cfg_flags & DFLL_HAS_IDLE_OVERRIDE) {
		val = td->lut_min << DFLL_CC4_HVC_FORCE_VAL_SHIFT;
		dfll_writel(td, val, DFLL_CC4_HVC);
		dfll_wmb(td);
	}
}

/*
 * Set/get the DFLL's targeted output clock rate
 */

/**
 * find_lut_index_for_rate - determine I2C LUT index for given DFLL rate
 * @td: DFLL instance
 * @rate: clock rate
 *
 * Determines the index of a I2C LUT entry for a voltage that approximately
 * produces the given DFLL clock rate. This is used when forcing a value
 * to the integrator during rate changes. Returns -ENOENT if a suitable
 * LUT index is not found.
 */
static int find_lut_index_for_rate(struct tegra_dfll *td, unsigned long rate)
{
	struct dev_pm_opp *opp;
	int i, align_volt;

	rcu_read_lock();

	opp = dev_pm_opp_find_freq_ceil(td->soc->dev, &rate);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return PTR_ERR(opp);
	}
	align_volt = dev_pm_opp_get_voltage(opp) / td->soc->alignment.step_uv;

	rcu_read_unlock();

	for (i = td->lut_bottom; i < td->lut_size; i++) {
		if ((td->lut_uv[i] / td->soc->alignment.step_uv) >= align_volt)
			return i;
	}

	return -ENOENT;
}

/**
 * find_mv_out_cap - find the out_map index with voltage >= @mv
 * @td: DFLL instance
 * @mv: millivolts
 *
 * Find the lut index with voltage greater than or equal to @mv,
 * and return it. If all of the voltages in out_map are less than
 * @mv, then return the lut index * corresponding to the highest
 * possible voltage, even though it's less than @mv.
 */
static u8 find_mv_out_cap(struct tegra_dfll *td, int mv)
{
	u8 i;

	for (i = td->lut_bottom; i < td->lut_size; i++) {
		if (td->lut_uv[i] >= mv * 1000)
			return i;
	}

	return i - 1;	/* maximum possible output */
}

/**
 * find_mv_out_floor - find the largest out_map index with voltage < @mv
 * @pdev: DFLL instance
 * @mv: millivolts
 *
 * Find the largest out_map index with voltage lesser to @mv,
 * and return it. If all of the voltages in out_map are greater than
 * @mv, then return the out_map index * corresponding to the minimum
 * possible voltage, even though it's greater than @mv.
 */
static u8 find_mv_out_floor(struct tegra_dfll *td, int mv)
{
	u8 i;

	for (i = td->lut_bottom; i < td->lut_size; i++) {
		if (td->lut_uv[i] > mv * 1000) {
			if (!i)
				/* minimum possible output */
				return 0;
			else
				break;
		}
	}

	return i - 1;
}

/**
 * dfll_calculate_rate_request - calculate DFLL parameters for a given rate
 * @td: DFLL instance
 * @req: DFLL-rate-request structure
 * @rate: the desired DFLL rate
 *
 * Populate the DFLL-rate-request record @req fields with the scale_bits
 * and mult_bits fields, based on the target input rate. Returns 0 upon
 * success, or -EINVAL if the requested rate in req->rate is too high
 * or low for the DFLL to generate.
 */
static int dfll_calculate_rate_request(struct tegra_dfll *td,
				       struct dfll_rate_req *req,
				       unsigned long rate)
{
	u32 val;

	/*
	 * If requested rate is below the minimum DVCO rate, active the scaler.
	 * In the future the DVCO minimum voltage should be selected based on
	 * chip temperature and the actual minimum rate should be calibrated
	 * at runtime.
	 */
	req->scale_bits = DFLL_FREQ_REQ_SCALE_MAX - 1;
	if (rate < td->dvco_rate_min) {
		int scale;

		scale = DIV_ROUND_CLOSEST(rate / 1000 * DFLL_FREQ_REQ_SCALE_MAX,
					  td->dvco_rate_min / 1000);
		if (!scale) {
			dev_err(td->dev, "%s: Rate %lu is too low\n",
				__func__, rate);
			return -EINVAL;
		}
		req->scale_bits = scale - 1;
		rate = td->dvco_rate_min;
	}

	/* Convert requested rate into frequency request and scale settings */
	val = DVCO_RATE_TO_MULT(rate, td->ref_rate);
	if (val > FREQ_MAX) {
		dev_err(td->dev, "%s: Rate %lu is above dfll range\n",
			__func__, rate);
		return -EINVAL;
	}
	req->mult_bits = val;
	req->dvco_target_rate = MULT_TO_DVCO_RATE(req->mult_bits, td->ref_rate);
	req->lut_index = find_lut_index_for_rate(td, req->dvco_target_rate);
	if (req->lut_index < 0)
		return req->lut_index;

	return 0;
}

/**
 * dfll_set_frequency_request - start the frequency change operation
 * @td: DFLL instance
 * @req: rate request structure
 *
 * Tell the DFLL to try to change its output frequency to the
 * frequency represented by @req. DFLL must be in closed-loop mode.
 */
static void dfll_set_frequency_request(struct tegra_dfll *td,
				       struct dfll_rate_req *req)
{
	u32 val;
	int force_val;
	int coef = 128; /* FIXME: td->cg_scale? */;

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
		force_val = td->lut[req->lut_index] - td->lut[td->lut_safe];
	else if (td->lut_force_min > req->lut_index) {
		int f;

		/* respect force output floor when new rate is lower */
		val = dfll_readl(td, DFLL_FREQ_REQ);
		f = val & DFLL_FREQ_REQ_MULT_MASK;
		if (!(val & DFLL_FREQ_REQ_FREQ_VALID)
			|| (f > req->mult_bits))
			force_val = td->lut_force_min - td->lut_safe;
		else
			force_val = req->lut_index - td->lut_safe;
	} else
		force_val = req->lut_index - td->lut_safe;

	force_val = force_val * coef / td->cg;
	force_val = clamp(force_val, FORCE_MIN, FORCE_MAX);

	val = req->mult_bits << DFLL_FREQ_REQ_MULT_SHIFT;
	val |= req->scale_bits << DFLL_FREQ_REQ_SCALE_SHIFT;
	val |= ((u32)force_val << DFLL_FREQ_REQ_FORCE_SHIFT) &
		DFLL_FREQ_REQ_FORCE_MASK;
	val |= DFLL_FREQ_REQ_FREQ_VALID | DFLL_FREQ_REQ_FORCE_ENABLE;

	dfll_writel(td, val, DFLL_FREQ_REQ);
	dfll_wmb(td);
}

/**
 * tegra_dfll_request_rate - set the next rate for the DFLL to tune to
 * @td: DFLL instance
 * @rate: clock rate to target
 *
 * Convert the requested clock rate @rate into the DFLL control logic
 * settings. In closed-loop mode, update new settings immediately to
 * adjust DFLL output rate accordingly. Otherwise, just save them
 * until the next switch to closed loop. Returns 0 upon success,
 * -EPERM if the DFLL driver has not yet been initialized, or -EINVAL
 * if @rate is outside the DFLL's tunable range.
 */
static int dfll_request_rate(struct tegra_dfll *td, unsigned long rate)
{
	int ret;
	struct dfll_rate_req req;
	bool dvco_min_crossed, dvco_min_updated;

	if (td->mode == DFLL_UNINITIALIZED) {
		dev_err(td->dev, "%s: Cannot set DFLL rate in %s mode\n",
			__func__, mode_name[td->mode]);
		return -EPERM;
	}

	req.rate = rate;

	/* Calibrate dfll minimum rate */
	dfll_calibrate(td);

	/* Update minimum dvco rate if we are crossing tuning threshold */
	dvco_min_updated = dfll_tune_target(td, rate) !=
			   dfll_tune_target(td, td->last_req.rate);
	if (dvco_min_updated)
		set_dvco_rate_min(td, &req);

	ret = dfll_calculate_rate_request(td, &req, rate);
	if (ret) {
		set_dvco_rate_min(td, &td->last_req);
		return ret;
	}

	dvco_min_crossed = (rate == td->dvco_rate_min) &&
			   (td->last_req.rate > td->dvco_rate_min);

	td->last_unrounded_rate = rate;
	td->last_req = req;

	if (td->mode == DFLL_CLOSED_LOOP) {
		dfll_set_close_loop_config(td, &td->last_req);
		dfll_set_frequency_request(td, &td->last_req);
		if (dvco_min_updated || dvco_min_crossed)
			calibration_timer_update(td);
	} 

	return 0;
}

unsigned long dfll_request_get(struct tegra_dfll *td)
{
	/*
	 * If running below dvco minimum rate with skipper resolution:
	 * dvco min rate / 256 - return last requested rate rounded to 1kHz.
	 * If running above dvco minimum, with closed loop resolution:
	 * ref rate / 2 - return cl_dvfs target rate.
	 */
	if ((td->last_req.scale_bits + 1) < DFLL_FREQ_REQ_SCALE_MAX)
		return (td->last_req.rate / 1000) * 1000;

	return td->last_req.dvco_target_rate;
}

static void calibration_timer_cb(unsigned long data)
{
	unsigned long rate_min, flags;
	struct tegra_dfll *td = (struct tegra_dfll *)data;

	spin_lock_irqsave(&td->lock, flags);

	rate_min = td->dvco_rate_min;
	dfll_calibrate(td);
	if (rate_min != td->dvco_rate_min)
		dfll_request_rate(td, dfll_request_get(td));

	spin_unlock_irqrestore(&td->lock, flags);
}

/*
 * DFLL enable/disable & open-loop <-> closed-loop transitions
 */

/**
 * dfll_disable - switch from open-loop mode to disabled mode
 * @td: DFLL instance
 *
 * Switch from OPEN_LOOP state to DISABLED state. Returns 0 upon success
 * or -EPERM if the DFLL is not currently in open-loop mode.
 */
static int dfll_disable(struct tegra_dfll *td)
{
	unsigned long flags;

	if (td->mode != DFLL_OPEN_LOOP) {
		dev_err(td->dev, "cannot disable DFLL in %s mode\n",
			mode_name[td->mode]);
		return -EINVAL;
	}

	spin_lock_irqsave(&td->lock, flags);
	dfll_set_mode(td, DFLL_DISABLED);
	spin_unlock_irqrestore(&td->lock, flags);

	pm_runtime_put_sync(td->dev);

	pr_debug("%s: done\n", __func__);
	return 0;
}

/**
 * dfll_enable - switch a disabled DFLL to open-loop mode
 * @td: DFLL instance
 *
 * Switch from DISABLED state to OPEN_LOOP state. Returns 0 upon success
 * or -EPERM if the DFLL is not currently disabled.
 */
static int dfll_enable(struct tegra_dfll *td)
{
	unsigned long flags;

	if (td->mode != DFLL_DISABLED) {
		dev_err(td->dev, "cannot enable DFLL in %s mode\n",
			mode_name[td->mode]);
		return -EPERM;
	}

	pm_runtime_get_sync(td->dev);

	spin_lock_irqsave(&td->lock, flags);
	dfll_set_mode(td, DFLL_OPEN_LOOP);
	spin_unlock_irqrestore(&td->lock, flags);

	pr_debug("%s: done\n", __func__);
	return 0;
}

static int _dfll_lock(struct tegra_dfll *td)
{
	struct dfll_rate_req *req = &td->last_req;

	switch (td->mode) {
	case DFLL_CLOSED_LOOP:
		return 0;

	case DFLL_OPEN_LOOP:
		if (req->rate == 0) {
			dev_err(td->dev, "%s: Cannot lock DFLL at rate 0\n",
				__func__);
			return -EINVAL;
		}

		if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
			tegra_dfll_pwm_output_enable();
		else
			dfll_i2c_set_output_enabled(td, true);

		dfll_set_mode(td, DFLL_CLOSED_LOOP);
		dfll_set_close_loop_config(td, req);
		dfll_set_frequency_request(td, req);
		dfll_set_force_output_enabled(td, false);
		calibration_timer_update(td);
		if (dfll_one_shot_calibrate_floors(td)) {
			set_dvco_rate_min(td, req);
			dfll_request_rate(td, dfll_request_get(td));
		}
		return 0;

	default:
		BUG_ON(td->mode > DFLL_CLOSED_LOOP);
		dev_err(td->dev, "%s: Cannot lock DFLL in %s mode\n",
			__func__, mode_name[td->mode]);
		return -EPERM;
	}
}

/**
 * tegra_dfll_lock - switch from open-loop to closed-loop mode
 * @td: DFLL instance
 *
 * Switch from OPEN_LOOP state to CLOSED_LOOP state. Returns 0 upon success,
 * -EINVAL if the DFLL's target rate hasn't been set yet, or -EPERM if the
 * DFLL is not currently in open-loop mode.
 */
static int dfll_lock(struct tegra_dfll *td)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&td->lock, flags);

	ret = _dfll_lock(td);

	spin_unlock_irqrestore(&td->lock, flags);

	if (!ret)
		pr_debug("%s: done\n", __func__);
	return ret;
}

static int _dfll_unlock(struct tegra_dfll *td)
{
	switch (td->mode) {
	case DFLL_CLOSED_LOOP:
		dfll_set_open_loop_config(td);
		dfll_set_mode(td, DFLL_OPEN_LOOP);

		if (td->pmu_if == TEGRA_DFLL_PMU_PWM)
			tegra_dfll_pwm_output_disable();
		else
			dfll_i2c_set_output_enabled(td, false);

		return 0;

	case DFLL_OPEN_LOOP:
		return 0;

	default:
		BUG_ON(td->mode > DFLL_CLOSED_LOOP);
		dev_err(td->dev, "%s: Cannot unlock DFLL in %s mode\n",
			__func__, mode_name[td->mode]);
		return -EPERM;
	}
}

/**
 * tegra_dfll_unlock - switch from closed-loop to open-loop mode
 * @td: DFLL instance
 *
 * Switch from CLOSED_LOOP state to OPEN_LOOP state. Returns 0 upon success,
 * or -EPERM if the DFLL is not currently in open-loop mode.
 */
static int dfll_unlock(struct tegra_dfll *td)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&td->lock, flags);

	ret = _dfll_unlock(td);

	spin_unlock_irqrestore(&td->lock, flags);

	if (!ret)
		pr_debug("%s: done\n", __func__);
	return ret;
}

/*
 * Clock framework integration
 *
 * When the DFLL is being controlled by the CCF, always enter closed loop
 * mode when the clk is enabled. This requires that a DFLL rate request
 * has been set beforehand, which implies that a clk_set_rate() call is
 * always required before a clk_enable().
 */

static int dfll_clk_is_enabled(struct clk_hw *hw)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);

	return dfll_is_running(td);
}

static int dfll_clk_enable(struct clk_hw *hw)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);

	return dfll_enable(td);
}

static void dfll_clk_disable(struct clk_hw *hw)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);

	dfll_disable(td);
}

static unsigned long dfll_clk_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);

	return td->last_unrounded_rate;
}

/* Must use determine_rate since it allows for rates exceeding 2^31-1 */
static int dfll_clk_determine_rate(struct clk_hw *hw,
				   struct clk_rate_request *clk_req)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);
	struct dfll_rate_req req;
	int ret;

	ret = dfll_calculate_rate_request(td, &req, clk_req->rate);
	if (ret)
		return ret;

	/*
	 * Don't set the rounded rate, since it doesn't really matter as
	 * the output rate will be voltage controlled anyway, and cpufreq
	 * freaks out if any rounding happens.
	 */

	return 0;
}

static int dfll_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct tegra_dfll *td = clk_hw_to_dfll(hw);
	int err;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	err = dfll_request_rate(td, rate);

	spin_unlock_irqrestore(&td->lock, flags);

	return err;
}

static const struct clk_ops dfll_clk_ops = {
	.is_enabled	= dfll_clk_is_enabled,
	.prepare	= dfll_clk_enable,
	.unprepare	= dfll_clk_disable,
	.recalc_rate	= dfll_clk_recalc_rate,
	.determine_rate	= dfll_clk_determine_rate,
	.set_rate	= dfll_clk_set_rate,
};

static struct clk_init_data dfll_clk_init_data = {
	.ops		= &dfll_clk_ops,
	.num_parents	= 0,
};

static int cclk_g_parent_event(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct clk_notifier_data *cnd = data;
	struct clk *cclk_g_parent = clk_get_parent(cnd->clk);
	struct tegra_dfll *td = cclk_g_nb_to_dfll(nb);

	switch (action) {
	case PRE_PARENT_CHANGE:
		if (cclk_g_parent == td->dfll_clk) {
			/* Unlock DFLL before switching to PLL parent */
			if(dfll_unlock(td))
				return NOTIFY_BAD;
		} else if (!td->last_req.rate) {
			dev_err(td->dev, "%s: Don't switch to DFLL at rate 0\n",
				__func__);
			return NOTIFY_BAD;
		}
		break;

	case ABORT_PARENT_CHANGE:
		/* fall thru to re-lock DFLL if switch to PLL was aborted */

	case POST_PARENT_CHANGE:
		if (cclk_g_parent == td->dfll_clk) {
			/*
			 * DFLL is enabled (i.e. running in open loop) by CCF
			 * set parent code before the parent switch. Now, after
			 * the switch DFLL can be locked.
			 */
			if(dfll_lock(td))
				return NOTIFY_BAD;
		}
		break;
	}

	return NOTIFY_DONE;
}

/**
 * dfll_register_clk - register the DFLL output clock with the clock framework
 * @td: DFLL instance
 *
 * Register the DFLL's output clock with the Linux clock framework and register
 * the DFLL driver as an OF clock provider. Returns 0 upon success or -EINVAL
 * or -ENOMEM upon failure.
 */
static int dfll_register_clk(struct tegra_dfll *td)
{
	int ret;

	dfll_clk_init_data.name = td->output_clock_name;
	td->dfll_clk_hw.init = &dfll_clk_init_data;

	td->dfll_clk = clk_register(td->dev, &td->dfll_clk_hw);
	if (IS_ERR(td->dfll_clk)) {
		dev_err(td->dev, "DFLL clock registration error\n");
		return -EINVAL;
	}

	ret = of_clk_add_provider(td->dev->of_node, of_clk_src_simple_get,
				  td->dfll_clk);
	if (ret) {
		dev_err(td->dev, "of_clk_add_provider() failed\n");

		clk_unregister(td->dfll_clk);
		return ret;
	}

	return 0;
}

/**
 * dfll_unregister_clk - unregister the DFLL output clock
 * @td: DFLL instance
 *
 * Unregister the DFLL's output clock from the Linux clock framework
 * and from clkdev. No return value.
 */
static void dfll_unregister_clk(struct tegra_dfll *td)
{
	of_clk_del_provider(td->dev->of_node);
	clk_unregister(td->dfll_clk);
	td->dfll_clk = NULL;
}

/*
 * Thermal interface
 */

/**
 * tegra_dfll_update_thermal_index - tell the DFLL how hot it is
 * @pdev: DFLL instance
 * @type: type of thermal floor or cap
 * @new_index: current DFLL temperature index
 *
 * Update the DFLL driver's sense of what temperature the DFLL is
 * running at.  Intended to be called by the function supplied to the struct
 * thermal_cooling_device_ops.set_cur_state function pointer.  Returns
 * 0 upon success or -ERANGE if @new_index is out of range.
 */
int tegra_dfll_update_thermal_index(struct tegra_dfll *td,
			enum tegra_dfll_thermal_type type,
			unsigned long new_index)
{
	int mv;
	unsigned long flags;

	if (type == TEGRA_DFLL_THERMAL_FLOOR && td->soc->thermal_floor_table) {
		if (new_index >= td->soc->thermal_floor_table_size)
			return -ERANGE;

		spin_lock_irqsave(&td->lock, flags);
		mv = td->soc->thermal_floor_table[new_index].millivolts;
		td->thermal_floor_output = find_mv_out_cap(td, mv);
		td->thermal_floor_index = new_index;

		dfll_one_shot_calibrate_floors(td);
		set_dvco_rate_min(td, &td->last_req);
		set_force_out_min(td);

		if (td->mode == DFLL_CLOSED_LOOP)
			dfll_request_rate(td, dfll_request_get(td));

		spin_unlock_irqrestore(&td->lock, flags);
	} else if (type == TEGRA_DFLL_THERMAL_CAP &&
		   td->soc->thermal_cap_table) {
		if (new_index >= td->soc->thermal_cap_table_size)
			return -ERANGE;

		spin_lock_irqsave(&td->lock, flags);
		mv = td->soc->thermal_cap_table[new_index].millivolts;
		td->thermal_cap_output = find_mv_out_floor(td, mv);
		td->thermal_cap_index = new_index;

		if (td->mode == DFLL_CLOSED_LOOP)
			dfll_request_rate(td, dfll_request_get(td));

		spin_unlock_irqrestore(&td->lock, flags);
	}

	return 0;
}
EXPORT_SYMBOL(tegra_dfll_update_thermal_index);

/**
 * tegra_dfll_get_thermal_index - return the DFLL's current thermal states
 * @pdev: DFLL instance
 * @type: type of thermal floor or cap
 *
 * Return the DFLL driver's copy of the DFLL's current temperature
 * index, set by tegra_dfll_update_thermal_index(). Intended to be
 * called by the function supplied to the struct
 * thermal_cooling_device_ops.get_cur_state function pointer.
 */
int tegra_dfll_get_thermal_index(struct tegra_dfll *td,
			enum tegra_dfll_thermal_type type)
{
	int index;

	switch (type) {
	case TEGRA_DFLL_THERMAL_FLOOR:
		index = td->thermal_floor_index;
		break;
	case TEGRA_DFLL_THERMAL_CAP:
		index = td->thermal_cap_index;
		break;
	default:
		index = -EINVAL;
	}

	return index;
}
EXPORT_SYMBOL(tegra_dfll_get_thermal_index);

/**
 * tegra_dfll_count_thermal_states - return the number of thermal states
 * @pdev: DFLL instance
 * @type: type of thermal floor or cap
 *
 * Return the number of thermal states passed into the DFLL driver
 * from the SoC data. Intended to be called by the function supplied
 * to the struct thermal_cooling_device_ops.get_max_state function
 * pointer, and by the integration code that binds a thermal zone to
 * the DFLL thermal reaction driver.
 */
int tegra_dfll_count_thermal_states(struct tegra_dfll *td,
			enum tegra_dfll_thermal_type type)
{
	int size;

	switch (type) {
	case TEGRA_DFLL_THERMAL_FLOOR:
		size = td->soc->thermal_floor_table_size;
		break;
	case TEGRA_DFLL_THERMAL_CAP:
		size = td->soc->thermal_cap_table_size;
		break;
	default:
		size = -EINVAL;
	}

	return size;
}
EXPORT_SYMBOL(tegra_dfll_count_thermal_states);

/**
 * tegra_dfll_get_thermal_floor_mv - return millivolts of thermal floor
 */
u32 tegra_dfll_get_thermal_floor_mv(void)
{
	return tegra_dfll_dev->lut_uv[tegra_dfll_dev->thermal_floor_output]
	       / 1000;
}
EXPORT_SYMBOL(tegra_dfll_get_thermal_floor_mv);

/**
 * tegra_dfll_get_thermal_index - return millivolts of thermal cap
 */
u32 tegra_dfll_get_thermal_cap_mv(void)
{
	return tegra_dfll_dev->lut_uv[tegra_dfll_dev->thermal_cap_output]
	       / 1000;
}
EXPORT_SYMBOL(tegra_dfll_get_thermal_cap_mv);

/**
 * tegra_dfll_get_by_phandle - get DFLL device from a phandle
 * @np: device node
 * @prop: property name
 *
 * Returns the DFLL instance referred to by the phandle @prop in node @np
 * or an ERR_PTR() on failure.
 */
struct tegra_dfll *tegra_dfll_get_by_phandle(struct device_node *np,
					     const char *prop)
{
	struct device_node *dfll_np;
	struct tegra_dfll *td;

	dfll_np = of_parse_phandle(np, prop, 0);
	if (!dfll_np)
		return ERR_PTR(-ENOENT);

	if (!tegra_dfll_dev) {
		td = ERR_PTR(-EPROBE_DEFER);
		goto error;
	}

	if (tegra_dfll_dev->dev->of_node != dfll_np) {
		td = ERR_PTR(-EINVAL);
		goto error;
	}

	td = tegra_dfll_dev;

error:
	of_node_put(dfll_np);
	return td;
}
EXPORT_SYMBOL(tegra_dfll_get_by_phandle);

/**
 * tegra_dfll_get_min_millivolts - return DFLL min millivolts
 */
u32 tegra_dfll_get_min_millivolts(void)
{
	return tegra_dfll_dev->soc->min_millivolts;
}
EXPORT_SYMBOL(tegra_dfll_get_min_millivolts);

/**
 * tegra_dfll_get_alignment - return DFLL alignment
 */
struct rail_alignment *tegra_dfll_get_alignment(void)
{
	if (!tegra_dfll_dev)
		return ERR_PTR(-EPROBE_DEFER);
	return &tegra_dfll_dev->soc->alignment;
}

/*
 * DFLL initialization
 */

/**
 * dfll_set_default_params - program non-output related DFLL parameters
 * @td: DFLL instance
 *
 * During DFLL driver initialization or resume from context loss,
 * program parameters for the closed loop integrator, DVCO tuning,
 * voltage droop control and monitor control.
 */
static void dfll_set_default_params(struct tegra_dfll *td)
{
	u32 val;

	val = DIV_ROUND_UP(td->ref_rate, td->sample_rate * 32);
	BUG_ON(val > DFLL_CONFIG_DIV_MASK);
	dfll_writel(td, val, DFLL_CONFIG);

	val = (td->force_mode << DFLL_PARAMS_FORCE_MODE_SHIFT) |
		(td->cf << DFLL_PARAMS_CF_PARAM_SHIFT) |
		(td->ci << DFLL_PARAMS_CI_PARAM_SHIFT) |
		(td->cg << DFLL_PARAMS_CG_PARAM_SHIFT) |
		(td->cg_scale ? DFLL_PARAMS_CG_SCALE : 0);
	dfll_writel(td, val, DFLL_PARAMS);

	dfll_tune_low(td);
	dfll_writel(td, td->droop_ctrl, DFLL_DROOP_CTRL);
	dfll_set_monitor_mode(td, DFLL_FREQ);
}

/**
 * dfll_init_clks - clk_get() the DFLL source clocks
 * @td: DFLL instance
 *
 * Call clk_get() on the DFLL source clocks and save the pointers for later
 * use. Returns 0 upon success or error (see devm_clk_get) if one or more
 * of the clocks couldn't be looked up.
 */
static int dfll_init_clks(struct tegra_dfll *td)
{
	int ret;

	td->ref_clk = devm_clk_get(td->dev, "ref");
	if (IS_ERR(td->ref_clk)) {
		dev_err(td->dev, "missing ref clock\n");
		return PTR_ERR(td->ref_clk);
	}

	td->soc_clk = devm_clk_get(td->dev, "soc");
	if (IS_ERR(td->soc_clk)) {
		dev_err(td->dev, "missing soc clock\n");
		return PTR_ERR(td->soc_clk);
	}

	td->i2c_clk = devm_clk_get(td->dev, "i2c");
	if (IS_ERR(td->i2c_clk)) {
		dev_err(td->dev, "missing i2c clock\n");
		return PTR_ERR(td->i2c_clk);
	}
	td->i2c_clk_rate = clk_get_rate(td->i2c_clk);

	td->cclk_g_clk = devm_clk_get(td->dev, "cclk_g");
	if (IS_ERR(td->cclk_g_clk)) {
		dev_err(td->dev, "missing cclk_g clock\n");
		return PTR_ERR(td->cclk_g_clk);
	}
	td->cclk_g_parent_nb.notifier_call = cclk_g_parent_event;
	ret = clk_notifier_register(td->cclk_g_clk, &td->cclk_g_parent_nb);
	if (ret) {
		dev_err(td->dev, "failed to register cclk_g notifier\n");
		return ret;
	}

	return 0;
}

/**
 * dfll_init_tuning_thresholds - set up the high voltage range, if possible
 * @td: DFLL instance
 *
 * Determine whether the DFLL tuning parameters need to be
 * reprogrammed when the DFLL voltage reaches a certain minimum
 * threshold.  No return value.
 */
static void dfll_init_tuning_thresholds(struct tegra_dfll *td)
{
	u8 out_min, out_start, max_voltage_index;

	max_voltage_index = td->lut_size - 1;

	/*
	 * Convert high tuning voltage threshold into output LUT
	 * index, and add necessary margin.  If voltage threshold is
	 * outside operating range set it at maximum output level to
	 * effectively disable tuning parameters adjustment.
	 */
	td->tune_high_out_min = max_voltage_index;
	td->tune_high_out_start = max_voltage_index;
	td->tune_high_dvco_rate_min = ULONG_MAX;
	td->tune_high_target_rate_min = ULONG_MAX;

	if (td->soc->tune_high_min_millivolts < td->soc->min_millivolts)
		return;	/* no difference between low & high voltage range */

	out_min = find_mv_out_cap(td, td->soc->tune_high_min_millivolts);
	if ((out_min + 2) > max_voltage_index)
		return;

	out_start = out_min + DFLL_TUNE_HIGH_MARGIN_STEPS;
	if (out_start > max_voltage_index)
		return;

	td->tune_high_out_min = out_min;
	td->tune_high_out_start = out_start;
	td->tune_high_dvco_rate_min = get_dvco_rate_above(td, out_start);
	td->tune_high_target_rate_min = get_dvco_rate_above(td, out_min);
}

/**
 * dfll_init - Prepare the DFLL IP block for use
 * @td: DFLL instance
 *
 * Do everything necessary to prepare the DFLL IP block for use. The
 * DFLL will be left in DISABLED state. Called by dfll_probe().
 * Returns 0 upon success, or passes along the error from whatever
 * function returned it.
 */
static int dfll_init(struct tegra_dfll *td)
{
	int ret;

	td->ref_rate = clk_get_rate(td->ref_clk);
	if (td->ref_rate != REF_CLOCK_RATE) {
		dev_err(td->dev, "unexpected ref clk rate %lu, expecting %lu",
			td->ref_rate, REF_CLOCK_RATE);
		return -EINVAL;
	}

	reset_control_deassert(td->dvco_rst);

	ret = clk_prepare(td->ref_clk);
	if (ret) {
		dev_err(td->dev, "failed to prepare ref_clk\n");
		return ret;
	}

	ret = clk_prepare(td->soc_clk);
	if (ret) {
		dev_err(td->dev, "failed to prepare soc_clk\n");
		goto di_err1;
	}

	ret = clk_prepare(td->i2c_clk);
	if (ret) {
		dev_err(td->dev, "failed to prepare i2c_clk\n");
		goto di_err2;
	}

	td->last_unrounded_rate = 0;

	pm_runtime_enable(td->dev);
	pm_runtime_get_sync(td->dev);

	dfll_set_mode(td, DFLL_DISABLED);
	dfll_set_default_params(td);

	if (td->soc->init_clock_trimmers)
		td->soc->init_clock_trimmers();

	dfll_set_open_loop_config(td);

	dfll_init_out_if(td);

	dfll_init_tuning_thresholds(td);

	pm_runtime_put_sync(td->dev);

	return 0;

di_err2:
	clk_unprepare(td->soc_clk);
di_err1:
	clk_unprepare(td->ref_clk);

	reset_control_assert(td->dvco_rst);

	return ret;
}

/*
 * DT data fetch
 */

/*
 * Find a PMIC voltage register-to-voltage mapping for the given voltage.
 * An exact voltage match is required.
 */
static int find_vdd_map_entry_exact(struct tegra_dfll *td, int uV)
{
	int i, n_voltages, reg_volt, align_volt;

	align_volt = uV / td->soc->alignment.step_uv;
	n_voltages = regulator_count_voltages(td->vdd_reg);

	for (i = 0; i < n_voltages; i++) {
		reg_volt = regulator_list_voltage_unlocked(td->vdd_reg, i) /
					td->soc->alignment.step_uv;

		if (reg_volt < 0)
			break;

		if (align_volt == reg_volt)
			return i;
	}

	dev_err(td->dev, "no voltage map entry for %d uV\n", uV);
	return -EINVAL;
}

/*
 * Find a PMIC voltage register-to-voltage mapping for the given voltage,
 * rounding up to the closest supported voltage.
 * */
static int find_vdd_map_entry_min(struct tegra_dfll *td, int uV)
{
	int i, n_voltages, reg_volt, align_volt;

	align_volt = uV / td->soc->alignment.step_uv;
	n_voltages = regulator_count_voltages(td->vdd_reg);

	for (i = 0; i < n_voltages; i++) {
		reg_volt = regulator_list_voltage_unlocked(td->vdd_reg, i) /
					td->soc->alignment.step_uv;

		if (reg_volt < 0)
			break;

		if (align_volt <= reg_volt)
			return i;
	}

	dev_err(td->dev, "no voltage map entry rounding to %d uV\n", uV);
	return -EINVAL;
}

/*
 * Look-up table in h/w is ignored when PWM is used as DFLL interface to PMIC.
 * In this case closed loop output is controlling duty cycle directly. The s/w
 * look-up that maps PWM duty cycle to voltage is still built by this function.
 */
static int dfll_build_lut_pwm(struct tegra_dfll *td, int v_max)
{
	int i, n_voltages, reg_volt;
	unsigned long rate;
	u8 lut_bottom = MAX_DFLL_VOLTAGES;
	int v_min = td->soc->min_millivolts * 1000;

	n_voltages = regulator_count_voltages(td->vdd_reg);
	n_voltages = min(MAX_DFLL_VOLTAGES, n_voltages);

	for (i = 0; i < n_voltages; i++) {
		reg_volt = regulator_list_voltage_unlocked(td->vdd_reg, i);
		if (reg_volt < 0) {
			dev_err(td->dev, "PWM selector %d not listed\n", i);
			return -EINVAL;
		}

		/* since opp voltage is exact mv */
		reg_volt = (reg_volt / 1000) * 1000;
		if (reg_volt > v_max)
			break;

		td->lut[i] = i;
		td->lut_uv[i] = reg_volt;
		if ((lut_bottom == MAX_DFLL_VOLTAGES) && (reg_volt >= v_min))
			lut_bottom = i;
	}

	/* determine voltage boundaries */
	td->lut_size = i;
	if ((lut_bottom == MAX_DFLL_VOLTAGES) ||
	    (lut_bottom + 1 >= td->lut_size)) {
		dev_err(td->dev, "no voltage above DFLL minimum %d mV\n",
			td->soc->min_millivolts);
		return -EINVAL;
	}
	td->lut_bottom = lut_bottom;

	/* determine rate boundaries */
	rate = get_dvco_rate_below(td, td->lut_bottom);
	if (!rate) {
		dev_err(td->dev, "no opp below DFLL minimum voltage %d mV\n",
			td->soc->min_millivolts);
		return -EINVAL;
	}
	td->dvco_rate_min = td->out_rate_min = rate;
	rate = get_dvco_rate_below(td, td->lut_size - 1);
	td->out_rate_max = rate;

	return 0;
}

/**
 * dfll_build_lut - build the I2C and PWM voltage register lookup table
 * @td: DFLL instance
 *
 * The DFLL hardware has 33 bytes of look-up table RAM that must be filled with
 * PMIC voltage register values that span the entire DFLL operating range.
 * This function builds the look-up table based on the OPP table provided by
 * the soc-specific platform driver (td->soc->opp_dev) and the PMIC
 * register-to-voltage mapping queried from the regulator framework.
 *
 * On success, fills in td->lut and returns 0, or -err on failure.
 */
static int dfll_build_lut(struct tegra_dfll *td)
{
	int ret = -EINVAL;
	int j, v, v_max, v_opp, v_min_align;
	int selector;
	unsigned long rate;
	struct dev_pm_opp *opp;
	int lut;

	rcu_read_lock();

	rate = ULONG_MAX;
	opp = dev_pm_opp_find_freq_floor(td->soc->dev, &rate);
	if (IS_ERR(opp)) {
		dev_err(td->dev, "couldn't get vmax opp, empty opp table?\n");
		goto out;
	}
	v_max = dev_pm_opp_get_voltage(opp);

	if (td->pmu_if == TEGRA_DFLL_PMU_PWM) {
		rcu_read_unlock();
		return dfll_build_lut_pwm(td, v_max);
	} else {
		v = td->soc->min_millivolts * 1000;
		lut = find_vdd_map_entry_exact(td, v);
	}

	v_min_align = DIV_ROUND_UP(td->soc->min_millivolts * 1000,
		td->soc->alignment.step_uv) * td->soc->alignment.step_uv;

	if (lut < 0)
		goto out;
	td->lut[0] = lut;
	td->lut_bottom = 0;

	for (j = 1, rate = 0; ; rate++) {
		opp = dev_pm_opp_find_freq_ceil(td->soc->dev, &rate);
		if (IS_ERR(opp))
			break;
		v_opp = dev_pm_opp_get_voltage(opp);
		if (v_opp <= v_min_align)
			td->out_rate_min = dev_pm_opp_get_freq(opp);

		if (rate > td->out_rate_max)
			td->out_rate_max = rate;

		for (;;) {
			v += max(1, (v_max - v) / (MAX_DFLL_VOLTAGES - j));
			if (v >= v_opp)
				break;

			selector = find_vdd_map_entry_min(td, v);
			if (selector < 0)
				goto out;
			if (selector != td->lut[j - 1])
				td->lut[j++] = selector;
		}

		v = (j == MAX_DFLL_VOLTAGES - 1) ? v_max : v_opp;
		selector = find_vdd_map_entry_exact(td, v);
		if (selector < 0)
			goto out;
		if (selector != td->lut[j - 1])
			td->lut[j++] = selector;

		if (v >= v_max)
			break;
	}
	td->lut_size = j;

	if (!td->out_rate_min) {
		dev_err(td->dev, "no opp below DFLL minimum voltage %d mV\n",
			td->soc->min_millivolts);
	} else {
		ret = 0;
		td->dvco_rate_min = td->out_rate_min;

		for (j = 0; j < td->lut_size; j++)
			td->lut_uv[j] =
				regulator_list_voltage_unlocked(td->vdd_reg,
				td->lut[j]);
	}

out:
	rcu_read_unlock();

	return ret;
}

/**
 * read_dt_param - helper function for reading required parameters from the DT
 * @td: DFLL instance
 * @param: DT property name
 * @dest: output pointer for the value read
 *
 * Read a required numeric parameter from the DFLL device node, or complain
 * if the property doesn't exist. Returns a boolean indicating success for
 * easy chaining of multiple calls to this function.
 */
static bool read_dt_param(struct tegra_dfll *td, const char *param, u32 *dest)
{
	int err = of_property_read_u32(td->dev->of_node, param, dest);

	if (err < 0) {
		dev_err(td->dev, "failed to read DT parameter %s: %d\n",
			param, err);
		return false;
	}

	return true;
}

/**
 * dfll_fetch_i2c_params - query PMIC I2C params from DT & regulator subsystem
 * @td: DFLL instance
 *
 * Read all the parameters required for operation in I2C mode. The parameters
 * can originate from the device tree or the regulator subsystem.
 * Returns 0 on success or -err on failure.
 */
static int dfll_fetch_i2c_params(struct tegra_dfll *td)
{
	struct regmap *regmap;
	struct device *i2c_dev;
	struct i2c_client *i2c_client;
	int vsel_reg, vsel_mask;
	int ret;

	if (!read_dt_param(td, "nvidia,i2c-fs-rate", &td->i2c_fs_rate))
		return -EINVAL;

	read_dt_param(td, "nvidia,pmic-undershoot-gb", &td->pmu_undershoot_gb);

	regmap = regulator_get_regmap(td->vdd_reg);
	i2c_dev = regmap_get_device(regmap);
	i2c_client = to_i2c_client(i2c_dev);

	td->i2c_slave_addr = i2c_client->addr;

	ret = regulator_get_hardware_vsel_register(td->vdd_reg,
						   &vsel_reg,
						   &vsel_mask);
	if (ret < 0) {
		dev_err(td->dev,
			"regulator unsuitable for DFLL I2C operation\n");
		return -EINVAL;
	}
	td->i2c_reg = vsel_reg;

	return 0;
}

/**
 * tegra_dfll_get_peak_thermal_floor_mv - get millivolts of peak thermal floor
 */
u32 tegra_dfll_get_peak_thermal_floor_mv(void)
{
	if (WARN_ON(!tegra_dfll_dev))
		return 0;

	return tegra_dfll_dev->soc->thermal_floor_table[0].millivolts;
}
EXPORT_SYMBOL(tegra_dfll_get_peak_thermal_floor_mv);

/**
 * dfll_fetch_common_params - read DFLL parameters from the device tree
 * @td: DFLL instance
 *
 * Read all the DT parameters that are common to both I2C and PWM operation.
 * Returns 0 on success or -EINVAL on any failure.
 */
static int dfll_fetch_common_params(struct tegra_dfll *td)
{
	bool ok = true;
	struct device_node *dn = td->dev->of_node;

	ok &= read_dt_param(td, "nvidia,droop-ctrl", &td->droop_ctrl);
	ok &= read_dt_param(td, "nvidia,sample-rate", &td->sample_rate);
	ok &= read_dt_param(td, "nvidia,force-mode", &td->force_mode);
	ok &= read_dt_param(td, "nvidia,cf", &td->cf);
	ok &= read_dt_param(td, "nvidia,ci", &td->ci);
	ok &= read_dt_param(td, "nvidia,cg", &td->cg);
	td->cg_scale = of_property_read_bool(dn, "nvidia,cg-scale");

	if (of_property_read_bool(dn, "nvidia,calibrate-force-vmin"))
		td->cfg_flags |= DFLL_CALIBRATE_FORCE_VMIN;
	if (of_property_read_bool(dn, "nvidia,defer-force-calibrate"))
		td->cfg_flags |= DFLL_DEFER_FORCE_CALIBRATE;
	if (of_property_read_bool(dn, "nvidia,one-shot-calibrate"))
		td->cfg_flags |= DFLL_ONE_SHOT_CALIBRATE;
	if (of_property_read_bool(dn, "nvidia,idle-override"))
		td->cfg_flags |= DFLL_HAS_IDLE_OVERRIDE;

	if (of_property_read_string(dn, "clock-output-names",
				    &td->output_clock_name)) {
		dev_err(td->dev, "missing clock-output-names property\n");
		ok = false;
	}

	return ok ? 0 : -EINVAL;
}

/**
 * dfll_dt_parse - read necessary parameters from the device tree
 * @td: DFLL instance
 *
 * Read necessary DT parameters based on I2C or PWM operation.
 * Returns 0 on success or -EINVAL on any failure.
 */
static int dfll_dt_parse(struct tegra_dfll *td)
{
	int ret;
	struct device_node *dn = td->dev->of_node;

	ret = dfll_fetch_common_params(td);
	if (ret) {
		dev_err(td->dev, "couldn't parse device tree parameters\n");
		return ret;
	}

	td->vdd_reg = devm_regulator_get(td->dev, "vdd-cpu");
	if (IS_ERR(td->vdd_reg)) {
		dev_err(td->dev, "couldn't get vdd_cpu regulator\n");
		return PTR_ERR(td->vdd_reg);
	}

	/* I2C or PWM interface */
	if (of_property_read_bool(dn, "nvidia,pwm-to-pmic")) {
		td->pmu_if = TEGRA_DFLL_PMU_PWM;
		dev_info(td->dev, "config PMU interface as PWM\n");

		ret = of_property_read_u32(dn, "nvidia,init-uv",
					&td->reg_init_uV);
		if (ret) {
			dev_err(td->dev, "couldn't get initialized voltage\n");
			return ret;
		}
	} else {
		td->pmu_if = TEGRA_DFLL_PMU_I2C;
		dev_info(td->dev, "config PMU interface as I2C\n");

		ret = dfll_fetch_i2c_params(td);
		if (ret) {
			dev_err(td->dev, "couldn't get I2C parameters\n");
			return ret;
		}
	}

	ret = dfll_build_lut(td);
	if (ret) {
		dev_err(td->dev, "couldn't build LUT\n");
		return ret;
	}

	return 0;
}

/*
 * tegra_dfll_suspend
 * @pdev: DFLL instance
 *
 * dfll controls clock/voltage to other devices, including CPU. Therefore,
 * dfll driver pm suspend callback does not stop cl-dvfs operations. It is
 * only used to enforce cold voltage limit, since SoC may cool down during
 * suspend without waking up. The correct temperature zone after suspend will
 * be updated via dfll cooling device interface during resume of temperature
 * sensor.
 */
void tegra_dfll_suspend(struct platform_device *pdev)
{
	struct tegra_dfll *td = dev_get_drvdata(&pdev->dev);

	if (td->mode <= DFLL_DISABLED)
		return;

	td->thermal_cap_index =
		tegra_dfll_count_thermal_states(td, TEGRA_DFLL_THERMAL_CAP);
	td->thermal_floor_index = 0;
	set_dvco_rate_min(td, &td->last_req);
	set_force_out_min(td);

	td->resume_mode = td->mode;
	switch (td->mode) {
	case DFLL_CLOSED_LOOP:
		dfll_set_close_loop_config(td, &td->last_req);
		dfll_set_frequency_request(td, &td->last_req);

		_dfll_unlock(td);
		break;
	default:
		break;
	}
}

/**
 * tegra_dfll_resume - reprogram the DFLL after context-loss
 * @pdev: DFLL instance
 *
 * Re-initialize and enable target device clock in open loop mode. Called
 * directly from SoC clock resume syscore operation. Closed loop will be
 * re-entered in platform syscore ops as well.
 */
void tegra_dfll_resume(struct platform_device *pdev)
{
	struct tegra_dfll *td = dev_get_drvdata(&pdev->dev);

	reset_control_deassert(td->dvco_rst);

	pm_runtime_get(td->dev);

	/* Re-init DFLL */
	dfll_init_out_if(td);
	dfll_init_tuning_thresholds(td);
	dfll_set_default_params(td);
	dfll_set_open_loop_config(td);

	pm_runtime_put(td->dev);

	if (td->mode <= DFLL_DISABLED)
		return;

	/* Restore last request and mode */
	switch (td->resume_mode) {
	case DFLL_OPEN_LOOP:
		dfll_set_mode(td, DFLL_OPEN_LOOP);
		dfll_i2c_set_output_enabled(td, false);
		break;
	case DFLL_CLOSED_LOOP:
		_dfll_lock(td);
		break;
	default:
		break;
	}
	td->resume_mode = DFLL_DISABLED;
}

/*
 * Debugfs interface
 */

#ifdef CONFIG_DEBUG_FS
/*
 * Output clock scaler helpers
 */

/**
 * dfll_scale_dvco_rate - calculate scaled rate from the DVCO rate
 * @scale_bits: clock scaler value (bits in the DFLL_FREQ_REQ_SCALE field)
 * @dvco_rate: the DVCO rate
 *
 * Apply the same scaling formula that the DFLL hardware uses to scale
 * the DVCO rate.
 */
static unsigned long dfll_scale_dvco_rate(int scale_bits,
					  unsigned long dvco_rate)
{
	return (u64)dvco_rate * (scale_bits + 1) / DFLL_FREQ_REQ_SCALE_MAX;
}


/*
 * Monitor control
 */

/**
 * dfll_read_monitor_rate - return the DFLL's output rate from internal monitor
 * @td: DFLL instance
 *
 * If the DFLL is enabled, return the last rate reported by the DFLL's
 * internal monitoring hardware. This works in both open-loop and
 * closed-loop mode, and takes the output scaler setting into account.
 * Assumes that the monitor was programmed to monitor frequency before
 * the sample period started. If the driver believes that the DFLL is
 * currently uninitialized or disabled, it will return 0, since
 * otherwise the DFLL monitor data register will return the last
 * measured rate from when the DFLL was active.
 */
static u64 dfll_read_monitor_rate(struct tegra_dfll *td)
{
	u32 v, s;
	u64 pre_scaler_rate, post_scaler_rate;
	unsigned long flags;

	if (!dfll_is_running(td))
		return 0;

	spin_lock_irqsave(&td->lock, flags);

	dfll_set_monitor_mode(td, DFLL_FREQ);
	dfll_get_monitor_data(td, &v);
	pre_scaler_rate = dfll_calc_monitored_rate(v, td->ref_rate);

	s = dfll_readl(td, DFLL_FREQ_REQ);
	s = (s & DFLL_FREQ_REQ_SCALE_MASK) >> DFLL_FREQ_REQ_SCALE_SHIFT;
	post_scaler_rate = dfll_scale_dvco_rate(s, pre_scaler_rate);

	spin_unlock_irqrestore(&td->lock, flags);

	return post_scaler_rate;
}

static int enable_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = dfll_is_running(td);

	return 0;
}
static int enable_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;

	return val ? dfll_enable(td) : dfll_disable(td);
}
DEFINE_SIMPLE_ATTRIBUTE(enable_fops, enable_get, enable_set, "%llu\n");

static int lock_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = (td->mode == DFLL_CLOSED_LOOP);

	return 0;
}
static int lock_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;

	return val ? dfll_lock(td) :  dfll_unlock(td);
}
DEFINE_SIMPLE_ATTRIBUTE(lock_fops, lock_get, lock_set, "%llu\n");

static int rate_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = dfll_read_monitor_rate(td);

	return 0;
}

static int rate_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;
	int err;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	err = dfll_request_rate(td, val);

	spin_unlock_irqrestore(&td->lock, flags);

	return err;
}
DEFINE_SIMPLE_ATTRIBUTE(rate_fops, rate_get, rate_set, "%llu\n");

static int dvco_rate_min_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = td->dvco_rate_min;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(dvco_rate_min_fops, dvco_rate_min_get, NULL, "%llu\n");

static int vmin_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = td->lut_uv[td->lut_min] / 1000;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(vmin_fops, vmin_get, NULL, "%llu\n");

static int vmax_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = td->lut_uv[td->lut_max] / 1000;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(vmax_fops, vmax_get, NULL, "%llu\n");

static int output_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;
	u32 reg;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	reg = dfll_readl(td, DFLL_OUTPUT_FORCE);
	if (reg & DFLL_OUTPUT_FORCE_ENABLE) {
		*val = td->lut_uv[reg & DFLL_OUTPUT_FORCE_VALUE_MASK] / 1000;
		goto out;
	}

	dfll_set_monitor_mode(td, DFLL_OUTPUT_VALUE);
	dfll_get_monitor_data(td, &reg);

	*val = td->lut_uv[reg] / 1000;

out:
	spin_unlock_irqrestore(&td->lock, flags);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(output_fops, output_get, NULL, "%llu\n");

static int fout_mv_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;
	u32 reg;

	reg = dfll_readl(td, DFLL_OUTPUT_FORCE);
	*val = td->lut_uv[reg & DFLL_OUTPUT_FORCE_VALUE_MASK] / 1000;

	return 0;
}

static int fout_mv_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	if (val) {
		u8 out_value;

		out_value = find_mv_out_cap(td, val);
		if (td->pmu_if == TEGRA_DFLL_PMU_I2C)
			out_value = td->lut[out_value];
		dfll_set_force_output_value(td, out_value);
		dfll_set_force_output_enabled(td, true);
	} else {
		dfll_set_force_output_enabled(td, false);
	}

	spin_unlock_irqrestore(&td->lock, flags);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(fout_mv_fops, fout_mv_get, fout_mv_set, "%llu\n");

static int undershoot_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = td->pmu_undershoot_gb;

	return 0;
}

static int undershoot_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	td->pmu_undershoot_gb = val;
	set_force_out_min(td);

	spin_unlock_irqrestore(&td->lock, flags);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(undershoot_fops, undershoot_get, undershoot_set,
			"%llu\n");
static int tune_high_mv_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = td->soc->tune_high_min_millivolts;

	return 0;
}

static int tune_high_mv_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);
	td->soc->tune_high_min_millivolts = val;
	dfll_init_tuning_thresholds(td);
	if (td->mode == DFLL_CLOSED_LOOP)
		dfll_set_frequency_request(td, &td->last_req);
	spin_unlock_irqrestore(&td->lock, flags);

	return clk_set_rate_nocache(td->dfll_clk, clk_get_rate(td->dfll_clk));
}
DEFINE_SIMPLE_ATTRIBUTE(tune_high_mv_fops, tune_high_mv_get, tune_high_mv_set,
			"%llu\n");

static int calibr_delay_get(void *data, u64 *val)
{
	struct tegra_dfll *td = data;

	*val = jiffies_to_msecs(td->calibration_delay);

	return 0;
}

static int calibr_delay_set(void *data, u64 val)
{
	struct tegra_dfll *td = data;
	unsigned long flags;

	if (!(td->cfg_flags & DFLL_ONE_SHOT_CALIBRATE)) {
		spin_lock_irqsave(&td->lock, flags);
		td->calibration_delay = msecs_to_jiffies(val);
		spin_unlock_irqrestore(&td->lock, flags);
	}

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(calibr_delay_fops, calibr_delay_get, calibr_delay_set,
			"%llu\n");

static int registers_show(struct seq_file *s, void *data)
{
	u32 val, offs;
	struct tegra_dfll *td = s->private;
	unsigned long flags;

	spin_lock_irqsave(&td->lock, flags);

	seq_puts(s, "CONTROL REGISTERS:\n");
	for (offs = 0; offs <= DFLL_MONITOR_DATA; offs += 4) {
		if (td->pmu_if == TEGRA_DFLL_PMU_I2C &&
		    offs == DFLL_OUTPUT_CFG)
			val = dfll_i2c_readl(td, offs);
		else
			val = dfll_readl(td, offs);
		seq_printf(s, "[0x%02x] = 0x%08x\n", offs, val);
	}

	seq_puts(s, "\nI2C and INTR REGISTERS:\n");
	for (offs = DFLL_I2C_CFG; offs <= DFLL_I2C_STS; offs += 4) {
		if (td->pmu_if == TEGRA_DFLL_PMU_I2C)
			val = dfll_i2c_readl(td, offs);
		else
			val = dfll_readl(td, offs);

		seq_printf(s, "[0x%02x] = 0x%08x\n", offs, val);
	}
	for (offs = DFLL_INTR_STS; offs <= DFLL_INTR_EN; offs += 4) {
		if (td->pmu_if == TEGRA_DFLL_PMU_I2C)
			val = dfll_i2c_readl(td, offs);
		else
			val = dfll_readl(td, offs);
		seq_printf(s, "[0x%02x] = 0x%08x\n", offs, val);
	}

	if (td->cfg_flags & DFLL_HAS_IDLE_OVERRIDE) {
		seq_puts(s, "\nOVERRIDE REGISTERS:\n");
		offs = DFLL_CC4_HVC;
		if (td->pmu_if == TEGRA_DFLL_PMU_I2C)
			val = dfll_i2c_readl(td, offs);
		else
			val = dfll_readl(td, offs);
		seq_printf(s, "[0x%02x] = 0x%08x\n", offs, val);
	}

	if (td->pmu_if == TEGRA_DFLL_PMU_I2C) {
		seq_puts(s, "\nINTEGRATED I2C CONTROLLER REGISTERS:\n");
		offs = DFLL_I2C_CLK_DIVISOR;
		seq_printf(s, "[0x%02x] = 0x%08x\n", offs,
			   __raw_readl(td->i2c_controller_base + offs));

		seq_puts(s, "\nLUT:\n");
		for (offs = 0; offs <  4 * MAX_DFLL_VOLTAGES; offs += 4)
			seq_printf(s, "[0x%02x] = 0x%08x\n", offs,
				   __raw_readl(td->lut_base + offs));
	}

	spin_unlock_irqrestore(&td->lock, flags);

	return 0;
}

static ssize_t register_write(struct file *file,
	const char __user *userbuf, size_t count, loff_t *ppos)
{
	char buf[80];
	u32 offs;
	u32 val;
	unsigned long flags;
	struct tegra_dfll *td = file->f_path.dentry->d_inode->i_private;

	if (sizeof(buf) <= count)
		return -EINVAL;

	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;

	/* terminate buffer and trim - white spaces may be appended
	 *  at the end when invoked from shell command line */
	buf[count] = '\0';
	strim(buf);

	if (sscanf(buf, "[0x%x] = 0x%x", &offs, &val) != 2)
		return -EINVAL;

	if (offs >= 0x400)
		return -EINVAL;

	clk_enable(td->soc_clk);
	spin_lock_irqsave(&td->lock, flags);
	dfll_writel(td, val, offs & (~0x3));
	spin_unlock_irqrestore(&td->lock, flags);
	clk_disable(td->soc_clk);

	return count;
}

static int registers_open(struct inode *inode, struct file *file)
{
	return single_open(file, registers_show, inode->i_private);
}

static const struct file_operations registers_fops = {
	.open		= registers_open,
	.read		= seq_read,
	.write		= register_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int profiles_show(struct seq_file *s, void *data)
{
	struct thermal_tv tv;
	int i, size;
	unsigned long r;
	struct tegra_dfll *td = s->private;
	u8 v;

	size = td->soc->thermal_cap_table_size;
	seq_printf(s, "THERM CAPS:%s\n", size ? "" : " NONE");
	for (i = 0; i < size; i++) {
		tv = td->soc->thermal_cap_table[i];
		if (tv.temp == TEGRA210_DFLL_THERMAL_CAP_NOCAP / 1000)
			continue;
		v = find_mv_out_floor(td, tv.millivolts);
		seq_printf(s, "%3dC.. %5dmV\n",
			   tv.temp, tegra_dfll_dev->lut_uv[v] / 1000);
	}

	if (td->tune_high_target_rate_min == ULONG_MAX) {
		seq_puts(s, "TUNE HIGH: NONE\n");
	} else {
		seq_puts(s, "TUNE HIGH:\n");
		seq_printf(s, "min    %5dmV%9lukHz%s\n",
			   td->lut_uv[td->tune_high_out_min],
			   td->tune_high_dvco_rate_min / 1000,
			   td->tune_high_calibrated ? " (calibrated)"  : "");
		seq_printf(s, "%-14s%9lukHz\n", "rate threshold",
			   td->tune_high_target_rate_min / 1000);
	}

	size = td->soc->thermal_floor_table_size;
	seq_printf(s, "THERM FLOORS:%s\n", size ? "" : " NONE");
	for (i = 0; i < size; i++) {
		tv = td->soc->thermal_floor_table[i];
		r = td->dvco_rate_floors[i];
		v = find_mv_out_cap(td, tv.millivolts);
		seq_printf(s, " ..%3dC%5dmV%9lukHz%s\n",
			   tv.temp, tegra_dfll_dev->lut_uv[v] / 1000,
			   (r ? : get_dvco_rate_below(td, v)) / 1000,
			   r ? " (calibrated)"  : "");
	}
	r = td->dvco_rate_floors[i];
	seq_printf(s, "  vmin:%5dmV%9lukHz%s\n",
		   td->lut_uv[td->lut_bottom] / 1000,
		   (r ? : td->out_rate_min) / 1000,
		   r ? " (calibrated)"  : "");

	return 0;
}

static int profiles_open(struct inode *inode, struct file *file)
{
	return single_open(file, profiles_show, inode->i_private);
}

static const struct file_operations profiles_fops = {
	.open		= profiles_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct {
	char				*name;
	umode_t				mode;
	const struct file_operations	*fops;
} dfll_debugfs_nodes[] = {
	{ "enable", S_IRUGO | S_IWUSR, &enable_fops },
	{ "lock", S_IRUGO | S_IWUSR, &lock_fops },
	{ "force_out_mv", S_IRUGO | S_IWUSR, &fout_mv_fops },
	{ "pmu_undershoot_gb", S_IRUGO | S_IWUSR, &undershoot_fops },
	{ "tune_high_mv", S_IRUGO | S_IWUSR, &tune_high_mv_fops },
	{ "calibr_delay", S_IRUGO | S_IWUSR, &calibr_delay_fops },
	{ "rate", S_IRUGO, &rate_fops },
	{ "dvco_rate_min", S_IRUGO, &dvco_rate_min_fops },
	{ "registers", S_IRUGO, &registers_fops },
	{ "vmin_mv", S_IRUGO, &vmin_fops },
	{ "vmax_mv", S_IRUGO, &vmax_fops },
	{ "output_mv", S_IRUGO, &output_fops },
	{ "profiles", S_IRUGO, &profiles_fops },
};

static int dfll_debug_init(struct tegra_dfll *td)
{
	int i;

	if (!td || (td->mode == DFLL_UNINITIALIZED))
		return 0;

	td->debugfs_dir = debugfs_create_dir("tegra_dfll_fcpu", NULL);
	if (!td->debugfs_dir)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(dfll_debugfs_nodes); i++) {
		if (!debugfs_create_file(dfll_debugfs_nodes[i].name,
					 dfll_debugfs_nodes[i].mode,
					 td->debugfs_dir, td,
					 dfll_debugfs_nodes[i].fops))
			goto err_out;
	}

	if (!debugfs_create_x32("flags", S_IRUGO, td->debugfs_dir,
				&td->cfg_flags))
		goto err_out;

	debugfs_create_symlink("monitor", td->debugfs_dir, "rate");
	debugfs_create_symlink("dvco_rate", td->debugfs_dir, "dvco_rate_min");

	clk_register_clkdev(td->dfll_clk, td->output_clock_name,
			    "tegra-clk-debug");
	return 0;

err_out:
	debugfs_remove_recursive(td->debugfs_dir);
	return -ENOMEM;
}

#endif /* CONFIG_DEBUG_FS */
/*
 * API exported to per-SoC platform drivers
 */

/**
 * tegra_dfll_register - probe a Tegra DFLL device
 * @pdev: DFLL platform_device *
 * @soc: Per-SoC integration and characterization data for this DFLL instance
 *
 * Probe and initialize a DFLL device instance. Intended to be called
 * by a SoC-specific shim driver that passes in per-SoC integration
 * and configuration data via @soc. Returns 0 on success or -err on failure.
 */
int tegra_dfll_register(struct platform_device *pdev,
			struct tegra_dfll_soc_data *soc)
{
	struct resource *mem;
	struct tegra_dfll *td;
	int ret, t_floor_size;

	if (!soc) {
		dev_err(&pdev->dev, "no tegra_dfll_soc_data provided\n");
		return -EINVAL;
	}

	td = devm_kzalloc(&pdev->dev, sizeof(*td), GFP_KERNEL);
	if (!td)
		return -ENOMEM;

	t_floor_size = sizeof(unsigned long) *
				(soc->thermal_floor_table_size + 1);
	td->dvco_rate_floors = devm_kzalloc(&pdev->dev, t_floor_size,
					    GFP_KERNEL);
	if (!td->dvco_rate_floors)
		return -ENOMEM;

	td->dev = &pdev->dev;
	platform_set_drvdata(pdev, td);

	td->soc = soc;

	ret = dfll_dt_parse(td);
	if (ret) {
		dev_err(td->dev, "parse device-tree failed\n");
		return ret;
	}

	td->dvco_rst = devm_reset_control_get(td->dev, "dvco");
	if (IS_ERR(td->dvco_rst)) {
		dev_err(td->dev, "couldn't get dvco reset\n");
		return PTR_ERR(td->dvco_rst);
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(td->dev, "no control register resource\n");
		return -ENODEV;
	}

	td->base = devm_ioremap(td->dev, mem->start, resource_size(mem));
	if (!td->base) {
		dev_err(td->dev, "couldn't ioremap DFLL control registers\n");
		return -ENODEV;
	}

	if (td->pmu_if == TEGRA_DFLL_PMU_I2C) {
		mem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!mem) {
			dev_err(td->dev, "no i2c_base resource\n");
			return -ENODEV;
		}

		td->i2c_base = devm_ioremap(td->dev, mem->start,
					resource_size(mem));
		if (!td->i2c_base) {
			dev_err(td->dev, "couldn't ioremap i2c_base resource\n");
			return -ENODEV;
		}

		mem = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		if (!mem) {
			dev_err(td->dev, "no i2c_controller_base resource\n");
			return -ENODEV;
		}

		td->i2c_controller_base = devm_ioremap(td->dev, mem->start,
					       resource_size(mem));
		if (!td->i2c_controller_base) {
			dev_err(td->dev,
				"couldn't ioremap i2c_controller_base resource\n");
			return -ENODEV;
		}

		mem = platform_get_resource(pdev, IORESOURCE_MEM, 3);
		if (!mem) {
			dev_err(td->dev, "no lut_base resource\n");
			return -ENODEV;
		}

		td->lut_base = devm_ioremap(td->dev, mem->start,
					resource_size(mem));
		if (!td->lut_base) {
			dev_err(td->dev,
				"couldn't ioremap lut_base resource\n");
			return -ENODEV;
		}
	}

	ret = dfll_init_clks(td);
	if (ret) {
		dev_err(&pdev->dev, "DFLL clock init error\n");
		return ret;
	}

	/* Enable the clocks and set the device up */
	ret = dfll_init(td);
	if (ret)
		return ret;

	spin_lock_init(&td->lock);

	ret = dfll_register_clk(td);
	if (ret) {
		dev_err(&pdev->dev, "DFLL clk registration failed\n");
		return ret;
	}

	/* Initialize tuning timer */
	init_timer(&td->tune_timer);
	td->tune_timer.function = dfll_tune_timer_cb;
	td->tune_timer.data = (unsigned long)td;
	td->tune_delay = usecs_to_jiffies(DFLL_TUNE_HIGH_DELAY);

	/* Initialize Vmin calibration timer */
	init_timer_deferrable(&td->calibration_timer);
	td->calibration_timer.function = calibration_timer_cb;
	td->calibration_timer.data = (unsigned long)td;
	if (!(td->cfg_flags & DFLL_ONE_SHOT_CALIBRATE))
		td->calibration_delay = usecs_to_jiffies(DFLL_CALIBR_TIME);

#ifdef CONFIG_DEBUG_FS
	dfll_debug_init(td);
#endif

	tegra_dfll_dev = td;

	return 0;
}
EXPORT_SYMBOL(tegra_dfll_register);

/**
 * tegra_dfll_unregister - release all of the DFLL driver resources for a device
 * @pdev: DFLL platform_device *
 *
 * Unbind this driver from the DFLL hardware device represented by
 * @pdev. The DFLL must be disabled for this to succeed. Returns 0
 * upon success or -EBUSY if the DFLL is still active.
 */
int tegra_dfll_unregister(struct platform_device *pdev)
{
	struct tegra_dfll *td = platform_get_drvdata(pdev);

	/* Try to prevent removal while the DFLL is active */
	if (td->mode != DFLL_DISABLED) {
		dev_err(&pdev->dev,
			"must disable DFLL before removing driver\n");
		return -EBUSY;
	}

	tegra_dfll_dev = NULL;

	debugfs_remove_recursive(td->debugfs_dir);

	dfll_unregister_clk(td);
	pm_runtime_disable(&pdev->dev);

	clk_unprepare(td->ref_clk);
	clk_unprepare(td->soc_clk);
	clk_unprepare(td->i2c_clk);

	reset_control_assert(td->dvco_rst);

	return 0;
}
EXPORT_SYMBOL(tegra_dfll_unregister);
