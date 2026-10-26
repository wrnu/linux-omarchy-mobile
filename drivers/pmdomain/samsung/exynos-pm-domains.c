// SPDX-License-Identifier: GPL-2.0
//
// Exynos Generic power domain support.
//
// Copyright (c) 2012 Samsung Electronics Co., Ltd.
//		http://www.samsung.com
//
// Implementation of Exynos specific power domain control which is used in
// conjunction with runtime-pm. Support for both device-tree and non-device-tree
// based power domain support is included.

#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pm_domain.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>

struct exynos_pm_domain_config {
	/* Value for LOCAL_PWR_CFG and STATUS fields for each domain */
	u32 local_pwr_cfg;
	/*
	 * Non-zero where the PMU only takes writes through the secure
	 * monitor (gs101): the domain is reached through the PMU regmap, at
	 * its address minus this PMU base.
	 */
	phys_addr_t pmu_base;
	/* How long a domain may take to reach its new state; 0 means 1 ms */
	unsigned int timeout_us;
	/* Offset of the TZ save/restore register in the domain's DTZPC */
	u32 smc_offset;
};

/*
 * Exynos specific wrapper around the generic power domain
 */
struct exynos_pm_domain {
	void __iomem *base;
	struct regmap *pmu;
	u32 offset;
	struct generic_pm_domain pd;
	u32 local_pwr_cfg;
	unsigned int timeout_us;
	u32 smc_offset;
	phys_addr_t ac_pa;
	bool ac_saved;		/* a save from a powered domain succeeded */
};

#define EXYNOS_SMC_CMD_PREPARE_PD_ONOFF		0x82000410
#define EXYNOS_GET_IN_PD_DOWN			0
#define EXYNOS_WAKEUP_PD_DOWN			1
#define EXYNOS_RUNTIME_PM_TZPC_GROUP		2

/*
 * A domain whose block is guarded by a DTZPC loses its TrustZone
 * configuration when it powers off. EL3 saves it before a power-off and
 * restores it after a power-on, when asked to.
 */
static int exynos_pd_access_controller_power(struct exynos_pm_domain *pd,
					     bool power_on)
{
	struct arm_smccc_res res;

	if (!pd->ac_pa || !pd->smc_offset)
		return 0;

	arm_smccc_smc(EXYNOS_SMC_CMD_PREPARE_PD_ONOFF,
		      power_on ? EXYNOS_WAKEUP_PD_DOWN : EXYNOS_GET_IN_PD_DOWN,
		      pd->ac_pa + pd->smc_offset,
		      EXYNOS_RUNTIME_PM_TZPC_GROUP, 0, 0, 0, 0, &res);

	return res.a0;
}

static u32 exynos_pd_read(struct exynos_pm_domain *pd, u32 reg)
{
	u32 val = 0;

	if (pd->pmu) {
		regmap_read(pd->pmu, pd->offset + reg, &val);
		return val;
	}
	return readl_relaxed(pd->base + reg);
}

static void exynos_pd_write(struct exynos_pm_domain *pd, u32 reg, u32 val)
{
	if (pd->pmu)
		regmap_write(pd->pmu, pd->offset + reg, val);
	else
		writel_relaxed(val, pd->base + reg);
}

static int exynos_pd_power(struct generic_pm_domain *domain, bool power_on)
{
	struct exynos_pm_domain *pd;
	u32 pwr, status;
	int ret;

	pd = container_of(domain, struct exynos_pm_domain, pd);

	if (!power_on) {
		ret = exynos_pd_access_controller_power(pd, power_on);
		if (ret) {
			pr_err("SMC for power domain %s disable failed: %d\n",
			       domain->name, ret);
			return ret;
		}
		pd->ac_saved = true;
	} else if (pd->ac_pa && !pd->ac_saved) {
		/*
		 * Restoring without a save of the powered block leaves the
		 * block's registers secure, and an access then hangs the SoC.
		 */
		pr_err("Power domain %s: no DTZPC save to restore, staying off\n",
		       domain->name);
		return -EIO;
	}

	pwr = power_on ? pd->local_pwr_cfg : 0;
	exynos_pd_write(pd, 0x0, pwr);

	ret = read_poll_timeout(exynos_pd_read, status,
				(status & pd->local_pwr_cfg) == pwr, 100,
				pd->timeout_us, false, pd, 0x4);
	if (ret) {
		pr_err("Power domain %s %s failed (status 0x%x)\n", domain->name,
		       power_on ? "enable" : "disable", status);
	} else if (power_on) {
		ret = exynos_pd_access_controller_power(pd, power_on);
		if (ret)
			pr_err("SMC for power domain %s enable failed: %d\n",
			       domain->name, ret);
	}

	return ret;
}

static int exynos_pd_power_on(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, true);
}

static int exynos_pd_power_off(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, false);
}

static const struct exynos_pm_domain_config exynos4210_cfg = {
	.local_pwr_cfg		= 0x7,
};

static const struct exynos_pm_domain_config exynos5433_cfg = {
	.local_pwr_cfg		= 0xf,
};

static const struct exynos_pm_domain_config gs101_cfg = {
	.local_pwr_cfg		= 0x1,
	.pmu_base		= 0x17460000,
	/*
	 * Google's CAL polls these domains' status up to 5000 times, 1 us
	 * apart; allow about twice that.
	 */
	.timeout_us		= 10000,
	.smc_offset		= 0x0204,
};

static const struct of_device_id exynos_pm_domain_of_match[] = {
	{
		.compatible = "samsung,exynos4210-pd",
		.data = &exynos4210_cfg,
	}, {
		.compatible = "samsung,exynos5433-pd",
		.data = &exynos5433_cfg,
	}, {
		.compatible = "google,gs101-pd",
		.data = &gs101_cfg,
	},
	{ },
};

static const char *exynos_get_domain_name(struct device *dev,
					  struct device_node *node)
{
	const char *name;

	if (of_property_read_string(node, "label", &name) < 0)
		name = kbasename(node->full_name);
	return devm_kstrdup_const(dev, name, GFP_KERNEL);
}

static int exynos_pd_get_access_controller(struct device *dev,
					   struct exynos_pm_domain *pd)
{
	struct device_node *ac_np;
	struct resource ac_res;
	int ret, err;

	ac_np = of_parse_phandle(dev->of_node, "samsung,dtzpc", 0);
	if (!ac_np)
		return 0;

	ret = of_address_to_resource(ac_np, 0, &ac_res);
	of_node_put(ac_np);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get access controller\n");

	pd->ac_pa = ac_res.start;

	/*
	 * For some domains, TZ save/restore might not be implemented. If that
	 * is the case, simply mark it as always on, as otherwise a power cycle
	 * will lead to lost TZ configuration, making it impossible to access
	 * registers from Linux afterwards.
	 */
	err = exynos_pd_access_controller_power(pd, false);
	if (err == -ENOENT) {
		pd->ac_pa = 0;
		pd->pd.flags |= GENPD_FLAG_ALWAYS_ON;
	} else if (!err) {
		/* Only a save of a powered domain can be restored */
		pd->ac_saved = exynos_pd_read(pd, 0x4) & pd->local_pwr_cfg;
	}

	return 0;
}

static int exynos_pd_probe(struct platform_device *pdev)
{
	const struct exynos_pm_domain_config *pm_domain_cfg;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct of_phandle_args child, parent;
	struct exynos_pm_domain *pd;
	int on, ret;

	pm_domain_cfg = of_device_get_match_data(dev);
	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->pd.name = exynos_get_domain_name(dev, np);
	if (!pd->pd.name)
		return -ENOMEM;

	if (pm_domain_cfg->pmu_base) {
		struct resource res;

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			return ret;
		pd->pmu = exynos_get_pmu_regmap();
		if (IS_ERR(pd->pmu))
			return PTR_ERR(pd->pmu);
		pd->offset = res.start - pm_domain_cfg->pmu_base;
	} else {
		pd->base = of_iomap(np, 0);
		if (!pd->base)
			return -ENODEV;
	}

	pd->pd.power_off = exynos_pd_power_off;
	pd->pd.power_on = exynos_pd_power_on;
	pd->local_pwr_cfg = pm_domain_cfg->local_pwr_cfg;
	pd->timeout_us = pm_domain_cfg->timeout_us ?: 1000;
	pd->smc_offset = pm_domain_cfg->smc_offset;

	ret = exynos_pd_get_access_controller(dev, pd);
	if (ret)
		return ret;

	/*
	 * Some Samsung platforms with bootloaders turning on the splash-screen
	 * and handing it over to the kernel, requires the power-domains to be
	 * reset during boot.
	 */
	if (IS_ENABLED(CONFIG_ARM) &&
	    of_device_is_compatible(np, "samsung,exynos4210-pd"))
		exynos_pd_power_off(&pd->pd);

	on = exynos_pd_read(pd, 0x4) & pd->local_pwr_cfg;

	pm_genpd_init(&pd->pd, NULL, !on);
	ret = of_genpd_add_provider_simple(np, &pd->pd);

	if (ret == 0 && of_parse_phandle_with_args(np, "power-domains",
				      "#power-domain-cells", 0, &parent) == 0) {
		child.np = np;
		child.args_count = 0;

		if (of_genpd_add_subdomain(&parent, &child))
			pr_warn("%pOF failed to add subdomain: %pOF\n",
				parent.np, child.np);
		else
			pr_info("%pOF has as child subdomain: %pOF.\n",
				parent.np, child.np);
	}

	pm_runtime_enable(dev);
	return ret;
}

static struct platform_driver exynos_pd_driver = {
	.probe	= exynos_pd_probe,
	.driver	= {
		.name		= "exynos-pd",
		.of_match_table	= exynos_pm_domain_of_match,
		.suppress_bind_attrs = true,
	}
};

static __init int exynos4_pm_init_power_domain(void)
{
	return platform_driver_register(&exynos_pd_driver);
}
core_initcall(exynos4_pm_init_power_domain);
