/*
 * rt5677.c  --  RT5677 ALSA SoC audio codec driver
 *
 * Copyright 2013 Realtek Semiconductor Corp.
 * Author: Bard Liao <bardliao@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/of_gpio.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <linux/htc_headset_mgr.h>

#define RTK_IOCTL
#define RT5677_DMIC_CLK_MAX 2400000
#define RT5677_PRIV_MIC_BUF_SIZE (64 * 1024)

#ifdef RTK_IOCTL
#if defined(CONFIG_SND_HWDEP) || defined(CONFIG_SND_HWDEP_MODULE)
#include "rt_codec_ioctl.h"
#include "rt5677_ioctl.h"
#endif
#endif

#include "rt5677.h"
#include "rt5677-spi.h"

#define VERSION "0.0.2 alsa 1.0.25"
#define RT5677_PATH "rt5677_"

static int dmic_depop_time = 100;
module_param(dmic_depop_time, int, 0644);
static int amic_depop_time = 150;
module_param(amic_depop_time, int, 0644);

static struct rt5677_priv *rt5677_global;

struct rt5677_init_reg {
	u8 reg;
	u16 val;
};

static char *dsp_vad_suffix = "vad";
module_param(dsp_vad_suffix, charp, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(dsp_vad_suffix, "DSP VAD Firmware Suffix");

extern void set_rt5677_power_extern(bool enable);
extern int get_mic_state(void);

static struct rt5677_init_reg init_list[] = {
	{RT5677_DIG_MISC		, 0x0021},
	{RT5677_PRIV_INDEX		, 0x003d},
	{RT5677_PRIV_DATA		, 0x364e},
	{RT5677_PWR_DSP2		, 0x0c00},
	/* 64Fs in TDM mode */
	{RT5677_TDM1_CTRL1		, 0x1300},
	{RT5677_MONO_ADC_DIG_VOL	, 0xafaf},

	/* MX80 bit10 0:MCLK1 1:MCLK2 */
	{RT5677_GLB_CLK1		, 0x0400},

	/* Playback Start */
	{RT5677_PRIV_INDEX		, 0x0017},
	{RT5677_PRIV_DATA		, 0x4fc0},
	{RT5677_PRIV_INDEX		, 0x0013},
	{RT5677_PRIV_DATA		, 0x0632},
	{RT5677_LOUT1			, 0xf800},
	{RT5677_STO1_DAC_MIXER		, 0x8a8a},
	/* Playback End */

	/* Record Start */
	{RT5677_PRIV_INDEX		, 0x001e},
	{RT5677_PRIV_DATA		, 0x0000},
	{RT5677_PRIV_INDEX		, 0x0012},
	{RT5677_PRIV_DATA		, 0x0eaa},
	{RT5677_PRIV_INDEX		, 0x0014},
	{RT5677_PRIV_DATA		, 0x0f8b},
	{RT5677_IN1			, 0x0040},
	{RT5677_MICBIAS			, 0x4000},
	{RT5677_MONO_ADC_MIXER		, 0xd4d5},
	{RT5677_TDM1_CTRL2		, 0x0106},
	/* Record End */
};
#define RT5677_INIT_REG_LEN ARRAY_SIZE(init_list)

static int rt5677_reg_init(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int i;

	for (i = 0; i < RT5677_INIT_REG_LEN; i++)
		regmap_write(rt5677->regmap, init_list[i].reg,
			init_list[i].val);

	return 0;
}

static int rt5677_index_sync(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int i;

	for (i = 0; i < RT5677_INIT_REG_LEN; i++)
		if (RT5677_PRIV_INDEX == init_list[i].reg ||
			RT5677_PRIV_DATA == init_list[i].reg)
			regmap_write(rt5677->regmap, init_list[i].reg,
					init_list[i].val);
	return 0;
}

static const struct reg_default rt5677_reg[] = {
	{RT5677_RESET			, 0x0000},
	{RT5677_LOUT1			, 0xa800},
	{RT5677_IN1			, 0x0000},
	{RT5677_MICBIAS			, 0x0000},
	{RT5677_SLIMBUS_PARAM		, 0x0000},
	{RT5677_SLIMBUS_RX		, 0x0000},
	{RT5677_SLIMBUS_CTRL		, 0x0000},
	{RT5677_SIDETONE_CTRL		, 0x000b},
	{RT5677_ANA_DAC1_2_3_SRC	, 0x0000},
	{RT5677_IF_DSP_DAC3_4_MIXER	, 0x1111},
	{RT5677_DAC4_DIG_VOL		, 0xafaf},
	{RT5677_DAC3_DIG_VOL		, 0xafaf},
	{RT5677_DAC1_DIG_VOL		, 0xafaf},
	{RT5677_DAC2_DIG_VOL		, 0xafaf},
	{RT5677_IF_DSP_DAC2_MIXER	, 0x0011},
	{RT5677_STO1_ADC_DIG_VOL	, 0x2f2f},
	{RT5677_MONO_ADC_DIG_VOL	, 0x2f2f},
	{RT5677_STO1_2_ADC_BST		, 0x0000},
	{RT5677_STO2_ADC_DIG_VOL	, 0x2f2f},
	{RT5677_ADC_BST_CTRL2		, 0x0000},
	{RT5677_STO3_4_ADC_BST		, 0x0000},
	{RT5677_STO3_ADC_DIG_VOL	, 0x2f2f},
	{RT5677_STO4_ADC_DIG_VOL	, 0x2f2f},
	{RT5677_STO4_ADC_MIXER		, 0xd4c0},
	{RT5677_STO3_ADC_MIXER		, 0xd4c0},
	{RT5677_STO2_ADC_MIXER		, 0xd4c0},
	{RT5677_STO1_ADC_MIXER		, 0xd4c0},
	{RT5677_MONO_ADC_MIXER		, 0xd4d1},
	{RT5677_ADC_IF_DSP_DAC1_MIXER	, 0x8080},
	{RT5677_STO1_DAC_MIXER		, 0xaaaa},
	{RT5677_MONO_DAC_MIXER		, 0xaaaa},
	{RT5677_DD1_MIXER		, 0xaaaa},
	{RT5677_DD2_MIXER		, 0xaaaa},
	{RT5677_IF3_DATA		, 0x0000},
	{RT5677_IF4_DATA		, 0x0000},
	{RT5677_PDM_OUT_CTRL		, 0x8888},
	{RT5677_PDM_DATA_CTRL1		, 0x0000},
	{RT5677_PDM_DATA_CTRL2		, 0x0000},
	{RT5677_PDM1_DATA_CTRL2		, 0x0000},
	{RT5677_PDM1_DATA_CTRL3		, 0x0000},
	{RT5677_PDM1_DATA_CTRL4		, 0x0000},
	{RT5677_PDM2_DATA_CTRL2		, 0x0000},
	{RT5677_PDM2_DATA_CTRL3		, 0x0000},
	{RT5677_PDM2_DATA_CTRL4		, 0x0000},
	{RT5677_TDM1_CTRL1		, 0x0300},
	{RT5677_TDM1_CTRL2		, 0x0000},
	{RT5677_TDM1_CTRL3		, 0x4000},
	{RT5677_TDM1_CTRL4		, 0x0123},
	{RT5677_TDM1_CTRL5		, 0x4567},
	{RT5677_TDM2_CTRL1		, 0x0300},
	{RT5677_TDM2_CTRL2		, 0x0000},
	{RT5677_TDM2_CTRL3		, 0x4000},
	{RT5677_TDM2_CTRL4		, 0x0123},
	{RT5677_TDM2_CTRL5		, 0x4567},
	{RT5677_I2C_MASTER_CTRL1	, 0x0001},
	{RT5677_I2C_MASTER_CTRL2	, 0x0000},
	{RT5677_I2C_MASTER_CTRL3	, 0x0000},
	{RT5677_I2C_MASTER_CTRL4	, 0x0000},
	{RT5677_I2C_MASTER_CTRL5	, 0x0000},
	{RT5677_I2C_MASTER_CTRL6	, 0x0000},
	{RT5677_I2C_MASTER_CTRL7	, 0x0000},
	{RT5677_I2C_MASTER_CTRL8	, 0x0000},
	{RT5677_DMIC_CTRL1		, 0x1505},
	{RT5677_DMIC_CTRL2		, 0x0055},
	{RT5677_HAP_GENE_CTRL1		, 0x0111},
	{RT5677_HAP_GENE_CTRL2		, 0x0064},
	{RT5677_HAP_GENE_CTRL3		, 0xef0e},
	{RT5677_HAP_GENE_CTRL4		, 0xf0f0},
	{RT5677_HAP_GENE_CTRL5		, 0xef0e},
	{RT5677_HAP_GENE_CTRL6		, 0xf0f0},
	{RT5677_HAP_GENE_CTRL7		, 0xef0e},
	{RT5677_HAP_GENE_CTRL8		, 0xf0f0},
	{RT5677_HAP_GENE_CTRL9		, 0xf000},
	{RT5677_HAP_GENE_CTRL10		, 0x0000},
	{RT5677_PWR_DIG1		, 0x0000},
	{RT5677_PWR_DIG2		, 0x0000},
	{RT5677_PWR_ANLG1		, 0x0055},
	{RT5677_PWR_ANLG2		, 0x0000},
	{RT5677_PWR_DSP1		, 0x0001},
	{RT5677_PWR_DSP_ST		, 0x0000},
	{RT5677_PWR_DSP2		, 0x0000},
	{RT5677_ADC_DAC_HPF_CTRL1	, 0x0e00},
	{RT5677_PRIV_INDEX		, 0x0000},
	{RT5677_PRIV_DATA		, 0x0000},
	{RT5677_I2S4_SDP		, 0x8000},
	{RT5677_I2S1_SDP		, 0x8000},
	{RT5677_I2S2_SDP		, 0x8000},
	{RT5677_I2S3_SDP		, 0x8000},
	{RT5677_CLK_TREE_CTRL1		, 0x1111},
	{RT5677_CLK_TREE_CTRL2		, 0x1111},
	{RT5677_CLK_TREE_CTRL3		, 0x0000},
	{RT5677_PLL1_CTRL1		, 0x0000},
	{RT5677_PLL1_CTRL2		, 0x0000},
	{RT5677_PLL2_CTRL1		, 0x0c60},
	{RT5677_PLL2_CTRL2		, 0x2000},
	{RT5677_GLB_CLK1		, 0x0000},
	{RT5677_GLB_CLK2		, 0x0000},
	{RT5677_ASRC_1			, 0x0000},
	{RT5677_ASRC_2			, 0x0000},
	{RT5677_ASRC_3			, 0x0000},
	{RT5677_ASRC_4			, 0x0000},
	{RT5677_ASRC_5			, 0x0000},
	{RT5677_ASRC_6			, 0x0000},
	{RT5677_ASRC_7			, 0x0000},
	{RT5677_ASRC_8			, 0x0000},
	{RT5677_ASRC_9			, 0x0000},
	{RT5677_ASRC_10			, 0x0000},
	{RT5677_ASRC_11			, 0x0000},
	{RT5677_ASRC_12			, 0x0008},
	{RT5677_ASRC_13			, 0x0000},
	{RT5677_ASRC_14			, 0x0000},
	{RT5677_ASRC_15			, 0x0000},
	{RT5677_ASRC_16			, 0x0000},
	{RT5677_ASRC_17			, 0x0000},
	{RT5677_ASRC_18			, 0x0000},
	{RT5677_ASRC_19			, 0x0000},
	{RT5677_ASRC_20			, 0x0000},
	{RT5677_ASRC_21			, 0x000c},
	{RT5677_ASRC_22			, 0x0000},
	{RT5677_ASRC_23			, 0x0000},
	{RT5677_VAD_CTRL1		, 0x2184},
	{RT5677_VAD_CTRL2		, 0x010a},
	{RT5677_VAD_CTRL3		, 0x0aea},
	{RT5677_VAD_CTRL4		, 0x000c},
	{RT5677_VAD_CTRL5		, 0x0000},
	{RT5677_DSP_INB_CTRL1		, 0x0000},
	{RT5677_DSP_INB_CTRL2		, 0x0000},
	{RT5677_DSP_IN_OUTB_CTRL	, 0x0000},
	{RT5677_DSP_OUTB0_1_DIG_VOL	, 0x2f2f},
	{RT5677_DSP_OUTB2_3_DIG_VOL	, 0x2f2f},
	{RT5677_DSP_OUTB4_5_DIG_VOL	, 0x2f2f},
	{RT5677_DSP_OUTB6_7_DIG_VOL	, 0x2f2f},
	{RT5677_ADC_EQ_CTRL1		, 0x6000},
	{RT5677_ADC_EQ_CTRL2		, 0x0000},
	{RT5677_EQ_CTRL1		, 0xc000},
	{RT5677_EQ_CTRL2		, 0x0000},
	{RT5677_EQ_CTRL3		, 0x0000},
	{RT5677_SOFT_VOL_ZERO_CROSS1	, 0x0009},
	{RT5677_JD_CTRL1		, 0x0000},
	{RT5677_JD_CTRL2		, 0x0000},
	{RT5677_JD_CTRL3		, 0x0000},
	{RT5677_IRQ_CTRL1		, 0x0000},
	{RT5677_IRQ_CTRL2		, 0x0000},
	{RT5677_GPIO_ST			, 0x0000},
	{RT5677_GPIO_CTRL1		, 0x0000},
	{RT5677_GPIO_CTRL2		, 0x0000},
	{RT5677_GPIO_CTRL3		, 0x0000},
	{RT5677_STO1_ADC_HI_FILTER1	, 0xb320},
	{RT5677_STO1_ADC_HI_FILTER2	, 0x0000},
	{RT5677_MONO_ADC_HI_FILTER1	, 0xb300},
	{RT5677_MONO_ADC_HI_FILTER2	, 0x0000},
	{RT5677_STO2_ADC_HI_FILTER1	, 0xb300},
	{RT5677_STO2_ADC_HI_FILTER2	, 0x0000},
	{RT5677_STO3_ADC_HI_FILTER1	, 0xb300},
	{RT5677_STO3_ADC_HI_FILTER2	, 0x0000},
	{RT5677_STO4_ADC_HI_FILTER1	, 0xb300},
	{RT5677_STO4_ADC_HI_FILTER2	, 0x0000},
	{RT5677_MB_DRC_CTRL1		, 0x0f20},
	{RT5677_DRC1_CTRL1		, 0x001f},
	{RT5677_DRC1_CTRL2		, 0x020c},
	{RT5677_DRC1_CTRL3		, 0x1f00},
	{RT5677_DRC1_CTRL4		, 0x0000},
	{RT5677_DRC1_CTRL5		, 0x0000},
	{RT5677_DRC1_CTRL6		, 0x0029},
	{RT5677_DRC2_CTRL1		, 0x001f},
	{RT5677_DRC2_CTRL2		, 0x020c},
	{RT5677_DRC2_CTRL3		, 0x1f00},
	{RT5677_DRC2_CTRL4		, 0x0000},
	{RT5677_DRC2_CTRL5		, 0x0000},
	{RT5677_DRC2_CTRL6		, 0x0029},
	{RT5677_DRC1_HL_CTRL1		, 0x8000},
	{RT5677_DRC1_HL_CTRL2		, 0x0200},
	{RT5677_DRC2_HL_CTRL1		, 0x8000},
	{RT5677_DRC2_HL_CTRL2		, 0x0200},
	{RT5677_DSP_INB1_SRC_CTRL1	, 0x5800},
	{RT5677_DSP_INB1_SRC_CTRL2	, 0x0000},
	{RT5677_DSP_INB1_SRC_CTRL3	, 0x0000},
	{RT5677_DSP_INB1_SRC_CTRL4	, 0x0800},
	{RT5677_DSP_INB2_SRC_CTRL1	, 0x5800},
	{RT5677_DSP_INB2_SRC_CTRL2	, 0x0000},
	{RT5677_DSP_INB2_SRC_CTRL3	, 0x0000},
	{RT5677_DSP_INB2_SRC_CTRL4	, 0x0800},
	{RT5677_DSP_INB3_SRC_CTRL1	, 0x5800},
	{RT5677_DSP_INB3_SRC_CTRL2	, 0x0000},
	{RT5677_DSP_INB3_SRC_CTRL3	, 0x0000},
	{RT5677_DSP_INB3_SRC_CTRL4	, 0x0800},
	{RT5677_DSP_OUTB1_SRC_CTRL1	, 0x5800},
	{RT5677_DSP_OUTB1_SRC_CTRL2	, 0x0000},
	{RT5677_DSP_OUTB1_SRC_CTRL3	, 0x0000},
	{RT5677_DSP_OUTB1_SRC_CTRL4	, 0x0800},
	{RT5677_DSP_OUTB2_SRC_CTRL1	, 0x5800},
	{RT5677_DSP_OUTB2_SRC_CTRL2	, 0x0000},
	{RT5677_DSP_OUTB2_SRC_CTRL3	, 0x0000},
	{RT5677_DSP_OUTB2_SRC_CTRL4	, 0x0800},
	{RT5677_DSP_OUTB_0123_MIXER_CTRL, 0xfefe},
	{RT5677_DSP_OUTB_45_MIXER_CTRL	, 0xfefe},
	{RT5677_DSP_OUTB_67_MIXER_CTRL	, 0xfefe},
	{RT5677_DIG_MISC		, 0x0000},
	{RT5677_GEN_CTRL1		, 0x0000},
	{RT5677_GEN_CTRL2		, 0x0000},
	{RT5677_VENDOR_ID		, 0x0000},
	{RT5677_VENDOR_ID1		, 0x10ec},
	{RT5677_VENDOR_ID2		, 0x6327},
};

static int rt5677_reset(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	return regmap_write(rt5677->regmap, RT5677_RESET, 0x10ec);
}

/**
 * rt5677_index_write - Write private register.
 * @codec: SoC audio codec device.
 * @reg: Private register index.
 * @value: Private register data.
 *
 * Modify private register for advanced setting. It can be written through
 * private index (0x6a) and data (0x6c) register.
 *
 * Returns 0 for success or negative error code.
 */
static int rt5677_index_write(struct snd_soc_codec *codec,
		unsigned int reg, unsigned int value)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int ret;

	mutex_lock(&rt5677->index_lock);

	ret = regmap_write(rt5677->regmap, RT5677_PRIV_INDEX, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private addr: %d\n", ret);
		goto err;
	}
	ret = regmap_write(rt5677->regmap, RT5677_PRIV_DATA, value);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private value: %d\n", ret);
		goto err;
	}

	mutex_unlock(&rt5677->index_lock);

	return 0;

err:
	mutex_unlock(&rt5677->index_lock);

	return ret;
}

/**
 * rt5677_index_read - Read private register.
 * @codec: SoC audio codec device.
 * @reg: Private register index.
 *
 * Read advanced setting from private register. It can be read through
 * private index (0x6a) and data (0x6c) register.
 *
 * Returns private register value or negative error code.
 */
static unsigned int rt5677_index_read(
	struct snd_soc_codec *codec, unsigned int reg)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int ret;

	mutex_lock(&rt5677->index_lock);

	ret = regmap_write(rt5677->regmap, RT5677_PRIV_INDEX, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private addr: %d\n", ret);
		mutex_unlock(&rt5677->index_lock);
		return ret;
	}

	regmap_read(rt5677->regmap, RT5677_PRIV_DATA, &ret);

	mutex_unlock(&rt5677->index_lock);

	return ret;
}

/**
 * rt5677_index_update_bits - update private register bits
 * @codec: audio codec
 * @reg: Private register index.
 * @mask: register mask
 * @value: new value
 *
 * Writes new register value.
 *
 * Returns 1 for change, 0 for no change, or negative error code.
 */
static int rt5677_index_update_bits(struct snd_soc_codec *codec,
	unsigned int reg, unsigned int mask, unsigned int value)
{
	unsigned int old, new;
	int change, ret;

	ret = rt5677_index_read(codec, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read private reg: %d\n", ret);
		goto err;
	}

	old = ret;
	new = (old & ~mask) | (value & mask);
	change = old != new;
	if (change) {
		ret = rt5677_index_write(codec, reg, new);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write private reg: %d\n", ret);
			goto err;
		}
	}
	return change;

err:
	return ret;
}

/**
 * rt5677_dsp_mode_i2c_write_address - Write value to address on DSP mode.
 * @codec: SoC audio codec device.
 * @address: Target address.
 * @value: Target data.
 *
 *
 * Returns 0 for success or negative error code.
 */
static int rt5677_dsp_mode_i2c_write_address(struct snd_soc_codec *codec,
		unsigned int address, unsigned int value, unsigned int opcode)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int ret;

	mutex_lock(&rt5677->index_lock);

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_ADDR_MSB,
			   address >> 16);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set addr msb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_ADDR_LSB,
			   address & 0xFFFF);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set addr lsb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_DATA_MSB,
			   value >> 16);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set data msb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_DATA_LSB,
			   value & 0xFFFF);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set data lsb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_OP_CODE, opcode);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set op code value: %d\n", ret);
		goto err;
	}

err:
	mutex_unlock(&rt5677->index_lock);

	return ret;
}

/**
 * rt5677_dsp_mode_i2c_read_address - Read value from address on DSP mode.
 * @codec: SoC audio codec device.
 * @addr: Address index.
 * @value: Address data.
 * Returns 0 for success or negative error code.
 */
static int rt5677_dsp_mode_i2c_read_address(
	struct snd_soc_codec *codec, unsigned int addr, unsigned int *value)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int ret;
	unsigned int msb, lsb;

	mutex_lock(&rt5677->index_lock);

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_ADDR_MSB,
		(addr & 0xffff0000) >> 16);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set addr msb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_ADDR_LSB,
		addr & 0xffff);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set addr lsb value: %d\n", ret);
		goto err;
	}

	ret = regmap_write(rt5677->regmap, RT5677_DSP_I2C_OP_CODE, 0x0002);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set op code value: %d\n", ret);
		goto err;
	}

	regmap_read(rt5677->regmap, RT5677_DSP_I2C_DATA_MSB, &msb);
	regmap_read(rt5677->regmap, RT5677_DSP_I2C_DATA_LSB, &lsb);
	*value = (msb << 16) | lsb;

err:
	mutex_unlock(&rt5677->index_lock);

	return ret;

}

/**
 * rt5677_dsp_mode_i2c_write - Write register on DSP mode.
 * @codec: SoC audio codec device.
 * @reg: Register index.
 * @value: Register data.
 *
 *
 * Returns 0 for success or negative error code.
 */
static int rt5677_dsp_mode_i2c_write(struct snd_soc_codec *codec,
		unsigned int reg, unsigned int value)
{
	return rt5677_dsp_mode_i2c_write_address(codec, 0x18020000 + reg * 2,
		value, 0x0001);
}

/**
 * rt5677_dsp_mode_i2c_read - Read register on DSP mode.
 * @codec: SoC audio codec device.
 * @reg: Register index.
 *
 *
 * Returns Register value.
 */
static unsigned int rt5677_dsp_mode_i2c_read(
	struct snd_soc_codec *codec, unsigned int reg)
{
	unsigned int value;

	rt5677_dsp_mode_i2c_read_address(codec, 0x18020000 + reg * 2,
		&value);
	return value & 0xffff;
}
/**
 * rt5677_dsp_mode_i2c_update_bits - update register on DSP mode.
 * @codec: audio codec
 * @reg: register index.
 * @mask: register mask
 * @value: new value
 *
 *
 * Returns 1 for change, 0 for no change, or negative error code.
 */
static int rt5677_dsp_mode_i2c_update_bits(struct snd_soc_codec *codec,
	unsigned int reg, unsigned int mask, unsigned int value)
{
	unsigned int old, new;
	int change, ret;

	ret = rt5677_dsp_mode_i2c_read(codec, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read private reg: %d\n", ret);
		goto err;
	}

	old = ret;
	new = (old & ~mask) | (value & mask);
	change = old != new;
	if (change) {
		ret = rt5677_dsp_mode_i2c_write(codec, reg, new);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write private reg: %d\n", ret);
			goto err;
		}
	}
	return change;

err:
	return ret;
}

static unsigned int rt5677_dsp_mbist_test(struct snd_soc_codec *codec)
{
	unsigned int i, value;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	rt5677->mbist_test_passed = true;

	for (i = 0; i < 0x40; i += 4) {
		rt5677_dsp_mode_i2c_write_address(codec, 0x1801f010 + i, 0x20,
			0x0003);
		rt5677_dsp_mode_i2c_write_address(codec, 0x1801f010 + i, 0x0,
			0x0003);
		rt5677_dsp_mode_i2c_write_address(codec, 0x1801f010 + i, 0x1,
			0x0003);
	}

	for (i = 0; i < 0x40; i += 4) {
		rt5677_dsp_mode_i2c_read_address(codec, 0x1801f010 + i, &value);
		if (value != 0x101) {
			pr_err("rt5677 MBIST test failed\n");
			rt5677->mbist_test_passed = false;
			break;
		}
	}

	rt5677_dsp_mode_i2c_write(codec, RT5677_PWR_DSP1, 0x07fd);
	regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2, RT5677_PWR_LDO1, 0);
	msleep(200);
	regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2, RT5677_PWR_LDO1, RT5677_PWR_LDO1);
	regmap_write(rt5677->regmap, RT5677_PWR_DSP1, 0x07ff);

	return 0;
}

static unsigned int rt5677_read_dsp_code_from_file(const struct firmware **fwp,
			const char *file_path, struct snd_soc_codec *codec)
{
	int ret;

	pr_debug("%s\n", __func__);

	ret = request_firmware(fwp, file_path, codec->dev);
	if (ret != 0) {
		pr_err("Failed to request '%s' = %d\n", file_path, ret);
		return 0;
	}
	return (*fwp)->size;
}

static unsigned int rt5677_set_vad_source(
	struct snd_soc_codec *codec, unsigned int source)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (source) {
	case RT5677_VAD_IDLE_DMIC1:
	case RT5677_VAD_SUSPEND_DMIC1:
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL2, 0x013f);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL3, 0x0a00);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL4, 0x017f);
		regmap_write(rt5677->regmap, RT5677_ADC_BST_CTRL2, 0xa000);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL2, 0x7777);
		regmap_write(rt5677->regmap, RT5677_MONO_ADC_MIXER, 0x54d1);
		regmap_update_bits(rt5677->regmap, RT5677_MONO_ADC_DIG_VOL, RT5677_L_MUTE, 0);
		regmap_write(rt5677->regmap, RT5677_DSP_INB_CTRL1, 0x4000);
		regmap_write(rt5677->regmap, RT5677_DIG_MISC, 0x0001);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL1, 0x2777);
		regmap_write(rt5677->regmap, RT5677_GLB_CLK2, 0x0080);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG1, 0x0027);

		/* from MCLK1 */
		regmap_write(rt5677->regmap, RT5677_GLB_CLK1, 0x0080);

		regmap_write(rt5677->regmap, RT5677_DMIC_CTRL1, 0x95a5);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL1, 0x273c);
		regmap_write(rt5677->regmap, RT5677_IRQ_CTRL2, 0x4000);
		rt5677_index_write(codec, 0x14, 0x018a);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG2, 0x4000);
		regmap_write(rt5677->regmap, RT5677_GPIO_CTRL2, 0x6000);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG2, 0x0081);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP2, 0x07ff);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP1, 0x07ff);
		break;
	case RT5677_VAD_IDLE_DMIC2:
	case RT5677_VAD_SUSPEND_DMIC2:
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL2, 0x013f);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL3, 0x0a00);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL4, 0x017f);
		regmap_write(rt5677->regmap, RT5677_ADC_BST_CTRL2, 0xa000);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL2, 0x7777);
		regmap_write(rt5677->regmap, RT5677_MONO_ADC_MIXER, 0x55d1);
		regmap_update_bits(rt5677->regmap, RT5677_MONO_ADC_DIG_VOL, RT5677_L_MUTE, 0);
		regmap_write(rt5677->regmap, RT5677_DSP_INB_CTRL1, 0x4000);
		regmap_write(rt5677->regmap, RT5677_DIG_MISC, 0x0001);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL1, 0x2777);
		regmap_write(rt5677->regmap, RT5677_GLB_CLK2, 0x0080);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG1, 0x0027);

		/* from MCLK1 */
		regmap_write(rt5677->regmap, RT5677_GLB_CLK1, 0x0080);

		regmap_write(rt5677->regmap, RT5677_DMIC_CTRL1, 0x55a5);
		regmap_update_bits(rt5677->regmap, RT5677_GEN_CTRL2, 0x0200,
			0x0200);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL1, 0x273c);
		regmap_write(rt5677->regmap, RT5677_IRQ_CTRL2, 0x4000);
		rt5677_index_write(codec, 0x14, 0x018a);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG2, 0x4000);
		regmap_write(rt5677->regmap, RT5677_GPIO_CTRL2, 0x6000);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG2, 0x0081);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP2, 0x07ff);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP1, 0x07ff);
		break;
	case RT5677_VAD_IDLE_AMIC:
	case RT5677_VAD_SUSPEND_AMIC:
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL2, 0x013f);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL3, 0x0a00);
		regmap_write(rt5677->regmap, RT5677_VAD_CTRL4, 0x027f);
		regmap_write(rt5677->regmap, RT5677_IN1, 0x01c0);
		regmap_write(rt5677->regmap, RT5677_ADC_BST_CTRL2, 0xa000);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL2, 0x7777);
		regmap_write(rt5677->regmap, RT5677_MONO_ADC_MIXER, 0xd451);
		regmap_update_bits(rt5677->regmap, RT5677_MONO_ADC_DIG_VOL, RT5677_R_MUTE, 0);
		regmap_write(rt5677->regmap, RT5677_DSP_INB_CTRL1, 0x4000);
		regmap_write(rt5677->regmap, RT5677_DIG_MISC, 0x0001);
		regmap_write(rt5677->regmap, RT5677_CLK_TREE_CTRL1, 0x2777);
		regmap_write(rt5677->regmap, RT5677_GLB_CLK2, 0x0080);

		/* from MCLK1 */
		regmap_write(rt5677->regmap, RT5677_GLB_CLK1, 0x0080);

		regmap_write(rt5677->regmap, RT5677_VAD_CTRL1, 0x273c);
		regmap_write(rt5677->regmap, RT5677_IRQ_CTRL2, 0x4000);
		rt5677_index_write(codec, 0x14, 0x018a);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG1, 0xa927);
		msleep(20);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG1, 0xe9a7);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG1, 0x0012);
		rt5677_index_write(codec, RT5677_CHOP_DAC_ADC, 0x364e);
		rt5677_index_write(codec, RT5677_ANA_ADC_GAIN_CTRL, 0x0000);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG2, 0x2000);
		regmap_write(rt5677->regmap, RT5677_GPIO_CTRL2, 0x6000);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG2, 0x4091);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP2, 0x07ff);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP1, 0x07ff);
		break;
	default:
		break;
	}

	return 0;
}

static int rt5677_parse_and_load_dsp(const u8 *buf, unsigned int len)
{
	Elf32_Ehdr *elf_hdr;
	Elf32_Phdr *pr_hdr;
	Elf32_Half i;

	if (!buf || (len < sizeof(Elf32_Ehdr)))
		return -ENOMEM;

	elf_hdr = (Elf32_Ehdr *)buf;
#ifdef DEBUG
#ifndef EM_XTENSA
#define EM_XTENSA	94
#endif
	if (strncmp(elf_hdr->e_ident, ELFMAG, sizeof(ELFMAG) - 1) != 0)
		pr_err("Wrong ELF header prefix\n");
	if (elf_hdr->e_ehsize != sizeof(Elf32_Ehdr))
		pr_err("Wrong Elf header size\n");
	if (elf_hdr->e_machine != EM_XTENSA)
		pr_err("Wrong DSP code file\n");
#endif
	if (len < elf_hdr->e_phoff)
		return -ENOMEM;
	pr_hdr = (Elf32_Phdr *)(buf + elf_hdr->e_phoff);
	for (i=0; i < elf_hdr->e_phnum; i++) {
		/* TODO: handle p_memsz != p_filesz */
		if (pr_hdr->p_paddr && pr_hdr->p_filesz) {
			pr_debug("Load [0x%x] -> 0x%x\n", pr_hdr->p_filesz,
			       pr_hdr->p_paddr);
			rt5677_spi_write(pr_hdr->p_paddr,
					 buf + pr_hdr->p_offset,
					 pr_hdr->p_filesz);
		}
		pr_hdr++;
	}
	return 0;
}

static int rt5677_load_dsp_from_file(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int len;
	const struct firmware *fwp;
	char file_path[64];
	int err = 0;
#ifdef DEBUG
	int ret = 0;
#endif

	sprintf(file_path, RT5677_PATH "elf_%s", dsp_vad_suffix);
	len = rt5677_read_dsp_code_from_file(&fwp, file_path, codec);
	if (len) {
		pr_debug("load %s [%u] ok\n", file_path, len);
		err = rt5677_parse_and_load_dsp(fwp->data, len);
		release_firmware(fwp);
	} else {
		sprintf(file_path, RT5677_PATH "0x50000000_%s",
			dsp_vad_suffix);
		len = rt5677_read_dsp_code_from_file(&fwp, file_path, codec);
		if (len) {
			pr_debug("load %s ok\n", file_path);
			rt5677_spi_write(0x50000000, fwp->data, len);
			release_firmware(fwp);
		} else {
			pr_err("load %s fail\n", file_path);
		}

		sprintf(file_path, RT5677_PATH "0x60000000_%s",
			dsp_vad_suffix);
		len = rt5677_read_dsp_code_from_file(&fwp, file_path, codec);
		if (len) {
			pr_debug("load %s ok\n", file_path);
			rt5677_spi_write(0x60000000, fwp->data, len);
			release_firmware(fwp);
		} else {
			pr_err("load %s fail\n", file_path);
		}
	}
	if (rt5677->model_buf && rt5677->model_len) {
		err = rt5677_spi_write(RT5677_MODEL_ADDR, rt5677->model_buf,
				       rt5677->model_len);
		if (err)
			pr_err("model load failed: %u\n", err);
	}

#ifdef DEBUG
	msleep(50);
	regmap_write(rt5677->regmap, 0x01, 0x0520);
	regmap_write(rt5677->regmap, 0x02, 0x5ffe);
	regmap_write(rt5677->regmap, 0x00, 0x0002);
	regmap_read(rt5677->regmap, 0x03, &ret);
	dev_err(codec->dev, "rt5677 0x5ffe0520 0x03 %x\n", ret);
	regmap_read(rt5677->regmap, 0x04, &ret);
	dev_err(codec->dev, "rt5677 0x5ffe0520 0x04 %x\n", ret);

	regmap_write(rt5677->regmap, 0x01, 0x0000);
	regmap_write(rt5677->regmap, 0x02, 0x6000);
	regmap_write(rt5677->regmap, 0x00, 0x0002);
	regmap_read(rt5677->regmap, 0x03, &ret);
	dev_err(codec->dev, "rt5677 0x60000000 0x03 %x\n", ret);
	regmap_read(rt5677->regmap, 0x04, &ret);
	dev_err(codec->dev, "rt5677 0x60000000 0x04 %x\n", ret);
#endif
	return err;
}

static unsigned int rt5677_set_vad(
	struct snd_soc_codec *codec, unsigned int on, bool use_lock)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	static bool activity;
	int pri99 = 0;

	if (use_lock)
		mutex_lock(&rt5677_global->vad_lock);

	if (on && !activity) {
		/* Kill the DSP so that all the registers are clean. */
		set_rt5677_power_extern(false);

		pr_info("rt5677_set_vad on, mic_state = %d\n", rt5677_global->mic_state);
		set_rt5677_power_extern(true);

		gpio_direction_output(rt5677->vad_clock_en, 1);
		activity = true;
		regcache_cache_only(rt5677->regmap, false);
		regcache_cache_bypass(rt5677->regmap, true);

		if (rt5677_global->mic_state == RT5677_VAD_ENABLE_AMIC)
			rt5677_set_vad_source(codec, RT5677_VAD_IDLE_AMIC);
		else
			rt5677_set_vad_source(codec, RT5677_VAD_IDLE_DMIC1);

		if (!rt5677->mbist_test) {
			rt5677_dsp_mbist_test(codec);
			rt5677->mbist_test = true;
		}

		/* Set PRI-99 with any flags that are passed to the firmware. */
		rt5677_dsp_mode_i2c_write(codec, RT5677_PRIV_INDEX, 0x99);
		/* The firmware knows what power state to use given the result
		 * of the MBIST test - if it passed, it will enter a low power
		 * wake, otherwise it will run at full power. */
		pri99 |= rt5677->mbist_test_passed ?
			(RT5677_MBIST_TEST_PASSED) : (RT5677_MBIST_TEST_FAILED);
		/* Inform the firmware if it should go to sleep or not. */
		pri99 |= rt5677->vad_sleep ?
			(RT5677_VAD_SLEEP) : (RT5677_VAD_NO_SLEEP);
		rt5677_dsp_mode_i2c_write(codec, RT5677_PRIV_DATA, pri99);

		/* Reset the mic buffer read pointer. */
		rt5677->mic_read_offset = 0;

		/* Boot the firmware from IRAM instead of SRAM0. */
		rt5677_dsp_mode_i2c_write_address(codec, RT5677_DSP_BOOT_VECTOR,
						  0x0009, 0x0003);
		rt5677_dsp_mode_i2c_write_address(codec, RT5677_DSP_BOOT_VECTOR,
						  0x0019, 0x0003);
		rt5677_dsp_mode_i2c_write_address(codec, RT5677_DSP_BOOT_VECTOR,
						  0x0009, 0x0003);

		rt5677_load_dsp_from_file(codec);

		rt5677_dsp_mode_i2c_update_bits(codec, RT5677_PWR_DSP1,
			0x1, 0x0);
		regcache_cache_bypass(rt5677->regmap, false);
		regcache_cache_only(rt5677->regmap, true);
	} else if (!on && activity) {
		pr_info("rt5677_set_vad off\n");
		activity = false;
		regcache_cache_only(rt5677->regmap, false);
		regcache_cache_bypass(rt5677->regmap, true);
		rt5677_dsp_mode_i2c_update_bits(codec, RT5677_PWR_DSP1,
			0x1, 0x1);
		rt5677_dsp_mode_i2c_write(codec, RT5677_PWR_DSP1, 0x0001);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP1, 0x0001);
		regmap_write(rt5677->regmap, RT5677_PWR_DSP2, 0x0c00);
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
			RT5677_PWR_LDO1, 0);

		rt5677_index_write(codec, 0x14, 0x0f8b);
		rt5677_index_update_bits(codec,
			RT5677_BIAS_CUR4, 0x0f00, 0x0000);
		regcache_cache_bypass(rt5677->regmap, false);
		regcache_cache_only(rt5677->regmap, true);
		gpio_direction_output(rt5677->vad_clock_en, 0);
		set_rt5677_power_extern(false);
	}

	if (use_lock)
		mutex_unlock(&rt5677_global->vad_lock);
	return 0;
}

static void rt5677_check_hp_mic(struct work_struct *work)
{
	int state;
	mutex_lock(&rt5677_global->vad_lock);
	state = get_mic_state();
	pr_debug("%s mic %d, rt5677_global->vad_mode %d\n",
			 __func__, state, rt5677_global->vad_mode);
	if (rt5677_global->mic_state != state) {
		rt5677_global->mic_state = state;
		if ((rt5677_global->codec->dapm.bias_level == SND_SOC_BIAS_OFF) &&
			(rt5677_global->vad_mode == RT5677_VAD_IDLE)) {
			rt5677_set_vad(rt5677_global->codec, 0, false);
			rt5677_set_vad(rt5677_global->codec, 1, false);
		}
	}
	mutex_unlock(&rt5677_global->vad_lock);
}

int rt5677_headset_detect(int on)
{
	pr_debug("%s on = %d\n", __func__, on);
	cancel_delayed_work_sync(&rt5677_global->check_hp_mic_work);
	queue_delayed_work(rt5677_global->check_mic_wq, &rt5677_global->check_hp_mic_work,
						msecs_to_jiffies(1100));
	return on;
}

static void rt5677_register_hs_notification(void)
{
	struct headset_notifier notifier;
	notifier.id = HEADSET_REG_HS_INSERT;
	notifier.func = rt5677_headset_detect;
	headset_notifier_register(&notifier);
}

static bool rt5677_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RT5677_RESET:
	case RT5677_SLIMBUS_PARAM:
	case RT5677_PDM_DATA_CTRL1:
	case RT5677_PDM_DATA_CTRL2:
	case RT5677_PDM1_DATA_CTRL4:
	case RT5677_PDM2_DATA_CTRL4:
	case RT5677_I2C_MASTER_CTRL1:
	case RT5677_I2C_MASTER_CTRL7:
	case RT5677_I2C_MASTER_CTRL8:
	case RT5677_HAP_GENE_CTRL2:
	case RT5677_PWR_DSP_ST:
	case RT5677_PRIV_DATA:
	case RT5677_PLL1_CTRL2:
	case RT5677_PLL2_CTRL2:
	case RT5677_ASRC_22:
	case RT5677_ASRC_23:
	case RT5677_VAD_CTRL5:
	case RT5677_ADC_EQ_CTRL1:
	case RT5677_EQ_CTRL1:
	case RT5677_IRQ_CTRL1:
	case RT5677_IRQ_CTRL2:
	case RT5677_GPIO_ST:
	case RT5677_DSP_INB1_SRC_CTRL4:
	case RT5677_DSP_INB2_SRC_CTRL4:
	case RT5677_DSP_INB3_SRC_CTRL4:
	case RT5677_DSP_OUTB1_SRC_CTRL4:
	case RT5677_DSP_OUTB2_SRC_CTRL4:
	case RT5677_VENDOR_ID:
	case RT5677_VENDOR_ID1:
	case RT5677_VENDOR_ID2:
		return true;
	default:
		return false;
	}
}

static bool rt5677_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RT5677_RESET:
	case RT5677_LOUT1:
	case RT5677_IN1:
	case RT5677_MICBIAS:
	case RT5677_SLIMBUS_PARAM:
	case RT5677_SLIMBUS_RX:
	case RT5677_SLIMBUS_CTRL:
	case RT5677_SIDETONE_CTRL:
	case RT5677_ANA_DAC1_2_3_SRC:
	case RT5677_IF_DSP_DAC3_4_MIXER:
	case RT5677_DAC4_DIG_VOL:
	case RT5677_DAC3_DIG_VOL:
	case RT5677_DAC1_DIG_VOL:
	case RT5677_DAC2_DIG_VOL:
	case RT5677_IF_DSP_DAC2_MIXER:
	case RT5677_STO1_ADC_DIG_VOL:
	case RT5677_MONO_ADC_DIG_VOL:
	case RT5677_STO1_2_ADC_BST:
	case RT5677_STO2_ADC_DIG_VOL:
	case RT5677_ADC_BST_CTRL2:
	case RT5677_STO3_4_ADC_BST:
	case RT5677_STO3_ADC_DIG_VOL:
	case RT5677_STO4_ADC_DIG_VOL:
	case RT5677_STO4_ADC_MIXER:
	case RT5677_STO3_ADC_MIXER:
	case RT5677_STO2_ADC_MIXER:
	case RT5677_STO1_ADC_MIXER:
	case RT5677_MONO_ADC_MIXER:
	case RT5677_ADC_IF_DSP_DAC1_MIXER:
	case RT5677_STO1_DAC_MIXER:
	case RT5677_MONO_DAC_MIXER:
	case RT5677_DD1_MIXER:
	case RT5677_DD2_MIXER:
	case RT5677_IF3_DATA:
	case RT5677_IF4_DATA:
	case RT5677_PDM_OUT_CTRL:
	case RT5677_PDM_DATA_CTRL1:
	case RT5677_PDM_DATA_CTRL2:
	case RT5677_PDM1_DATA_CTRL2:
	case RT5677_PDM1_DATA_CTRL3:
	case RT5677_PDM1_DATA_CTRL4:
	case RT5677_PDM2_DATA_CTRL2:
	case RT5677_PDM2_DATA_CTRL3:
	case RT5677_PDM2_DATA_CTRL4:
	case RT5677_TDM1_CTRL1:
	case RT5677_TDM1_CTRL2:
	case RT5677_TDM1_CTRL3:
	case RT5677_TDM1_CTRL4:
	case RT5677_TDM1_CTRL5:
	case RT5677_TDM2_CTRL1:
	case RT5677_TDM2_CTRL2:
	case RT5677_TDM2_CTRL3:
	case RT5677_TDM2_CTRL4:
	case RT5677_TDM2_CTRL5:
	case RT5677_I2C_MASTER_CTRL1:
	case RT5677_I2C_MASTER_CTRL2:
	case RT5677_I2C_MASTER_CTRL3:
	case RT5677_I2C_MASTER_CTRL4:
	case RT5677_I2C_MASTER_CTRL5:
	case RT5677_I2C_MASTER_CTRL6:
	case RT5677_I2C_MASTER_CTRL7:
	case RT5677_I2C_MASTER_CTRL8:
	case RT5677_DMIC_CTRL1:
	case RT5677_DMIC_CTRL2:
	case RT5677_HAP_GENE_CTRL1:
	case RT5677_HAP_GENE_CTRL2:
	case RT5677_HAP_GENE_CTRL3:
	case RT5677_HAP_GENE_CTRL4:
	case RT5677_HAP_GENE_CTRL5:
	case RT5677_HAP_GENE_CTRL6:
	case RT5677_HAP_GENE_CTRL7:
	case RT5677_HAP_GENE_CTRL8:
	case RT5677_HAP_GENE_CTRL9:
	case RT5677_HAP_GENE_CTRL10:
	case RT5677_PWR_DIG1:
	case RT5677_PWR_DIG2:
	case RT5677_PWR_ANLG1:
	case RT5677_PWR_ANLG2:
	case RT5677_PWR_DSP1:
	case RT5677_PWR_DSP_ST:
	case RT5677_PWR_DSP2:
	case RT5677_ADC_DAC_HPF_CTRL1:
	case RT5677_PRIV_INDEX:
	case RT5677_PRIV_DATA:
	case RT5677_I2S4_SDP:
	case RT5677_I2S1_SDP:
	case RT5677_I2S2_SDP:
	case RT5677_I2S3_SDP:
	case RT5677_CLK_TREE_CTRL1:
	case RT5677_CLK_TREE_CTRL2:
	case RT5677_CLK_TREE_CTRL3:
	case RT5677_PLL1_CTRL1:
	case RT5677_PLL1_CTRL2:
	case RT5677_PLL2_CTRL1:
	case RT5677_PLL2_CTRL2:
	case RT5677_GLB_CLK1:
	case RT5677_GLB_CLK2:
	case RT5677_ASRC_1:
	case RT5677_ASRC_2:
	case RT5677_ASRC_3:
	case RT5677_ASRC_4:
	case RT5677_ASRC_5:
	case RT5677_ASRC_6:
	case RT5677_ASRC_7:
	case RT5677_ASRC_8:
	case RT5677_ASRC_9:
	case RT5677_ASRC_10:
	case RT5677_ASRC_11:
	case RT5677_ASRC_12:
	case RT5677_ASRC_13:
	case RT5677_ASRC_14:
	case RT5677_ASRC_15:
	case RT5677_ASRC_16:
	case RT5677_ASRC_17:
	case RT5677_ASRC_18:
	case RT5677_ASRC_19:
	case RT5677_ASRC_20:
	case RT5677_ASRC_21:
	case RT5677_ASRC_22:
	case RT5677_ASRC_23:
	case RT5677_VAD_CTRL1:
	case RT5677_VAD_CTRL2:
	case RT5677_VAD_CTRL3:
	case RT5677_VAD_CTRL4:
	case RT5677_VAD_CTRL5:
	case RT5677_DSP_INB_CTRL1:
	case RT5677_DSP_INB_CTRL2:
	case RT5677_DSP_IN_OUTB_CTRL:
	case RT5677_DSP_OUTB0_1_DIG_VOL:
	case RT5677_DSP_OUTB2_3_DIG_VOL:
	case RT5677_DSP_OUTB4_5_DIG_VOL:
	case RT5677_DSP_OUTB6_7_DIG_VOL:
	case RT5677_ADC_EQ_CTRL1:
	case RT5677_ADC_EQ_CTRL2:
	case RT5677_EQ_CTRL1:
	case RT5677_EQ_CTRL2:
	case RT5677_EQ_CTRL3:
	case RT5677_SOFT_VOL_ZERO_CROSS1:
	case RT5677_JD_CTRL1:
	case RT5677_JD_CTRL2:
	case RT5677_JD_CTRL3:
	case RT5677_IRQ_CTRL1:
	case RT5677_IRQ_CTRL2:
	case RT5677_GPIO_ST:
	case RT5677_GPIO_CTRL1:
	case RT5677_GPIO_CTRL2:
	case RT5677_GPIO_CTRL3:
	case RT5677_STO1_ADC_HI_FILTER1:
	case RT5677_STO1_ADC_HI_FILTER2:
	case RT5677_MONO_ADC_HI_FILTER1:
	case RT5677_MONO_ADC_HI_FILTER2:
	case RT5677_STO2_ADC_HI_FILTER1:
	case RT5677_STO2_ADC_HI_FILTER2:
	case RT5677_STO3_ADC_HI_FILTER1:
	case RT5677_STO3_ADC_HI_FILTER2:
	case RT5677_STO4_ADC_HI_FILTER1:
	case RT5677_STO4_ADC_HI_FILTER2:
	case RT5677_MB_DRC_CTRL1:
	case RT5677_DRC1_CTRL1:
	case RT5677_DRC1_CTRL2:
	case RT5677_DRC1_CTRL3:
	case RT5677_DRC1_CTRL4:
	case RT5677_DRC1_CTRL5:
	case RT5677_DRC1_CTRL6:
	case RT5677_DRC2_CTRL1:
	case RT5677_DRC2_CTRL2:
	case RT5677_DRC2_CTRL3:
	case RT5677_DRC2_CTRL4:
	case RT5677_DRC2_CTRL5:
	case RT5677_DRC2_CTRL6:
	case RT5677_DRC1_HL_CTRL1:
	case RT5677_DRC1_HL_CTRL2:
	case RT5677_DRC2_HL_CTRL1:
	case RT5677_DRC2_HL_CTRL2:
	case RT5677_DSP_INB1_SRC_CTRL1:
	case RT5677_DSP_INB1_SRC_CTRL2:
	case RT5677_DSP_INB1_SRC_CTRL3:
	case RT5677_DSP_INB1_SRC_CTRL4:
	case RT5677_DSP_INB2_SRC_CTRL1:
	case RT5677_DSP_INB2_SRC_CTRL2:
	case RT5677_DSP_INB2_SRC_CTRL3:
	case RT5677_DSP_INB2_SRC_CTRL4:
	case RT5677_DSP_INB3_SRC_CTRL1:
	case RT5677_DSP_INB3_SRC_CTRL2:
	case RT5677_DSP_INB3_SRC_CTRL3:
	case RT5677_DSP_INB3_SRC_CTRL4:
	case RT5677_DSP_OUTB1_SRC_CTRL1:
	case RT5677_DSP_OUTB1_SRC_CTRL2:
	case RT5677_DSP_OUTB1_SRC_CTRL3:
	case RT5677_DSP_OUTB1_SRC_CTRL4:
	case RT5677_DSP_OUTB2_SRC_CTRL1:
	case RT5677_DSP_OUTB2_SRC_CTRL2:
	case RT5677_DSP_OUTB2_SRC_CTRL3:
	case RT5677_DSP_OUTB2_SRC_CTRL4:
	case RT5677_DSP_OUTB_0123_MIXER_CTRL:
	case RT5677_DSP_OUTB_45_MIXER_CTRL:
	case RT5677_DSP_OUTB_67_MIXER_CTRL:
	case RT5677_DIG_MISC:
	case RT5677_GEN_CTRL1:
	case RT5677_GEN_CTRL2:
	case RT5677_VENDOR_ID:
	case RT5677_VENDOR_ID1:
	case RT5677_VENDOR_ID2:
		return true;
	default:
		return false;
	}
}

static const DECLARE_TLV_DB_SCALE(out_vol_tlv, -4650, 150, 0);
static const DECLARE_TLV_DB_SCALE(dac_vol_tlv, -65625, 375, 0);
static const DECLARE_TLV_DB_SCALE(in_vol_tlv, -3450, 150, 0);
static const DECLARE_TLV_DB_SCALE(adc_vol_tlv, -17625, 375, 0);
static const DECLARE_TLV_DB_SCALE(adc_bst_tlv, 0, 1200, 0);

/* {0, +20, +24, +30, +35, +40, +44, +50, +52} dB */
static unsigned int bst_tlv[] = {
	TLV_DB_RANGE_HEAD(7),
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 1, TLV_DB_SCALE_ITEM(2000, 0, 0),
	2, 2, TLV_DB_SCALE_ITEM(2400, 0, 0),
	3, 5, TLV_DB_SCALE_ITEM(3000, 500, 0),
	6, 6, TLV_DB_SCALE_ITEM(4400, 0, 0),
	7, 7, TLV_DB_SCALE_ITEM(5000, 0, 0),
	8, 8, TLV_DB_SCALE_ITEM(5200, 0, 0),
};

/* IN1/IN2 Input Type */
static const char * const rt5677_input_mode[] = {
	"Single ended", "Differential"
};

static const char * const rt5677_vad_mode[] = {
	"Disable", "Idle-DMIC1", "Idle-DMIC2", "Idle-AMIC", "Suspend-DMIC1",
	"Suspend-DMIC2", "Suspend-AMIC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_in1_mode_enum, RT5677_IN1,
	RT5677_IN_DF1_SFT, rt5677_input_mode);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_in2_mode_enum, RT5677_IN1,
	RT5677_IN_DF2_SFT, rt5677_input_mode);

static const SOC_ENUM_SINGLE_DECL(rt5677_vad_mode_enum, 0, 0,
	rt5677_vad_mode);

static int rt5677_vad_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = rt5677->vad_source;

	return 0;
}

static int rt5677_vad_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	rt5677->vad_source = ucontrol->value.integer.value[0];

	switch (rt5677->vad_source) {
	case RT5677_VAD_IDLE_DMIC1:
	case RT5677_VAD_IDLE_DMIC2:
	case RT5677_VAD_IDLE_AMIC:
		rt5677->vad_mode = RT5677_VAD_IDLE;
		break;

	case RT5677_VAD_SUSPEND_DMIC1:
	case RT5677_VAD_SUSPEND_DMIC2:
	case RT5677_VAD_SUSPEND_AMIC:
		rt5677->vad_mode = RT5677_VAD_SUSPEND;
		break;
	default:
		rt5677->vad_mode = RT5677_VAD_OFF;
		break;
	}

	if (codec->dapm.bias_level == SND_SOC_BIAS_OFF) {
		pr_debug("codec->dapm.bias_level = SND_SOC_BIAS_OFF\n");
		if (rt5677->vad_mode == RT5677_VAD_IDLE)
			rt5677_set_vad(codec, 1, true);
		else if (rt5677->vad_mode == RT5677_VAD_OFF)
			rt5677_set_vad(codec, 0, true);
	}
	return 0;
}

static const char * const rt5677_tdm1_mode[] = {
	"Normal", "LL Copy", "RR Copy"
};

static const SOC_ENUM_SINGLE_DECL(rt5677_tdm1_enum, 0, 0, rt5677_tdm1_mode);

static int rt5677_tdm1_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int value;

	regmap_read(rt5677->regmap, RT5677_TDM1_CTRL1, &value);
	if ((value & 0xc) == 0xc)
		ucontrol->value.integer.value[0] = 2;
	else if ((value & 0xc) == 0x8)
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	return 0;
}

static int rt5677_tdm1_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	if (ucontrol->value.integer.value[0] == 0)
		regmap_update_bits(rt5677->regmap, RT5677_TDM1_CTRL1, 0xc, 0x0);
	else if (ucontrol->value.integer.value[0] == 1)
		regmap_update_bits(rt5677->regmap, RT5677_TDM1_CTRL1, 0xc, 0x8);
	else
		regmap_update_bits(rt5677->regmap, RT5677_TDM1_CTRL1, 0xc, 0xc);

	return 0;
}

static const struct snd_kcontrol_new rt5677_snd_controls[] = {
	/* OUTPUT Control */
	SOC_SINGLE("OUT1 Playback Switch", RT5677_LOUT1,
		RT5677_LOUT1_L_MUTE_SFT, 1, 1),
	SOC_SINGLE("OUT2 Playback Switch", RT5677_LOUT1,
		RT5677_LOUT2_L_MUTE_SFT, 1, 1),
	SOC_SINGLE("OUT3 Playback Switch", RT5677_LOUT1,
		RT5677_LOUT3_L_MUTE_SFT, 1, 1),

	/* DAC Digital Volume */
	SOC_DOUBLE_TLV("DAC1 Playback Volume", RT5677_DAC1_DIG_VOL,
			RT5677_L_VOL_SFT, RT5677_R_VOL_SFT,
			175, 0, dac_vol_tlv),
	SOC_DOUBLE_TLV("DAC2 Playback Volume", RT5677_DAC2_DIG_VOL,
			RT5677_L_VOL_SFT, RT5677_R_VOL_SFT,
			175, 0, dac_vol_tlv),
	SOC_DOUBLE_TLV("DAC3 Playback Volume", RT5677_DAC3_DIG_VOL,
			RT5677_L_VOL_SFT, RT5677_R_VOL_SFT,
			175, 0, dac_vol_tlv),
	SOC_DOUBLE_TLV("DAC4 Playback Volume", RT5677_DAC4_DIG_VOL,
			RT5677_L_VOL_SFT, RT5677_R_VOL_SFT,
			175, 0, dac_vol_tlv),

	/* IN1/IN2 Control */
	SOC_ENUM("IN1 Mode Control",  rt5677_in1_mode_enum),
	SOC_SINGLE_TLV("IN1 Boost", RT5677_IN1,
		RT5677_BST_SFT1, 8, 0, bst_tlv),
	SOC_ENUM("IN2 Mode Control", rt5677_in2_mode_enum),
	SOC_SINGLE_TLV("IN2 Boost", RT5677_IN1,
		RT5677_BST_SFT2, 8, 0, bst_tlv),

	/* ADC Digital Volume Control */
	SOC_DOUBLE("ADC1 Capture Switch", RT5677_STO1_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, RT5677_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE("ADC2 Capture Switch", RT5677_STO2_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, RT5677_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE("ADC3 Capture Switch", RT5677_STO3_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, RT5677_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE("ADC4 Capture Switch", RT5677_STO4_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, RT5677_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE("Mono ADC Capture Switch", RT5677_MONO_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, RT5677_R_MUTE_SFT, 1, 1),

	SOC_DOUBLE_TLV("ADC1 Capture Volume", RT5677_STO1_ADC_DIG_VOL,
			RT5677_STO1_ADC_L_VOL_SFT, RT5677_STO1_ADC_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("ADC2 Capture Volume", RT5677_STO2_ADC_DIG_VOL,
			RT5677_STO1_ADC_L_VOL_SFT, RT5677_STO1_ADC_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("ADC3 Capture Volume", RT5677_STO3_ADC_DIG_VOL,
			RT5677_STO1_ADC_L_VOL_SFT, RT5677_STO1_ADC_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("ADC4 Capture Volume", RT5677_STO4_ADC_DIG_VOL,
			RT5677_STO1_ADC_L_VOL_SFT, RT5677_STO1_ADC_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("Mono ADC Capture Volume", RT5677_MONO_ADC_DIG_VOL,
			RT5677_MONO_ADC_L_VOL_SFT, RT5677_MONO_ADC_R_VOL_SFT,
			127, 0, adc_vol_tlv),

	/* ADC Boost Volume Control */
	SOC_DOUBLE_TLV("STO1 ADC Boost Gain", RT5677_STO1_2_ADC_BST,
			RT5677_STO1_ADC_L_BST_SFT, RT5677_STO1_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),
	SOC_DOUBLE_TLV("STO2 ADC Boost Gain", RT5677_STO1_2_ADC_BST,
			RT5677_STO2_ADC_L_BST_SFT, RT5677_STO2_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),
	SOC_DOUBLE_TLV("STO3 ADC Boost Gain", RT5677_STO3_4_ADC_BST,
			RT5677_STO3_ADC_L_BST_SFT, RT5677_STO3_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),
	SOC_DOUBLE_TLV("STO4 ADC Boost Gain", RT5677_STO3_4_ADC_BST,
			RT5677_STO4_ADC_L_BST_SFT, RT5677_STO4_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),
	SOC_DOUBLE_TLV("Mono ADC Boost Gain", RT5677_ADC_BST_CTRL2,
			RT5677_MONO_ADC_L_BST_SFT, RT5677_MONO_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),

	SOC_ENUM_EXT("VAD Mode", rt5677_vad_mode_enum, rt5677_vad_get,
		rt5677_vad_put),

	SOC_ENUM_EXT("TDM1 Mode", rt5677_tdm1_enum, rt5677_tdm1_get,
		rt5677_tdm1_put),
};

/**
 * set_dmic_clk - Set parameter of dmic.
 *
 * @w: DAPM widget.
 * @kcontrol: The kcontrol of this widget.
 * @event: Event id.
 *
 * Choose dmic clock between 1MHz and 3MHz.
 * It is better for clock to approximate 3MHz.
 */
static int set_dmic_clk(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int div[] = {2, 3, 4, 6, 8, 12}, idx = -EINVAL, i;
	int rate, red, bound, temp;

	rate = rt5677->lrck[rt5677->aif_pu] << 8;
	red = RT5677_DMIC_CLK_MAX * 12;
	for (i = 0; i < ARRAY_SIZE(div); i++) {
		bound = div[i] * RT5677_DMIC_CLK_MAX;
		if (rate > bound)
			continue;
		temp = bound - rate;
		if (temp < red) {
			red = temp;
			idx = i;
		}
	}
#ifdef USE_ASRC
	idx = 5;
#endif
	if (idx < 0)
		dev_err(codec->dev, "Failed to set DMIC clock\n");
	else
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_CLK_MASK, idx << RT5677_DMIC_CLK_SFT);
	return idx;
}

static int check_sysclk1_source(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(source->codec);
	unsigned int val;

	regmap_read(rt5677->regmap, RT5677_GLB_CLK1, &val);
	val &= RT5677_SCLK_SRC_MASK;
	if (val == RT5677_SCLK_SRC_PLL1)
		return 1;
	else
		return 0;
}

/* Digital Mixer */
static const struct snd_kcontrol_new rt5677_sto1_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO1_ADC_MIXER,
			RT5677_M_STO1_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO1_ADC_MIXER,
			RT5677_M_STO1_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto1_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO1_ADC_MIXER,
			RT5677_M_STO1_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO1_ADC_MIXER,
			RT5677_M_STO1_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto2_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO2_ADC_MIXER,
			RT5677_M_STO2_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO2_ADC_MIXER,
			RT5677_M_STO2_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto2_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO2_ADC_MIXER,
			RT5677_M_STO2_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO2_ADC_MIXER,
			RT5677_M_STO2_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto3_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO3_ADC_MIXER,
			RT5677_M_STO3_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO3_ADC_MIXER,
			RT5677_M_STO3_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto3_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO3_ADC_MIXER,
			RT5677_M_STO3_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO3_ADC_MIXER,
			RT5677_M_STO3_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto4_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO4_ADC_MIXER,
			RT5677_M_STO4_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO4_ADC_MIXER,
			RT5677_M_STO4_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto4_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_STO4_ADC_MIXER,
			RT5677_M_STO4_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_STO4_ADC_MIXER,
			RT5677_M_STO4_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_mono_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_MONO_ADC_MIXER,
			RT5677_M_MONO_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_MONO_ADC_MIXER,
			RT5677_M_MONO_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_mono_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5677_MONO_ADC_MIXER,
			RT5677_M_MONO_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5677_MONO_ADC_MIXER,
			RT5677_M_MONO_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dac_l_mix[] = {
	SOC_DAPM_SINGLE("Stereo ADC Switch", RT5677_ADC_IF_DSP_DAC1_MIXER,
			RT5677_M_ADDA_MIXER1_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 Switch", RT5677_ADC_IF_DSP_DAC1_MIXER,
			RT5677_M_DAC1_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dac_r_mix[] = {
	SOC_DAPM_SINGLE("Stereo ADC Switch", RT5677_ADC_IF_DSP_DAC1_MIXER,
			RT5677_M_ADDA_MIXER1_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 Switch", RT5677_ADC_IF_DSP_DAC1_MIXER,
			RT5677_M_DAC1_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto1_dac_l_mix[] = {
	SOC_DAPM_SINGLE("ST L Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_ST_DAC1_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 L Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC1_L_STO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 L Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC2_L_STO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 R Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC1_R_STO_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_sto1_dac_r_mix[] = {
	SOC_DAPM_SINGLE("ST R Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_ST_DAC1_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 R Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC1_R_STO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 R Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC2_R_STO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 L Switch", RT5677_STO1_DAC_MIXER,
			RT5677_M_DAC1_L_STO_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_mono_dac_l_mix[] = {
	SOC_DAPM_SINGLE("ST L Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_ST_DAC2_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 L Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC1_L_MONO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 L Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC2_L_MONO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 R Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC2_R_MONO_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_mono_dac_r_mix[] = {
	SOC_DAPM_SINGLE("ST R Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_ST_DAC2_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 R Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC1_R_MONO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 R Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC2_R_MONO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC2 L Switch", RT5677_MONO_DAC_MIXER,
			RT5677_M_DAC2_L_MONO_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dd1_l_mix[] = {
	SOC_DAPM_SINGLE("Sto DAC Mix L Switch", RT5677_DD1_MIXER,
			RT5677_M_STO_L_DD1_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("Mono DAC Mix L Switch", RT5677_DD1_MIXER,
			RT5677_M_MONO_L_DD1_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC3 L Switch", RT5677_DD1_MIXER,
			RT5677_M_DAC3_L_DD1_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC3 R Switch", RT5677_DD1_MIXER,
			RT5677_M_DAC3_R_DD1_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dd1_r_mix[] = {
	SOC_DAPM_SINGLE("Sto DAC Mix R Switch", RT5677_DD1_MIXER,
			RT5677_M_STO_R_DD1_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("Mono DAC Mix R Switch", RT5677_DD1_MIXER,
			RT5677_M_MONO_R_DD1_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC3 R Switch", RT5677_DD1_MIXER,
			RT5677_M_DAC3_R_DD1_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC3 L Switch", RT5677_DD1_MIXER,
			RT5677_M_DAC3_L_DD1_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dd2_l_mix[] = {
	SOC_DAPM_SINGLE("Sto DAC Mix L Switch", RT5677_DD2_MIXER,
			RT5677_M_STO_L_DD2_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("Mono DAC Mix L Switch", RT5677_DD2_MIXER,
			RT5677_M_MONO_L_DD2_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC4 L Switch", RT5677_DD2_MIXER,
			RT5677_M_DAC4_L_DD2_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC4 R Switch", RT5677_DD2_MIXER,
			RT5677_M_DAC4_R_DD2_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_dd2_r_mix[] = {
	SOC_DAPM_SINGLE("Sto DAC Mix R Switch", RT5677_DD2_MIXER,
			RT5677_M_STO_R_DD2_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("Mono DAC Mix R Switch", RT5677_DD2_MIXER,
			RT5677_M_MONO_R_DD2_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC4 R Switch", RT5677_DD2_MIXER,
			RT5677_M_DAC4_R_DD2_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC4 L Switch", RT5677_DD2_MIXER,
			RT5677_M_DAC4_L_DD2_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_01_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_01_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_23_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_45_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_6_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_7_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_8_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_9_H_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_23_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_01_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_23_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_45_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_6_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_7_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_8_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_0123_MIXER_CTRL,
			RT5677_DSP_IB_9_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_4_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_01_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_23_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_45_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_6_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_7_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_8_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_9_H_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_5_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_01_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_23_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_45_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_6_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_7_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_8_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_45_MIXER_CTRL,
			RT5677_DSP_IB_9_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_6_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_01_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_23_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_45_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_6_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_7_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_8_H_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_9_H_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5677_ob_7_mix[] = {
	SOC_DAPM_SINGLE("IB01 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_01_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB23 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_23_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB45 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_45_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB6 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_6_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB7 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_7_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB8 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_8_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("IB9 Switch", RT5677_DSP_OUTB_67_MIXER_CTRL,
			RT5677_DSP_IB_9_L_SFT, 1, 1),
};


/* Mux */
/* DAC1 L/R source */ /* MX-29 [10:8] */
static const char * const rt5677_dac1_src[] = {
	"IF1 DAC 01", "IF2 DAC 01", "IF3 DAC LR", "IF4 DAC LR", "SLB DAC 01",
	"OB 01"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac1_enum, RT5677_ADC_IF_DSP_DAC1_MIXER,
	RT5677_DAC1_L_SEL_SFT, rt5677_dac1_src);

static const struct snd_kcontrol_new rt5677_dac1_mux =
	SOC_DAPM_ENUM("DAC1 source", rt5677_dac1_enum);

/* ADDA1 L/R source */ /* MX-29 [1:0] */
static const char * const rt5677_adda1_src[] = {
	"STO1 ADC MIX", "STO2 ADC MIX", "OB 67",
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_adda1_enum, RT5677_ADC_IF_DSP_DAC1_MIXER,
	RT5677_ADDA1_SEL_SFT, rt5677_adda1_src);

static const struct snd_kcontrol_new rt5677_adda1_mux =
	SOC_DAPM_ENUM("ADDA1 source", rt5677_adda1_enum);


/*DAC2 L/R source*/ /* MX-1B [6:4] [2:0] */
static const char * const rt5677_dac2l_src[] = {
	"IF1 DAC 2", "IF2 DAC 2", "IF3 DAC L", "IF4 DAC L", "SLB DAC 2",
	"OB 2",
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac2l_enum, RT5677_IF_DSP_DAC2_MIXER,
	RT5677_SEL_DAC2_L_SRC_SFT, rt5677_dac2l_src);

static const struct snd_kcontrol_new rt5677_dac2_l_mux =
	SOC_DAPM_ENUM("DAC2 L source", rt5677_dac2l_enum);

static const char * const rt5677_dac2r_src[] = {
	"IF1 DAC 3", "IF2 DAC 3", "IF3 DAC R", "IF4 DAC R", "SLB DAC 3",
	"OB 3", "Haptic Generator", "VAD ADC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac2r_enum, RT5677_IF_DSP_DAC2_MIXER,
	RT5677_SEL_DAC2_R_SRC_SFT, rt5677_dac2r_src);

static const struct snd_kcontrol_new rt5677_dac2_r_mux =
	SOC_DAPM_ENUM("DAC2 R source", rt5677_dac2r_enum);

/*DAC3 L/R source*/ /* MX-16 [6:4] [2:0] */
static const char * const rt5677_dac3l_src[] = {
	"IF1 DAC 4", "IF2 DAC 4", "IF3 DAC L", "IF4 DAC L",
	"SLB DAC 4", "OB 4"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac3l_enum, RT5677_IF_DSP_DAC3_4_MIXER,
	RT5677_SEL_DAC3_L_SRC_SFT, rt5677_dac3l_src);

static const struct snd_kcontrol_new rt5677_dac3_l_mux =
	SOC_DAPM_ENUM("DAC3 L source", rt5677_dac3l_enum);

static const char * const rt5677_dac3r_src[] = {
	"IF1 DAC 5", "IF2 DAC 5", "IF3 DAC R", "IF4 DAC R",
	"SLB DAC 5", "OB 5"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac3r_enum, RT5677_IF_DSP_DAC3_4_MIXER,
	RT5677_SEL_DAC3_R_SRC_SFT, rt5677_dac3r_src);

static const struct snd_kcontrol_new rt5677_dac3_r_mux =
	SOC_DAPM_ENUM("DAC3 R source", rt5677_dac3r_enum);

/*DAC4 L/R source*/ /* MX-16 [14:12] [10:8] */
static const char * const rt5677_dac4l_src[] = {
	"IF1 DAC 6", "IF2 DAC 6", "IF3 DAC L", "IF4 DAC L",
	"SLB DAC 6", "OB 6"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac4l_enum, RT5677_IF_DSP_DAC3_4_MIXER,
	RT5677_SEL_DAC4_L_SRC_SFT, rt5677_dac4l_src);

static const struct snd_kcontrol_new rt5677_dac4_l_mux =
	SOC_DAPM_ENUM("DAC4 L source", rt5677_dac4l_enum);

static const char * const rt5677_dac4r_src[] = {
	"IF1 DAC 7", "IF2 DAC 7", "IF3 DAC R", "IF4 DAC R",
	"SLB DAC 7", "OB 7"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac4r_enum, RT5677_IF_DSP_DAC3_4_MIXER,
	RT5677_SEL_DAC4_R_SRC_SFT, rt5677_dac4r_src);

static const struct snd_kcontrol_new rt5677_dac4_r_mux =
	SOC_DAPM_ENUM("DAC4 R source", rt5677_dac4r_enum);

/* In/OutBound Source Pass SRC */ /* MX-A5 [3] [4] [0] [1] [2] */
static const char * const rt5677_iob_bypass_src[] = {
	"Bypass", "Pass SRC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_ob01_bypass_src_enum, RT5677_DSP_IN_OUTB_CTRL,
	RT5677_SEL_SRC_OB01_SFT, rt5677_iob_bypass_src);

static const struct snd_kcontrol_new rt5677_ob01_bypass_src_mux =
	SOC_DAPM_ENUM("OB01 Bypass source", rt5677_ob01_bypass_src_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_ob23_bypass_src_enum, RT5677_DSP_IN_OUTB_CTRL,
	RT5677_SEL_SRC_OB23_SFT, rt5677_iob_bypass_src);

static const struct snd_kcontrol_new rt5677_ob23_bypass_src_mux =
	SOC_DAPM_ENUM("OB23 Bypass source", rt5677_ob23_bypass_src_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_ib01_bypass_src_enum, RT5677_DSP_IN_OUTB_CTRL,
	RT5677_SEL_SRC_IB01_SFT, rt5677_iob_bypass_src);

static const struct snd_kcontrol_new rt5677_ib01_bypass_src_mux =
	SOC_DAPM_ENUM("IB01 Bypass source", rt5677_ib01_bypass_src_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_ib23_bypass_src_enum, RT5677_DSP_IN_OUTB_CTRL,
	RT5677_SEL_SRC_IB23_SFT, rt5677_iob_bypass_src);

static const struct snd_kcontrol_new rt5677_ib23_bypass_src_mux =
	SOC_DAPM_ENUM("IB23 Bypass source", rt5677_ib23_bypass_src_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_ib45_bypass_src_enum, RT5677_DSP_IN_OUTB_CTRL,
	RT5677_SEL_SRC_IB45_SFT, rt5677_iob_bypass_src);

static const struct snd_kcontrol_new rt5677_ib45_bypass_src_mux =
	SOC_DAPM_ENUM("IB45 Bypass source", rt5677_ib45_bypass_src_enum);

/* Stereo ADC Source 2 */ /* MX-27 MX26  MX25 [11:10] */
static const char * const rt5677_stereo_adc2_src[] = {
	"DD MIX1", "DMIC", "Stereo DAC MIX"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo1_adc2_enum, RT5677_STO1_ADC_MIXER,
	RT5677_SEL_STO1_ADC2_SFT, rt5677_stereo_adc2_src);

static const struct snd_kcontrol_new rt5677_sto1_adc2_mux =
	SOC_DAPM_ENUM("Stereo1 ADC2 source", rt5677_stereo1_adc2_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo2_adc2_enum, RT5677_STO2_ADC_MIXER,
	RT5677_SEL_STO2_ADC2_SFT, rt5677_stereo_adc2_src);

static const struct snd_kcontrol_new rt5677_sto2_adc2_mux =
	SOC_DAPM_ENUM("Stereo2 ADC2 source", rt5677_stereo2_adc2_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo3_adc2_enum, RT5677_STO3_ADC_MIXER,
	RT5677_SEL_STO3_ADC2_SFT, rt5677_stereo_adc2_src);

static const struct snd_kcontrol_new rt5677_sto3_adc2_mux =
	SOC_DAPM_ENUM("Stereo3 ADC2 source", rt5677_stereo3_adc2_enum);

/* DMIC Source */ /* MX-28 [9:8][1:0] MX-27 MX-26 MX-25 MX-24 [9:8] */
static const char * const rt5677_dmic_src[] = {
	"DMIC1", "DMIC2", "DMIC3", "DMIC4"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_dmic_l_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_DMIC_L_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_mono_dmic_l_mux =
	SOC_DAPM_ENUM("Mono DMIC L source", rt5677_mono_dmic_l_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_dmic_r_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_DMIC_R_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_mono_dmic_r_mux =
	SOC_DAPM_ENUM("Mono DMIC R source", rt5677_mono_dmic_r_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo1_dmic_enum, RT5677_STO1_ADC_MIXER,
	RT5677_SEL_STO1_DMIC_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_sto1_dmic_mux =
	SOC_DAPM_ENUM("Stereo1 DMIC source", rt5677_stereo1_dmic_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo2_dmic_enum, RT5677_STO2_ADC_MIXER,
	RT5677_SEL_STO2_DMIC_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_sto2_dmic_mux =
	SOC_DAPM_ENUM("Stereo2 DMIC source", rt5677_stereo2_dmic_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo3_dmic_enum, RT5677_STO3_ADC_MIXER,
	RT5677_SEL_STO3_DMIC_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_sto3_dmic_mux =
	SOC_DAPM_ENUM("Stereo3 DMIC source", rt5677_stereo3_dmic_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo4_dmic_enum, RT5677_STO4_ADC_MIXER,
	RT5677_SEL_STO4_DMIC_SFT, rt5677_dmic_src);

static const struct snd_kcontrol_new rt5677_sto4_dmic_mux =
	SOC_DAPM_ENUM("Stereo4 DMIC source", rt5677_stereo4_dmic_enum);

/* Stereo2 ADC source */ /* MX-26 [0] */
static const char * const rt5677_stereo2_adc_lr_src[] = {
	"L", "LR"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo2_adc_lr_enum, RT5677_STO2_ADC_MIXER,
	RT5677_SEL_STO2_LR_MIX_SFT, rt5677_stereo2_adc_lr_src);

static const struct snd_kcontrol_new rt5677_sto2_adc_lr_mux =
	SOC_DAPM_ENUM("Stereo2 ADC LR source", rt5677_stereo2_adc_lr_enum);

/* Stereo1 ADC Source 1 */ /* MX-27 MX26  MX25 [13:12] */
static const char * const rt5677_stereo_adc1_src[] = {
	"DD MIX1", "ADC1/2", "Stereo DAC MIX"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo1_adc1_enum, RT5677_STO1_ADC_MIXER,
	RT5677_SEL_STO1_ADC1_SFT, rt5677_stereo_adc1_src);

static const struct snd_kcontrol_new rt5677_sto1_adc1_mux =
	SOC_DAPM_ENUM("Stereo1 ADC1 source", rt5677_stereo1_adc1_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo2_adc1_enum, RT5677_STO2_ADC_MIXER,
	RT5677_SEL_STO2_ADC1_SFT, rt5677_stereo_adc1_src);

static const struct snd_kcontrol_new rt5677_sto2_adc1_mux =
	SOC_DAPM_ENUM("Stereo2 ADC1 source", rt5677_stereo2_adc1_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo3_adc1_enum, RT5677_STO3_ADC_MIXER,
	RT5677_SEL_STO3_ADC1_SFT, rt5677_stereo_adc1_src);

static const struct snd_kcontrol_new rt5677_sto3_adc1_mux =
	SOC_DAPM_ENUM("Stereo3 ADC1 source", rt5677_stereo3_adc1_enum);

/* Mono ADC Left source 2 */ /* MX-28 [11:10] */
static const char * const rt5677_mono_adc2_l_src[] = {
	"DD MIX1L", "DMIC", "MONO DAC MIXL"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_adc2_l_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_ADC_L2_SFT, rt5677_mono_adc2_l_src);

static const struct snd_kcontrol_new rt5677_mono_adc2_l_mux =
	SOC_DAPM_ENUM("Mono ADC2 L source", rt5677_mono_adc2_l_enum);

/* Mono ADC Left source 1 */ /* MX-28 [13:12] */
static const char * const rt5677_mono_adc1_l_src[] = {
	"DD MIX1L", "ADC1", "MONO DAC MIXL"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_adc1_l_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_ADC_L1_SFT, rt5677_mono_adc1_l_src);

static const struct snd_kcontrol_new rt5677_mono_adc1_l_mux =
	SOC_DAPM_ENUM("Mono ADC1 L source", rt5677_mono_adc1_l_enum);

/* Mono ADC Right source 2 */ /* MX-28 [3:2] */
static const char * const rt5677_mono_adc2_r_src[] = {
	"DD MIX1R", "DMIC", "MONO DAC MIXR"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_adc2_r_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_ADC_R2_SFT, rt5677_mono_adc2_r_src);

static const struct snd_kcontrol_new rt5677_mono_adc2_r_mux =
	SOC_DAPM_ENUM("Mono ADC2 R source", rt5677_mono_adc2_r_enum);

/* Mono ADC Right source 1 */ /* MX-28 [5:4] */
static const char * const rt5677_mono_adc1_r_src[] = {
	"DD MIX1R", "ADC2", "MONO DAC MIXR"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_mono_adc1_r_enum, RT5677_MONO_ADC_MIXER,
	RT5677_SEL_MONO_ADC_R1_SFT, rt5677_mono_adc1_r_src);

static const struct snd_kcontrol_new rt5677_mono_adc1_r_mux =
	SOC_DAPM_ENUM("Mono ADC1 R source", rt5677_mono_adc1_r_enum);

/* Stereo4 ADC Source 2 */ /* MX-24 [11:10] */
static const char * const rt5677_stereo4_adc2_src[] = {
	"DD MIX1", "DMIC", "DD MIX2"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo4_adc2_enum, RT5677_STO4_ADC_MIXER,
	RT5677_SEL_STO4_ADC2_SFT, rt5677_stereo4_adc2_src);

static const struct snd_kcontrol_new rt5677_sto4_adc2_mux =
	SOC_DAPM_ENUM("Stereo4 ADC2 source", rt5677_stereo4_adc2_enum);


/* Stereo4 ADC Source 1 */ /* MX-24 [13:12] */
static const char * const rt5677_stereo4_adc1_src[] = {
	"DD MIX1", "ADC1/2", "DD MIX2"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_stereo4_adc1_enum, RT5677_STO4_ADC_MIXER,
	RT5677_SEL_STO4_ADC1_SFT, rt5677_stereo4_adc1_src);

static const struct snd_kcontrol_new rt5677_sto4_adc1_mux =
	SOC_DAPM_ENUM("Stereo4 ADC1 source", rt5677_stereo4_adc1_enum);

/* InBound0/1 Source */ /* MX-A3 [14:12] */
static const char * const rt5677_inbound01_src[] = {
	"IF1 DAC 01", "IF2 DAC 01", "SLB DAC 01", "STO1 ADC MIX",
	"VAD ADC/DAC1 FS"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound01_enum, RT5677_DSP_INB_CTRL1,
	RT5677_IB01_SRC_SFT, rt5677_inbound01_src);

static const struct snd_kcontrol_new rt5677_ib01_src_mux =
	SOC_DAPM_ENUM("InBound0/1 Source", rt5677_inbound01_enum);

/* InBound2/3 Source */ /* MX-A3 [10:8] */
static const char * const rt5677_inbound23_src[] = {
	"IF1 DAC 23", "IF2 DAC 23", "SLB DAC 23", "STO2 ADC MIX",
	"DAC1 FS", "IF4 DAC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound23_enum, RT5677_DSP_INB_CTRL1,
	RT5677_IB23_SRC_SFT, rt5677_inbound23_src);

static const struct snd_kcontrol_new rt5677_ib23_src_mux =
	SOC_DAPM_ENUM("InBound2/3 Source", rt5677_inbound23_enum);

/* InBound4/5 Source */ /* MX-A3 [6:4] */
static const char * const rt5677_inbound45_src[] = {
	"IF1 DAC 45", "IF2 DAC 45", "SLB DAC 45", "STO3 ADC MIX",
	"IF3 DAC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound45_enum, RT5677_DSP_INB_CTRL1,
	RT5677_IB45_SRC_SFT, rt5677_inbound45_src);

static const struct snd_kcontrol_new rt5677_ib45_src_mux =
	SOC_DAPM_ENUM("InBound4/5 Source", rt5677_inbound45_enum);

/* InBound6 Source */ /* MX-A3 [2:0] */
static const char * const rt5677_inbound6_src[] = {
	"IF1 DAC 6", "IF2 DAC 6", "SLB DAC 6", "STO4 ADC MIX L",
	"IF4 DAC L", "STO1 ADC MIX L", "STO2 ADC MIX L", "STO3 ADC MIX L"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound6_enum, RT5677_DSP_INB_CTRL1,
	RT5677_IB6_SRC_SFT, rt5677_inbound6_src);

static const struct snd_kcontrol_new rt5677_ib6_src_mux =
	SOC_DAPM_ENUM("InBound6 Source", rt5677_inbound6_enum);

/* InBound7 Source */ /* MX-A4 [14:12] */
static const char * const rt5677_inbound7_src[] = {
	"IF1 DAC 7", "IF2 DAC 7", "SLB DAC 7", "STO4 ADC MIX R",
	"IF4 DAC R", "STO1 ADC MIX R", "STO2 ADC MIX R", "STO3 ADC MIX R"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound7_enum, RT5677_DSP_INB_CTRL2,
	RT5677_IB7_SRC_SFT, rt5677_inbound7_src);

static const struct snd_kcontrol_new rt5677_ib7_src_mux =
	SOC_DAPM_ENUM("InBound7 Source", rt5677_inbound7_enum);

/* InBound8 Source */ /* MX-A4 [10:8] */
static const char * const rt5677_inbound8_src[] = {
	"STO1 ADC MIX L", "STO2 ADC MIX L", "STO3 ADC MIX L", "STO4 ADC MIX L",
	"MONO ADC MIX L", "DACL1 FS"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound8_enum, RT5677_DSP_INB_CTRL2,
	RT5677_IB8_SRC_SFT, rt5677_inbound8_src);

static const struct snd_kcontrol_new rt5677_ib8_src_mux =
	SOC_DAPM_ENUM("InBound8 Source", rt5677_inbound8_enum);

/* InBound9 Source */ /* MX-A4 [6:4] */
static const char * const rt5677_inbound9_src[] = {
	"STO1 ADC MIX R", "STO2 ADC MIX R", "STO3 ADC MIX R", "STO4 ADC MIX R",
	"MONO ADC MIX R", "DACR1 FS", "DAC1 FS"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_inbound9_enum, RT5677_DSP_INB_CTRL2,
	RT5677_IB9_SRC_SFT, rt5677_inbound9_src);

static const struct snd_kcontrol_new rt5677_ib9_src_mux =
	SOC_DAPM_ENUM("InBound9 Source", rt5677_inbound9_enum);

/* VAD Source */ /* MX-9F [6:4] */
static const char * const rt5677_vad_src[] = {
	"STO1 ADC MIX L", "MONO ADC MIX L", "MONO ADC MIX R", "STO2 ADC MIX L",
	"STO3 ADC MIX L"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_vad_enum, RT5677_VAD_CTRL4,
	RT5677_VAD_SRC_SFT, rt5677_vad_src);

static const struct snd_kcontrol_new rt5677_vad_src_mux =
	SOC_DAPM_ENUM("VAD Source", rt5677_vad_enum);

/* Sidetone Source */ /* MX-13 [11:9] */
static const char * const rt5677_sidetone_src[] = {
	"DMIC1 L", "DMIC2 L", "DMIC3 L", "DMIC4 L", "ADC1", "ADC2"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_sidetone_enum, RT5677_SIDETONE_CTRL,
	RT5677_ST_SEL_SFT, rt5677_sidetone_src);

static const struct snd_kcontrol_new rt5677_sidetone_mux =
	SOC_DAPM_ENUM("Sidetone Source", rt5677_sidetone_enum);

/* DAC1/2 Source */ /* MX-15 [1:0] */
static const char * const rt5677_dac12_src[] = {
	"STO1 DAC MIX", "MONO DAC MIX", "DD MIX1", "DD MIX2"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac12_enum, RT5677_ANA_DAC1_2_3_SRC,
	RT5677_ANA_DAC1_2_SRC_SEL_SFT, rt5677_dac12_src);

static const struct snd_kcontrol_new rt5677_dac12_mux =
	SOC_DAPM_ENUM("Analog DAC1/2 Source", rt5677_dac12_enum);

/* DAC3 Source */ /* MX-15 [5:4] */
static const char * const rt5677_dac3_src[] = {
	"MONO DAC MIXL", "MONO DAC MIXR", "DD MIX1L", "DD MIX2L"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_dac3_enum, RT5677_ANA_DAC1_2_3_SRC,
	RT5677_ANA_DAC3_SRC_SEL_SFT, rt5677_dac3_src);

static const struct snd_kcontrol_new rt5677_dac3_mux =
	SOC_DAPM_ENUM("Analog DAC3 Source", rt5677_dac3_enum);

/* PDM channel source */ /* MX-31 [13:12][9:8][5:4][1:0] */
static const char * const rt5677_pdm_src[] = {
	"STO1 DAC MIX", "MONO DAC MIX", "DD MIX1", "DD MIX2"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_pdm1_l_enum, RT5677_PDM_OUT_CTRL,
	RT5677_SEL_PDM1_L_SFT, rt5677_pdm_src);

static const struct snd_kcontrol_new rt5677_pdm1_l_mux =
	SOC_DAPM_ENUM("PDM1 source", rt5677_pdm1_l_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_pdm2_l_enum, RT5677_PDM_OUT_CTRL,
	RT5677_SEL_PDM2_L_SFT, rt5677_pdm_src);

static const struct snd_kcontrol_new rt5677_pdm2_l_mux =
	SOC_DAPM_ENUM("PDM2 source", rt5677_pdm2_l_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_pdm1_r_enum, RT5677_PDM_OUT_CTRL,
	RT5677_SEL_PDM1_R_SFT, rt5677_pdm_src);

static const struct snd_kcontrol_new rt5677_pdm1_r_mux =
	SOC_DAPM_ENUM("PDM1 source", rt5677_pdm1_r_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_pdm2_r_enum, RT5677_PDM_OUT_CTRL,
	RT5677_SEL_PDM2_R_SFT, rt5677_pdm_src);

static const struct snd_kcontrol_new rt5677_pdm2_r_mux =
	SOC_DAPM_ENUM("PDM2 source", rt5677_pdm2_r_enum);

/* TDM IF1/2 SLB ADC1 Data Selection */ /* MX-3C MX-41 [5:4] MX-08 [1:0]*/
static const char * const rt5677_if12_adc1_src[] = {
	"STO1 ADC MIX", "OB01", "VAD ADC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if1_adc1_enum, RT5677_TDM1_CTRL2,
	RT5677_IF1_ADC1_SFT, rt5677_if12_adc1_src);

static const struct snd_kcontrol_new rt5677_if1_adc1_mux =
	SOC_DAPM_ENUM("IF1 ADC1 source", rt5677_if1_adc1_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if2_adc1_enum, RT5677_TDM2_CTRL2,
	RT5677_IF2_ADC1_SFT, rt5677_if12_adc1_src);

static const struct snd_kcontrol_new rt5677_if2_adc1_mux =
	SOC_DAPM_ENUM("IF2 ADC1 source", rt5677_if2_adc1_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_slb_adc1_enum, RT5677_SLIMBUS_RX,
	RT5677_SLB_ADC1_SFT, rt5677_if12_adc1_src);

static const struct snd_kcontrol_new rt5677_slb_adc1_mux =
	SOC_DAPM_ENUM("SLB ADC1 source", rt5677_slb_adc1_enum);

/* TDM IF1/2 SLB ADC2 Data Selection */ /* MX-3C MX-41 [7:6] MX-08 [3:2] */
static const char * const rt5677_if12_adc2_src[] = {
	"STO2 ADC MIX", "OB23"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if1_adc2_enum, RT5677_TDM1_CTRL2,
	RT5677_IF1_ADC2_SFT, rt5677_if12_adc2_src);

static const struct snd_kcontrol_new rt5677_if1_adc2_mux =
	SOC_DAPM_ENUM("IF1 ADC2 source", rt5677_if1_adc2_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if2_adc2_enum, RT5677_TDM2_CTRL2,
	RT5677_IF2_ADC2_SFT, rt5677_if12_adc2_src);

static const struct snd_kcontrol_new rt5677_if2_adc2_mux =
	SOC_DAPM_ENUM("IF2 ADC2 source", rt5677_if2_adc2_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_slb_adc2_enum, RT5677_SLIMBUS_RX,
	RT5677_SLB_ADC2_SFT, rt5677_if12_adc2_src);

static const struct snd_kcontrol_new rt5677_slb_adc2_mux =
	SOC_DAPM_ENUM("SLB ADC2 source", rt5677_slb_adc2_enum);

/* TDM IF1/2 SLB ADC3 Data Selection */ /* MX-3C MX-41 [9:8] MX-08 [5:4] */
static const char * const rt5677_if12_adc3_src[] = {
	"STO3 ADC MIX", "MONO ADC MIX", "OB45"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if1_adc3_enum, RT5677_TDM1_CTRL2,
	RT5677_IF1_ADC3_SFT, rt5677_if12_adc3_src);

static const struct snd_kcontrol_new rt5677_if1_adc3_mux =
	SOC_DAPM_ENUM("IF1 ADC3 source", rt5677_if1_adc3_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if2_adc3_enum, RT5677_TDM2_CTRL2,
	RT5677_IF2_ADC3_SFT, rt5677_if12_adc3_src);

static const struct snd_kcontrol_new rt5677_if2_adc3_mux =
	SOC_DAPM_ENUM("IF2 ADC3 source", rt5677_if2_adc3_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_slb_adc3_enum, RT5677_SLIMBUS_RX,
	RT5677_SLB_ADC3_SFT, rt5677_if12_adc3_src);

static const struct snd_kcontrol_new rt5677_slb_adc3_mux =
	SOC_DAPM_ENUM("SLB ADC3 source", rt5677_slb_adc3_enum);

/* TDM IF1/2 SLB ADC4 Data Selection */ /* MX-3C MX-41 [11:10]  MX-08 [7:6] */
static const char * const rt5677_if12_adc4_src[] = {
	"STO4 ADC MIX", "OB67", "OB01"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if1_adc4_enum, RT5677_TDM1_CTRL2,
	RT5677_IF1_ADC4_SFT, rt5677_if12_adc4_src);

static const struct snd_kcontrol_new rt5677_if1_adc4_mux =
	SOC_DAPM_ENUM("IF1 ADC4 source", rt5677_if1_adc4_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if2_adc4_enum, RT5677_TDM2_CTRL2,
	RT5677_IF2_ADC4_SFT, rt5677_if12_adc4_src);

static const struct snd_kcontrol_new rt5677_if2_adc4_mux =
	SOC_DAPM_ENUM("IF2 ADC4 source", rt5677_if2_adc4_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_slb_adc4_enum, RT5677_SLIMBUS_RX,
	RT5677_SLB_ADC4_SFT, rt5677_if12_adc4_src);

static const struct snd_kcontrol_new rt5677_slb_adc4_mux =
	SOC_DAPM_ENUM("SLB ADC4 source", rt5677_slb_adc4_enum);

/* Interface3/4 ADC Data Input */ /* MX-2F [3:0] MX-30 [7:4]*/
static const char * const rt5677_if34_adc_src[] = {
	"STO1 ADC MIX", "STO2 ADC MIX", "STO3 ADC MIX", "STO4 ADC MIX",
	"MONO ADC MIX", "OB01", "OB23", "VAD ADC"
};

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if3_adc_enum, RT5677_IF3_DATA,
	RT5677_IF3_ADC_IN_SFT, rt5677_if34_adc_src);

static const struct snd_kcontrol_new rt5677_if3_adc_mux =
	SOC_DAPM_ENUM("IF3 ADC source", rt5677_if3_adc_enum);

static const SOC_ENUM_SINGLE_DECL(
	rt5677_if4_adc_enum, RT5677_IF4_DATA,
	RT5677_IF4_ADC_IN_SFT, rt5677_if34_adc_src);

static const struct snd_kcontrol_new rt5677_if4_adc_mux =
	SOC_DAPM_ENUM("IF4 ADC source", rt5677_if4_adc_enum);

static int rt5677_adc_clk_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		rt5677_index_update_bits(codec,
			RT5677_CHOP_DAC_ADC, 0x1000, 0x1000);
		break;

	case SND_SOC_DAPM_POST_PMD:
		rt5677_index_update_bits(codec,
			RT5677_CHOP_DAC_ADC, 0x1000, 0x0000);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_sto1_adcl_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_STO1_ADC_DIG_VOL,
			RT5677_L_MUTE, 0);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_STO1_ADC_DIG_VOL,
			RT5677_L_MUTE,
			RT5677_L_MUTE);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_sto1_adcr_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_STO1_ADC_DIG_VOL,
			RT5677_R_MUTE, 0);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_STO1_ADC_DIG_VOL,
			RT5677_R_MUTE,
			RT5677_R_MUTE);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_mono_adcl_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int val;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_read(rt5677->regmap, RT5677_DMIC_CTRL1, &val);
		if (val & RT5677_DMIC_1_EN_MASK)
			msleep(dmic_depop_time);
		else
			msleep(amic_depop_time);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_mono_adcr_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int val;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_read(rt5677->regmap, RT5677_DMIC_CTRL1, &val);
		if (val & RT5677_DMIC_2_EN_MASK)
			msleep(dmic_depop_time);
		else
			msleep(amic_depop_time);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_lout1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO1, RT5677_PWR_LO1);
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT1_L_MUTE, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT1_L_MUTE, RT5677_LOUT1_L_MUTE);
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO1, 0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_lout2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO2, RT5677_PWR_LO2);
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT2_L_MUTE, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT2_L_MUTE, RT5677_LOUT2_L_MUTE);
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO2, 0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_lout3_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO3, RT5677_PWR_LO3);
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT3_L_MUTE, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_LOUT1,
			RT5677_LOUT3_L_MUTE, RT5677_LOUT3_L_MUTE);
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
			RT5677_PWR_LO3, 0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_set_dmic1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int val;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_read(rt5677->regmap, RT5677_DMIC_CTRL1, &val);
		if (!(val & RT5677_DMIC_2_EN_MASK)) {
			regmap_update_bits(rt5677->regmap, RT5677_GPIO_CTRL2,
				0x4000, 0x0);
			regmap_update_bits(rt5677->regmap, RT5677_GEN_CTRL2,
				0x0200, 0x0);
		}
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_1L_LH_MASK | RT5677_DMIC_1R_LH_MASK,
			RT5677_DMIC_1L_LH_FALLING | RT5677_DMIC_1R_LH_RISING);
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_1_EN_MASK, RT5677_DMIC_1_EN);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_1_EN_MASK, RT5677_DMIC_1_DIS);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5677_set_dmic2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_GPIO_CTRL2, 0x4000,
			0x4000);
		regmap_update_bits(rt5677->regmap, RT5677_GEN_CTRL2, 0x0200,
			0x0200);
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_2L_LH_MASK | RT5677_DMIC_2R_LH_MASK,
			RT5677_DMIC_2L_LH_FALLING | RT5677_DMIC_2R_LH_RISING);
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_2_EN_MASK, RT5677_DMIC_2_EN);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_2_EN_MASK, RT5677_DMIC_2_DIS);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5677_set_dmic3_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_3L_LH_MASK | RT5677_DMIC_3R_LH_MASK,
			RT5677_DMIC_3L_LH_FALLING | RT5677_DMIC_3R_LH_RISING);
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_3_EN_MASK, RT5677_DMIC_3_EN);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL1,
			RT5677_DMIC_3_EN_MASK, RT5677_DMIC_3_DIS);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5677_set_dmic4_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_4L_LH_MASK | RT5677_DMIC_4R_LH_MASK,
			RT5677_DMIC_4L_LH_FALLING | RT5677_DMIC_4R_LH_RISING);
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_4_EN_MASK, RT5677_DMIC_4_EN);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_DMIC_CTRL2,
			RT5677_DMIC_4_EN_MASK, RT5677_DMIC_4_DIS);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5677_bst1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
			RT5677_PWR_BST1_P, RT5677_PWR_BST1_P);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
			RT5677_PWR_BST1_P, 0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_bst2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
			RT5677_PWR_BST2_P, RT5677_PWR_BST2_P);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
			RT5677_PWR_BST2_P, 0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_pdm1_l_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM1_L, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM1_L, RT5677_M_PDM1_L);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_pdm1_r_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM1_R, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM1_R, RT5677_M_PDM1_R);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_pdm2_l_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM2_L, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM2_L, RT5677_M_PDM2_L);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_pdm2_r_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM2_R, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(rt5677->regmap, RT5677_PDM_OUT_CTRL,
			RT5677_M_PDM2_R, RT5677_M_PDM2_R);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5677_post_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		break;
	default:
		return 0;
	}
	return 0;
}

static int rt5677_pre_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	switch (event) {
	case SND_SOC_DAPM_PRE_PMD:
		break;
	case SND_SOC_DAPM_PRE_PMU:
		break;
	default:
		return 0;
	}
	return 0;
}

static int rt5677_set_pll1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PLL1_CTRL2, 0x2, 0x2);
		regmap_update_bits(rt5677->regmap, RT5677_PLL1_CTRL2, 0x2, 0x0);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5677_set_pll2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(rt5677->regmap, RT5677_PLL2_CTRL2, 0x2, 0x2);
		regmap_update_bits(rt5677->regmap, RT5677_PLL2_CTRL2, 0x2, 0x0);
		break;
	default:
		return 0;
	}

	return 0;
}

static const struct snd_soc_dapm_widget rt5677_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("PLL1", 0, RT5677_PWR_ANLG2, RT5677_PWR_PLL1_BIT,
		0, rt5677_set_pll1_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_SUPPLY_S("PLL2", 0, RT5677_PWR_ANLG2, RT5677_PWR_PLL2_BIT,
		0, rt5677_set_pll2_event, SND_SOC_DAPM_POST_PMU),

	/* Input Side */
	/* Input Lines */
	SND_SOC_DAPM_INPUT("DMIC L1"),
	SND_SOC_DAPM_INPUT("DMIC R1"),
	SND_SOC_DAPM_INPUT("DMIC L2"),
	SND_SOC_DAPM_INPUT("DMIC R2"),
	SND_SOC_DAPM_INPUT("DMIC L3"),
	SND_SOC_DAPM_INPUT("DMIC R3"),
	SND_SOC_DAPM_INPUT("DMIC L4"),
	SND_SOC_DAPM_INPUT("DMIC R4"),

	SND_SOC_DAPM_INPUT("IN1P"),
	SND_SOC_DAPM_INPUT("IN1N"),
	SND_SOC_DAPM_INPUT("IN2P"),
	SND_SOC_DAPM_INPUT("IN2N"),

	SND_SOC_DAPM_INPUT("Haptic Generator"),

	SND_SOC_DAPM_PGA_E("DMIC1", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5677_set_dmic1_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("DMIC2", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5677_set_dmic2_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("DMIC3", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5677_set_dmic3_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("DMIC4", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5677_set_dmic4_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("DMIC CLK", SND_SOC_NOPM, 0, 0,
		set_dmic_clk, SND_SOC_DAPM_PRE_PMU),

	/* Boost */
	SND_SOC_DAPM_PGA_E("BST1", RT5677_PWR_ANLG2,
		RT5677_PWR_BST1_BIT, 0, NULL, 0, rt5677_bst1_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_E("BST2", RT5677_PWR_ANLG2,
		RT5677_PWR_BST2_BIT, 0, NULL, 0, rt5677_bst2_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),

	/* ADCs */
	SND_SOC_DAPM_ADC("ADC 1", NULL, SND_SOC_NOPM,
		0, 0),
	SND_SOC_DAPM_ADC("ADC 2", NULL, SND_SOC_NOPM,
		0, 0),
	SND_SOC_DAPM_PGA("ADC 1_2", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("ADC 1 power", RT5677_PWR_DIG1,
		RT5677_PWR_ADC_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC 2 power", RT5677_PWR_DIG1,
		RT5677_PWR_ADC_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC clock", SND_SOC_NOPM, 0, 0,
		rt5677_adc_clk_event, SND_SOC_DAPM_POST_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_SUPPLY("ADC1 clock", RT5677_PWR_DIG1,
		RT5677_PWR_ADCFED1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC2 clock", RT5677_PWR_DIG1,
		RT5677_PWR_ADCFED2_BIT, 0, NULL, 0),

	/* ADC Mux */
	SND_SOC_DAPM_MUX("Stereo1 DMIC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto1_dmic_mux),
	SND_SOC_DAPM_MUX("Stereo1 ADC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto1_adc1_mux),
	SND_SOC_DAPM_MUX("Stereo1 ADC2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto1_adc2_mux),
	SND_SOC_DAPM_MUX("Stereo2 DMIC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto2_dmic_mux),
	SND_SOC_DAPM_MUX("Stereo2 ADC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto2_adc1_mux),
	SND_SOC_DAPM_MUX("Stereo2 ADC2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto2_adc2_mux),
	SND_SOC_DAPM_MUX("Stereo2 ADC LR Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto2_adc_lr_mux),
	SND_SOC_DAPM_MUX("Stereo3 DMIC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto3_dmic_mux),
	SND_SOC_DAPM_MUX("Stereo3 ADC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto3_adc1_mux),
	SND_SOC_DAPM_MUX("Stereo3 ADC2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto3_adc2_mux),
	SND_SOC_DAPM_MUX("Stereo4 DMIC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto4_dmic_mux),
	SND_SOC_DAPM_MUX("Stereo4 ADC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto4_adc1_mux),
	SND_SOC_DAPM_MUX("Stereo4 ADC2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_sto4_adc2_mux),
	SND_SOC_DAPM_MUX("Mono DMIC L Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_dmic_l_mux),
	SND_SOC_DAPM_MUX("Mono DMIC R Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_dmic_r_mux),
	SND_SOC_DAPM_MUX("Mono ADC2 L Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_adc2_l_mux),
	SND_SOC_DAPM_MUX("Mono ADC1 L Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_adc1_l_mux),
	SND_SOC_DAPM_MUX("Mono ADC1 R Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_adc1_r_mux),
	SND_SOC_DAPM_MUX("Mono ADC2 R Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_mono_adc2_r_mux),

	/* ADC Mixer */
	SND_SOC_DAPM_SUPPLY("adc stereo1 filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_S1F_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("adc stereo2 filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_S2F_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("adc stereo3 filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_S3F_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("adc stereo4 filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_S4F_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER_E("Sto1 ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_sto1_adc_l_mix, ARRAY_SIZE(rt5677_sto1_adc_l_mix),
		rt5677_sto1_adcl_event, SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("Sto1 ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_sto1_adc_r_mix, ARRAY_SIZE(rt5677_sto1_adc_r_mix),
		rt5677_sto1_adcr_event, SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER("Sto2 ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_sto2_adc_l_mix, ARRAY_SIZE(rt5677_sto2_adc_l_mix)),
	SND_SOC_DAPM_MIXER("Sto2 ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_sto2_adc_r_mix, ARRAY_SIZE(rt5677_sto2_adc_r_mix)),
	SND_SOC_DAPM_MIXER("Sto3 ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_sto3_adc_l_mix, ARRAY_SIZE(rt5677_sto3_adc_l_mix)),
	SND_SOC_DAPM_MIXER("Sto3 ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_sto3_adc_r_mix, ARRAY_SIZE(rt5677_sto3_adc_r_mix)),

	SND_SOC_DAPM_MIXER("Sto4 ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_sto4_adc_l_mix, ARRAY_SIZE(rt5677_sto4_adc_l_mix)),
	SND_SOC_DAPM_MIXER("Sto4 ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_sto4_adc_r_mix, ARRAY_SIZE(rt5677_sto4_adc_r_mix)),
	SND_SOC_DAPM_SUPPLY("adc mono left filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_MF_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("Mono ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_mono_adc_l_mix, ARRAY_SIZE(rt5677_mono_adc_l_mix)),
	SND_SOC_DAPM_SUPPLY("adc mono right filter", RT5677_PWR_DIG2,
		RT5677_PWR_ADC_MF_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("Mono ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_mono_adc_r_mix, ARRAY_SIZE(rt5677_mono_adc_r_mix)),

	SND_SOC_DAPM_ADC_E("Mono ADC MIXL ADC", NULL, RT5677_MONO_ADC_DIG_VOL,
		RT5677_L_MUTE_SFT, 1, rt5677_mono_adcl_event,
		SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_ADC_E("Mono ADC MIXR ADC", NULL, RT5677_MONO_ADC_DIG_VOL,
		RT5677_R_MUTE_SFT, 1, rt5677_mono_adcr_event,
		SND_SOC_DAPM_PRE_PMU),

	/* ADC PGA */
	SND_SOC_DAPM_PGA("Stereo1 ADC MIXL", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo1 ADC MIXR", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo1 ADC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo2 ADC MIXL", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo2 ADC MIXR", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo2 ADC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo3 ADC MIXL", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo3 ADC MIXR", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo3 ADC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo4 ADC MIXL", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo4 ADC MIXR", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Stereo4 ADC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Sto2 ADC LR MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Mono ADC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1_ADC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1_ADC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1_ADC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1_ADC4", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* DSP */
	SND_SOC_DAPM_MUX("IB9 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib9_src_mux),
	SND_SOC_DAPM_MUX("IB8 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib8_src_mux),
	SND_SOC_DAPM_MUX("IB7 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib7_src_mux),
	SND_SOC_DAPM_MUX("IB6 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib6_src_mux),
	SND_SOC_DAPM_MUX("IB45 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib45_src_mux),
	SND_SOC_DAPM_MUX("IB23 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib23_src_mux),
	SND_SOC_DAPM_MUX("IB01 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib01_src_mux),
	SND_SOC_DAPM_MUX("IB45 Bypass Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib45_bypass_src_mux),
	SND_SOC_DAPM_MUX("IB23 Bypass Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib23_bypass_src_mux),
	SND_SOC_DAPM_MUX("IB01 Bypass Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ib01_bypass_src_mux),
	SND_SOC_DAPM_MUX("OB23 Bypass Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ob23_bypass_src_mux),
	SND_SOC_DAPM_MUX("OB01 Bypass Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_ob01_bypass_src_mux),

	SND_SOC_DAPM_PGA("OB45", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OB67", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_PGA("OutBound2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OutBound3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OutBound4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OutBound5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OutBound6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OutBound7", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Digital Interface */
	SND_SOC_DAPM_SUPPLY("I2S1", RT5677_PWR_DIG1,
		RT5677_PWR_I2S1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC0", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC7", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC01", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC23", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC45", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC67", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC4", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("I2S2", RT5677_PWR_DIG1,
		RT5677_PWR_I2S2_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC0", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC7", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC01", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC23", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC45", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC67", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC4", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("I2S3", RT5677_PWR_DIG1,
		RT5677_PWR_I2S3_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC R", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("I2S4", RT5677_PWR_DIG1,
		RT5677_PWR_I2S4_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 DAC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 DAC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 DAC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 ADC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 ADC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF4 ADC R", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("SLB", RT5677_PWR_DIG1,
		RT5677_PWR_SLB_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC0", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC7", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC01", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC23", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC45", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB DAC67", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB ADC1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB ADC2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB ADC3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SLB ADC4", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Digital Interface Select */
	SND_SOC_DAPM_MUX("IF1 ADC1 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if1_adc1_mux),
	SND_SOC_DAPM_MUX("IF1 ADC2 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if1_adc2_mux),
	SND_SOC_DAPM_MUX("IF1 ADC3 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if1_adc3_mux),
	SND_SOC_DAPM_MUX("IF1 ADC4 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if1_adc4_mux),
	SND_SOC_DAPM_MUX("IF2 ADC1 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if2_adc1_mux),
	SND_SOC_DAPM_MUX("IF2 ADC2 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if2_adc2_mux),
	SND_SOC_DAPM_MUX("IF2 ADC3 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if2_adc3_mux),
	SND_SOC_DAPM_MUX("IF2 ADC4 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if2_adc4_mux),
	SND_SOC_DAPM_MUX("IF3 ADC Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if3_adc_mux),
	SND_SOC_DAPM_MUX("IF4 ADC Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_if4_adc_mux),
	SND_SOC_DAPM_MUX("SLB ADC1 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_slb_adc1_mux),
	SND_SOC_DAPM_MUX("SLB ADC2 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_slb_adc2_mux),
	SND_SOC_DAPM_MUX("SLB ADC3 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_slb_adc3_mux),
	SND_SOC_DAPM_MUX("SLB ADC4 Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_slb_adc4_mux),

	/* Audio Interface */
	SND_SOC_DAPM_AIF_IN("AIF1RX", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF1TX", "AIF1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF2RX", "AIF2 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF2TX", "AIF2 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF3RX", "AIF3 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF3TX", "AIF3 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF4RX", "AIF4 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF4TX", "AIF4 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("SLBRX", "SLIMBus Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SLBTX", "SLIMBus Capture", 0, SND_SOC_NOPM, 0, 0),

	/* Sidetone Mux */
	SND_SOC_DAPM_MUX("Sidetone Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_sidetone_mux),
	/* VAD Mux*/
	SND_SOC_DAPM_MUX("VAD ADC Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_vad_src_mux),

	/* Tensilica DSP */
	SND_SOC_DAPM_PGA("Tensilica DSP", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("OB01 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_01_mix, ARRAY_SIZE(rt5677_ob_01_mix)),
	SND_SOC_DAPM_MIXER("OB23 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_23_mix, ARRAY_SIZE(rt5677_ob_23_mix)),
	SND_SOC_DAPM_MIXER("OB4 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_4_mix, ARRAY_SIZE(rt5677_ob_4_mix)),
	SND_SOC_DAPM_MIXER("OB5 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_5_mix, ARRAY_SIZE(rt5677_ob_5_mix)),
	SND_SOC_DAPM_MIXER("OB6 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_6_mix, ARRAY_SIZE(rt5677_ob_6_mix)),
	SND_SOC_DAPM_MIXER("OB7 MIX", SND_SOC_NOPM, 0, 0,
		rt5677_ob_7_mix, ARRAY_SIZE(rt5677_ob_7_mix)),

	/* Output Side */
	/* DAC mixer before sound effect  */
	SND_SOC_DAPM_MIXER("DAC1 MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_dac_l_mix, ARRAY_SIZE(rt5677_dac_l_mix)),
	SND_SOC_DAPM_MIXER("DAC1 MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_dac_r_mix, ARRAY_SIZE(rt5677_dac_r_mix)),
	SND_SOC_DAPM_PGA("DAC1 FS", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* DAC Mux */
	SND_SOC_DAPM_MUX("DAC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_dac1_mux),
	SND_SOC_DAPM_MUX("ADDA1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_adda1_mux),
	SND_SOC_DAPM_MUX("DAC12 SRC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_dac12_mux),
	SND_SOC_DAPM_MUX("DAC3 SRC Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_dac3_mux),

	/* DAC2 channel Mux */
	SND_SOC_DAPM_MUX("DAC2 L Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_dac2_l_mux),
	SND_SOC_DAPM_MUX("DAC2 R Mux", SND_SOC_NOPM, 0, 0,
				&rt5677_dac2_r_mux),

	/* DAC3 channel Mux */
	SND_SOC_DAPM_MUX("DAC3 L Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_dac3_l_mux),
	SND_SOC_DAPM_MUX("DAC3 R Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_dac3_r_mux),

	/* DAC4 channel Mux */
	SND_SOC_DAPM_MUX("DAC4 L Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_dac4_l_mux),
	SND_SOC_DAPM_MUX("DAC4 R Mux", SND_SOC_NOPM, 0, 0,
			&rt5677_dac4_r_mux),

	/* DAC Mixer */
	SND_SOC_DAPM_SUPPLY("dac stereo1 filter", RT5677_PWR_DIG2,
		RT5677_PWR_DAC_S1F_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("dac mono left filter", RT5677_PWR_DIG2,
		RT5677_PWR_DAC_M2F_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("dac mono right filter", RT5677_PWR_DIG2,
		RT5677_PWR_DAC_M2F_R_BIT, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("Stereo DAC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_sto1_dac_l_mix, ARRAY_SIZE(rt5677_sto1_dac_l_mix)),
	SND_SOC_DAPM_MIXER("Stereo DAC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_sto1_dac_r_mix, ARRAY_SIZE(rt5677_sto1_dac_r_mix)),
	SND_SOC_DAPM_MIXER("Mono DAC MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_mono_dac_l_mix, ARRAY_SIZE(rt5677_mono_dac_l_mix)),
	SND_SOC_DAPM_MIXER("Mono DAC MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_mono_dac_r_mix, ARRAY_SIZE(rt5677_mono_dac_r_mix)),
	SND_SOC_DAPM_MIXER("DD1 MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_dd1_l_mix, ARRAY_SIZE(rt5677_dd1_l_mix)),
	SND_SOC_DAPM_MIXER("DD1 MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_dd1_r_mix, ARRAY_SIZE(rt5677_dd1_r_mix)),
	SND_SOC_DAPM_MIXER("DD2 MIXL", SND_SOC_NOPM, 0, 0,
		rt5677_dd2_l_mix, ARRAY_SIZE(rt5677_dd2_l_mix)),
	SND_SOC_DAPM_MIXER("DD2 MIXR", SND_SOC_NOPM, 0, 0,
		rt5677_dd2_r_mix, ARRAY_SIZE(rt5677_dd2_r_mix)),
	SND_SOC_DAPM_PGA("Stereo DAC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Mono DAC MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("DD1 MIX", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("DD2 MIX", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* DACs */
	SND_SOC_DAPM_DAC("DAC 1", NULL, RT5677_PWR_DIG1,
		RT5677_PWR_DAC1_BIT, 0),
	SND_SOC_DAPM_DAC("DAC 2", NULL, RT5677_PWR_DIG1,
		RT5677_PWR_DAC2_BIT, 0),
	SND_SOC_DAPM_DAC("DAC 3", NULL, RT5677_PWR_DIG1,
		RT5677_PWR_DAC3_BIT, 0),

	/* PDM */
	SND_SOC_DAPM_SUPPLY("PDM1 Power", RT5677_PWR_DIG2,
		RT5677_PWR_PDM1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("PDM2 Power", RT5677_PWR_DIG2,
		RT5677_PWR_PDM2_BIT, 0, NULL, 0),

	SND_SOC_DAPM_MUX_E("PDM1 L Mux", SND_SOC_NOPM, 0, 0,
		&rt5677_pdm1_l_mux, rt5677_pdm1_l_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MUX_E("PDM1 R Mux", SND_SOC_NOPM, 0, 0,
		&rt5677_pdm1_r_mux, rt5677_pdm1_r_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MUX_E("PDM2 L Mux", SND_SOC_NOPM, 0, 0,
		&rt5677_pdm2_l_mux, rt5677_pdm2_l_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MUX_E("PDM2 R Mux", SND_SOC_NOPM, 0, 0,
		&rt5677_pdm2_r_mux, rt5677_pdm2_r_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_PGA_S("LOUT1 amp", 1, SND_SOC_NOPM, 0, 0,
		rt5677_lout1_event, SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("LOUT2 amp", 1, SND_SOC_NOPM, 0, 0,
		rt5677_lout2_event, SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("LOUT3 amp", 1, SND_SOC_NOPM, 0, 0,
		rt5677_lout3_event, SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU),

	/* Output Lines */
	SND_SOC_DAPM_OUTPUT("LOUT1"),
	SND_SOC_DAPM_OUTPUT("LOUT2"),
	SND_SOC_DAPM_OUTPUT("LOUT3"),
	SND_SOC_DAPM_OUTPUT("PDM1L"),
	SND_SOC_DAPM_OUTPUT("PDM1R"),
	SND_SOC_DAPM_OUTPUT("PDM2L"),
	SND_SOC_DAPM_OUTPUT("PDM2R"),

	SND_SOC_DAPM_POST("DAPM_POST", rt5677_post_event),
	SND_SOC_DAPM_PRE("DAPM_PRE", rt5677_pre_event),
};

static const struct snd_soc_dapm_route rt5677_dapm_routes[] = {
	{ "DMIC1", NULL, "DMIC L1" },
	{ "DMIC1", NULL, "DMIC R1" },
	{ "DMIC2", NULL, "DMIC L2" },
	{ "DMIC2", NULL, "DMIC R2" },
	{ "DMIC3", NULL, "DMIC L3" },
	{ "DMIC3", NULL, "DMIC R3" },
	{ "DMIC4", NULL, "DMIC L4" },
	{ "DMIC4", NULL, "DMIC R4" },

	{ "DMIC L1", NULL, "DMIC CLK" },
	{ "DMIC R1", NULL, "DMIC CLK" },
	{ "DMIC L2", NULL, "DMIC CLK" },
	{ "DMIC R2", NULL, "DMIC CLK" },
	{ "DMIC L3", NULL, "DMIC CLK" },
	{ "DMIC R3", NULL, "DMIC CLK" },
	{ "DMIC L4", NULL, "DMIC CLK" },
	{ "DMIC R4", NULL, "DMIC CLK" },

	{ "BST1", NULL, "IN1P" },
	{ "BST1", NULL, "IN1N" },
	{ "BST2", NULL, "IN2P" },
	{ "BST2", NULL, "IN2N" },

	{ "ADC 1", NULL, "BST1" },
	{ "ADC 1", NULL, "ADC 1 power" },
	{ "ADC 1", NULL, "ADC clock" },
	{ "ADC 1", NULL, "ADC1 clock" },
	{ "ADC 2", NULL, "BST2" },
	{ "ADC 2", NULL, "ADC 2 power" },
	{ "ADC 2", NULL, "ADC clock" },
	{ "ADC 2", NULL, "ADC2 clock" },

	{ "Stereo1 DMIC Mux", "DMIC1", "DMIC1" },
	{ "Stereo1 DMIC Mux", "DMIC2", "DMIC2" },
	{ "Stereo1 DMIC Mux", "DMIC3", "DMIC3" },
	{ "Stereo1 DMIC Mux", "DMIC4", "DMIC4" },

	{ "Stereo2 DMIC Mux", "DMIC1", "DMIC1" },
	{ "Stereo2 DMIC Mux", "DMIC2", "DMIC2" },
	{ "Stereo2 DMIC Mux", "DMIC3", "DMIC3" },
	{ "Stereo2 DMIC Mux", "DMIC4", "DMIC4" },

	{ "Stereo3 DMIC Mux", "DMIC1", "DMIC1" },
	{ "Stereo3 DMIC Mux", "DMIC2", "DMIC2" },
	{ "Stereo3 DMIC Mux", "DMIC3", "DMIC3" },
	{ "Stereo3 DMIC Mux", "DMIC4", "DMIC4" },

	{ "Stereo4 DMIC Mux", "DMIC1", "DMIC1" },
	{ "Stereo4 DMIC Mux", "DMIC2", "DMIC2" },
	{ "Stereo4 DMIC Mux", "DMIC3", "DMIC3" },
	{ "Stereo4 DMIC Mux", "DMIC4", "DMIC4" },

	{ "Mono DMIC L Mux", "DMIC1", "DMIC1" },
	{ "Mono DMIC L Mux", "DMIC2", "DMIC2" },
	{ "Mono DMIC L Mux", "DMIC3", "DMIC3" },
	{ "Mono DMIC L Mux", "DMIC4", "DMIC4" },

	{ "Mono DMIC R Mux", "DMIC1", "DMIC1" },
	{ "Mono DMIC R Mux", "DMIC2", "DMIC2" },
	{ "Mono DMIC R Mux", "DMIC3", "DMIC3" },
	{ "Mono DMIC R Mux", "DMIC4", "DMIC4" },

	{ "ADC 1_2", NULL, "ADC 1" },
	{ "ADC 1_2", NULL, "ADC 2" },

	{ "Stereo1 ADC1 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo1 ADC1 Mux", "ADC1/2", "ADC 1_2" },
	{ "Stereo1 ADC1 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo1 ADC2 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo1 ADC2 Mux", "DMIC", "Stereo1 DMIC Mux" },
	{ "Stereo1 ADC2 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo2 ADC1 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo2 ADC1 Mux", "ADC1/2", "ADC 1_2" },
	{ "Stereo2 ADC1 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo2 ADC2 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo2 ADC2 Mux", "DMIC", "Stereo2 DMIC Mux" },
	{ "Stereo2 ADC2 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo3 ADC1 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo3 ADC1 Mux", "ADC1/2", "ADC 1_2" },
	{ "Stereo3 ADC1 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo3 ADC2 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo3 ADC2 Mux", "DMIC", "Stereo3 DMIC Mux" },
	{ "Stereo3 ADC2 Mux", "Stereo DAC MIX", "Stereo DAC MIX" },

	{ "Stereo4 ADC1 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo4 ADC1 Mux", "ADC1/2", "ADC 1_2" },
	{ "Stereo4 ADC1 Mux", "DD MIX2", "DD2 MIX" },

	{ "Stereo4 ADC2 Mux", "DD MIX1", "DD1 MIX" },
	{ "Stereo4 ADC2 Mux", "DMIC", "Stereo3 DMIC Mux" },
	{ "Stereo4 ADC2 Mux", "DD MIX2", "DD2 MIX" },

	{ "Mono ADC2 L Mux", "DD MIX1L", "DD1 MIXL" },
	{ "Mono ADC2 L Mux", "DMIC", "Mono DMIC L Mux" },
	{ "Mono ADC2 L Mux", "MONO DAC MIXL", "Mono DAC MIXL" },

	{ "Mono ADC1 L Mux", "DD MIX1L", "DD1 MIXL" },
	{ "Mono ADC1 L Mux", "ADC1", "ADC 1" },
	{ "Mono ADC1 L Mux", "MONO DAC MIXL", "Mono DAC MIXL" },

	{ "Mono ADC1 R Mux", "DD MIX1R", "DD1 MIXR" },
	{ "Mono ADC1 R Mux", "ADC2", "ADC 2" },
	{ "Mono ADC1 R Mux", "MONO DAC MIXR", "Mono DAC MIXR" },

	{ "Mono ADC2 R Mux", "DD MIX1R", "DD1 MIXR" },
	{ "Mono ADC2 R Mux", "DMIC", "Mono DMIC R Mux" },
	{ "Mono ADC2 R Mux", "MONO DAC MIXR", "Mono DAC MIXR" },

	{ "Sto1 ADC MIXL", "ADC1 Switch", "Stereo1 ADC1 Mux" },
	{ "Sto1 ADC MIXL", "ADC2 Switch", "Stereo1 ADC2 Mux" },
	{ "Sto1 ADC MIXR", "ADC1 Switch", "Stereo1 ADC1 Mux" },
	{ "Sto1 ADC MIXR", "ADC2 Switch", "Stereo1 ADC2 Mux" },

	{ "Stereo1 ADC MIXL", NULL, "Sto1 ADC MIXL" },
	{ "Stereo1 ADC MIXL", NULL, "adc stereo1 filter" },
	{ "adc stereo1 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo1 ADC MIXR", NULL, "Sto1 ADC MIXR" },
	{ "Stereo1 ADC MIXR", NULL, "adc stereo1 filter" },
	{ "adc stereo1 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo1 ADC MIX", NULL, "Stereo1 ADC MIXL" },
	{ "Stereo1 ADC MIX", NULL, "Stereo1 ADC MIXR" },

	{ "Sto2 ADC MIXL", "ADC1 Switch", "Stereo2 ADC1 Mux" },
	{ "Sto2 ADC MIXL", "ADC2 Switch", "Stereo2 ADC2 Mux" },
	{ "Sto2 ADC MIXR", "ADC1 Switch", "Stereo2 ADC1 Mux" },
	{ "Sto2 ADC MIXR", "ADC2 Switch", "Stereo2 ADC2 Mux" },

	{ "Sto2 ADC LR MIX", NULL, "Sto2 ADC MIXL" },
	{ "Sto2 ADC LR MIX", NULL, "Sto2 ADC MIXR" },

	{ "Stereo2 ADC LR Mux", "L", "Sto2 ADC MIXL" },
	{ "Stereo2 ADC LR Mux", "LR", "Sto2 ADC LR MIX" },

	{ "Stereo2 ADC MIXL", NULL, "Stereo2 ADC LR Mux" },
	{ "Stereo2 ADC MIXL", NULL, "adc stereo2 filter" },
	{ "adc stereo2 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo2 ADC MIXR", NULL, "Sto2 ADC MIXR" },
	{ "Stereo2 ADC MIXR", NULL, "adc stereo2 filter" },
	{ "adc stereo2 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo2 ADC MIX", NULL, "Stereo2 ADC MIXL" },
	{ "Stereo2 ADC MIX", NULL, "Stereo2 ADC MIXR" },

	{ "Sto3 ADC MIXL", "ADC1 Switch", "Stereo3 ADC1 Mux" },
	{ "Sto3 ADC MIXL", "ADC2 Switch", "Stereo3 ADC2 Mux" },
	{ "Sto3 ADC MIXR", "ADC1 Switch", "Stereo3 ADC1 Mux" },
	{ "Sto3 ADC MIXR", "ADC2 Switch", "Stereo3 ADC2 Mux" },

	{ "Stereo3 ADC MIXL", NULL, "Sto3 ADC MIXL" },
	{ "Stereo3 ADC MIXL", NULL, "adc stereo3 filter" },
	{ "adc stereo3 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo3 ADC MIXR", NULL, "Sto3 ADC MIXR" },
	{ "Stereo3 ADC MIXR", NULL, "adc stereo3 filter" },
	{ "adc stereo3 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo3 ADC MIX", NULL, "Stereo3 ADC MIXL" },
	{ "Stereo3 ADC MIX", NULL, "Stereo3 ADC MIXR" },

	{ "Sto4 ADC MIXL", "ADC1 Switch", "Stereo4 ADC1 Mux" },
	{ "Sto4 ADC MIXL", "ADC2 Switch", "Stereo4 ADC2 Mux" },
	{ "Sto4 ADC MIXR", "ADC1 Switch", "Stereo4 ADC1 Mux" },
	{ "Sto4 ADC MIXR", "ADC2 Switch", "Stereo4 ADC2 Mux" },

	{ "Stereo4 ADC MIXL", NULL, "Sto4 ADC MIXL" },
	{ "Stereo4 ADC MIXL", NULL, "adc stereo4 filter" },
	{ "adc stereo4 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo4 ADC MIXR", NULL, "Sto4 ADC MIXR" },
	{ "Stereo4 ADC MIXR", NULL, "adc stereo4 filter" },
	{ "adc stereo4 filter", NULL, "PLL1", check_sysclk1_source },

	{ "Stereo4 ADC MIX", NULL, "Stereo4 ADC MIXL" },
	{ "Stereo4 ADC MIX", NULL, "Stereo4 ADC MIXR" },

	{ "Mono ADC MIXL", "ADC1 Switch", "Mono ADC1 L Mux" },
	{ "Mono ADC MIXL", "ADC2 Switch", "Mono ADC2 L Mux" },
	{ "Mono ADC MIXL", NULL, "adc mono left filter" },
	{ "adc mono left filter", NULL, "PLL1", check_sysclk1_source },

	{ "Mono ADC MIXR", "ADC1 Switch", "Mono ADC1 R Mux" },
	{ "Mono ADC MIXR", "ADC2 Switch", "Mono ADC2 R Mux" },
	{ "Mono ADC MIXR", NULL, "adc mono right filter" },
	{ "adc mono right filter", NULL, "PLL1", check_sysclk1_source },

	{ "Mono ADC MIXL ADC", NULL, "Mono ADC MIXL" },
	{ "Mono ADC MIXR ADC", NULL, "Mono ADC MIXR" },

	{ "Mono ADC MIX", NULL, "Mono ADC MIXL ADC" },
	{ "Mono ADC MIX", NULL, "Mono ADC MIXR ADC" },

	{ "VAD ADC Mux", "STO1 ADC MIX L", "Stereo1 ADC MIXL" },
	{ "VAD ADC Mux", "MONO ADC MIX L", "Mono ADC MIXL ADC" },
	{ "VAD ADC Mux", "MONO ADC MIX R", "Mono ADC MIXR ADC" },
	{ "VAD ADC Mux", "STO2 ADC MIX L", "Stereo2 ADC MIXL" },
	{ "VAD ADC Mux", "STO3 ADC MIX L", "Stereo3 ADC MIXL" },

	{ "IF1 ADC1 Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "IF1 ADC1 Mux", "OB01", "OB01 Bypass Mux" },
	{ "IF1 ADC1 Mux", "VAD ADC", "VAD ADC Mux" },

	{ "IF1 ADC2 Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "IF1 ADC2 Mux", "OB23", "OB23 Bypass Mux" },

	{ "IF1 ADC3 Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "IF1 ADC3 Mux", "MONO ADC MIX", "Mono ADC MIX" },
	{ "IF1 ADC3 Mux", "OB45", "OB45" },

	{ "IF1 ADC4 Mux", "STO4 ADC MIX", "Stereo4 ADC MIX" },
	{ "IF1 ADC4 Mux", "OB67", "OB67" },
	{ "IF1 ADC4 Mux", "OB01", "OB01 Bypass Mux" },

	{ "AIF1TX", NULL, "I2S1" },
	{ "AIF1TX", NULL, "IF1 ADC1 Mux" },
	{ "AIF1TX", NULL, "IF1 ADC2 Mux" },
	{ "AIF1TX", NULL, "IF1 ADC3 Mux" },
	{ "AIF1TX", NULL, "IF1 ADC4 Mux" },

	{ "IF2 ADC1 Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "IF2 ADC1 Mux", "OB01", "OB01 Bypass Mux" },
	{ "IF2 ADC1 Mux", "VAD ADC", "VAD ADC Mux" },

	{ "IF2 ADC2 Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "IF2 ADC2 Mux", "OB23", "OB23 Bypass Mux" },

	{ "IF2 ADC3 Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "IF2 ADC3 Mux", "MONO ADC MIX", "Mono ADC MIX" },
	{ "IF2 ADC3 Mux", "OB45", "OB45" },

	{ "IF2 ADC4 Mux", "STO4 ADC MIX", "Stereo4 ADC MIX" },
	{ "IF2 ADC4 Mux", "OB67", "OB67" },
	{ "IF2 ADC4 Mux", "OB01", "OB01 Bypass Mux" },

	{ "AIF2TX", NULL, "I2S2" },
	{ "AIF2TX", NULL, "IF2 ADC1 Mux" },
	{ "AIF2TX", NULL, "IF2 ADC2 Mux" },
	{ "AIF2TX", NULL, "IF2 ADC3 Mux" },
	{ "AIF2TX", NULL, "IF2 ADC4 Mux" },

	{ "IF3 ADC Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "IF3 ADC Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "IF3 ADC Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "IF3 ADC Mux", "STO4 ADC MIX", "Stereo4 ADC MIX" },
	{ "IF3 ADC Mux", "MONO ADC MIX", "Mono ADC MIX" },
	{ "IF3 ADC Mux", "OB01", "OB01 Bypass Mux" },
	{ "IF3 ADC Mux", "OB23", "OB23 Bypass Mux" },
	{ "IF3 ADC Mux", "VAD ADC", "VAD ADC Mux" },

	{ "AIF3TX", NULL, "I2S3" },
	{ "AIF3TX", NULL, "IF3 ADC Mux" },

	{ "IF4 ADC Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "IF4 ADC Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "IF4 ADC Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "IF4 ADC Mux", "STO4 ADC MIX", "Stereo4 ADC MIX" },
	{ "IF4 ADC Mux", "MONO ADC MIX", "Mono ADC MIX" },
	{ "IF4 ADC Mux", "OB01", "OB01 Bypass Mux" },
	{ "IF4 ADC Mux", "OB23", "OB23 Bypass Mux" },
	{ "IF4 ADC Mux", "VAD ADC", "VAD ADC Mux" },

	{ "AIF4TX", NULL, "I2S4" },
	{ "AIF4TX", NULL, "IF4 ADC Mux" },

	{ "SLB ADC1 Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "SLB ADC1 Mux", "OB01", "OB01 Bypass Mux" },
	{ "SLB ADC1 Mux", "VAD ADC", "VAD ADC Mux" },

	{ "SLB ADC2 Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "SLB ADC2 Mux", "OB23", "OB23 Bypass Mux" },

	{ "SLB ADC3 Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "SLB ADC3 Mux", "MONO ADC MIX", "Mono ADC MIX" },
	{ "SLB ADC3 Mux", "OB45", "OB45" },

	{ "SLB ADC4 Mux", "STO4 ADC MIX", "Stereo4 ADC MIX" },
	{ "SLB ADC4 Mux", "OB67", "OB67" },
	{ "SLB ADC4 Mux", "OB01", "OB01 Bypass Mux" },

	{ "SLBTX", NULL, "SLB" },
	{ "SLBTX", NULL, "SLB ADC1 Mux" },
	{ "SLBTX", NULL, "SLB ADC2 Mux" },
	{ "SLBTX", NULL, "SLB ADC3 Mux" },
	{ "SLBTX", NULL, "SLB ADC4 Mux" },

	{ "IB01 Mux", "IF1 DAC 01", "IF1 DAC01" },
	{ "IB01 Mux", "IF2 DAC 01", "IF2 DAC01" },
	{ "IB01 Mux", "SLB DAC 01", "SLB DAC01" },
	{ "IB01 Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "IB01 Mux", "VAD ADC/DAC1 FS", "DAC1 FS" },

	{ "IB01 Bypass Mux", "Bypass", "IB01 Mux" },
	{ "IB01 Bypass Mux", "Pass SRC", "IB01 Mux" },

	{ "IB23 Mux", "IF1 DAC 23", "IF1 DAC23" },
	{ "IB23 Mux", "IF2 DAC 23", "IF2 DAC23" },
	{ "IB23 Mux", "SLB DAC 23", "SLB DAC23" },
	{ "IB23 Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "IB23 Mux", "DAC1 FS", "DAC1 FS" },
	{ "IB23 Mux", "IF4 DAC", "IF4 DAC" },

	{ "IB23 Bypass Mux", "Bypass", "IB23 Mux" },
	{ "IB23 Bypass Mux", "Pass SRC", "IB23 Mux" },

	{ "IB45 Mux", "IF1 DAC 45", "IF1 DAC45" },
	{ "IB45 Mux", "IF2 DAC 45", "IF2 DAC45" },
	{ "IB45 Mux", "SLB DAC 45", "SLB DAC45" },
	{ "IB45 Mux", "STO3 ADC MIX", "Stereo3 ADC MIX" },
	{ "IB45 Mux", "IF3 DAC", "IF3 DAC" },

	{ "IB45 Bypass Mux", "Bypass", "IB45 Mux" },
	{ "IB45 Bypass Mux", "Pass SRC", "IB45 Mux" },

	{ "IB6 Mux", "IF1 DAC 6", "IF1 DAC6" },
	{ "IB6 Mux", "IF2 DAC 6", "IF2 DAC6" },
	{ "IB6 Mux", "SLB DAC 6", "SLB DAC6" },
	{ "IB6 Mux", "STO4 ADC MIX L", "Stereo4 ADC MIXL" },
	{ "IB6 Mux", "IF4 DAC L", "IF4 DAC L" },
	{ "IB6 Mux", "STO1 ADC MIX L", "Stereo1 ADC MIXL" },
	{ "IB6 Mux", "STO2 ADC MIX L", "Stereo2 ADC MIXL" },
	{ "IB6 Mux", "STO3 ADC MIX L", "Stereo3 ADC MIXL" },

	{ "IB7 Mux", "IF1 DAC 7", "IF1 DAC7" },
	{ "IB7 Mux", "IF2 DAC 7", "IF2 DAC7" },
	{ "IB7 Mux", "SLB DAC 7", "SLB DAC7" },
	{ "IB7 Mux", "STO4 ADC MIX R", "Stereo4 ADC MIXR" },
	{ "IB7 Mux", "IF4 DAC R", "IF4 DAC R" },
	{ "IB7 Mux", "STO1 ADC MIX R", "Stereo1 ADC MIXR" },
	{ "IB7 Mux", "STO2 ADC MIX R", "Stereo2 ADC MIXR" },
	{ "IB7 Mux", "STO3 ADC MIX R", "Stereo3 ADC MIXR" },

	{ "IB8 Mux", "STO1 ADC MIX L", "Stereo1 ADC MIXL" },
	{ "IB8 Mux", "STO2 ADC MIX L", "Stereo2 ADC MIXL" },
	{ "IB8 Mux", "STO3 ADC MIX L", "Stereo3 ADC MIXL" },
	{ "IB8 Mux", "STO4 ADC MIX L", "Stereo4 ADC MIXL" },
	{ "IB8 Mux", "MONO ADC MIX L", "Mono ADC MIXL ADC" },
	{ "IB8 Mux", "DACL1 FS", "DAC1 MIXL" },

	{ "IB9 Mux", "STO1 ADC MIX R", "Stereo1 ADC MIXR" },
	{ "IB9 Mux", "STO2 ADC MIX R", "Stereo2 ADC MIXR" },
	{ "IB9 Mux", "STO3 ADC MIX R", "Stereo3 ADC MIXR" },
	{ "IB9 Mux", "STO4 ADC MIX R", "Stereo4 ADC MIXR" },
	{ "IB9 Mux", "MONO ADC MIX R", "Mono ADC MIXR ADC" },
	{ "IB9 Mux", "DACR1 FS", "DAC1 MIXR" },
	{ "IB9 Mux", "DAC1 FS", "DAC1 FS" },

	{ "OB01 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB01 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB01 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB01 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB01 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB01 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB01 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB23 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB23 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB23 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB23 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB23 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB23 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB23 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB4 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB4 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB4 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB4 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB4 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB4 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB4 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB5 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB5 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB5 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB5 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB5 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB5 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB5 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB6 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB6 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB6 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB6 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB6 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB6 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB6 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB7 MIX", "IB01 Switch", "IB01 Bypass Mux" },
	{ "OB7 MIX", "IB23 Switch", "IB23 Bypass Mux" },
	{ "OB7 MIX", "IB45 Switch", "IB45 Bypass Mux" },
	{ "OB7 MIX", "IB6 Switch", "IB6 Mux" },
	{ "OB7 MIX", "IB7 Switch", "IB7 Mux" },
	{ "OB7 MIX", "IB8 Switch", "IB8 Mux" },
	{ "OB7 MIX", "IB9 Switch", "IB9 Mux" },

	{ "OB01 Bypass Mux", "Bypass", "OB01 MIX" },
	{ "OB01 Bypass Mux", "Pass SRC", "OB01 MIX" },
	{ "OB23 Bypass Mux", "Bypass", "OB23 MIX" },
	{ "OB23 Bypass Mux", "Pass SRC", "OB23 MIX" },

	{ "OutBound2", NULL, "OB23 Bypass Mux" },
	{ "OutBound3", NULL, "OB23 Bypass Mux" },
	{ "OutBound4", NULL, "OB4 MIX" },
	{ "OutBound5", NULL, "OB5 MIX" },
	{ "OutBound6", NULL, "OB6 MIX" },
	{ "OutBound7", NULL, "OB7 MIX" },

	{ "OB45", NULL, "OutBound4" },
	{ "OB45", NULL, "OutBound5" },
	{ "OB67", NULL, "OutBound6" },
	{ "OB67", NULL, "OutBound7" },

	{ "IF1 DAC0", NULL, "AIF1RX" },
	{ "IF1 DAC1", NULL, "AIF1RX" },
	{ "IF1 DAC2", NULL, "AIF1RX" },
	{ "IF1 DAC3", NULL, "AIF1RX" },
	{ "IF1 DAC4", NULL, "AIF1RX" },
	{ "IF1 DAC5", NULL, "AIF1RX" },
	{ "IF1 DAC6", NULL, "AIF1RX" },
	{ "IF1 DAC7", NULL, "AIF1RX" },
	{ "IF1 DAC0", NULL, "I2S1" },
	{ "IF1 DAC1", NULL, "I2S1" },
	{ "IF1 DAC2", NULL, "I2S1" },
	{ "IF1 DAC3", NULL, "I2S1" },
	{ "IF1 DAC4", NULL, "I2S1" },
	{ "IF1 DAC5", NULL, "I2S1" },
	{ "IF1 DAC6", NULL, "I2S1" },
	{ "IF1 DAC7", NULL, "I2S1" },

	{ "IF1 DAC01", NULL, "IF1 DAC0" },
	{ "IF1 DAC01", NULL, "IF1 DAC1" },
	{ "IF1 DAC23", NULL, "IF1 DAC2" },
	{ "IF1 DAC23", NULL, "IF1 DAC3" },
	{ "IF1 DAC45", NULL, "IF1 DAC4" },
	{ "IF1 DAC45", NULL, "IF1 DAC5" },
	{ "IF1 DAC67", NULL, "IF1 DAC6" },
	{ "IF1 DAC67", NULL, "IF1 DAC7" },

	{ "IF2 DAC0", NULL, "AIF2RX" },
	{ "IF2 DAC1", NULL, "AIF2RX" },
	{ "IF2 DAC2", NULL, "AIF2RX" },
	{ "IF2 DAC3", NULL, "AIF2RX" },
	{ "IF2 DAC4", NULL, "AIF2RX" },
	{ "IF2 DAC5", NULL, "AIF2RX" },
	{ "IF2 DAC6", NULL, "AIF2RX" },
	{ "IF2 DAC7", NULL, "AIF2RX" },
	{ "IF2 DAC0", NULL, "I2S2" },
	{ "IF2 DAC1", NULL, "I2S2" },
	{ "IF2 DAC2", NULL, "I2S2" },
	{ "IF2 DAC3", NULL, "I2S2" },
	{ "IF2 DAC4", NULL, "I2S2" },
	{ "IF2 DAC5", NULL, "I2S2" },
	{ "IF2 DAC6", NULL, "I2S2" },
	{ "IF2 DAC7", NULL, "I2S2" },

	{ "IF2 DAC01", NULL, "IF2 DAC0" },
	{ "IF2 DAC01", NULL, "IF2 DAC1" },
	{ "IF2 DAC23", NULL, "IF2 DAC2" },
	{ "IF2 DAC23", NULL, "IF2 DAC3" },
	{ "IF2 DAC45", NULL, "IF2 DAC4" },
	{ "IF2 DAC45", NULL, "IF2 DAC5" },
	{ "IF2 DAC67", NULL, "IF2 DAC6" },
	{ "IF2 DAC67", NULL, "IF2 DAC7" },

	{ "IF3 DAC", NULL, "AIF3RX" },
	{ "IF3 DAC", NULL, "I2S3" },

	{ "IF4 DAC", NULL, "AIF4RX" },
	{ "IF4 DAC", NULL, "I2S4" },

	{ "IF3 DAC L", NULL, "IF3 DAC" },
	{ "IF3 DAC R", NULL, "IF3 DAC" },

	{ "IF4 DAC L", NULL, "IF4 DAC" },
	{ "IF4 DAC R", NULL, "IF4 DAC" },

	{ "SLB DAC0", NULL, "SLBRX" },
	{ "SLB DAC1", NULL, "SLBRX" },
	{ "SLB DAC2", NULL, "SLBRX" },
	{ "SLB DAC3", NULL, "SLBRX" },
	{ "SLB DAC4", NULL, "SLBRX" },
	{ "SLB DAC5", NULL, "SLBRX" },
	{ "SLB DAC6", NULL, "SLBRX" },
	{ "SLB DAC7", NULL, "SLBRX" },
	{ "SLB DAC0", NULL, "SLB" },
	{ "SLB DAC1", NULL, "SLB" },
	{ "SLB DAC2", NULL, "SLB" },
	{ "SLB DAC3", NULL, "SLB" },
	{ "SLB DAC4", NULL, "SLB" },
	{ "SLB DAC5", NULL, "SLB" },
	{ "SLB DAC6", NULL, "SLB" },
	{ "SLB DAC7", NULL, "SLB" },

	{ "SLB DAC01", NULL, "SLB DAC0" },
	{ "SLB DAC01", NULL, "SLB DAC1" },
	{ "SLB DAC23", NULL, "SLB DAC2" },
	{ "SLB DAC23", NULL, "SLB DAC3" },
	{ "SLB DAC45", NULL, "SLB DAC4" },
	{ "SLB DAC45", NULL, "SLB DAC5" },
	{ "SLB DAC67", NULL, "SLB DAC6" },
	{ "SLB DAC67", NULL, "SLB DAC7" },

	{ "ADDA1 Mux", "STO1 ADC MIX", "Stereo1 ADC MIX" },
	{ "ADDA1 Mux", "STO2 ADC MIX", "Stereo2 ADC MIX" },
	{ "ADDA1 Mux", "OB 67", "OB67" },

	{ "DAC1 Mux", "IF1 DAC 01", "IF1 DAC01" },
	{ "DAC1 Mux", "IF2 DAC 01", "IF2 DAC01" },
	{ "DAC1 Mux", "IF3 DAC LR", "IF3 DAC" },
	{ "DAC1 Mux", "IF4 DAC LR", "IF4 DAC" },
	{ "DAC1 Mux", "SLB DAC 01", "SLB DAC01" },
	{ "DAC1 Mux", "OB 01", "OB01 Bypass Mux" },

	{ "DAC1 MIXL", "Stereo ADC Switch", "ADDA1 Mux" },
	{ "DAC1 MIXL", "DAC1 Switch", "DAC1 Mux" },
	{ "DAC1 MIXL", NULL, "dac stereo1 filter" },
	{ "DAC1 MIXR", "Stereo ADC Switch", "ADDA1 Mux" },
	{ "DAC1 MIXR", "DAC1 Switch", "DAC1 Mux" },
	{ "DAC1 MIXR", NULL, "dac stereo1 filter" },

	{ "DAC1 FS", NULL, "DAC1 MIXL" },
	{ "DAC1 FS", NULL, "DAC1 MIXR" },

	{ "DAC2 L Mux", "IF1 DAC 2", "IF1 DAC2" },
	{ "DAC2 L Mux", "IF2 DAC 2", "IF2 DAC2" },
	{ "DAC2 L Mux", "IF3 DAC L", "IF3 DAC L" },
	{ "DAC2 L Mux", "IF4 DAC L", "IF4 DAC L" },
	{ "DAC2 L Mux", "SLB DAC 2", "SLB DAC2" },
	{ "DAC2 L Mux", "OB 2", "OutBound2" },

	{ "DAC2 R Mux", "IF1 DAC 3", "IF1 DAC3" },
	{ "DAC2 R Mux", "IF2 DAC 3", "IF2 DAC3" },
	{ "DAC2 R Mux", "IF3 DAC R", "IF3 DAC R" },
	{ "DAC2 R Mux", "IF4 DAC R", "IF4 DAC R" },
	{ "DAC2 R Mux", "SLB DAC 3", "SLB DAC3" },
	{ "DAC2 R Mux", "OB 3", "OutBound3" },
	{ "DAC2 R Mux", "Haptic Generator", "Haptic Generator" },
	{ "DAC2 R Mux", "VAD ADC", "VAD ADC Mux" },

	{ "DAC3 L Mux", "IF1 DAC 4", "IF1 DAC4" },
	{ "DAC3 L Mux", "IF2 DAC 4", "IF2 DAC4" },
	{ "DAC3 L Mux", "IF3 DAC L", "IF3 DAC L" },
	{ "DAC3 L Mux", "IF4 DAC L", "IF4 DAC L" },
	{ "DAC3 L Mux", "SLB DAC 4", "SLB DAC4" },
	{ "DAC3 L Mux", "OB 4", "OutBound4" },

	{ "DAC3 R Mux", "IF1 DAC 5", "IF1 DAC4" },
	{ "DAC3 R Mux", "IF2 DAC 5", "IF2 DAC4" },
	{ "DAC3 R Mux", "IF3 DAC R", "IF3 DAC R" },
	{ "DAC3 R Mux", "IF4 DAC R", "IF4 DAC R" },
	{ "DAC3 R Mux", "SLB DAC 5", "SLB DAC5" },
	{ "DAC3 R Mux", "OB 5", "OutBound5" },

	{ "DAC4 L Mux", "IF1 DAC 6", "IF1 DAC6" },
	{ "DAC4 L Mux", "IF2 DAC 6", "IF2 DAC6" },
	{ "DAC4 L Mux", "IF3 DAC L", "IF3 DAC L" },
	{ "DAC4 L Mux", "IF4 DAC L", "IF4 DAC L" },
	{ "DAC4 L Mux", "SLB DAC 6", "SLB DAC6" },
	{ "DAC4 L Mux", "OB 6", "OutBound6" },

	{ "DAC4 R Mux", "IF1 DAC 7", "IF1 DAC7" },
	{ "DAC4 R Mux", "IF2 DAC 7", "IF2 DAC7" },
	{ "DAC4 R Mux", "IF3 DAC R", "IF3 DAC R" },
	{ "DAC4 R Mux", "IF4 DAC R", "IF4 DAC R" },
	{ "DAC4 R Mux", "SLB DAC 7", "SLB DAC7" },
	{ "DAC4 R Mux", "OB 7", "OutBound7" },

	{ "Sidetone Mux", "DMIC1 L", "DMIC L1" },
	{ "Sidetone Mux", "DMIC2 L", "DMIC L2" },
	{ "Sidetone Mux", "DMIC3 L", "DMIC L3" },
	{ "Sidetone Mux", "DMIC4 L", "DMIC L4" },
	{ "Sidetone Mux", "ADC1", "ADC 1" },
	{ "Sidetone Mux", "ADC2", "ADC 2" },

	{ "Stereo DAC MIXL", "ST L Switch", "Sidetone Mux" },
	{ "Stereo DAC MIXL", "DAC1 L Switch", "DAC1 MIXL" },
	{ "Stereo DAC MIXL", "DAC2 L Switch", "DAC2 L Mux" },
	{ "Stereo DAC MIXL", "DAC1 R Switch", "DAC1 MIXR" },
	{ "Stereo DAC MIXL", NULL, "dac stereo1 filter" },
	{ "Stereo DAC MIXR", "ST R Switch", "Sidetone Mux" },
	{ "Stereo DAC MIXR", "DAC1 R Switch", "DAC1 MIXR" },
	{ "Stereo DAC MIXR", "DAC2 R Switch", "DAC2 R Mux" },
	{ "Stereo DAC MIXR", "DAC1 L Switch", "DAC1 MIXL" },
	{ "Stereo DAC MIXR", NULL, "dac stereo1 filter" },

	{ "Mono DAC MIXL", "ST L Switch", "Sidetone Mux" },
	{ "Mono DAC MIXL", "DAC1 L Switch", "DAC1 MIXL" },
	{ "Mono DAC MIXL", "DAC2 L Switch", "DAC2 L Mux" },
	{ "Mono DAC MIXL", "DAC2 R Switch", "DAC2 R Mux" },
	{ "Mono DAC MIXL", NULL, "dac mono left filter" },
	{ "Mono DAC MIXR", "ST R Switch", "Sidetone Mux" },
	{ "Mono DAC MIXR", "DAC1 R Switch", "DAC1 MIXR" },
	{ "Mono DAC MIXR", "DAC2 R Switch", "DAC2 R Mux" },
	{ "Mono DAC MIXR", "DAC2 L Switch", "DAC2 L Mux" },
	{ "Mono DAC MIXR", NULL, "dac mono right filter" },

	{ "DD1 MIXL", "Sto DAC Mix L Switch", "Stereo DAC MIXL" },
	{ "DD1 MIXL", "Mono DAC Mix L Switch", "Mono DAC MIXL" },
	{ "DD1 MIXL", "DAC3 L Switch", "DAC3 L Mux" },
	{ "DD1 MIXL", "DAC3 R Switch", "DAC3 R Mux" },
	{ "DD1 MIXR", "Sto DAC Mix R Switch", "Stereo DAC MIXR" },
	{ "DD1 MIXR", "Mono DAC Mix R Switch", "Mono DAC MIXR" },
	{ "DD1 MIXR", "DAC3 L Switch", "DAC3 L Mux" },
	{ "DD1 MIXR", "DAC3 R Switch", "DAC3 R Mux" },

	{ "DD2 MIXL", "Sto DAC Mix L Switch", "Stereo DAC MIXL" },
	{ "DD2 MIXL", "Mono DAC Mix L Switch", "Mono DAC MIXL" },
	{ "DD2 MIXL", "DAC4 L Switch", "DAC4 L Mux" },
	{ "DD2 MIXL", "DAC4 R Switch", "DAC4 R Mux" },
	{ "DD2 MIXR", "Sto DAC Mix R Switch", "Stereo DAC MIXR" },
	{ "DD2 MIXR", "Mono DAC Mix R Switch", "Mono DAC MIXR" },
	{ "DD2 MIXR", "DAC4 L Switch", "DAC4 L Mux" },
	{ "DD2 MIXR", "DAC4 R Switch", "DAC4 R Mux" },

	{ "Stereo DAC MIX", NULL, "Stereo DAC MIXL" },
	{ "Stereo DAC MIX", NULL, "Stereo DAC MIXR" },
	{ "Mono DAC MIX", NULL, "Mono DAC MIXL" },
	{ "Mono DAC MIX", NULL, "Mono DAC MIXR" },
	{ "DD1 MIX", NULL, "DD1 MIXL" },
	{ "DD1 MIX", NULL, "DD1 MIXR" },
	{ "DD2 MIX", NULL, "DD2 MIXL" },
	{ "DD2 MIX", NULL, "DD2 MIXR" },

	{ "DAC12 SRC Mux", "STO1 DAC MIX", "Stereo DAC MIX" },
	{ "DAC12 SRC Mux", "MONO DAC MIX", "Mono DAC MIX" },
	{ "DAC12 SRC Mux", "DD MIX1", "DD1 MIX" },
	{ "DAC12 SRC Mux", "DD MIX2", "DD2 MIX" },

	{ "DAC3 SRC Mux", "MONO DAC MIXL", "Mono DAC MIXL" },
	{ "DAC3 SRC Mux", "MONO DAC MIXR", "Mono DAC MIXR" },
	{ "DAC3 SRC Mux", "DD MIX1L", "DD1 MIXL" },
	{ "DAC3 SRC Mux", "DD MIX2L", "DD2 MIXL" },

	{ "DAC 1", NULL, "DAC12 SRC Mux" },
	{ "DAC 1", NULL, "PLL1", check_sysclk1_source },
	{ "DAC 2", NULL, "DAC12 SRC Mux" },
	{ "DAC 2", NULL, "PLL1", check_sysclk1_source },
	{ "DAC 3", NULL, "DAC3 SRC Mux" },
	{ "DAC 3", NULL, "PLL1", check_sysclk1_source },

	{ "PDM1 L Mux", "STO1 DAC MIX", "Stereo DAC MIXL" },
	{ "PDM1 L Mux", "MONO DAC MIX", "Mono DAC MIXL" },
	{ "PDM1 L Mux", "DD MIX1", "DD1 MIXL" },
	{ "PDM1 L Mux", "DD MIX2", "DD2 MIXL" },
	{ "PDM1 L Mux", NULL, "PDM1 Power" },
	{ "PDM1 R Mux", "STO1 DAC MIX", "Stereo DAC MIXR" },
	{ "PDM1 R Mux", "MONO DAC MIX", "Mono DAC MIXR" },
	{ "PDM1 R Mux", "DD MIX1", "DD1 MIXR" },
	{ "PDM1 R Mux", "DD MIX2", "DD2 MIXR" },
	{ "PDM1 R Mux", NULL, "PDM1 Power" },
	{ "PDM2 L Mux", "STO1 DAC MIX", "Stereo DAC MIXL" },
	{ "PDM2 L Mux", "MONO DAC MIX", "Mono DAC MIXL" },
	{ "PDM2 L Mux", "DD MIX1", "DD1 MIXL" },
	{ "PDM2 L Mux", "DD MIX2", "DD2 MIXL" },
	{ "PDM2 L Mux", NULL, "PDM2 Power" },
	{ "PDM2 R Mux", "STO1 DAC MIX", "Stereo DAC MIXR" },
	{ "PDM2 R Mux", "MONO DAC MIX", "Mono DAC MIXR" },
	{ "PDM2 R Mux", "DD MIX1", "DD1 MIXR" },
	{ "PDM2 R Mux", "DD MIX1", "DD2 MIXR" },
	{ "PDM2 R Mux", NULL, "PDM2 Power" },

	{ "LOUT1 amp", NULL, "DAC 1" },
	{ "LOUT2 amp", NULL, "DAC 2" },
	{ "LOUT3 amp", NULL, "DAC 3" },

	{ "LOUT1", NULL, "LOUT1 amp" },
	{ "LOUT2", NULL, "LOUT2 amp" },
	{ "LOUT3", NULL, "LOUT3 amp" },

	{ "PDM1L", NULL, "PDM1 L Mux" },
	{ "PDM1R", NULL, "PDM1 R Mux" },
	{ "PDM2L", NULL, "PDM2 L Mux" },
	{ "PDM2R", NULL, "PDM2 R Mux" },
};

static int get_clk_info(int sclk, int rate)
{
	int i, pd[] = {1, 2, 3, 4, 6, 8, 12, 16};

#ifdef USE_ASRC
	return 0;
#endif
	if (sclk <= 0 || rate <= 0)
		return -EINVAL;

	rate = rate << 8;
	for (i = 0; i < ARRAY_SIZE(pd); i++)
		if (sclk == rate * pd[i])
			return i;

	return -EINVAL;
}

static int rt5677_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int val_len = 0, val_clk, mask_clk;
	int pre_div, bclk_ms, frame_size;

	rt5677->lrck[dai->id] = params_rate(params);
	pre_div = get_clk_info(rt5677->sysclk, rt5677->lrck[dai->id]);
	if (pre_div < 0) {
		dev_err(codec->dev, "Unsupported clock setting\n");
		return -EINVAL;
	}
	frame_size = snd_soc_params_to_frame_size(params);
	if (frame_size < 0) {
		dev_err(codec->dev, "Unsupported frame size: %d\n", frame_size);
		return -EINVAL;
	}
	bclk_ms = frame_size > 32 ? 1 : 0;
	rt5677->bclk[dai->id] = rt5677->lrck[dai->id] * (32 << bclk_ms);

	dev_dbg(dai->dev, "bclk is %dHz and lrck is %dHz\n",
		rt5677->bclk[dai->id], rt5677->lrck[dai->id]);
	dev_dbg(dai->dev, "bclk_ms is %d and pre_div is %d for iis %d\n",
				bclk_ms, pre_div, dai->id);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		val_len |= RT5677_I2S_DL_20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		val_len |= RT5677_I2S_DL_24;
		break;
	case SNDRV_PCM_FORMAT_S8:
		val_len |= RT5677_I2S_DL_8;
		break;
	default:
		return -EINVAL;
	}

	switch (dai->id) {
	case RT5677_AIF1:
		mask_clk = RT5677_I2S_PD1_MASK;
		val_clk = pre_div << RT5677_I2S_PD1_SFT;
		regmap_update_bits(rt5677->regmap, RT5677_I2S1_SDP,
			RT5677_I2S_DL_MASK, val_len);
		regmap_update_bits(rt5677->regmap, RT5677_CLK_TREE_CTRL1,
			mask_clk, val_clk);
		break;
	case RT5677_AIF2:
		mask_clk = RT5677_I2S_BCLK_MS2_MASK | RT5677_I2S_PD2_MASK;
		val_clk = bclk_ms << RT5677_I2S_BCLK_MS2_SFT |
			pre_div << RT5677_I2S_PD2_SFT;
		regmap_update_bits(rt5677->regmap, RT5677_I2S2_SDP,
			RT5677_I2S_DL_MASK, val_len);
		regmap_update_bits(rt5677->regmap, RT5677_CLK_TREE_CTRL1,
			mask_clk, val_clk);
		break;
	default:
		break;
	}


	return 0;
}

static int rt5677_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	rt5677->aif_pu = dai->id;
	rt5677->stream = substream->stream;
	return 0;
}

static int rt5677_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int reg_val = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		rt5677->master[dai->id] = 1;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		reg_val |= RT5677_I2S_MS_S;
		rt5677->master[dai->id] = 0;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		reg_val |= RT5677_I2S_BP_INV;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		reg_val |= RT5677_I2S_DF_LEFT;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		reg_val |= RT5677_I2S_DF_PCM_A;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		reg_val |= RT5677_I2S_DF_PCM_B;
		break;
	default:
		return -EINVAL;
	}

	switch (dai->id) {
	case RT5677_AIF1:
		regmap_update_bits(rt5677->regmap, RT5677_I2S1_SDP,
			RT5677_I2S_MS_MASK | RT5677_I2S_BP_MASK |
			RT5677_I2S_DF_MASK, reg_val);
		break;
	case RT5677_AIF2:
		regmap_update_bits(rt5677->regmap, RT5677_I2S2_SDP,
			RT5677_I2S_MS_MASK | RT5677_I2S_BP_MASK |
			RT5677_I2S_DF_MASK, reg_val);
		break;
	default:
		break;
	}


	return 0;
}

static int rt5677_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	unsigned int reg_val = 0;

	if (freq == rt5677->sysclk && clk_id == rt5677->sysclk_src)
		return 0;

	switch (clk_id) {
	case RT5677_SCLK_S_MCLK:
		reg_val |= RT5677_SCLK_SRC_MCLK;
		break;
	case RT5677_SCLK_S_PLL1:
		reg_val |= RT5677_SCLK_SRC_PLL1;
		break;
	case RT5677_SCLK_S_RCCLK:
		reg_val |= RT5677_SCLK_SRC_RCCLK;
		break;
	default:
		dev_err(codec->dev, "Invalid clock id (%d)\n", clk_id);
		return -EINVAL;
	}
	regmap_update_bits(rt5677->regmap, RT5677_GLB_CLK1,
		RT5677_SCLK_SRC_MASK, reg_val);
	rt5677->sysclk = freq;
	rt5677->sysclk_src = clk_id;

	dev_dbg(dai->dev, "Sysclk is %dHz and clock id is %d\n", freq, clk_id);

	return 0;
}

/**
 * rt5677_pll_calc - Calcualte PLL M/N/K code.
 * @freq_in: external clock provided to codec.
 * @freq_out: target clock which codec works on.
 * @pll_code: Pointer to structure with M, N, K, bypass K and bypass M flag.
 *
 * Calcualte M/N/K code to configure PLL for codec. And K is assigned to 2
 * which make calculation more efficiently.
 *
 * Returns 0 for success or negative error code.
 */
static int rt5677_pll_calc(const unsigned int freq_in,
	const unsigned int freq_out, struct rt5677_pll_code *pll_code)
{
	int max_n = RT5677_PLL_N_MAX, max_m = RT5677_PLL_M_MAX;
	int k, n = 0, m = 0, red, n_t, m_t, pll_out, in_t;
	int out_t, red_t = abs(freq_out - freq_in);
	bool m_bp = false, k_bp = false;

	if (RT5677_PLL_INP_MAX < freq_in || RT5677_PLL_INP_MIN > freq_in)
		return -EINVAL;

	k = 100000000 / freq_out - 2;
	if (k > RT5677_PLL_K_MAX)
		k = RT5677_PLL_K_MAX;
	for (n_t = 0; n_t <= max_n; n_t++) {
		in_t = freq_in / (k + 2);
		pll_out = freq_out / (n_t + 2);
		if (in_t < 0)
			continue;
		if (in_t == pll_out) {
			m_bp = true;
			n = n_t;
			goto code_find;
		}
		red = abs(in_t - pll_out);
		if (red < red_t) {
			m_bp = true;
			n = n_t;
			m = m_t;
			if (red == 0)
				goto code_find;
			red_t = red;
		}
		for (m_t = 0; m_t <= max_m; m_t++) {
			out_t = in_t / (m_t + 2);
			red = abs(out_t - pll_out);
			if (red < red_t) {
				m_bp = false;
				n = n_t;
				m = m_t;
				if (red == 0)
					goto code_find;
				red_t = red;
			}
		}
	}
	pr_debug("Only get approximation about PLL\n");

code_find:

	pll_code->m_bp = m_bp;
	pll_code->k_bp = k_bp;
	pll_code->m_code = m;
	pll_code->n_code = n;
	pll_code->k_code = k;
	return 0;
}

static int rt5677_set_dai_pll(struct snd_soc_dai *dai, int pll_id, int source,
			unsigned int freq_in, unsigned int freq_out)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	struct rt5677_pll_code pll_code;
	int ret;

	if (source == rt5677->pll_src && freq_in == rt5677->pll_in &&
	    freq_out == rt5677->pll_out)
		return 0;

	if (!freq_in || !freq_out) {
		dev_dbg(codec->dev, "PLL disabled\n");

		rt5677->pll_in = 0;
		rt5677->pll_out = 0;
		regmap_update_bits(rt5677->regmap, RT5677_GLB_CLK1,
			RT5677_SCLK_SRC_MASK, RT5677_SCLK_SRC_MCLK);
		return 0;
	}

	switch (source) {
	case RT5677_PLL1_S_MCLK:
		regmap_update_bits(rt5677->regmap, RT5677_GLB_CLK1,
			RT5677_PLL1_SRC_MASK, RT5677_PLL1_SRC_MCLK);
		break;
	case RT5677_PLL1_S_BCLK1:
	case RT5677_PLL1_S_BCLK2:
	case RT5677_PLL1_S_BCLK3:
	case RT5677_PLL1_S_BCLK4:
		switch (dai->id) {
		case RT5677_AIF1:
			regmap_update_bits(rt5677->regmap, RT5677_GLB_CLK1,
				RT5677_PLL1_SRC_MASK, RT5677_PLL1_SRC_BCLK1);
			break;
		case RT5677_AIF2:
			regmap_update_bits(rt5677->regmap, RT5677_GLB_CLK1,
				RT5677_PLL1_SRC_MASK, RT5677_PLL1_SRC_BCLK2);
			break;
		default:
			break;
		}
		break;
	default:
		dev_err(codec->dev, "Unknown PLL source %d\n", source);
		return -EINVAL;
	}

	ret = rt5677_pll_calc(freq_in, freq_out, &pll_code);
	if (ret < 0) {
		dev_err(codec->dev, "Unsupport input clock %d\n", freq_in);
		return ret;
	}

	dev_dbg(codec->dev, "m_bypass=%d k_bypass=%d m=%d n=%d k=%d\n",
		pll_code.m_bp, pll_code.k_bp,
		(pll_code.m_bp ? 0 : pll_code.m_code), pll_code.n_code,
		(pll_code.k_bp ? 0 : pll_code.k_code));

	regmap_write(rt5677->regmap, RT5677_PLL1_CTRL1,
		pll_code.n_code << RT5677_PLL_N_SFT |
		pll_code.k_bp << RT5677_PLL_K_BP_SFT |
		(pll_code.k_bp ? 0 : pll_code.k_code));
	regmap_write(rt5677->regmap, RT5677_PLL1_CTRL2,
		(pll_code.m_bp ? 0 : pll_code.m_code) << RT5677_PLL_M_SFT |
		pll_code.m_bp << RT5677_PLL_M_BP_SFT);

	rt5677->pll_in = freq_in;
	rt5677->pll_out = freq_out;
	rt5677->pll_src = source;

	return 0;
}

/**
 * rt5677_index_show - Dump private registers.
 * @dev: codec device.
 * @attr: device attribute.
 * @buf: buffer for display.
 *
 * To show non-zero values of all private registers.
 *
 * Returns buffer length.
 */
static ssize_t rt5677_index_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5677->codec;
	unsigned int val;
	int cnt = 0, i;

	cnt += sprintf(buf, "RT5677 index register\n");
	for (i = 0; i < 0xff; i++) {
		if (cnt + RT5677_REG_DISP_LEN >= PAGE_SIZE)
			break;
		val = rt5677_index_read(codec, i);
		if (!val)
			continue;
		cnt += snprintf(buf + cnt, RT5677_REG_DISP_LEN,
				"%02x: %04x\n", i, val);
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	return cnt;
}

static ssize_t rt5677_index_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5677->codec;
	unsigned int val = 0, addr = 0;
	int i;

	for (i = 0; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			addr = (addr << 4) | (*(buf + i) - '0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			addr = (addr << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf+i) >= 'A')
			addr = (addr << 4) | ((*(buf + i) - 'A') + 0xa);
		else
			break;
	}

	for (i = i + 1; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			val = (val << 4) | (*(buf + i)-'0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			val = (val << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
			val = (val << 4) | ((*(buf+i)-'A') + 0xa);
		else
			break;

	}
	pr_info("addr = 0x%02x val = 0x%04x\n", addr, val);
	if (addr > RT5677_VENDOR_ID2 || val > 0xffff || val < 0)
		return count;

	if (i == count)
		pr_info("0x%02x = 0x%04x\n", addr,
			rt5677_index_read(codec, addr));
	else
		rt5677_index_write(codec, addr, val);

	return count;
}
static DEVICE_ATTR(index_reg, 0666, rt5677_index_show, rt5677_index_store);

static ssize_t rt5677_codec_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	unsigned int val;
	int cnt = 0, i;

	for (i = 0; i <= RT5677_VENDOR_ID2; i++) {
		if (cnt + RT5677_REG_DISP_LEN >= PAGE_SIZE)
			break;

		if (rt5677_readable_register(NULL, i)) {
			regmap_read(rt5677->regmap, i, &val);

			cnt += snprintf(buf + cnt, RT5677_REG_DISP_LEN,
					"%04x: %04x\n", i, val);
		}
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	return cnt;
}

static ssize_t rt5677_codec_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	unsigned int val = 0, addr = 0;
	int i;

	pr_info("register \"%s\" count = %zu\n", buf, count);
	for (i = 0; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			addr = (addr << 4) | (*(buf + i) - '0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			addr = (addr << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
			addr = (addr << 4) | ((*(buf + i)-'A') + 0xa);
		else
			break;
	}

	for (i = i + 1; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			val = (val << 4) | (*(buf + i) - '0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			val = (val << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
			val = (val << 4) | ((*(buf + i) - 'A') + 0xa);
		else
			break;
	}

	pr_info("addr = 0x%02x val = 0x%04x\n", addr, val);
	if (addr > RT5677_VENDOR_ID2 || val > 0xffff || val < 0)
		return count;

	if (i == count) {
		regmap_read(rt5677->regmap, addr, &val);
		pr_info("0x%02x = 0x%04x\n", addr, val);
	} else
		regmap_write(rt5677->regmap, addr, val);

	return count;
}

static DEVICE_ATTR(codec_reg, 0666, rt5677_codec_show, rt5677_codec_store);

static ssize_t rt5677_dsp_codec_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5677->codec;
	unsigned int val;
	int cnt = 0, i;

	regcache_cache_only(rt5677->regmap, false);
	regcache_cache_bypass(rt5677->regmap, true);

	for (i = 0; i <= RT5677_VENDOR_ID2; i++) {
		if (cnt + RT5677_REG_DISP_LEN >= PAGE_SIZE)
			break;

		if (rt5677_readable_register(NULL, i)) {
			val = rt5677_dsp_mode_i2c_read(codec, i);

			cnt += snprintf(buf + cnt, RT5677_REG_DISP_LEN,
					"%04x: %04x\n", i, val);
		}
	}

	regcache_cache_bypass(rt5677->regmap, false);
	regcache_cache_only(rt5677->regmap, true);

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	return cnt;
}

static ssize_t rt5677_dsp_codec_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5677->codec;
	unsigned int val = 0, addr = 0;
	int i;

	pr_info("register \"%s\" count = %zu\n", buf, count);
	for (i = 0; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			addr = (addr << 4) | (*(buf + i) - '0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			addr = (addr << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
			addr = (addr << 4) | ((*(buf + i)-'A') + 0xa);
		else
			break;
	}

	for (i = i + 1; i < count; i++) {
		if (*(buf + i) <= '9' && *(buf + i) >= '0')
			val = (val << 4) | (*(buf + i) - '0');
		else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
			val = (val << 4) | ((*(buf + i) - 'a') + 0xa);
		else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
			val = (val << 4) | ((*(buf + i) - 'A') + 0xa);
		else
			break;
	}

	pr_info("addr = 0x%02x val = 0x%04x\n", addr, val);
	if (addr > RT5677_VENDOR_ID2 || val > 0xffff || val < 0)
		return count;

	regcache_cache_only(rt5677->regmap, false);
	regcache_cache_bypass(rt5677->regmap, true);

	if (i == count) {
		val = rt5677_dsp_mode_i2c_read(codec, addr);
		pr_info("0x%02x = 0x%04x\n", addr, val);
	} else
		rt5677_dsp_mode_i2c_write(codec, addr, val);

	regcache_cache_bypass(rt5677->regmap, false);
	regcache_cache_only(rt5677->regmap, true);

	return count;
}
static DEVICE_ATTR(dsp_codec_reg, 0666, rt5677_dsp_codec_show, rt5677_dsp_codec_store);

static int rt5677_set_bias_level(struct snd_soc_codec *codec,
			enum snd_soc_bias_level level)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
	int i;

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		if (codec->dapm.bias_level == SND_SOC_BIAS_STANDBY) {
			regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
				RT5677_LDO1_SEL_MASK | RT5677_LDO2_SEL_MASK,
				0x36);
			rt5677_index_update_bits(codec, RT5677_BIAS_CUR4,
				0x0f00, 0x0f00);
			regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
				RT5677_PWR_VREF1 | RT5677_PWR_MB |
				RT5677_PWR_BG | RT5677_PWR_VREF2,
				RT5677_PWR_VREF1 | RT5677_PWR_MB |
				RT5677_PWR_BG | RT5677_PWR_VREF2);
			if (rt5677->stream == SNDRV_PCM_STREAM_PLAYBACK) {
				regmap_update_bits(rt5677->regmap,
					RT5677_PWR_ANLG1,
					RT5677_PWR_LO1 | RT5677_PWR_LO2,
					RT5677_PWR_LO1 | RT5677_PWR_LO2);
			}
			usleep_range(15000, 20000);
			regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
				RT5677_PWR_FV1 | RT5677_PWR_FV2,
				RT5677_PWR_FV1 | RT5677_PWR_FV2);
			regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
				RT5677_PWR_CORE, RT5677_PWR_CORE);
			regmap_update_bits(rt5677->regmap, RT5677_DIG_MISC,
				0x1, 0x1);
		}
		break;

	case SND_SOC_BIAS_STANDBY:
		if (codec->dapm.bias_level == SND_SOC_BIAS_OFF) {
			rt5677_set_vad(codec, 0, true);
			set_rt5677_power_extern(true);
			regcache_cache_only(rt5677->regmap, false);
			regcache_mark_dirty(rt5677->regmap);
			for (i = 0; i < RT5677_VENDOR_ID2 + 1; i++)
				regcache_sync_region(rt5677->regmap, i, i);
			rt5677_index_sync(codec);
		}
		break;

	case SND_SOC_BIAS_OFF:
		regmap_update_bits(rt5677->regmap, RT5677_DIG_MISC, 0x1, 0x0);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG1, 0x0000);
		regmap_write(rt5677->regmap, RT5677_PWR_DIG2, 0x0000);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG1, 0x0022);
		regmap_write(rt5677->regmap, RT5677_PWR_ANLG2, 0x0000);
		rt5677_index_update_bits(codec,
			RT5677_BIAS_CUR4, 0x0f00, 0x0000);

		if (rt5677->vad_mode == RT5677_VAD_IDLE)
			rt5677_set_vad(codec, 1, true);
		if (rt5677->vad_mode == RT5677_VAD_OFF)
			regcache_cache_only(rt5677->regmap, true);
		break;

	default:
		break;
	}
	codec->dapm.bias_level = level;

	return 0;
}

static int rt5677_probe(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);
#ifdef RTK_IOCTL
#if defined(CONFIG_SND_HWDEP) || defined(CONFIG_SND_HWDEP_MODULE)
	struct rt_codec_ops *ioctl_ops = rt_codec_get_ioctl_ops();
#endif
#endif
	int ret;

	pr_info("Codec driver version %s\n", VERSION);

	ret = snd_soc_codec_set_cache_io(codec, 8, 16, SND_SOC_REGMAP);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
		return ret;
	}

	regmap_read(rt5677->regmap, RT5677_VENDOR_ID2, &ret);
	if (ret != RT5677_DEVICE_ID) {
		dev_err(codec->dev,
			"Device with ID register %x is not rt5677\n", ret);
		return -ENODEV;
	}

	rt5677_reset(codec);
	regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
		RT5677_PWR_VREF1 | RT5677_PWR_MB |
		RT5677_PWR_BG | RT5677_PWR_VREF2,
		RT5677_PWR_VREF1 | RT5677_PWR_MB |
		RT5677_PWR_BG | RT5677_PWR_VREF2);
	msleep(20);
	regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG1,
		RT5677_PWR_FV1 | RT5677_PWR_FV2,
		RT5677_PWR_FV1 | RT5677_PWR_FV2);

	regmap_update_bits(rt5677->regmap, RT5677_PWR_ANLG2,
		RT5677_PWR_CORE, RT5677_PWR_CORE);

	rt5677_reg_init(codec);

	rt5677->codec = codec;

	mutex_init(&rt5677->index_lock);
	mutex_init(&rt5677->vad_lock);

#ifdef RTK_IOCTL
#if defined(CONFIG_SND_HWDEP) || defined(CONFIG_SND_HWDEP_MODULE)
	ioctl_ops->index_write = rt5677_index_write;
	ioctl_ops->index_read = rt5677_index_read;
	ioctl_ops->index_update_bits = rt5677_index_update_bits;
	ioctl_ops->ioctl_common = rt5677_ioctl_common;
	realtek_ce_init_hwdep(codec);
#endif
#endif

	ret = device_create_file(codec->dev, &dev_attr_index_reg);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create index_reg sysfs files: %d\n", ret);
		return ret;
	}

	ret = device_create_file(codec->dev, &dev_attr_codec_reg);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create codec_reg sysfs files: %d\n", ret);
		return ret;
	}

	ret = device_create_file(codec->dev, &dev_attr_dsp_codec_reg);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create dsp_codec_reg sysfs files: %d\n", ret);
		return ret;
	}

	rt5677_set_bias_level(codec, SND_SOC_BIAS_OFF);

	rt5677_register_hs_notification();
	rt5677->check_mic_wq = create_workqueue("check_hp_mic");
	INIT_DELAYED_WORK(&rt5677->check_hp_mic_work, rt5677_check_hp_mic);
	rt5677->mic_state = HEADSET_UNPLUG;
	rt5677_global = rt5677;

	return 0;
}

static int rt5677_remove(struct snd_soc_codec *codec)
{
	rt5677_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

#ifdef CONFIG_PM
static int rt5677_suspend(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	if (rt5677->vad_mode == RT5677_VAD_SUSPEND)
		rt5677_set_vad(codec, 1, true);

	return 0;
}

static int rt5677_resume(struct snd_soc_codec *codec)
{
	struct rt5677_priv *rt5677 = snd_soc_codec_get_drvdata(codec);

	if (rt5677->vad_mode == RT5677_VAD_SUSPEND)
		rt5677_set_vad(codec, 0, true);

	return 0;
}
#else
#define rt5677_suspend NULL
#define rt5677_resume NULL
#endif

static void rt5677_shutdown(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	pr_debug("enter %s\n", __func__);
}

#define RT5677_STEREO_RATES SNDRV_PCM_RATE_8000_96000
#define RT5677_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S8)

struct snd_soc_dai_ops rt5677_aif_dai_ops = {
	.hw_params = rt5677_hw_params,
	.prepare = rt5677_prepare,
	.set_fmt = rt5677_set_dai_fmt,
	.set_sysclk = rt5677_set_dai_sysclk,
	.set_pll = rt5677_set_dai_pll,
	.shutdown = rt5677_shutdown,
};

struct snd_soc_dai_driver rt5677_dai[] = {
	{
		.name = "rt5677-aif1",
		.id = RT5677_AIF1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.ops = &rt5677_aif_dai_ops,
	},
	{
		.name = "rt5677-aif2",
		.id = RT5677_AIF2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.ops = &rt5677_aif_dai_ops,
	},
	{
		.name = "rt5677-aif3",
		.id = RT5677_AIF3,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.ops = &rt5677_aif_dai_ops,
	},
	{
		.name = "rt5677-aif4",
		.id = RT5677_AIF4,
		.playback = {
			.stream_name = "AIF4 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.capture = {
			.stream_name = "AIF4 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.ops = &rt5677_aif_dai_ops,
	},
	{
		.name = "rt5677-slimbus",
		.id = RT5677_AIF5,
		.playback = {
			.stream_name = "SLIMBus Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.capture = {
			.stream_name = "SLIMBus Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5677_STEREO_RATES,
			.formats = RT5677_FORMATS,
		},
		.ops = &rt5677_aif_dai_ops,
	},
};

static struct snd_soc_codec_driver soc_codec_dev_rt5677 = {
	.probe = rt5677_probe,
	.remove = rt5677_remove,
	.suspend = rt5677_suspend,
	.resume = rt5677_resume,
	.set_bias_level = rt5677_set_bias_level,
	.idle_bias_off = true,
	.controls = rt5677_snd_controls,
	.num_controls = ARRAY_SIZE(rt5677_snd_controls),
	.dapm_widgets = rt5677_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rt5677_dapm_widgets),
	.dapm_routes = rt5677_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(rt5677_dapm_routes),
};

static const struct regmap_config rt5677_regmap = {
	.reg_bits = 8,
	.val_bits = 16,

	.max_register = RT5677_VENDOR_ID2 + 1,
	.volatile_reg = rt5677_volatile_register,
	.readable_reg = rt5677_readable_register,

	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = rt5677_reg,
	.num_reg_defaults = ARRAY_SIZE(rt5677_reg),
};

static const struct i2c_device_id rt5677_i2c_id[] = {
	{ "rt5677", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt5677_i2c_id);

static int rt5677_i2c_probe(struct i2c_client *i2c,
		    const struct i2c_device_id *id)
{
	struct rt5677_priv *rt5677;
	int ret;

	rt5677 = kzalloc(sizeof(struct rt5677_priv), GFP_KERNEL);
	if (NULL == rt5677)
		return -ENOMEM;

	rt5677->mbist_test = false;
	rt5677->vad_sleep = true;
	rt5677->mic_buf_len = RT5677_PRIV_MIC_BUF_SIZE;
	rt5677->mic_buf = vmalloc(rt5677->mic_buf_len);
	if (NULL == rt5677->mic_buf) {
		kfree(rt5677);
		return -ENOMEM;
	}

	i2c_set_clientdata(i2c, rt5677);

	rt5677->regmap = devm_regmap_init_i2c(i2c, &rt5677_regmap);
	if (IS_ERR(rt5677->regmap)) {
		ret = PTR_ERR(rt5677->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_rt5677,
			rt5677_dai, ARRAY_SIZE(rt5677_dai));
	if (ret < 0)
		kfree(rt5677);

	if (i2c->dev.platform_data)	{

		rt5677->vad_clock_en =
		((struct rt5677_priv *)i2c->dev.platform_data)->vad_clock_en;

		if (gpio_is_valid(rt5677->vad_clock_en)) {
			dev_dbg(&i2c->dev, "vad_clock_en: %d\n",
			rt5677->vad_clock_en);

			ret = gpio_request(rt5677->vad_clock_en, "vad_clock_en");
			if (ret) {
				dev_err(&i2c->dev, "cannot get vad_clock_en gpio\n");
			} else {
				ret = gpio_direction_output(rt5677->vad_clock_en, 0);
				if (ret) {
					dev_err(&i2c->dev,
					"vad_clock_en=0 fail,%d\n", ret);

					gpio_free(rt5677->vad_clock_en);
				} else
					dev_dbg(&i2c->dev, "vad_clock_en=0\n");
			}
		} else {
			dev_dbg(&i2c->dev, "vad_clock_en is invalid: %d\n",
			rt5677->vad_clock_en);
		}
	}

	return ret;
}

static int rt5677_i2c_remove(struct i2c_client *i2c)
{
	struct rt5677_priv *rt5677 = i2c_get_clientdata(i2c);

	snd_soc_unregister_codec(&i2c->dev);
	vfree(rt5677->mic_buf);
	vfree(rt5677->model_buf);
	kfree(rt5677);
	return 0;
}

void rt5677_i2c_shutdown(struct i2c_client *client)
{
	struct rt5677_priv *rt5677 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5677->codec;

	if (codec != NULL)
		rt5677_set_bias_level(codec, SND_SOC_BIAS_OFF);
}

struct i2c_driver rt5677_i2c_driver = {
	.driver = {
		.name = "rt5677",
		.owner = THIS_MODULE,
	},
	.probe = rt5677_i2c_probe,
	.remove   = rt5677_i2c_remove,
	.shutdown = rt5677_i2c_shutdown,
	.id_table = rt5677_i2c_id,
};

static int __init rt5677_modinit(void)
{
	return i2c_add_driver(&rt5677_i2c_driver);
}
module_init(rt5677_modinit);

static void __exit rt5677_modexit(void)
{
	i2c_del_driver(&rt5677_i2c_driver);
}
module_exit(rt5677_modexit);

MODULE_DESCRIPTION("ASoC RT5677 driver");
MODULE_AUTHOR("Bard Liao <bardliao@realtek.com>");
MODULE_LICENSE("GPL");