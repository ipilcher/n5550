Name:		n5550
Summary:	Hardware support and monitoring for Thecus N5550 NAS
Version:	0.4
Release:	1%{?dist}
Source:		https://github.com/ipilcher/%{name}/archive/%{version}.tar.gz#/%{name}-%{version}.tar.gz
License:	GPLv2
Requires:	kernel-plus-devel gcc make
Requires:	/usr/sbin/mdadm /usr/sbin/hddtemp /usr/sbin/smartctl
BuildRequires:	gcc libcip-devel

%description
Hardware support and monitoring for Thecus N5550 NAS.  This package includes
kernel module source code, hooks to automatically build the kernel modules for
newly installed kernels, and a system monitoring daemon which uses the N5550's
LCD display and LEDs to report system status.

%prep
%setup -q

%build
cd freecusd
gcc -Os -Wall -Wextra -pthread -o freecusd *.c -lcip

%install
rm -rf %{buildroot}
# Monitoring daemon
mkdir -p %{buildroot}/usr/bin
cp freecusd/freecusd %{buildroot}/usr/bin/
mkdir -p %{buildroot}/usr/lib/systemd/system
cp freecusd/freecusd.service %{buildroot}/usr/lib/systemd/system/
mkdir %{buildroot}/etc
cp freecusd/freecusd.conf %{buildroot}/etc/
# Kernel module sources
mkdir -p %{buildroot}/usr/src/n5550/modules
cp modules/{Makefile,n5550_ahci_leds.c,n5550_board.c} %{buildroot}/usr/src/n5550/modules/
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

%files
%attr(0755,root,root) /usr/bin/freecusd
%attr(0644,root,root) /usr/lib/systemd/system/freecusd.service
%attr(0644,root,root) %config /etc/freecusd.conf
%attr(0755,root,root) %dir /usr/src/n5550
%attr(0755,root,root) %dir /usr/src/n5550/modules
%attr(0644,root,root) /usr/src/n5550/modules/Makefile
%attr(0644,root,root) /usr/src/n5550/modules/n5550_ahci_leds.c
%attr(0644,root,root) /usr/src/n5550/modules/n5550_board.c
%attr(0644,root,root) /usr/lib/modules-load.d/n5550.conf
%attr(0644,root,root) /usr/lib/modprobe.d/n5550.conf
%attr(0644,root,root) /usr/lib/udev/rules.d/99-n5550.rules
%attr(0755,root,root) %dir /usr/lib/dracut/modules.d/99n5550
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/check
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/installkernel
%attr(0755,root,root) /usr/lib/dracut/modules.d/99n5550/install
%attr(0644,root,root) /usr/lib/dracut/dracut.conf.d/n5550.conf
%attr(0644,root,root) %doc LICENSE README

%changelog
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
