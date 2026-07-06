# YukiZygisk Kernel LKM

This directory is the first standalone extraction point for the kernel-side
YukiZygisk code currently implemented inside YukiSU.

Current state:

- `feature/zygote_*.c` and matching headers are imported from YukiSU commit
  `754182ed55923beba83484fa6624250a8c78cf39`.
- `core/module.c` provides an independent module entry/exit path.
- `core/control.c` exposes a standalone `/dev/yukizygisk` ioctl surface for
  the YukiZygisk UAPI.
- `host/host.h` and `host/adapter.c` are the temporary host adapter. They
  preserve source-level build boundaries while the remaining SELinux,
  root-backend, and daemon integrations are extracted.
- `host/runtime.c` and `host/root_impl.c` reuse Kasumi's runtime symbol
  resolver and root implementation detector under YukiZygisk naming.
- `host/lsm.c` provides the versioned LSM hook backend for the AT_ENTRY
  interception point. It patches `static_calls_table` on 6.12+ and
  `security_hook_heads` on older kernels.
- `host/patch_text.c` provides the arm64 patch primitive used by the LSM
  backend.

This is not yet a complete replacement for the YukiSU-integrated module. KSU,
KernelPatch, APatch, Magisk, and other root stacks should be modeled as host
backends rather than core dependencies. The remaining hard dependencies are
documented in `docs/source-inventory.md`.

The standalone ioctl ABI uses only `YZ_IOCTL_*` with magic `'Y'`. Do not add
`KSU_IOCTL_YZ_*` or magic `'K'` here; that ABI remains with the integrated
YukiSU/YukiZygisk module.

Build shape:

```bash
make -C kernel KDIR=/path/to/android/kernel/build
```

The command needs an Android kernel build tree or DDK-provided kernel build
directory matching the target kernel.
