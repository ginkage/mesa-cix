# mesa-cix: Mesa for the CIX P1 vendor GPU stack

This is Armbian's Mesa for Ubuntu 26.04 "resolute" plus zink work for Arm's proprietary Mali driver (libmali r54p1 on the CIX P1 / CD8180 Mali-G720-Immortalis). libmali has no desktop GL, so desktop GL runs as zink on libmali's Vulkan; with these changes that is **GL 4.6** (stock Mesa: 4.0). One build serves both GPU stacks: the vendor one (`mali_kbase` + libmali) and the upstream one (Panthor + Panfrost/PanVK), which these changes don't touch. Mesa's own README is [README.rst](README.rst).

It is one part of the vendor stack; install the whole stack with the script from [libmali-cix](https://github.com/ginkage/libmali-cix#install-the-whole-stack).

## Branches

- `armbian/resolute`: the exact source of Armbian's Mesa build (`27.0.0~git202610251245.e737139f66-1+armbian1.1~resolute`): [mojyack/mesa](https://gitlab.freedesktop.org/mojyack/mesa) `t210-develop` at `e737139f66e`, plus the two source patches of Debian's `26.2.6-1` packaging.
- `cix` (default): `armbian/resolute` plus the changes below and the CI.

## Changes on `cix`

Every Mali-specific change is gated on the Vulkan driver ID `VK_DRIVER_ID_ARM_PROPRIETARY`, so zink on other drivers (PanVK included) behaves as before.

- From Arm's and CIX's zink-on-Mali series in [cixtech/mesa](https://github.com/cixtech/mesa) `cix_mesa-25.1.5`, ported or rewritten for this Mesa: cull distance, multiViewport, fillModeNonSolid and alphaToOne emulation, drawing from instance 0 without `supportsNonZeroFirstInstance`, the L4A4 swap workaround, async submit on Wayland, and generic fixes (blit FS output type, storesAndAtomics image limit, robustImageAccess2, isnan, query copy barrier, tc renderpass invalidation, kopper attached sizes).
- Fixes found here: TCS built-ins in a `gl_PerVertex` block (libmali draws nothing otherwise), no varying packing into the position output, no FS front/back colour aliasing, array/per-slot I/O alignment, no per-frame wait before presenting (the Mali WSI layer waits), finishing frames rendered to shared dma-bufs before presenting them (kbase has no implicit sync; zink's DRI3 path without kopper), and two generic bugs (64-bit inlined system values, flat point values in the passthrough GS) plus GL points lowering in a GS.

`git log armbian/resolute..cix` lists them; each commit message explains its change.

## Build

`ci/build-deb.sh <work dir> [--install-deps]` builds the Debian packages the way Armbian does (Debian's packaging, Armbian's packaging patches, the meson subprojects Armbian downloads, all drivers). The version is Armbian's plus `+cix<commit date>`, so every newer commit sorts higher, and above Armbian's own build. CI runs it in an `ubuntu:26.04` container for every push to `cix` and publishes the packages as a GitHub release; a commit with `[skip ci]` in its message doesn't build.

When Armbian moves to a newer Mesa, `armbian/resolute` and `cix` are rebased and the three pins at the top of `ci/build-deb.sh` updated.
