// SPDX-License-Identifier: GPL-2.0-only
/* Hardware identity supplied by the rothko bootloader. */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rothko_hwid.h>

static unsigned int hwid_value;
static char *project_name;

/* Preserve the stock hwid.hwid_value and hwid.project_name boot arguments. */
module_param(hwid_value, uint, 0);
module_param(project_name, charp, 0);

int get_hw_country_version(void)
{
	return hwid_value >> 20;
}
EXPORT_SYMBOL_GPL(get_hw_country_version);

const char *product_name_get(void)
{
	return project_name;
}
EXPORT_SYMBOL_GPL(product_name_get);

MODULE_LICENSE("GPL");
