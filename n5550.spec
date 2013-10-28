Name:		n5550
Summary:	Hardware support and monitoring for Thecus N5550 NAS
Version:	0.1
Release:	2
Source:		https://github.com/ipilcher/%{name}/archive/%{version}.tar.gz#/%{name}-%{version}.tar.gz
License:	GPLv2
Requires:	kernel-ml-devel gcc make
BuildRequires:	gcc

%description
Hardware support and monitoring for Thecus N5550 NAS.  This package includes
kernel module source code, hooks to automatically build the kernel modules for
newly installed kernels, and a system monitoring daemon which uses the N5550's
LCD display and LEDs to report system status.

%prep
%setup -q
sed -i 's|exec=/usr/local/bin/freecusd|exec=/usr/bin/freecusd|' freecusd/freecusd.init

%build
cd freecusd
gcc -Os -Wall -Wextra -pthread -lrt -o freecusd *.c

%install
rm -rf %{buildroot}
# Monitoring daemon binary
mkdir -p %{buildroot}/usr/bin
cp freecusd/freecusd %{buildroot}/usr/bin/
# Monitoring daemon init script
mkdir -p %{buildroot}/etc/rc.d/init.d
cp freecusd/freecusd.init %{buildroot}/etc/rc.d/init.d/freecusd
# Kernel module sources
mkdir -p %{buildroot}/usr/src/n5550/modules
cp modules/{Makefile,n5550_ahci_leds.c,n5550_board.c} %{buildroot}/usr/src/n5550/modules/
# Module config
mkdir -p %{buildroot}/etc/sysconfig/modules
cp conf/n5550.modules %{buildroot}/etc/sysconfig/modules/
mkdir -p %{buildroot}/etc/modprobe.d
cp conf/modprobe_n5550.conf %{buildroot}/etc/modprobe.d/n5550.conf
# Dracut module
mkdir -p %{buildroot}/usr/share/dracut/modules.d/99n5550
cp dracut/{check,installkernel} %{buildroot}/usr/share/dracut/modules.d/99n5550/

%clean
rm -rf %{buildroot}

%files
%attr(0755,root,root) /usr/bin/freecusd
%attr(0755,root,root) /etc/rc.d/init.d/freecusd
%attr(0755,root,root) %dir /usr/src/n5550
%attr(0755,root,root) %dir /usr/src/n5550/modules
%attr(0644,root,root) /usr/src/n5550/modules/Makefile
%attr(0644,root,root) /usr/src/n5550/modules/n5550_ahci_leds.c
%attr(0644,root,root) /usr/src/n5550/modules/n5550_board.c
%attr(0755,root,root) /etc/sysconfig/modules/n5550.modules
%attr(0644,root,root) /etc/modprobe.d/n5550.conf
%attr(0755,root,root) %dir /usr/share/dracut/modules.d/99n5550
%attr(0755,root,root) /usr/share/dracut/modules.d/99n5550/check
%attr(0755,root,root) /usr/share/dracut/modules.d/99n5550/installkernel
%attr(0644,root,root) %doc LICENSE README

%changelog
* Mon Oct 28 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1-2
- Correct location of kernel module sources

* Mon Oct 28 2013 Ian Pilcher <arequipeno@gmail.com> - 0.1-1
- Initial SPEC file
