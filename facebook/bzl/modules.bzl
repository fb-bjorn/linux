load(":external_module_build.bzl", "build_external_module")
load(":modules_list.bzl", MODULES="modules")

# This file is concatenated with the modules coming from configerator
# before being used

def standalone_module():
    kernel_version = native.read_config("kernel", "version")
    arch = native.read_config("kernel", "arch", "x86_64")
    name = native.read_config("kernel", "module")
    module_srpm = native.read_config("kernel", "module_srpm", None)
    kernel_devel = native.read_config("kernel", "devel_rpm", None)
    is_locally_defined_module = (native.read_config("kernel", "locally_defined_module", "false")
        .lower() in ["true", "1", "yes", "y", "on", "enable", "enabled"]) and name and module_srpm
    locally_defined_module = struct(
        name = name,
        url = module_srpm,
        sha256 = "",
        kernels = [],
        flavors = [],
        depends = [],
        archs = [],
    )
    if kernel_devel:
        native.genrule(
            name = "kernel-devel",
            cmd = "mkdir -p $OUT && cp {} $OUT/kernel-devel.rpm".format(kernel_devel),
            out = "kernel-devel",
            labels = ["linux_kernel"],
        )
    else:
        native.genrule(
            name = "kernel-devel",
            cmd = "mkdir -p $OUT && kerctl download --devel --arch {} --out-dir $OUT {} && mv $OUT/*.rpm $OUT/kernel-devel.rpm".format(arch, kernel_version),
            out = "kernel-devel",
            labels = ["linux_kernel"],
        )
    for mod in (MODULES if not is_locally_defined_module else [locally_defined_module]):
        if mod.name != name:
            continue
        deps = []
        d = 0
        for dep in mod.depends:
            d += 1
            native.genrule(
                name = "dependency-{}".format(dep),
                cmd = "mkdir -p $OUT ; kerctl download --arch {} --module {} --out-dir $OUT {}".format(arch, dep, kernel_version),
                out = "dependency-{}".format(dep),
                labels = ["linux_kernel"],
            )
            tpl = (
                "$(location :dependency-{})".format(dep),
                "/tmp/dependency-{}-{}".format(d, dep)
            )
            deps.append(tpl)
        module(
            name = "{}-{}".format(name, kernel_version),
            arch = arch,
            module = mod,
            kernel_devel = "$(location :kernel-devel)/kernel-devel.rpm",
            dependencies = deps,
        )
        native.genrule(
            name = "standalone_module",
            cmd = "mkdir -p $OUT && cp -a $(location :{}-{})/*.rpm $OUT".format(name, kernel_version),
            out = "rpmdir",
            labels = ["linux_kernel"],
        )

        break


def module(name, arch, module, kernel_devel, uname=None, dependencies=None, local_srpm=None):
    build_external_module(
        name = name,
        arch = arch,
        srpm = local_srpm or module.url,
        kernel_devel = kernel_devel,
        uname = uname,
        dependencies = dependencies,
    )
