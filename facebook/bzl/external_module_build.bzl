load(":container.bzl", "container_genrule")


def _is_remote_srpm_url(url):
    if not url:
        return False
    return url.startswith("http") or url.startswith("manifold://")


def _srpm_fetch_cmd(url):
    # manifold://<bucket>/<path> is fetched with the manifold CLI; everything else
    # (http/https) with curl. $OUT is the genrule output path.
    if url.startswith("manifold://"):
        return "manifold get {} $OUT".format(url[len("manifold://"):])
    return "curl {} -o $OUT".format(url)


def build_external_module(name, arch, srpm, kernel_devel, uname=None, dependencies=None):
    if _is_remote_srpm_url(srpm):
        native.genrule(
            name="{}.src.rpm".format(name),
            cmd=_srpm_fetch_cmd(srpm),
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
