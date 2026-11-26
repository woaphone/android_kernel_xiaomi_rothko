// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Griffin Kroah-Hartman <griffin.kroah@fairphone.com>
 *
 * Partially based on vendor driver:
 *	Copyright (c) 2021 AWINIC Technology CO., LTD
 *
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/pinctrl/consumer.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#define AW86927_RSTCFG_REG			0x00
#define AW86927_RSTCFG_SOFTRST			0xaa

#define AW86927_SYSINT_REG			0x02
#define AW86927_SYSINT_BST_SCPI			BIT(7)
#define AW86927_SYSINT_BST_OVPI			BIT(6)
#define AW86927_SYSINT_UVLI			BIT(5)
#define AW86927_SYSINT_FF_AEI			BIT(4)
#define AW86927_SYSINT_FF_AFI			BIT(3)
#define AW86927_SYSINT_OCDI			BIT(2)
#define AW86927_SYSINT_OTI			BIT(1)
#define AW86927_SYSINT_DONEI			BIT(0)

#define AW86927_SYSINTM_REG			0x03
#define AW86927_SYSINTM_BST_OVPM		BIT(6)
#define AW86927_SYSINTM_FF_AEM			BIT(4)
#define AW86927_SYSINTM_FF_AFM			BIT(3)
#define AW86927_SYSINTM_DONEM			BIT(0)

#define AW86927_PLAYCFG1_REG			0x06
#define AW86927_PLAYCFG1_BST_MODE_MASK		GENMASK(7, 7)
#define AW86927_PLAYCFG1_BST_MODE_BOOST		BIT(7)
#define AW86927_PLAYCFG1_BST_VOUT_VREFSET_MASK	GENMASK(6, 0)
#define AW86927_ROTHKO_BOOST_MAX		0x64
#define AW86927_ROTHKO_GAIN_MAX		0x7c
#define AW86927_MAX_EFFECTS		32
#define AW86927_RAM_FIRMWARE		"aw8697_haptic.bin"

#define AW86927_PLAYCFG2_REG			0x07

#define AW86927_PLAYCFG3_REG			0x08
#define AW86927_PLAYCFG3_AUTO_BST_MASK		GENMASK(4, 4)
#define AW86927_PLAYCFG3_AUTO_BST_DISABLE	0
#define AW86927_PLAYCFG3_PLAY_MODE_MASK		GENMASK(1, 0)
#define AW86927_PLAYCFG3_PLAY_MODE_RAM		0

#define AW86927_PLAYCFG4_REG			0x09
#define AW86927_PLAYCFG4_STOP			BIT(1)
#define AW86927_PLAYCFG4_GO			BIT(0)

#define AW86927_WAVCFG1_REG			0x0a
#define AW86927_WAVCFG1_WAVSEQ1_MASK		GENMASK(6, 0)

#define AW86927_WAVCFG2_REG			0x0b
#define AW86927_WAVCFG2_WAVSEQ2_MASK		GENMASK(6, 0)

#define AW86927_WAVCFG9_REG			0x12
#define AW86927_WAVCFG9_SEQ1LOOP_MASK		GENMASK(7, 4)
#define AW86927_WAVCFG9_SEQ1LOOP_INFINITELY	0x0f

#define AW86927_CONTCFG1_REG			0x18
#define AW86927_CONTCFG1_BRK_BST_MD_MASK	GENMASK(6, 6)

#define AW86927_CONTCFG5_REG			0x1c
#define AW86927_CONTCFG5_BST_BRK_GAIN_MASK	GENMASK(7, 4)
#define AW86927_CONTCFG5_BRK_GAIN_MASK		GENMASK(3, 0)

#define AW86927_CONTCFG10_REG			0x21
#define AW86927_CONTCFG10_BRK_TIME_MASK		GENMASK(7, 0)
#define AW86927_CONTCFG10_BRK_TIME_DEFAULT	8

#define AW86927_CONTCFG13_REG			0x24
#define AW86927_CONTCFG13_TSET_MASK		GENMASK(7, 4)
#define AW86927_CONTCFG13_BEME_SET_MASK		GENMASK(3, 0)

#define AW86927_BASEADDRH_REG			0x2d
#define AW86927_BASEADDRL_REG			0x2e

#define AW86927_GLBRD5_REG			0x3f
#define AW86927_GLBRD5_STATE_MASK		GENMASK(3, 0)
#define AW86927_GLBRD5_STATE_STANDBY		0

#define AW86927_RAMADDRH_REG			0x40

#define AW86927_RAMADDRL_REG			0x41

#define AW86927_RAMDATA_REG			0x42

#define AW86927_SYSCTRL3_REG			0x45
#define AW86927_SYSCTRL3_STANDBY_MASK           GENMASK(5, 5)
#define AW86927_SYSCTRL3_STANDBY_ON             1
#define AW86927_SYSCTRL3_STANDBY_OFF            0
#define AW86927_SYSCTRL3_EN_RAMINIT_MASK        GENMASK(2, 2)
#define AW86927_SYSCTRL3_EN_RAMINIT_ON          1
#define AW86927_SYSCTRL3_EN_RAMINIT_OFF         0

#define AW86927_SYSCTRL4_REG			0x46
#define AW86927_SYSCTRL4_WAVDAT_MODE_MASK	GENMASK(6, 5)
#define AW86927_SYSCTRL4_WAVDAT_24K		0
#define AW86927_SYSCTRL4_INT_EDGE_MODE_MASK	GENMASK(4, 4)
#define AW86927_SYSCTRL4_INT_EDGE_MODE_POS	0
#define AW86927_SYSCTRL4_INT_MODE_MASK		GENMASK(3, 3)
#define AW86927_SYSCTRL4_INT_MODE_EDGE		1
#define AW86927_SYSCTRL4_GAIN_BYPASS_MASK	GENMASK(0, 0)

#define AW86927_PWMCFG1_REG			0x48
#define AW86927_PWMCFG1_PRC_EN_MASK		GENMASK(7, 7)
#define AW86927_PWMCFG1_PRC_DISABLE		0

#define AW86927_PWMCFG3_REG			0x4a
#define AW86927_PWMCFG3_PR_EN_MASK		GENMASK(7, 7)
#define AW86927_PWMCFG3_PRCTIME_MASK		GENMASK(6, 0)

#define AW86927_PWMCFG4_REG			0x4b
#define AW86927_PWMCFG4_PRTIME_MASK		GENMASK(7, 0)

#define AW86927_VBATCTRL_REG			0x4c
#define AW86927_VBATCTRL_VBAT_MODE_MASK		GENMASK(6, 6)
#define AW86927_VBATCTRL_VBAT_MODE_SW		0

#define AW86927_DETCFG1_REG			0x4d
#define AW86927_DETCFG1_DET_GO_MASK		GENMASK(1, 0)
#define AW86927_DETCFG1_DET_GO_DET_SEQ0		1
#define AW86927_DETCFG1_DET_GO_NA		0

#define AW86927_DETCFG2_REG			0x4e
#define AW86927_DETCFG2_DET_SEQ0_MASK		GENMASK(6, 3)
#define AW86927_DETCFG2_DET_SEQ0_VBAT		0
#define AW86927_DETCFG2_D2S_GAIN_MASK		GENMASK(2, 0)
#define AW86927_DETCFG2_D2S_GAIN_10		4

#define AW86927_CHIPIDH_REG			0x57
#define AW86927_CHIPIDL_REG			0x58
#define AW86927_CHIPID				0x9270

#define AW86927_TMCFG_REG			0x5b
#define AW86927_TMCFG_UNLOCK			0x7d
#define AW86927_TMCFG_LOCK			0x00

#define AW86927_ANACFG11_REG			0x70

#define AW86927_ANACFG12_REG			0x71
#define AW86927_ANACFG12_BST_SKIP_MASK		GENMASK(7, 7)
#define AW86927_ANACFG12_BST_SKIP_SHUTDOWN	1

#define AW86927_ANACFG13_REG			0x72
#define AW86927_ANACFG13_BST_PC_MASK		GENMASK(7, 4)
#define AW86927_ANACFG13_BST_PEAKCUR_3P45A	6

#define AW86927_ANACFG15_REG			0x74
#define AW86927_ANACFG15_BST_PEAK_MODE_MASK	GENMASK(7, 7)
#define AW86927_ANACFG15_BST_PEAK_BACK		1

#define AW86927_ANACFG16_REG			0x75
#define AW86927_ANACFG16_BST_SRC_MASK		GENMASK(4, 4)
#define AW86927_ANACFG16_BST_SRC_3NS		0

enum aw86927_work_mode {
	AW86927_STANDBY_MODE,
	AW86927_RAM_MODE,
	AW86927_RAM_LOOP_MODE,
};

struct aw86927_data {
	struct work_struct play_work;
	struct delayed_work stop_work;
	spinlock_t effect_lock;
	struct device *dev;
	struct input_dev *input_dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u8 waveform[AW86927_MAX_EFFECTS];
	u16 ram_duration_ms[127];
	u8 wave_count, effect_boundary, boost;
	u8 selected_wave, level;
	u16 gain, duration;
	int playing_id;
	bool running, loop, play_pending, gain_pending;
};

static const struct regmap_config aw86927_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.max_register = 0x80,
};

static int aw86927_wait_enter_standby(struct aw86927_data *haptics)
{
	unsigned int reg_val;
	int err;

	err = regmap_read_poll_timeout(haptics->regmap, AW86927_GLBRD5_REG, reg_val,
				       (FIELD_GET(AW86927_GLBRD5_STATE_MASK, reg_val) ==
						AW86927_GLBRD5_STATE_STANDBY),
				       2500, 2500 * 100);

	if (err) {
		dev_err(haptics->dev, "did not enter standby: %d\n", err);
		return err;
	}
	return 0;
}

static int aw86927_play_mode(struct aw86927_data *haptics, u8 play_mode)
{
	int err;

	switch (play_mode) {
	case AW86927_STANDBY_MODE:
		/* Briefly toggle standby, then toggle back to standby off */
		err = regmap_update_bits(haptics->regmap,
					 AW86927_SYSCTRL3_REG,
					 AW86927_SYSCTRL3_STANDBY_MASK,
					 FIELD_PREP(AW86927_SYSCTRL3_STANDBY_MASK,
						    AW86927_SYSCTRL3_STANDBY_ON));
		if (err)
			return err;

		err = regmap_update_bits(haptics->regmap,
					 AW86927_SYSCTRL3_REG,
					 AW86927_SYSCTRL3_STANDBY_MASK,
					 FIELD_PREP(AW86927_SYSCTRL3_STANDBY_MASK,
						    AW86927_SYSCTRL3_STANDBY_OFF));
		if (err)
			return err;

		break;

	case AW86927_RAM_MODE:
	case AW86927_RAM_LOOP_MODE:
		err = regmap_update_bits(haptics->regmap,
					 AW86927_PLAYCFG3_REG,
					 AW86927_PLAYCFG3_PLAY_MODE_MASK,
					 FIELD_PREP(AW86927_PLAYCFG3_PLAY_MODE_MASK,
						    AW86927_PLAYCFG3_PLAY_MODE_RAM));
		if (err)
			return err;

		/* Xiaomi's one-shot RAM mode enables boost. RAM loop bypasses it. */
		err = regmap_update_bits(haptics->regmap, AW86927_PLAYCFG1_REG,
					 AW86927_PLAYCFG1_BST_MODE_MASK,
					 play_mode == AW86927_RAM_MODE ?
					 AW86927_PLAYCFG1_BST_MODE_BOOST : 0);
		if (err)
			return err;

		err = regmap_update_bits(haptics->regmap,
					 AW86927_VBATCTRL_REG,
					 AW86927_VBATCTRL_VBAT_MODE_MASK,
					 FIELD_PREP(AW86927_VBATCTRL_VBAT_MODE_MASK,
						    AW86927_VBATCTRL_VBAT_MODE_SW));
		if (err)
			return err;

		break;
	}

	return 0;
}

static int aw86927_stop(struct aw86927_data *haptics)
{
	int err;

	err = regmap_write(haptics->regmap, AW86927_PLAYCFG4_REG, AW86927_PLAYCFG4_STOP);
	if (err) {
		dev_err(haptics->dev, "Failed to stop playback: %d\n", err);
		return err;
	}

	err = aw86927_wait_enter_standby(haptics);
	if (err) {
		dev_err(haptics->dev, "Failed to enter standby, trying to force it\n");
		err = aw86927_play_mode(haptics, AW86927_STANDBY_MODE);
		if (err)
			return err;
	}

	return 0;
}

static int aw86927_prepare_waveforms(struct aw86927_data *haptics);

static int aw86927_upload(struct input_dev *dev, struct ff_effect *effect,
			  struct ff_effect *old)
{
	struct aw86927_data *haptics = input_get_drvdata(dev);
	s16 selector;
	u8 wave;
	int ret;

	ret = aw86927_prepare_waveforms(haptics);
	if (ret)
		return ret;

	if (effect->type == FF_PERIODIC) {
		if (effect->u.periodic.waveform != FF_CUSTOM ||
		    !effect->u.periodic.custom_len)
			return -EINVAL;
		if (get_user(selector, effect->u.periodic.custom_data))
			return -EFAULT;
		if (selector < 0)
			return -EINVAL;
		if (selector >= haptics->effect_boundary)
			return -EOPNOTSUPP;
		wave = selector + 1;
	} else {
		/* Xiaomi reserves the next RAM entry for sustained vibration. */
		wave = haptics->effect_boundary + 1;
	}
	if (wave > haptics->wave_count)
		return -EINVAL;

	/* HyperOS reads the selected RAM effect's duration back in seconds/ms. */
	if (effect->type == FF_PERIODIC && effect->u.periodic.custom_len >= 3) {
		u16 duration = haptics->ram_duration_ms[wave - 1];
		s16 custom[] = { wave - 1, duration / 1000, duration % 1000 };

		if (copy_to_user(effect->u.periodic.custom_data, custom, sizeof(custom)))
			return -EFAULT;
	}

	WRITE_ONCE(haptics->waveform[effect->id], wave);
	return 0;
}

/* Xiaomi's AW86927 force-feedback values use a biased gain scale. */
static u8 aw86927_ff_level(u16 magnitude)
{
	if (magnitude >= 0x7fff)
		return 0x80;
	if (magnitude < 0x4000)
		return 0x1e;

	return max(0x1e, (magnitude - 0x3fff) / 128);
}

static int aw86927_playback(struct input_dev *dev, int id, int value)
{
	struct aw86927_data *haptics = input_get_drvdata(dev);
	const struct ff_effect *effect = &dev->ff->effects[id];
	unsigned int strength;
	u8 level;

	switch (effect->type) {
	case FF_RUMBLE:
		strength = max(effect->u.rumble.strong_magnitude,
			       effect->u.rumble.weak_magnitude);
		level = DIV_ROUND_CLOSEST(strength * AW86927_ROTHKO_GAIN_MAX, U16_MAX);
		break;
	case FF_CONSTANT:
		level = aw86927_ff_level(abs(effect->u.constant.level));
		break;
	default:
		level = aw86927_ff_level((u16)effect->u.periodic.magnitude);
		break;
	}

	spin_lock(&haptics->effect_lock);
	if (value <= 0 && haptics->playing_id != id) {
		spin_unlock(&haptics->effect_lock);
		return 0;
	}
	haptics->playing_id = value > 0 ? id : -1;
	haptics->selected_wave = READ_ONCE(haptics->waveform[id]);
	haptics->level = level;
	haptics->duration = effect->type == FF_PERIODIC ? 0 : effect->replay.length;
	haptics->loop = effect->type == FF_CONSTANT || haptics->duration;
	haptics->running = value > 0 && level;
	haptics->play_pending = true;
	haptics->gain_pending = false;
	spin_unlock(&haptics->effect_lock);
	schedule_work(&haptics->play_work);
	return 0;
}

static void aw86927_set_gain(struct input_dev *dev, u16 gain)
{
	struct aw86927_data *haptics = input_get_drvdata(dev);

	spin_lock(&haptics->effect_lock);
	haptics->gain = gain;
	haptics->gain_pending = true;
	spin_unlock(&haptics->effect_lock);
	schedule_work(&haptics->play_work);
}

static int aw86927_play_wave(struct aw86927_data *haptics, u8 wave, u8 gain,
			     bool loop)
{
	int err;

	err = aw86927_stop(haptics);
	if (err)
		return err;

	err = aw86927_play_mode(haptics, loop ? AW86927_RAM_LOOP_MODE : AW86927_RAM_MODE);
	if (err)
		return err;

	/* Play the selected entry from the stock waveform table. */
	err = regmap_update_bits(haptics->regmap, AW86927_WAVCFG1_REG,
				 AW86927_WAVCFG1_WAVSEQ1_MASK,
				 FIELD_PREP(AW86927_WAVCFG1_WAVSEQ1_MASK, wave));
	if (err)
		return err;

	/* set wavseq 2 to zero */
	err = regmap_update_bits(haptics->regmap, AW86927_WAVCFG2_REG,
				 AW86927_WAVCFG2_WAVSEQ2_MASK,
				 FIELD_PREP(AW86927_WAVCFG2_WAVSEQ2_MASK, 0));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_WAVCFG9_REG,
				 AW86927_WAVCFG9_SEQ1LOOP_MASK,
				 FIELD_PREP(AW86927_WAVCFG9_SEQ1LOOP_MASK,
					    loop ? AW86927_WAVCFG9_SEQ1LOOP_INFINITELY : 0));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW86927_PLAYCFG2_REG, gain);
	if (err)
		return err;

	/* Start playback */
	err = regmap_write(haptics->regmap, AW86927_PLAYCFG4_REG, AW86927_PLAYCFG4_GO);
	if (err)
		return err;

	return 0;
}

static void aw86927_close(struct input_dev *input)
{
	struct aw86927_data *haptics = input_get_drvdata(input);
	struct device *dev = &haptics->client->dev;
	int err;

	cancel_work_sync(&haptics->play_work);
	cancel_delayed_work_sync(&haptics->stop_work);

	err = aw86927_stop(haptics);
	if (err)
		dev_err(dev, "Failed to close the Driver: %d\n", err);
}

static void aw86927_stop_work(struct work_struct *work)
{
	struct aw86927_data *haptics =
		container_of(to_delayed_work(work), struct aw86927_data, stop_work);
	int ret = aw86927_stop(haptics);

	if (ret)
		dev_err(haptics->dev, "Failed to stop playback: %d\n", ret);
}

static void aw86927_haptics_play_work(struct work_struct *work)
{
	struct aw86927_data *haptics =
		container_of(work, struct aw86927_data, play_work);
	unsigned long flags;
	bool running, loop, play_pending, gain_pending;
	u8 wave, gain;
	u16 duration;
	int err;

	spin_lock_irqsave(&haptics->effect_lock, flags);
	running = haptics->running;
	loop = haptics->loop;
	wave = haptics->selected_wave;
	play_pending = haptics->play_pending;
	gain_pending = haptics->gain_pending;
	gain = gain_pending ? aw86927_ff_level(haptics->gain) : haptics->level;
	duration = haptics->duration;
	haptics->play_pending = false;
	haptics->gain_pending = false;
	spin_unlock_irqrestore(&haptics->effect_lock, flags);

	if (!play_pending) {
		if (!gain_pending)
			return;
		err = regmap_write(haptics->regmap, AW86927_PLAYCFG2_REG, gain);
	} else {
		cancel_delayed_work_sync(&haptics->stop_work);
		if (running && wave) {
			err = aw86927_play_wave(haptics, wave, gain, loop);
			if (!err && duration)
				mod_delayed_work(system_wq, &haptics->stop_work,
						 msecs_to_jiffies(duration));
		} else {
			err = aw86927_stop(haptics);
		}
	}
	if (err)
		dev_err(haptics->dev, "Failed to execute playback: %d\n", err);
}

static void aw86927_hw_reset(struct aw86927_data *haptics)
{
	/* Stock reset-gpio is declared active high but the chip resets at low. */
	gpiod_set_raw_value_cansleep(haptics->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_raw_value_cansleep(haptics->reset_gpio, 1);
	usleep_range(8000, 8500);
}

static int aw86927_haptic_init(struct aw86927_data *haptics)
{
	int err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_SYSCTRL4_REG,
				 AW86927_SYSCTRL4_WAVDAT_MODE_MASK,
				 FIELD_PREP(AW86927_SYSCTRL4_WAVDAT_MODE_MASK,
					    AW86927_SYSCTRL4_WAVDAT_24K));
	if (err)
		return err;

	/* enable gain bypass */
	err = regmap_update_bits(haptics->regmap,
				 AW86927_SYSCTRL4_REG,
				 AW86927_SYSCTRL4_GAIN_BYPASS_MASK,
				 FIELD_PREP(AW86927_SYSCTRL4_GAIN_BYPASS_MASK,
					    0x01));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_TMCFG_REG, AW86927_TMCFG_UNLOCK);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW86927_ANACFG11_REG, 0x0f);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_ANACFG12_REG,
				 AW86927_ANACFG12_BST_SKIP_MASK,
				 FIELD_PREP(AW86927_ANACFG12_BST_SKIP_MASK,
					    AW86927_ANACFG12_BST_SKIP_SHUTDOWN));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_ANACFG15_REG,
				 AW86927_ANACFG15_BST_PEAK_MODE_MASK,
				 FIELD_PREP(AW86927_ANACFG15_BST_PEAK_MODE_MASK,
					    AW86927_ANACFG15_BST_PEAK_BACK));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_ANACFG16_REG,
				 AW86927_ANACFG16_BST_SRC_MASK,
				 FIELD_PREP(AW86927_ANACFG16_BST_SRC_MASK,
					    AW86927_ANACFG16_BST_SRC_3NS));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_TMCFG_REG, AW86927_TMCFG_LOCK);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_CONTCFG1_REG,
				 AW86927_CONTCFG1_BRK_BST_MD_MASK,
				 FIELD_PREP(AW86927_CONTCFG1_BRK_BST_MD_MASK, 0x00));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_CONTCFG5_REG,
			   FIELD_PREP(AW86927_CONTCFG5_BST_BRK_GAIN_MASK, 0x05) |
				FIELD_PREP(AW86927_CONTCFG5_BRK_GAIN_MASK, 0x08));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW86927_CONTCFG10_REG,
				 AW86927_CONTCFG10_BRK_TIME_MASK,
				 FIELD_PREP(AW86927_CONTCFG10_BRK_TIME_MASK,
					    AW86927_CONTCFG10_BRK_TIME_DEFAULT));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_CONTCFG13_REG,
			   FIELD_PREP(AW86927_CONTCFG13_TSET_MASK, 0x06) |
				FIELD_PREP(AW86927_CONTCFG13_BEME_SET_MASK, 0x02));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_DETCFG2_REG,
				 AW86927_DETCFG2_D2S_GAIN_MASK,
				 FIELD_PREP(AW86927_DETCFG2_D2S_GAIN_MASK,
					    AW86927_DETCFG2_D2S_GAIN_10));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_PWMCFG1_REG,
				 AW86927_PWMCFG1_PRC_EN_MASK,
				 FIELD_PREP(AW86927_PWMCFG1_PRC_EN_MASK,
					    AW86927_PWMCFG1_PRC_DISABLE));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_PWMCFG3_REG,
			   FIELD_PREP(AW86927_PWMCFG3_PR_EN_MASK, 0x01) |
				FIELD_PREP(AW86927_PWMCFG3_PRCTIME_MASK, 0x3f));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_PWMCFG4_REG,
				 AW86927_PWMCFG4_PRTIME_MASK,
				 FIELD_PREP(AW86927_PWMCFG4_PRTIME_MASK, 0x32));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_TMCFG_REG, AW86927_TMCFG_UNLOCK);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_ANACFG13_REG,
				 AW86927_ANACFG13_BST_PC_MASK,
				 FIELD_PREP(AW86927_ANACFG13_BST_PC_MASK,
					    AW86927_ANACFG13_BST_PEAKCUR_3P45A));
	if (err)
		return err;

	err = regmap_write(haptics->regmap,
			   AW86927_TMCFG_REG, AW86927_TMCFG_LOCK);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_PLAYCFG1_REG,
				 AW86927_PLAYCFG1_BST_VOUT_VREFSET_MASK,
				 FIELD_PREP(AW86927_PLAYCFG1_BST_VOUT_VREFSET_MASK,
					    haptics->boost));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap,
				 AW86927_PLAYCFG3_REG,
				 AW86927_PLAYCFG3_AUTO_BST_MASK,
				 FIELD_PREP(AW86927_PLAYCFG3_AUTO_BST_MASK,
					    AW86927_PLAYCFG3_AUTO_BST_DISABLE));
	if (err)
		return err;

	return 0;
}

static int aw86927_load_ram(struct aw86927_data *haptics, const struct firmware *fw)
{
	const u8 *data = fw->data;
	unsigned int base, first, sum = 0;
	size_t i;
	int ret, disable_ret;

	/* Awinic RAM images have checksum, base address, then the RAM bytes. */
	if (fw->size < 9 || fw->size > 8196)
		return -EINVAL;
	for (i = 2; i < fw->size; i++)
		sum += data[i];
	if ((u16)sum != get_unaligned_be16(data))
		return -EBADMSG;
	base = get_unaligned_be16(data + 2);
	first = get_unaligned_be16(data + 5);
	if (base + fw->size - 4 > 8192 || first <= base + 1 ||
	    first >= base + fw->size - 4 || (first - base - 1) % 4 ||
	    (first - base - 1) / 4 > 127)
		return -EINVAL;

	ret = aw86927_stop(haptics);
	if (ret)
		return ret;
	ret = regmap_update_bits(haptics->regmap, AW86927_SYSCTRL3_REG,
				 AW86927_SYSCTRL3_EN_RAMINIT_MASK,
				 AW86927_SYSCTRL3_EN_RAMINIT_MASK);
	if (ret)
		return ret;
	ret = regmap_bulk_write(haptics->regmap, AW86927_BASEADDRH_REG, data + 2, 2);
	if (!ret)
		ret = regmap_bulk_write(haptics->regmap, AW86927_RAMADDRH_REG, data + 2, 2);
	for (i = 4; !ret && i < fw->size; i += 128)
		ret = regmap_noinc_write(haptics->regmap, AW86927_RAMDATA_REG,
					 data + i, min_t(size_t, fw->size - i, 128));
	disable_ret = regmap_update_bits(haptics->regmap, AW86927_SYSCTRL3_REG,
					  AW86927_SYSCTRL3_EN_RAMINIT_MASK, 0);
	if (!ret && !disable_ret)
		haptics->wave_count = (first - base - 1) / 4;
	return ret ?: disable_ret;
}

static int aw86927_prepare_waveforms(struct aw86927_data *haptics)
{
	const struct firmware *fw;
	int ret;

	if (haptics->wave_count)
		return 0;
	/* Load on the first FF upload, after Android has mounted vendor. */
	ret = request_firmware_direct(&fw, AW86927_RAM_FIRMWARE, haptics->dev);
	if (ret)
		return ret;
	ret = aw86927_load_ram(haptics, fw);
	release_firmware(fw);
	return ret;
}

static irqreturn_t aw86927_irq(int irq, void *data)
{
	struct aw86927_data *haptics = data;
	struct device *dev = &haptics->client->dev;
	unsigned int reg_val;
	int err;

	err = regmap_read(haptics->regmap, AW86927_SYSINT_REG, &reg_val);
	if (err) {
		dev_err(dev, "Failed to read SYSINT register: %d\n", err);
		return IRQ_NONE;
	}

	if (reg_val & AW86927_SYSINT_BST_SCPI)
		dev_err(dev, "Received a Short Circuit Protection interrupt\n");
	if (reg_val & AW86927_SYSINT_BST_OVPI)
		dev_err(dev, "Received an Over Voltage Protection interrupt\n");
	if (reg_val & AW86927_SYSINT_UVLI)
		dev_err(dev, "Received an Under Voltage Lock Out interrupt\n");
	if (reg_val & AW86927_SYSINT_OCDI)
		dev_err(dev, "Received an Over Current interrupt\n");
	if (reg_val & AW86927_SYSINT_OTI)
		dev_err(dev, "Received an Over Temperature interrupt\n");

	if (reg_val & AW86927_SYSINT_DONEI)
		dev_dbg(dev, "Chip playback done!\n");
	if (reg_val & AW86927_SYSINT_FF_AFI)
		dev_dbg(dev, "The RTP mode FIFO is almost full!\n");
	if (reg_val & AW86927_SYSINT_FF_AEI)
		dev_dbg(dev, "The RTP mode FIFO is almost empty!\n");

	return IRQ_HANDLED;
}

static int aw86927_detect(struct aw86927_data *haptics)
{
	__be16 read_buf;
	u16 chip_id;
	int err;

	err = regmap_bulk_read(haptics->regmap, AW86927_CHIPIDH_REG, &read_buf, 2);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to read CHIPID registers\n");

	chip_id = be16_to_cpu(read_buf);

	if (chip_id != AW86927_CHIPID) {
		dev_err(haptics->dev, "Unexpected CHIPID value 0x%x\n", chip_id);
		return -ENODEV;
	}

	return 0;
}

static int aw86927_probe(struct i2c_client *client)
{
	struct aw86927_data *haptics;
	struct pinctrl *pinctrl;
	struct pinctrl_state *irq_state;
	struct gpio_desc *irq_gpio;
	u32 boundary, boost;
	int err, irq;

	if (client->addr != 0x5a ||
	    device_property_read_u32(&client->dev, "aw86927_vib_bst_vol_default", &boost) ||
	    device_property_read_u32(&client->dev, "vib_effect_id_boundary", &boundary) ||
	    !boundary || boundary >= 127)
		return -ENODEV;

	haptics = devm_kzalloc(&client->dev, sizeof(struct aw86927_data), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->dev = &client->dev;
	haptics->client = client;
	haptics->boost = min_t(u32, boost, AW86927_ROTHKO_BOOST_MAX);
	haptics->gain = U16_MAX;
	haptics->playing_id = -1;
	haptics->effect_boundary = boundary;
	device_for_each_child_node_scoped(haptics->dev, child) {
		u32 id, rate_us;

		if (!fwnode_property_read_u32(child, "mtk,effect-id", &id) &&
		    id < boundary &&
		    !fwnode_property_read_u32(child, "mtk,wf-play-rate-us", &rate_us))
			haptics->ram_duration_ms[id] = min_t(u32, rate_us / 1000, U16_MAX);
	}

	i2c_set_clientdata(client, haptics);

	haptics->regmap = devm_regmap_init_i2c(client, &aw86927_regmap_config);
	if (IS_ERR(haptics->regmap))
		return dev_err_probe(haptics->dev, PTR_ERR(haptics->regmap),
					"Failed to allocate register map\n");

	haptics->input_dev = devm_input_allocate_device(haptics->dev);
	if (!haptics->input_dev)
		return -ENOMEM;

	haptics->reset_gpio = devm_gpiod_get(haptics->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(haptics->reset_gpio))
		return dev_err_probe(haptics->dev, PTR_ERR(haptics->reset_gpio),
				     "Failed to get reset gpio\n");

	/* Hardware reset */
	aw86927_hw_reset(haptics);

	/* Software reset */
	err = regmap_write(haptics->regmap, AW86927_RSTCFG_REG, AW86927_RSTCFG_SOFTRST);
	if (err)
		return dev_err_probe(haptics->dev, err,	"Failed Software reset\n");

	/* Wait ~3ms until I2C is accessible */
	usleep_range(3000, 3500);

	err = aw86927_detect(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to find chip\n");

	pinctrl = devm_pinctrl_get(haptics->dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(haptics->dev, PTR_ERR(pinctrl), "Awinic pinctrl\n");
	irq_state = pinctrl_lookup_state(pinctrl, "awinic_interrupt_active");
	if (IS_ERR(irq_state))
		return dev_err_probe(haptics->dev, PTR_ERR(irq_state), "Awinic IRQ state\n");
	err = pinctrl_select_state(pinctrl, irq_state);
	if (err)
		return dev_err_probe(haptics->dev, err, "Awinic IRQ pin\n");
	irq_gpio = devm_gpiod_get(haptics->dev, "irq", GPIOD_IN);
	if (IS_ERR(irq_gpio))
		return dev_err_probe(haptics->dev, PTR_ERR(irq_gpio), "Awinic IRQ GPIO\n");
	irq = gpiod_to_irq(irq_gpio);
	if (irq < 0)
		return dev_err_probe(haptics->dev, irq, "Awinic IRQ mapping\n");

	/* IRQ config */
	err = regmap_write(haptics->regmap, AW86927_SYSCTRL4_REG,
			   FIELD_PREP(AW86927_SYSCTRL4_INT_MODE_MASK,
				      AW86927_SYSCTRL4_INT_MODE_EDGE) |
				FIELD_PREP(AW86927_SYSCTRL4_INT_EDGE_MODE_MASK,
					   AW86927_SYSCTRL4_INT_EDGE_MODE_POS));
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to configure interrupt modes\n");

	err = regmap_write(haptics->regmap, AW86927_SYSINTM_REG,
			   AW86927_SYSINTM_BST_OVPM |
				AW86927_SYSINTM_FF_AEM |
				AW86927_SYSINTM_FF_AFM |
				AW86927_SYSINTM_DONEM);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to configure interrupt masks\n");

	err = devm_request_threaded_irq(haptics->dev, irq, NULL, aw86927_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING, NULL, haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to request threaded irq\n");

	INIT_WORK(&haptics->play_work, aw86927_haptics_play_work);
	INIT_DELAYED_WORK(&haptics->stop_work, aw86927_stop_work);
	spin_lock_init(&haptics->effect_lock);

	haptics->input_dev->name = "awinic_haptic";
	haptics->input_dev->close = aw86927_close;

	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);
	input_set_capability(haptics->input_dev, EV_FF, FF_CONSTANT);
	input_set_capability(haptics->input_dev, EV_FF, FF_PERIODIC);
	input_set_capability(haptics->input_dev, EV_FF, FF_CUSTOM);
	input_set_capability(haptics->input_dev, EV_FF, FF_GAIN);

	err = input_ff_create(haptics->input_dev, AW86927_MAX_EFFECTS);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to create FF dev\n");

	haptics->input_dev->ff->upload = aw86927_upload;
	haptics->input_dev->ff->playback = aw86927_playback;
	haptics->input_dev->ff->set_gain = aw86927_set_gain;

	/* Set up registers */
	err = aw86927_play_mode(haptics, AW86927_STANDBY_MODE);
	if (err)
		return dev_err_probe(haptics->dev, err,
				     "Failed to enter standby for Haptic init\n");

	err = aw86927_haptic_init(haptics);
	if (err)
		return dev_err_probe(haptics->dev, err, "Haptic init failed\n");

	err = input_register_device(haptics->input_dev);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to register input device\n");

	return 0;
}

static const struct of_device_id aw86927_of_id[] = {
	{ .compatible = "awinic,awinic_haptic" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, aw86927_of_id);

static struct i2c_driver aw86927_driver = {
	.driver = {
		.name = "awinic_haptic",
		.of_match_table = aw86927_of_id,
	},
	.probe = aw86927_probe,
};

module_i2c_driver(aw86927_driver);

MODULE_AUTHOR("Griffin Kroah-Hartman <griffin.kroah@fairphone.com>");
MODULE_DESCRIPTION("AWINIC AW86927 LRA Haptic Driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(AW86927_RAM_FIRMWARE);
