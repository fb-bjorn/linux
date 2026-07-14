%define has_kver %{?rpm_kernel_version: 1} %{?!rpm_kernel_version: 0}
%if !%{has_kver}
%define rpm_kernel_version %(uname -r)
%endif

Name:	klp-%{rpm_kernel_version}
Version: %{hf_name}
Release: r1
Summary: No-op kernel live patch placeholder. This hotfix has no functional changes for %{_target_cpu}, so this rpm intentionally ships no livepatch module. The rpm and build system are not designed to be used directly; do not install manually.
Requires: kpatch

Group: System Environment/Kernel
License: GPL
Packager: kernel_infra
URL: http://www.kernel.org

%prep
mkdir -p $RPM_BUILD_ROOT/var/lib/kpatch/%{rpm_kernel_version}/
echo "Hotfix %{hf_name} has no functional changes for %{_target_cpu}; this placeholder rpm intentionally ships no livepatch (.ko) module." > $RPM_BUILD_ROOT/var/lib/kpatch/%{rpm_kernel_version}/NOOP-%{_target_cpu}-README

%files
/var/lib/kpatch/%{rpm_kernel_version}/NOOP-%{_target_cpu}-README

%post

%postun

%description
No-op placeholder for hotfix %{hf_name} on version %{rpm_kernel_version}: no functional changes for %{_target_cpu}, so no livepatch module is shipped. Installs a README marker instead of a .ko so the ohai livepatch plugin finds nothing to load.
