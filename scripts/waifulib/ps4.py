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

# file functions redirected to working directory emulation, see engine/platform/ps4/compat/ps4_cwd.c
PS4_WRAPPED_FUNCS = ['open', 'fopen', 'stat', 'lstat', 'fstat', 'opendir', 'mkdir', 'rename', 'remove',
	'unlink', 'rmdir', 'access', 'chdir', 'getcwd', 'realpath']

def configure(conf):
	toolchain = conf.env.PS4_TOOLCHAIN
	conf.env.PS4_CRT_PROGRAM = os.path.join(toolchain, 'lib', 'crt1.o')
	conf.env.PS4_CRT_LIBRARY = os.path.join(toolchain, 'lib', 'crtlib.o')

	# replace -shared, modules are PIE images too
	for i in ['cprogram', 'cxxprogram', 'cshlib', 'cxxshlib']:
		conf.env['LINKFLAGS_' + i] = ['-Wl,-pie']

	# compile working directory emulation once, it's linked into every image
	# engine and hlsdk-portable keep it in different places
	for i in ['engine/platform/ps4/compat/ps4_cwd.c', 'scripts/ps4/ps4_cwd.c']:
		src = conf.path.find_node(i)
		if src:
			break
	else:
		conf.fatal('ps4_cwd.c not found')

	obj = conf.bldnode.make_node('ps4_cwd.o')
	conf.start_msg('Compiling PS4 working directory emulation')
	cmd = conf.env.CC + conf.env.CFLAGS + ['-O2', '-c', src.abspath(), '-o', obj.abspath()]
	try:
		conf.cmd_and_log(cmd)
	except Exception as e:
		conf.end_msg('failed', color='RED')
		conf.fatal('Failed to compile %s: %s' % (src.abspath(), e))
	conf.end_msg('ok')

	conf.env.PS4_CWD_OBJ = obj.abspath()
	conf.env.PS4_WRAP_FLAGS = ['-Wl,--wrap=%s' % i for i in PS4_WRAPPED_FUNCS]

@TaskGen.feature('cprogram', 'cxxprogram', 'cshlib', 'cxxshlib')
@TaskGen.after_method('propagate_uselib_vars')
def ps4_add_runtime(self):
	if self.env.DEST_OS != 'ps4' or getattr(self, 'ps4_runtime_added', False):
		return

	self.ps4_runtime_added = True

	is_library = 'cshlib' in self.features or 'cxxshlib' in self.features
	is_cxx = 'cxx' in self.features or 'cxxprogram' in self.features or 'cxxshlib' in self.features

	flags = [self.env.PS4_CRT_LIBRARY if is_library else self.env.PS4_CRT_PROGRAM, self.env.PS4_CWD_OBJ]
	flags += self.env.PS4_WRAP_FLAGS
	if is_cxx:
		flags += ['-lc++']
	flags += ['-lc'] + PS4_SYSTEM_LIBS

	self.env.append_value('LDFLAGS', flags)
