#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <dirent.h>
#include <pthread.h>
#include <limits.h>

#include <config.h>

#include <g2d.h>
#include <g2dExt.h>

#include <imxdmabuffer/imxdmabuffer.h>
#include <imxdmabuffer/imxdmabuffer_ion_allocator.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"





/* IMPORTANT: This code mentions "output" and "capture" buffers.
 * This is V4L2 mem2mem terminology. Mem2mem devices have two
 * queues, an OUTPUT and a CAPTURE queue. In decoders, encoded
 * data is fed into the decoder through the output queue. Raw,
 * decoded data is retrieved from the capture queue. Especially
 * the "output" buffers can sound confusing (since encoded data
 * is the input for decoders, meaning that _input_ data is fed
 * into an _output_ queue), so keep this distinction in mind
 * when reading this code. */




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


#define MAX(a, b) ( ((a) > (b)) ? (a) : (b) )

/* Extra V4L2 FourCC's specific to the Amphion Malone decoder. */

#ifndef V4L2_VPU_PIX_FMT_VP6
#define V4L2_VPU_PIX_FMT_VP6         v4l2_fourcc('V', 'P', '6', '0')
#endif

#ifndef V4L2_VPU_PIX_FMT_AVS
#define V4L2_VPU_PIX_FMT_AVS         v4l2_fourcc('A', 'V', 'S', '0')
#endif

#ifndef V4L2_VPU_PIX_FMT_RV
#define V4L2_VPU_PIX_FMT_RV          v4l2_fourcc('R', 'V', '0', '0')
#endif

#ifndef V4L2_VPU_PIX_FMT_VP6
#define V4L2_VPU_PIX_FMT_VP6         v4l2_fourcc('V', 'P', '6', '0')
#endif

#ifndef V4L2_VPU_PIX_FMT_SPK
#define V4L2_VPU_PIX_FMT_SPK         v4l2_fourcc('S', 'P', 'K', '0')
#endif

#ifndef V4L2_VPU_PIX_FMT_DIV3
#define V4L2_VPU_PIX_FMT_DIV3        v4l2_fourcc('D', 'I', 'V', '3')
#endif

#ifndef V4L2_VPU_PIX_FMT_DIVX
#define V4L2_VPU_PIX_FMT_DIVX        v4l2_fourcc('D', 'I', 'V', 'X')
#endif

#ifndef V4L2_PIX_FMT_NV12_10BIT
#define V4L2_PIX_FMT_NV12_10BIT      v4l2_fourcc('N', 'T', '1', '2') /*  Y/CbCr 4:2:0 for 10bit  */
#endif

#ifndef V4L2_EVENT_SKIP
#define V4L2_EVENT_SKIP                      (V4L2_EVENT_PRIVATE_START + 2)
#endif

#define V4L2_FOURCC_ARGS(FOURCC) \
		  ((char)(((FOURCC) >> 0)) & 0xFF) \
		, ((char)(((FOURCC) >> 8)) & 0xFF) \
		, ((char)(((FOURCC) >> 16)) & 0xFF) \
		, ((char)(((FOURCC) >> 24)) & 0xFF)


static uint32_t convert_to_v4l2_fourcc(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG: return V4L2_PIX_FMT_MJPEG;
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG2: return V4L2_PIX_FMT_MPEG2;
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4: return V4L2_PIX_FMT_MPEG4;
		case IMX_VPU_API_COMPRESSION_FORMAT_H263: return V4L2_PIX_FMT_H263;
		case IMX_VPU_API_COMPRESSION_FORMAT_H264: return V4L2_PIX_FMT_H264;
		case IMX_VPU_API_COMPRESSION_FORMAT_H265: return V4L2_PIX_FMT_HEVC;
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3: return V4L2_PIX_FMT_VC1_ANNEX_L;
		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1: return V4L2_PIX_FMT_VC1_ANNEX_G;
		case IMX_VPU_API_COMPRESSION_FORMAT_VP6: return V4L2_VPU_PIX_FMT_VP6;
		case IMX_VPU_API_COMPRESSION_FORMAT_VP8: return V4L2_PIX_FMT_VP8;
		case IMX_VPU_API_COMPRESSION_FORMAT_VP9: return V4L2_PIX_FMT_VP9;
		case IMX_VPU_API_COMPRESSION_FORMAT_AVS: return V4L2_VPU_PIX_FMT_AVS;
		case IMX_VPU_API_COMPRESSION_FORMAT_RV30: return V4L2_VPU_PIX_FMT_RV;
		case IMX_VPU_API_COMPRESSION_FORMAT_RV40: return V4L2_VPU_PIX_FMT_RV;
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX3: return V4L2_VPU_PIX_FMT_DIV3;
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX4: return V4L2_VPU_PIX_FMT_DIVX;
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX5: return V4L2_VPU_PIX_FMT_DIVX;
		case IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK: return V4L2_VPU_PIX_FMT_SPK;
		default: return 0;
	}
}


#define VPU_DEVICE_FILENAME_LENGTH 512

typedef struct
{
	BOOL initialized;
	char decoder_filename[VPU_DEVICE_FILENAME_LENGTH];
	char encoder_filename[VPU_DEVICE_FILENAME_LENGTH];
}
VpuDeviceFilenames;

static VpuDeviceFilenames vpu_device_filenames;
static pthread_mutex_t vpu_device_fn_mutex = PTHREAD_MUTEX_INITIALIZER;


static void init_vpu_device_filenames(void)
{
	DIR *dir = NULL;
	char tempstr[VPU_DEVICE_FILENAME_LENGTH];
	static char const device_node_fn_prefix[] = "/dev/video";
	static size_t const device_node_fn_prefix_length = sizeof(device_node_fn_prefix) - 1;
	struct dirent *dir_entry;

	pthread_mutex_lock(&vpu_device_fn_mutex);

	if (vpu_device_filenames.initialized)
		goto finish;

	IMX_VPU_API_DEBUG("scanning for VPU device nodes");

	memset(&vpu_device_filenames, 0, sizeof(vpu_device_filenames));

	dir = opendir("/dev");
	if (dir == NULL)
	{
		IMX_VPU_API_ERROR("could not open /dev/ directory to look for V4L2 device nodes: %s (%d)", strerror(errno), errno);
		goto finish;
	}

	while ((dir_entry = readdir(dir)) != NULL)
	{
		int index;
		int fd = -1;
		BOOL is_valid_decoder;
		BOOL is_valid_encoder;
		struct stat entry_stat;
		struct v4l2_capability capability;
		struct v4l2_fmtdesc format_desc;

		snprintf(tempstr, sizeof(tempstr), "/dev/%s", dir_entry->d_name);

		/* Run stat() on the file with filename tempstr, and perform
		 * checks on that call's output to filter out candidates. */

		if (stat(tempstr, &entry_stat) < 0)
		{
			switch (errno)
			{
				case EACCES:
					IMX_VPU_API_DEBUG("skipping \"%s\" while looking for V4L2 device nodes since access was denied", tempstr);
					break;
				default:
					IMX_VPU_API_ERROR("stat() call on \"%s\" failed: %s (%d)", tempstr, strerror(errno), errno);
					break;
			}

			goto next;
		}

		if (!S_ISCHR(entry_stat.st_mode))
			goto next;

		if (strncmp(tempstr, device_node_fn_prefix, device_node_fn_prefix_length) != 0)
			goto next;

		/* This might be a valid en/decoder. Open a FD and perform
		 * V4L2 queries to further analyze this device node. */

		fd = open(tempstr, O_RDWR);
		if (fd < 0)
		{
			IMX_VPU_API_DEBUG("could not open device node \"%s\": %s (%d) - skipping", tempstr, strerror(errno), errno);
			goto next;
		}

		if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
		{
			IMX_VPU_API_DEBUG("could not query V4L2 capability from device node \"%s\": %s (%d) - skipping", tempstr, strerror(errno), errno);
			goto next;
		}

		if ((capability.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) == 0)
		{
			IMX_VPU_API_DEBUG("skipping V4L2 device \"%s\" since it does not support multi-planar mem2mem processing", tempstr);
			goto next;
		}

		if ((capability.capabilities & V4L2_CAP_STREAMING) == 0)
		{
			IMX_VPU_API_DEBUG("skipping V4L2 device \"%s\" since it does not support frame streaming", tempstr);
			goto next;
		}

		is_valid_decoder = FALSE;
		is_valid_encoder = FALSE;

		IMX_VPU_API_DEBUG("analyzing device node \"%s\"", tempstr);

		/* Check if this device node is a valid decoder. Do this by
		 * looking at the input formats it supports. The Malone
		 * decoder supports h.264 as input, so check for that. */

		for (index = 0; ; ++index)
		{
			memset(&format_desc, 0, sizeof(format_desc));
			format_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			format_desc.index = index;

			if (ioctl(fd, VIDIOC_ENUM_FMT, &format_desc) < 0)
			{
				/* EINVAL is not an actual error. It just denotes that
				 * we have reached the list of supported formats. */
				if (errno != EINVAL)
					IMX_VPU_API_DEBUG("could not query output format (index %d) from from decoder candidate \"%s\": %s (%d) - skipping", index, tempstr, strerror(errno), errno);

				break;
			}

			IMX_VPU_API_DEBUG("  input format query returned fourCC for format at index %d: %" FOURCC_FORMAT, index, V4L2_FOURCC_ARGS(format_desc.pixelformat));
			if (format_desc.pixelformat == V4L2_PIX_FMT_H264)
			{
				is_valid_decoder = TRUE;
				break;
			}
		}

		if (is_valid_decoder)
			memcpy(vpu_device_filenames.decoder_filename, tempstr, VPU_DEVICE_FILENAME_LENGTH);

		/* Check if this device node is a valid encoder. Do this by
		 * looking at the output formats it supports. The Windsor
		 * encoder supports h.264 as output, so check for that. */

		for (index = 0; ; ++index)
		{
			memset(&format_desc, 0, sizeof(format_desc));
			format_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			format_desc.index = index;

			if (ioctl(fd, VIDIOC_ENUM_FMT, &format_desc) < 0)
			{
				/* EINVAL is not an actual error. It just denotes that
				 * we have reached the list of supported formats. */
				if (errno != EINVAL)
					IMX_VPU_API_DEBUG("could not query capture format (index %d) from from encoder candidate \"%s\": %s (%d) - skipping", index, tempstr, strerror(errno), errno);

				break;
			}

			IMX_VPU_API_DEBUG("  output format query returned fourCC for format at index %d: %" FOURCC_FORMAT, index, V4L2_FOURCC_ARGS(format_desc.pixelformat));
			if (format_desc.pixelformat == V4L2_PIX_FMT_H264)
			{
				is_valid_encoder = TRUE;
				break;
			}
		}

		if (is_valid_encoder)
			memcpy(vpu_device_filenames.encoder_filename, tempstr, VPU_DEVICE_FILENAME_LENGTH);

		if (is_valid_encoder)
			IMX_VPU_API_DEBUG("device node \"%s\" is a valid encoder", tempstr);
		else if (is_valid_decoder)
			IMX_VPU_API_DEBUG("device node \"%s\" is a valid decoder", tempstr);
		else
			IMX_VPU_API_DEBUG("device node \"%s\" is neither a valid encoder nor a valid decoder", tempstr);

next:
		if (fd >= 0)
			close(fd);
	}

	vpu_device_filenames.initialized = TRUE;

finish:
	if (dir != NULL)
		closedir(dir);

	pthread_mutex_unlock(&vpu_device_fn_mutex);
}


/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* This imxvpuapi decoder works through the Video4Linux memory-to-memory
 * (mem2mem) API. Its documentation can be found here:
 *
 * https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html
 *
 * This is written for the Amphion Malone driver as it exists in
 * the NXP kernel (at time of writing this code, 2021-09-05). This
 * driver has several peculiarities that make it necessary to add
 * custom handling. Furthermore, by adding Amphion Malone support,
 * libimxvpuapi becomes universally usable across all i.MX6 and
 * i.MX8 SoCs.
 *
 * Most of this code is rather straightforward mem2mem use. There
 * are three important peculiarities:
 *
 * 1. The libimxvpuapi API associates raw and encoded frames through
 *    PTS, DTS, and a context pointer. All three of these are purely
 *    user defined. The library just passes these values through
 *    between associated frames. The Amphion Malone driver is not
 *    designed to work this way, however - it expects the timestamp
 *    field in the v4l2_buffer instances that are fed to the V4L2
 *    output queue to be PTS. The driver uses this timestamp to reorder
 *    frames. This means that the timestamp field cannot just be set
 *    to any arbitrary value (like some form of index). This complicates
 *    the logic, and makes it mandatory for the user to set the PTS
 *    in encoded input frames to a valid value. Normally, mem2mem
 *    drivers do not use the timestamp field for frame reordering,
 *    so this is a deviation that needs to be taken into account here.
 * 2. Some codecs like VP8 feature "invisible" frames that are meant for
 *    internal use during decoding, and are _not_ supposed to be shown.
 *    The Malone decoder skips these. Unfortunately, while the driver does
 *    have a custom V4L2_EVENT_SKIP V4L2 event, that event only informs us
 *    that "a" frame was skipped, not which frame exactly. This event is
 *    used for adjusting the num_detected_skipped_frames (that field is
 *    incremented every time the skip event is received).
 * 3. Amphion Malone frames are decoded to a custom tiled version of NV12.
 *    These tiled frames are detiled in imx_vpu_api_dec_decode() using
 *    the G2D API. This also implicitly makes a frame copy, which allows
 *    imx_vpu_api_dec_decode() to immediately return the v4l2_buffer
 *    that holds the decoded frame back to the V4L2 capture queue.
 *    The de-tiled copy is stored in output_frame_dma_buffer.
 *    (Note that on i.MX8 SoCs, there is no actual G2D core. Instead,
 *    there is the "DPU", which can function as a subsystem for 2D blit.
 *    operations. Currently, the DPU is only accessible through an
 *    emulated G2D layer. This might change in the future if direct
 *    DPU access through NXP's libdrm fork becomes possible.)
 *
 * The first two peculiarities are distinct, and yet related.
 * They are handled in this way:
 *
 * In the ImxVpuApiDecoder, there is the frame_context_items array.
 * A frame context item contains the PTS, DTS, and context pointer that
 * was passed to imx_vpu_api_dec_push_encoded_frame (inside the
 * encoded_frame argument). This set of parameters is inserted into the
 * array in an unused or new frame context item. When the driver later
 * decodes a frame, the timestamp field of the v4l2_buffer that holds the
 * decoded frame is looked at. A frame context item with a matching PTS
 * is searched. Once found, the PTS, DTS, and context values in that
 * frame context item are passed to the decoded_frame argument in the
 * imx_vpu_api_dec_get_decoded_frame() call.
 *
 * The frame_context_items array is automatically expanded if necessary.
 * It cannot just grow unconstrained though - in imx_vpu_api_dec_decode(),
 * there is a throttling mechanism. This is necessary because writing an
 * encoded frame into the output queue is not subject to blocking (it is
 * a queue after all). It is therefore possible to feed a lot of encoded
 * frames before a frame is decoded and returned. To migitate this,
 * the throttling mechanism looks at how many of the frame context items
 * are currently in use. If this number is below the value in
 * used_frame_context_item_count_limit, imx_vpu_api_dec_decode() signals
 * that the user can feed in encoded data. If the number of used frames
 * is at that limit, this is not signaled.
 *
 * Skipped frames require special attention because as said, there is no
 * way by which the V4L2 driver is telling exactly which frame has been
 * skipped. The only way to determine which frames were skipped is to look
 * at the frame context items that are in use. The idea is that over time,
 * the frame context items that are associated with skipped frames stick
 * around, and progressively become "older". Therefore, after a while,
 * the oldest frame context item must belong to a skipped frame. (The
 * "age" is determined by the PTS; smallest PTS value of all items ->
 * oldest frame context item).
 *
 * This ties into the throttling mechanism described above. The
 * used_frame_context_item_count_limit does _not_ include the frame context
 * items that are in use by _skipped_ frames. In imx_vpu_api_dec_decode(),
 * if the number of used frame context items is at or exceeds the limit,
 * _and_ the value of num_detected_skipped_frames is nonzero, then the
 * oldest frame context item must belong to a skipped frame, and is
 * "garbage collected". The num_detected_skipped_frames check is essential
 * to make sure frame context items aren't prematurely garbage collected.
 */


/* We need 2 buffers for the output queue, where encoded frames
 * are pushed to be decoder. One buffer is in the queue, the
 * other is available for accepting more encoded data. */
#define DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS 2

/* We allocate 2 MB for each output v4l2_buffer. This gives
 * us plenty of room. Encoded frames are expected to be
 * far smaller than this. */
#define DEC_REQUESTED_OUTPUT_BUFFER_SIZE (2 * 1024 * 1024)

/* The number of planes in capture buffers. The Amphion
 * Malone decoder always produces NV12 data (8 or 10 bit), so
 * there are always exactly 2 planes (one Y- and one UV-plane).
 * Note that the actual _output_ of the decoder can be something
 * different, since there is a detiling process in between the
 * dequeuing of the capture buffers and the actual decoder output.
 * That detiling can produce a number of color formats. */
#define DEC_NUM_CAPTURE_BUFFER_PLANES 2

/* An alignment of 128 is required for the Amphion detiling.
 * Note that this is required for the _destination_ surface.
 * If that surface is not aligned this way, the resulting
 * detiled frames are corrupted. */
#define G2D_DEST_AMPHION_STRIDE_ALIGNMENT 128
#define G2D_ROW_COUNT_ALIGNMENT 8


typedef struct
{
	/* context pointer, PTS, DTS from the input frame. */
	void *context;
	uint64_t pts, dts;
	/* PTS in microseconds. Used for comparing context items. */
	uint64_t pts_microseconds;
	/* Set to TRUE if the frame context currently holds PTS/DTS/context
	 * data of a pushed encoded frame. FALSE if this item is currently
	 * not used and thus available for storing context data. */
	BOOL in_use;
}
DecFrameContextItem;


/* Structure for housing a V4L2 output buffer and its associated plane structure.
 * Note that "output" is V4L2 mem2mem decoder terminology for "encoded data". */
typedef struct
{
	/* The buffer's "planes" pointer is set to point to the "plane" instance
	 * below when the decoder's output_buffer_items are allocated.
	 * This happens in the imx_vpu_api_dec_open() function. */
	struct v4l2_buffer buffer;
	/* Since the Amphion decoder uses the multi-planar API, we need to
	 * specify a plane structure. (Encoded data uses exactly 1 "plane"). */
	struct v4l2_plane plane;
}
DecV4L2OutputBufferItem;


/* Structure for housing a V4L2 capture buffer and its associated
 * plane structure and DMA buffers for housing raw frames. */
typedef struct
{
	/* The buffer's "planes" pointer is set to point to the "plane" instance
	 * below when the decoder's capture_buffer_items are allocated.
	 * This happens in the imx_vpu_api_dec_handle_resolution_change() function. */
	struct v4l2_buffer buffer;
	/* Since the Amphion decoder uses the multi-planar API, we need to
	 * specify a plane structure. */
	struct v4l2_plane planes[DEC_NUM_CAPTURE_BUFFER_PLANES];
	/* The DMA buffer that actually contains the captured frame. */
	ImxDmaBuffer *dma_buffer;
}
DecV4L2CaptureBufferItem;


struct _ImxVpuApiDecoder
{
	int v4l2_fd;


	/* Output queue states */

	/* Array of allocated output buffer items that contain V4L2 output buffers.
	 * There is exactly one output buffer item for each V4L2 output buffer that
	 * was allocated with the VIDIOC_REQBUFS ioctl in imx_vpu_api_dec_open(). */
	DecV4L2OutputBufferItem *output_buffer_items;
	int num_output_buffers;

	/* TRUE if the output queue was enabled with the VIDIOC_STREAMON ioctl. */
	BOOL output_stream_enabled;

	/* The actual output buffer format, retrieved by using the VIDIOC_G_FMT ioctl.
	 * The driver may pick a format that differs from the requested format
	 * (requested with the VIDIOC_S_FMT ioctl), so we store the actual format here. */
	struct v4l2_format output_buffer_format;

	/* Size in bytes of one V4L2 output buffer. This needs to be
	 * passed to mmap() when writing encoded data to such a buffer. */
	int output_buffer_size;

	/* How many of the output buffers have been pushed into the output queue
	 * with the VIDIOC_QBUF ioctl and haven't yet been dequeued again. */
	int num_output_buffers_in_queue;


	/* Capture buffer states */

	/* Array of allocated capture buffer items that contain V4L2 capture buffers.
	 * There is exactly one capture buffer item for each V4L2 capture buffer that
	 * was allocated with the VIDIOC_REQBUFS ioctl in
	 * imx_vpu_api_dec_handle_resolution_change(). */
	DecV4L2CaptureBufferItem *capture_buffer_items;
	int num_capture_buffers;

	/* TRUE if the capture queue was enabled with the VIDIOC_STREAMON ioctl. */
	BOOL capture_stream_enabled;

	/* The actual capture buffer format, retrieved by using the VIDIOC_G_FMT ioctl.
	 * The driver may pick a format that differs from the requested format
	 * (requested with the VIDIOC_S_FMT ioctl), so we store the actual format here. */
	struct v4l2_format capture_buffer_format;

	/* Size in bytes of one V4L2 capture buffer. This is used as the
	 * size for the DMA buffers in each DecV4L2CaptureBufferItem. */
	int capture_buffer_size;

	/* Plane sizes/offsets. Used for setting up the fields in the
	 * v4l2_plane instances in DecV4L2CaptureBufferItem. */
	int capture_buffer_y_offset, capture_buffer_uv_offset;
	int capture_buffer_y_stride, capture_buffer_uv_stride;
	int capture_buffer_y_size, capture_buffer_uv_size;

	/* This is the V4L2 pixelformat that was requested from the capture queue via
	 * the VIDIOC_S_FMT ioctl. The driver is free to pick a different one. To be
	 * able to check later if the driver picked a different format, we store a
	 * copy of that requested pixelformat here. Note that this _not_ what this
	 * decoder outputs; rather, it is related to what the driver produces.
	 * There is still the G2D/DPU based detiling in between. */
	uint32_t requested_v4l2_pixelformat;
	/* This is the V4L2 pixelformat that is actually used by the capture queue. */
	uint32_t actual_v4l2_pixelformat;


	/* Frame context states */

	/* Array of frame context items. A "frame context" is the tuple of
	 * PTS, DTS, and context pointer that is specified by the user through
	 * the imx_vpu_api_dec_push_encoded_frame() function's encoded_frame
	 * argument. That argument points to an ImxVpuApiEncodedFrame instance
	 * which contains these three values. The frame context is associated
	 * with an encoded and with the corresponding decoded frame. This means
	 * that the imx_vpu_api_dec_get_decoded_frame() returns a decoded frame
	 * and the PTS/DTS/context pointer frame context values that were
	 * associated with the corresponding encoded frame. This array is filled
	 * and expanded on-demand by imx_vpu_api_dec_add_frame_context().
	 * The array expansion is dictated by the behavior of the V4L2 driver,
	 * that is, when the driver decides to decode and output a frame.
	 * A larger array can happen with h.264 and h.265 streams that contain
	 * B-frames. And, if this array keeps getting expanded over time, it
	 * indicates that the driver is internally skipping invisible or
	 * corrupted frames. */
	DecFrameContextItem *frame_context_items;
	int num_frame_context_items;

	/* Array of frame context item indices (= indices into the
	 * frame_context_items array). Whenever a frame context is to be stored,
	 * this array is looked into first to reuse a currently unused frame context
	 * item. New items are allocated only if this array is empty. The size
	 * of this array is num_frame_context_items. */
	int *available_frame_context_item_indices;
	int num_available_frame_context_items;


	/* Decoded frame states */

	/* The output frame to transfer decoded pixels into. 
	 * These fields are set by imx_vpu_api_dec_set_output_frame_dma_buffer(). */
	ImxDmaBuffer *output_frame_dma_buffer;
	void *output_frame_fb_context;

	/* G2D surfaces used when detiling decoded frames. The source surface
	 * is tied to dequeued V4L2 capture buffers. The destination surface
	 * is tied to the output_frame_dma_buffer. */
	struct g2d_surfaceEx source_g2d_surface;
	struct g2d_surfaceEx dest_g2d_surface;

	void *g2d_handle;

	/* The format to use for decoded frames. This is the result of the frame
	 * decoding (done by the driver) and the detiling (done by the DPU/G2D).
	 * This is what imx_vpu_api_dec_get_decoded_frame() outputs. */
	ImxVpuApiColorFormat decoded_frame_format;

	DecFrameContextItem *decoded_frame_context_item;
	int decoded_frame_context_index;


	/* Miscellaneous */

	/* Stream info that is filled in imx_vpu_api_dec_handle_resolution_change(). */
	ImxVpuApiDecStreamInfo stream_info;
	/* Initially set to FALSE. When the V4L2_EVENT_SOURCE_CHANGE event is observed,
	 * imx_vpu_api_dec_handle_resolution_change() is called, which sets this to TRUE. */
	BOOL stream_info_announced;

	/* This is TRUE if an imx_vpu_api_dec_decode() call actually
	 * decoded a frame. imx_vpu_api_dec_get_decoded_frame() uses
	 * this to check for incorrect calls (that is, when the caller
	 * tries to get a decoded frame even though none was decoded).
	 * Set to TRUE in imx_vpu_api_dec_decode() and back to FALSE
	 * in imx_vpu_api_dec_get_decoded_frame(). */
	BOOL frame_was_decoded;

	/* Set to TRUE in imx_vpu_api_dec_enable_drain_mode(). */
	BOOL drain_mode_enabled;

	/* Set to TRUE if a decoded buffer's V4L2_BUF_FLAG_LAST flag is set.
	 * Afterwards, the next imx_vpu_api_dec_decode() call will set the
	 * output code to IMX_VPU_API_DEC_OUTPUT_CODE_EOS and exit immediately. */
	BOOL last_decoded_frame_seen;

	/* ION allocator to allocate the DMA buffers for the V4L2 capture buffers. */
	ImxDmaBufferAllocator *ion_dma_buffer_allocator;

	/* How many frames have been detected as having being skipped by the driver.
	 * This is incremented every time the V4L2_EVENT_SKIP event is received,
	 * and decremented once skipped frames are garbage-collected. */
	int num_detected_skipped_frames;

	/* How many frame context items can be in use at any moment. This does _not_
	 * include skipped frames. */
	int used_frame_context_item_count_limit;

	/* If a skipped frame was previously found, the associated context item's
	 * contents are copied into this item. This info will be used by the next
	 * imx_vpu_api_dec_get_skipped_frame_info() call. */
	DecFrameContextItem skipped_frame_context_item;
};


/* Internal function declarations */

/* Enables/disables the output or capture stream (depending)
 * on the type) via VIDIOC_STREAMON / VIDIOC_STREAMOFF. */
static BOOL imx_vpu_api_dec_enable_stream(ImxVpuApiDecoder *decoder, BOOL do_enable, enum v4l2_buf_type type);

static void imx_vpu_api_dec_mark_frame_context_as_available(ImxVpuApiDecoder *decoder, int frame_context_index);
static int imx_vpu_api_dec_add_frame_context(ImxVpuApiDecoder *decoder, void *context, uint64_t pts, uint64_t dts);
static int imx_vpu_api_dec_get_frame_context(ImxVpuApiDecoder *decoder, struct v4l2_buffer *buffer);

static BOOL imx_vpu_api_dec_handle_resolution_change(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code);

static BOOL imx_vpu_api_dec_garbage_collect_oldest_frame(ImxVpuApiDecoder *decoder);


/* Global decoder and compression support info */

static ImxVpuApiCompressionFormat const supported_dec_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_H264,
	IMX_VPU_API_COMPRESSION_FORMAT_H265,
	IMX_VPU_API_COMPRESSION_FORMAT_VP8,

	IMX_VPU_API_COMPRESSION_FORMAT_JPEG,
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG2,
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG4,
	IMX_VPU_API_COMPRESSION_FORMAT_H263,
	IMX_VPU_API_COMPRESSION_FORMAT_WMV3,
	IMX_VPU_API_COMPRESSION_FORMAT_WVC1,
	IMX_VPU_API_COMPRESSION_FORMAT_VP6,
	IMX_VPU_API_COMPRESSION_FORMAT_AVS,
	IMX_VPU_API_COMPRESSION_FORMAT_RV30,
	IMX_VPU_API_COMPRESSION_FORMAT_RV40,
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX3,
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX4,
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX5,

	IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK
};

/* This list reflects the color formats supported by the DPU / by G2D. */
// TODO: 10-bit support
static ImxVpuApiColorFormat const standard_supported_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_UYVY_8BIT,
	IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_YUYV_8BIT,
	IMX_VPU_API_COLOR_FORMAT_RGBA8888,
	IMX_VPU_API_COLOR_FORMAT_BGRA8888,
	IMX_VPU_API_COLOR_FORMAT_RGB565,
	IMX_VPU_API_COLOR_FORMAT_BGR565
};
static int const num_standard_supported_color_formats = (sizeof(standard_supported_color_formats) / sizeof(ImxVpuApiColorFormat));

static ImxVpuApiDecGlobalInfo const global_info = {
	.flags = IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER
	       | IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED,

	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_AMPHION,

	/* V4L2 interface has its own internal stream buffer, so we do not
	 * need one from the user. To signal this, set the min required size
	 * to 0. The alignments are set to 1, but they are irrelevant anyway. */
	.min_required_stream_buffer_size = 0,
	.required_stream_buffer_physaddr_alignment = 1,
	.required_stream_buffer_size_alignment = 1,

	.supported_compression_formats = supported_dec_compression_formats,
	.num_supported_compression_formats = sizeof(supported_dec_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};

ImxVpuApiDecGlobalInfo const * imx_vpu_api_dec_get_global_info(void)
{
	return &global_info;
}

// TODO: Determine the details empirically

static ImxVpuApiCompressionFormatSupportDetails const basic_compression_format_support_details = {
	.min_width = 4, .max_width = INT_MAX,
	.min_height = 4, .max_height = INT_MAX,
	.supported_color_formats = standard_supported_color_formats,
	.num_supported_color_formats = num_standard_supported_color_formats
};

static ImxVpuApiH264SupportDetails const h264_support_details = {
	.parent = {
		.min_width = 4, .max_width = INT_MAX,
		.min_height = 4, .max_height = INT_MAX,
		.supported_color_formats = standard_supported_color_formats,
		.num_supported_color_formats = num_standard_supported_color_formats
	},

	.max_constrained_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
#ifdef IMXVPUAPI_IMX8_SOC_TYPE_MX8MM
	.max_main_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_high_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
#else
	.max_main_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
	.max_high_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
#endif
	.max_high10_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,

	.flags = IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED | IMX_VPU_API_H264_FLAG_ACCESS_UNITS_REQUIRED
};

static ImxVpuApiH265SupportDetails const h265_support_details = {
	.parent = {
		.min_width = 4, .max_width = INT_MAX,
		.min_height = 4, .max_height = INT_MAX,
		.supported_color_formats = standard_supported_color_formats,
		.num_supported_color_formats = num_standard_supported_color_formats
	},

	.max_main_profile_level = IMX_VPU_API_H265_LEVEL_5_1,
	.max_main10_profile_level = IMX_VPU_API_H265_LEVEL_5_1,

	.flags = IMX_VPU_API_H265_FLAG_ACCESS_UNITS_SUPPORTED | IMX_VPU_API_H265_FLAG_ACCESS_UNITS_REQUIRED
};

ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_dec_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&h264_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_H265:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&h265_support_details);

		default:
			return &basic_compression_format_support_details;
	}

	return NULL;
}


/* Function implementations */


ImxVpuApiDecReturnCodes imx_vpu_api_dec_open(ImxVpuApiDecoder **decoder, ImxVpuApiDecOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	ImxVpuApiDecReturnCodes dec_ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	int fd, i, errornum;
	struct v4l2_capability capability;
	struct v4l2_format requested_output_buffer_format;
	struct v4l2_requestbuffers output_buffer_request;
	struct v4l2_event_subscription event_subscription;

	IMX_VPU_API_UNUSED_PARAM(stream_buffer);

	assert(decoder != NULL);
	assert(open_params != NULL);


	init_vpu_device_filenames();


	/* Allocate the decoder structure and set the first states. */

	*decoder = malloc(sizeof(ImxVpuApiDecoder));
	assert((*decoder) != NULL);

	memset(*decoder, 0, sizeof(ImxVpuApiDecoder));

	/* Configure the V4L2 pixel format to request. This is always an
	 * Amphion tiled format. The choice is between 8 and 10 bit output. */
	if (open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_10BIT_DECODING)
		(*decoder)->requested_v4l2_pixelformat = V4L2_PIX_FMT_NV12_10BIT;
	else
		(*decoder)->requested_v4l2_pixelformat = V4L2_PIX_FMT_NV12;

	/* Configure the color format for this decoder's output. */
	{
		BOOL decoded_frame_format_set = FALSE;

		if (!decoded_frame_format_set && (open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SUGGESTED_COLOR_FORMAT))
		{
			for (i = 0; i < num_standard_supported_color_formats; ++i)
			{
				if (open_params->suggested_color_format == standard_supported_color_formats[i])
				{
					(*decoder)->decoded_frame_format = open_params->suggested_color_format;
					decoded_frame_format_set = TRUE;
					IMX_VPU_API_DEBUG(
						"using suggested color format %s as the format for decoded frames",
						imx_vpu_api_color_format_string((*decoder)->decoded_frame_format)
					);
				}
			}
		}

		/* If the suggested format was not picked, use NV12. The
		 * DPU/G2D can't do I420, so there is no point in trying
		 * out IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT. */
		if (!decoded_frame_format_set)
		{
			(*decoder)->decoded_frame_format = IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT;
			IMX_VPU_API_DEBUG(
				"using color format %s as the format for decoded frames",
				imx_vpu_api_color_format_string((*decoder)->decoded_frame_format)
			);
		}
	}

	/* Open the device and query its capabilities. */

	IMX_VPU_API_DEBUG("opening V4L2 device node \"%s\"", vpu_device_filenames.decoder_filename);
	(*decoder)->v4l2_fd = fd = open(vpu_device_filenames.decoder_filename, O_RDWR);
	if (fd < 0)
	{
		IMX_VPU_API_ERROR("could not open V4L2 device: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
	{
		IMX_VPU_API_ERROR("could not query capability: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	IMX_VPU_API_DEBUG("driver:         [%s]", (char const *)(capability.driver));
	IMX_VPU_API_DEBUG("card:           [%s]", (char const *)(capability.card));
	IMX_VPU_API_DEBUG("bus info:       [%s]", (char const *)(capability.bus_info));
	IMX_VPU_API_DEBUG(
		"driver version: %d.%d.%d",
		(int)((capability.version >> 16) & 0xFF),
		(int)((capability.version >> 8) & 0xFF),
		(int)((capability.version >> 0) & 0xFF)
	);

	if ((capability.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) == 0)
	{
		IMX_VPU_API_ERROR("device does not support multi-planar mem2mem decoding");
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	if ((capability.capabilities & V4L2_CAP_STREAMING) == 0)
	{
		IMX_VPU_API_ERROR("device does not support frame streaming");
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}


	/* Create the ION allocator for the capture buffer DMA buffers. */
	(*decoder)->ion_dma_buffer_allocator = imx_dma_buffer_ion_allocator_new(
		IMX_DMA_BUFFER_ION_ALLOCATOR_DEFAULT_ION_FD,
		IMX_DMA_BUFFER_ION_ALLOCATOR_DEFAULT_HEAP_ID_MASK,
		IMX_DMA_BUFFER_ION_ALLOCATOR_DEFAULT_HEAP_FLAGS,
		&errornum
	);
	if ((*decoder)->ion_dma_buffer_allocator == NULL)
	{
		IMX_VPU_API_ERROR("could not create ION allocator: %s (%d)", strerror(errornum), errornum);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}


	/* Set the encoded data format in the OUTPUT queue. */

	memset(&requested_output_buffer_format, 0, sizeof(struct v4l2_format));
	requested_output_buffer_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	requested_output_buffer_format.fmt.pix_mp.width = open_params->frame_width;
	requested_output_buffer_format.fmt.pix_mp.height = open_params->frame_height;
	requested_output_buffer_format.fmt.pix_mp.pixelformat = convert_to_v4l2_fourcc(open_params->compression_format);
	requested_output_buffer_format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
	requested_output_buffer_format.fmt.pix_mp.num_planes = 1;
	requested_output_buffer_format.fmt.pix_mp.plane_fmt[0].sizeimage = DEC_REQUESTED_OUTPUT_BUFFER_SIZE;
	requested_output_buffer_format.fmt.pix_mp.plane_fmt[0].bytesperline = 0; /* This is set to 0 for encoded data. */

	/* Special case for MVC (= "3D video"). Only h.264 supports this. */
	if ((open_params->compression_format == IMX_VPU_API_COMPRESSION_FORMAT_H264)
	 && (open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MVC))
	{
		IMX_VPU_API_DEBUG("enabling h.264 MVC support");
		requested_output_buffer_format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_MVC;
	}

	if (ioctl(fd, VIDIOC_S_FMT, &requested_output_buffer_format) < 0)
	{
		IMX_VPU_API_ERROR("could not set V4L2 output buffer video format (= encoded data format): %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
		goto error;
	}

	IMX_VPU_API_INFO(
		"set up V4L2 output buffer video format (= encoded data format): %s (V4L2 fourCC: %" FOURCC_FORMAT ")",
		imx_vpu_api_compression_format_string(open_params->compression_format),
		V4L2_FOURCC_ARGS(requested_output_buffer_format.fmt.pix_mp.pixelformat)
	);

	/* The driver may adjust the size of the output buffers. Retrieve
	 * the sizeimage value (which contains what the driver picked). */
	(*decoder)->output_buffer_size = requested_output_buffer_format.fmt.pix_mp.plane_fmt[0].sizeimage;
	IMX_VPU_API_DEBUG(
		"V4L2 output buffer size in bytes:  requested: %d  actual: %d",
		DEC_REQUESTED_OUTPUT_BUFFER_SIZE,
		(*decoder)->output_buffer_size
	);

	/* Finished setting the format. Make a copy for later use. */
	memcpy(&((*decoder)->output_buffer_format), &requested_output_buffer_format, sizeof(struct v4l2_format));


	/* Allocate the output buffers. */

	IMX_VPU_API_DEBUG("requesting output buffers");

	memset(&output_buffer_request, 0, sizeof(output_buffer_request));
	output_buffer_request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	output_buffer_request.memory = V4L2_MEMORY_MMAP;
	output_buffer_request.count = DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS;

	if (ioctl(fd, VIDIOC_REQBUFS, &output_buffer_request) < 0)
	{
		IMX_VPU_API_ERROR("could not request output buffers: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	/* VIDIOC_REQBUFS stores the number of actually requested buffers in the "count" field. */
	(*decoder)->num_output_buffers = output_buffer_request.count;
	IMX_VPU_API_DEBUG(
		"num V4L2 output buffers:  requested: %d  actual: %d",
		DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS,
		(*decoder)->num_output_buffers
	);

	assert((*decoder)->num_output_buffers > 0);

	(*decoder)->output_buffer_items = calloc((*decoder)->num_output_buffers, sizeof(DecV4L2OutputBufferItem));
	assert((*decoder)->output_buffer_items != NULL);

	/* After requesting the buffers we need to query them to get
	 * the necessary information for later access via mmap().
	 * In here, we also associate each DecV4L2OutputBufferItem's
	 * v4l2_plane with the accompanying v4l2_buffer. */
	for (i = 0; i < (*decoder)->num_output_buffers; ++i)
	{
		DecV4L2OutputBufferItem *output_buffer_item = &((*decoder)->output_buffer_items[i]);

		output_buffer_item->buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		output_buffer_item->buffer.memory = V4L2_MEMORY_MMAP;
		output_buffer_item->buffer.index = i;
		output_buffer_item->buffer.m.planes = &(output_buffer_item->plane);
		output_buffer_item->buffer.length = 1;

		if (ioctl(fd, VIDIOC_QUERYBUF, &(output_buffer_item->buffer)) < 0)
		{
			IMX_VPU_API_ERROR("could not query output buffer #%d: %s (%d)", i, strerror(errno), errno);
			dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			goto error;
		}

		IMX_VPU_API_DEBUG(
			"  output buffer #%d:  flags: %08x  length: %u  mem offset: %u",
			(unsigned int)(output_buffer_item->buffer.flags),
			(unsigned int)(output_buffer_item->buffer.m.planes[0].length),
			(unsigned int)(output_buffer_item->buffer.m.planes[0].m.mem_offset)
		);
	}


	/* Subscribe to the V4L2_EVENT_SOURCE_CHANGE event to get notified
	 * when (1) the initial resolution information becomes available
	 * and (2) when during the stream a new resolution is found. */

	IMX_VPU_API_DEBUG("subscribing to source change event");

	memset(&event_subscription, 0, sizeof(event_subscription));
	event_subscription.type = V4L2_EVENT_SOURCE_CHANGE;

	if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &event_subscription) < 0) {
		IMX_VPU_API_ERROR("could not subscribe to source change event: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	/* Subscribe to the EOS event. The Amphion Malone V4L2 driver
	 * does not seem to support the V4L2_BUF_FLAG_LAST flag, so
	 * we need this event to detect the EOS. */

	IMX_VPU_API_DEBUG("subscribing to EOS event");

	memset(&event_subscription, 0, sizeof(event_subscription));
	event_subscription.type = V4L2_EVENT_EOS;

	if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &event_subscription) < 0) {
		IMX_VPU_API_ERROR("could not subscribe to EOS event: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	/* Subscribe to the custom Malone skip event. This is used
	 * to keep track of how many frames have been skipped and
	 * haven't been garbage-collected yet. */

	IMX_VPU_API_DEBUG("subscribing to skip event");

	memset(&event_subscription, 0, sizeof(event_subscription));
	event_subscription.type = V4L2_EVENT_SKIP;

	if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &event_subscription) < 0) {
		IMX_VPU_API_ERROR("could not subscribe to skip event: %s (%d)", strerror(errno), errno);
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}


	/* Set up G2D. */

	(*decoder)->g2d_handle = NULL;
	if (g2d_open(&((*decoder)->g2d_handle)) != 0)
	{
		IMX_VPU_API_ERROR("opening G2D device failed");
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	if (g2d_make_current((*decoder)->g2d_handle, G2D_HARDWARE_2D) != 0)
	{
		IMX_VPU_API_ERROR("g2d_make_current() failed");
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto error;
	}


	IMX_VPU_API_INFO("decoder opened successfully");


finish:
	return dec_ret;

error:
	/* This rolls back any partial initializations. */
	imx_vpu_api_dec_close(*decoder);

	free(*decoder);
	*decoder = NULL;

	goto finish;
}


void imx_vpu_api_dec_close(ImxVpuApiDecoder *decoder)
{
	if (decoder == NULL)
		return;

	if (decoder->v4l2_fd > 0)
	{
		int i;
		struct v4l2_requestbuffers frame_buffer_request;

		/* Disable any ongoing streams. */
		imx_vpu_api_dec_enable_stream(decoder, FALSE, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		imx_vpu_api_dec_enable_stream(decoder, FALSE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

		/* Deallocate previously requested frame OUTPUT and CAPTURE buffers. */

		IMX_VPU_API_DEBUG("freeing V4L2 output buffers");

		memset(&frame_buffer_request, 0, sizeof(frame_buffer_request));
		frame_buffer_request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		frame_buffer_request.memory = V4L2_MEMORY_MMAP;
		frame_buffer_request.count = 0;

		if (ioctl(decoder->v4l2_fd, VIDIOC_REQBUFS, &frame_buffer_request) < 0)
			IMX_VPU_API_ERROR("could not free V4L2 output buffers: %s (%d)", strerror(errno), errno);

		IMX_VPU_API_DEBUG("freeing V4L2 capture buffers");

		for (i = 0; i < decoder->num_capture_buffers; ++i)
		{
			DecV4L2CaptureBufferItem *capture_buffer_item = &(decoder->capture_buffer_items[i]);
			imx_dma_buffer_deallocate(capture_buffer_item->dma_buffer);
		}

		memset(&frame_buffer_request, 0, sizeof(frame_buffer_request));
		frame_buffer_request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		frame_buffer_request.memory = V4L2_MEMORY_DMABUF;
		frame_buffer_request.count = 0;

		if (ioctl(decoder->v4l2_fd, VIDIOC_REQBUFS, &frame_buffer_request) < 0)
			IMX_VPU_API_ERROR("could not free V4L2 capture buffers: %s (%d)", strerror(errno), errno);

		close(decoder->v4l2_fd);
	}

	if (decoder->ion_dma_buffer_allocator != NULL)
		imx_dma_buffer_allocator_destroy(decoder->ion_dma_buffer_allocator);

	if (decoder->g2d_handle != NULL)
		g2d_close(decoder->g2d_handle);

	free(decoder->output_buffer_items);
	free(decoder->capture_buffer_items);
	free(decoder->frame_context_items);
	free(decoder->available_frame_context_item_indices);

	free(decoder);

	IMX_VPU_API_INFO("decoder closed");
}


ImxVpuApiDecStreamInfo const * imx_vpu_api_dec_get_stream_info(ImxVpuApiDecoder *decoder)
{
	return &(decoder->stream_info);
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_add_framebuffers_to_pool(ImxVpuApiDecoder *decoder, ImxDmaBuffer **fb_dma_buffers, void **fb_contexts, size_t num_framebuffers)
{
	IMX_VPU_API_UNUSED_PARAM(decoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffers);
	IMX_VPU_API_UNUSED_PARAM(fb_contexts);
	IMX_VPU_API_UNUSED_PARAM(num_framebuffers);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


void imx_vpu_api_dec_enable_drain_mode(ImxVpuApiDecoder *decoder)
{
	struct v4l2_decoder_cmd command;

	assert(decoder != NULL);

	if (decoder->drain_mode_enabled || !decoder->output_stream_enabled || !decoder->capture_stream_enabled)
		return;

	IMX_VPU_API_DEBUG("starting decoder drain");

	command.cmd = V4L2_DEC_CMD_STOP;
	command.flags = 0;
	command.stop.pts = 0;

	if (ioctl(decoder->v4l2_fd, VIDIOC_DECODER_CMD, &command) < 0)
		IMX_VPU_API_ERROR("could not initiate drain mode: %s (%d)", strerror(errno), errno);

	decoder->drain_mode_enabled = TRUE;
}


int imx_vpu_api_dec_is_drain_mode_enabled(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	return decoder->drain_mode_enabled;
}


void imx_vpu_api_dec_flush(ImxVpuApiDecoder *decoder)
{
	int i;
	BOOL capture_stream_enabled = decoder->capture_stream_enabled;

	assert(decoder != NULL);

	IMX_VPU_API_DEBUG("beginning decoder flush");

	/* Turn off the stream (if they are turned on) to
	 * flush both output and capture buffer queues. */
	imx_vpu_api_dec_enable_stream(decoder, FALSE, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	imx_vpu_api_dec_enable_stream(decoder, FALSE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	/* There are no output buffers queued anymore. */
	decoder->num_output_buffers_in_queue = 0;

	/* Any frames that were skipped and not yet
	 * garbage-collected are now also gone. */
	decoder->num_detected_skipped_frames = 0;

	/* After flushing, all frame context items are available.
	 * Mark them accordingly. */
	for (i = 0; i < decoder->num_frame_context_items; ++i)
	{
		decoder->frame_context_items[i].in_use = FALSE;
		decoder->available_frame_context_item_indices[i] = i;
	}
	decoder->num_available_frame_context_items = decoder->num_frame_context_items;
	IMX_VPU_API_DEBUG("marked all frame context items as available");

	/* Any previously decoded frame is no longer available. */
	decoder->frame_was_decoded = FALSE;

	/* In drain mode, the decoder no longer accepts encoded data.
	 * It just decodes pending frames. Since flushing gets rid of
	 * pending frames, we are effectively at the EOS if we flush
	 * in this mode. */
	if (decoder->drain_mode_enabled)
	{
		IMX_VPU_API_DEBUG("flushing in drain mode; setting flag to let next decode() call return EOS");
		decoder->last_decoded_frame_seen = TRUE;
	}

	/* Set up the capture stream again if it was running prior to the flush. */

	if (capture_stream_enabled)
	{
		for (i = 0; i < decoder->num_capture_buffers; ++i)
		{
			int dmabuf_fd;
			struct v4l2_buffer buffer;
			struct v4l2_plane planes[DEC_NUM_CAPTURE_BUFFER_PLANES];
			DecV4L2CaptureBufferItem *capture_buffer_item = &(decoder->capture_buffer_items[i]);

			dmabuf_fd = imx_dma_buffer_get_fd(capture_buffer_item->dma_buffer);

			IMX_VPU_API_DEBUG("re-queuing V4L2 capture buffer #%d with DMA-BUF FD %d", i, dmabuf_fd);

			/* We copy the v4l2_buffer instance in case the driver
			 * modifies its fields. (This preserves the original.) */
			memcpy(&buffer, &(capture_buffer_item->buffer), sizeof(buffer));
			memcpy(planes, capture_buffer_item->planes, sizeof(struct v4l2_plane) * DEC_NUM_CAPTURE_BUFFER_PLANES);
			/* Make sure "planes" points to the _copy_ of the planes structures. */
			buffer.m.planes = planes;

			if (ioctl(decoder->v4l2_fd, VIDIOC_QBUF, &buffer) < 0)
				IMX_VPU_API_ERROR("could not queue capture buffer: %s (%d)", strerror(errno), errno);
		}

		imx_vpu_api_dec_enable_stream(decoder, TRUE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	}

	IMX_VPU_API_DEBUG("decoder flush finished");
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_push_encoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	ImxVpuApiDecReturnCodes dec_ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	struct v4l2_plane plane;
	struct v4l2_buffer buffer;
	int available_space_for_encoded_data;
	int frame_context_index;
	void *mapped_buffer_data = NULL;

	assert(decoder != NULL);
	assert(encoded_frame != NULL);


	/* Get an unused output buffer for our new encoded data. */

	/* Depending on how many output buffers we already queued,
	 * we might have to dequeue one first. In the beginning,
	 * the queue is not yet full, so we just keep queuing. */
	if (decoder->num_output_buffers_in_queue < DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS)
	{
		int output_buffer_index = decoder->num_output_buffers_in_queue;
		DecV4L2OutputBufferItem *output_buffer_item = &(decoder->output_buffer_items[output_buffer_index]);
		decoder->num_output_buffers_in_queue++;

		/* We copy the v4l2_buffer instance in case the driver
		 * modifies its fields. (This preserves the original.) */
		memcpy(&buffer, &(output_buffer_item->buffer), sizeof(buffer));
		memcpy(&plane, &(output_buffer_item->plane), sizeof(plane));
		buffer.m.planes = &plane;
		buffer.length = 1;

		IMX_VPU_API_LOG(
			"V4L2 output queue has room for %d more buffer(s); using buffer with buffer index %d to fill it with new encoded data and enqueue it",
			DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS - decoder->num_output_buffers_in_queue,
			output_buffer_index
		);
	}
	else
	{
		memset(&buffer, 0, sizeof(buffer));
		buffer.m.planes = &plane;
		buffer.length = 1;
		buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buffer.memory = V4L2_MEMORY_MMAP;

		if (ioctl(decoder->v4l2_fd, VIDIOC_DQBUF, &buffer) < 0)
		{
			IMX_VPU_API_ERROR("could not dequeue V4L2 output buffer: %s (%d)", strerror(errno), errno);
			dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			goto error;
		}

		IMX_VPU_API_LOG(
			"V4L2 output queue is full; dequeued output buffer with buffer index %d to fill it with new encoded data and then re-enqueue it",
			(int)(buffer.index)
		);
	}

	available_space_for_encoded_data = buffer.m.planes[0].length;

	/* Sanity check. This should never happen. */
	if ((int)(encoded_frame->data_size) > available_space_for_encoded_data)
	{
		IMX_VPU_API_ERROR(
			"encoded frame size %zu exceeds available space for encoded data %d",
			encoded_frame->data_size,
			available_space_for_encoded_data
		);
		goto error;
	}


	/* Store the frame context in the frame context array, and
	 * keep track of the index where the context is stored.
	 * We'll store that index in the frames that get queued. */

	frame_context_index = imx_vpu_api_dec_add_frame_context(decoder, encoded_frame->context, encoded_frame->pts, encoded_frame->dts);
	if (frame_context_index < 0)
		goto error;


	/* Fill some metadata into the V4L2 buffer, specifically
	 * the frame context index and the size of the encoded data. */

	buffer.m.planes[0].bytesused = encoded_frame->data_size;
	/* Look up the code in imx_vpu_api_dec_get_frame_context()
	 * to see why the PTS is stored in the timestamp field. */
	buffer.timestamp.tv_sec = encoded_frame->pts / 1000000000;
	buffer.timestamp.tv_usec = (encoded_frame->pts - ((uint64_t)(buffer.timestamp.tv_sec)) * 1000000000) / 1000;


	/* Copy the encoded data into the output buffer. */

	mapped_buffer_data = mmap(
		NULL,
		available_space_for_encoded_data,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		decoder->v4l2_fd,
		buffer.m.planes[0].m.mem_offset
	);
	if (mapped_buffer_data == MAP_FAILED)
	{
		IMX_VPU_API_ERROR("could not map V4L2 output buffer: %s (%d)", strerror(errno), errno);
		goto error;
	}
	memcpy(mapped_buffer_data, encoded_frame->data, encoded_frame->data_size);
	munmap(mapped_buffer_data, available_space_for_encoded_data);


	/* Finally, queue the buffer. */
	if (ioctl(decoder->v4l2_fd, VIDIOC_QBUF, &buffer) < 0)
	{
		IMX_VPU_API_ERROR("could not queue output buffer: %s (%d)", strerror(errno), errno);
		goto error;
	}


	IMX_VPU_API_LOG(
		"queued V4L2 output buffer with a payload of %zu byte(s) buffer index %d and frame context index %d (context pointer %p PTS %" PRIu64 " DTS %" PRIu64 ")",
		encoded_frame->data_size,
		(int)(buffer.index),
		frame_context_index,
		encoded_frame->context,
		encoded_frame->pts,
		encoded_frame->dts
	);


	if (decoder->num_output_buffers_in_queue == DEC_MIN_NUM_REQUIRED_OUTPUT_BUFFERS)
	{
		/* If there are enough queued encoded frames,
		 * enable the OUTPUT stream if not already eanbled. */

		if (!imx_vpu_api_dec_enable_stream(decoder, TRUE, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE))
			goto error;
	}


finish:
	return dec_ret;

error:
	if (dec_ret == IMX_VPU_API_DEC_RETURN_CODE_OK)
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;

	goto finish;
}


void imx_vpu_api_dec_set_output_frame_dma_buffer(ImxVpuApiDecoder *decoder, ImxDmaBuffer *output_frame_dma_buffer, void *fb_context)
{
	assert(decoder != NULL);
	assert(output_frame_dma_buffer != NULL);

	decoder->output_frame_dma_buffer = output_frame_dma_buffer;
	decoder->output_frame_fb_context = fb_context;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_decode(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code)
{
	ImxVpuApiDecReturnCodes dec_ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	struct pollfd pfd;
	int num_used_frame_context_items;

	assert(decoder != NULL);
	assert(output_code != NULL);

	*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE;


	/* Handle EOS case. */
	if (decoder->last_decoded_frame_seen)
	{
		/* Handle any remaining skipped frames at the time we
		 * reach EOS before actually announcing the EOS. */
		if (decoder->num_detected_skipped_frames > 0)
		{
			if (imx_vpu_api_dec_garbage_collect_oldest_frame(decoder))
			{
				decoder->num_detected_skipped_frames--;
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;
				return IMX_VPU_API_DEC_RETURN_CODE_OK;
			}
			else
			{
				/* Should not be reached. Getting here would imply that
				 * there are no frame context items currently in use. */
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}
		}

		IMX_VPU_API_INFO("end of stream reached");
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
		return IMX_VPU_API_DEC_RETURN_CODE_OK;
	}


	/* Output stream must be enabled before any decoding can happen.
	 * This is not an error; it happens at the beginning of a stream. */
	if (!decoder->output_stream_enabled)
	{
		IMX_VPU_API_LOG("output stream not enabled yet; cannot decode anything yet, more encoded data needed");
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
		return IMX_VPU_API_DEC_RETURN_CODE_OK;
	}


	/* This happens when this function is called after a previous call
	 * returned the IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE
	 * output code and the user did not retrieve the decoded frame with
	 * imx_vpu_api_dec_get_decoded_frame(). */
	if (decoder->frame_was_decoded)
	{
		IMX_VPU_API_ERROR("attempted to decode frame before the previously decoded frame was retrieved");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}


	num_used_frame_context_items = decoder->num_frame_context_items - decoder->num_available_frame_context_items;


	if (decoder->stream_info_announced)
	{
		/* This happens when the user tries to decode a frame after
		 * the stream info was announced and the user did not call
		 * imx_vpu_api_dec_set_output_frame_dma_buffer(). */
		if (decoder->output_frame_dma_buffer == NULL)
		{
			IMX_VPU_API_ERROR("no output frame buffer set");
			return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
		}

		/* Perform garbage collection of skipped frames after the stream
		 * info was announced. See the decoder description above for an
		 * explanation of this logic. */
		if (num_used_frame_context_items >= decoder->used_frame_context_item_count_limit)
		{
			if (decoder->num_detected_skipped_frames > 0)
			{
				IMX_VPU_API_DEBUG("used frame context item count limit reached, and number of detected skipped frames is %d; garbage-collecting oldest frame", decoder->num_detected_skipped_frames);

				if (imx_vpu_api_dec_garbage_collect_oldest_frame(decoder))
				{
					decoder->num_detected_skipped_frames--;
					*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;
					return IMX_VPU_API_DEC_RETURN_CODE_OK;
				}
				else
				{
					/* Should not be reached. Getting here would imply that
					 * there are no frame context items currently in use. */
					return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
				}
			}
		}
	}


	/* We add POLLIN and POLLPRI always, since these do not need to be throttled.
	 * POLLOUT needs to be throttled, so it is added only in these cases:
	 *
	 * - If the stream info has not been announced, POLLOUT is always added.
	 *   This is necessary to make sure the decoder is fed enough encoded data
	 *   to be able to produce stream information (communicated through th
	 *   V4L2_EVENT_SOURCE_CHANGE event below).
	 * - If the number of used frame context items is below the limit.
	 *
	 * The latter case makes sure that the number of encoded frames that have
	 * been fed into the encoder and haven't been decoded yet is never too big.
	 * This functions as a throttling mechanism. Also see the decoder description
	 * at the beginning of this file.
	 *
	 * Finally, if drain mode is active, POLLOUT is never added, because in that
	 * mode, we aren't going to write data into the output queue anyway - we just
	 * want the decoder to decode the pending frames.
	 */

	pfd.fd = decoder->v4l2_fd;
	pfd.events = POLLIN | POLLPRI;

	if (!decoder->drain_mode_enabled)
	{
		if (!decoder->stream_info_announced)
		{
			IMX_VPU_API_LOG("stream info has not yet been announced; enabling POLLOUT event");
			pfd.events |= POLLOUT;
		}
		else if (num_used_frame_context_items < decoder->used_frame_context_item_count_limit)
		{
			IMX_VPU_API_LOG("there is room for more encoded frames to be pushed into the V4L2 output queue; enabling POLLOUT event");
			pfd.events |= POLLOUT;
		}
		else
		{
			IMX_VPU_API_LOG("there is no room for more encoded frames to be pushed into the V4L2 output queue; not enabling POLLOUT event");
		}
	}
	else
	{
		IMX_VPU_API_LOG("drain mode is active; not enabling POLLOUT event");
	}

	/* The actual poll() call. This is done in a loop in case poll() gets
	 * interrupted by a signal, in which case we can safely try again. */
	while (TRUE)
	{
		int poll_ret = poll(&pfd, 1, -1);
		if (poll_ret < 0)
		{
			switch (errno)
			{
				case EINTR:
					IMX_VPU_API_LOG("poll() was interrupted by signal; retrying call");
					break;

				default:
					IMX_VPU_API_ERROR("poll() failed: %s (%d)", strerror(errno), errno);
					goto error;
			}
		}
		else
			break;
	}


	/* Handle the returned poll() events. */

	/* POLLPRI = a V4L2 event has arrived. */
	if (pfd.revents & POLLPRI)
	{
		struct v4l2_event event;

		if (ioctl(decoder->v4l2_fd, VIDIOC_DQEVENT, &event) < 0)
		{
			IMX_VPU_API_ERROR("could not dequeue event: %s (%d)", strerror(errno), errno);
			goto error;
		}

		switch (event.type)
		{
			case V4L2_EVENT_SOURCE_CHANGE:
			{
				if (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)
				{
					IMX_VPU_API_DEBUG("source change event with a resolution change detected");

					if (!imx_vpu_api_dec_handle_resolution_change(decoder, output_code))
						goto error;
				}
				else
					IMX_VPU_API_DEBUG("ignoring source change event that does not contain a resolution change bit");

				break;
			}

			case V4L2_EVENT_EOS:
				IMX_VPU_API_DEBUG("EOS event detected");
				/* Set this flag so the next imx_vpu_api_dec_decode()
				 * call immediately returns EOS. We do not set the
				 * IMX_VPU_API_DEC_OUTPUT_CODE_EOS right here, since
				 * a last frame may have been decoded, and that
				 * output code would effectively mask that frame
				 * (since the caller is informed about the decoded
				 * frame by setting the output code to
				 * IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE). */
				decoder->last_decoded_frame_seen = TRUE;
				break;

			case V4L2_EVENT_SKIP:
				decoder->num_detected_skipped_frames++;
				IMX_VPU_API_DEBUG("skip event detected; new number of detected skipped frames: %d", decoder->num_detected_skipped_frames);
				break;

			default:
				IMX_VPU_API_DEBUG("ignoring event of type %" PRIu32, (uint32_t)(event.type));
				break;
		}

		goto finish;
	}

	/* POLLIN = decoded (raw) frame available at the CAPTURE queue. */
	if (pfd.revents & POLLIN)
	{
		struct v4l2_buffer buffer;
		struct v4l2_plane planes[DEC_NUM_CAPTURE_BUFFER_PLANES];
		int dequeued_capture_buffer_index;
		int frame_context_index;
		DecV4L2CaptureBufferItem *capture_buffer_item;

		IMX_VPU_API_LOG("decoded frame is available");
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE;

		/* The code above already checks for this and returns an error
		 * code if it isn't set. So, if at this point this is NULL, then
		 * something internal is wrong. */
		assert(decoder->output_frame_dma_buffer != NULL);

		/* Dequeue the decoded frame. */

		memset(&buffer, 0, sizeof(buffer));
		memset(planes, 0, sizeof(planes));

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buffer.memory = V4L2_MEMORY_DMABUF;
		buffer.m.planes = planes;
		buffer.length = DEC_NUM_CAPTURE_BUFFER_PLANES;

		/* Dequeue the decoded frame from the CAPTURE queue. */
		if (ioctl(decoder->v4l2_fd, VIDIOC_DQBUF, &buffer) < 0)
		{
			IMX_VPU_API_ERROR("could not dequeue decoded frame buffer: %s (%d)", strerror(errno), errno);
			goto error;
		}

		/* Get information about the dequeued buffer. */

		dequeued_capture_buffer_index = buffer.index;
		assert(dequeued_capture_buffer_index < decoder->num_capture_buffers);

		capture_buffer_item = &(decoder->capture_buffer_items[dequeued_capture_buffer_index]);

		frame_context_index = imx_vpu_api_dec_get_frame_context(decoder, &buffer);
		assert(frame_context_index >= 0);
		assert(frame_context_index < (int)(decoder->num_frame_context_items));
		decoder->decoded_frame_context_index = frame_context_index;
		decoder->decoded_frame_context_item = &(decoder->frame_context_items[frame_context_index]);

		/* Do the detiling with G2D, which also implicitly copies the
		 * frame from the capture buffer to the output_frame_dma_buffer. */
		{
			ImxVpuApiFramebufferMetrics *fb_metrics = &(decoder->stream_info.decoded_frame_framebuffer_metrics);
			ImxDmaBuffer *capture_dma_buffer = capture_buffer_item->dma_buffer;
			imx_physical_address_t src_physical_address = imx_dma_buffer_get_physical_address(capture_dma_buffer);
			imx_physical_address_t dest_physical_address = imx_dma_buffer_get_physical_address(decoder->output_frame_dma_buffer);

			decoder->source_g2d_surface.base.planes[0] = src_physical_address + decoder->capture_buffer_y_offset;
			decoder->source_g2d_surface.base.planes[1] = src_physical_address + decoder->capture_buffer_uv_offset;

			decoder->dest_g2d_surface.base.planes[0] = dest_physical_address + fb_metrics->y_offset;
			decoder->dest_g2d_surface.base.planes[1] = dest_physical_address + fb_metrics->u_offset;

			if (g2d_blitEx(decoder->g2d_handle, &(decoder->source_g2d_surface), &(decoder->dest_g2d_surface)) != 0)
			{
				IMX_VPU_API_ERROR("could not detile frame by using the G2D blitter");
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}
		}

		IMX_VPU_API_LOG(
			"got decoded frame:"
			"  capture buffer index %d  frame context index %d"
			"  V4L2 buffer flags %08x bytesused %u"
			"  context pointer %p PTS %" PRIu64 " DTS %" PRIu64,
			dequeued_capture_buffer_index,
			frame_context_index,
			(unsigned int)(buffer.flags),
			(unsigned int)(buffer.bytesused),
			decoder->decoded_frame_context_item->context,
			decoder->decoded_frame_context_item->pts,
			decoder->decoded_frame_context_item->dts
		);

		if (buffer.flags & V4L2_BUF_FLAG_LAST)
		{
			IMX_VPU_API_DEBUG("this decoded frame is the last frame in the stream");
			decoder->last_decoded_frame_seen = TRUE;
		}

		/* Finally, return the V4L2 capture buffer back to the capture queue. */

		/* We copy the v4l2_buffer instance in case the driver
		 * modifies its fields. (This preserves the original.) */
		memcpy(&buffer, &(capture_buffer_item->buffer), sizeof(buffer));
		memcpy(planes, capture_buffer_item->planes, sizeof(struct v4l2_plane) * DEC_NUM_CAPTURE_BUFFER_PLANES);
		/* Make sure "planes" points to the _copy_ of the planes structures. */
		buffer.m.planes = planes;

		if (ioctl(decoder->v4l2_fd, VIDIOC_QBUF, &buffer) < 0)
		{
			IMX_VPU_API_ERROR("could not queue capture buffer: %s (%d)", strerror(errno), errno);
			goto error;
		}

		decoder->frame_was_decoded = TRUE;

		goto finish;
	}

	/* POLLOUT = we can or need to write another encoded frame to the OUTPUT queue. */
	if (pfd.revents & POLLOUT)
	{
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
		IMX_VPU_API_LOG("driver can now accept more encoded data");
		goto finish;
	}


finish:
	return dec_ret;

error:
	if (dec_ret == IMX_VPU_API_DEC_RETURN_CODE_OK)
		dec_ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;

	goto finish;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_get_decoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiRawFrame *decoded_frame)
{
	assert(decoder != NULL);

	/* Sanity checks. */

	if (!decoder->frame_was_decoded)
	{
		IMX_VPU_API_ERROR("attempted to get decoded frame even though no frame has been decoded yet");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	/* Output the result. */
	decoded_frame->fb_dma_buffer = decoder->output_frame_dma_buffer;
	decoded_frame->fb_context = decoder->output_frame_fb_context;
	decoded_frame->context = decoder->decoded_frame_context_item->context;
	decoded_frame->pts = decoder->decoded_frame_context_item->pts;
	decoded_frame->dts = decoder->decoded_frame_context_item->dts;

	imx_vpu_api_dec_mark_frame_context_as_available(decoder, decoder->decoded_frame_context_index);

	/* Clear this flag since we are now done with this decoded frame. */
	decoder->frame_was_decoded = FALSE;

	return IMX_VPU_API_DEC_RETURN_CODE_OK;
}


void imx_vpu_api_dec_return_framebuffer_to_decoder(ImxVpuApiDecoder *decoder, ImxDmaBuffer *fb_dma_buffer)
{
	IMX_VPU_API_UNUSED_PARAM(decoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffer);
}


void imx_vpu_api_dec_get_skipped_frame_info(ImxVpuApiDecoder *decoder, ImxVpuApiDecSkippedFrameReasons *reason, void **context, uint64_t *pts, uint64_t *dts)
{
	assert(decoder != NULL);

	if (reason != NULL)
		*reason = IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME; /* TODO: signal corrupted frames */
	if (context != NULL)
		*context = decoder->skipped_frame_context_item.context;
	if (pts != NULL)
		*pts = decoder->skipped_frame_context_item.pts;
	if (dts != NULL)
		*dts = decoder->skipped_frame_context_item.dts;
}


static BOOL imx_vpu_api_dec_enable_stream(ImxVpuApiDecoder *decoder, BOOL do_enable, enum v4l2_buf_type type)
{
	BOOL *stream_enabled;
	char const *stream_name;

	switch (type)
	{
		case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
			stream_enabled = &(decoder->output_stream_enabled);
			stream_name = "output (= encoded data)";
			break;

		case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
			stream_enabled = &(decoder->capture_stream_enabled);
			stream_name = "capture (= decoded data)";
			break;

		default:
			assert(FALSE);
	}

	if (*stream_enabled == do_enable)
		return TRUE;

	IMX_VPU_API_DEBUG("%s %s stream", (do_enable ? "enabling" : "disabling"), stream_name);

	if (ioctl(decoder->v4l2_fd, do_enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type) < 0)
	{
		IMX_VPU_API_ERROR("could not %s %s stream: %s (%d)", (do_enable ? "enable" : "disable"), stream_name, strerror(errno), errno);
		return FALSE;
	}
	else
	{
		IMX_VPU_API_DEBUG("%s stream %s", stream_name, (do_enable ? "enabled" : "disabled"));
		*stream_enabled = do_enable;
		return TRUE;
	}
}


static void imx_vpu_api_dec_mark_frame_context_as_available(ImxVpuApiDecoder *decoder, int frame_context_index)
{
	assert(frame_context_index >= 0);
	assert(frame_context_index < decoder->num_frame_context_items);
	decoder->num_available_frame_context_items++;
	decoder->available_frame_context_item_indices[decoder->num_available_frame_context_items - 1] = frame_context_index;

	decoder->frame_context_items[frame_context_index].in_use = FALSE;

	IMX_VPU_API_LOG(
		"marked frame context item as available:  index: %d  num available / total frame context items: %d / %d",
		frame_context_index,
		decoder->num_available_frame_context_items,
		decoder->num_frame_context_items
	);
}


static int imx_vpu_api_dec_add_frame_context(ImxVpuApiDecoder *decoder, void *context, uint64_t pts, uint64_t dts)
{
	DecFrameContextItem *frame_context_item;
	int index, i;

	if (decoder->num_available_frame_context_items == 0)
	{
		int const item_count_increment = 10;
		size_t new_num_frame_context_items;
		DecFrameContextItem *new_frame_context_items;
		int *new_available_frame_context_indices;

		new_num_frame_context_items = decoder->num_frame_context_items + item_count_increment;
		new_frame_context_items = realloc(decoder->frame_context_items, new_num_frame_context_items * sizeof(DecFrameContextItem));
		if (new_frame_context_items == NULL)
		{
			IMX_VPU_API_ERROR("could not allocate space for frame context");
			return -1;
		}
		new_available_frame_context_indices = realloc(decoder->available_frame_context_item_indices, new_num_frame_context_items * sizeof(int));
		if (new_available_frame_context_indices == NULL)
		{
			IMX_VPU_API_ERROR("could not allocate space for available frame context indices");
			return -1;
		}

		memset(&(new_frame_context_items[decoder->num_frame_context_items]), 0, item_count_increment * sizeof(DecFrameContextItem));

		for (i = 0; i < item_count_increment; ++i)
		{
			new_available_frame_context_indices[decoder->num_available_frame_context_items + i] = decoder->num_frame_context_items + i;
		}

		decoder->frame_context_items = new_frame_context_items;
		decoder->num_frame_context_items = new_num_frame_context_items;

		decoder->available_frame_context_item_indices = new_available_frame_context_indices;
		decoder->num_available_frame_context_items += item_count_increment;

		IMX_VPU_API_LOG(
			"all frame context items are in use, or none exist yet; allocated %d more items (total amount now %d)",
			item_count_increment,
			decoder->num_frame_context_items
		);
	}

	index = decoder->available_frame_context_item_indices[decoder->num_available_frame_context_items - 1];
	frame_context_item = &(decoder->frame_context_items[index]);
	decoder->num_available_frame_context_items--;

	frame_context_item->context = context;
	frame_context_item->pts_microseconds = pts / 1000;
	frame_context_item->pts = pts;
	frame_context_item->dts = dts;
	frame_context_item->in_use = TRUE;

	return index;
}


static int imx_vpu_api_dec_get_frame_context(ImxVpuApiDecoder *decoder, struct v4l2_buffer *buffer)
{
	uint64_t pts_microseconds;
	int i;
	int frame_context_index;
	DecFrameContextItem *frame_context_item;

	/* Look up the frame context items that matches the frame that is stored in the v4l2_buffer.
	 * TODO: Normally, this should be doable by storing the frame context index in the
	 * buffer->timestamp field. However, the Amphion decoder seems to expect PTS in that
	 * timestamp to use this for reordering frames, so we can't store the index directly.
	 * Instead, "timestamp" contains the frame PTS in microseconds, and we have to find
	 * the context that has a matching PTS. */ 

	pts_microseconds = ((uint64_t)(buffer->timestamp.tv_sec)) * 1000000 + ((uint64_t)(buffer->timestamp.tv_usec));
	frame_context_index = -1;

	for (i = 0; i < decoder->num_frame_context_items; ++i)
	{
		frame_context_item = &(decoder->frame_context_items[i]);

		if (!frame_context_item->in_use)
			continue;

		if (frame_context_item->pts_microseconds == pts_microseconds)
		{
			frame_context_index = i;
			break;
		}
	}

	if (frame_context_index < 0)
	{
		IMX_VPU_API_ERROR(
			"could not find frame context index for V4L2 capture buffer with index %d (pts_microseconds %" PRIu64 ")",
			(int)(buffer->index),
			pts_microseconds
		);
	}

	return frame_context_index;
}


static BOOL imx_vpu_api_dec_handle_resolution_change(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code)
{
	int i, num_planes, errornum;
	int min_num_buffers_for_capture;
	ImxVpuApiDecStreamInfo *stream_info;
	ImxVpuApiFramebufferMetrics *fb_metrics;
	struct v4l2_control control;
	struct v4l2_requestbuffers capture_buffer_request;
	enum g2d_format g2d_dest_format;


	/* Preliminary checks. */

	if (decoder->stream_info_announced)
	{
		IMX_VPU_API_DEBUG("detected changed resolution information");
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED;
		return TRUE;
	}


	/* Misc setup. */

	stream_info = &(decoder->stream_info);
	fb_metrics = &(stream_info->decoded_frame_framebuffer_metrics);

	IMX_VPU_API_DEBUG("detected resolution information");
	*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE;


	/* Get the format that was chosen by the driver so we
	 * can set up the capture buffers and the contents of
	 * ImxVpuApiFramebufferMetrics. */

	memset(&(decoder->capture_buffer_format), 0, sizeof(decoder->capture_buffer_format));
	decoder->capture_buffer_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (ioctl(decoder->v4l2_fd, VIDIOC_G_FMT, &(decoder->capture_buffer_format)) < 0)
	{
		IMX_VPU_API_ERROR("could not get V4L2 capture buffer format: %s (%d)", strerror(errno), errno);
		goto error;
	}

	num_planes = (int)(decoder->capture_buffer_format.fmt.pix_mp.num_planes);
	for (i = 0; i < num_planes; ++i)
	{
		IMX_VPU_API_DEBUG(
			"plane %d/%d: sizeimage %" PRIu32 " bytesperline %" PRIu16,
			i,
			num_planes,
			decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[i].sizeimage,
			decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[i].bytesperline
		);
	}

	decoder->actual_v4l2_pixelformat = decoder->capture_buffer_format.fmt.pix_mp.pixelformat;
	IMX_VPU_API_DEBUG(
		"requested V4L2 pixelformat: %" FOURCC_FORMAT "  actual V4L2 pixelformat: %" FOURCC_FORMAT,
		V4L2_FOURCC_ARGS(decoder->requested_v4l2_pixelformat),
		V4L2_FOURCC_ARGS(decoder->actual_v4l2_pixelformat)
	);


	/* Compute the Y/UV offsets and sizes for the capture buffers.
	 * (The sizes/offsets for the stream info framebuffer metrics
	 * are handled separately later.) */

	decoder->capture_buffer_y_stride = decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[0].bytesperline;
	decoder->capture_buffer_uv_stride = decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[1].bytesperline;
	decoder->capture_buffer_y_size = decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[0].sizeimage;
	decoder->capture_buffer_uv_size = decoder->capture_buffer_format.fmt.pix_mp.plane_fmt[1].sizeimage;
	decoder->capture_buffer_y_offset = 0;
	decoder->capture_buffer_uv_offset = decoder->capture_buffer_y_size;

	decoder->capture_buffer_size = decoder->capture_buffer_uv_offset + decoder->capture_buffer_uv_size;


	/* Allocate and queue the capture buffers. */

	memset(&control, 0, sizeof(control));
	control.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
	if (ioctl(decoder->v4l2_fd, VIDIOC_G_CTRL, &control) < 0)
	{
		IMX_VPU_API_ERROR("could not query min number of V4L2 capture buffers: %s (%d)", strerror(errno), errno);
		goto error;
	}
	min_num_buffers_for_capture = control.value;
	IMX_VPU_API_DEBUG("min num buffers for capture queue: %d", min_num_buffers_for_capture);

	IMX_VPU_API_DEBUG("requesting V4L2 capture buffers");
	memset(&capture_buffer_request, 0, sizeof(capture_buffer_request));
	capture_buffer_request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	capture_buffer_request.memory = V4L2_MEMORY_DMABUF;
	capture_buffer_request.count = min_num_buffers_for_capture;

	if (ioctl(decoder->v4l2_fd, VIDIOC_REQBUFS, &capture_buffer_request) < 0)
	{
		IMX_VPU_API_ERROR("could not request V4L2 capture buffers: %s (%d)", strerror(errno), errno);
		goto error;
	}

	decoder->num_capture_buffers = capture_buffer_request.count;
	IMX_VPU_API_DEBUG(
		"num V4L2 capture buffers:  requested: %d  actual: %d",
		min_num_buffers_for_capture,
		decoder->num_capture_buffers
	);

	if (decoder->num_capture_buffers < min_num_buffers_for_capture)
	{
		IMX_VPU_API_ERROR("driver did not provide enough capture buffers");
		goto error;
	}

	assert(decoder->num_capture_buffers > 0);

	/* Calculate the used_frame_context_item_count_limit out of the
	 * number of capture buffers. The basic idea behind this is that
	 * the decoder needs at least num_capture_buffers _encoded_ buffers
	 * to be able to begin produce a decoded frame. The
	 * decoder->num_capture_buffers * 3 + 1 calculation was determined
	 * empirically; just using decoder->num_capture_buffers did not
	 * work, since in some cases, the decoder stalled. It seems that
	 * in these cases, the decoder actually needs a little
	 * more than num_capture_buffers encoded buffers. */
	decoder->used_frame_context_item_count_limit = decoder->num_capture_buffers * 3 + 1;
	IMX_VPU_API_DEBUG("setting used frame context item count limit to %d", decoder->used_frame_context_item_count_limit);

	IMX_VPU_API_DEBUG("allocating and queuing V4L2 capture buffers");

	decoder->capture_buffer_items = calloc(decoder->num_capture_buffers, sizeof(DecV4L2CaptureBufferItem));
	assert(decoder->capture_buffer_items != NULL);

	for (i = 0; i < decoder->num_capture_buffers; ++i)
	{
		int dmabuf_fd;
		struct v4l2_buffer buffer;
		struct v4l2_plane planes[DEC_NUM_CAPTURE_BUFFER_PLANES];
		DecV4L2CaptureBufferItem *capture_buffer_item = &(decoder->capture_buffer_items[i]);

		capture_buffer_item->dma_buffer = imx_dma_buffer_allocate(
			decoder->ion_dma_buffer_allocator,
			decoder->capture_buffer_size,
			1,
			&errornum
		);
		if (capture_buffer_item->dma_buffer == NULL)
		{
			IMX_VPU_API_ERROR("could not allocate DMA buffer for V4L2 capture buffer #%d: %s (%d)", i, strerror(errornum), errornum);
			goto error;
		}

		dmabuf_fd = imx_dma_buffer_get_fd(capture_buffer_item->dma_buffer);

		IMX_VPU_API_DEBUG("allocated DMA buffer for V4L2 capture buffer #%d with DMA-BUF FD %d", i, dmabuf_fd);

		/* Set the offsets, sizes, and DMA-BUF FDs in the V4L2 plane
		 * structures. Note that here, we (= the application) set
		 * the data_offset fields, even though the v4l2_plane
		 * documentation suggests that normally, these fields are
		 * set by the driver. That rule seems to be specific to
		 * video capture devices (like webcams), and not apply to
		 * mem2mem decoders. */

		capture_buffer_item->planes[0].data_offset = decoder->capture_buffer_y_offset;
		capture_buffer_item->planes[0].bytesused = decoder->capture_buffer_y_offset + decoder->capture_buffer_y_size;
		capture_buffer_item->planes[0].m.fd = dmabuf_fd;

		capture_buffer_item->planes[1].data_offset = decoder->capture_buffer_uv_offset;
		capture_buffer_item->planes[1].bytesused = decoder->capture_buffer_uv_offset + decoder->capture_buffer_uv_size;
		capture_buffer_item->planes[1].m.fd = dmabuf_fd;

		capture_buffer_item->buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		capture_buffer_item->buffer.index = i;
		capture_buffer_item->buffer.memory = V4L2_MEMORY_DMABUF;
		capture_buffer_item->buffer.length = DEC_NUM_CAPTURE_BUFFER_PLANES;
		capture_buffer_item->buffer.m.planes = capture_buffer_item->planes;

		/* We copy the v4l2_buffer instance in case the driver
		 * modifies its fields. (This preserves the original.) */
		memcpy(&buffer, &(capture_buffer_item->buffer), sizeof(buffer));
		memcpy(planes, capture_buffer_item->planes, sizeof(struct v4l2_plane) * DEC_NUM_CAPTURE_BUFFER_PLANES);
		/* Make sure "planes" points to the _copy_ of the planes structures. */
		buffer.m.planes = planes;

		if (ioctl(decoder->v4l2_fd, VIDIOC_QBUF, &buffer) < 0)
		{
			IMX_VPU_API_ERROR("could not queue capture buffer: %s (%d)", strerror(errno), errno);
			goto error;
		}
	}


	/* Write the stream info fields and compute the framebuffer metrics. */

	memset(stream_info, 0, sizeof(ImxVpuApiDecStreamInfo));

	fb_metrics->actual_frame_width = decoder->capture_buffer_format.fmt.pix_mp.width;
	fb_metrics->actual_frame_height = decoder->capture_buffer_format.fmt.pix_mp.height;

	switch (decoder->decoded_frame_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
		{
			fb_metrics->y_stride = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, G2D_DEST_AMPHION_STRIDE_ALIGNMENT);
			/* Y and UV stride are the same because in NV12 formats the UV
			 * values are contained in the same plane. As a result, one UV
			 * row contains 2 * (y_stride/2) = y_stride values. */
			fb_metrics->uv_stride = fb_metrics->y_stride;

			fb_metrics->aligned_frame_width = fb_metrics->y_stride;
			fb_metrics->aligned_frame_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, G2D_ROW_COUNT_ALIGNMENT);

			fb_metrics->y_size = fb_metrics->y_stride * fb_metrics->aligned_frame_height;
			/* NV12 is a 4:2:0 format. The U and V planes are both half width
			 * and half height. In NV12, they are combined into one UV plane.
			 * As a result, that plane's stride is the same as that of the Y
			 * plane, but the plane's height is still half. */
			fb_metrics->uv_size = fb_metrics->y_stride * IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height / 2, G2D_ROW_COUNT_ALIGNMENT);

			fb_metrics->y_offset = 0;
			fb_metrics->u_offset = fb_metrics->y_size;

			break;
		}

		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_UYVY_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_YUYV_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_RGBA8888:
		case IMX_VPU_API_COLOR_FORMAT_BGRA8888:
		case IMX_VPU_API_COLOR_FORMAT_RGB565:
		case IMX_VPU_API_COLOR_FORMAT_BGR565:
		{
			/* Only setting the values for Y here. Packed YUV formats
			 * and RGB formats use only one plane, so the U and V
			 * values remain unused. */

			fb_metrics->y_stride = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, G2D_DEST_AMPHION_STRIDE_ALIGNMENT);

			fb_metrics->aligned_frame_width = fb_metrics->y_stride;
			fb_metrics->aligned_frame_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, G2D_ROW_COUNT_ALIGNMENT);

			fb_metrics->y_size = fb_metrics->y_stride * fb_metrics->aligned_frame_height;

			fb_metrics->y_offset = 0;

			break;
		}

		default:
			/* Should not be reached. If so, this indicates that code is missing for
			 * a particular format, or that the decoded_frame_format is corrupted. */
			assert(FALSE);
	}

	switch (decoder->decoded_frame_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT: g2d_dest_format = G2D_NV12; break;
		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_UYVY_8BIT: g2d_dest_format = G2D_UYVY; break;
		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_YUYV_8BIT: g2d_dest_format = G2D_YUYV; break;
		case IMX_VPU_API_COLOR_FORMAT_RGBA8888: g2d_dest_format = G2D_RGBA8888; break;
		case IMX_VPU_API_COLOR_FORMAT_BGRA8888: g2d_dest_format = G2D_BGRA8888; break;
		case IMX_VPU_API_COLOR_FORMAT_RGB565: g2d_dest_format = G2D_RGB565; break;
		case IMX_VPU_API_COLOR_FORMAT_BGR565: g2d_dest_format = G2D_BGR565; break;
		default:
			/* Should not be reached. If so, this indicates that code is missing for
			 * a particular format, or that the decoded_frame_format is corrupted. */
			assert(FALSE);
	}

	stream_info->min_fb_pool_framebuffer_size = stream_info->min_output_framebuffer_size = fb_metrics->u_offset + fb_metrics->uv_size;

	IMX_VPU_API_DEBUG(
		"framebuffer metrics:  Y/UV stride: %zu/%zu  Y/UV size: %zu/%zu  Y/U offset: %zu/%zu",
		fb_metrics->y_stride, fb_metrics->uv_stride,
		fb_metrics->y_size, fb_metrics->uv_size,
		fb_metrics->y_offset, fb_metrics->u_offset
	);
	IMX_VPU_API_DEBUG("min output framebuffer size: %zu", stream_info->min_output_framebuffer_size);

	/* Set to 0 since this decoder does not require FB pool framebuffers. */
	stream_info->fb_pool_framebuffer_alignment = 0;
	/* Set to the alignment that G2D expects in output physical addresses. */
	stream_info->output_framebuffer_alignment = 64;

	/* TODO: Can the framerate be retrieved from mem2mem devices? */
	stream_info->frame_rate_numerator = 0;
	stream_info->frame_rate_denominator = 0;

	stream_info->min_num_required_framebuffers = 0;

	stream_info->color_format = decoder->decoded_frame_format;

	if (imx_vpu_api_is_color_format_semi_planar(decoder->decoded_frame_format))
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES;


	/* Set up the G2D surfaces. */

	memset(&(decoder->source_g2d_surface), 0, sizeof(struct g2d_surfaceEx));
	decoder->source_g2d_surface.base.format = G2D_NV12;
	decoder->source_g2d_surface.base.left = 0;
	decoder->source_g2d_surface.base.top = 0;
	decoder->source_g2d_surface.base.right = fb_metrics->actual_frame_width;
	decoder->source_g2d_surface.base.bottom = fb_metrics->actual_frame_height;
	decoder->source_g2d_surface.base.stride = decoder->capture_buffer_y_stride;
	decoder->source_g2d_surface.base.width = decoder->capture_buffer_y_stride;
	/* Include padding rows in the "height" field, since G2D has no other
	 * way to specify how many padding rows there are.
	 * (The actual frame height is written into the "bottom" field above.) */
	decoder->source_g2d_surface.base.height = decoder->capture_buffer_y_size / decoder->capture_buffer_y_stride;
	decoder->source_g2d_surface.base.blendfunc = G2D_ONE;
	decoder->source_g2d_surface.tiling = (decoder->actual_v4l2_pixelformat == V4L2_PIX_FMT_NV12) ? G2D_AMPHION_TILED : G2D_AMPHION_TILED_10BIT;

	memset(&(decoder->dest_g2d_surface), 0, sizeof(struct g2d_surfaceEx));
	decoder->dest_g2d_surface.base.format = g2d_dest_format;
	decoder->dest_g2d_surface.base.left = 0;
	decoder->dest_g2d_surface.base.top = 0;
	decoder->dest_g2d_surface.base.right = fb_metrics->actual_frame_width;
	decoder->dest_g2d_surface.base.bottom = fb_metrics->actual_frame_height;
	decoder->dest_g2d_surface.base.stride = fb_metrics->y_stride;
	decoder->dest_g2d_surface.base.width = fb_metrics->aligned_frame_width;
	decoder->dest_g2d_surface.base.height = fb_metrics->aligned_frame_height;
	decoder->dest_g2d_surface.base.blendfunc = G2D_ZERO;
	decoder->dest_g2d_surface.tiling = G2D_LINEAR;


	/* Now enable the capture stream. */

	if (!imx_vpu_api_dec_enable_stream(decoder, TRUE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))
		goto error;


	IMX_VPU_API_DEBUG(
		"num frame context items at time of resolution change event: %d, %d out of which are available",
		decoder->num_frame_context_items,
		decoder->num_available_frame_context_items
	);


	decoder->stream_info_announced = TRUE;

	return TRUE;

error:
	return FALSE;
}


static BOOL imx_vpu_api_dec_garbage_collect_oldest_frame(ImxVpuApiDecoder *decoder)
{
	int i;
	int oldest_frame_index = -1;
	uint64_t oldest_pts = 0;

	/* Find the oldest frame context item that is still in use.
	 * The PTS determines which one is the oldest. */
	for (i = 0; i < decoder->num_frame_context_items; ++i)
	{
		DecFrameContextItem *frame_context_item = &(decoder->frame_context_items[i]);

		if (!frame_context_item->in_use)
			continue;

		/* The (oldest_frame_index < 0) check is here to make
		 * sure that when we encounter the very first item that
		 * is in use we set it as the current oldest frame.
		 * Otherwise, oldest_pts would be used in comparisons
		 * without having been initialized to a valid value.
		 * Note that we make sure to take the first item
		 * _that is in use_, not necessarily the first item
		 * overall, since the item at index #0 may currently
		 * not be in use. */
		if ((oldest_frame_index < 0) || (frame_context_item->pts < oldest_pts))
		{
			oldest_pts = frame_context_item->pts;
			oldest_frame_index = i;
		}
	}

	if (oldest_frame_index >= 0)
	{
		DecFrameContextItem *frame_context_item = &(decoder->frame_context_items[oldest_frame_index]);

		IMX_VPU_API_LOG(
			"garbage-collecting oldest frame context with index %d:"
			"  context pointer %p PTS %" PRIu64 " DTS %" PRIu64,
			oldest_frame_index,
			frame_context_item->context,
			frame_context_item->pts,
			frame_context_item->dts
		);

		/* Copy the frame context item contents for later
		 * imx_vpu_api_dec_get_skipped_frame_info() calls. */
		memcpy(&(decoder->skipped_frame_context_item), frame_context_item, sizeof(DecFrameContextItem));

		/* As past of the garbage collection, mark this frame context
		 * item as free, since the associated frame was skipped and
		 * will never be output. */
		imx_vpu_api_dec_mark_frame_context_as_available(decoder, oldest_frame_index);

		return TRUE;
	}
	else
	{
		/* Not expected to happen! */
		IMX_VPU_API_ERROR("could not find oldest frame for garbage-collection");
		return FALSE;
	}
}




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


struct _ImxVpuApiEncoder
{
	int v4l2_fd;

	/* DEPRECATED. This is kept here for backwards compatibility. */
	BOOL drain_mode_enabled;
};


static ImxVpuApiCompressionFormat const enc_supported_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_H264
};

static ImxVpuApiEncGlobalInfo const enc_global_info = {
	.flags = /*IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_HAS_ENCODER
	       | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED
	       | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED
	       | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_ENCODER_SUPPORTS_RGB_FORMATS*/0,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_AMPHION,
	.min_required_stream_buffer_size = 0,
	.required_stream_buffer_physaddr_alignment = 1,
	.required_stream_buffer_size_alignment = 1,
	.supported_compression_formats = enc_supported_compression_formats,
	.num_supported_compression_formats = sizeof(enc_supported_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};

ImxVpuApiEncGlobalInfo const * imx_vpu_api_enc_get_global_info(void)
{
	return &enc_global_info;
}


ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_enc_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	IMX_VPU_API_UNUSED_PARAM(compression_format);
	return NULL;
}


void imx_vpu_api_enc_set_default_open_params(ImxVpuApiCompressionFormat compression_format, ImxVpuApiColorFormat color_format, size_t frame_width, size_t frame_height, ImxVpuApiEncOpenParams *open_params)
{
	IMX_VPU_API_UNUSED_PARAM(compression_format);
	IMX_VPU_API_UNUSED_PARAM(color_format);
	IMX_VPU_API_UNUSED_PARAM(frame_width);
	IMX_VPU_API_UNUSED_PARAM(frame_height);
	IMX_VPU_API_UNUSED_PARAM(open_params);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_open(ImxVpuApiEncoder **encoder, ImxVpuApiEncOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(open_params);
	IMX_VPU_API_UNUSED_PARAM(stream_buffer);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


void imx_vpu_api_enc_close(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
}


ImxVpuApiEncStreamInfo const * imx_vpu_api_enc_get_stream_info(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	return NULL;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_add_framebuffers_to_pool(ImxVpuApiEncoder *encoder, ImxDmaBuffer **fb_dma_buffers, size_t num_framebuffers)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffers);
	IMX_VPU_API_UNUSED_PARAM(num_framebuffers);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


void imx_vpu_api_enc_enable_drain_mode(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	assert(encoder != NULL);
	encoder->drain_mode_enabled = TRUE;
}


int imx_vpu_api_enc_is_drain_mode_enabled(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	assert(encoder != NULL);
	return encoder->drain_mode_enabled;
}


void imx_vpu_api_enc_flush(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_bitrate(ImxVpuApiEncoder *encoder, unsigned int bitrate)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(bitrate);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_frame_rate(ImxVpuApiEncoder *encoder, unsigned int frame_rate_numerator, unsigned int frame_rate_denominator)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(frame_rate_numerator);
	IMX_VPU_API_UNUSED_PARAM(frame_rate_denominator);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_push_raw_frame(ImxVpuApiEncoder *encoder, ImxVpuApiRawFrame const *raw_frame)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(raw_frame);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_encode(ImxVpuApiEncoder *encoder, size_t *encoded_frame_size, ImxVpuApiEncOutputCodes *output_code)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(encoded_frame_size);
	IMX_VPU_API_UNUSED_PARAM(output_code);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(encoded_frame);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}
