/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_ROTHKO_PSTORE_H
#define _LINUX_ROTHKO_PSTORE_H

#include <linux/compiler.h>
#include <linux/types.h>

#define ROTHKO_PSTORE_PHYS_ADDR	0x400000000ULL
#define ROTHKO_PSTORE_SIZE	(200UL * 1024UL * 1024UL)

#ifdef CONFIG_ROTHKO_PSTORE_HIGHMEM
extern bool rothko_pstore_highmem_ready;

static inline bool rothko_pstore_highmem_active(void)
{
	return rothko_pstore_highmem_ready;
}
#else
static inline bool rothko_pstore_highmem_active(void)
{
	return false;
}
#endif

#if defined(CONFIG_DRM_ROTHKO_N12) && defined(CONFIG_PSTORE_CONSOLE) && \
    defined(CONFIG_PSTORE_RAM)
void rothko_pstore_suspend_stage(unsigned int attempt, const char *stage, int status);
#else
static inline void rothko_pstore_suspend_stage(unsigned int attempt, const char *stage,
					     int status) { }
#endif

#if defined(CONFIG_SUSPEND) && defined(CONFIG_DRM_ROTHKO_N12) && \
    defined(CONFIG_PSTORE_CONSOLE) && defined(CONFIG_PSTORE_RAM)
unsigned int rothko_s2idle_attempt(void);
#else
static inline unsigned int rothko_s2idle_attempt(void) { return 0; }
#endif

#ifdef CONFIG_ROTHKO_PM_PSTORE_DIAGNOSTIC
void __printf(1, 2) rothko_pstore_mark(const char *fmt, ...);
extern unsigned int rothko_pm_attempt;
#else
static inline __printf(1, 2) void rothko_pstore_mark(const char *fmt, ...) { }
#endif

static inline unsigned int rothko_pstore_attempt(void)
{
#ifdef CONFIG_ROTHKO_PM_PSTORE_DIAGNOSTIC
	return READ_ONCE(rothko_pm_attempt);
#else
	return 0;
#endif
}
#endif /* _LINUX_ROTHKO_PSTORE_H */
