# Kernel Build Flavors and Target Determinator

## Build Flavors

Single source of truth for all buildable kernel flavors: `facebook/bzl/flavors.td.bzl`. It defines `ARCH_FLAVORS` (full list of flavors per architecture) and `BUILD_ONLY_FLAVORS`. Loaded by:
- `TARGETS` (root) — generates buck2 kernel build targets
- `facebook/config/TARGETS` — generates kernel config targets
- `facebook/determinator.td` — the target determinator

## Target Determinator

The target determinator lives at `facebook/determinator.td`. It controls which Skycastle builds get triggered when a diff is submitted via `jf submit`.

The determinator does NOT build all flavors from `ARCH_FLAVORS`. It has its own `TD_FLAVORS` dict that defines a reduced subset of flavors to build at diff time. This separation exists so flavors can remain buildable via buck2 without being triggered on every diff.

The determinator is invoked as `SandcastleKernelDeterminatorCommand` (alias `linux-kernel-determinator`), configured in `.jfconfig`.

**To change which flavors build on diffs**, edit `TD_FLAVORS` in `facebook/determinator.td`.
**To add/remove a flavor entirely**, edit `ARCH_FLAVORS` in `facebook/bzl/flavors.td.bzl`.
