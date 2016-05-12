libimxvpuapi - frontend for the i.MX6 VPU hardware video engine
===============================================================

This library provides an API for using the iMX6 VPU video engine. It is an alternative to Freescale's VPU wrapper.
Both the wrapper and this library are layered on top of imx-vpu , the low-level iMX6 VPU interface.

The aim is to provide a cleaned up API with additional features, most prominently:
* User-defined context information associated with input frames, which is passed on to corresponding output frames
  (to be able to identify which input frame produced which output frame)
* Groundwork for future DMA-BUF/BMM/ION/CMA allocator integration, using file descriptors instead of physical addresses
* Indicators for when it is safe to try to decode frames, which is critical in multi-threaded playback cases
* Switchable implementation backend: the main backend is the "vpulib" backend, which uses the low-level imx-vpu
  library directly; an alternative backend is the "fslwrapper" backend, based on the Freescale VPU wrapper
* Simplified, higher-level JPEG en/decoding API, based on the VPU MJPEG codec; useful for picture viewing without
  the extra boilerplate for VPU-based en/decoding


License
-------

This library is licensed under the LGPL v2.1.


Dependencies
------------

This depends on the backend in use. The default (the vpulib backend) needs imx-vpu 3.10.17 or newer. The fslwrapper
backend needs libfslvpuwrap 1.0.45 or newer.


Supported formats
-----------------

These formats correspond to the capabilities of the VPU hardware.
Only a subset of these are also supported by the encoder.
Unless otherwise noted, the maximum supported resolution is 1920x1088. 

* MPEG-1 part 2 and MPEG-2 part 2:
  Decoding: Fully compatible with the ISO/IEC 13182-2 specification and the main and high
            profiles. Both progressive and interlaced content is supported.

* MPEG-4 part 2:
  Decoding: Supports simple and advanced simple profile (except for GMC).
            NOTE: DivX 3/5/6 are not supported and require special licensing by Freescale.
  Encoding: Supports the simple profile and max. level 5/6.

* h.263:
  Decoding: Supports baseline profile and Annex I, J, K (except for RS/ASO), T, and max. level 70.
  Encoding: Supports baseline profile and Annex I, J, K (RS and ASO are 0), T, and max. level 70.

* h.264:
  Decoding: Supports baseline, main, high profiles, max. level 4.1. (10-bit decoding is not supported.)
            Only Annex.B byte-stream formatted input is supported.
  Encoding: Supports baseline and constrained baseline profile, max. level 4.0.
            Only Annex.B byte-stream formatted output is supported.

* WMV3 (also known as Windows Media Video 9):
  Compatible to VC-1 simple and main profiles.
  Decoding: Fully supported WMV3 decoding, excluding the deprecated WMV3 interlace support
            (which has been obsoleted by the interlacing in the VC-1 advanced profile).

* VC-1 (also known as Windows Media Video 9 Advanced Profile):
  Decoding: SMPTE VC-1 compressed video standard fully supported. Max. level is 3.

* Motion JPEG:
  Decoding: Only baseline JPEG frames are supported. Maximum resolution is 8192x8192.
  Encoding: Only baseline JPEG frames are supported. Maximum resolution is 8192x8192.
            NOTE: Encoder always operates in constant quality mode, even if the open
            params have a nonzero bitrate set.

* VP8:
  Decoding: fully compatible with the VP8 decoding specification.
            Both simple and normal in-loop deblocking are supported.
            NOTE: VPU specs state that the maximum supported resolution is 1280x720, but
            tests show that up to 1920x1088 pixels do work.


Building and installing
-----------------------

This project uses the [waf meta build system](https://code.google.com/p/waf/). To configure , first set
the following environment variables to whatever is necessary for cross compilation for your platform:

* `CC`
* `CFLAGS`
* `LDFLAGS`
* `PKG_CONFIG_PATH`
* `PKG_CONFIG_SYSROOT_DIR`

Then, run:

    ./waf configure --prefix=PREFIX

(The aforementioned environment variables are only necessary for this configure call.)
PREFIX defines the installation prefix, that is, where the built binaries will be installed.

Once configuration is complete, run:

    ./waf

This builds the library.
Finally, to install, run:

    ./waf install

This will install the headers in `$PREFIX/include/imxvpuapi/` , the libraries in `$PREFIX/lib/` ,
and generate a pkg-config .pc file, which is placed in `$PREFIX/lib/pkgconfig/` .


Selecting backends
------------------

By default, the vpulib backend is used. To use the fslwrapper backend, an additional switch has to be
passed to the configuration step:

    ./waf configure --prefix=PREFIX --use-fslwrapper-backend

Please note that the fslwrapper backend should not be used unless there are serious problems with the
vpulib backend; due to limitations in the VPU wrapper's API, the fslwrapper backend does not achieve
the vpulib's functionality fully. The fslwrapper backend is mainly used for debugging to enable
comparisons in behavior between libimxvpuapi and libfslwrapper.


API documentation
-----------------

The API is documented in these headers:

* `imxvpuapi/imxvpuapi.h` : main en/decoding API
* `imxvpuapi/imxvpuapi_jpeg.h` : simplified JPEG en/decoding API


Examples
--------

libimxvpuapi comes with these examples in the `example/` directory:

* `decode-example.c` : demonstrates how to use the decoder API for decoding an h.264 video
* `encode-example.c` : demonstrates how to use the encoder API for encoding an h.264 video
* `jpeg-dec-example.c` : demonstrates how to use the simplified JPEG API for decoding JPEG files
* `jpeg-enc-example.c` : demonstrates how to use the simplified JPEG API for encoding JPEG files

(Other source files in the `example/` directory are common utility code used by all examples above.)


VPU timeout issues
------------------

If errors like `imx_vpu_dec_decode() failed: timeout` or `VPU blocking: timeout` are observed, check if the
following workarounds for known problems help:

* Overclocked VPU: The VPU is clocked at 266 MHz by default (according to the VPU documentation). Some
  configurations clock the VPU at 352 MHz, and can exhibit VPU timeout problems, particularly during
  h.264 encoding. Try running the VPU at 266 MHz.

* Known issue with IPU configuration: As shown by [this Github entry](https://github.com/Freescale/libimxvpuapi/issues/11),
  the `CONFIG_IMX_IPUV3_CORE` kernel config flag can cause problems with the VPU. Disable it, then try again.

* Low-level VPU library bug: imx-vpu versions prior to 5.4.31 also have been observed to cause VPU timeouts.
  These seem to be related to the 5.4.31 fix described as: "Fix VPU blocked in BWB module".
