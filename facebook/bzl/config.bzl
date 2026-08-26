load(":container.bzl", "container_genrule", "target_container_image")
load(":flavors.td.bzl", "ARCHITECTURE_TO_CONFIG_ARCH", "ARCH_FLAVORS", "ARCHITECTURES")
load(":constants.bzl", "SELFTESTS", "SelftestsType")

def validate_flavor(arch, flavor):
    if flavor not in ARCH_FLAVORS[arch] + ["lol2"]:
        fail(
            "{} not an allowed flavor, allowed flavors for {} arch: {}".format(
                flavor,
                arch,
                ARCH_FLAVORS[arch]
            )
        )

def config_name(arch, flavor = None, selftests: SelftestsType = SelftestsType("none"), replace_arch = True):
    config_name = ARCHITECTURE_TO_CONFIG_ARCH[arch] if replace_arch else arch

    if flavor:
        validate_flavor(arch, flavor)
        config_name += "-" + flavor

    if selftests == SelftestsType("all"):
        config_name += "-selftests"
    elif selftests == SelftestsType("bpf"):
        config_name += "-bpf-selftests"

    return config_name

def _fragments(carch, flavor):
    if flavor == "lol2":
        return ["flavor-lol2-{}.config".format(carch)]

    frags = ["common.config", "arch-{}.config".format(carch)]
    if flavor != "default":
        frags.append("flavor-{}-{}.config".format(flavor, carch))
    return frags

_MERGE_CONFIG = """set -e
cd /ro/source
export LLVM=1
export ARCH={carch}
export KCONFIG_WARN_UNKNOWN_SYMBOLS=1
export KCONFIG_CONFIG=/rw/out/.config
ret=0
./scripts/kconfig/merge_config.sh -O /rw/work {fragments} > /rw/work/merge.log 2>&1 || ret=$?

# TODO: some of the warnings from merge_config.sh should be fatal,
# but for now just print the warnings and exit.
cat /rw/work/merge.log >&2

exit $ret
"""

def generated_config(arch, flavor = None):
    """The .config resolved on-the-fly from the fragments.

    Notice we have :<cfg>-generate and :<cfg>-generated rules. The first does
    the merging of fragments inside the container and the second one pulls the
    generated .config out of it.
    """
    carch = ARCHITECTURE_TO_CONFIG_ARCH[arch]
    cfg = config_name(arch = arch, flavor = flavor)
    fragments = _fragments(carch, flavor if flavor else "default")

    container_genrule(
        name = cfg + "-generate",
        arch = arch,
        # Use native toolchain to avoid toolchain-derived configs flagged as stale when cross-compiling.
        image_override = "$(location {})".format(target_container_image(arch)),
        cmd = _MERGE_CONFIG.format(
            carch = carch,
            fragments = " ".join(["/ro/frags/" + f for f in fragments]),
        ),
        bind_ro = [
            ("//:sources", "/ro/source"),
            ("//facebook/config:files", "/ro/frags"),
        ],
        bind_rw = [
            ("$OUT", "/rw/out"),
            ("$TMP", "/rw/work"),
        ],
        cacheable = True,
    )

    native.genrule(
        name = cfg + "-generated",
        cmd = "cp $(location :{}-generate)/.config $OUT".format(cfg),
        out = "config",
        labels = ["linux_kernel"],
    )

def generate_all():
    copies = []
    for arch in ARCHITECTURES:
        for flavor in [None] + ARCH_FLAVORS[arch]:
            cfg = config_name(arch = arch, flavor = flavor)
            copies.append("cp $(location :{cfg}-generated) $OUT/config-{cfg}.config".format(cfg = cfg))

    native.genrule(
        name = "generate-all",
        cmd = "mkdir -p $OUT && " + " && ".join(copies),
        out = ".",
        labels = ["linux_kernel"],
    )

def _base_config(ctx: AnalysisContext) -> Artifact:
    prepare_config_script = ctx.attrs._prepareconfig[RunInfo]
    base_config = ctx.actions.declare_output("base.config")

    config_files_dir = ctx.actions.copied_dir(
        "config_dir",
        {s.basename: s for s in ctx.attrs.srcs},
    )

    ctx.actions.run(
        cmd_args(
            "bash",
            "-c",
            cmd_args(
                prepare_config_script,
                "-a",
                ARCHITECTURE_TO_CONFIG_ARCH[ctx.attrs.arch],
                "-f",
                ctx.attrs.flavor,
                "-p",
                config_files_dir,
                ">",
                base_config.as_output(),
                delimiter = " ",
            )
        ),
        category = "base",
    )

    return base_config

def _selftests_config(ctx: AnalysisContext) -> Artifact:
    selftests_config_script = ctx.attrs._selftestsconfig[RunInfo]
    selftests_config = ctx.actions.declare_output("selftests.config")

    ctx.actions.run(
        cmd_args(
            "bash",
            "-c",
            cmd_args(
                selftests_config_script,
                "-a",
                ctx.attrs.arch,
                " ".join(SELFTESTS),
                ">>",
                selftests_config.as_output(),
                delimiter = " ",
            )
        ),
        category = "selftests",
        local_only = True,
    )

    return selftests_config

def _bpf_selftests_config(ctx: AnalysisContext) -> Artifact:
    selftests_config = ctx.actions.declare_output("selftests.config")
    ctx.actions.run(
        cmd_args(
            "bash",
            "-c",
            cmd_args(
                "cat",
                "tools/testing/selftests/bpf/config",
                "tools/testing/selftests/bpf/config.vm",
                "tools/testing/selftests/bpf/config.x86_64",
                ">",
                selftests_config.as_output(),
                delimiter = " ",
            ),
        ),
        category = "selftests",
        local_only = True,
    )

    return selftests_config

def _impl(ctx: AnalysisContext) -> list[Provider]:
    base_config = _base_config(ctx)
    selftests_type = SelftestsType(ctx.attrs.selftests)

    if selftests_type == SelftestsType("none"):
        return [
            DefaultInfo(
                default_output = ctx.actions.copy_file(".config", base_config),
            )
        ]

    selftests_config = _selftests_config(ctx) if selftests_type != SelftestsType("bpf") else _bpf_selftests_config(ctx)

    kernel_config = ctx.actions.declare_output(".config")
    ctx.actions.run(
        cmd_args(
            "bash",
            "-c",
            cmd_args(
                "cat",
                base_config,
                selftests_config,
                ">",
                kernel_config.as_output(),
                delimiter = " ",
            ),
        ),
        category = "merge",
    )

    return [
        DefaultInfo(
            default_output = kernel_config,
        )
    ]

_attrs = {
    "arch": attrs.enum(ARCHITECTURES),
    # Use attrs.one_of() to enforce that flavor is one of the allowed values.
    "flavor": attrs.string(default="default"),
    "selftests": attrs.enum(SelftestsType.values()),
    "srcs": attrs.list(attrs.source()),
    "_prepareconfig": attrs.exec_dep(default = "//facebook/scripts:prepareconfig"),
    "_selftestsconfig": attrs.exec_dep(default = "//facebook/scripts:selftestsconfig"),
}

kernel_config_rule = rule(
    impl = _impl,
    attrs = _attrs,
)

def kernel_config(arch, flavor = None, srcs = []):
    """
    Generate targets that will create the .config file used to configure the
    kernel at build time. Two sets of config targts are created:
    - Targets for regular kernel builds. That is, targets for the specified
      name and flavor.
    - Targets that specifically enable kernel selftests. These targets provide
      exactly the same configurations as the regular targets, but also include
      whatever configurations are required in order to run selftests.
    """
    generated_config(arch = arch, flavor = flavor)

    for selftests in SelftestsType.values():
        rule_name = config_name(
            arch=arch,
            flavor=flavor,
            selftests=SelftestsType(selftests),
        )

        kernel_config_rule(
            name=rule_name,
            arch=arch,
            flavor=flavor,
            selftests=selftests,
            srcs=srcs,
        )
