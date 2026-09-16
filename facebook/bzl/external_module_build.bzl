load(":container.bzl", "container_genrule")


def _is_remote_srpm_url(url):
    if not url:
        return False
    return url.startswith("http") or url.startswith("manifold://")


def _remote_fetch_cmd(url, output="$OUT"):
    # manifold://<bucket>/<path> is fetched with the manifold CLI; everything else
    # (http/https) with curl.
    if url.startswith("manifold://"):
        return "manifold get {} {}".format(url[len("manifold://"):], output)
    return "curl {} -o {}".format(url, output)


def build_external_module(name, arch, srpm, kernel_devel, uname=None, dependencies=None):
    if _is_remote_srpm_url(srpm):
        native.genrule(
            name="{}.src.rpm".format(name),
            cmd=_remote_fetch_cmd(srpm),
            out="{}.src.rpm".format(name),
            labels=[
                "linux_kernel",
                "module",
            ],
        )
    else:
        native.genrule(
            name="{}.src.rpm".format(name),
            cmd="cp {} $OUT".format(srpm),
            out="{}.src.rpm".format(name),
            labels = ["linux_kernel"],
        )

    if uname:
        native.genrule(
            name = "uname-{}".format(name),
            cmd = "echo {} > $OUT".format(uname),
            out = "uname",
            labels = ["linux_kernel"],
        )
    else:
        # determine uname from rpm -ql of kernel-devel
        native.genrule(
            name = "uname-{}".format(name),
            cmd = "rpm -qp --queryformat '%{version}-%{release}' " + "{} > $OUT".format(kernel_devel),
            out = "uname",
            labels = ["linux_kernel"],
        )

    native.genrule(
        name = name + "-shared-build-dir",
        cmd = """
            OUT=`realpath $OUT`
            sudo rm -rf $OUT
            mkdir -p $OUT
        """,
        out = ".",
        labels = ["linux_kernel", "uses_sudo"],
    )

    bind_ro = [
        (":{}.src.rpm".format(name), "/tmp/module.src.rpm"),
        (kernel_devel, "/tmp/kernel-devel.rpm"),
        ("$(location :uname-{})".format(name), "/tmp/uname")
    ]

    if dependencies:
        for dep in dependencies:
            bind_ro.append(dep)

    # Build an rpm macro config that uses a do-signature semaphore file inside
    # the container to trigger autograph outside the container.
    # Yay escaping backslashes all the way down.
    native.genrule(
        name = name + "-rpmmacros",
        cmd = """
                cat > "$OUT" <<EOF
%__spec_install_post\\\\
%{?__debug_package:%{__debug_install_post}}\\\\
%{__arch_install_post}\\\\
%{__os_install_post}\\\\
touch /rw/BUILDROOT/do-signature\\\\
while [ -e /rw/BUILDROOT/do-signature ]; do sleep 10; done\\\\
%{nil}
EOF
""",
        out = "rpmmacros",
        labels = ["linux_kernel"],
    )

    sign = native.read_config('kernel', 'sign_mod', 'false')
    sign_key = native.read_config('kernel', 'sign_mod_key', 'hsm-test-key')
    if sign != "false":
        bind_ro.append((":{}-rpmmacros".format(name), "/tmp/rpmmacros"))

    build_prep = """
        rm -rf /root/rpmbuild/BUILDROOT
        mkdir -p /root/rpmbuild/
        ln -s /rw/BUILDROOT /root/rpmbuild/BUILDROOT
        [ -f "/tmp/rpmmacros" ] && cp /tmp/rpmmacros /root/.rpmmacros
        mkdir -p /fbgcc/bin
        [ -f "/fbgcc/bin/gcc" ] || ln -s /usr/bin/gcc /fbgcc/bin/gcc
        [ -f "/fbgcc/bin/cc" ] || ln -s /usr/bin/gcc /fbgcc/bin/cc
        if ls /tmp/dependency*/*.rpm; then
            rpm -ivh --ignorearch /tmp/dependency*/*.rpm
        fi
        rpm -Uvh --ignorearch /tmp/kernel-devel.rpm
    """

    #
    # `--define "using_clang 1"' is the old way to say "use Clang", and is
    # respected by all existing modules. This will continue to work on x86_64.
    #
    # `--with llvm_cross' is the new way to build with Clang, and requires
    # .spec files to be updated. This is required to build for aarch64 and
    # desired to build correctly (i.e. the same way as the kernel) for x86_64.
    #
    # Once all modules are ported to respect `--with llvm_cross' we
    # can delete `--define "using_clang 1"'.
    #
    container_genrule(
        name = name + "-rpmbuild",
        arch = arch,
        pre_cmd = """
            $(location //facebook:sign-hook-wrapper-sh) \
            $(location :{name}-shared-build-dir) \
            {sign_key} \
            $(location //facebook:sign-sandcastle-spec)
        """.format(name = name, sign_key = sign_key) if sign != "false" else "",
        cmd = """
            {build_prep}
            rpmbuild -rb /tmp/module.src.rpm \
            --target {arch} \
            --define "rpm_kernel_version {uname}" \
            --define "kernel_version {uname}" \
            --define "using_clang 1" \
            --with llvm_cross || touch /rw/BUILDROOT/abort-signature
            if [ -f /rw/BUILDROOT/failed-signature ]; then
                echo "Failed to sign module"
                exit 1
            fi
            cp -R /root/rpmbuild/RPMS/{arch}/*.rpm /rw/rpms
        """.format(uname = "\$(cat /tmp/uname)", build_prep = build_prep, arch = arch),
        bind_ro = bind_ro,
        bind_rw = [
            ("$OUT", "/rw/rpms"),
            (":{}-shared-build-dir".format(name), "/rw/BUILDROOT/"),
        ],
        tmpfs = ["/fbgcc"],
    )

    native.genrule(
        name = name,
        out = ".",
        cmd = """
            sudo rm -rf $(location :{name}-shared-build-dir)
            mkdir -p $OUT && cp $(location :{name}-rpmbuild)/*.rpm $OUT
        """.format(name = name),
        type = "module",
        labels = ["linux_kernel", "uses_sudo"],
    )


def standalone_external_module():
    """Defines a registry-independent external module build request.

    Required Buck configs are kernel.module, kernel.module_srpm, and
    kernel.version. kernel.arch defaults to x86_64. kernel.devel_rpm and
    kernel.uname may be supplied by an orchestrator; otherwise the kernel-devel
    RPM is downloaded with kerctl and the uname is read from that RPM. Optional
    dependency RPM paths or URLs can be passed as a comma-separated list in
    kernel.module_dependency_rpms.

    The stable caller-facing outputs are external_module,
    external_module_uname, and external_module_kernelversion.
    """
    module_name = native.read_config("kernel", "module", "")
    module_srpm = native.read_config("kernel", "module_srpm", "")
    kernel_version = native.read_config("kernel", "version", "")
    kernel_devel = native.read_config("kernel", "devel_rpm", "")
    kernel_uname = native.read_config("kernel", "uname", "")
    dependency_rpms = native.read_config("kernel", "module_dependency_rpms", "")
    arch = native.read_config("kernel", "arch", "x86_64")

    missing_configs = []
    if not module_name:
        missing_configs.append("kernel.module")
    if not module_srpm:
        missing_configs.append("kernel.module_srpm")
    if not kernel_version:
        missing_configs.append("kernel.version")

    if missing_configs:
        failure_cmd = "echo 'Missing required Buck config(s): {}' >&2; exit 1".format(", ".join(missing_configs))
        for target_name, output in [
            ("external_module", "rpmdir"),
            ("external_module_uname", "uname"),
            ("external_module_kernelversion", "kernelversion"),
        ]:
            native.genrule(
                name = target_name,
                cmd = failure_cmd,
                out = output,
                labels = ["linux_kernel", "module"],
                visibility = ["PUBLIC"],
            )
        return

    kernel_devel_target = "external_module_kernel_devel"

    if kernel_devel:
        native.genrule(
            name = kernel_devel_target,
            cmd = "mkdir -p $OUT && cp {} $OUT/kernel-devel.rpm".format(kernel_devel),
            out = "kernel-devel",
            labels = ["linux_kernel"],
        )
    else:
        native.genrule(
            name = kernel_devel_target,
            cmd = "mkdir -p $OUT && kerctl download --devel --arch {} --out-dir $OUT {} && mv $OUT/*.rpm $OUT/kernel-devel.rpm".format(arch, kernel_version),
            out = "kernel-devel",
            labels = ["linux_kernel"],
        )

    dependencies = []
    for index, dependency_rpm in enumerate(dependency_rpms.split(",")):
        dependency_rpm = dependency_rpm.strip()
        if not dependency_rpm:
            continue

        dependency_name = "external_module_dependency_{}".format(index)
        if _is_remote_srpm_url(dependency_rpm):
            dependency_cmd = "mkdir -p $OUT && {}".format(_remote_fetch_cmd(dependency_rpm, "$OUT/dependency.rpm"))
        else:
            dependency_cmd = "mkdir -p $OUT && cp {} $OUT/dependency.rpm".format(dependency_rpm)

        native.genrule(
            name = dependency_name,
            cmd = dependency_cmd,
            out = dependency_name,
            labels = ["linux_kernel", "module"],
        )
        dependencies.append((":{}".format(dependency_name), "/tmp/dependency-{}".format(index)))

    build_name = "external_module_{}_{}".format(module_name, kernel_version)

    build_external_module(
        name = build_name,
        arch = arch,
        srpm = module_srpm,
        kernel_devel = "$(location :{})/kernel-devel.rpm".format(kernel_devel_target),
        uname = kernel_uname or None,
        dependencies = dependencies,
    )

    native.genrule(
        name = "external_module",
        cmd = "mkdir -p $OUT && cp -a $(location :{})/*.rpm $OUT".format(build_name),
        out = "rpmdir",
        labels = ["linux_kernel", "module"],
        visibility = ["PUBLIC"],
    )

    native.genrule(
        name = "external_module_uname",
        cmd = "cp $(location :uname-{}) $OUT".format(build_name),
        out = "uname",
        labels = ["linux_kernel", "module"],
        visibility = ["PUBLIC"],
    )

    native.genrule(
        name = "external_module_kernelversion",
        cmd = "rpm -qp --queryformat '%{{version}}' $(location :{})/kernel-devel.rpm > $OUT".format(kernel_devel_target),
        out = "kernelversion",
        labels = ["linux_kernel", "module"],
        visibility = ["PUBLIC"],
    )
