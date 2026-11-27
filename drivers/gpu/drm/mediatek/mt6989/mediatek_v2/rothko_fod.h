/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ROTHKO_FOD_H__
#define __ROTHKO_FOD_H__

struct device;
struct drm_device;
struct drm_crtc;

struct device *rothko_fod_device_create(void);
void rothko_fod_device_destroy(struct device *dev);
int rothko_fod_register(struct drm_device *drm);
void rothko_fod_unregister(struct drm_device *drm);
void rothko_fod_backlight_done(struct drm_crtc *crtc, unsigned int dbv);

#endif
