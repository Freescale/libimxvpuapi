libimxvpuapi - frontend for i.MX hardware video codecs
======================================================

This library provides an API for using hardware video codecs on i.MX platforms.
The API abstracts away platform specific details and allows for using the same
code with different hardware video codecs on different i.MX platforms.

The hardware video codec is referred to as the _VPU_.

Currently, the following platforms are supported (listed with their VPUs):

* i.MX6 (Chips&Media CODA960 codec)
* i.MX8m quad (Hantro G1/G2 decoder, no encoder)
  it is also sometimes referred to as just "the i.MX8m"
* i.MX8m mini (Hantro G1/G2 decoder, Hantro H1 encoder)
* i.MX8m plus (Hantro G1/G2 decoder, Hantro VC8000E encoder)

Not yet supported:

* i.MX8 / i.MX8X (Amphion Malone codec)

This is the second version of this library. Differences to the older version
are described below.


License
-------

This library is licensed under the LGPL v2.1.


Dependencies
------------

libimxvpuapi depends on [libimxdmabuffer](https://github.com/dv1/libimxdmabuffer).

Additional dependencies are specific to the target platform:

* i.MX6: `imx-vpu` package version 3.10.17 or newer.
* i.MX8m quad & i.MX8m mini: `imx-vpu-hantro` 1.8.0 or newer.
  Please note that the build scripts assume that this is a version of the
  `imx-vpu-hantro` package with fixed header installation destination. Earlier
  versions installed all Hantro headers in the main include directory. Newer
  ones create `hantro_enc` and `hantro_dec`subdirectories. libimxvpuapi
  expects these directories to exist.
* i.MX8m plus: `imx-vpu-hantro` 1.8.0 or newer, just like above
  (since the plus has the same G1 / G2 decoder as the i.MX8m mini),
  and also `imx-vpu-hantro-vc` 1.1.0 or newer (for the VC8000E encoder).


Building and installing
-----------------------

This project uses the [waf meta build system](https://waf.io/).
To configure , first set the following environment variables to whatever is
necessary for cross compilation for your platform:

* `CC`
* `CFLAGS`
* `LDFLAGS`
* `PKG_CONFIG_PATH`
* `PKG_CONFIG_SYSROOT_DIR`

Then, run:

    ./waf configure --prefix=PREFIX --imx-platform=IMX_PLATFORM --sysroot-path=SYSROOT

(The aforementioned environment variables are only necessary for this
configure call.)

The arguments are as follows:
* `PREFIX` defines the installation prefix, that is, where the built binaries
  will be installed.
* `IMX_PLATFORM` specifies what i.MX platform to build for. See the list below
  for the valid values.
* `SYSROOT` is the absolute path to the sysroot for the platform. This is the
  path where `usr/include/imx/mxcfb.h` can be found. In cross compilation
  environments like Yocto or buildroot, this is where the sysroot files for the
  target i.MX platforms are.

Valid `IMX_PLATFORM` values are:

* `imx6` : i.MX6 (all variants)
* `imx8m` : i.MX8m quad
* `imx8mm` : i.MX8m mini
* `imx8mp` : i.MX8m plus


Once configuration is complete, run:

    ./waf

This builds the library.
Finally, to install, run:

    ./waf install

This will install the headers in `$PREFIX/include/imxvpuapi2/`, the libraries
in `$PREFIX/lib/`, and generate a pkg-config .pc file, which is placed in
`$PREFIX/lib/pkgconfig/` .


API documentation
-----------------

The API is documented in the `imxvpuapi/imxvpuapi.h` header.


Examples
--------

libimxvpuapi comes with these examples in the `example/` directory:

* `decode-example.c` : demonstrates how to use the decoder API
* `encode-example.c` : demonstrates how to use the encoder API

(Other source files in the `example/` directory are common utility code used
by all examples above.)

Raw frames are read/written as [YUV4MPEG2 (y4m) data](https://wiki.multimedia.cx/index.php/YUV4MPEG2),
and encoded to / decoded from h.264.


Known issues
------------

**i.MX6 VPU timeout**

If errors like `imx_vpu_api_dec_decode() failed: timeout` or `VPU blocking: timeout`
are observed, check if the following workarounds for known problems help:

* Overclocked VPU: The VPU is clocked at 266 MHz by default (according to the
  VPU documentation). Some configurations clock the VPU at 352 MHz, and can
  exhibit VPU timeout problems, particularly during h.264 encoding. Try running
  the VPU at 266 MHz.

* Known issue with IPU configuration: As shown by [this Github entry](https://github.com/Freescale/libimxvpuapi/issues/11),
  the `CONFIG_IMX_IPUV3_CORE` kernel config flag can cause problems with the
  VPU. Disable it, then try again.

* Low-level VPU library bug: imx-vpu versions prior to 5.4.31 also have been
  observed to cause VPU timeouts. These seem to be related to the 5.4.31 fix
  described as: "Fix VPU blocked in BWB module".

**VP6 frames decoded upside down by Hantro G1 decoder**

This seems to be a bug in imx-vpu-hantro. A workaround is currently not known.


Differences to the older libimxvpuapi version
---------------------------------------------

This is version 2 of libimxvpuapi. Major changes are:

* The API has been rewritten, and is incompatible with the older one.
* The `imx_vpu_` prefix has been changed to `imx_vpu_api_`.
* Files have been renamed from `imxvpuapi*` to `imxvpuapi2` to reflect the
  new and incompatible API and to allow for coexistence with the old version
  of the library.
* DMA allocation functions have been factored out as a separate library,
  [libimxdmabuffer](https://github.com/Freescale/libimxdmabuffer).
* The old API required checks with `imx_vpu_dec_check_if_can_decode()` to see
  if decoding is possible now to avoid a deadlock. This is no longer needed.
  Decoders either perform internal DMA-based copies of frames (i.MX6, in the
  same step as detiling from the VPU layout), or accept additional framebuffers
  in their framebuffer pools on the fly. This simplifies the use of the
  decoding API considerably.
* API for getting global en/decoder information and list of supported formats,
  profiles, levels has been added.
* Color formats have been merged with the semi/fully planar attribute.
* h.265 colorimetry information has been added.
* Encoder API now returns encoded data in a two-step approach: First, the size
  of the encoded data is returned. Then the user allocates a buffer with at
  least that size. Finally, that buffer is passed to the encoder to retrieve
  the encoded data.
* the `imx_vpu_load()` and `imx_vpu_unload()` functions are gone. (Un)loading
  the i.MX6 VPU firmware is now done internally.
* 10-bit support and tiled output support have been added (the latter's tiling
  layout is platform specific).
* `codec_format` was renamed to `compression_format` to avoid confusion with
  the notion of hardware video codecs.


To do
-----

* Add more encoder options (and evaluate which ones to add)
* More wiki entries describing format support per i.MX platform / codec
* RealVideo decoding support
