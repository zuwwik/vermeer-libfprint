/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "fpi-device.h"
#include <vermeer-fingerprint.h>
#include "illumination.h"
G_DECLARE_FINAL_TYPE (FpiDeviceGoodixVermeer, fpi_device_goodixvermeer, FPI, DEVICE_GOODIXVERMEER, FpDevice)
struct _FpiDeviceGoodixVermeer {
  FpDevice parent;
  struct vfp_device *native;
  VermeerIllumination *ui;
};
