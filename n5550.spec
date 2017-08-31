
# If you dont want the freecusd package, add '--without freecusd' to the rpmbuild line
%bcond_without freecusd

# If you want the DKMS package, add '--with dkms' to the rpmbuild line
%bcond_with dkms

# If you want the kmod package, add '--with kmod' to the rpmbuild line
%bcond_with kmod

# If kversion isn't defined on the rpmbuild line, define it here.
#  Default to first el7 kernel with GPIO support
%if 0%{!?kversion:1}
%define kversion 3.10.0-514.el7.%{_target_cpu}
%endif

Name:		n5550
Summary:	Hardware support and monitoring for Thecus N5550 NAS
Version:	0.5
Release:	1%{?dist}
Source:		https://github.com/ipilcher/%{name}/archive/%{version}.tar.gz#/%{name}-%{version}.tar.gz
Source10:	kmodtool-n5550-el7.sh
License:	GPLv2
BuildRequires:	gcc make redhat-rpm-config
BuildRequires:	kernel-abi-whitelists kernel-devel
Requires:	n5550-source = %{name}-%{version}
Requires:	n5550-dracut = %{name}-%{version}

%if %{with freecusd}
%if 0%{?rhel} > 6
# for pre/post macros
BuildRequires: systemd
%endif
BuildRequires: libcip-devel
BuildRequires: hddtemp
%endif

%description
Hardware support and monitoring for Thecus N5550 NAS.  This package includes
kernel module source code, hooks to automatically build the kernel modules for
newly installed kernels.

%if %{with kmod}
# Magic hidden here.
%{expand:%(sh %{SOURCE10} rpmtemplate n5550 %{kversion} "")}
%endif

%if %{with freecusd}
%package -n n5550-freecusd
Summary: A system monitoring daemon for the N5550 LCD display
Group: System Environment/Daemons
Requires: /usr/sbin/mdadm /usr/sbin/hddtemp /usr/sbin/smartctl
Requires: n5550 >= %{version}

%description -n n5550-freecusd
Hardware support and monitoring for Thecus N5550 NAS including a system
monitoring daemon which uses the N5550's LCD display and LEDs to report
system status.
%endif

%package -n n5550-source
Summary: The module source for kmod-n5550
Group: System Environment/Kernel
Requires: kernel-devel >= %{kversion}
Requires: gcc make

%description -n n5550-source
If you want the exact source files, for building your own kernel modules
for the N5550, this package contains those files.

%if %{with dkms}
%package -n n5550-dkms
Summary: DKMS configs for building the n5550 modules
Group: System Environment/Kernel
Requires: dkms >= 2.2.0.0
Requires: n5550-source >= %{version}

%description -n n5550-dkms
This package enables dkms building for the N5550 kernel modules.
%endif

%package -n n5550-dracut
Summary: The relevant dracut support files for n5550
Group: System Environment/Kernel
Requires: dracut

%description -n n5550-dracut
In order to load the module early enough and in the right way, a set of
dracut configs are provided for the N5550 modules.


%prep
%setup -q

%if %{with dkms}
cat << DKMS >> dkms.conf
PACKAGE_NAME="n5550"
PACKAGE_VERSION="%{version}"
MAKE[0]="make"

DKMS

modcount=0
for module in n5550_ahci_leds n5550_board; do
    echo "BUILT_MODULE_NAME[${modcount}]='${module}'" >> dkms.conf
    echo "DEST_MODULE_LOCATION[${modcount}]='/extra/n5550'" >> dkms.conf
    modcount=$(expr ${modcount} + 1 )
done
%endif

%if %{with kmod}
echo "override n5550 * weak-updates/n5550" > kmod-n5550.conf
%endif

%build
%if %{with freecusd}
cd freecusd
gcc -Os -Wall -Wextra -pthread -o freecusd *.c -lcip
%endif

%if %{with kmod}
pushd modules
echo "Building n5550 kmod for %{kversion}"
KSRC=%{_usrsrc}/kernels/%{kversion}
%{__make} -C "${KSRC}" %{?_smp_mflags} modules M=$PWD
popd

%endif

%install
rm -rf %{buildroot}

# Monitoring daemon
%if %{with freecusd}
mkdir -p %{buildroot}/usr/bin
cp freecusd/freecusd %{buildroot}/usr/bin/
mkdir -p %{buildroot}/usr/lib/systemd/system
cp freecusd/freecusd.service %{buildroot}/usr/lib/systemd/system/
mkdir %{buildroot}/etc
cp freecusd/freecusd.conf %{buildroot}/etc/
%endif

%if %{with kmod}
%{__install} kmod-n5550.conf %{buildroot}%{_sysconfdir}/depmod.d/
%{__install} -d %{buildroot}/lib/modules/%{kversion}/extra/n5550/
for module in n5550_ahci_leds n5550_board; do
    %{__install} modules/${module}.ko %{buildroot}/lib/modules/%{kversion}/extra/n5550/
done
%endif

# Kernel module sources
mkdir -p %{buildroot}/usr/src/n5550/modules
cp modules/{Makefile,n5550_ahci_leds.c,n5550_board.c} %{buildroot}/usr/src/n5550/modules/

%if %{with dkms}
%{__install} -m 0644 dkms.conf %{buildroot}/usr/src/n5550
%endif

# Kernel module loading
mkdir -p %{buildroot}/usr/lib/modules-load.d
cp conf/modules-load_n5550.conf %{buildroot}/usr/lib/modules-load.d/n5550.conf
mkdir -p %{buildroot}/usr/lib/modprobe.d
cp conf/modprobe_n5550.conf %{buildroot}/usr/lib/modprobe.d/n5550.conf

# udev rules
mkdir -p %{buildroot}/usr/lib/udev/rules.d
cp conf/99-n5550.rules %{buildroot}//usr/lib/udev/rules.d/

# Dracut
mkdir -p %{buildroot}/usr/lib/dracut/modules.d/99n5550
cp dracut/{check,installkernel,install} %{buildroot}/usr/lib/dracut/modules.d/99n5550/
mkdir -p %{buildroot}/usr/lib/dracut/dracut.conf.d
cp conf/dracut_n5550.conf %{buildroot}/usr/lib/dracut/dracut.conf.d/n5550.conf

%clean
rm -rf %{buildroot}

%if %{with freecusd}
%post -n n5550-freecusd
%systemd_post freecusd.service
%preun -n n5550-freecusd
%systemd_preun freecusd.service
%postun -n n5550-freecusd
%systemd_postun_with_restart freecusd.service
%endif

%if %{with dkms}
%post -n n5550-dkms
for POSTINST in /usr/lib/dkms/common.postinst; do
    if [ -f $POSTINST ]; then
        $POSTINST n5550 %{version}
        exit $?
    fi
    echo "WARNING: $POSTINST does not exist."
done
echo -e "ERROR: DKMS version is too old and n5550 was not"
echo -e "built with legacy DKMS support."
echo -e "You must either rebuild n5550 with legacy postinst"
echo -e "support or upgrade DKMS to a more current version."
exit 1

%preun -n n5550-dkms
# Only remove the modules if they are for this %{version}-%{release}.  A
# package upgrade can replace them if only the %{release} is changed.
RELEASE="/var/lib/dkms/n5550/%{version}/build/n5550.release"
if [ -f $RELEASE ] && [ `cat $RELEASE`%{?dist} = "%{version}-%{release}" ]; then
    echo -e
    echo -e "Uninstall of n5550 module (version %{version}) beginning:"
    dkms remove -m n5550 -v %{version} --all --rpm_safe_upgrade
fi
exit 0
%endif

%files
%defattr(0644,root,root,-)
%attr(0644,root,root) %doc LICENSE README

%files -n n5550-dracut
%attr(0644,root,root) /usr/lib/modules-load.d/n5550.conf
%attr(0644,root,root) /usr/lib/modprobe.d/n5550.conf
%attr(0644,root,root) /usr/lib/udev/rules.d/99-n5550.rules
%attr(0755,root,root) %dir /usr/lib/dracut/modules.d/99n5550
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/check
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/installkernel
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/install
%attr(0644,root,root) /usr/lib/dracut/dracut.conf.d/n5550.conf

%if %{with freecusd}
%files -n n5550-freecusd
%defattr(0644,root,root,-)
%attr(0755,root,root) /usr/bin/freecusd
%attr(0644,root,root) /usr/lib/systemd/system/freecusd.service
%attr(0644,root,root) %config /etc/freecusd.conf
%endif

%files -n n5550-source
%defattr(0644,root,root,-)
%attr(0755,root,root) %dir /usr/src/n5550
%attr(0755,root,root) %dir /usr/src/n5550/modules
%attr(0644,root,root) /usr/src/n5550/modules/Makefile
%attr(0644,root,root) /usr/src/n5550/modules/n5550_ahci_leds.c
%attr(0644,root,root) /usr/src/n5550/modules/n5550_board.c

%if %{with dkms}
%files -n n5550-dkms
%defattr(0644,root,root,-)
%attr(0644,root,root) /usr/src/n5550/dkms.conf
%endif


%changelog
* Wed Dec 28 2016 Ian Pilcher <arequipeno@gmail.com> - 0.5-1
- Version 0.5
- freecusd: Add mutex to coordinate HDD temp & SMART threads

* Sat Dec 17 2016 Ian Pilcher <arequipeno@gmail.com> - 0.4-1
- Version 0.4
- freecusd: Handle new coretemp sysfs path
- dracut: Use udev to load n5550_board when gpio_ich is ready

* Wed Apr 01 2015 Ian Pilcher <arequipeno@gmail.com> - 0.3.1-1
- Version 0.3.1
- dracut: ensure libahci module is included in initramfs

* Mon Sep 22 2014 Ian Pilcher <arequipeno@gmail.com> - 0.3-1
- Version 0.3
- freecusd: auto-detect RAID disks
- freecusd: make gcc command line work with "as needed" linking

* Mon Sep 08 2014 Ian Pilcher <arequipeno@gmail.com> - 0.2.1-1
- Version 0.2.1
- Fix freecusd error reporting between fork() and execv()

* Tue Aug 12 2014 Ian Pilcher <arequipeno@gmail.com> - 0.2-1
- EL7-compatible package

* Mon Aug 11 2014 Ian Pilcher <arequipeno@gmail.com> - 0.1.9.1-1
- Replace SysV init script with systemd service file
- Move various integration files from /etc to /usr/lib

* Mon Aug 11 2014 Ian Pilcher <arequipeno@gmail.com> - 0.1.9-1
- Pre-release EL7-compatible package

* Sun Aug 03 2014 Ian Pilcher <arequipeno@gmail.com> - 0.1.1-4
- Build for EL7

* Tue Nov 05 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1.1-3
- Add file dependencies for external commands called by freecusd
- Add dist tag to release

* Tue Nov 05 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1.1-2
- Switch from kernel-ml to kernel-lt (now that kernel-lt is tracking 3.10.x)

* Mon Oct 28 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1.1-1
- Version 0.1.1 fixes S.M.A.R.T. monitor not sleeping

* Mon Oct 28 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1-2
- Correct location of kernel module sources

* Mon Oct 28 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1-1
- Initial SPEC file
