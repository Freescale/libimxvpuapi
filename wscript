#!/usr/bin/env python3


from waflib.Build import BuildContext, CleanContext, InstallContext, UninstallContext, Logs
import os

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
def check_compiler_flag(conf, flag, mandatory = 0):
	return conf.check(fragment = c_cflag_check_code, mandatory = mandatory, execute = 0, define_ret = 0, msg = 'Checking for compiler switch %s' % flag, cflags = [flag], okmsg = 'yes', errmsg = 'no')

def check_combined_build_flags(conf, cflags, ldflags, mandatory = 0):
	if ldflags:
		Logs.pprint('NORMAL', 'Now checking the combination of CFLAGS %s and LDFLAGS %s' % (' '.join(cflags), ' '.join(ldflags)))
	else:
		Logs.pprint('NORMAL', 'Now checking the combination of CFLAGS %s' % ' '.join(cflags))
	return conf.check(fragment = c_cflag_check_code, mandatory = mandatory, execute = 0, define_ret = 0, msg = 'Checking if this combination works', cflags = cflags, ldflags = ldflags, okmsg = 'yes', errmsg = 'no')	


class PlatformIMX6:
	description = 'i.MX6 with Chips&Media CODA960 codec as VPU'

	def configure(self, conf):
		conf.check_cc(lib = 'vpu', uselib_store = 'CODA960', define_name = '', mandatory = 1)
		with_sof_stuff = conf.check_cc(fragment = '''
			#include <vpu_lib.h>
			int main() {
				return ENC_ENABLE_SOF_STUFF * 0;
			}
			''',
			uselib = 'CODA960',
			mandatory = False,
			execute = False,
			define_name = '',
			msg = 'checking if ENC_ENABLE_SOF_STUFF exists'
		)
		if with_sof_stuff:
			conf.define('HAVE_IMXVPUENC_ENABLE_SOF_STUFF', 1)

		imx_linux_headers_path = conf.options.imx_headers
		if not imx_linux_headers_path:
			imx_linux_headers_path = os.path.join(conf.options.sysroot_path, 'usr/include/imx')
		Logs.pprint('NORMAL', 'i.MX linux headers path: %s' % imx_linux_headers_path)
		conf.env['INCLUDES_IMXHEADERS'] = [imx_linux_headers_path]

		if not conf.check_cc(fragment = '''
			#include <time.h>
			#include <sys/types.h>
			#include <linux/ipu.h>

			int main() { return 0; }
			''',
			uselib_store = 'CODA960',
			uselib = ['IMXHEADERS', 'GNU99'],
			mandatory = False,
			execute = False,
			define_name = '',
			msg = 'checking for the IPU header linux/ipu.h'
		):
			conf.fatal('Cannot build CODA960 backend: IPU header linux/ipu.h not found (needed for combined frame copying / de-tiling)')

	def build(self, bld):
		bld(
			features = ['c'],
			includes = ['.'],
			cflags = ['-Wno-pedantic'],
			uselib = ['IMXDMABUFFER', 'GNU99', 'IMXHEADERS'],
			source = ['imxvpuapi2/imxvpuapi2_imx6_coda_ipu.c'],
			name = 'imx6_coda_ipu'
		)
		bld(
			features = ['c'],
			includes = ['.'],
			uselib = ['IMXDMABUFFER', 'C99', 'CODA960'],
			source = ['imxvpuapi2/imxvpuapi2_imx6_coda.c'],
			name = 'imx6_coda'
		)

		return {
			'uselib': ['CODA960'],
			'use': ['imx6_coda_ipu', 'imx6_coda']
		}


class PlatformIMX8M:
	description = 'i.MX8 M with Hantro G1/G2 decoder (optionally also a Hantro H1 encoder)'

	def __init__(self, soc_type, has_encoder):
		self.soc_type = soc_type
		self.has_encoder = has_encoder

	def configure(self, conf):
		sysroot_path = conf.env['SYSROOT']
		conf.env['CFLAGS_HANTRO'] += ['-pthread']
		conf.env['LINKFLAGS_HANTRO'] += ['-pthread']
		conf.check_cc(uselib_store = 'HANTRO', uselib = 'HANTRO', define_name = '', mandatory = 1, lib = 'hantro')
		conf.check_cc(uselib_store = 'HANTRO', uselib = 'HANTRO', define_name = '', mandatory = 1, lib = 'codec')
		conf.env['DEFINES_HANTRO'] += ['SET_OUTPUT_CROP_RECT', 'USE_EXTERNAL_BUFFER', 'VSI_API', 'ENABLE_CODEC_VP8']

		conf.check_cc(uselib_store = 'HANTRO_DEC', uselib = 'HANTRO', define_name = '', mandatory = 1, includes = [os.path.join(sysroot_path, 'usr/include/hantro_dec')], header_name = 'dwl.h')
		conf.check_cc(uselib_store = 'HANTRO_DEC', uselib = 'HANTRO', define_name = '', mandatory = 1, includes = [os.path.join(sysroot_path, 'usr/include/hantro_dec')], header_name = 'codec.h')

		if self.has_encoder:
			conf.define('IMXVPUAPI2_VPU_HAS_ENCODER', 1)
			conf.check_cc(uselib_store = 'HANTRO', uselib = 'HANTRO', define_name = '', mandatory = 1, lib = 'hantro_h1')
			conf.check_cc(uselib_store = 'HANTRO', uselib = 'HANTRO', define_name = '', mandatory = 1, lib = 'codec_enc')
			conf.env['DEFINES_HANTRO_ENC'] += ['ENCH1', 'OMX_ENCODER_VIDEO_DOMAIN', 'ENABLE_HANTRO_ENC']
			conf.check_cc(uselib_store = 'HANTRO_ENC', uselib = 'HANTRO', define_name = '', mandatory = 1, includes = [os.path.join(sysroot_path, 'usr/include/hantro_enc'), os.path.join(sysroot_path, 'usr/include/hantro_enc/headers')], header_name = 'encoder/codec.h')

		with_hantro_codec_error_frame_retval = conf.check_cc(fragment = '''
			#include "dwl.h"
			#include "codec.h"
			int main() {
				return CODEC_ERROR_FRAME * 0;
			}
			''',
			uselib = ['C99', 'HANTRO', 'HANTRO_DEC'],
			mandatory = False,
			execute = False,
			define_name = '',
			msg = 'checking if CODEC_ERROR_FRAME exists'
		)
		if with_hantro_codec_error_frame_retval:
			conf.define('HAVE_IMXVPUDEC_HANTRO_CODEC_ERROR_FRAME', 1)

		conf.define('IMXVPUAPI_IMX8_SOC_TYPE_' + self.soc_type, 1)

	def build(self, bld):
		bld(
			features = ['c'],
			includes = ['.'],
			uselib = ['IMXDMABUFFER', 'C99', 'HANTRO', 'HANTRO_DEC'],
			source = ['imxvpuapi2/imxvpuapi2_imx8m_hantro_decoder.c'],
			name = 'imx8_decoder'
		)
		bld(
			features = ['c'],
			includes = ['.'],
			uselib = ['IMXDMABUFFER', 'C99', 'HANTRO', 'HANTRO_ENC'],
			source = ['imxvpuapi2/imxvpuapi2_imx8m_hantro_encoder.c'],
			name = 'imx8_encoder'
		)

		return {
			'uselib': ['HANTRO', 'HANTRO_DEC', 'HANTRO_ENC'],
			'use': ['imx8_decoder', 'imx8_encoder']
		}


imx_platforms = {
	'imx6': PlatformIMX6(),
	'imx8m': PlatformIMX8M(soc_type = 'MX8M', has_encoder = False),
	'imx8mm': PlatformIMX8M(soc_type = 'MX8MM', has_encoder = True)
}


def options(opt):
	opt.add_option('--enable-debug', action = 'store_true', default = False, help = 'enable debug build [default: disabled]')
	opt.add_option('--enable-static', action = 'store_true', default = False, help = 'build static library [default: build shared library]')
	opt.add_option('--imx-platform', action='store', default='', help='i.MX platform to build for (valid platforms: ' + ' '.join(imx_platforms.keys()) + ')')
	opt.add_option('--imx-headers', action='store', default='', help='path to where linux/ipu.h etc. can be found [default: <sysroot path>/usr/include/imx]')
	opt.add_option('--sysroot-path', action='store', default='', help='path to the sysroot')
	opt.add_option('--disable-examples', action = 'store_true', default = False, help = 'do not compile examples [default: build examples]')
	opt.load('compiler_c')
	opt.load('gnu_dirs')


def configure(conf):
	conf.load('compiler_c')
	conf.load('gnu_dirs')

	# check and add compiler flags

	basic_cflags = conf.env['CFLAGS'] or []
	basic_ldflags = conf.env['LINKFLAGS'] or []

	basic_cflags += ['-Wextra', '-Wall', '-pedantic', '-fPIC', '-DPIC']
	if conf.options.enable_debug:
		basic_cflags += ['-O0', '-g3', '-ggdb']
	else:
		basic_cflags += ['-O2']

	for cflag in basic_cflags:
		check_compiler_flag(conf, cflag, 1)
	for std in ['gnu99', 'c99']:
		check_compiler_flag(conf, '-std=' + std)
		check_combined_build_flags(conf, basic_cflags + ['-std=' + std], basic_ldflags, 1)
		conf.env['CFLAGS_' + std.upper()] = ['-std=' + std]


	conf.env['CFLAGS'] = basic_cflags
	conf.env['LINKFLAGS'] = basic_ldflags
	conf.env['BUILD_STATIC'] = conf.options.enable_static
	conf.env['DISABLE_EXAMPLES'] = conf.options.disable_examples


	# check libimxdmabuffer dependency
	conf.check_cfg(package = 'libimxdmabuffer >= 1.1.1', uselib_store = 'IMXDMABUFFER', define_name = '', args = '--cflags --libs', mandatory = 1)


	# check sysroot path
	if not conf.options.sysroot_path:
		conf.fatal('Sysroot path not set; add --sysroot-path switch to configure command line')
	sysroot_path = os.path.abspath(os.path.expanduser(conf.options.sysroot_path))
	if os.path.isdir(sysroot_path):
		Logs.pprint('NORMAL', 'Using "%s" as sysroot path' % sysroot_path)
	else:
		conf.fatal('Path "%s" does not exist or is not a valid directory; cannot use as sysroot path' % sysroot_path)
	conf.env['SYSROOT'] = sysroot_path


	# check i.MX platform
	if not conf.options.imx_platform:
		conf.fatal('i.MX platform not defined; add --imx-platform switch to configure command line')
	imx_platform_id = conf.options.imx_platform
	try:
		imx_platform = imx_platforms[imx_platform_id]
	except KeyError:
		conf.fatal('Invalid i.MX platform "%s" specified; valid platforms: %s' % (imx_platform_id, ' '.join(imx_platforms.keys())))
	conf.env['IMX_PLATFORM'] = imx_platform_id


	# configure platform
	imx_platform.configure(conf)


	# process the library version number
	version_node = conf.srcnode.find_node('VERSION')
	if not version_node:
		conf.fatal('Could not open VERSION file')
	with open(version_node.abspath()) as x:
		version = x.readline().splitlines()[0]
	conf.env['IMXVPUAPI2_VERSION'] = version
	conf.define('IMXVPUAPI2_VERSION', version)
	Logs.pprint('NORMAL', 'libimxvpuapi version %s' % version)


	# write the config header
	conf.write_config_header('config.h')


def build(bld):
	imx_platform_id = bld.env['IMX_PLATFORM']
	imx_platform = imx_platforms[imx_platform_id]

	use_lists = imx_platform.build(bld)

	bld(
		features = ['c', 'cstlib' if bld.env['BUILD_STATIC'] else 'cshlib'],
		includes = ['.'],
		uselib = ['IMXDMABUFFER', 'C99'] + use_lists['uselib'],
		use = use_lists['use'],
		source = ['imxvpuapi2/imxvpuapi2.c', 'imxvpuapi2/imxvpuapi2_priv.c', 'imxvpuapi2/imxvpuapi2_jpeg.c'],
		name = 'imxvpuapi2',
		target = 'imxvpuapi2',
		install_path="${LIBDIR}",
		vnum = bld.env['IMXVPUAPI2_VERSION']
	)

	bld.install_files('${PREFIX}/include/imxvpuapi2/', ['imxvpuapi2/imxvpuapi2.h', 'imxvpuapi2/imxvpuapi2_jpeg.h'])

	bld(
		features = ['subst'],
		source = "libimxvpuapi2.pc.in",
		target="libimxvpuapi2.pc",
		install_path="${LIBDIR}/pkgconfig"
	)

	if not bld.env['DISABLE_EXAMPLES']:
		examples = [ \
			{ 'name': 'decode-example',         'source': ['example/decode-example.c']         }, \
			{ 'name': 'encode-example',         'source': ['example/encode-example.c']         }, \
			{ 'name': 'jpeg-dec-example',       'source': ['example/jpeg-dec-example.c']       }, \
			{ 'name': 'jpeg-enc-example',       'source': ['example/jpeg-enc-example.c']       }, \
		]

		bld(
			features = ['c'],
			includes = ['.', 'example'],
			uselib = ['IMXDMABUFFER', 'C99'],
			use = 'imxvpuapi2',
			source = ['example/main.c', 'example/y4m_io.c', 'example/h264_utils.c'],
			name = 'examples-common'
		)

		for example in examples:
			bld(
				features = ['c', 'cprogram'],
				includes = ['.', 'example'],
				uselib = ['IMXDMABUFFER', 'C99'],
				use = 'imxvpuapi2 examples-common',
				source = example['source'],
				target = 'example/' + example['name'],
				install_path = None # makes sure the example is not installed
			)
