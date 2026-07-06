# YukiZygisk

A new kernel-level Zygisk implementation designed to explore better, cleaner, and more flexible ways of injecting.

## Project Notes

This repository is the extraction target for the YukiZygisk work currently living in YukiSU.

Start here:

- [AGENTS.md](AGENTS.md) - working rules for agents and maintainers.
- [docs/project-state.md](docs/project-state.md) - current YukiSU source state and verified capability snapshot.
- [docs/architecture.md](docs/architecture.md) - current architecture boundaries and source map.
- [docs/extraction-plan.md](docs/extraction-plan.md) - staged plan for turning the YukiSU branch into a standalone project.
- [docs/source-inventory.md](docs/source-inventory.md) - exact imported source files and remaining host dependencies.
- [docs/validation-and-device-safety.md](docs/validation-and-device-safety.md) - build checks, CI checks, and device-operation constraints.

## Kernel LKM Skeleton

The first standalone kernel skeleton lives in [kernel](kernel). It imports the
current YukiSU YukiZygisk kernel feature files and builds an Android
`android16-6.12` LKM through DDK:

```bash
ddk build -- W=1
```

This is a buildable extraction checkpoint, not a complete runtime replacement
for the YukiSU-integrated module yet. See the source inventory for the
remaining setresuid/syscall, SELinux policy, mount cleanup, and userspace
control adapters.

The standalone design target is root-implementation agnostic. KSU/YukiSU,
KernelPatch, APatch, Magisk, and other high-CAP root environments should be
host backends, not project owners. Kernel code should call YukiZygisk-owned
`yz_*` and `yz_host_*` interfaces. Host initialization now reuses the
Kasumi runtime resolver and root implementation detector under YukiZygisk
naming.

The LSM interception point is now extracted into the standalone host layer:
`selinux_bprm_committed_creds` is patched through a versioned adapter that uses
the 6.12+ `static_calls_table` path or the older `security_hook_heads` path.

The standalone control ABI is `YZ_IOCTL_*` with ioctl magic `'Y'` only. It does
not accept the integrated YukiSU/YukiZygisk `KSU_IOCTL_YZ_*`/`'K'` ABI.
