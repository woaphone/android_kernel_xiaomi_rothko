# 红米 K70 至尊版（rothko）· ACK 6.18 内核移植

这是面向红米 K70 至尊版（`rothko`，MediaTek MT6989）的实验性内核移植，基于 **Android Common Kernel（ACK）6.18.33**，上游分支为 `android17-6.18`。实机主要在 **Android 15（HyperOS 2，NoGZ 普通 VHE）** 上验证，同时也在 Android 14–18 环境进行过兼容性适配；`android17-6.18` 是上游内核分支名，不代表本项目移植了 Android 17 ROM。

本仓库是 ACK 工作区的 **`common/` 源码树**，不包含完整 Android 系统、签名私钥、原厂 HAL 或固件。当前 `rothko-android17-6.18` 分支为**单 `boot.img` 整核内建形态**（将 MT6989/rothko 驱动编入内核）；拆分为标准 GKI `boot` + `vendor_boot`（DTB 与首阶段 `.ko`）+ `vendor_dlkm` + `system_dlkm` 的模块化工作正在独立新分支推进。本项目不是小米或 Google 的官方发布，请务必保留备份与恢复手段。

## 功能与当前状态

下表按同一台 `rothko` 的最新实机验证与离线接入进度如实区分**已实机验证**、**部分可用**与**刚接入待实测**状态：

| 功能 | 当前状态 |
| --- | --- |
| Android 启动、NoGZ VHE 与 MiTEE | 已实机验证。兼容 Android 15 `libvintf` 内核命名；NoGZ 普通 VHE 启动正常，FF-A / MiTEE（`0x8001`）、Gatekeeper 与 KeyMint 正常绑定。 |
| USB 调试、网络共享与 Recovery | 已实机验证。USB ADB 与 MTK RNDIS USB 共享网络可用；OrangeFox Recovery 混合 LZ4+gzip initramfs 解包与 DVFSRC 路径已修复。 |
| 显示、高刷与 AOD | 已实机验证。0A/0B 面板点亮、常规刷新率与 VIS 高刷同步、普通 AOD 正常；包含实验性 165/185 Hz 测试档。 |
| 屏下指纹与触控 | 已实机验证（亮屏与普通 AOD）。物理指纹驱动 `xiaomi_spi_tee`（`/dev/xiaomi-fp`）、`/dev/mi_display/disp_feature` 局部高亮与 Goodix FOD 触控已打通，亮屏与普通 AOD 解锁可用；全黑深度休眠息屏指纹仍待完善。 |
| 触觉马达 | 已实机验证基础与侧滑返回触觉。AW86927 实测 LRA F0 频率并暴露只读 `f0_value` 节点，侧滑返回与系统预置波形已恢复；流式 RTP / `custom_wave` 仍在核对。 |
| 普通 CPU 深态与调度 | 已实机验证。已恢复 MCUPM Alive / 校时与 MCDI phase 3，八核 MCUSYS-off / system-vcore 普通空闲深态驻留已恢复增长；MT6989 CCI 双档联动调频已补齐；内置只读默认状态的 MCUPM UV32 通道。 |
| 电池、双泵 PPS 与 120W 快充 | **双泵 PPS 已实机验证；小米 120W 私有协议刚离线接入待实测**。外置 BQ27xxx BMS 电量/温度/电流正负号已校准；双 SC858x 4:1 电荷泵 PPS 直充已实机启泵带载，状态栏快充与 55 W 锁屏瓦数接口已恢复。`9b485e15` 新增 `Charging_Adapter/pd_adapter` 15 节点、`"SNK_Ready"` 映射、SVID `0x2717`/`0x2b01` UVDM 与 `MTK_PD_APDO_REGAIN` 120 W 策略，待原装 120 W 充电器实机验证。 |
| Wi-Fi、热点与蓝牙 | 已实机验证基础功能。MT6639 Wi-Fi（含 PCIe 父子拓扑与休眠恢复修复）、热点 ACS/P2P 尾数组与蓝牙连接可用；长待机与全编解码回归仍需继续。 |
| 手电筒 | 已实机验证常亮。OCP81373 常亮路径已接入外置电量计与 PPB 实际电池预算；相机拍照主闪脉冲与完整热控 ABI 仍待完善。 |
| 音频、蜂窝与相机/媒体 | 部分实机验证。基础音频、移动数据与短信、前摄出帧及 Codec2 硬解已有实测记录；OS4 VOW 大模型共享区、全摄像头/录像、通话与 IMS 仍需逐项验收。 |
| NFC、GNSS、传感器 | 已接入驱动与接口；完整读卡、eSE、定位与全传感器长期稳定性仍需实测验收。 |
| KernelSU 与 pstore | 测试构建启用 KernelSU（v3.3.1）与普通 ramoops/pstore（保留重启与睡眠诊断信息）。 |

## 已知遗留与下一步

1. **小米 120 W 私有快充实机验收**：`9b485e15` 已离线完成 `batterysecret` UVDM 认证与 20 V / 6 A 双泵放开逻辑，仍待原装 120 W 充电器实测握手、满载温控、拔线退档与第三方 PPS 回归。
2. **系统深度睡眠（`s2idle`）长待机与全黑息屏指纹**：普通 CPU 深态与 MT6639 `noirq` 拓扑已修复，但长时间 `s2idle` 待机耗电、Wi-Fi 唤醒与全黑状态息屏指纹仍需持续回归。
3. **多媒体与外设长尾项**：OS4 VOW 模型尺寸兼容、AW86927 `custom_wave` 流式触觉、相机多摄与拍照主闪脉冲等。
4. **标准 GKI 四分区拆分（进行中）**：正在独立新分支将当前整核形态拆分为标准 GKI `boot` + `vendor_boot`（DTB + 首阶段 `.ko`）+ `vendor_dlkm` + `system_dlkm`。

## 构建

- ACK 基线：[`76bdb627cab87dfa1d901810e22ee7fae6d3dc89`](https://android.googlesource.com/kernel/common/+/76bdb627cab87dfa1d901810e22ee7fae6d3dc89)。
- 已使用的工作区参考：AOSP `kernel/manifest` 的 `46a1a5274ff0b40419952c861a8958bba5f28cfc`，Kleaf（`kernel/build`）的 `cdaf8a663270411b6864193b32c30f0166756677`。
- 准备匹配的 [AOSP 内核构建工作区](https://source.android.com/docs/setup/build/building-kernels)，将本仓库的 `rothko-android17-6.18` 分支用作工作区的 `common/`，从工作区根目录运行：

```sh
tools/bazel run --config=fast --jobs=8 --lto=none \
  --incompatible_autoload_build_config_constants=if_unset \
  --defconfig_fragment=//common:rothko_nogz_vhe_n12_power_defconfigs \
  //common:rothko_dist -- --destdir=out/rothko/dist
```

产物位于 `out/rothko/dist/`（包含 `Image.lz4`、`vmlinux` 与少量 `.ko`）。上述配置面向 NoGZ 普通 VHE 测试环境，含 KernelSU 与诊断选项，并非生产安全配置。

## 源码来源与许可

- 上游：[AOSP kernel/common](https://android.googlesource.com/kernel/common)；上游补丁规则见 [ACK 原 README](https://android.googlesource.com/kernel/common/+/76bdb627cab87dfa1d901810e22ee7fae6d3dc89/README.md)。
- MT6989 下游参考及移植来源包含 [OnePlusOSS MT6989](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_mt6989)（`d60d564c82de298fe4909657c31c29c73ba25aba`）、[小米开源内核](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)（`bsp-rothko-u-oss`）和 [MiCode MTK kernel modules](https://github.com/MiCode/MTK_kernel_modules)。
- KernelSU 采用 v3.3.1（`0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d`），许可见 [`drivers/kernelsu/LICENSE`](drivers/kernelsu/LICENSE)。内核及第三方代码保留原版权和许可声明，参见 [`COPYING`](COPYING)、[`LICENSES/`](LICENSES/) 和各文件声明。
