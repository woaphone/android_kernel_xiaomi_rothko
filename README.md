# 红米 K70 至尊版（rothko）· ACK 6.18 内核移植

这是面向红米 K70 至尊版（`rothko`，MediaTek MT6989）的实验性内核移植，基于 **Android Common Kernel（ACK）6.18.33**，上游分支为 `android17-6.18`。实机验证使用的是 **Android 15 用户态**；`android17-6.18` 是内核源码分支名，不代表本项目移植了 Android 17 ROM。

本仓库是 ACK 工作区的 **`common/` 源码树**，不包含完整 Android 系统、可直接刷入的 `boot.img`、签名私钥、原厂 HAL 或固件。本项目不是小米或 Google 的官方发布，也不建议在未保留恢复方案的日用设备上部署。

## 功能与实测状态

下表汇总的是同一台 `rothko` 在**不同阶段构建**中的验证结果，不表示每项功能都在当前公开提交上重新测过。已接入驱动、设备已枚举与完整可用分别记录。

| 功能 | 当前状态 |
| --- | --- |
| Android 启动、USB 调试 | 实机进入 Android，USB ADB 可用。ADB 正常不等于所有 USB 模式、Type-C 附件或 OTG 均已验收。 |
| 显示、触控与按键 | 实体屏、Goodix 触控和物理按键已恢复；Mali、DRM/HWC 与 N12 面板已有适配。不同面板批次和 AOD 长待机仍需回归。 |
| 手电筒 | 实体点亮已确认。OCP81373 常亮路径接入了外置电量计和实际功率预算，保留低预算保护。相机拍照主闪、完整热控 ABI 尚未验收。 |
| Wi-Fi | MT6639 已在实机连接并取得收发记录；系统睡眠后的恢复与长待机仍待验证。 |
| 蓝牙 | 已在阶段构建中验证连接恢复；未覆盖所有编解码器、外设和长时间回归。 |
| 音频 | 基础音频已由实机确认可用；不代表 DSP 压缩卸载和所有音频周边路径均通过。 |
| 蜂窝网络 | 移动数据、短信在阶段构建中实测正常；通话、IMS/VoLTE 和完整运营商兼容性未作全面验收。 |
| 相机和媒体 | 相机已有实机初测，前摄有真实出帧记录；Codec2 硬解与 MDP 后处理已有任务回执。全摄像头、拍照闪光、录像、编解码组合及性能仍需逐项测试。 |
| NFC、GNSS、传感器、振动 | 有相应驱动或接口适配；不能用节点出现代替读卡、定位、传感器覆盖、振感及 eSE 业务验收。 |
| 电池与温控 | 外置 BMS 电量、温度和 PPB 预算已在实机读取；接入板级热控与充电相关驱动，完整充电策略见下节。 |
| KernelSU、pstore | 开发构建可选内置 KernelSU；普通 ramoops/pstore 用于保留重启和睡眠诊断信息。 |

## 三项主要遗留

1. **息屏睡死／睡眠后自动重启**：系统能走到 `s2idle` 的部分阶段，但曾在挂起等待或恢复附近出现异常。诊断版增加了等待、唤醒通知和返回标记；尚无完整回归证明问题已修复。
2. **普通 CPU 深态**：实机普通空闲采样中，各核深态进入次数与驻留时间仍未增长，曾记录 PSCI 固件返回 `-2`。已补 MCUPM/MCDI 校时及握手观测，尚未恢复稳定驻留。它与系统 `s2idle` 是两项不同的验收。
3. **充电与原厂快充**：基础上报、固定档 USB PD 协商和充电在阶段构建中有实测记录，不是完全不能充。原厂级双 SC858x 充电泵、PPS 直充、温控限流与降档等尚未完整实现或验收，不宣称达到原厂充电功率。

**另有未完成项：**屏下指纹目前未接入物理指纹驱动，虚拟 HAL 不算解锁可用；OrangeFox Recovery 尚未完成实机闭环。全功能和功耗、性能也未完成全面原厂对照。

## 构建

- ACK 基线：[`76bdb627cab87dfa1d901810e22ee7fae6d3dc89`](https://android.googlesource.com/kernel/common/+/76bdb627cab87dfa1d901810e22ee7fae6d3dc89)。
- 已使用的工作区参考：AOSP `kernel/manifest` 的 `46a1a5274ff0b40419952c861a8958bba5f28cfc`，Kleaf（`kernel/build`）的 `cdaf8a663270411b6864193b32c30f0166756677`。
- 准备匹配的 [AOSP 内核构建工作区](https://source.android.com/docs/setup/build/building-kernels)，将本仓库的 `rothko-android17-6.18` 分支用作工作区的 `common/`，从工作区根目录运行：

```sh
tools/bazel run --config=fast --jobs=8 --lto=none \
  --incompatible_autoload_build_config_constants=if_unset \
  --defconfig_fragment=//common:rothko_kernelsu_defconfigs \
  //common:rothko_dist -- --destdir=out/rothko/dist
```

产物在 `out/rothko/dist/`。上述是项目用过的**诊断配置**，含 KernelSU 和 SELinux 宽容诊断选项，并非生产安全配置。原开发树的诊断内核已构建成功；当前公开源码未在全新工作区独立复建。内核构建产物也不等于可直接刷机的启动镜像。

## 源码来源与许可

- 上游：[AOSP kernel/common](https://android.googlesource.com/kernel/common)；上游补丁规则见 [ACK 原 README](https://android.googlesource.com/kernel/common/+/76bdb627cab87dfa1d901810e22ee7fae6d3dc89/README.md)。
- MT6989 下游参考及移植来源包含 [OnePlusOSS MT6989](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_mt6989)（`d60d564c82de298fe4909657c31c29c73ba25aba`）、[小米开源内核](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)（`bsp-rothko-u-oss`）和 [MiCode MTK kernel modules](https://github.com/MiCode/MTK_kernel_modules)。
- KernelSU 采用 v3.3.1（`0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d`），许可见 [`drivers/kernelsu/LICENSE`](drivers/kernelsu/LICENSE)。内核及第三方代码保留原版权和许可声明，参见 [`COPYING`](COPYING)、[`LICENSES/`](LICENSES/) 和各文件声明。
