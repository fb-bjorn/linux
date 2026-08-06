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

## Fragments vs materialized configs

The fragments are **not** what the kernel is built with. The build consumes a
checked-in, fully-resolved **materialized** config per flavor at
`facebook/config/materialized/config-<arch>-<flavor>.config`. Each is the final
`.config` produced by merging the flavor's fragments and resolving them with the
build container's toolchain (`scripts/kconfig/merge_config.sh` +
`make LLVM=1 alldefconfig`, run in the build container), so the committed config
matches what the kernel is actually built with.

A branch can opt out by not having a `facebook/config/materialized/` directory
at all: it then carries only the fragments and resolves them in the container at
build time, and the build prints a warning saying so. That suits branches where
the staleness check would fire constantly for no useful reason — an
upstream-nightly branch, say, goes stale on every new symbol added upstream.
Branches that are meant to have materialized configs should not rely on this;
there, a missing config is an error rather than a fallback.

Workflow for any config change:

1. Edit the fragment(s) in `facebook/config/*.config`.
2. Regenerate: `facebook/scripts/materializeconfigs` (rewrites the materialized
   files). Use `--check` to only verify they are up to date.
3. Commit the fragments **and** the regenerated materialized files together.

Do not hand-edit the materialized files. Verification happens whenever a config
is consumed: `//facebook/config:<arch>-<flavor>` regenerates from the fragments
and fails the build (with a regenerate hint and a diff) if the committed
materialized file is stale — so CI catches a forgotten regeneration, and any
consumer of a config is covered without having to opt in.

Note this also fires on kernel-side churn, not just fragment edits: an upstream
change that adds a symbol or flips a Kconfig default changes the resolved output,
so the materialized files go stale and every build fails until you rerun
`materializeconfigs` and commit the result. Merges that touch Kconfig should
include that regeneration.

The materialized files also capture toolchain-derived values (compiler version
strings like `CONFIG_CC_VERSION_TEXT`, `CLANG_VERSION`, and the `CC_HAS_*` set),
so bumping the build container's clang/lld/pahole makes them stale the same way —
regenerate after a toolchain roll.
