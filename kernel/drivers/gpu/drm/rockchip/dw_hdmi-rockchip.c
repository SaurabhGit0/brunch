/*
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/crc16.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <drm/drm_of.h>
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/bridge/dw_hdmi.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_vop.h"

#define RK3288_GRF_SOC_CON6		0x025C
#define RK3288_HDMI_LCDC_SEL		BIT(4)
#define RK3399_GRF_SOC_CON20		0x6250
#define RK3399_HDMI_LCDC_SEL		BIT(6)

#define HIWORD_UPDATE(val, mask)	(val | (mask) << 16)

/**
 * struct rockchip_hdmi_chip_data - splite the grf setting of kind of chips
 * @lcdsel_grf_reg: grf register offset of lcdc select
 * @lcdsel_big: reg value of selecting vop big for HDMI
 * @lcdsel_lit: reg value of selecting vop little for HDMI
 */
struct rockchip_hdmi_chip_data {
	u32	lcdsel_grf_reg;
	u32	lcdsel_big;
	u32	lcdsel_lit;
};

struct rockchip_hdmi {
	struct device *dev;
	struct regmap *regmap;
	struct drm_encoder encoder;
	const struct rockchip_hdmi_chip_data *chip_data;
	struct clk *vpll_clk;
	struct clk *grf_clk;
	struct dw_hdmi *hdmi;
};

#define CLK_SLOP(clk)		((clk) / 1000)
#define CLK_PLUS_SLOP(clk)	((clk) + CLK_SLOP(clk))

#define to_rockchip_hdmi(x)	container_of(x, struct rockchip_hdmi, x)

static const int dw_hdmi_rates[] = {
	25176471,	/* for 25.175 MHz, 0.006% off */
	25200000,
	27000000,
	28320000,
	30240000,
	31500000,
	32000000,
	33750000,
	36000000,
	40000000,
	49500000,
	50000000,
	54000000,
	57290323,	/* for 57.284 MHz, .011 % off */
	65000000,
	68250000,
	71000000,
	72000000,
	73250000,
	74250000,
	74437500,	/* for 74.44 MHz, .003% off */
	75000000,
	78750000,
	78800000,
	79500000,
	83500000,
	85500000,
	88750000,
	97750000,
	101000000,
	106500000,
	108000000,
	115500000,
	118666667,	/* for 118.68 MHz, .011% off */
	119000000,
	121714286,	/* for 121.75 MHz, .029% off */
	135000000,
	136800000,	/* for 136.75 MHz, .037% off */
	146250000,
	148500000,
	154000000,
	162000000,
};

static const struct dw_hdmi_mpll_config rockchip_mpll_cfg[] = {
	{
		30666000, {
			{ 0x00b3, 0x0000 },
			{ 0x2153, 0x0000 },
			{ 0x40f3, 0x0000 },
		},
	},  {
		36800000, {
			{ 0x00b3, 0x0000 },
			{ 0x2153, 0x0000 },
			{ 0x40a2, 0x0001 },
		},
	},  {
		46000000, {
			{ 0x00b3, 0x0000 },
			{ 0x2142, 0x0001 },
			{ 0x40a2, 0x0001 },
		},
	},  {
		61333000, {
			{ 0x0072, 0x0001 },
			{ 0x2142, 0x0001 },
			{ 0x40a2, 0x0001 },
		},
	},  {
		73600000, {
			{ 0x0072, 0x0001 },
			{ 0x2142, 0x0001 },
			{ 0x4061, 0x0002 },
		},
	},  {
		92000000, {
			{ 0x0072, 0x0001 },
			{ 0x2145, 0x0002 },
			{ 0x4061, 0x0002 },
		},
	},  {
		122666000, {
			{ 0x0051, 0x0002 },
			{ 0x2145, 0x0002 },
			{ 0x4061, 0x0002 },
		},
	},  {
		147200000, {
			{ 0x0051, 0x0002 },
			{ 0x2145, 0x0002 },
			{ 0x4064, 0x0003 },
		},
	},  {
		184000000, {
			{ 0x0051, 0x0002 },
			{ 0x214c, 0x0003 },
			{ 0x4064, 0x0003 },
		},
	},  {
		226666000, {
			{ 0x0040, 0x0003 },
			{ 0x214c, 0x0003 },
			{ 0x4064, 0x0003 },
		},
	},  {
		272000000, {
			{ 0x0040, 0x0003 },
			{ 0x214c, 0x0003 },
			{ 0x5a64, 0x0003 },
		},
	},  {
		340000000, {
			{ 0x0040, 0x0003 },
			{ 0x3b4c, 0x0003 },
			{ 0x5a64, 0x0003 },
		},
	},  {
		600000000, {
			{ 0x1a40, 0x0003 },
			{ 0x3b4c, 0x0003 },
			{ 0x5a64, 0x0003 },
		},
	},  {
		~0UL, {
			{ 0x0000, 0x0000 },
			{ 0x0000, 0x0000 },
			{ 0x0000, 0x0000 },
		},
	}
};

static const struct dw_hdmi_curr_ctrl rockchip_cur_ctr[] = {
	/*      pixelclk     bpp8    bpp10   bpp12 */
	{
		600000000, { 0x0000, 0x0000, 0x0000 },
	},  {
		~0UL,      { 0x0000, 0x0000, 0x0000 },
	},
};

static const struct dw_hdmi_phy_config rockchip_phy_config[] = {
	/*pixelclk   symbol   term   vlev*/
	{ CLK_PLUS_SLOP(74250000),  0x8009, 0x0004, 0x0272},
	{ CLK_PLUS_SLOP(165000000), 0x802b, 0x0004, 0x0209},
	{ CLK_PLUS_SLOP(297000000), 0x8039, 0x0005, 0x028d},
	{ ~0UL,	                    0x0000, 0x0000, 0x0000}
};

static int rockchip_hdmi_parse_dt(struct rockchip_hdmi *hdmi)
{
	struct device_node *np = hdmi->dev->of_node;

	hdmi->regmap = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(hdmi->regmap)) {
		DRM_DEV_ERROR(hdmi->dev, "Unable to get rockchip,grf\n");
		return PTR_ERR(hdmi->regmap);
	}

	hdmi->vpll_clk = devm_clk_get(hdmi->dev, "vpll");
	if (PTR_ERR(hdmi->vpll_clk) == -ENOENT) {
		hdmi->vpll_clk = NULL;
	} else if (PTR_ERR(hdmi->vpll_clk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(hdmi->vpll_clk)) {
		DRM_DEV_ERROR(hdmi->dev, "failed to get grf clock\n");
		return PTR_ERR(hdmi->vpll_clk);
	}

	hdmi->grf_clk = devm_clk_get(hdmi->dev, "grf");
	if (PTR_ERR(hdmi->grf_clk) == -ENOENT) {
		hdmi->grf_clk = NULL;
	} else if (PTR_ERR(hdmi->grf_clk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(hdmi->grf_clk)) {
		DRM_DEV_ERROR(hdmi->dev, "failed to get grf clock\n");
		return PTR_ERR(hdmi->grf_clk);
	}

	return 0;
}

static enum drm_mode_status
dw_hdmi_rockchip_mode_valid(struct drm_connector *connector,
			    const struct drm_display_mode *mode)
{
	int pclk = mode->clock * 1000;
	int num_rates = ARRAY_SIZE(dw_hdmi_rates);
	int i;

	/*
	 * Pixel clocks we support are always < 2GHz and so fit in an
	 * int.  We should make sure source rate does too so we don't get
	 * overflow when we multiply by 1000.
	 */
	if (mode->clock > INT_MAX / 1000)
		return MODE_BAD;

	/* HACK: Modes > 3840x2160 pixels can't work on the VOP; filter them. */
	if (mode->hdisplay > 3840 || mode->vdisplay > 2160)
		return MODE_BAD;

	for (i = 0; i < num_rates; i++) {
		int slop = CLK_SLOP(pclk);

		if ((pclk >= dw_hdmi_rates[i] - slop) &&
		    (pclk <= dw_hdmi_rates[i] + slop))
			return MODE_OK;
	}

	return MODE_BAD;
}

static const struct drm_encoder_funcs dw_hdmi_rockchip_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void dw_hdmi_rockchip_encoder_disable(struct drm_encoder *encoder)
{
}

static int
dw_hdmi_rockchip_encoder_mode_fixup(struct drm_encoder *encoder,
				    struct drm_display_mode *adj_mode)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(encoder);
	int pclk = adj_mode->clock * 1000;
	int best_diff = INT_MAX;
	int best_clock = 0;
	int slop;
	int i;

	/* Pick the best clock */
	for (i = 0; i < ARRAY_SIZE(dw_hdmi_rates); i++) {
		int diff = dw_hdmi_rates[i] - pclk;

		if (diff < 0)
			diff = -diff;
		if (diff < best_diff) {
			best_diff = diff;
			best_clock = dw_hdmi_rates[i];

			/* Bail early if we're exact */
			if (best_diff == 0)
				return 0;
		}
	}

	/* Double check that it's OK */
	slop = CLK_SLOP(pclk);
	if ((pclk >= best_clock - slop) && (pclk <= best_clock + slop)) {
		adj_mode->clock = DIV_ROUND_UP(best_clock, 1000);
		return 0;
	}

	/* Shoudn't be here; we should have said rate wasn't valid */
	dev_warn(hdmi->dev, "tried to set invalid rate %d\n", adj_mode->clock);
	return -EINVAL;
}

static void dw_hdmi_rockchip_encoder_mode_set(struct drm_encoder *encoder,
					      struct drm_display_mode *mode,
					      struct drm_display_mode *adj_mode)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(encoder);

	clk_set_rate(hdmi->vpll_clk, adj_mode->clock * 1000);
}

static void dw_hdmi_rockchip_encoder_enable(struct drm_encoder *encoder)
{
	struct rockchip_hdmi *hdmi = to_rockchip_hdmi(encoder);
	u32 val;
	int ret;

	ret = drm_of_encoder_active_endpoint_id(hdmi->dev->of_node, encoder);
	if (ret)
		val = hdmi->chip_data->lcdsel_lit;
	else
		val = hdmi->chip_data->lcdsel_big;

	ret = clk_prepare_enable(hdmi->grf_clk);
	if (ret < 0) {
		DRM_DEV_ERROR(hdmi->dev, "failed to enable grfclk %d\n", ret);
		return;
	}

	ret = regmap_write(hdmi->regmap, hdmi->chip_data->lcdsel_grf_reg, val);
	if (ret != 0)
		DRM_DEV_ERROR(hdmi->dev, "Could not write to GRF: %d\n", ret);

	clk_disable_unprepare(hdmi->grf_clk);
	DRM_DEV_DEBUG(hdmi->dev, "vop %s output to hdmi\n",
		      ret ? "LIT" : "BIG");
}

static int
dw_hdmi_rockchip_encoder_atomic_check(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);

	s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
	s->output_type = DRM_MODE_CONNECTOR_HDMIA;

	return dw_hdmi_rockchip_encoder_mode_fixup(encoder,
						   &crtc_state->adjusted_mode);
}

static ssize_t hdcp_key_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rockchip_hdmi *hdmi = dev_get_drvdata(dev);
	struct dw_hdmi_hdcp_key_1x *key;
	struct nvmem_cell *cell;
	u16 encrypt_seed;
	u8 *cpu_id;
	size_t len;
	int ret;

	/*
	 * The HDCP Key format should look like this: "12345678...",
	 * every two characters stand for a byte, so the total key size
	 * would be (308 * 2) byte.
	 *
	 * Allow key size to be bigger as a simple way to allow "\r" or "\n"
	 * after the key.
	 */
	if (count < (DW_HDMI_HDCP_KEY_LEN * 2))
		return -EINVAL;

	cell = nvmem_cell_get(dev, "cpu-id");
	if (IS_ERR(cell)) {
		DRM_DEV_ERROR(dev, "err getting CPU ID %ld\n", PTR_ERR(cell));
		return PTR_ERR(cell);
	}
	cpu_id = (u8 *)nvmem_cell_read(cell, &len);
	if (IS_ERR(cpu_id)) {
		nvmem_cell_put(cell);
		DRM_DEV_ERROR(dev, "err reading CPU ID %ld\n", PTR_ERR(cpu_id));
		return PTR_ERR(cpu_id);
	}
	encrypt_seed = crc16(0xffff, cpu_id, len);
	nvmem_cell_put(cell);

	key = kzalloc(sizeof(*key), GFP_KERNEL);
	if (IS_ERR(key))
		return -ENOMEM;

	/*
	 * The format of input HDCP Key should be "12345678...".
	 * there is no standard format for HDCP keys, so it is
	 * just made up for this driver.
	 *
	 * The "ksv & device_key & sha" should parsed from input data
	 * buffer, and the "seed" would take the crc16 of cpu uid.
	 */
	ret = hex2bin(key->user_provided_key, buf, DW_HDMI_HDCP_KEY_LEN);
	if (ret) {
		dev_err(dev, "Failed to decode the input HDCP key: %d\n", ret);
		goto exit;
	}

	key->seed[0] = encrypt_seed & 0xFF;
	key->seed[1] = (encrypt_seed >> 8) & 0xFF;

	ret = dw_hdmi_config_hdcp_key(hdmi->hdmi, key);
	if (ret)
		goto exit;

	ret = count;
exit:
	kfree(key);

	return ret;
}
static DEVICE_ATTR_WO(hdcp_key);

static const struct drm_encoder_helper_funcs dw_hdmi_rockchip_encoder_helper_funcs = {
	.mode_set   = dw_hdmi_rockchip_encoder_mode_set,
	.enable     = dw_hdmi_rockchip_encoder_enable,
	.disable    = dw_hdmi_rockchip_encoder_disable,
	.atomic_check = dw_hdmi_rockchip_encoder_atomic_check,
};

static struct rockchip_hdmi_chip_data rk3288_chip_data = {
	.lcdsel_grf_reg = RK3288_GRF_SOC_CON6,
	.lcdsel_big = HIWORD_UPDATE(0, RK3288_HDMI_LCDC_SEL),
	.lcdsel_lit = HIWORD_UPDATE(RK3288_HDMI_LCDC_SEL, RK3288_HDMI_LCDC_SEL),
};

static const struct dw_hdmi_plat_data rk3288_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rockchip_mode_valid,
	.mpll_cfg   = rockchip_mpll_cfg,
	.cur_ctr    = rockchip_cur_ctr,
	.phy_config = rockchip_phy_config,
	.phy_data = &rk3288_chip_data,
};

static struct rockchip_hdmi_chip_data rk3399_chip_data = {
	.lcdsel_grf_reg = RK3399_GRF_SOC_CON20,
	.lcdsel_big = HIWORD_UPDATE(0, RK3399_HDMI_LCDC_SEL),
	.lcdsel_lit = HIWORD_UPDATE(RK3399_HDMI_LCDC_SEL, RK3399_HDMI_LCDC_SEL),
};

static const struct dw_hdmi_plat_data rk3399_hdmi_drv_data = {
	.mode_valid = dw_hdmi_rockchip_mode_valid,
	.mpll_cfg   = rockchip_mpll_cfg,
	.cur_ctr    = rockchip_cur_ctr,
	.phy_config = rockchip_phy_config,
	.phy_data = &rk3399_chip_data,
};

static const struct of_device_id dw_hdmi_rockchip_dt_ids[] = {
	{ .compatible = "rockchip,rk3288-dw-hdmi",
	  .data = &rk3288_hdmi_drv_data
	},
	{ .compatible = "rockchip,rk3399-dw-hdmi",
	  .data = &rk3399_hdmi_drv_data
	},
	{},
};
MODULE_DEVICE_TABLE(of, dw_hdmi_rockchip_dt_ids);

static int dw_hdmi_rockchip_bind(struct device *dev, struct device *master,
				 void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct dw_hdmi_plat_data *plat_data;
	const struct of_device_id *match;
	struct drm_device *drm = data;
	struct drm_encoder *encoder;
	struct rockchip_hdmi *hdmi;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	hdmi = devm_kzalloc(&pdev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	match = of_match_node(dw_hdmi_rockchip_dt_ids, pdev->dev.of_node);
	plat_data = match->data;
	hdmi->dev = &pdev->dev;
	hdmi->chip_data = plat_data->phy_data;
	encoder = &hdmi->encoder;

	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm, dev->of_node);
	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (encoder->possible_crtcs == 0)
		return -EPROBE_DEFER;

	ret = rockchip_hdmi_parse_dt(hdmi);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev, "Unable to parse OF data\n");
		return ret;
	}

	ret = clk_prepare_enable(hdmi->vpll_clk);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev, "Failed to enable HDMI vpll: %d\n",
			      ret);
		return ret;
	}

	device_create_file(dev, &dev_attr_hdcp_key);

	drm_encoder_helper_add(encoder, &dw_hdmi_rockchip_encoder_helper_funcs);
	drm_encoder_init(drm, encoder, &dw_hdmi_rockchip_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);

	platform_set_drvdata(pdev, hdmi);

	hdmi->hdmi = dw_hdmi_bind(pdev, encoder, plat_data);

	/*
	 * If dw_hdmi_bind() fails we'll never call dw_hdmi_unbind(),
	 * which would have called the encoder cleanup.  Do it manually.
	 */
	if (IS_ERR(hdmi->hdmi)) {
		ret = PTR_ERR(hdmi->hdmi);
		drm_encoder_cleanup(encoder);
		clk_disable_unprepare(hdmi->vpll_clk);
	}

	return ret;
}

static void dw_hdmi_rockchip_unbind(struct device *dev, struct device *master,
				    void *data)
{
	struct rockchip_hdmi *hdmi = dev_get_drvdata(dev);

	dw_hdmi_unbind(hdmi->hdmi);
	clk_disable_unprepare(hdmi->vpll_clk);
}

static const struct component_ops dw_hdmi_rockchip_ops = {
	.bind	= dw_hdmi_rockchip_bind,
	.unbind	= dw_hdmi_rockchip_unbind,
};

static int dw_hdmi_rockchip_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &dw_hdmi_rockchip_ops);
}

static int dw_hdmi_rockchip_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dw_hdmi_rockchip_ops);

	return 0;
}

static int __maybe_unused dw_hdmi_rockchip_resume(struct device *dev)
{
	struct rockchip_hdmi *hdmi = dev_get_drvdata(dev);

	dw_hdmi_resume(hdmi->hdmi);

	return 0;
}

static const struct dev_pm_ops dw_hdmi_rockchip_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(NULL, dw_hdmi_rockchip_resume)
};

struct platform_driver dw_hdmi_rockchip_pltfm_driver = {
	.probe  = dw_hdmi_rockchip_probe,
	.remove = dw_hdmi_rockchip_remove,
	.driver = {
		.name = "dwhdmi-rockchip",
		.pm = &dw_hdmi_rockchip_pm,
		.of_match_table = dw_hdmi_rockchip_dt_ids,
	},
};
