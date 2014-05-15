libimxvpuapi
============

About
-----

This library provides an API for using the i.MX VPU video engine. It is an alternative to Freescale's VPU wrapper.
Both the wrapper and this library are layers on top of imx-vpu , the low-level i.MX VPU interface.

The aim is to provide a cleaned up API with additional features, most prominently:
* User-defined context associated with input frames, which is passed on to corresponding output frames
  (to be able to identify which input frame produced which output frame)
* Groundwork for future DMA-BUF/BMM/ION/CMA allocator integration, using file descriptors instead of physical addresses
* Indicators for number of currently free framebuffers, which is critical in multi-threaded playback cases
* Switchable implementation backend: currently, the backend lies on top of the VPU wrapper, but the goal is to
  implement it directly on top of imx-vpu


License
-------

This library is licensed under the LGPL v2.1.


Dependencies
------------

Currently, the lone dependency is libfslvpuwrap 1.0.45 or newer. This is likely to change in the future.


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
