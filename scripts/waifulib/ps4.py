# encoding: utf-8
# ps4.py -- PlayStation 4 (OpenOrbis toolchain) linking support
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# Every PS4 image (eboot.bin or .prx module) is a PIE ELF linked against
# OpenOrbis startup object, static musl libc and stubs of system libraries,
# later converted to signed ELF by create-fself (see scripts/ps4/package.sh).
#
# Startup object and runtime libraries must appear exactly once per image,
# but task generators often have both C and C++ link features, so we
# can't use LDFLAGS_<feature> variables and add them here instead.

import os
from waflib import TaskGen

PS4_SYSTEM_LIBS = ['-lSceNet', '-lkernel']

def configure(conf):
	toolchain = conf.env.PS4_TOOLCHAIN
	conf.env.PS4_CRT_PROGRAM = os.path.join(toolchain, 'lib', 'crt1.o')
	conf.env.PS4_CRT_LIBRARY = os.path.join(toolchain, 'lib', 'crtlib.o')

	# replace -shared, modules are PIE images too
	for i in ['cprogram', 'cxxprogram', 'cshlib', 'cxxshlib']:
		conf.env['LINKFLAGS_' + i] = ['-Wl,-pie']

@TaskGen.feature('cprogram', 'cxxprogram', 'cshlib', 'cxxshlib')
@TaskGen.after_method('propagate_uselib_vars')
def ps4_add_runtime(self):
	if self.env.DEST_OS != 'ps4' or getattr(self, 'ps4_runtime_added', False):
		return

	self.ps4_runtime_added = True

	is_library = 'cshlib' in self.features or 'cxxshlib' in self.features
	is_cxx = 'cxx' in self.features or 'cxxprogram' in self.features or 'cxxshlib' in self.features

	flags = [self.env.PS4_CRT_LIBRARY if is_library else self.env.PS4_CRT_PROGRAM]
	if is_cxx:
		flags += ['-lc++']
	flags += ['-lc'] + PS4_SYSTEM_LIBS

	self.env.append_value('LDFLAGS', flags)
