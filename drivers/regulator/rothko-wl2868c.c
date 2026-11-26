// SPDX-License-Identifier: GPL-2.0-only
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define WL2868C_I2C_ADDR	0x2f
#define WL2868C_PRODUCT_ID	0x00
#define WL2868C_ID		0x82
#define WL2868C_VSEL_BASE	0x03
#define WL2868C_ENABLE		0x0e
#define WL2868C_LDO4_TRIM	0x17
#define WL2868C_LDO4_TRIM_VALUE	0xb0
#define WL2868C_LDO_COUNT	7
#define WL2868C_STEP_UV		8000

static const struct regmap_config wl2868c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x25,
};

static const struct regulator_ops wl2868c_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
};

static int wl2868c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct i2c_client *wl_client;
	struct regulator_desc *descs;
	struct regmap *regmap;
	unsigned int id, seen = 0;
	int ret;

	/*
	 * The factory DT uses 0x35 for the shared LDO binding, but the
	 * factory WL2868C driver transfers at 0x2f. Keep the DT client for
	 * the regulator phandles and reserve the actual address.
	 */
	wl_client = devm_i2c_new_dummy_device(dev, client->adapter, WL2868C_I2C_ADDR);
	if (IS_ERR(wl_client))
		return dev_err_probe(dev, PTR_ERR(wl_client), "Failed to reserve WL2868C address\n");

	regmap = devm_regmap_init_i2c(wl_client, &wl2868c_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = regmap_read(regmap, WL2868C_PRODUCT_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read WL2868C product ID at %#x\n",
				     wl_client->addr);
	if (id != WL2868C_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected WL2868C product ID %#x\n", id);

	descs = devm_kcalloc(dev, WL2868C_LDO_COUNT, sizeof(*descs), GFP_KERNEL);
	if (!descs)
		return -ENOMEM;

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		struct regulator_config config = { .dev = dev, .regmap = regmap };
		struct regulator_desc *desc;
		struct regulator_dev *rdev;
		u32 initial_uv, trim;
		u16 offset;

		ret = of_property_read_u16(child, "offset", &offset);
		if (ret || offset >= WL2868C_LDO_COUNT || (seen & BIT(offset)))
			return dev_err_probe(dev, -EINVAL, "Invalid LDO offset in %pOF\n", child);
		seen |= BIT(offset);
		desc = &descs[offset];
		ret = of_property_read_string(child, "regulator-name", &desc->name);
		if (ret)
			return ret;
		desc->id = offset;
		desc->type = REGULATOR_VOLTAGE;
		desc->owner = THIS_MODULE;
		desc->ops = &wl2868c_ops;
		desc->min_uV = offset < 2 ? 496000 : 1504000;
		desc->uV_step = WL2868C_STEP_UV;
		desc->n_voltages = 256;
		desc->enable_time = 150;
		desc->enable_reg = WL2868C_ENABLE;
		desc->enable_mask = BIT(offset);
		desc->vsel_reg = WL2868C_VSEL_BASE + offset;
		desc->vsel_mask = 0xff;

		config.of_node = child;
		config.init_data = of_get_regulator_init_data(dev, child, desc);
		if (!config.init_data)
			return -ENOMEM;

		if (!of_property_read_u32(child, "init-voltage", &initial_uv)) {
			const struct regulation_constraints *c = &config.init_data->constraints;

			if (initial_uv < c->min_uV || initial_uv > c->max_uV ||
			    initial_uv < desc->min_uV ||
			    (initial_uv - desc->min_uV) / WL2868C_STEP_UV >= desc->n_voltages)
				return dev_err_probe(dev, -EINVAL, "Invalid initial voltage for %pOF\n", child);
			ret = regmap_write(regmap, desc->vsel_reg,
					   (initial_uv - desc->min_uV) / WL2868C_STEP_UV);
			if (ret)
				return ret;
		}

		if (offset == 3 && !of_property_read_u32(child, "mi_adjust_reg", &trim) &&
		    trim == WL2868C_LDO4_TRIM) {
			ret = regmap_write(regmap, trim, WL2868C_LDO4_TRIM_VALUE);
			if (ret)
				return ret;
		}

		rdev = devm_regulator_register(dev, desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "Failed to register %pOF\n", child);
	}

	return 0;
}

static const struct of_device_id wl2868c_of_match[] = {
	{ .compatible = "onsemi,fan53870" },
	{ }
};
MODULE_DEVICE_TABLE(of, wl2868c_of_match);

static struct i2c_driver wl2868c_driver = {
	.driver = {
		.name = "rothko-wl2868c",
		.of_match_table = wl2868c_of_match,
	},
	.probe = wl2868c_probe,
};
module_i2c_driver(wl2868c_driver);

MODULE_DESCRIPTION("WL2868C camera regulators for the Xiaomi factory DT");
MODULE_LICENSE("GPL");
