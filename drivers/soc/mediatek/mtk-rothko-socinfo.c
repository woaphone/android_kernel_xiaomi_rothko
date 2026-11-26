// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/sys_soc.h>

static struct soc_device_attribute rothko_soc_attrs = {
	/* The stock Android audio framework matches this exact string. */
	.family = "Mediatek",
};

static int __init rothko_socinfo_init(void)
{
	struct device_node *root;
	int ret;

	if (!of_machine_is_compatible("mediatek,MT6989"))
		return 0;

	root = of_find_node_by_path("/");
	ret = of_property_read_string(root, "model", &rothko_soc_attrs.soc_id);
	of_node_put(root);
	if (ret)
		return ret;

	return PTR_ERR_OR_ZERO(soc_device_register(&rothko_soc_attrs));
}
device_initcall(rothko_socinfo_init);
