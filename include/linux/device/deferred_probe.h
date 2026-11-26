/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DEVICE_DEFERRED_PROBE_H
#define _LINUX_DEVICE_DEFERRED_PROBE_H

/* Retry deferred consumers when a bound supplier's firmware becomes ready. */
void driver_deferred_probe_trigger(void);

#endif /* _LINUX_DEVICE_DEFERRED_PROBE_H */
