/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CPUIDLE_PSCI_H
#define __CPUIDLE_PSCI_H

struct device_node;
struct generic_pm_domain;

void psci_set_domain_state(struct generic_pm_domain *pd, unsigned int state_idx,
			   u32 state);
int psci_dt_parse_state_node(struct device_node *np, u32 *state);

#ifdef CONFIG_MTK_ROTHKO_LPM
void mt6989_lpm_suspend_source(unsigned int cpu, bool enter);
#else
static inline void mt6989_lpm_suspend_source(unsigned int cpu, bool enter) { }
#endif

#endif /* __CPUIDLE_PSCI_H */
