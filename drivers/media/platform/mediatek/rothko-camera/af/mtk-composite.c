// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 MediaTek Inc. */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>

struct mtk_composite_v4l2_device {
	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
};

static int mtk_composite_complete(struct v4l2_async_notifier *notifier)
{
	return v4l2_device_register_subdev_nodes(notifier->v4l2_dev);
}

static const struct v4l2_async_notifier_operations mtk_composite_async_ops = {
	.complete = mtk_composite_complete,
};

static int mtk_composite_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_composite_v4l2_device *af;
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	af = devm_kzalloc(dev, sizeof(*af), GFP_KERNEL);
	if (!af)
		return -ENOMEM;

	af->media_dev.dev = dev;
	strscpy(af->media_dev.model, of_device_get_match_data(dev),
		sizeof(af->media_dev.model));
	media_device_init(&af->media_dev);
	af->v4l2_dev.mdev = &af->media_dev;

	ret = v4l2_device_register(dev, &af->v4l2_dev);
	if (ret)
		goto clean_media;

	v4l2_async_nf_init(&af->notifier, &af->v4l2_dev);
	af->notifier.ops = &mtk_composite_async_ops;
	fwnode_graph_for_each_endpoint(dev_fwnode(dev), ep) {
		asc = v4l2_async_nf_add_fwnode_remote(&af->notifier, ep,
						    struct v4l2_async_connection);
		if (IS_ERR(asc)) {
			fwnode_handle_put(ep);
			ret = PTR_ERR(asc);
			goto clean_notifier;
		}
	}

	ret = v4l2_async_nf_register(&af->notifier);
	if (ret)
		goto clean_notifier;

	ret = media_device_register(&af->media_dev);
	if (ret)
		goto unregister_notifier;

	platform_set_drvdata(pdev, af);
	return 0;

unregister_notifier:
	v4l2_async_nf_unregister(&af->notifier);
clean_notifier:
	v4l2_async_nf_cleanup(&af->notifier);
	v4l2_device_unregister(&af->v4l2_dev);
clean_media:
	media_device_cleanup(&af->media_dev);
	return ret;
}

static void mtk_composite_remove(struct platform_device *pdev)
{
	struct mtk_composite_v4l2_device *af = platform_get_drvdata(pdev);

	v4l2_async_nf_unregister(&af->notifier);
	v4l2_async_nf_cleanup(&af->notifier);
	v4l2_device_unregister(&af->v4l2_dev);
	media_device_unregister(&af->media_dev);
	media_device_cleanup(&af->media_dev);
}

static const struct of_device_id mtk_composite_of_match[] = {
	{ .compatible = "mediatek,mtk_composite_v4l2_2", .data = "mtk_camera_af" },
	{ .compatible = "mediatek,mtk_composite_v4l2_1", .data = "mtk-camera-flash" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_composite_of_match);

static struct platform_driver mtk_composite_platform_driver = {
	.probe = mtk_composite_probe,
	.remove = mtk_composite_remove,
	.driver = {
		.name = "mtk-composite-v4l2-2",
		.of_match_table = mtk_composite_of_match,
	},
};
module_platform_driver(mtk_composite_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MTK V4L2 Composite Driver");
