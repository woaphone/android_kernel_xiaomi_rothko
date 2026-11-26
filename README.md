# Redmi K70 Ultra (`rothko`) — Linux 6.18 port

Experimental kernel source for the MediaTek MT6989 Redmi K70 Ultra. This is the kernel **`common/`** repository, not a complete Android kernel workspace and not a flashable image. It is downstream work, not an official Xiaomi or Google release.

## Base and build

- Android Common Kernel: `android17-6.18`, base `76bdb627cab87dfa1d901810e22ee7fae6d3dc89`.
- Tested workspace references: AOSP `kernel/manifest` at `46a1a5274ff0b40419952c861a8958bba5f28cfc`, Kleaf (`kernel/build`) at `cdaf8a663270411b6864193b32c30f0166756677`.
- Start from the matching AOSP kernel manifest workspace, then check this port out as its `common/` checkout. Run from the workspace root:

```sh
tools/bazel run --config=fast --jobs=8 --lto=none   --incompatible_autoload_build_config_constants=if_unset   --defconfig_fragment=//common:rothko_kernelsu_defconfigs   //common:rothko_dist -- --destdir=out/rothko/dist
```

This is the development profile used in testing: it includes KernelSU and a permissive SELinux diagnostic configuration. It is **not a production security configuration**. The underlying working tree built the diagnostic kernel; this publication-only subset has not been independently built from a fresh workspace. Device-specific image packaging, keys, original modules and proprietary firmware/userspace are not included.

## Status

- Android boots on the tested device. Display, touch, and torch operation were observed; torch thermal-HAL compatibility is not complete.
- Ordinary CPU deep idle and sleep-related reboot remain unresolved. The latest source adds diagnostic observations, not a fix for either issue.
- Under-display fingerprint authentication is not implemented in this port. Do not treat this tree as a complete device bring-up.

## Source attribution

The port is based on [AOSP kernel/common](https://android.googlesource.com/kernel/common), with MediaTek code from published downstream trees including [OnePlusOSS MT6989](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_mt6989) (`d60d564c82de298fe4909657c31c29c73ba25aba`) and [Xiaomi OSS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) (`bsp-rothko-u-oss`). See also [MiCode MTK kernel modules](https://github.com/MiCode/MTK_kernel_modules). KernelSU is vendored at v3.3.1 (`0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d`); its license is in `drivers/kernelsu/LICENSE`. Existing notices have been retained; see `COPYING`, `LICENSES/` and the individual source files. Local build artifacts, personal logs, an unused restricted Mali header, and unused vendor Android packaging makefiles were excluded from this source release.

---

# How do I submit patches to Android Common Kernels

1. BEST: Make all of your changes to upstream Linux. If appropriate, backport to the stable releases.
   These patches will be merged automatically in the corresponding common kernels. If the patch is already
   in upstream Linux, post a backport of the patch that conforms to the patch requirements below.
   - Do not send patches upstream that contain only symbol exports. To be considered for upstream Linux,
additions of `EXPORT_SYMBOL_GPL()` require an in-tree modular driver that uses the symbol -- so include
the new driver or changes to an existing driver in the same patchset as the export.
   - When sending patches upstream, the commit message must contain a clear case for why the patch
is needed and beneficial to the community. Enabling out-of-tree drivers or functionality is not
a persuasive case.

2. LESS GOOD: Develop your patches out-of-tree (from an upstream Linux point-of-view). Unless these are
   fixing an Android-specific bug, these are very unlikely to be accepted unless they have been
   coordinated with kernel-team@android.com. If you want to proceed, post a patch that conforms to the
   patch requirements below.

# Common Kernel patch requirements

- All patches must conform to the Linux kernel coding standards and pass `scripts/checkpatch.pl`
- Patches shall not break gki_defconfig or allmodconfig builds for arm, arm64, x86, x86_64 architectures
(see  https://source.android.com/setup/build/building-kernels)
- If the patch is not merged from an upstream branch, the subject must be tagged with the type of patch:
`UPSTREAM:`, `BACKPORT:`, `FROMGIT:`, `FROMLIST:`, or `ANDROID:`.
- All patches must have a `Change-Id:` tag (see https://gerrit-review.googlesource.com/Documentation/user-changeid.html)
- If an Android bug has been assigned, there must be a `Bug:` tag.
- All patches must have a `Signed-off-by:` tag by the author and the submitter

Additional requirements are listed below based on patch type

## Requirements for backports from mainline Linux: `UPSTREAM:`, `BACKPORT:`

- If the patch is a cherry-pick from Linux mainline with no changes at all
    - tag the patch subject with `UPSTREAM:`.
    - add upstream commit information with a `(cherry picked from commit ...)` line
    - if applicable, prefer to cherry-pick the commit from the corresponding LTS branch.
    - append new signature tags (e.g. `Bug:`, `Change-Id:`, etc.) at the end to keep the
      chronological order.
    - Example:
        - if the upstream commit message is
```
        important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>
```
>- then Joe Smith would upload the patch for the common kernel as
```
        UPSTREAM: important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>

        Bug: 135791357
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        (cherry picked from commit c31e73121f4c1ec41143423ac6ce3ce6dafdcec1)
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```

- If the patch requires any changes from the upstream version, tag the patch with `BACKPORT:`
instead of `UPSTREAM:`.
    - use the same tags as `UPSTREAM:`
    - add comments about the changes under the `(cherry picked from commit ...)` line
    - Example:
```
        BACKPORT: important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>

        Bug: 135791357
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        (cherry picked from commit c31e73121f4c1ec41143423ac6ce3ce6dafdcec1)
        [joe: Resolved minor conflict in drivers/foo/bar.c ]
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```

## Requirements for other backports: `FROMGIT:`, `FROMLIST:`,

- If the patch has been merged into an upstream maintainer tree, but has not yet
been merged into Linux mainline
    - tag the patch subject with `FROMGIT:`
    - add info on where the patch came from as `(cherry picked from commit <sha1> <repo> <branch>)`.
This must be a branch on a tree which is normally merged into Linus's tree and is not rebased. For
example, don't use `linux-next` which is rebased and never directly merged into Linus's tree, but
you *can* use SHAs from `net` *or* `net-next`, which are merged into Linus's tree at various points
in the release.
    - if changes were required, use `BACKPORT: FROMGIT:`
    - Example:
        - if the commit message in the maintainer tree is
```
        important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>
```
>- then Joe Smith would upload the patch for the common kernel as
```
        FROMGIT: important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>

        Bug: 135791357
        (cherry picked from commit 878a2fd9de10b03d11d2f622250285c7e63deace
         https://git.kernel.org/pub/scm/linux/kernel/git/foo/bar.git test-branch)
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```


- If the patch has been submitted to LKML, but not accepted into any maintainer tree
    - tag the patch subject with `FROMLIST:`
    - add a `Link:` tag with a link to the submittal on lore.kernel.org
    - add a `Bug:` tag with the Android bug (required for patches not accepted into
a maintainer tree)
    - if changes were required, use `BACKPORT: FROMLIST:`
    - Example:
```
        FROMLIST: important patch from upstream

        This is the detailed description of the important patch

        Signed-off-by: Fred Jones <fred.jones@foo.org>

        Bug: 135791357
        Link: https://lore.kernel.org/lkml/20190619171517.GA17557@someone.com/
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```

- If a patch has been submitted to the community, but rejected, do NOT use the
  `FROMLIST:` tag to try to hide this fact.  Use the `ANDROID:` tag as
  described below as this must be considered as an Android-specific submission,
  not an upstream submission as the community will not accept these changes
  as-is.

## Requirements for Android-specific patches: `ANDROID:`

- If the patch is fixing a bug to Android-specific code
    - tag the patch subject with `ANDROID:`
    - add a `Fixes:` tag that cites the patch with the bug
    - Example:
```
        ANDROID: fix android-specific bug in foobar.c

        This is the detailed description of the important fix

        Fixes: 1234abcd2468 ("foobar: add cool feature")
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```

- If the patch is a new feature
    - tag the patch subject with `ANDROID:`
    - add a `Bug:` tag with the Android bug (required for android-specific features)

## Requirements for revert patches:

- Add a reason for the revert
- Do not delete or modify the revert information that is generated when using
`git revert`
- If modifications have been made after creating the revert, include a list of
these in the commit message
- Example:
```
        Revert "ANDROID: fix android-specific bug in foobar.c"

        This reverts commit a57a7913f53e34c8a8d905444b126b3316146e69.

        Reason for revert: Breaks a lot of internal tests

        Additional modifications: Resolved merge conflicts

        Bug: 135791357
        Change-Id: I4caaaa566ea080fa148c5e768bb1a0b6f7201c01
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```
