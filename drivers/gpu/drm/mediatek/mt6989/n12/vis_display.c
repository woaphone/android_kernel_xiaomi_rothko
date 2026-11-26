// SPDX-License-Identifier: GPL-2.0
/* Novatek VIS panel notifications and SPI transport recovered from A15.
 * Firmware loading, PQ/UseCase and OEM ioctl consumers are separate recovery
 * work; this file does not claim a firmware usecase was successfully started.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/mt6685-335clk.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/unaligned.h>
#include "vis_display.h"

#define VIS_SPI_WRITE_CHUNK 24576
#define VIS_SPI_RATE 24000000
#define VIS_DSI_UNINITIALIZED ((int)0xdeadfffc)

struct n12_vis {
	struct spi_device *spi;
	struct spi_mem mem;
	struct gpio_desc *always08;
	struct gpio_desc *always18;
	struct gpio_desc *reset;
	struct gpio_desc *ready;
	u8 *tx;
	u8 *rx;
	u8 *write_data;
	u32 clock_rate;
	u32 panel_fps;
	u32 dsi_rate;
	bool powered;
};

static DEFINE_MUTEX(vis_lock);
static struct n12_vis *vis;
static unsigned int (*panel_id_callback)(void);
static int (*osc_callback)(unsigned int setting);

bool is_mi_dev_support_nova(void)
{
	const char *product = product_name_get();

	return product && !get_hw_country_version() && !strcmp(product, "rothko");
}
EXPORT_SYMBOL_GPL(is_mi_dev_support_nova);

void nvt_get_lcm_id_notify(unsigned int (*get_id)(void))
{
	mutex_lock(&vis_lock);
	panel_id_callback = get_id;
	mutex_unlock(&vis_lock);
}
EXPORT_SYMBOL_GPL(nvt_get_lcm_id_notify);

void vis_display_ops_osc_tricking_notify(int (*osc_tricking)(unsigned int setting))
{
	mutex_lock(&vis_lock);
	osc_callback = osc_tricking;
	mutex_unlock(&vis_lock);
}
EXPORT_SYMBOL_GPL(vis_display_ops_osc_tricking_notify);

int vis_fps_notify(unsigned int fps)
{
	int ret = 0;

	mutex_lock(&vis_lock);
	if (!vis)
		ret = VIS_DSI_UNINITIALIZED;
	else
		vis->panel_fps = fps;
	mutex_unlock(&vis_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vis_fps_notify);

int vis_dsi_rate_notify(unsigned int rate)
{
	int ret = 0;

	mutex_lock(&vis_lock);
	if (!vis)
		ret = VIS_DSI_UNINITIALIZED;
	else
		vis->dsi_rate = rate;
	mutex_unlock(&vis_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vis_dsi_rate_notify);

/* spiDataTranfer uses one full-duplex transfer of max(txlen, rxlen).
 * DMA-safe buffers replace the original stack-buffer transfer, and actual
 * transport errors are returned rather than reporting boolean success.
 */
static int vis_transfer(struct n12_vis *ctx, unsigned int length, bool read)
{
	struct spi_transfer transfer = {
		.tx_buf = ctx->tx,
		.rx_buf = read ? ctx->rx : NULL,
		.len = length,
		.speed_hz = ctx->clock_rate,
		.bits_per_word = 8,
		.tx_nbits = SPI_NBITS_SINGLE,
		.rx_nbits = SPI_NBITS_SINGLE,
	};

	return spi_sync_transfer(ctx->spi, &transfer, 1);
}

static int vis_read_spi_status(struct n12_vis *ctx, u8 *status)
{
	int ret;

	memset(ctx->tx, 0, 4);
	ctx->tx[0] = 0x35;
	ret = vis_transfer(ctx, 4, true);
	if (!ret)
		*status = ctx->rx[1];
	return ret;
}

/* setDeviceReady335: read 35, clear/unlock with 06 + 01 80 if required,
 * then wait up to the original 500 x 1000..1100 us for status bits 0x81.
 */
static int vis_ready335(struct n12_vis *ctx)
{
	u8 status;
	unsigned int i;
	int ret;

	ret = vis_read_spi_status(ctx, &status);
	if (ret)
		return ret;
	if (status == 0x83)
		return 0;
	if ((status & 0x7c) || !(status & 2)) {
		memset(ctx->tx, 0, 4);
		ctx->tx[0] = 0x06;
		ret = vis_transfer(ctx, 4, false);
		if (ret)
			return ret;
		ctx->tx[0] = 0x01;
		ctx->tx[1] = 0x80;
		ret = vis_transfer(ctx, 4, false);
		if (ret)
			return ret;
	}
	for (i = 0; i < 500; i++) {
		ret = vis_read_spi_status(ctx, &status);
		if (ret)
			return ret;
		if ((status & 0x81) == 0x81)
			return 0;
		usleep_range(1000, 1100);
	}
	dev_err(&ctx->spi->dev, "VIS 335 SPI handshake timed out, status %02x\n", status);
	return -ETIMEDOUT;
}

/* getDeviceRegister uses 43 + BE address, then 44; LE result at rx+1. */
static int vis_read_register(struct n12_vis *ctx, u32 address, u32 *value)
{
	int ret;

	ret = vis_ready335(ctx);
	if (ret)
		return ret;
	memset(ctx->tx, 0, 8);
	ctx->tx[0] = 0x43;
	put_unaligned_be32(address, ctx->tx + 1);
	ret = vis_transfer(ctx, 8, false);
	if (ret)
		return ret;
	ret = vis_ready335(ctx);
	if (ret)
		return ret;
	memset(ctx->tx, 0, 8);
	ctx->tx[0] = 0x44;
	ret = vis_transfer(ctx, 8, true);
	if (!ret)
		*value = get_unaligned_le32(ctx->rx + 1);
	return ret;
}

/* A transport probe is not digital READY: stock bypass holds resetN low. */
static int vis_check_reset(struct n12_vis *ctx)
{
	int resetn;

	if (!ctx)
		return -ENODEV;
	resetn = gpiod_get_raw_value_cansleep(ctx->reset);
	if (resetn < 0)
		return resetn;
	return resetn ? 0 : -EHOSTDOWN;
}

int n12_vis_read_register(u32 address, u32 *value)
{
	int ret;

	mutex_lock(&vis_lock);
	ret = vis_check_reset(vis);
	if (!ret)
		ret = vis_read_register(vis, address, value);
	mutex_unlock(&vis_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(n12_vis_read_register);

/* Default stock status +0x17c=0x27, +0x180=3, +0x184=0, +0x188=2:
 * memory writes use opcode 45, 4 address bytes on one wire and dual data.
 * This is the incrementing memory operation, not the FIFO operation.
 */
static int vis_write_memory(struct n12_vis *ctx, u32 address,
			    const void *data, size_t length)
{
	struct spi_mem_op op = SPI_MEM_OP(
		SPI_MEM_OP_CMD(0x45, 1), SPI_MEM_OP_ADDR(4, address, 1),
		SPI_MEM_OP_NO_DUMMY, SPI_MEM_OP_DATA_OUT(0, ctx->write_data, 2));
	const u8 *bytes = data;
	int ret;

	op.max_freq = ctx->clock_rate;
	while (length) {
		op.addr.val = address;
		op.data.nbytes = min_t(size_t, length, VIS_SPI_WRITE_CHUNK);
		ret = spi_mem_adjust_op_size(&ctx->mem, &op);
		if (ret)
			return ret;
		if (!op.data.nbytes)
			return -EIO;
		memcpy(ctx->write_data, bytes, op.data.nbytes);
		ret = vis_ready335(ctx);
		if (ret)
			return ret;
		ret = spi_mem_exec_op(&ctx->mem, &op);
		if (ret)
			return ret;
		bytes += op.data.nbytes;
		length -= op.data.nbytes;
		address += op.data.nbytes;
	}
	return 0;
}

int n12_vis_write_memory(u32 address, const void *data, size_t length)
{
	int ret;

	mutex_lock(&vis_lock);
	ret = vis_check_reset(vis);
	if (!ret)
		ret = vis_write_memory(vis, address, data, length);
	mutex_unlock(&vis_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(n12_vis_write_memory);

int n12_vis_write_register(u32 address, u32 value)
{
	__le32 payload = cpu_to_le32(value);

	return n12_vis_write_memory(address, &payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(n12_vis_write_register);

/* Stock power-on establishes analog bypass without releasing resetN.
 * GPIO 0/1 are always08/always18; the optional always12 rail is unused on N12.
 */
int n12_vis_panel_power(bool enable)
{
	struct n12_vis *ctx;
	int ret = 0;

	mutex_lock(&vis_lock);
	ctx = vis;
	if (!ctx) {
		ret = VIS_DSI_UNINITIALIZED;
		goto out;
	}
	if (ctx->powered == enable)
		goto out;

	if (enable) {
		gpiod_set_raw_value_cansleep(ctx->always08, 1);
		usleep_range(4500, 4600);
		gpiod_set_raw_value_cansleep(ctx->always18, 1);
		usleep_range(12000, 12100);
		pinctrl_pm_select_default_state(&ctx->spi->dev);
		mt6685_set_335_dcxo(true);
		usleep_range(10000, 10100);
		mt6685_set_335_dcxo(false);
		usleep_range(2000, 2100);
	} else {
		gpiod_set_raw_value_cansleep(ctx->reset, 0);
		usleep_range(10000, 11000);
		mt6685_set_335_dcxo(false);
		usleep_range(2500, 3000);
		pinctrl_pm_select_sleep_state(&ctx->spi->dev);
		gpiod_set_raw_value_cansleep(ctx->always18, 0);
		usleep_range(10000, 11000);
		gpiod_set_raw_value_cansleep(ctx->always08, 0);
		usleep_range(10000, 11000);
	}
	ctx->powered = enable;
out:
	mutex_unlock(&vis_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(n12_vis_panel_power);

static int vis_get_gpio(struct spi_device *spi, const char *property,
			struct gpio_desc **gpio, enum gpiod_flags flags)
{
	int number, ret;
	unsigned long gpio_flags;

	/* Stock properties have no -gpios suffix; keep their real OF lookup. */
	number = of_get_named_gpio(spi->dev.of_node, property, 0);
	if (number < 0)
		return number;
	gpio_flags = flags == GPIOD_IN ? GPIOF_IN :
		(flags == GPIOD_OUT_HIGH ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW);
	ret = devm_gpio_request_one(&spi->dev, number, gpio_flags, property);
	if (ret)
		return ret;
	*gpio = gpio_to_desc(number);
	return 0;
}

static int n12_vis_probe(struct spi_device *spi)
{
	struct n12_vis *ctx;
	int ret;

	/* Stock loads 335CLK before VIS; builtin probes can reach VIS first. */
	if (!mt6685_335clk_ready())
		return -EPROBE_DEFER;

	ctx = devm_kzalloc(&spi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->spi = spi;
	ctx->mem.spi = spi;
	ctx->mem.name = "novatek-vis";
	ctx->clock_rate = VIS_SPI_RATE;
	of_property_read_u32(spi->dev.of_node, "spi-max-frequency", &ctx->clock_rate);
	ctx->panel_fps = 120;
	ctx->tx = devm_kzalloc(&spi->dev, 12, GFP_KERNEL);
	ctx->rx = devm_kzalloc(&spi->dev, 8, GFP_KERNEL);
	ctx->write_data = devm_kmalloc(&spi->dev, VIS_SPI_WRITE_CHUNK, GFP_KERNEL);
	if (!ctx->tx || !ctx->rx || !ctx->write_data)
		return -ENOMEM;
	spi->bits_per_word = 8;
	/* Registers use one wire; the recovered SPI-MEM write uses dual TX.
	 * Requesting both DUAL and QUAD for each direction fails spi_setup().
	 */
	spi->mode = SPI_TX_DUAL;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "VIS SPI setup\n");

	/* Stock vis_gpio_init and the analog-bypass semi-power-off state
	 * keep both analog rails on and hold the digital ASIC in reset.
	 */
	ret = vis_get_gpio(spi, "nvt,always08", &ctx->always08, GPIOD_OUT_HIGH);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "VIS always08 GPIO\n");
	ret = vis_get_gpio(spi, "nvt,always18", &ctx->always18, GPIOD_OUT_HIGH);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "VIS always18 GPIO\n");
	ret = vis_get_gpio(spi, "nvt,resetN", &ctx->reset, GPIOD_OUT_LOW);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "VIS resetN GPIO\n");
	ret = vis_get_gpio(spi, "nvt,ready", &ctx->ready, GPIOD_IN);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "VIS ready GPIO\n");
	mt6685_set_335_dcxo_mode(0);
	/* Stock host probe starts in the powered analog-bypass state (1). */
	ctx->powered = true;
	spi_set_drvdata(spi, ctx);
	mutex_lock(&vis_lock);
	vis = ctx;
	mutex_unlock(&vis_lock);
	dev_info(&spi->dev, "VIS analog bypass configured (always08/18 on, resetN low)\n");
	return 0;
}

static void n12_vis_remove(struct spi_device *spi)
{
	mutex_lock(&vis_lock);
	vis = NULL;
	mutex_unlock(&vis_lock);
}

static const struct of_device_id n12_vis_of_match[] = {
	{ .compatible = "novatek,vis-display" }, { }
};
MODULE_DEVICE_TABLE(of, n12_vis_of_match);
static struct spi_driver n12_vis_driver = {
	.driver = {
		.name = "vis-display", .of_match_table = n12_vis_of_match,
	},
	.probe = n12_vis_probe, .remove = n12_vis_remove,
};

static int __init n12_vis_init(void)
{
	/* Original vis_display_host_init does not register for unsupported HWIDs. */
	if (!is_mi_dev_support_nova())
		return 0;
	return spi_register_driver(&n12_vis_driver);
}
module_init(n12_vis_init);

static void __exit n12_vis_exit(void)
{
	if (is_mi_dev_support_nova())
		spi_unregister_driver(&n12_vis_driver);
}
module_exit(n12_vis_exit);
MODULE_DESCRIPTION("Recovered Novatek VIS SPI transport and N12 panel notifications");
MODULE_LICENSE("GPL");
