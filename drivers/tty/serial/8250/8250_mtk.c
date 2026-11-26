// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek 8250 driver.
 *
 * Copyright (c) 2014 MundoReader S.L.
 * Author: Matthias Brugger <matthias.bgg@gmail.com>
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/serial_8250.h>
#include <linux/serial_reg.h>
#include <linux/console.h>
#include <linux/dma-mapping.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include "8250.h"

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
#include "../../../net/wireless/mediatek/rothko-conninfra/uarthub/common/uarthub_drv_export.h"
#endif

#define MTK_UART_HIGHS		0x09	/* Highspeed register */
#define MTK_UART_SAMPLE_COUNT	0x0a	/* Sample count register */
#define MTK_UART_SAMPLE_POINT	0x0b	/* Sample point register */
#define MTK_UART_RATE_FIX	0x0d	/* UART Rate Fix Register */
#define MTK_UART_ESCAPE_DAT	0x10	/* Escape Character register */
#define MTK_UART_ESCAPE_EN	0x11	/* Escape Enable register */
#define MTK_UART_DMA_EN		0x13	/* DMA Enable register */
#define MTK_UART_RXTRI_AD	0x14	/* RX Trigger address */
#define MTK_UART_FRACDIV_L	0x15	/* Fractional divider LSB address */
#define MTK_UART_FRACDIV_M	0x16	/* Fractional divider MSB address */
#define MTK_UART_DEBUG0	0x18
#define MTK_UART_IER_XOFFI	0x20	/* Enable XOFF character interrupt */
#define MTK_UART_IER_RTSI	0x40	/* Enable RTS Modem status interrupt */
#define MTK_UART_IER_CTSI	0x80	/* Enable CTS Modem status interrupt */

#define MTK_UART_RXTRI_AD	0x14
#define MTK_UART_FCR_RD		0x17
#define MTK_UART_DLL		0x24
#define MTK_UART_DLH		0x25
#define MTK_UART_FEATURE_SEL	0x27
#define MTK_UART_HUB_BAUD	12000000
#define MTK_UART_HUB_RXTRIG	0x19

#define MTK_UART_EFR		38	/* I/O: Extended Features Register */
#define MTK_UART_EFR_EN		0x10	/* Enable enhancement feature */
#define MTK_UART_EFR_RTS	0x40	/* Enable hardware rx flow control */
#define MTK_UART_EFR_CTS	0x80	/* Enable hardware tx flow control */
#define MTK_UART_EFR_NO_SW_FC	0x0	/* no sw flow control */
#define MTK_UART_EFR_XON1_XOFF1	0xa	/* XON1/XOFF1 as sw flow control */
#define MTK_UART_EFR_XON2_XOFF2	0x5	/* XON2/XOFF2 as sw flow control */
#define MTK_UART_EFR_SW_FC_MASK	0xf	/* Enable CTS Modem status interrupt */
#define MTK_UART_EFR_HW_FC	(MTK_UART_EFR_RTS | MTK_UART_EFR_CTS)
#define MTK_UART_DMA_EN_TX	0x2
#define MTK_UART_DMA_EN_RX	0x5

#define MTK_UART_ESCAPE_CHAR	0x77	/* Escape char added under sw fc */
#define MTK_UART_HUB_ESCAPE_CHAR	0xdb
#define MTK_UART_RX_SIZE	0x8000
#define MTK_UART_TX_TRIGGER	1
#define MTK_UART_RX_TRIGGER	MTK_UART_RX_SIZE

#define MTK_UART_XON1		40	/* I/O: Xon character 1 */
#define MTK_UART_XON2		41	/* I/O: Xon character 2 */
#define MTK_UART_XOFF1		42	/* I/O: Xoff character 1 */
#define MTK_UART_XOFF2		43	/* I/O: Xoff character 2 */

#ifdef CONFIG_SERIAL_8250_DMA
enum dma_rx_status {
	DMA_RX_START = 0,
	DMA_RX_RUNNING = 1,
	DMA_RX_SHUTDOWN = 2,
};
#endif

struct mtk8250_data {
	int			line;
	unsigned int		rx_pos;
	unsigned int		clk_count;
	struct clk		*uart_clk;
	struct clk		*bus_clk;
	struct uart_8250_dma	*dma;
#ifdef CONFIG_SERIAL_8250_DMA
	enum dma_rx_status	rx_status;
#endif
	int			rx_wakeup_irq;
#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	u32			hub_baud;
	void __iomem		*hub_clk_reg;
	u32			hub_clk_mask;
	u32			hub_clk_value;
	void __iomem		*hub_reset_reg;
	u32			hub_reset_bit;
#endif
};

/* flow control mode */
enum {
	MTK_UART_FC_NONE,
	MTK_UART_FC_SW,
	MTK_UART_FC_HW,
};

#ifdef CONFIG_SERIAL_8250_DMA
static void mtk8250_rx_dma(struct uart_8250_port *up);

static void mtk8250_dma_rx_complete(void *param, const struct dmaengine_result *result)
{
	struct uart_8250_port *up = param;
	struct uart_8250_dma *dma = up->dma;
	struct mtk8250_data *data = up->port.private_data;
	struct tty_port *tty_port = &up->port.state->port;
	struct dma_tx_state state;
	int copied, total, cnt;
	unsigned char *ptr;
	unsigned long flags;

	uart_port_lock_irqsave(&up->port, &flags);
	if (data->rx_status == DMA_RX_SHUTDOWN) {
		uart_port_unlock_irqrestore(&up->port, flags);
		return;
	}

	if ((result && result->result != DMA_TRANS_NOERROR) ||
	    dmaengine_tx_status(dma->rxchan, dma->rx_cookie, &state) == DMA_ERROR ||
	    state.residue > dma->rx_size) {
		struct tty_struct *tty = tty_port->tty;

		data->rx_status = DMA_RX_SHUTDOWN;
		dev_err_ratelimited(up->port.dev, "UART RX DMA failed; reopen required\n");
		if (tty) {
			set_bit(TTY_IO_ERROR, &tty->flags);
			wake_up_interruptible(&tty->read_wait);
		}
		uart_write_wakeup(&up->port);
		uart_port_unlock_irqrestore(&up->port, flags);
		return;
	}
	total = dma->rx_size - state.residue;
	cnt = total;

	if ((data->rx_pos + cnt) > dma->rx_size)
		cnt = dma->rx_size - data->rx_pos;

	ptr = (unsigned char *)(data->rx_pos + dma->rx_buf);
	copied = tty_insert_flip_string(tty_port, ptr, cnt);
	data->rx_pos += cnt;

	if (total > cnt) {
		ptr = (unsigned char *)(dma->rx_buf);
		cnt = total - cnt;
		copied += tty_insert_flip_string(tty_port, ptr, cnt);
		data->rx_pos = cnt;
	}

	up->port.icount.rx += copied;

	tty_flip_buffer_push(tty_port);

	mtk8250_rx_dma(up);

	uart_port_unlock_irqrestore(&up->port, flags);
}

static void mtk8250_rx_dma(struct uart_8250_port *up)
{
	struct uart_8250_dma *dma = up->dma;
	struct dma_async_tx_descriptor	*desc;

	desc = dmaengine_prep_slave_single(dma->rxchan, dma->rx_addr,
					   dma->rx_size, DMA_DEV_TO_MEM,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		pr_err("failed to prepare rx slave single\n");
		return;
	}

	desc->callback_result = mtk8250_dma_rx_complete;
	desc->callback_param = up;

	dma->rx_cookie = dmaengine_submit(desc);

	dma_async_issue_pending(dma->rxchan);
}

static void mtk8250_dma_enable(struct uart_8250_port *up)
{
	struct uart_8250_dma *dma = up->dma;
	struct mtk8250_data *data = up->port.private_data;
	int lcr = serial_in(up, UART_LCR);

	if (data->rx_status != DMA_RX_START)
		return;

	dma->rxconf.src_port_window_size	= dma->rx_size;
	dma->rxconf.src_addr				= dma->rx_addr;

	dma->txconf.dst_port_window_size	= UART_XMIT_SIZE;
	dma->txconf.dst_addr				= dma->tx_addr;

	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR |
		UART_FCR_CLEAR_XMIT);
	serial_out(up, MTK_UART_DMA_EN,
		   MTK_UART_DMA_EN_RX | MTK_UART_DMA_EN_TX);

	serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);
	serial_out(up, MTK_UART_EFR, UART_EFR_ECB);
	serial_out(up, UART_LCR, lcr);

	if (dmaengine_slave_config(dma->rxchan, &dma->rxconf) != 0)
		pr_err("failed to configure rx dma channel\n");
	if (dmaengine_slave_config(dma->txchan, &dma->txconf) != 0)
		pr_err("failed to configure tx dma channel\n");

	data->rx_status = DMA_RX_RUNNING;
	data->rx_pos = 0;
	mtk8250_rx_dma(up);
}
#endif

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
int mtk8250_uart_hub_reset_flow_ctrl(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_reset_flow_control();
}

int mtk8250_uart_hub_enable_bypass_mode(int bypass)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_bypass_mode_ctrl(bypass);
}

int mtk8250_uart_hub_is_ready(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_dev0_is_uarthub_ready();
}

int mtk8250_uart_hub_fifo_ctrl(int ctrl)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_md_adsp_fifo_ctrl(ctrl);
}

int mtk8250_uart_hub_dump_with_tag(const char *tag)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_dump_debug_info_with_tag(tag);
}

int mtk8250_uart_hub_reset(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_sw_reset();
}

int mtk8250_uart_hub_register_cb(UARTHUB_IRQ_CB irq_callback)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_irq_register_cb(irq_callback);
}

int mtk8250_uart_hub_assert_bit_ctrl(int ctrl)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_assert_state_ctrl(ctrl);
}

int mtk8250_uart_hub_dev0_set_rx_request(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_dev0_set_rx_request();
}

int mtk8250_uart_hub_dev0_clear_tx_request(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_dev0_clear_tx_request();
}

int mtk8250_uart_hub_get_host_wakeup_status(void)
{
	if (!UARTHUB_provider_ready())
		return -ENODEV;

	return UARTHUB_get_host_wakeup_status();
}

static struct uart_port *mtk8250_hub_port(struct tty_struct *tty)
{
	struct uart_state *state = tty->driver_data;
	struct uart_port *port;
	struct mtk8250_data *data;

	if (!state || !UARTHUB_provider_ready())
		return NULL;
	port = state->uart_port;
	if (!port || !port->dev ||
	    !of_device_is_compatible(port->dev->of_node, "mediatek,mt6985-uart"))
		return NULL;
	data = port->private_data;
	return data->hub_baud ? port : NULL;
}

int mtk8250_uart_hub_dev0_set_tx_request(struct tty_struct *tty)
{
	if (!mtk8250_hub_port(tty))
		return -ENODEV;

	return UARTHUB_dev0_set_tx_request();
}

int mtk8250_uart_hub_dev0_clear_rx_request(struct tty_struct *tty)
{
	struct uart_port *port = mtk8250_hub_port(tty);
	unsigned int empty;
	int ret;

	if (!port)
		return -ENODEV;
	ret = read_poll_timeout(port->ops->tx_empty, empty, empty & TIOCSER_TEMT,
				10, 10000, false, port);
	if (ret)
		return ret;

	return UARTHUB_dev0_clear_rx_request();
}
#endif

static int mtk8250_startup(struct uart_port *port)
{
#if IS_ENABLED(CONFIG_MTK_UARTHUB) || defined(CONFIG_SERIAL_8250_DMA)
	struct mtk8250_data *data = port->private_data;
#endif
	int ret;
#ifdef CONFIG_SERIAL_8250_DMA
	struct uart_8250_port *up = up_to_u8250p(port);

	if (READ_ONCE(port->tx_dma_fault)) {
		if (!up->dma)
			return -EIO;
		serial8250_release_dma(up);
		if (READ_ONCE(port->tx_dma_fault))
			return -EIO;
	}

	/* disable DMA for console */
	if (uart_console(port))
		up->dma = NULL;

	if (up->dma) {
		data->rx_status = DMA_RX_START;
		kfifo_reset(&port->state->port.xmit_fifo);
	}
#endif
	memset(&port->icount, 0, sizeof(port->icount));

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	if (data->hub_baud) {
		u32 value;

		if (!UARTHUB_provider_ready())
			return -EAGAIN;
		value = readl(data->hub_clk_reg);
		writel((value & ~data->hub_clk_mask) | data->hub_clk_value,
		       data->hub_clk_reg);
		if (data->hub_reset_reg) {
			serial_port_out(port, UART_FCR, UART_FCR_ENABLE_FIFO |
					UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
			udelay(2);
			/* PERI reset has write-one set and clear aliases. */
			writel(data->hub_reset_bit, data->hub_reset_reg + 4);
			readl(data->hub_reset_reg);
			writel(data->hub_reset_bit, data->hub_reset_reg + 8);
			readl(data->hub_reset_reg);
		}
		ret = UARTHUB_open();
		if (ret)
			return ret;
		port->uartclk = clk_get_rate(data->uart_clk);
	}
#endif
	ret = serial8250_do_startup(port);
	if (!ret) {
		guard(uart_port_lock_irqsave)(port);

		port->tx_dma_shutdown = false;
	}
#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	if (ret && data->hub_baud)
		UARTHUB_close();
#ifdef CONFIG_SERIAL_8250_DMA
	if (!ret && data->hub_baud && up->dma) {
		/* APDMA owns RX; do not enable the generic 8250 RX IRQs. */
		up->ier &= ~(UART_IER_RDI | UART_IER_RLSI);
	}
#endif
#endif
	return ret;
}

static void mtk8250_shutdown(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct mtk8250_data *data = port->private_data;
	int irq = data->rx_wakeup_irq;

#ifdef CONFIG_SERIAL_8250_DMA
	if (up->dma) {
		unsigned long flags;

		/* Prevent the callback from rearming RX during DMA teardown. */
		uart_port_lock_irqsave(port, &flags);
		data->rx_status = DMA_RX_SHUTDOWN;
		uart_port_unlock_irqrestore(port, flags);
	}
#endif

	serial8250_do_shutdown(port);
#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	if (data->hub_baud)
		UARTHUB_close();
#endif

	if (irq >= 0)
		serial8250_do_set_mctrl(&up->port, TIOCM_RTS);
}

static void mtk8250_disable_intrs(struct uart_8250_port *up, int mask)
{
	/* Port locked to synchronize UART_IER access against the console. */
	lockdep_assert_held_once(&up->port.lock);

	serial_out(up, UART_IER, serial_in(up, UART_IER) & (~mask));
}

static void mtk8250_enable_intrs(struct uart_8250_port *up, int mask)
{
	/* Port locked to synchronize UART_IER access against the console. */
	lockdep_assert_held_once(&up->port.lock);

	serial_out(up, UART_IER, serial_in(up, UART_IER) | mask);
}

static void mtk8250_hub_set_flow_ctrl(struct uart_8250_port *up, int mode,
				      const struct ktermios *termios)
{
	unsigned int efr;
	unsigned int xon1 = 0, xoff1 = 0, xon2 = 0, xoff2 = 0;

	lockdep_assert_held_once(&up->port.lock);

	serial_out(up, MTK_UART_ESCAPE_DAT, MTK_UART_HUB_ESCAPE_CHAR);
	serial_out(up, MTK_UART_ESCAPE_EN, mode == MTK_UART_FC_SW);
	if (mode == MTK_UART_FC_HW)
		serial_out(up, UART_MCR, UART_MCR_RTS);

	/* The MT6989 hub UART exposes these registers through FEATURE_SEL. */
	serial_out(up, MTK_UART_FEATURE_SEL, 1);
	efr = serial_in(up, MTK_UART_EFR);
	efr &= ~(MTK_UART_EFR_HW_FC | MTK_UART_EFR_SW_FC_MASK);
	efr |= UART_EFR_ECB;
	if (mode == MTK_UART_FC_HW)
		efr |= MTK_UART_EFR_HW_FC;
	else if (mode == MTK_UART_FC_SW)
		efr |= MTK_UART_EFR_XON1_XOFF1;
	serial_out(up, MTK_UART_EFR, efr);
	if (mode == MTK_UART_FC_SW) {
		serial_out(up, MTK_UART_XON1, termios->c_cc[VSTART]);
		serial_out(up, MTK_UART_XOFF1, termios->c_cc[VSTOP]);
		serial_out(up, MTK_UART_XON2, termios->c_cc[VSTART]);
		serial_out(up, MTK_UART_XOFF2, termios->c_cc[VSTOP]);
		efr = serial_in(up, MTK_UART_EFR);
		xon1 = serial_in(up, MTK_UART_XON1);
		xoff1 = serial_in(up, MTK_UART_XOFF1);
		xon2 = serial_in(up, MTK_UART_XON2);
		xoff2 = serial_in(up, MTK_UART_XOFF2);
	}
	serial_out(up, MTK_UART_FEATURE_SEL, 0);

	switch (mode) {
	case MTK_UART_FC_HW:
		mtk8250_disable_intrs(up, MTK_UART_IER_XOFFI);
		mtk8250_enable_intrs(up, MTK_UART_IER_CTSI | MTK_UART_IER_RTSI);
		break;
	case MTK_UART_FC_SW:
		mtk8250_disable_intrs(up, MTK_UART_IER_CTSI | MTK_UART_IER_RTSI);
		mtk8250_enable_intrs(up, MTK_UART_IER_XOFFI);
		dev_info_ratelimited(up->port.dev,
			"UART hub SW flow: EFR=%#x escape=%#x enable=%#x XON/XOFF=%#x/%#x,%#x/%#x\n",
			efr, serial_in(up, MTK_UART_ESCAPE_DAT),
			serial_in(up, MTK_UART_ESCAPE_EN), xon1, xoff1, xon2, xoff2);
		break;
	default:
		mtk8250_disable_intrs(up, MTK_UART_IER_XOFFI |
			MTK_UART_IER_RTSI | MTK_UART_IER_CTSI);
		break;
	}
}

static void mtk8250_set_flow_ctrl(struct uart_8250_port *up, int mode)
{
	struct uart_port *port = &up->port;
	int lcr = serial_in(up, UART_LCR);

	/* Port locked to synchronize UART_IER access against the console. */
	lockdep_assert_held_once(&port->lock);

	serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);
	serial_out(up, MTK_UART_EFR, UART_EFR_ECB);
	serial_out(up, UART_LCR, lcr);
	lcr = serial_in(up, UART_LCR);

	switch (mode) {
	case MTK_UART_FC_NONE:
		serial_out(up, MTK_UART_ESCAPE_DAT, MTK_UART_ESCAPE_CHAR);
		serial_out(up, MTK_UART_ESCAPE_EN, 0x00);
		serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);
		serial_out(up, MTK_UART_EFR, serial_in(up, MTK_UART_EFR) &
			(~(MTK_UART_EFR_HW_FC | MTK_UART_EFR_SW_FC_MASK)));
		serial_out(up, UART_LCR, lcr);
		mtk8250_disable_intrs(up, MTK_UART_IER_XOFFI |
			MTK_UART_IER_RTSI | MTK_UART_IER_CTSI);
		break;

	case MTK_UART_FC_HW:
		serial_out(up, MTK_UART_ESCAPE_DAT, MTK_UART_ESCAPE_CHAR);
		serial_out(up, MTK_UART_ESCAPE_EN, 0x00);
		serial_out(up, UART_MCR, UART_MCR_RTS);
		serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);

		/*enable hw flow control*/
		serial_out(up, MTK_UART_EFR, MTK_UART_EFR_HW_FC |
			(serial_in(up, MTK_UART_EFR) &
			(~(MTK_UART_EFR_HW_FC | MTK_UART_EFR_SW_FC_MASK))));

		serial_out(up, UART_LCR, lcr);
		mtk8250_disable_intrs(up, MTK_UART_IER_XOFFI);
		mtk8250_enable_intrs(up, MTK_UART_IER_CTSI | MTK_UART_IER_RTSI);
		break;

	case MTK_UART_FC_SW:	/*MTK software flow control */
		serial_out(up, MTK_UART_ESCAPE_DAT, MTK_UART_ESCAPE_CHAR);
		serial_out(up, MTK_UART_ESCAPE_EN, 0x01);
		serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);

		/*enable sw flow control */
		serial_out(up, MTK_UART_EFR, MTK_UART_EFR_XON1_XOFF1 |
			(serial_in(up, MTK_UART_EFR) &
			(~(MTK_UART_EFR_HW_FC | MTK_UART_EFR_SW_FC_MASK))));

		serial_out(up, MTK_UART_XON1, START_CHAR(port->state->port.tty));
		serial_out(up, MTK_UART_XOFF1, STOP_CHAR(port->state->port.tty));
		serial_out(up, UART_LCR, lcr);
		mtk8250_disable_intrs(up, MTK_UART_IER_CTSI|MTK_UART_IER_RTSI);
		mtk8250_enable_intrs(up, MTK_UART_IER_XOFFI);
		break;
	default:
		break;
	}
}

static void mtk8250_hub_set_divisor(struct uart_port *port, unsigned int baud,
				   unsigned int quot, unsigned int quot_frac)
{
	serial_port_out(port, MTK_UART_FEATURE_SEL, 1);
	serial_port_out(port, MTK_UART_DLL, quot & 0xff);
	serial_port_out(port, MTK_UART_DLH, (quot >> 8) & 0xff);
	serial_port_out(port, MTK_UART_FEATURE_SEL, 0);
}

static void
mtk8250_set_termios(struct uart_port *port, struct ktermios *termios,
		    const struct ktermios *old)
{
	static const unsigned short fraction_L_mapping[] = {
		0, 1, 0x5, 0x15, 0x55, 0x57, 0x57, 0x77, 0x7F, 0xFF, 0xFF
	};
	static const unsigned short fraction_M_mapping[] = {
		0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 3
	};
	struct uart_8250_port *up = up_to_u8250p(port);
	bool hub_port = port->set_divisor == mtk8250_hub_set_divisor;
	unsigned int baud, quot, fraction;
	unsigned long flags;
	int mode;

#ifdef CONFIG_SERIAL_8250_DMA
	if (up->dma) {
		if (uart_console(port)) {
			devm_kfree(up->port.dev, up->dma);
			up->dma = NULL;
		} else {
			mtk8250_dma_enable(up);
		}
	}
#endif

	/*
	 * Store the requested baud rate before calling the generic 8250
	 * set_termios method. Standard 8250 port expects bauds to be
	 * no higher than (uartclk / 16) so the baud will be clamped if it
	 * gets out of that bound. Mediatek 8250 port supports speed
	 * higher than that, therefore we'll get original baud rate back
	 * after calling the generic set_termios method and recalculate
	 * the speed later in this method.
	 */
	baud = tty_termios_baud_rate(termios);

	serial8250_do_set_termios(port, termios, NULL);

	tty_termios_encode_baud_rate(termios, baud, baud);

	/*
	 * Mediatek UARTs use an extra highspeed register (MTK_UART_HIGHS)
	 *
	 * We need to recalculate the quot register, as the calculation depends
	 * on the value in the highspeed register.
	 *
	 * Some baudrates are not supported by the chip, so we use the next
	 * lower rate supported and update termios c_flag.
	 *
	 * If highspeed register is set to 3, we need to specify sample count
	 * and sample point to increase accuracy. If not, we reset the
	 * registers to their default values.
	 */
	baud = uart_get_baud_rate(port, termios, old,
				  port->uartclk / 16 / UART_DIV_MAX,
				  port->uartclk);

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	{
		struct mtk8250_data *data = port->private_data;

		if (data->hub_baud && UARTHUB_provider_ready() && !UARTHUB_is_bypass_mode()) {
			int ret;

			ret = UARTHUB_config_internal_baud_rate(0, data->hub_baud);
			if (!ret)
				ret = UARTHUB_config_external_baud_rate(data->hub_baud);
			if (ret)
				dev_err(port->dev, "failed to set UART HUB baud: %d\n", ret);
			else
				baud = data->hub_baud;
		}
	}
#endif
	if (baud < 115200) {
		serial_port_out(port, MTK_UART_HIGHS, 0x0);
		quot = uart_get_divisor(port, baud);
	} else {
		serial_port_out(port, MTK_UART_HIGHS, 0x3);
		quot = DIV_ROUND_UP(port->uartclk, 256 * baud);
	}

	/*
	 * Ok, we're now changing the port state.  Do it with
	 * interrupts disabled.
	 */
	uart_port_lock_irqsave(port, &flags);

	/*
	 * Update the per-port timeout.
	 */
	uart_update_timeout(port, termios->c_cflag, baud);

	if (hub_port) {
		unsigned int fcr = serial_in(up, MTK_UART_FCR_RD);
		unsigned int efr;

		/* Program the hub port RX trigger after the generic 8250 setup. */
		serial_out(up, MTK_UART_RXTRI_AD, MTK_UART_HUB_RXTRIG);
		serial_out(up, MTK_UART_FEATURE_SEL, 1);
		efr = serial_in(up, MTK_UART_EFR);
		serial_out(up, MTK_UART_EFR, efr | UART_EFR_ECB);
		serial_out(up, UART_FCR, fcr | UART_FCR_R_TRIG_11 | UART_FCR_ENABLE_FIFO);
		serial_out(up, MTK_UART_EFR, efr);
		serial_out(up, MTK_UART_FEATURE_SEL, 0);

		mtk8250_hub_set_divisor(port, baud, quot, 0);
	} else {
		serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);
		serial_dl_write(up, quot);
		serial_port_out(port, UART_LCR, up->lcr);
	}

	if (hub_port && baud == MTK_UART_HUB_BAUD) {
		serial_port_out(port, MTK_UART_SAMPLE_COUNT, 7);
		serial_port_out(port, MTK_UART_SAMPLE_POINT, 3);
		serial_port_out(port, MTK_UART_FRACDIV_L, 0xdb);
		serial_port_out(port, MTK_UART_FRACDIV_M, 0x1);
	} else if (baud >= 115200) {
		unsigned int tmp;

		tmp = (port->uartclk / (baud *  quot)) - 1;
		serial_port_out(port, MTK_UART_SAMPLE_COUNT, tmp);
		serial_port_out(port, MTK_UART_SAMPLE_POINT,
					(tmp >> 1) - 1);

		/* Keep the scaled clock from overflowing at high UART clock rates. */
		fraction = div_u64((u64)port->uartclk * 100, baud * quot) % 100;
		fraction = DIV_ROUND_CLOSEST(fraction, 10);
		serial_port_out(port, MTK_UART_FRACDIV_L,
						fraction_L_mapping[fraction]);
		serial_port_out(port, MTK_UART_FRACDIV_M,
						fraction_M_mapping[fraction]);
	} else {
		serial_port_out(port, MTK_UART_SAMPLE_COUNT, 0x00);
		serial_port_out(port, MTK_UART_SAMPLE_POINT, 0xff);
		serial_port_out(port, MTK_UART_FRACDIV_L, 0x00);
		serial_port_out(port, MTK_UART_FRACDIV_M, 0x00);
	}

	if ((termios->c_cflag & CRTSCTS) && (!(termios->c_iflag & CRTSCTS)))
		mode = MTK_UART_FC_HW;
	else if (termios->c_iflag & CRTSCTS)
		mode = MTK_UART_FC_SW;
	else
		mode = MTK_UART_FC_NONE;

	if (hub_port)
		mtk8250_hub_set_flow_ctrl(up, mode, termios);
	else
		mtk8250_set_flow_ctrl(up, mode);

	if (uart_console(port))
		up->port.cons->cflag = termios->c_cflag;

	uart_port_unlock_irqrestore(port, flags);
	/* Don't rewrite B0 */
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
}

static int __maybe_unused mtk8250_runtime_suspend(struct device *dev)
{
	struct mtk8250_data *data = dev_get_drvdata(dev);
	struct uart_8250_port *up = serial8250_get_port(data->line);

	/* wait until UART in idle status */
	while
		(serial_in(up, MTK_UART_DEBUG0));

	clk_disable_unprepare(data->uart_clk);
	clk_disable_unprepare(data->bus_clk);

	return 0;
}

static int __maybe_unused mtk8250_runtime_resume(struct device *dev)
{
	struct mtk8250_data *data = dev_get_drvdata(dev);

	clk_prepare_enable(data->bus_clk);
	clk_prepare_enable(data->uart_clk);

	return 0;
}

static void
mtk8250_do_pm(struct uart_port *port, unsigned int state, unsigned int old)
{
	if (!state)
		pm_runtime_get_sync(port->dev);

	serial8250_do_pm(port, state, old);

	if (state)
		pm_runtime_put_sync_suspend(port->dev);
}

#ifdef CONFIG_SERIAL_8250_DMA
static bool mtk8250_dma_filter(struct dma_chan *chan, void *param)
{
	return false;
}
#endif

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
static int mtk8250_probe_hub_reset(struct device *dev, struct mtk8250_data *data)
{
	struct device_node *np = dev->of_node;
	u32 base, set[3], clear[3];

	if (!of_property_present(np, "peri-reset"))
		return 0;

	if (of_property_read_u32(np, "peri-reset", &base) ||
	    of_property_read_u32_array(np, "peri-reset-set", set, ARRAY_SIZE(set)) ||
	    of_property_read_u32_array(np, "peri-reset-clr", clear, ARRAY_SIZE(clear)))
		return -EINVAL;

	if (set[0] != (u64)base + 4 || clear[0] != (u64)base + 8 ||
	    !is_power_of_2(set[1]) || set[1] != set[2] ||
	    set[1] != clear[1] || set[1] != clear[2])
		return -EINVAL;

	data->hub_reset_reg = devm_ioremap(dev, base, 3 * sizeof(u32));
	if (!data->hub_reset_reg)
		return -ENOMEM;

	data->hub_reset_bit = set[1];
	return 0;
}
#endif

static int mtk8250_probe_of(struct platform_device *pdev, struct uart_port *p,
			   struct mtk8250_data *data)
{
#ifdef CONFIG_SERIAL_8250_DMA
	int dmacnt;
#endif

	data->uart_clk = devm_clk_get_enabled(&pdev->dev, "baud");
	if (IS_ERR(data->uart_clk)) {
		/*
		 * For compatibility with older device trees try unnamed
		 * clk when no baud clk can be found.
		 */
		data->uart_clk = devm_clk_get_enabled(&pdev->dev, NULL);
		if (IS_ERR(data->uart_clk)) {
			dev_warn(&pdev->dev, "Can't get uart clock\n");
			return PTR_ERR(data->uart_clk);
		}

		return 0;
	}

	data->bus_clk = devm_clk_get_enabled(&pdev->dev, "bus");
	if (IS_ERR(data->bus_clk))
		return PTR_ERR(data->bus_clk);

#if IS_ENABLED(CONFIG_MTK_UARTHUB)
	if (of_device_is_compatible(pdev->dev.of_node, "mediatek,mt6985-uart") &&
	    !of_property_read_u32(pdev->dev.of_node, "hub-baud", &data->hub_baud)) {
		u32 clock_cfg[3];
		int ret;

		ret = of_property_read_u32_array(pdev->dev.of_node, "peri-clock-con",
						 clock_cfg, ARRAY_SIZE(clock_cfg));
		if (ret)
			return ret;
		data->hub_clk_reg = devm_ioremap(&pdev->dev, clock_cfg[0], sizeof(u32));
		if (!data->hub_clk_reg)
			return -ENOMEM;
		data->hub_clk_mask = clock_cfg[1];
		data->hub_clk_value = clock_cfg[2] & clock_cfg[1];
		if (data->hub_baud) {
			ret = mtk8250_probe_hub_reset(&pdev->dev, data);
			if (ret)
				return dev_err_probe(&pdev->dev, ret, "invalid UART HUB PERI reset\n");
			p->set_divisor = mtk8250_hub_set_divisor;
		}
	}
#endif

	data->dma = NULL;
#ifdef CONFIG_SERIAL_8250_DMA
	dmacnt = of_property_count_strings(pdev->dev.of_node, "dma-names");
	if (dmacnt == 2) {
		data->dma = devm_kzalloc(&pdev->dev, sizeof(*data->dma),
					 GFP_KERNEL);
		if (!data->dma)
			return -ENOMEM;

		data->dma->fn = mtk8250_dma_filter;
		data->dma->rx_size = MTK_UART_RX_SIZE;
		data->dma->tx_check_cookie = true;
		data->dma->rxconf.src_maxburst = MTK_UART_RX_TRIGGER;
		data->dma->txconf.dst_maxburst = MTK_UART_TX_TRIGGER;
	}
#endif

	return 0;
}

static int mtk8250_probe(struct platform_device *pdev)
{
	struct uart_8250_port uart = {};
	struct mtk8250_data *data;
	struct resource *regs;
	int irq, err;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(&pdev->dev, "no registers defined\n");
		return -EINVAL;
	}

	uart.port.membase = devm_ioremap(&pdev->dev, regs->start,
					 resource_size(regs));
	if (!uart.port.membase)
		return -ENOMEM;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->clk_count = 0;

	if (pdev->dev.of_node) {
		err = mtk8250_probe_of(pdev, &uart.port, data);
		if (err)
			return err;
	} else
		return -ENODEV;

	spin_lock_init(&uart.port.lock);
	uart.port.mapbase = regs->start;
	uart.port.irq = irq;
	uart.port.pm = mtk8250_do_pm;
	uart.port.type = PORT_16550;
	uart.port.flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT;
	uart.port.dev = &pdev->dev;
	of_property_read_u32(pdev->dev.of_node, "uart-line", &uart.port.line);
	uart.port.iotype = UPIO_MEM32;
	uart.port.regshift = 2;
	uart.port.private_data = data;
	uart.port.shutdown = mtk8250_shutdown;
	uart.port.startup = mtk8250_startup;
	uart.port.set_termios = mtk8250_set_termios;
	uart.port.uartclk = clk_get_rate(data->uart_clk);
#ifdef CONFIG_SERIAL_8250_DMA
	if (data->dma)
		uart.dma = data->dma;
#endif

	/* Disable Rate Fix function */
	writel(0x0, uart.port.membase +
			(MTK_UART_RATE_FIX << uart.port.regshift));

	platform_set_drvdata(pdev, data);

	data->line = serial8250_register_8250_port(&uart);
	if (data->line < 0)
		return data->line;

	data->rx_wakeup_irq = platform_get_irq_optional(pdev, 1);

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	return 0;
}

static void mtk8250_remove(struct platform_device *pdev)
{
	struct mtk8250_data *data = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);

	serial8250_unregister_port(data->line);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
}

static int __maybe_unused mtk8250_suspend(struct device *dev)
{
	struct mtk8250_data *data = dev_get_drvdata(dev);
	int irq = data->rx_wakeup_irq;
	int err;

	serial8250_suspend_port(data->line);

	pinctrl_pm_select_sleep_state(dev);
	if (irq >= 0) {
		err = enable_irq_wake(irq);
		if (err) {
			dev_err(dev,
				"failed to enable irq wake on IRQ %d: %d\n",
				irq, err);
			pinctrl_pm_select_default_state(dev);
			serial8250_resume_port(data->line);
			return err;
		}
	}

	return 0;
}

static int __maybe_unused mtk8250_resume(struct device *dev)
{
	struct mtk8250_data *data = dev_get_drvdata(dev);
	int irq = data->rx_wakeup_irq;

	if (irq >= 0)
		disable_irq_wake(irq);
	pinctrl_pm_select_default_state(dev);

	serial8250_resume_port(data->line);

	return 0;
}

static const struct dev_pm_ops mtk8250_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk8250_suspend, mtk8250_resume)
	SET_RUNTIME_PM_OPS(mtk8250_runtime_suspend, mtk8250_runtime_resume,
				NULL)
};

static const struct of_device_id mtk8250_of_match[] = {
	{ .compatible = "mediatek,mt6577-uart" },
	{ .compatible = "mediatek,mt6985-uart" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk8250_of_match);

static struct platform_driver mtk8250_platform_driver = {
	.driver = {
		.name		= "mt6577-uart",
		.pm		= &mtk8250_pm_ops,
		.of_match_table	= mtk8250_of_match,
	},
	.probe			= mtk8250_probe,
	.remove			= mtk8250_remove,
};
module_platform_driver(mtk8250_platform_driver);

#ifdef CONFIG_SERIAL_8250_CONSOLE
static int __init early_mtk8250_setup(struct earlycon_device *device,
					const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->port.iotype = UPIO_MEM32;
	device->port.regshift = 2;

	return early_serial8250_setup(device, NULL);
}

OF_EARLYCON_DECLARE(mtk8250, "mediatek,mt6577-uart", early_mtk8250_setup);
#endif

MODULE_AUTHOR("Matthias Brugger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mediatek 8250 serial port driver");
