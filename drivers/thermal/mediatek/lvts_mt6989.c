// SPDX-License-Identifier: GPL-2.0
/*
 * MT6989 LVTS firmware hand-off driver.
 * Controller mapping and calibration based on the MediaTek MT6989
 * soc_temp_lvts driver, Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>

#define MT6989_LVTS_MONINT		0x00c
#define MT6989_LVTS_MONINTSTS		0x010
#define MT6989_LVTS_MSR(slot)		(0x090 + 4 * (slot))
#define MT6989_LVTS_STAGE3		BIT(31)
#define MT6989_LVTS_MSR_VALID		BIT(16)
#define MT6989_LVTS_MSR_RAW		GENMASK(15, 0)
#define MT6989_LVTS_MSR_WAIT_US		200
#define MT6989_LVTS_MSR_TIMEOUT_US	400
#define MT6989_LVTS_NUM_SENSORS		32
#define MT6989_LVTS_SENSORS_PER_CTRL	4
#define MT6989_LVTS_EFUSE_WORDS		32

/* MMIO and IRQ indices in the original mediatek,mt6989-lvts binding. */
enum mt6989_lvts_domain_id {
	MT6989_LVTS_AP,
	MT6989_LVTS_MCU,
	MT6989_LVTS_APU,
	MT6989_LVTS_GPU,
	MT6989_LVTS_NUM_DOMAINS,
};

/* Sensor IDs here are DT arguments. ID 0 is the aggregate; -1 is unused. */
static const struct mt6989_lvts_controller {
	enum mt6989_lvts_domain_id domain;
	u16 offset;
	bool high_temp_calibration;
	s8 sensor_ids[MT6989_LVTS_SENSORS_PER_CTRL];
} mt6989_lvts_controllers[] = {
	{ MT6989_LVTS_MCU, 0x000, true,  {  1,  2,  3,  4 } },
	{ MT6989_LVTS_MCU, 0x100, true,  {  5,  6,  7,  8 } },
	{ MT6989_LVTS_MCU, 0x200, false, {  9, 10, 11, 12 } },
	{ MT6989_LVTS_MCU, 0x300, false, { 13, 14, 15, 16 } },
	{ MT6989_LVTS_AP,  0x000, false, { 25, 26, 27, 28 } },
	{ MT6989_LVTS_AP,  0x100, false, { 29, 30, 31, 32 } },
	{ MT6989_LVTS_APU, 0x000, false, { 17, 18, 19, 20 } },
	/* TS6_0 is disconnected. APU B0 is TS6_1, at slot 1. */
	{ MT6989_LVTS_APU, 0x100, false, { -1, 21, -1, -1 } },
	{ MT6989_LVTS_GPU, 0x000, false, { 22, 23, 24, -1 } },
};

struct mt6989_lvts_sensor {
	void __iomem *msr;
	int slope;
	int offset;
};

struct mt6989_lvts_domain {
	void __iomem *base;
	struct mt6989_lvts *lvts;
	enum mt6989_lvts_domain_id id;
};

struct mt6989_lvts {
	struct mt6989_lvts_sensor sensors[MT6989_LVTS_NUM_SENSORS];
	struct thermal_zone_device *soc_max;
	struct mt6989_lvts_domain domains[MT6989_LVTS_NUM_DOMAINS];
};

static int mt6989_lvts_read_efuse(struct device *dev, u32 *efuse)
{
	static const struct {
		const char *name;
		size_t words;
	} blocks[] = {
		{ "e_data1", 10 }, { "e_data2", 1 }, { "e_data3", 5 },
		{ "e_data4", 5 }, { "e_data5", 11 },
	};
	struct nvmem_cell *cell;
	size_t len;
	void *data;
	int i, j;

	for (i = 0; i < ARRAY_SIZE(blocks); i++) {
		cell = nvmem_cell_get(dev, blocks[i].name);
		if (IS_ERR(cell))
			return dev_err_probe(dev, PTR_ERR(cell), "get %s\n",
					     blocks[i].name);

		data = nvmem_cell_read(cell, &len);
		nvmem_cell_put(cell);
		if (IS_ERR(data))
			return dev_err_probe(dev, PTR_ERR(data), "read %s\n",
					     blocks[i].name);

		if (len != blocks[i].words * sizeof(*efuse)) {
			kfree(data);
			return dev_err_probe(dev, -EINVAL, "bad %s length: %zu\n",
					     blocks[i].name, len);
		}

		for (j = 0; j < blocks[i].words; j++)
			*efuse++ = get_unaligned_le32(data + j * sizeof(*efuse));
		kfree(data);
	}

	return 0;
}

static int mt6989_lvts_init_sensors(struct device *dev, struct mt6989_lvts *lvts)
{
	const struct mt6989_lvts_controller *ctrl;
	u32 efuse[MT6989_LVTS_EFUSE_WORDS];
	struct mt6989_lvts_sensor *sensor;
	int golden_nt, golden_ht, golden;
	unsigned int count_r;
	bool high_temp_mode;
	int i, slot, index, ret;
	void __iomem *base;

	ret = mt6989_lvts_read_efuse(dev, efuse);
	if (ret)
		return ret;

	high_temp_mode = efuse[0] & BIT(31);
	golden_nt = FIELD_GET(GENMASK(7, 0), efuse[0]);
	golden_ht = FIELD_GET(GENMASK(15, 8), efuse[0]);
	if (!golden_nt || (high_temp_mode && !golden_ht))
		return dev_err_probe(dev, -EINVAL, "missing golden temperature\n");

	for (i = 0; i < ARRAY_SIZE(mt6989_lvts_controllers); i++) {
		ctrl = &mt6989_lvts_controllers[i];
		base = lvts->domains[ctrl->domain].base + ctrl->offset;
		golden = high_temp_mode && ctrl->high_temp_calibration ? golden_ht : golden_nt;

		for (slot = 0; slot < MT6989_LVTS_SENSORS_PER_CTRL; slot++) {
			if (ctrl->sensor_ids[slot] < 0)
				continue;

			index = ctrl->sensor_ids[slot] - 1;
			sensor = &lvts->sensors[index];
			count_r = (efuse[1 + index / 2] >> (index & 1 ? 0 : 16)) & 0xffff;
			sensor->slope = (int)(280000ULL * count_r / 14437) - 60040;
			if (sensor->slope <= 0)
				return dev_err_probe(dev, -EINVAL,
						     "invalid calibration for sensor %d\n",
						     index + 1);

			/* SPM has already applied the hardware R/RC calibration. */
			sensor->offset = golden * 500 - sensor->slope;
			sensor->msr = base + MT6989_LVTS_MSR(slot);
		}
	}

	return 0;
}

static int mt6989_lvts_decode_temp(const struct mt6989_lvts_sensor *sensor,
				   u32 value, int *temp)
{
	u32 raw = FIELD_GET(MT6989_LVTS_MSR_RAW, value);
	s64 mc;

	if (value == U32_MAX || !(value & MT6989_LVTS_MSR_VALID) || !raw)
		return -EAGAIN;

	mc = div_s64((s64)sensor->slope << 14, raw) + sensor->offset;
	if (mc > INT_MAX)
		return -EAGAIN;

	*temp = mc;
	return 0;
}

static int mt6989_lvts_get_temp(struct thermal_zone_device *tz, int *temp)
{
	const struct mt6989_lvts_sensor *sensor = thermal_zone_device_priv(tz);
	u32 value;
	int ret;

	/* A hand-off channel may be between valid samples. */
	ret = readl_poll_timeout(sensor->msr, value, value != U32_MAX &&
				 (value & MT6989_LVTS_MSR_VALID) &&
				 (value & MT6989_LVTS_MSR_RAW),
				 MT6989_LVTS_MSR_WAIT_US, MT6989_LVTS_MSR_TIMEOUT_US);
	if (ret)
		return -EAGAIN;

	return mt6989_lvts_decode_temp(sensor, value, temp);
}

static int mt6989_lvts_get_max_temp(struct thermal_zone_device *tz, int *temp)
{
	struct mt6989_lvts *lvts = thermal_zone_device_priv(tz);
	int i, sample, maximum = INT_MIN;
	const struct mt6989_lvts_sensor *sensor;

	/* Unpowered domains must not stall the aggregate safety reading. */
	for (i = 0; i < ARRAY_SIZE(lvts->sensors); i++) {
		sensor = &lvts->sensors[i];
		if (!mt6989_lvts_decode_temp(sensor, readl(sensor->msr), &sample))
			maximum = max(maximum, sample);
	}

	if (maximum == INT_MIN)
		return -EAGAIN;

	*temp = maximum;
	return 0;
}

static const struct thermal_zone_device_ops mt6989_lvts_sensor_ops = {
	.get_temp = mt6989_lvts_get_temp,
};

static const struct thermal_zone_device_ops mt6989_lvts_max_ops = {
	.get_temp = mt6989_lvts_get_max_temp,
};

static irqreturn_t mt6989_lvts_irq(int irq, void *data)
{
	const struct mt6989_lvts_domain *domain = data;
	const struct mt6989_lvts_controller *ctrl;
	struct thermal_zone_device *soc_max = domain->lvts->soc_max;
	bool stage3 = false;
	void __iomem *base;
	u32 status;
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6989_lvts_controllers); i++) {
		ctrl = &mt6989_lvts_controllers[i];
		if (ctrl->domain != domain->id)
			continue;

		base = domain->base + ctrl->offset;
		status = readl(base + MT6989_LVTS_MONINTSTS);
		if (status == U32_MAX || !(status & MT6989_LVTS_STAGE3) ||
		    !(readl(base + MT6989_LVTS_MONINT) & MT6989_LVTS_STAGE3))
			continue;

		/* The SCP owns the other thresholds. Acknowledge only stage 3. */
		writel(MT6989_LVTS_STAGE3, base + MT6989_LVTS_MONINTSTS);
		stage3 = true;
	}

	if (!stage3)
		return IRQ_NONE;

	/* Stage 3 itself is critical, even if a later sample has cooled. */
	thermal_zone_device_critical(soc_max);
	return IRQ_HANDLED;
}

static int mt6989_lvts_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct mt6989_lvts *lvts;
	int irq[MT6989_LVTS_NUM_DOMAINS];
	int i, ret;

	lvts = devm_kzalloc(dev, sizeof(*lvts), GFP_KERNEL);
	if (!lvts)
		return -ENOMEM;

	/* SPM owns initialization, clocks and resets across system sleep. */
	for (i = 0; i < MT6989_LVTS_NUM_DOMAINS; i++) {
		lvts->domains[i].base = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(lvts->domains[i].base))
			return PTR_ERR(lvts->domains[i].base);

		lvts->domains[i].lvts = lvts;
		lvts->domains[i].id = i;
		irq[i] = platform_get_irq(pdev, i);
		if (irq[i] < 0)
			return irq[i];
	}

	ret = mt6989_lvts_init_sensors(dev, lvts);
	if (ret)
		return ret;

	lvts->soc_max = devm_thermal_of_zone_register(dev, 0, lvts, &mt6989_lvts_max_ops);
	if (IS_ERR(lvts->soc_max))
		return dev_err_probe(dev, PTR_ERR(lvts->soc_max), "register SoC maximum zone\n");

	for (i = 0; i < ARRAY_SIZE(lvts->sensors); i++) {
		tz = devm_thermal_of_zone_register(dev, i + 1, &lvts->sensors[i],
						  &mt6989_lvts_sensor_ops);
		if (IS_ERR(tz) && PTR_ERR(tz) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(tz), "register sensor %d\n", i + 1);
	}

	for (i = 0; i < MT6989_LVTS_NUM_DOMAINS; i++) {
		ret = devm_request_threaded_irq(dev, irq[i], NULL, mt6989_lvts_irq,
					       IRQF_ONESHOT, dev_name(dev), &lvts->domains[i]);
		if (ret)
			return dev_err_probe(dev, ret, "request domain %d IRQ\n", i);
	}

	return 0;
}

static const struct of_device_id mt6989_lvts_of_match[] = {
	{ .compatible = "mediatek,mt6989-lvts" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6989_lvts_of_match);

static struct platform_driver mt6989_lvts_driver = {
	.probe = mt6989_lvts_probe,
	.driver = {
		.name = "mt6989-lvts",
		.of_match_table = mt6989_lvts_of_match,
	},
};
module_platform_driver(mt6989_lvts_driver);

MODULE_DESCRIPTION("MediaTek MT6989 LVTS thermal driver");
MODULE_LICENSE("GPL");
