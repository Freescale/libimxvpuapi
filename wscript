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
	opt.add_option('--use-vpulib-backend', action = 'store_true', default = False, help = 'use the vpulib backend instead of the vpu wrapper one [EXPERIMENTAL] [default: %default]')
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
	conf.env['USE_VPULIB_BACKEND'] = conf.options.use_vpulib_backend


	# test for Freescale libraries

	if conf.options.use_vpulib_backend:
		conf.check_cc(lib = 'vpu', uselib_store = 'VPULIB', mandatory = 1)
		conf.env['VPUAPI_USELIBS'] = ['VPULIB']
		conf.env['VPUAPI_BACKEND_SOURCE'] = ['imxvpuapi/imxvpuapi_vpulib.c']
	else:
		conf.check_cfg(package = 'libfslvpuwrap', uselib_store = 'FSLVPUWRAPPER', args = '--cflags --libs', mandatory = 1)
		conf.env['VPUAPI_USELIBS'] = ['FSLVPUWRAPPER']
		conf.env['VPUAPI_BACKEND_SOURCE'] = ['imxvpuapi/imxvpuapi_fslwrapper.c']


def build(bld):
	version_node = bld.srcnode.find_node('VERSION')
	with open(version_node.abspath()) as x:
		version = x.readline().splitlines()[0]

	bld(
		features = ['c', 'cstlib' if bld.env['BUILD_STATIC'] else 'cshlib'],
		includes = ['.'],
		uselib = bld.env['VPUAPI_USELIBS'],
		source = ['imxvpuapi/imxvpuapi.c', 'imxvpuapi/imxvpuapi_jpeg.c'] + bld.env['VPUAPI_BACKEND_SOURCE'],
		name = 'imxvpuapi',
		target = 'imxvpuapi',
		vnum = version
	)

	examples = [ \
		{ 'name': 'decode-example', 'source': ['example/decode-example.c'] }, \
		{ 'name': 'encode-example', 'source': ['example/encode-example.c'] }, \
		{ 'name': 'jpeg-dec-example', 'source': ['example/jpeg-dec-example.c'] }, \
	]

	bld(
		features = ['c'],
		includes = ['.', 'example'],
		cflags = ['-std=gnu99'],
		use = 'imxvpuapi',
		source = ['example/main.c', 'example/h264_utils.c'],
		name = 'examples-common'
	)

	for example in examples:
		bld(
			features = ['c', 'cprogram'],
			includes = ['.', 'example'],
			cflags = ['-std=gnu99'],
			use = 'imxvpuapi examples-common',
			source = example['source'],
			target = 'example/' + example['name'],
			install_path = None # makes sure the example is not installed
		)
