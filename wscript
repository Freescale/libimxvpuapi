#!/usr/bin/env python


from waflib.Build import BuildContext, CleanContext, InstallContext, UninstallContext, Logs

top = '.'
out = 'build'


# the code inside fragment deliberately does an unsafe implicit cast float->char to trigger a
# compiler warning; sometimes, gcc does not tell about an unsupported parameter *unless* the
# code being compiled causes a warning
c_cflag_check_code = """
int main()
{
	float f = 4.0;
	char c = f;
	return c - 4;
}
"""
def check_compiler_flag(conf, flag, lang):
	return conf.check(fragment = c_cflag_check_code, mandatory = 0, execute = 0, define_ret = 0, msg = 'Checking for compiler switch %s' % flag, cxxflags = conf.env[lang + 'FLAGS'] + [flag], okmsg = 'yes', errmsg = 'no')  
def check_compiler_flags_2(conf, cflags, ldflags, msg):
	Logs.pprint('NORMAL', msg)
	return conf.check(fragment = c_cflag_check_code, mandatory = 0, execute = 0, define_ret = 0, msg = 'Checking if building with these flags works', cxxflags = cflags, ldflags = ldflags, okmsg = 'yes', errmsg = 'no')


def add_compiler_flags(conf, env, flags, lang, compiler, uselib = ''):
	for flag in reversed(flags):
		if type(flag) == type(()):
			flag_candidate = flag[0]
			flag_alternative = flag[1]
		else:
			flag_candidate = flag
			flag_alternative = None

		if uselib:
			flags_pattern = lang + 'FLAGS_' + uselib
		else:
			flags_pattern = lang + 'FLAGS'

		if check_compiler_flag(conf, flag_candidate, compiler):
			env.prepend_value(flags_pattern, [flag_candidate])
		elif flag_alternative:
			if check_compiler_flag(conf, flag_alternative, compiler):
				env.prepend_value(flags_pattern, [flag_alternative])


def options(opt):
	opt.add_option('--enable-debug', action = 'store_true', default = False, help = 'enable debug build [default: %default]')
	opt.add_option('--enable-static', action = 'store_true', default = False, help = 'build static library [default: build shared library]')
	opt.load('compiler_c')


def configure(conf):
	import os

	conf.load('compiler_c')

	# check and add compiler flags

	if conf.env['CFLAGS'] and conf.env['LINKFLAGS']:
		check_compiler_flags_2(conf, conf.env['CFLAGS'], conf.env['LINKFLAGS'], "Testing compiler flags %s and linker flags %s" % (' '.join(conf.env['CFLAGS']), ' '.join(conf.env['LINKFLAGS'])))
	elif conf.env['CFLAGS']:
		check_compiler_flags_2(conf, conf.env['CFLAGS'], '', "Testing compiler flags %s" % ' '.join(conf.env['CFLAGS']))
	elif conf.env['LINKFLAGS']:
		check_compiler_flags_2(conf, '', conf.env['LINKFLAGS'], "Testing linker flags %s" % ' '.join(conf.env['LINKFLAGS']))

	compiler_flags = ['-Wextra', '-Wall', '-std=c99', '-pedantic', '-fPIC', '-DPIC']
	if conf.options.enable_debug:
		compiler_flags += ['-O0', '-g3', '-ggdb']
	else:
		compiler_flags += ['-O2']

	add_compiler_flags(conf, conf.env, compiler_flags, 'C', 'C')

	conf.env['BUILD_STATIC'] = conf.options.enable_static


	# test for Freescale libraries

	conf.check_cfg(package = 'libfslvpuwrap', uselib_store = 'FSLVPUWRAPPER', args = '--cflags --libs', mandatory = 1)



def build(bld):
	version_node = bld.srcnode.find_node('VERSION')
	with open(version_node.abspath()) as x:
		version = x.readline().splitlines()[0]

	bld(
		features = ['c', 'cstlib' if bld.env['BUILD_STATIC'] else 'cshlib'],
		features = ['c', 'cshlib'],
		includes = ['.'],
		uselib = 'FSLVPUWRAPPER',
		source = bld.path.ant_glob('imxvpuapi/*.c'),
		name = 'imxvpuapi',
		target = 'imxvpuapi',
		vnum = version
	)
	bld(
		features = ['c', 'cprogram'],
		includes = ['.', 'example'],
		cflags = ['-std=gnu99'],
		uselib = 'FSLVPUWRAPPER',
		use = 'imxvpuapi',
		source = ['example/decode-example.c', 'example/h264_utils.c'],
		target = 'example/decode-example',
		install_path = None # makes sure the example is not installed
	)

