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
	opt.add_option('--use-fslwrapper-backend', action = 'store_true', default = False, help = 'use the Freescale VPU wrapper (= libfslvpuwrap) backend instead of the vpulib (= imx-vpu) one [default: %default]')
	opt.load('compiler_c')
	opt.load('gnu_dirs')


def configure(conf):
	import os

	conf.load('compiler_c')
	conf.load('gnu_dirs')

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

	if not conf.options.use_fslwrapper_backend:
		Logs.pprint('GREEN', 'using the vpulib backend')
		conf.check_cc(lib = 'vpu', uselib_store = 'VPULIB', mandatory = 1)
		conf.env['VPUAPI_USELIBS'] = ['VPULIB']
		conf.env['VPUAPI_BACKEND_SOURCE'] = ['imxvpuapi/imxvpuapi_vpulib.c']

		with_sof_stuff = conf.check_cc(fragment = '''
			#include <vpu_lib.h>
			int main() {
				return ENC_ENABLE_SOF_STUFF * 0;
			}
			''',
			uselib = 'VPULIB',
			mandatory = False,
			execute = False,
			msg = 'checking if ENC_ENABLE_SOF_STUFF exists'
		)
		if with_sof_stuff:
			conf.define('HAVE_ENC_ENABLE_SOF_STUFF', 1)

	else:
		Logs.pprint('GREEN', 'using the fslwrapper backend')
		conf.check_cfg(package = 'libfslvpuwrap >= 1.0.45', uselib_store = 'FSLVPUWRAPPER', args = '--cflags --libs', mandatory = 1)
		conf.env['VPUAPI_USELIBS'] = ['FSLVPUWRAPPER']
		conf.env['VPUAPI_BACKEND_SOURCE'] = ['imxvpuapi/imxvpuapi_fslwrapper.c']


	# Process the library version number

	version_node = conf.srcnode.find_node('VERSION')
	with open(version_node.abspath()) as x:
		version = x.readline().splitlines()[0]

	conf.env['IMXVPUAPI_VERSION'] = version
	conf.define('IMXVPUAPI_VERSION', version)


	# Workaround to ensure previously generated .pc files aren't stale

	pcnode = conf.path.get_bld().find_node('libimxvpuapi.pc')
	if pcnode:
		pcnode.delete()


	# Write the config header

	conf.write_config_header('config.h')


def build(bld):
	bld(
		features = ['c', 'cstlib' if bld.env['BUILD_STATIC'] else 'cshlib'],
		includes = ['.'],
		uselib = bld.env['VPUAPI_USELIBS'],
		source = ['imxvpuapi/imxvpuapi.c', 'imxvpuapi/imxvpuapi_jpeg.c', 'imxvpuapi/imxvpuapi_parse_jpeg.c'] + bld.env['VPUAPI_BACKEND_SOURCE'],
		name = 'imxvpuapi',
		target = 'imxvpuapi',
		vnum = bld.env['IMXVPUAPI_VERSION']
	)

	bld.install_files('${PREFIX}/include/imxvpuapi/', ['imxvpuapi/imxvpuapi.h', 'imxvpuapi/imxvpuapi_jpeg.h'])

	examples = [ \
		{ 'name': 'decode-example', 'source': ['example/decode-example.c'] }, \
		{ 'name': 'encode-example', 'source': ['example/encode-example.c'] }, \
		{ 'name': 'encode-example-writecb', 'source': ['example/encode-example-writecb.c'] }, \
		{ 'name': 'jpeg-dec-example', 'source': ['example/jpeg-dec-example.c'] }, \
		{ 'name': 'jpeg-enc-example', 'source': ['example/jpeg-enc-example.c'] }, \
	]

	bld(
		features = ['c'],
		includes = ['.', 'example'],
		cflags = ['-std=gnu99'],
		use = 'imxvpuapi',
		source = ['example/main.c', 'example/h264_utils.c'],
		name = 'examples-common'
	)

	bld(
		features = ['subst'],
		source = "libimxvpuapi.pc.in",
		target="libimxvpuapi.pc",
		install_path="${LIBDIR}/pkgconfig"
	)

	for example in examples:
		bld(
			features = ['c', 'cprogram'],
			includes = ['.', 'example'],
			cflags = ['-std=gnu99'],
			uselib = bld.env['VPUAPI_USELIBS'],
			use = 'imxvpuapi examples-common',
			source = example['source'],
			target = 'example/' + example['name'],
			install_path = None # makes sure the example is not installed
		)
