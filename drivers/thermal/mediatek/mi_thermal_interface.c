// SPDX-License-Identifier: GPL-2.0
/* Xiaomi thermal message ABI for the thermal daemon and Android framework. */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/sysfs.h>

#include "../thermal_core.h"

#define MESSAGE_SIZE	128

static DEFINE_MUTEX(message_lock);
static const char *board_sensor = "invalid";
static char boost[MESSAGE_SIZE];
static char board_sensor_temp[MESSAGE_SIZE];

#define THERMAL_MESSAGE_INT(_name, _initial)				\
static int _name = (_initial);						\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%d\n", READ_ONCE(_name));			\
}									\
static ssize_t _name##_store(struct device *dev,				\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	int ret, value;							\
									\
	ret = kstrtoint(buf, 10, &value);				\
	if (ret)							\
		return ret;						\
	WRITE_ONCE(_name, value);					\
	return count;							\
}									\
static DEVICE_ATTR(_name, 0664, _name##_show, _name##_store)

THERMAL_MESSAGE_INT(temp_state, 0);
/* Charging scene forwarding needs the Xiaomi charger policy backend. */
THERMAL_MESSAGE_INT(sconfig, -1);
THERMAL_MESSAGE_INT(video_mode, -1);
THERMAL_MESSAGE_INT(wifi_limit, 0);
THERMAL_MESSAGE_INT(torch_level, 0);
THERMAL_MESSAGE_INT(torch_real_level, -1);

static ssize_t message_show(char *buf, const char *message)
{
	ssize_t len;

	mutex_lock(&message_lock);
	len = sysfs_emit(buf, "%s", message);
	mutex_unlock(&message_lock);

	return len;
}

static ssize_t message_store(char *message, const char *buf, size_t count)
{
	size_t len = min_t(size_t, count, MESSAGE_SIZE - 1);

	mutex_lock(&message_lock);
	memcpy(message, buf, len);
	message[len] = '\0';
	mutex_unlock(&message_lock);

	return count;
}

static ssize_t boost_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return message_show(buf, boost);
}

static ssize_t boost_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	return message_store(boost, buf, count);
}
static DEVICE_ATTR_RW(boost);

static ssize_t board_sensor_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor);
}
static DEVICE_ATTR(board_sensor, 0664, board_sensor_show, NULL);

static ssize_t board_sensor_temp_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return message_show(buf, board_sensor_temp);
}

static ssize_t board_sensor_temp_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	/* The thermal daemon supplies this reading for other userspace clients. */
	return message_store(board_sensor_temp, buf, count);
}
static DEVICE_ATTR(board_sensor_temp, 0664, board_sensor_temp_show,
		   board_sensor_temp_store);

static struct attribute *mi_thermal_attrs[] = {
	&dev_attr_temp_state.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_boost.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_video_mode.attr,
	&dev_attr_wifi_limit.attr,
	&dev_attr_torch_level.attr,
	&dev_attr_torch_real_level.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mi_thermal);

static int __init mi_thermal_message_init(void)
{
	struct device_node *np;

	np = of_find_node_by_path("/odm/thermal-message");
	if (!np)
		np = of_find_node_by_path("/soc/thermal-message");
	if (!np)
		return 0;

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}

	of_property_read_string(np, "board-sensor", &board_sensor);
	of_node_put(np);

	return PTR_ERR_OR_ZERO(thermal_class_device_register("thermal_message", NULL,
							    mi_thermal_groups));
}
device_initcall(mi_thermal_message_init);
