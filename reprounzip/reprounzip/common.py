from __future__ import unicode_literals

from datetime import datetime
import os
import yaml

from . import __version__ as reprozip_version
from .utils import CommonEqualityMixin, escape, hsize


FILE_READ = 0x01
FILE_WRITE = 0x02
FILE_WDIR = 0x04


class File(CommonEqualityMixin):
    """A file, used at some point during the experiment.
    """
    def __init__(self, path):
        self.path = path
        try:
            stat = os.stat(path)
        except OSError:
            self.size = None
        else:
            self.size = stat.st_size

    def __eq__(self, other):
        return (isinstance(other, File) and
                self.path == other.path)

    def __hash__(self):
        return hash(self.path)


class Package(CommonEqualityMixin):
    def __init__(self, name, version, files=[], packfiles=True, size=None):
        self.name = name
        self.version = version
        self.files = list(files)
        self.packfiles = packfiles
        self.size = size

    def add_file(self, filename):
        self.files.append(filename)

    def __unicode__(self):
        return '%s (%s)' % (self.name, self.version)
    __str__ = __unicode__


class InvalidConfig(ValueError):
    """Configuration file is invalid.
    """


def read_files(files, File=File):
    return [File(f) for f in files]


def read_packages(packages, File=File, Package=Package):
    new_pkgs = []
    for pkg in packages:
        pkg['files'] = read_files(pkg['files'], File)
        new_pkgs.append(Package(**pkg))
    return new_pkgs


def load_config(filename, File=File, Package=Package):
    with open(filename) as fp:
        config = yaml.safe_load(fp)

    keys_ = set(config.keys())
    if 'version' not in keys_:
        raise InvalidConfig("Missing version")
    elif config['version'] != '0.0':
        raise InvalidConfig("Unknown version")
    elif not keys_.issubset(set(['version', 'runs',
                                 'packages', 'other_files'])):
        raise InvalidConfig("Unrecognized sections")

    runs = config.get('runs', [])
    packages = read_packages(config.get('packages', []), File, Package)
    other_files = read_files(config.get('other_files', []), File)

    return runs, packages, other_files


def write_file(fp, fi, indent=0):
    fp.write("%s  - \"%s\" # %s\n" % ("    " * indent,
                                      escape(fi.path),
                                      hsize(fi.size)))


def write_package(fp, pkg, indent=0):
    indent_str = "    " * indent
    fp.write("%s  - name: \"%s\"\n" % (indent_str, escape(pkg.name)))
    fp.write("%s    version: \"%s\"\n" % (indent_str, escape(pkg.version)))
    if pkg.size is not None:
        fp.write("%s    size: %d\n" % (indent_str, pkg.size))
    fp.write("%s    packfiles: %s\n" % (indent_str, 'true' if pkg.packfiles
                                                    else 'false'))
    fp.write("%s    files:\n"
             "%s      # Total files used: %s\n" % (
                 indent_str, indent_str,
                 hsize(sum(fi.size
                           for fi in pkg.files
                           if fi.size is not None))))
    if pkg.size is not None:
        fp.write("%s      # Installed package size: %s\n" % (
                 indent_str, hsize(pkg.size)))
    for fi in pkg.files:
        write_file(fp, fi, indent + 1)


def save_config(filename, runs, packages, other_files):
    dump = lambda x: yaml.safe_dump(x, encoding='utf-8', allow_unicode=True)
    with open(filename, 'w') as fp:
        # Writes preamble
        fp.write("""\
# ReproZip configuration file
# This file was generated by reprozip {version} at {date}

# You might want to edit this file before running the packer
# See 'reprozip pack -h' for help

# Run info
version: "{version!s}"
""".format(version=escape(reprozip_version),
           date=datetime.now().isoformat()))
        fp.write(dump({'runs': runs}).decode('utf-8'))
        fp.write("""\


# Files to pack
# All the files below were used by the program; they will be included in the
# generated package

# These files come from packages; we can thus choose not to include them, as it
# will simply be possible to install that package on the destination system
# They are included anyway by default
packages:
""")

        # Writes files
        for pkg in packages:
            write_package(fp, pkg)

        fp.write("""\

# These files do not appear to come with an installed package -- you probably
# want them packed
other_files:
""")
        for f in other_files:
            write_file(fp, f)
