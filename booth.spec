%if 0%{?suse_version}
%global booth_docdir %{_defaultdocdir}/%{name}
%else
# newer fedora distros have _pkgdocdir, rely on that when
# available
%{!?_pkgdocdir: %global _pkgdocdir %%{_docdir}/%{name}-%{version}}
# Directory where we install documentation
%global booth_docdir %{_pkgdocdir}
%endif

%global test_path   	%{_datadir}/booth/tests

%if 0%{?suse_version}
%define _libexecdir %{_libdir}
%endif
%define with_extra_warnings   	0
%define with_debugging  	0
%define without_fatal_warnings 	1
%if 0%{?fedora} || 0%{?centos} || 0%{?rhel}
%define pkg_group System Environment/Daemons
%else
%define pkg_group Productivity/Clustering/HA
%endif

Name:           booth
Summary:        Ticket Manager for Multi-site Clusters
License:        GPL-2.0+
Group:          %{pkg_group}
Version:        0.2.0
Release:        0
Source:         booth.tar.bz2
Source1:        %name-rpmlintrc
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
BuildRequires:  asciidoc
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  glib2-devel
%if 0%{?fedora} || 0%{?centos} || 0%{?rhel}
BuildRequires:  cluster-glue-libs-devel
BuildRequires:  pacemaker-libs-devel
%else
BuildRequires:  libglue-devel
BuildRequires:  libpacemaker-devel
%endif
BuildRequires:  libxml2-devel
BuildRequires:  pkgconfig
%if 0%{?fedora} || 0%{?centos} || 0%{?rhel}
Requires:       pacemaker >= 1.1.8
Requires:       cluster-glue-libs >= 1.0.6
%else
Requires:       pacemaker-ticket-support >= 2.0
%endif

%description
Booth manages the ticket which authorizes one of the cluster sites located in
geographically dispersed distances to run certain resources. It is designed to
be an add-on of Pacemaker, which extends Pacemaker to support geographically
distributed clustering.

%prep
%setup -q -n %{name}

%build
./autogen.sh
%configure \
	--with-initddir=%{_initrddir} \
	--docdir=%{booth_docdir}

make

#except check
#%check
#make check

%install
make DESTDIR=$RPM_BUILD_ROOT install docdir=%{booth_docdir}

mkdir -p %{buildroot}/%{_mandir}/man8/
gzip < docs/boothd.8 > %{buildroot}/%{_mandir}/man8/booth.8.gz
ln %{buildroot}/%{_mandir}/man8/booth.8.gz %{buildroot}/%{_mandir}/man8/boothd.8.gz 

%if %{defined _unitdir}
# systemd
mkdir -p %{buildroot}/%{_unitdir}
cp -a conf/booth@.service %{buildroot}/%{_unitdir}/booth@.service
ln -s /usr/sbin/service %{buildroot}%{_sbindir}/rcbooth-arbitrator
%else
# sysV init
ln -s ../../%{_initddir}/booth-arbitrator %{buildroot}%{_sbindir}/rcbooth-arbitrator
%endif

#install test-parts

mkdir -p %{buildroot}/%{test_path}
cp -a unit-tests/ script/unit-test.py test conf %{buildroot}/%{test_path}/
chmod +x %{buildroot}/%{test_path}/test/booth_path
chmod +x %{buildroot}/%{test_path}/test/live_test.sh

mkdir -p %{buildroot}/%{test_path}/src/
ln -s %{_sbindir}/boothd %{buildroot}/%{test_path}/src/
rm -f %{buildroot}/%{test_path}/test/*.pyc

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%{_sbindir}/booth
%{_sbindir}/boothd
%{_mandir}/man8/booth.8.gz
%{_mandir}/man8/boothd.8.gz
%dir /usr/lib/ocf
%dir /usr/lib/ocf/resource.d
%dir /usr/lib/ocf/resource.d/pacemaker
%dir %{_sysconfdir}/booth
%{_sbindir}/rcbooth-arbitrator
/usr/lib/ocf/resource.d/pacemaker/booth-site
%config %{_sysconfdir}/booth/booth.conf.example

%if %{defined _unitdir}
%{_unitdir}/booth@.service
%exclude %{_initddir}/booth-arbitrator
%else
%{_initddir}/booth-arbitrator
%endif

%dir %{_datadir}/booth
%{_datadir}/booth/service-runnable

%doc README COPYING
%doc README.upgrade-from-v0.1

# this should be preun, but...
%pre
# stop the arbitrator if it's the previous paxos version 1.0
if [ "`booth version | awk '{print $2}'`" = "1.0" ]; then
	echo "booth v0.1 found"
	if grep -qs 'ticket.*;' /etc/booth/booth.conf; then
		echo "Convert the booth configuration in /etc/booth/booth.conf!"
	fi
	if ps -o pid,cmd -e | grep -qs "[b]oothd arbitrator"; then
		rcbooth-arbitrator stop
	fi
fi
exit 0


%package test
Summary:        Test scripts for Booth
Group:          %{pkg_group}
Requires:       booth
Requires:       python

%description test
This package contains automated tests for Booth,
the Cluster Ticket Manager for Pacemaker.

%files test
%defattr(-,root,root)

%doc README-testing
%{test_path}

%changelog
