import platform
import subprocess


class Package(object):
    def __init__(self, name, version, files=[], packfiles=True):
        self.name = name
        self.version = version
        self.files = list(files)

    def add_file(self, filename):
        self.files.append(filename)


magic_dirs = ('/dev', '/proc', '/sys')
system_dirs = ('/bin', '/etc', '/lib', '/sbin', '/usr', '/var')


class DpkgManager(object):
    def __init__(self):
        self.unknown_files = []
        self.packages = {}
        self.package_files = {}

    def search_for_file(self, f):
        # Special files
        if any(f.path.startswith(c) for c in magic_dirs):
            return

        # If it's not in a system directory, no need to look for it
        if (f.path.startswith('/usr/local') or
                not any(f.path.startswith(c) for c in system_dirs)):
            self.unknown_files.append(f)
            return

        # Looks in our cache
        if f.path in self.package_files:
            pkgname = self.package_files[f.path]
        else:
            pkgname = self._get_package_for_file(f.path)
            self.package_files[f.path] = pkgname

        # Stores the file
        if pkgname is None:
            self.unknown_files.append(f)
        else:
            if pkgname in self.packages:
                self.packages[pkgname].add_file(f)
            else:
                self._create_package(pkgname, [f])

    def _get_package_for_file(self, filename):
        p = subprocess.Popen(['dpkg', '-S', filename], stdout=subprocess.PIPE)
        try:
            for l in p.stdout:
                pkgname, f = l.split(b': ', 1)
                pkgname, f = pkgname.decode('ascii'), f.strip().decode('ascii')
                self.package_files[f] = pkgname
                if f == filename:
                    return pkgname
        finally:
            p.wait()
        return None

    def _create_package(self, pkgname, files):
        p = subprocess.Popen(['dpkg', '-l', pkgname], stdout=subprocess.PIPE)
        try:
            version = None
            for l in p.stdout:
                if not l.startswith(b'ii'):
                    continue
                fields = l.split()
                if fields[1].decode('ascii') == pkgname:
                    version = str(fields[2])
                    break
        finally:
            p.wait()
        assert p.returncode == 0
        pkg = Package(pkgname, version, files)
        self.packages[pkgname] = pkg
        return pkg


def identify_packages(files):
    """Organizes the files, using the distribution's package manager.
    """
    distribution = platform.linux_distribution()[0].lower()
    if distribution == 'ubuntu':
        manager = DpkgManager()
    elif distribution == 'debian':
        manager = DpkgManager()
    else:
        return files, []

    for f in files:
        manager.search_for_file(f)

    return manager.unknown_files, manager.packages.values()
