#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>

#include <config.h>

#include <imxdmabuffer/imxdmabuffer.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"

/* This is necessary to turn off these warning that originate in OMX_Core.h :
 *   "ISO C restricts enumerator values to range of ‘int’""    */
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "dwl.h"
#include "codec.h"
#include "codec_avs.h"
#include "codec_h264.h"
#include "codec_hevc.h"
#include "codec_jpeg.h"
#include "codec_mpeg2.h"
#include "codec_mpeg4.h"
#include "codec_pp.h"
#include "codec_rv.h"
#include "codec_vc1.h"
#include "codec_vp6.h"
#include "codec_vp8.h"
#include "codec_vp9.h"
#include "codec_webp.h"
#include "vsi_vendor_ext.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


/* Define the stream buffer size to be able to hold 2 big frames */
#define VPU_DEC_MIN_REQUIRED_STREAM_BUFFER_SIZE  (1024*1024*16)
#define STREAM_BUFFER_PHYSADDR_ALIGNMENT         (0x10)
#define STREAM_BUFFER_SIZE_ALIGNMENT             (1024)


/* This constant is used to specify that an index does not exist
 * or is otherwise invalid, for example when a fram entry is searched
 * and not found */
#define INVALID_FRAME_ENTRY_INDEX  SIZE_MAX


static char const * codec_state_to_string(CODEC_STATE codec_state)
{
	switch (codec_state)
	{
		case CODEC_NEED_MORE: return "CODEC_NEED_MORE";
		case CODEC_HAS_FRAME: return "CODEC_HAS_FRAME";
		case CODEC_HAS_INFO: return "CODEC_HAS_INFO";
		case CODEC_OK: return "CODEC_OK";
		case CODEC_PIC_SKIPPED: return "CODEC_PIC_SKIPPED";
		case CODEC_END_OF_STREAM: return "CODEC_END_OF_STREAM";
		case CODEC_WAITING_FRAME_BUFFER: return "CODEC_WAITING_FRAME_BUFFER";
		case CODEC_ABORTED: return "CODEC_ABORTED";
		case CODEC_FLUSHED: return "CODEC_FLUSHED";
		case CODEC_BUFFER_EMPTY: return "CODEC_BUFFER_EMPTY";
		case CODEC_PENDING_FLUSH: return "CODEC_PENDING_FLUSH";
		case CODEC_NO_DECODING_BUFFER: return "CODEC_NO_DECODING_BUFFER";
#ifdef HAVE_IMXVPUDEC_HANTRO_CODEC_ERROR_FRAME
		case CODEC_ERROR_FRAME: return "CODEC_ERROR_FRAME";
#endif
		case CODEC_ERROR_HW_TIMEOUT: return "CODEC_ERROR_HW_TIMEOUT";
		case CODEC_ERROR_HW_BUS_ERROR: return "CODEC_ERROR_HW_BUS_ERROR";
		case CODEC_ERROR_SYS: return "CODEC_ERROR_SYS";
		case CODEC_ERROR_DWL: return "CODEC_ERROR_DWL";
		case CODEC_ERROR_UNSPECIFIED: return "CODEC_ERROR_UNSPECIFIED";
		case CODEC_ERROR_STREAM: return "CODEC_ERROR_STREAM";
		case CODEC_ERROR_INVALID_ARGUMENT: return "CODEC_ERROR_INVALID_ARGUMENT";
		case CODEC_ERROR_NOT_INITIALIZED: return "CODEC_ERROR_NOT_INITIALIZED";
		case CODEC_ERROR_INITFAIL: return "CODEC_ERROR_INITFAIL";
		case CODEC_ERROR_HW_RESERVED: return "CODEC_ERROR_HW_RESERVED";
		case CODEC_ERROR_MEMFAIL: return "CODEC_ERROR_MEMFAIL";
		case CODEC_ERROR_STREAM_NOT_SUPPORTED: return "CODEC_ERROR_STREAM_NOT_SUPPORTED";
		case CODEC_ERROR_FORMAT_NOT_SUPPORTED: return "CODEC_ERROR_FORMAT_NOT_SUPPORTED";
		case CODEC_ERROR_NOT_ENOUGH_FRAME_BUFFERS: return "CODEC_ERROR_NOT_ENOUGH_FRAME_BUFFERS";
		case CODEC_ERROR_BUFFER_SIZE: return "CODEC_ERROR_BUFFER_SIZE";
		default:
			return "<unknown>";
	}
}




/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* Structure for framebuffers that get added to the VPU's pool.
 * There is one entry for each added framebuffer. Not to be
 * confused with FrameEntry.
 * Each framebuffer consists of a DMA buffer, a context pointer,
 * the DMA buffer's physical address, and the virtual address
 * that the DMA buffer was mapped to. */
typedef struct
{
	void *mapped_virtual_address;
	imx_physical_address_t physical_address;
	ImxDmaBuffer *fb_dma_buffer;
	void *fb_context;
}
FramebufferEntry;


/* Structure for keeping track of frames that are being decoded
 * or got decoded. This is different to FramebufferEntry in that
 * framebuffers make up a buffer pool that is used by the VPU,
 * while FrameEntry contains entries that correspond to encoded
 * input frames and allow for associating encoded input frames
 * with raw output frames. */
typedef struct
{
	/* Nonzero if this FrameEntry is filled with information about
	 * an encoded input frame that was fed into the VPU. Zero if
	 * this entry is not associated with such a frame and can be
	 * used for storing frame information. */
	int occupied;
	/* Context pointer from the ImxVpuApiEncodedFrame context field. */
	void *context;
	/* PTS/DTS from the ImxVpuApiEncodedFrame pts/dts fields. */
	uint64_t pts, dts;
}
FrameEntry;


/* RealVideo specific information, coming from the header. */
/* TODO: This is not in use yet since the RealVideo decoding is not yet working. */
typedef struct
{
	uint32_t offset;
	uint32_t endianness;
}
RvDecSliceInfo;


struct _ImxVpuApiDecoder
{
	/* Hantro codec that is in use. */
	CODEC_PROTOTYPE *codec;
	/* DWL instance, needed by the codec. */
	void const *dwl_instance;

	/* Stream buffer. Holds encoded data that shall be decoded. This includes
	 * additional header metadata that may have to be manually produced and
	 * inserted by the imx_vpu_api_dec_preprocess_input_data() function. The
	 * actual insertion of data into the stream buffer is done by calling
	 * imx_vpu_api_dec_push_input_data(). */
	ImxDmaBuffer *stream_buffer;
	uint8_t *stream_buffer_virtual_address;
	imx_physical_address_t stream_buffer_physical_address;
	size_t stream_buffer_size;
	/* Offset and size values to keep track of where to read from and write
	 * to the stream buffer. */
	size_t stream_buffer_read_offset;
	size_t stream_buffer_write_offset;
	size_t stream_buffer_fill_level;

	/* Offset used in imx_vpu_api_dec_push_encoded_frame(). The first N bytes
	 * of the encoded frame data will be skipped, N = encoded_frame_offset. */
	size_t encoded_frame_offset;

	/* If TRUE, then EOS will be reported by imx_vpu_api_dec_decode() after
	 * one frame was decoded. Used by the WebP decoder. */
	BOOL single_frame_decoding;

	/* If TRUE, then certain "invisible frames" will be skipped. With some
	 * formats like VP9, there are internal frames that get decoded, but are
	 * not intended to be shown. In such cases, this internal frame needs to
	 * be reported as skipped, and the reason for the skip has to be set to
	 * IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME. The value of
	 * decoded_frame_reported also needs to be checked to determine this. */
	BOOL skip_invisible_frames;

	/* The codec endofstream() function is used if this is set to TRUE. Some
	 * codecs have a bug-ridden endofstream() implementation, and/or do not
	 * actually need these calls. In such cases, this is set to FALSE. */
	BOOL use_endofstream_function;

	/* The codec decode() function reports decoded frames by returning
	 * CODEC_HAS_FRAME. However, "decoded" does not automatically mean that
	 * said frame is already available. Instead, getframe() has to be called
	 * to determine if a decoded frame can be shown. In case of internal
	 * frames (see skip_invisible_frames above), we need to keep track of
	 * decoded internal frames. */
	BOOL decoded_frame_reported;
	/* TRUE if encoded data is available to the codec. This is used to
	 * prevent excess imx_vpu_api_dec_push_encoded_frame() calls. This
	 * function is supposed to be called only after imx_vpu_api_dec_decode()
	 * returned the IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED
	 * output code. If this is set to TRUE, and this function is called,
	 * then it returns with an error. This value is set to TRUE if
	 * imx_vpu_api_dec_push_encoded_frame() finishes successfully, and
	 * set back to FALSE once imx_vpu_api_dec_decode() returns the output
	 * code mentioned above. */
	BOOL encoded_data_available;

	/* RealVideo specific information. */
	/* TODO: Not in use yet due to no-yet-working RealVideo decoding. */
	int slice_info_nr;
	RvDecSliceInfo slice_info[128];

	/* Copy of the open_params argument from imx_vpu_api_dec_open(). */
	ImxVpuApiDecOpenParams open_params;

	/* Information about the stream that is being decoded. This field's
	 * contents may change mid-decoding with some formats like h.264,
	 * which can contain subsections with different formats. */
	ImxVpuApiDecStreamInfo stream_info;
	BOOL has_new_stream_info;

	BOOL ring_buffer_mode;
	BOOL main_header_pushed;
	BOOL drain_mode_enabled;
	BOOL end_of_stream_reached;

	FramebufferEntry *framebuffer_entries;
	size_t num_framebuffer_entries;

	FrameEntry *frame_entries;
	size_t num_frame_entries;

	size_t num_framebuffers_to_be_added;

	size_t last_pushed_frame_entry_index;
	size_t decoded_frame_fb_entry_index;
	size_t decoded_frame_entry_index;

	ImxVpuApiDecSkippedFrameReasons skipped_frame_reason;
	void *skipped_frame_context;
	uint64_t skipped_frame_pts;
	uint64_t skipped_frame_dts;
};


static void imx_vpu_api_dec_preprocess_input_data(ImxVpuApiDecoder *decoder, uint8_t const *extra_header_data, size_t extra_header_data_size, uint8_t *main_data, size_t main_data_size);
static void imx_vpu_api_dec_push_input_data(ImxVpuApiDecoder *decoder, void const *data, size_t data_size);

static size_t imx_vpu_api_get_free_frame_entry_index(ImxVpuApiDecoder *decoder);
static void imx_vpu_api_dec_clear_frame_entries(ImxVpuApiDecoder *decoder);

static size_t imx_vpu_api_dec_find_framebuffer_entry_index(ImxVpuApiDecoder *decoder, imx_physical_address_t physical_address);
static size_t imx_vpu_api_dec_add_framebuffer_entries(ImxVpuApiDecoder *decoder, size_t num_new_entries);
static void imx_vpu_api_dec_clear_added_framebuffers(ImxVpuApiDecoder *decoder);

static BOOL imx_vpu_api_dec_get_new_stream_info(ImxVpuApiDecoder *decoder);


static void imx_vpu_api_dec_preprocess_input_data(ImxVpuApiDecoder *decoder, uint8_t const *extra_header_data, size_t extra_header_data_size, uint8_t *main_data, size_t main_data_size)
{
	assert(decoder != NULL);

	switch (decoder->open_params.compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX3:
		{
			uint8_t header[DIVX3_FRAME_HEADER_SIZE];

			if (!(decoder->main_header_pushed))
			{
				imx_vpu_api_insert_divx3_frame_header(header, decoder->open_params.frame_width, decoder->open_params.frame_height);
				imx_vpu_api_dec_push_input_data(decoder, header, DIVX3_FRAME_HEADER_SIZE);
				decoder->main_header_pushed = TRUE;
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_RV30:
		case IMX_VPU_API_COMPRESSION_FORMAT_RV40:
		{
			/* NOTE: This code is derived from FFmpeg's rv34.c ff_rv34_decode_frame()
			 * and get_slice_offset() as well as from code pieces from imx-vpuwrap
			 * (the vpu-wrapper-hantro.c file). It is currently unclear if this is
			 * really correct, since no official spec is available. */

			size_t read_offset = 0;
			size_t i, num_fragments;
			num_fragments = (size_t)(main_data[read_offset++]) + 1;

			IMX_VPU_API_DEBUG("RealVideo num fragments: %zu", num_fragments);

			decoder->encoded_frame_offset = 1 + num_fragments * 8;

			for (i = 0; i < num_fragments; ++i)
			{
				decoder->slice_info[i].endianness = READ_32BIT_LE(main_data, read_offset);
				read_offset += 4;
				decoder->slice_info[i].offset = (decoder->slice_info[i].endianness == 1) ? READ_32BIT_LE(main_data, read_offset) : READ_32BIT_BE(main_data, read_offset);
				read_offset += 4;
				IMX_VPU_API_DEBUG("RealVideo slice #%zu: endianness %" PRIu32 " offset %" PRIu32, i, decoder->slice_info[i].endianness, decoder->slice_info[i].offset);
			}

			decoder->slice_info_nr = num_fragments;

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WEBP:
		{
			/* There is nothing to insert for WebP. However, we have
			 * to skip the initial RIFF header and some FourCCs,
			 * since the codec expects these to be read already. */
			decoder->encoded_frame_offset = 20;
			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		{
			/* The Hantro decoder does not require frame
			 * layer headers, but it does require a sequence
			 * layer header, so insert that one here. */
			if (!(decoder->main_header_pushed))
			{
				uint8_t header[WMV3_RCV_SEQUENCE_LAYER_HEADER_SIZE];

				assert(extra_header_data != NULL);
				assert(extra_header_data_size >= 4);

				imx_vpu_api_insert_wmv3_sequence_layer_header(header, decoder->open_params.frame_width, decoder->open_params.frame_height, main_data_size, extra_header_data);
				/* The Hantro VC-1/WMV3 parser expects an RCV1 header
				 * without the 32-bit unsigned integer at the end (this
				 * integer contains the frame size), so make sure it is
				 * left out by subtracting 4 from the size of the header
				 * data that we want to push into the stream buffer. */
				imx_vpu_api_dec_push_input_data(decoder, header, WMV3_RCV_SEQUENCE_LAYER_HEADER_SIZE - 4);
				decoder->main_header_pushed = TRUE;
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		{
			if (!(decoder->main_header_pushed))
			{
				assert(extra_header_data != NULL);
				assert(extra_header_data_size > 1);

				/* First, push the extra_header_data (except for its first byte,
				 * which contains the size of the extra header data), since it
				 * contains the sequence layer header */
				IMX_VPU_API_LOG("pushing extra header data with %zu byte", extra_header_data_size - 1);
				imx_vpu_api_dec_push_input_data(decoder, extra_header_data + 1, extra_header_data_size - 1);

				decoder->main_header_pushed = TRUE;

				/* Next, the frame layer header will be pushed by the
				 * block below */
			}

			if (decoder->main_header_pushed)
			{
				uint8_t header[VC1_NAL_FRAME_LAYER_HEADER_MAX_SIZE];
				size_t actual_header_length;
				imx_vpu_api_insert_vc1_frame_layer_header(header, main_data, &actual_header_length);
				if (actual_header_length > 0)
				{
					IMX_VPU_API_LOG("pushing frame layer header with %zu byte", actual_header_length);
					imx_vpu_api_dec_push_input_data(decoder, header, actual_header_length);
				}
			}

			break;
		}

		default:
			if (!(decoder->main_header_pushed) && (extra_header_data != NULL) && (extra_header_data_size > 0))
			{
				imx_vpu_api_dec_push_input_data(decoder, extra_header_data, extra_header_data_size);
				decoder->main_header_pushed = TRUE;
			}
	}
}


static void imx_vpu_api_dec_push_input_data(ImxVpuApiDecoder *decoder, void const *data, size_t data_size)
{
	size_t read_offset, write_offset, fill_level;
	size_t bbuf_size;
	uint8_t const *src_data_bytes;
	uint8_t *streambuf_bytes;

	assert(decoder != NULL);
	assert(data != NULL);
	assert(data_size > 0);

	read_offset = decoder->stream_buffer_read_offset;
	write_offset = decoder->stream_buffer_write_offset;
	fill_level = decoder->stream_buffer_fill_level;

	bbuf_size = decoder->stream_buffer_size;

	src_data_bytes = (uint8_t const *)data;
	streambuf_bytes = (uint8_t *)(decoder->stream_buffer_virtual_address);

	/**
	 * In ring buffer mode, the read offset must not be touched here.
	 * Instead, the write operation has to wrap around the stream
	 * buffer size if writing the entire data set would exceed the
	 * boundary of the buffer.
	 *
	 * In non ring buffer mode, no such wrap around is done. Instead,
	 * the data that is still to be read is shifted forwards so that
	 * the read offset is 0 (but only if the write operation would
	 * exceed the boundary of the buffer).
	 *
	 * JPEG data is a special case. JPEG frames are always read from
	 * the start, so we have to constantly move leftover data to the
	 * front of the ringbuffer.
	 */
	if (((write_offset + data_size) > bbuf_size) || (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG))
	{
		if (decoder->ring_buffer_mode)
		{
			size_t first_copy_size = bbuf_size - write_offset;
			size_t second_copy_size = data_size - first_copy_size;
			memcpy(streambuf_bytes + write_offset, src_data_bytes, first_copy_size);
			memcpy(streambuf_bytes, src_data_bytes + first_copy_size, second_copy_size);
			decoder->stream_buffer_write_offset = second_copy_size;
		}
		else
		{
			memmove(streambuf_bytes, streambuf_bytes + read_offset, fill_level);
			decoder->stream_buffer_read_offset = 0;
			decoder->stream_buffer_write_offset = fill_level;
			memcpy(streambuf_bytes + decoder->stream_buffer_write_offset, src_data_bytes, data_size);
			decoder->stream_buffer_write_offset += data_size;
		}
	}
	else
	{
		memcpy(streambuf_bytes + write_offset, src_data_bytes, data_size);
		decoder->stream_buffer_write_offset += data_size;
	}

	decoder->stream_buffer_fill_level += data_size;
}


static size_t imx_vpu_api_get_free_frame_entry_index(ImxVpuApiDecoder *decoder)
{
	size_t index;
	FrameEntry *new_entries;

	assert(decoder != NULL);

	for (index = 0; index < decoder->num_frame_entries; ++index)
	{
		FrameEntry *entry = &(decoder->frame_entries[index]);
		if (!(entry->occupied))
			return index;
	}

	new_entries = realloc(decoder->frame_entries, sizeof(FrameEntry) * (decoder->num_frame_entries + 1));
	assert(new_entries != NULL);

	IMX_VPU_API_DEBUG("(re)allocated space for additional frame entry");

	index = decoder->num_frame_entries;
	decoder->num_frame_entries++;
	decoder->frame_entries = new_entries;

	return index;
}


static void imx_vpu_api_dec_clear_frame_entries(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	IMX_VPU_API_LOG("clearing %zu frame entries", decoder->num_frame_entries);
	free(decoder->frame_entries);
	decoder->frame_entries = NULL;
	decoder->num_frame_entries = 0;
}


static size_t imx_vpu_api_dec_find_framebuffer_entry_index(ImxVpuApiDecoder *decoder, imx_physical_address_t physical_address)
{
	size_t index;

	assert(decoder != NULL);
	assert(physical_address != 0);

	for (index = 0; index < decoder->num_framebuffer_entries; ++index)
	{
		FramebufferEntry *entry = &(decoder->framebuffer_entries[index]);
		if (entry->physical_address == physical_address)
			return index;
	}

	return INVALID_FRAME_ENTRY_INDEX;
}


static size_t imx_vpu_api_dec_add_framebuffer_entries(ImxVpuApiDecoder *decoder, size_t num_new_entries)
{
	int new_entries_index;
	FramebufferEntry *new_entries;

	assert(decoder != NULL);
	assert(num_new_entries > 0);

	new_entries = realloc(decoder->framebuffer_entries, sizeof(FramebufferEntry) * (decoder->num_framebuffer_entries + num_new_entries));
	assert(new_entries != NULL);

	new_entries_index = decoder->num_framebuffer_entries;
	IMX_VPU_API_DEBUG("(re)allocated space for additional %d framebuffer entries", num_new_entries);
	decoder->num_framebuffer_entries += num_new_entries;
	decoder->framebuffer_entries = new_entries;

	memset(&decoder->framebuffer_entries[new_entries_index], 0, sizeof(FramebufferEntry) * num_new_entries);

	return new_entries_index;
}


static void imx_vpu_api_dec_clear_added_framebuffers(ImxVpuApiDecoder *decoder)
{
	size_t index;

	assert(decoder != NULL);

	IMX_VPU_API_LOG("clearing %zu added framebuffer(s)", decoder->num_framebuffer_entries);

	for (index = 0; index < decoder->num_framebuffer_entries; ++index)
	{
		FramebufferEntry *entry = &(decoder->framebuffer_entries[index]);
		if (entry->mapped_virtual_address != NULL)
			imx_dma_buffer_unmap(entry->fb_dma_buffer);
	}

	free(decoder->framebuffer_entries);
	decoder->framebuffer_entries = NULL;
	decoder->num_framebuffer_entries = 0;
}


static BOOL imx_vpu_api_dec_get_new_stream_info(ImxVpuApiDecoder *decoder)
{
	CODEC_STATE codec_state;
	STREAM_INFO hantro_stream_info = { 0 };
	ImxVpuApiDecStreamInfo *stream_info = &(decoder->stream_info);
	BOOL is_8bit;

	codec_state = decoder->codec->getinfo(decoder->codec, &hantro_stream_info);
	if (codec_state != CODEC_OK)
	{
		IMX_VPU_API_ERROR("could not get stream info: %s (%d)", codec_state_to_string(codec_state), codec_state);
		return FALSE;
	}

	is_8bit = (hantro_stream_info.bit_depth != 10);

	stream_info->flags = 0;

	switch ((int)(hantro_stream_info.format))
	{
		case OMX_COLOR_FormatYUV420Planar:
		case OMX_COLOR_FormatYUV420PackedPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_10BIT;
			break;

		case OMX_COLOR_FormatYUV420SemiPlanar:
		case OMX_COLOR_FormatYUV420PackedSemiPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT;
			break;

		case OMX_COLOR_FormatYUV411Planar:
		case OMX_COLOR_FormatYUV411PackedPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_10BIT;
			break;

		case OMX_COLOR_FormatYUV411SemiPlanar:
		case OMX_COLOR_FormatYUV411PackedSemiPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_8BIT : IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_10BIT;
			break;

		case OMX_COLOR_FormatYUV422Planar:
		case OMX_COLOR_FormatYUV422PackedPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_10BIT;
			break;

		case OMX_COLOR_FormatYUV422SemiPlanar:
		case OMX_COLOR_FormatYUV422PackedSemiPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT;
			break;

		case OMX_COLOR_FormatYUV440SemiPlanar:
		case OMX_COLOR_FormatYUV440PackedSemiPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT : IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT;
			break;

		case OMX_COLOR_FormatYUV444SemiPlanar:
		case OMX_COLOR_FormatYUV444PackedSemiPlanar:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT : IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT;
			break;

		case OMX_COLOR_FormatL8:
			stream_info->color_format = is_8bit ? IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT : IMX_VPU_API_COLOR_FORMAT_YUV400_10BIT;
			break;

		case OMX_COLOR_FormatYUV420SemiPlanar4x4Tiled:
			stream_info->color_format = (ImxVpuApiColorFormat)(is_8bit ? IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT : IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT);
			break;

		case OMX_COLOR_FormatYUV420SemiPlanar8x4Tiled:
			stream_info->color_format = (ImxVpuApiColorFormat)(is_8bit ? IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT : IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT);
			break;

		case OMX_COLOR_FormatYUV420SemiPlanarP010:
			stream_info->color_format = (ImxVpuApiColorFormat)IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT;
			break;

		default:
			if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_DIVX3)
			{
				IMX_VPU_API_DEBUG("using workaround for bug in DivX 3 codec; it always outputs invalid color format, even though it is always actually semi-planar YUV420 8-bit");
				stream_info->color_format = IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT;
			}
			else
			{
				IMX_VPU_API_ERROR("unrecognized pixel format %#x", hantro_stream_info.format);
				stream_info->color_format = IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT;
				return FALSE;
			}
	}


	/* Determine if the chroma planes are interleaved. */
	switch ((int)(hantro_stream_info.format))
	{
		case OMX_COLOR_FormatYUV420Planar:
		case OMX_COLOR_FormatYUV420PackedPlanar:
		case OMX_COLOR_FormatYUV411Planar:
		case OMX_COLOR_FormatYUV411PackedPlanar:
		case OMX_COLOR_FormatL8:
			break;

		case OMX_COLOR_FormatYUV420SemiPlanar:
		case OMX_COLOR_FormatYUV420PackedSemiPlanar:
		case OMX_COLOR_FormatYUV411SemiPlanar:
		case OMX_COLOR_FormatYUV411PackedSemiPlanar:
		case OMX_COLOR_FormatYUV440SemiPlanar:
		case OMX_COLOR_FormatYUV440PackedSemiPlanar:
		case OMX_COLOR_FormatYUV444SemiPlanar:
		case OMX_COLOR_FormatYUV444PackedSemiPlanar:
			stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES;
			break;

		default:
			break;
	}

	codec_state = decoder->codec->setnoreorder(decoder->codec, (decoder->open_params.flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING) ? OMX_FALSE : OMX_TRUE);
	IMX_VPU_API_DEBUG("setnoreorder() called;  frame reordering: %d  codec state: %s (%d)", !!(decoder->open_params.flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING), codec_state_to_string(codec_state), codec_state);

	stream_info->decoded_frame_framebuffer_metrics.aligned_frame_width = hantro_stream_info.width;
	stream_info->decoded_frame_framebuffer_metrics.aligned_frame_height = hantro_stream_info.height;

	if (hantro_stream_info.crop_available)
	{
		stream_info->decoded_frame_framebuffer_metrics.actual_frame_width = hantro_stream_info.crop_width;
		stream_info->decoded_frame_framebuffer_metrics.actual_frame_height = hantro_stream_info.crop_height;

		/* Hantro stream info stores the actual, non-aligned width/height
		 * in the crop rectangle, even if there is no actual crop rectangle.
		 * For this reason, we consider a crop rectangle to be present only
		 * if at least one of the crop_left and crop_top coordinates are
		 * nonzero. Because, otherwise, we can just use crop_width and
		 * crop_height as the non-aligned width/height without involving
		 * any extra cropping operation. */
		stream_info->has_crop_rectangle = (hantro_stream_info.crop_left != 0) || (hantro_stream_info.crop_top != 0);

		stream_info->crop_left = hantro_stream_info.crop_left;
		stream_info->crop_top = hantro_stream_info.crop_top;
		stream_info->crop_width = hantro_stream_info.crop_width;
		stream_info->crop_height = hantro_stream_info.crop_height;
		IMX_VPU_API_DEBUG("crop rectangle coordinates: left %zu top %zu width %zu height %zu  setting has_crop_rectangle to %d", stream_info->crop_left, stream_info->crop_top, stream_info->crop_width, stream_info->crop_height, stream_info->has_crop_rectangle);
	}
	else
	{
		stream_info->decoded_frame_framebuffer_metrics.actual_frame_width = hantro_stream_info.width;
		stream_info->decoded_frame_framebuffer_metrics.actual_frame_height = hantro_stream_info.height;

		stream_info->has_crop_rectangle = FALSE;
		stream_info->crop_left = 0;
		stream_info->crop_top = 0;
		stream_info->crop_width = hantro_stream_info.width;
		stream_info->crop_height = hantro_stream_info.height;
		IMX_VPU_API_DEBUG("crop rectangle not available, setting whole frame as rectangle instead: left %zu top %zu width %zu height %zu", stream_info->crop_left, stream_info->crop_top, stream_info->crop_width, stream_info->crop_height);
	}

	stream_info->decoded_frame_framebuffer_metrics.y_stride = hantro_stream_info.stride;
	stream_info->decoded_frame_framebuffer_metrics.y_size = hantro_stream_info.stride * hantro_stream_info.sliceheight;

	/* Fill the CbCr values. */
	switch (stream_info->color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT:
			stream_info->decoded_frame_framebuffer_metrics.uv_stride = stream_info->decoded_frame_framebuffer_metrics.y_stride / 2;
			stream_info->decoded_frame_framebuffer_metrics.uv_size = stream_info->decoded_frame_framebuffer_metrics.y_size / 4;
			break;
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT:
			stream_info->decoded_frame_framebuffer_metrics.uv_stride = stream_info->decoded_frame_framebuffer_metrics.y_stride / 2;
			stream_info->decoded_frame_framebuffer_metrics.uv_size = stream_info->decoded_frame_framebuffer_metrics.y_size / 2;
			break;
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT:
			stream_info->decoded_frame_framebuffer_metrics.uv_stride = stream_info->decoded_frame_framebuffer_metrics.y_stride;
			stream_info->decoded_frame_framebuffer_metrics.uv_size = stream_info->decoded_frame_framebuffer_metrics.y_size;
			break;
		case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_YUV400_10BIT:
			stream_info->decoded_frame_framebuffer_metrics.uv_stride = 0;
			stream_info->decoded_frame_framebuffer_metrics.uv_size = 0;
			break;
		default:
			assert(FALSE);
	}

	/* Chrome interleaving means that there is one plane with
	 * both Cb and Cr values interleaved, so it is twice as big. */
	if (stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES)
	{
		stream_info->decoded_frame_framebuffer_metrics.uv_stride *= 2;
		stream_info->decoded_frame_framebuffer_metrics.uv_size *= 2;
	}

	stream_info->decoded_frame_framebuffer_metrics.y_offset = 0;
	stream_info->decoded_frame_framebuffer_metrics.u_offset = stream_info->decoded_frame_framebuffer_metrics.y_size;
	stream_info->decoded_frame_framebuffer_metrics.v_offset = stream_info->decoded_frame_framebuffer_metrics.u_offset + stream_info->decoded_frame_framebuffer_metrics.uv_size;

	/* The Hantro decoder requires some extra space after the
	 * actual framebuffer data. This extra space is included in
	 * the hantro_stream_info.framesize field and cannot be computed
	 * otherwise, so just use that field's value. */
	stream_info->min_fb_pool_framebuffer_size = stream_info->min_output_framebuffer_size = hantro_stream_info.framesize;
	/* Alignment determined by looking at the X170_CHECK_BUS_ADDRESS_AGLINED
	 * macro in the Hantro deccfg.h header. */
	stream_info->fb_pool_framebuffer_alignment = stream_info->output_framebuffer_alignment = 16;

	/* Frame rate is not available from the Hantro decoder. */
	stream_info->frame_rate_numerator = 0;
	stream_info->frame_rate_denominator = 0;

	stream_info->min_num_required_framebuffers = hantro_stream_info.frame_buffers;

	if (hantro_stream_info.interlaced)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_INTERLACED;
	if (hantro_stream_info.bit_depth == 10)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_10BIT;
	if (hantro_stream_info.hdr10_available)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_HDR_METADATA_AVAILABLE;
	if (hantro_stream_info.colour_desc_available)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_COLOR_DESCRIPTION_AVAILABLE;
	if (hantro_stream_info.chroma_loc_info_available)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_LOCATION_OF_CHROMA_INFO_AVAILABLE;

	IMX_VPU_API_DEBUG(
		"stream info:  aligned width/height: %zu/%zu  actual width/height: %zu/%zu  Y/UV stride: %zu/%zu  Y/UV size: %zu/%zu  Y/U/V offsets: %zu/%zu/%zu  sliceheight: %zu  min fb pool framebuffer size: %zu  frame rate: %u/%u  min num required framebuffers: %zu  color format: %s  semi-planar: %d  is interlaced: %d  is 10 bit: %d  has HDR metadata: %d  has color description: %d  has location of chroma info: %d",
		stream_info->decoded_frame_framebuffer_metrics.aligned_frame_width, stream_info->decoded_frame_framebuffer_metrics.aligned_frame_height,
		stream_info->decoded_frame_framebuffer_metrics.actual_frame_width, stream_info->decoded_frame_framebuffer_metrics.actual_frame_height,
		stream_info->decoded_frame_framebuffer_metrics.y_stride, stream_info->decoded_frame_framebuffer_metrics.uv_stride,
		stream_info->decoded_frame_framebuffer_metrics.y_size, stream_info->decoded_frame_framebuffer_metrics.uv_size,
		stream_info->decoded_frame_framebuffer_metrics.y_offset, stream_info->decoded_frame_framebuffer_metrics.u_offset, stream_info->decoded_frame_framebuffer_metrics.v_offset,
		(size_t)(hantro_stream_info.sliceheight),
		stream_info->min_fb_pool_framebuffer_size,
		stream_info->frame_rate_numerator,
		stream_info->frame_rate_denominator,
		stream_info->min_num_required_framebuffers,
		imx_vpu_api_color_format_string(stream_info->color_format),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_INTERLACED),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_10BIT),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_HDR_METADATA_AVAILABLE),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_COLOR_DESCRIPTION_AVAILABLE),
		!!(stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_LOCATION_OF_CHROMA_INFO_AVAILABLE)
	);

	if (stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_HDR_METADATA_AVAILABLE)
	{
		ImxVpuApiDecHDRMetadata *hdr_metadata = &(stream_info->hdr_metadata);
		hdr_metadata->red_primary_x = hantro_stream_info.hdr10_metadata.redPrimary[0];
		hdr_metadata->red_primary_y = hantro_stream_info.hdr10_metadata.redPrimary[1];
		hdr_metadata->green_primary_x = hantro_stream_info.hdr10_metadata.greenPrimary[0];
		hdr_metadata->green_primary_x = hantro_stream_info.hdr10_metadata.greenPrimary[1];
		hdr_metadata->blue_primary_x = hantro_stream_info.hdr10_metadata.bluePrimary[0];
		hdr_metadata->blue_primary_y = hantro_stream_info.hdr10_metadata.bluePrimary[1];
		hdr_metadata->white_point_x = hantro_stream_info.hdr10_metadata.whitePoint[0];
		hdr_metadata->white_point_y = hantro_stream_info.hdr10_metadata.whitePoint[1];
		hdr_metadata->xy_range[0] = 0;
		hdr_metadata->xy_range[1] = 50000;
		hdr_metadata->min_mastering_luminance = hantro_stream_info.hdr10_metadata.minMasteringLuminance;
		hdr_metadata->max_mastering_luminance = hantro_stream_info.hdr10_metadata.maxMasteringLuminance;
		hdr_metadata->max_content_light_level = hantro_stream_info.hdr10_metadata.maxContentLightLevel;
		hdr_metadata->max_frame_average_light_level = hantro_stream_info.hdr10_metadata.maxFrameAverageLightLevel;

#define NORMALIZED_PRIMARY_COORD(COORD) \
			((float)(hdr_metadata->COORD - hdr_metadata->xy_range[0]) / ((float)(hdr_metadata->xy_range[1] - hdr_metadata->xy_range[0])))

		IMX_VPU_API_DEBUG(
			"HDR metadata:"
			"  primaries (x,y coordinates):"
			" red %f,%f (raw: %" PRIu32 ",%" PRIu32 ")"
			" green %f,%f (raw: %" PRIu32 ",%" PRIu32 ")"
			" blue %f,%f (raw: %" PRIu32 ",%" PRIu32 ")"
			"  xy range: %" PRIu32 "-%"PRIu32
			"  min/max mastering luminance: %" PRIu32 "/%" PRIu32
			"  max content light level: %" PRIu32
			"  max frame average light level: %" PRIu32,
			NORMALIZED_PRIMARY_COORD(red_primary_x), NORMALIZED_PRIMARY_COORD(red_primary_y),
			hdr_metadata->red_primary_x, hdr_metadata->red_primary_y,
			NORMALIZED_PRIMARY_COORD(green_primary_x), NORMALIZED_PRIMARY_COORD(green_primary_y),
			hdr_metadata->green_primary_x, hdr_metadata->green_primary_y,
			NORMALIZED_PRIMARY_COORD(blue_primary_x), NORMALIZED_PRIMARY_COORD(blue_primary_y),
			hdr_metadata->blue_primary_x, hdr_metadata->blue_primary_y,
			hdr_metadata->xy_range[0], hdr_metadata->xy_range[1],
			hdr_metadata->min_mastering_luminance, hdr_metadata->max_mastering_luminance,
			hdr_metadata->max_content_light_level,
			hdr_metadata->max_frame_average_light_level
		);

#undef NORMALIZED_PRIMARY_COORD
	}

	if (stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_COLOR_DESCRIPTION_AVAILABLE)
	{
		ImxVpuApiDecColorDescription *color_description = &(stream_info->color_description);
		color_description->color_primaries = hantro_stream_info.colour_primaries;
		color_description->transfer_characteristics = hantro_stream_info.transfer_characteristics;
		color_description->matrix_coefficients = hantro_stream_info.matrix_coeffs;

		IMX_VPU_API_DEBUG(
			"color description:  color primaries: %" PRIu32 "  transfer characteristics: %" PRIu32 "  matrix coefficients: %" PRIu32,
			color_description->color_primaries,
			color_description->transfer_characteristics,
			color_description->matrix_coefficients
		);
	}

	if (stream_info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_LOCATION_OF_CHROMA_INFO_AVAILABLE)
	{
		ImxVpuApiDecLocationOfChromaInfo *location_of_chroma_info = &(stream_info->location_of_chroma_info);
		location_of_chroma_info->chroma_sample_loc_type_top_field = hantro_stream_info.chroma_sample_loc_type_top_field;
		location_of_chroma_info->chroma_sample_loc_type_bottom_field = hantro_stream_info.chroma_sample_loc_type_bottom_field;

		IMX_VPU_API_DEBUG(
			"location of chroma info:  loc type top field: %" PRIu32 "  loc type bottom field: %" PRIu32,
			location_of_chroma_info->chroma_sample_loc_type_top_field,
			location_of_chroma_info->chroma_sample_loc_type_bottom_field
		);
	}

	return TRUE;
}

static ImxVpuApiColorFormat const jpeg_supported_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT,
	IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT,
	(ImxVpuApiColorFormat)IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT,
	(ImxVpuApiColorFormat)IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT
};

static ImxVpuApiColorFormat const g1_supported_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	(ImxVpuApiColorFormat)IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT
};

static ImxVpuApiColorFormat const g2_supported_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	(ImxVpuApiColorFormat)IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT,
	(ImxVpuApiColorFormat)IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT
};

static ImxVpuApiCompressionFormat const supported_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_H264,
	IMX_VPU_API_COMPRESSION_FORMAT_H265,
	IMX_VPU_API_COMPRESSION_FORMAT_VP8,
	IMX_VPU_API_COMPRESSION_FORMAT_VP9,

	/* The Hantro decoder on the i.MX8M Mini does not support these extra formats. */
#ifndef IMXVPUAPI_IMX8_SOC_TYPE_MX8MM
	IMX_VPU_API_COMPRESSION_FORMAT_JPEG,
	IMX_VPU_API_COMPRESSION_FORMAT_WEBP,
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG2,
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG4,
	IMX_VPU_API_COMPRESSION_FORMAT_H263,
	IMX_VPU_API_COMPRESSION_FORMAT_WMV3,
	IMX_VPU_API_COMPRESSION_FORMAT_WVC1,
	IMX_VPU_API_COMPRESSION_FORMAT_VP6,
#if 0
	/* This one is disabled even though the Hantro decoder can handle VP7.
	 * That's because there is no VP7 interface in imx-vpu-hantro/ openmax_il.
	 * There are interfaces for VP8 and WebM, and these set an internal
	 * identifier (VP8DEC_VP8 and VP8DEC_WEBM respectively). There's also
	 * VP8DEC_VP7, so in theory a codec_vp7.h/.c interface could also exist,
	 * but currently it doesn't. */
	IMX_VPU_API_COMPRESSION_FORMAT_VP7,
#endif
	IMX_VPU_API_COMPRESSION_FORMAT_AVS,
	/* TODO: RealVideo integration is not yet working. Either, the slice
	 * info parsing code is wrong in imx_vpu_api_dec_preprocess_input_data(),
	 * or something is off with the extra header data that gets scanned in
	 * imx_vpu_api_dec_open. */
#if 0
	IMX_VPU_API_COMPRESSION_FORMAT_RV30,
	IMX_VPU_API_COMPRESSION_FORMAT_RV40,
#endif
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX3,
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX4,
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX5,
	IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK
#endif
};

static ImxVpuApiDecGlobalInfo const global_info = {
	.flags = IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER | IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED | IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_HANTRO,
	.min_required_stream_buffer_size = VPU_DEC_MIN_REQUIRED_STREAM_BUFFER_SIZE,
	.required_stream_buffer_physaddr_alignment = STREAM_BUFFER_PHYSADDR_ALIGNMENT,
	.required_stream_buffer_size_alignment = STREAM_BUFFER_SIZE_ALIGNMENT,
	.supported_compression_formats = supported_compression_formats,
	.num_supported_compression_formats = sizeof(supported_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};

ImxVpuApiDecGlobalInfo const * imx_vpu_api_dec_get_global_info(void)
{
	return &global_info;
}


static ImxVpuApiCompressionFormatSupportDetails const default_g1_compression_format_support_details = {
	.min_width = 8, .max_width = 4096,
	.min_height = 8, .max_height = 4096,
	.supported_color_formats = g1_supported_color_formats,
	.num_supported_color_formats = sizeof(g1_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
};

static ImxVpuApiCompressionFormatSupportDetails const jpeg_compression_format_support_details = {
	.min_width = 8, .max_width = 4096,
	.min_height = 8, .max_height = 4096,
	.supported_color_formats = jpeg_supported_color_formats,
	.num_supported_color_formats = sizeof(jpeg_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
};

static ImxVpuApiH264SupportDetails const h264_support_details = {
	.parent = {
		.min_width = 8, .max_width = 4096,
		.min_height = 8, .max_height = 4096,
		.supported_color_formats = g1_supported_color_formats,
		.num_supported_color_formats = sizeof(g1_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
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

	.flags = IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED
};

static ImxVpuApiH265SupportDetails const h265_support_details = {
	.parent = {
		.min_width = 8, .max_width = 4096,
		.min_height = 8, .max_height = 2304,
		.supported_color_formats = g2_supported_color_formats,
		.num_supported_color_formats = sizeof(g2_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
	},

	.max_main_profile_level = IMX_VPU_API_H265_LEVEL_5_1,
	.max_main10_profile_level = IMX_VPU_API_H265_LEVEL_5_1,

	.flags = IMX_VPU_API_H265_FLAG_ACCESS_UNITS_SUPPORTED
};

static ImxVpuApiVP8SupportDetails const vp8_support_details = {
	.parent = {
		.min_width = 8, .max_width = 4096,
		.min_height = 8, .max_height = 2304,
		.supported_color_formats = jpeg_supported_color_formats,
		.num_supported_color_formats = sizeof(jpeg_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
	},

	.supported_profiles = (1 << IMX_VPU_API_VP8_PROFILE_0)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_1)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_2)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_3)
};

static ImxVpuApiVP9SupportDetails const vp9_support_details = {
	.parent = {
		.min_width = 8, .max_width = 4096,
		.min_height = 8, .max_height = 2304,
		.supported_color_formats = g2_supported_color_formats,
		.num_supported_color_formats = sizeof(g2_supported_color_formats) / sizeof(ImxVpuApiColorFormat)
	},

#ifdef IMXVPUAPI_IMX8_SOC_TYPE_MX8MM
	.supported_profiles = (1 << IMX_VPU_API_VP9_PROFILE_0)
#else
	.supported_profiles = (1 << IMX_VPU_API_VP9_PROFILE_0) | (1 << IMX_VPU_API_VP9_PROFILE_2)
#endif
};

ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_dec_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&h264_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_H265:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&h265_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&vp8_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_VP9:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&vp9_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			return &jpeg_compression_format_support_details;

		case IMX_VPU_API_COMPRESSION_FORMAT_WEBP:
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG2:
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
		case IMX_VPU_API_COMPRESSION_FORMAT_H263:
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		case IMX_VPU_API_COMPRESSION_FORMAT_VP6:
		case IMX_VPU_API_COMPRESSION_FORMAT_VP7:
		case IMX_VPU_API_COMPRESSION_FORMAT_AVS:
		case IMX_VPU_API_COMPRESSION_FORMAT_RV30:
		case IMX_VPU_API_COMPRESSION_FORMAT_RV40:
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX3:
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX4:
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX5:
		case IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK:
			return &default_g1_compression_format_support_details;

		default:
			return NULL;
	}

	return NULL;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_open(ImxVpuApiDecoder **decoder, ImxVpuApiDecOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	int err;
	CODEC_STATE codec_state;
	ImxVpuApiDecReturnCodes ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	OMX_VIDEO_PARAM_CONFIGTYPE codec_config = { 0 };
	PP_ARGS post_processor_args = { 0 };

	assert(decoder != NULL);
	assert(open_params != NULL);
	assert(stream_buffer != NULL);


	/* Check that the allocated stream buffer is big enough */
	{
		size_t stream_buffer_size = imx_dma_buffer_get_size(stream_buffer);
		if (stream_buffer_size < VPU_DEC_MIN_REQUIRED_STREAM_BUFFER_SIZE) 
		{
			IMX_VPU_API_ERROR("stream buffer size is %zu bytes; need at least %zu bytes", stream_buffer_size, (size_t)VPU_DEC_MIN_REQUIRED_STREAM_BUFFER_SIZE);
			return IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
		}
	}


	/* Verify extra header data */
	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		{
			if (open_params->extra_header_data == NULL)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data, but none has been set");
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
			}

			if (open_params->extra_header_data_size < 4)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data size of 4 bytes, got %u byte(s)", open_params->extra_header_data_size);
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		{
			if (open_params->extra_header_data == NULL)
			{
				IMX_VPU_API_ERROR("WVC1 input expects extra header data, but none has been set");
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
			}

			if (open_params->extra_header_data_size < 2)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data size of at least 2 bytes, got %u byte(s)", open_params->extra_header_data_size);
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
			}

			break;
		}

		default:
			break;
	}


	/* Allocate decoder instance */
	*decoder = malloc(sizeof(ImxVpuApiDecoder));
	assert((*decoder) != NULL);


	/* Set default decoder values */
	memset(*decoder, 0, sizeof(ImxVpuApiDecoder));


	/* Get DWL instance */
	{
		struct DWLInitParam dwl_init_param = { 0 };

		/* This essentially selects between the G1 and G2 cores.
		 * However, there is no API to specify G1 or G2. Instead,
		 * selection is done by choosing either HEVC or H264 as
		 * "client type". */
		switch (open_params->compression_format)
		{
			case IMX_VPU_API_COMPRESSION_FORMAT_H265:
			case IMX_VPU_API_COMPRESSION_FORMAT_VP9:
				dwl_init_param.client_type = DWL_CLIENT_TYPE_HEVC_DEC;
				break;
			default:
				dwl_init_param.client_type = DWL_CLIENT_TYPE_H264_DEC;
		}

		(*decoder)->dwl_instance = DWLInit(&dwl_init_param);
		if ((*decoder)->dwl_instance == NULL)
		{
			IMX_VPU_API_ERROR("initializing DWL instance failed");
			ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			goto cleanup;
		}
	}


	/* Map the stream buffer. We need to keep it mapped always so we can
	 * keep updating it. It is mapped as readwrite so we can shift data
	 * inside it later with memmove() if necessary.
	 * Mapping this with IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC since
	 * the stream buffer stays mapped until the decoder is closed, and
	 * we do copy encoded data into the stream buffer. Also see the
	 * imx_dma_buffer_start_sync_session() / imx_dma_buffer_stop_sync_session()
	 * calls in imx_vpu_api_dec_push_encoded_frame(). */
	(*decoder)->stream_buffer_virtual_address = imx_dma_buffer_map(stream_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE | IMX_DMA_BUFFER_MAPPING_FLAG_READ | IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC, &err);
	if ((*decoder)->stream_buffer_virtual_address == NULL)
	{
			IMX_VPU_API_ERROR("mapping stream buffer to virtual address space failed: %s (%d)", strerror(err), err);
			ret = IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
	}

	(*decoder)->stream_buffer_physical_address = imx_dma_buffer_get_physical_address(stream_buffer);
	(*decoder)->stream_buffer_read_offset = 0;
	(*decoder)->stream_buffer_write_offset = 0;
	(*decoder)->stream_buffer_size = imx_dma_buffer_get_size(stream_buffer);
	(*decoder)->stream_buffer = stream_buffer;

	(*decoder)->open_params = *open_params;

	(*decoder)->last_pushed_frame_entry_index = INVALID_FRAME_ENTRY_INDEX;
	(*decoder)->decoded_frame_fb_entry_index = INVALID_FRAME_ENTRY_INDEX;
	(*decoder)->decoded_frame_entry_index = INVALID_FRAME_ENTRY_INDEX;

	(*decoder)->skipped_frame_context = NULL;
	(*decoder)->skipped_frame_pts = 0;
	(*decoder)->skipped_frame_dts = 0;

	(*decoder)->skip_invisible_frames = FALSE;
	(*decoder)->use_endofstream_function = TRUE;
	(*decoder)->decoded_frame_reported = FALSE;
	(*decoder)->encoded_data_available = FALSE;


	/* Set up the codec */

	/* DPB = decoded picture buffer */
	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_VP9:
		case IMX_VPU_API_COMPRESSION_FORMAT_H265:
			codec_config.g2_conf.bEnableTiled = !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_TILED_OUTPUT);
			codec_config.g2_conf.ePixelFormat = (open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_10BIT_DECODING) ? OMX_VIDEO_G2PixelFormat_Default : OMX_VIDEO_G2PixelFormat_8bit;
			codec_config.g2_conf.bEnableRFC = OMX_FALSE;
			codec_config.g2_conf.bEnableRingBuffer = OMX_FALSE;
			codec_config.g2_conf.bEnableFetchOnePic = OMX_TRUE;
			codec_config.g2_conf.nGuardSize = 0;
			codec_config.g2_conf.bEnableAdaptiveBuffers = OMX_FALSE;
			codec_config.g2_conf.bEnableSecureMode = OMX_FALSE;
			break;

		default:
			codec_config.g1_conf.bEnableTiled = !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_TILED_OUTPUT);
			codec_config.g1_conf.bAllowFieldDBP = OMX_FALSE;
			codec_config.g1_conf.nGuardSize = 0;
			codec_config.g1_conf.bEnableAdaptiveBuffers = OMX_FALSE;
			codec_config.g1_conf.bEnableSecureMode = OMX_FALSE;
			break;
	}

	(*decoder)->ring_buffer_mode = FALSE;

	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_jpeg(OMX_TRUE);
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_WEBP:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_webp((*decoder)->dwl_instance);
			(*decoder)->single_frame_decoding = TRUE;
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG2:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg2((*decoder)->dwl_instance, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_MPEG4, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H263:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_H263, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
		{
			(*decoder)->codec = HantroHwDecOmx_decoder_create_h264((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MVC), &(codec_config.g1_conf));
			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_H265:
#if 0
			if (open_params->use_secure_mode)
			{
				codec_config.g2_conf.bEnableRingBuffer = OMX_TRUE;
				(*decoder)->ring_buffer_mode = TRUE;
			}
#endif
			(*decoder)->codec = HantroHwDecOmx_decoder_create_hevc((*decoder)->dwl_instance, &(codec_config.g2_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_vc1((*decoder)->dwl_instance, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_VP6:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_vp6((*decoder)->dwl_instance, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_VP7:
		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
			(*decoder)->skip_invisible_frames = TRUE;
			(*decoder)->use_endofstream_function = FALSE;
			(*decoder)->codec = HantroHwDecOmx_decoder_create_vp8((*decoder)->dwl_instance, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_VP9:
			(*decoder)->skip_invisible_frames = TRUE;
			(*decoder)->use_endofstream_function = FALSE;
			(*decoder)->codec = HantroHwDecOmx_decoder_create_vp9((*decoder)->dwl_instance, &(codec_config.g2_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_AVS:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_avs((*decoder)->dwl_instance, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_RV30:
		case IMX_VPU_API_COMPRESSION_FORMAT_RV40:
		{
			uint8_t const *rv8_info = open_params->extra_header_data;
			size_t rv8_info_numbytesread = 0;
			OMX_U32 frame_sizes[18];
			OMX_U32 num_frame_sizes = 0;
			OMX_U32 frame_code_lengths[9] = { 0, 1, 1, 2, 2, 3, 3, 3, 3 };
			OMX_BOOL is_rv8 = (open_params->compression_format == IMX_VPU_API_COMPRESSION_FORMAT_RV30) ? OMX_TRUE : OMX_FALSE;

			if ((open_params->extra_header_data == NULL) || (open_params->extra_header_data_size == 0))
			{
				IMX_VPU_API_ERROR("no RealVideo extra header data set");
				ret = IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
				goto cleanup;
			}

			if (is_rv8)
			{
				OMX_U32 j;
				num_frame_sizes = 1 + (rv8_info[1] & 0x7);
				frame_sizes[0] = open_params->frame_width;
				frame_sizes[1] = open_params->frame_height;
				rv8_info += 8;

				IMX_VPU_API_DEBUG("this is a RealVideo 8 stream; extra data:  num frame sizes: %u  primary frame size width/height: %u/%u", (unsigned int)num_frame_sizes, (unsigned int)(frame_sizes[0]), (unsigned int)(frame_sizes[1]));

				for (j = 1; j < num_frame_sizes; ++j)
				{
					OMX_U32 width, height;

					if ((rv8_info_numbytesread + 2) > open_params->extra_header_data_size)
					{
						IMX_VPU_API_ERROR("RealVideo extra header data is insufficient and/or invalid");
						ret = IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
						goto cleanup;
					}

					width = ((OMX_U32)(rv8_info[0])) << 2u;
					height = ((OMX_U32)(rv8_info[1])) << 2u;
					frame_sizes[j * 2 + 0] = width;
					frame_sizes[j * 2 + 1] = height;
					rv8_info += 2;

					IMX_VPU_API_DEBUG("additional frame size #%u width/height:  %u/%u", (unsigned int)j, (unsigned int)(width), (unsigned int)(height));
				}
			}

			(*decoder)->codec = HantroHwDecOmx_decoder_create_rv(
				(*decoder)->dwl_instance,
				is_rv8,
				frame_code_lengths[num_frame_sizes],
				frame_sizes,
				open_params->frame_width,
				open_params->frame_height,
				&(codec_config.g1_conf)
			);

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_SORENSON, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX3:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_CUSTOM_1_3, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX4:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_CUSTOM_1, &(codec_config.g1_conf));
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX5:
			(*decoder)->codec = HantroHwDecOmx_decoder_create_mpeg4((*decoder)->dwl_instance, !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING), MPEG4FORMAT_CUSTOM_1, &(codec_config.g1_conf));
			break;

		default:
			IMX_VPU_API_ERROR("unknown compression format");
			ret = IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT;
			goto cleanup;
	}

	if ((*decoder)->codec == NULL)
	{
		IMX_VPU_API_ERROR("could not create codec");
		ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto cleanup;
	}

	/* Set post processor arguments */
	codec_state = (*decoder)->codec->setppargs((*decoder)->codec, &post_processor_args);
	if (codec_state != CODEC_OK)
		IMX_VPU_API_WARNING("could not set post processor arguments: %s", codec_state_to_string(codec_state));


	/* Finish & cleanup (in case of error) */
finish:
	if (ret == IMX_VPU_API_DEC_RETURN_CODE_OK)
		IMX_VPU_API_DEBUG("successfully opened decoder");

	return ret;

cleanup:
	if ((*decoder)->dwl_instance != NULL)
		DWLRelease((*decoder)->dwl_instance);

	if ((*decoder) != NULL)
	{
		if ((*decoder)->stream_buffer_virtual_address != NULL)
			imx_dma_buffer_unmap((*decoder)->stream_buffer);
		free(*decoder);
		*decoder = NULL;
	}

	goto finish;
}


void imx_vpu_api_dec_close(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);

	IMX_VPU_API_DEBUG("closing decoder");

		if (decoder->stream_buffer_virtual_address != NULL)
			imx_dma_buffer_unmap(decoder->stream_buffer);

	if (decoder->codec != NULL)
		decoder->codec->destroy(decoder->codec);

	if (decoder->dwl_instance != NULL)
		DWLRelease(decoder->dwl_instance);

	imx_vpu_api_dec_clear_added_framebuffers(decoder);
	imx_vpu_api_dec_clear_frame_entries(decoder);

	free(decoder);
}


ImxVpuApiDecStreamInfo const * imx_vpu_api_dec_get_stream_info(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	return &(decoder->stream_info);
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_add_framebuffers_to_pool(ImxVpuApiDecoder *decoder, ImxDmaBuffer **fb_dma_buffers, void **fb_contexts, size_t num_framebuffers)
{
	ImxVpuApiDecReturnCodes ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	int err;
	size_t i, num_mapped_fbs;
	size_t new_fb_entries_base_index;

	assert(decoder != NULL);
	assert(decoder->codec != NULL);
	assert(fb_dma_buffers != NULL);
	assert(num_framebuffers >= 1);

	num_mapped_fbs = 0;

	if (decoder->num_framebuffers_to_be_added == 0)
	{
		IMX_VPU_API_ERROR("no framebuffers need to be added");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	if (num_framebuffers < decoder->num_framebuffers_to_be_added)
	{
		IMX_VPU_API_ERROR("decoder needs %zu framebuffers to be added, got %zu", decoder->num_framebuffers_to_be_added, num_framebuffers);
		return IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS;
	}

	new_fb_entries_base_index = imx_vpu_api_dec_add_framebuffer_entries(decoder, num_framebuffers);
	assert(new_fb_entries_base_index != INVALID_FRAME_ENTRY_INDEX);

	for (i = 0; i < num_framebuffers; ++i)
	{
		imx_physical_address_t physical_address;
		void *virtual_address;
		ImxDmaBuffer *dma_buffer;
		size_t dma_buffer_size;
		FramebufferEntry *fb_entry;
		CODEC_STATE codec_state;
		BUFFER buffer = { 0 };
		size_t new_fb_entry_index = new_fb_entries_base_index + i;

		dma_buffer = fb_dma_buffers[i];
		fb_entry = &(decoder->framebuffer_entries[new_fb_entry_index]);

		virtual_address = imx_dma_buffer_map(dma_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE | IMX_DMA_BUFFER_MAPPING_FLAG_READ, &err);
		if (virtual_address == NULL)
		{
			IMX_VPU_API_ERROR("mapping stream buffer to virtual address space failed: %s (%d)", strerror(err), err);
			ret = IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
		}

		++num_mapped_fbs;

		physical_address = imx_dma_buffer_get_physical_address(dma_buffer);
		dma_buffer_size = imx_dma_buffer_get_size(dma_buffer);

		IMX_VPU_API_DEBUG("adding framebuffer entry with index %zu:  virtual address %p  physical address %" IMX_PHYSICAL_ADDRESS_FORMAT "  size %zu", new_fb_entry_index, virtual_address, physical_address, dma_buffer_size);

		fb_entry->mapped_virtual_address = virtual_address;
		fb_entry->physical_address = physical_address;
		fb_entry->fb_dma_buffer = dma_buffer;
		if (fb_contexts != NULL)
			fb_entry->fb_context = fb_contexts[i];

		buffer.bus_data = virtual_address;
		buffer.bus_address = (OSAL_BUS_WIDTH)physical_address;
		buffer.allocsize = dma_buffer_size;

		codec_state = decoder->codec->setframebuffer(decoder->codec, &buffer, num_framebuffers);
		switch (codec_state)
		{
			case CODEC_OK:
			case CODEC_NEED_MORE:
				break;

			case CODEC_ERROR_INVALID_ARGUMENT:
				IMX_VPU_API_ERROR("invalid arguments when adding framebuffer");
				ret = IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
				goto cleanup;

			case CODEC_ERROR_BUFFER_SIZE:
				IMX_VPU_API_ERROR("invalid buffer size %zu specified when adding framebuffer", (size_t)(buffer.allocsize));
				ret = IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS;
				goto cleanup;

			default:
				IMX_VPU_API_ERROR("could not add framebuffer: %s", codec_state_to_string(codec_state));
				ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
				goto cleanup;
		}
	}

finish:
	decoder->num_framebuffers_to_be_added = 0;
	return ret;

cleanup:
	for (i = 0; i < num_mapped_fbs; ++i)
	{
		ImxDmaBuffer *dma_buffer = fb_dma_buffers[i];
		imx_dma_buffer_unmap(dma_buffer);
	}

	goto finish;
}


void imx_vpu_api_dec_enable_drain_mode(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	decoder->drain_mode_enabled = TRUE;
}


int imx_vpu_api_dec_is_drain_mode_enabled(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	return decoder->drain_mode_enabled;
}


void imx_vpu_api_dec_flush(ImxVpuApiDecoder *decoder)
{
	CODEC_STATE codec_state;
	BOOL do_loop = TRUE;
	FRAME frame = { 0 };

	assert(decoder != NULL);
	assert(decoder->codec != NULL);

	if (decoder->framebuffer_entries == NULL)
	{
		IMX_VPU_API_DEBUG("attempted to flush, but there are no framebuffers in the pool; ignoring call");
		return;
	}

	decoder->stream_buffer_read_offset = 0;
	decoder->stream_buffer_write_offset = 0;
	decoder->stream_buffer_fill_level = 0;

	decoder->has_new_stream_info = FALSE;

	decoder->main_header_pushed = FALSE;

	decoder->last_pushed_frame_entry_index = INVALID_FRAME_ENTRY_INDEX;
	decoder->decoded_frame_fb_entry_index = INVALID_FRAME_ENTRY_INDEX;
	decoder->decoded_frame_entry_index = INVALID_FRAME_ENTRY_INDEX;

	decoder->skipped_frame_context = NULL;
	decoder->skipped_frame_pts = 0;
	decoder->skipped_frame_dts = 0;

	decoder->decoded_frame_reported = FALSE;
	decoder->encoded_data_available = FALSE;

	decoder->end_of_stream_reached = FALSE;
	decoder->drain_mode_enabled = FALSE;

	IMX_VPU_API_DEBUG("flushing decoder");

	while (do_loop)
	{
		codec_state = decoder->codec->getframe(decoder->codec, &frame, decoder->drain_mode_enabled ? OMX_TRUE : OMX_FALSE);
		IMX_VPU_API_DEBUG("attempting to retrieve frame (to discard it) during flush; codec state: %s (%d)", codec_state_to_string(codec_state), codec_state);
		switch (codec_state)
		{
			case CODEC_HAS_FRAME:
			{
				BUFFER buffer = { 0 };

				buffer.bus_data = frame.fb_bus_data;
				buffer.bus_address = frame.fb_bus_address;

				codec_state = decoder->codec->pictureconsumed(decoder->codec, &buffer);
				IMX_VPU_API_DEBUG("discarded picture during flush;  virtual address %p  physical address %" IMX_PHYSICAL_ADDRESS_FORMAT "  codec state: %s (%d)", (void*)(frame.fb_bus_data), (imx_physical_address_t)(frame.fb_bus_address), codec_state_to_string(codec_state), codec_state);

				break;
			}

			default:
				do_loop = FALSE;
				break;
		}
	}

	codec_state = decoder->codec->abort(decoder->codec);
	if (codec_state != CODEC_OK)
		IMX_VPU_API_ERROR("error while calling abort() during flush: %s (%d)", codec_state_to_string(codec_state), codec_state);
	decoder->codec->abortafter(decoder->codec);
	if (codec_state != CODEC_OK)
		IMX_VPU_API_ERROR("error while calling abortafter() during flush: %s (%d)", codec_state_to_string(codec_state), codec_state);

	IMX_VPU_API_DEBUG("flushed decoder");
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_push_encoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	FrameEntry *frame_entry;

	assert(decoder != NULL);
	assert(decoder->codec != NULL);
	assert(encoded_frame != NULL);

	if (decoder->drain_mode_enabled)
	{
		IMX_VPU_API_ERROR("tried to push an encoded frame after drain mode was enabled");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	if (decoder->encoded_data_available)
	{
		IMX_VPU_API_ERROR("tried to push an encoded frame before previously pushed frame was fully processed");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	/* Begin synced access since we have to copy the encoded
	 * data into the stream buffer. */
	imx_dma_buffer_start_sync_session(decoder->stream_buffer);

	/* Process input data first to make sure any headers are
	 * inserted and any necessary parsing is done before the
	 * main frame. */
	imx_vpu_api_dec_preprocess_input_data(decoder, decoder->open_params.extra_header_data, decoder->open_params.extra_header_data_size, encoded_frame->data, encoded_frame->data_size);

		/* Handle main frame data */
	imx_vpu_api_dec_push_input_data(decoder, encoded_frame->data + decoder->encoded_frame_offset, encoded_frame->data_size - decoder->encoded_frame_offset);

	decoder->last_pushed_frame_entry_index = imx_vpu_api_get_free_frame_entry_index(decoder);
	assert(decoder->last_pushed_frame_entry_index != INVALID_FRAME_ENTRY_INDEX);

	IMX_VPU_API_LOG("pushed frame with context %p PTS %" PRIu64 " DTS %" PRIu64 " frame entry index %zu and %zu bytes of main data", encoded_frame->context, encoded_frame->pts, encoded_frame->dts, decoder->last_pushed_frame_entry_index, encoded_frame->data_size);

	frame_entry = &(decoder->frame_entries[decoder->last_pushed_frame_entry_index]);
	frame_entry->occupied = TRUE;
	frame_entry->context = encoded_frame->context;
	frame_entry->pts = encoded_frame->pts;
	frame_entry->dts = encoded_frame->dts;

	decoder->encoded_data_available = TRUE;

	/* Clear end_of_stream_reached flag since feeding in data
	 * means that we are no longer at the end of stream. */
	decoder->end_of_stream_reached = FALSE;

	imx_dma_buffer_stop_sync_session(decoder->stream_buffer);

	return IMX_VPU_API_DEC_RETURN_CODE_OK;
}


void imx_vpu_api_dec_set_output_frame_dma_buffer(ImxVpuApiDecoder *decoder, ImxDmaBuffer *output_frame_dma_buffer, void *fb_context)
{
	IMX_VPU_API_UNUSED_PARAM(decoder);
	IMX_VPU_API_UNUSED_PARAM(output_frame_dma_buffer);
	IMX_VPU_API_UNUSED_PARAM(fb_context);
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_decode(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code)
{
	CODEC_STATE codec_state;
	OMX_S32 scan_ret;
	ImxVpuApiDecReturnCodes ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	OMX_U32 first_offset_ptr = 0, last_offset_ptr = 0;
	OMX_U32 num_used_input_bytes = 0;
	FRAME frame = { 0 };
	STREAM_BUFFER stream_buffer = { 0 };
	BOOL do_loop = TRUE;

	assert(decoder != NULL);
	assert(output_code != NULL);

	if (decoder->decoded_frame_entry_index != INVALID_FRAME_ENTRY_INDEX)
	{
		IMX_VPU_API_ERROR("there is a decoded frame to be retrieved, but imx_vpu_api_dec_get_decoded_frame() wasn't called");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	if (decoder->end_of_stream_reached)
	{
		IMX_VPU_API_LOG("end of stream already reached; not doing anything");
		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
		return IMX_VPU_API_DEC_RETURN_CODE_OK;
	}

	if ((decoder->stream_info.min_num_required_framebuffers > 0) && decoder->frame_entries == NULL)
	{
		IMX_VPU_API_LOG("no framebuffers have been added to the pool");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	codec_state = decoder->codec->getframe(decoder->codec, &frame, decoder->drain_mode_enabled ? OMX_TRUE : OMX_FALSE);
	IMX_VPU_API_LOG("decoding frame(s);  drain mode enabled: %d  codec state %s (%d)", decoder->drain_mode_enabled, codec_state_to_string(codec_state), codec_state);

	switch (codec_state)
	{
		case CODEC_HAS_FRAME:
		{
			decoder->decoded_frame_reported = FALSE;

			imx_physical_address_t physical_address = frame.fb_bus_address;
			if (physical_address == 0)
			{
				/* This can happen with interlaced data for example. This
				 * return value in combination with physical & virtual address
				 * zero occurs when the codec decides to not re-send a one-field
				 * frame twice. Observed in imx-vpu-hantro codec_mpeg2.c
				 * decoder_getframe_mpeg2(). */
				IMX_VPU_API_LOG("got a CODEC_HAS_FRAME return value, but physical address is 0, meaning this frame is to be skipped");
				break;
			}

			/* We use the physical address as the key for finding
			 * matching framebuffer entries later. */
			size_t index = imx_vpu_api_dec_find_framebuffer_entry_index(decoder, physical_address);
			if (index == INVALID_FRAME_ENTRY_INDEX)
			{
				decoder->decoded_frame_fb_entry_index = INVALID_FRAME_ENTRY_INDEX;
				IMX_VPU_API_ERROR("could not find index for an entry with physical address %" IMX_PHYSICAL_ADDRESS_FORMAT, physical_address);
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}

			decoder->decoded_frame_fb_entry_index = index;
			/* The header indicates that there are two nPicId integers for
			 * top/bottom interlaced fields. But in practice, those two
			 * integers have never been found to differ, so we ignore the
			 * second one and just use the first one. */
			decoder->decoded_frame_entry_index = frame.outBufPrivate.nPicId[0];
			IMX_VPU_API_LOG("found frame entry at index %zu and framebuffer entry at index %zu for decoded frame with physical address %" IMX_PHYSICAL_ADDRESS_FORMAT, decoder->decoded_frame_entry_index, decoder->decoded_frame_fb_entry_index, physical_address);

			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE;

			if (decoder->single_frame_decoding)
			{
				IMX_VPU_API_DEBUG("single frame decoding is enabled, and a frame was just decoded; setting EOS flag");
				decoder->end_of_stream_reached = TRUE;
				decoder->drain_mode_enabled = FALSE;
			}

			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}

		case CODEC_END_OF_STREAM:
			IMX_VPU_API_DEBUG("video codec reports end of stream");
			decoder->end_of_stream_reached = TRUE;
			decoder->drain_mode_enabled = FALSE;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
			return IMX_VPU_API_DEC_RETURN_CODE_OK;

		/* These are not errors, we just have no frame to output. */
		case CODEC_OK:
		case CODEC_ABORTED:
		case CODEC_FLUSHED:
			/* As explained in imx_vpu_api_dec_decode() CODEC_HAS_FRAME
			 * handling, if decode() returned CODEC_HAS_FRAME but
			 * getframe() didn't, then we are dealing with an internal
			 * frame. These will never be output. We have to skip these. */
			if (decoder->decoded_frame_reported && decoder->skip_invisible_frames)
			{
				FrameEntry *frame_entry;

				decoder->decoded_frame_reported = FALSE;
				decoder->encoded_data_available = FALSE;

				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;

				if ((decoder->last_pushed_frame_entry_index != INVALID_FRAME_ENTRY_INDEX) || (decoder->last_pushed_frame_entry_index < decoder->num_frame_entries))
				{
					frame_entry = &(decoder->frame_entries[decoder->last_pushed_frame_entry_index]);
					decoder->skipped_frame_reason = IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME;
					decoder->skipped_frame_context = frame_entry->context;
					decoder->skipped_frame_pts = frame_entry->pts;
					decoder->skipped_frame_dts = frame_entry->dts;
					frame_entry->occupied = FALSE;

					IMX_VPU_API_LOG("frame at entry index %zu with context %p PTS %" PRIu64 " DTS %" PRIu64 " got skipped because it is an invisible internal frame", decoder->last_pushed_frame_entry_index, frame_entry->context, frame_entry->pts, frame_entry->dts);

					return IMX_VPU_API_DEC_RETURN_CODE_OK;
				}
				else
				{
					IMX_VPU_API_ERROR("could not get context for skipped invisible internal frame; last pushed frame entry index is invalid (%zu)", decoder->last_pushed_frame_entry_index);
					return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
				}
				decoder->decoded_frame_reported = FALSE;
			}
			else
				IMX_VPU_API_LOG("VPU has no decoded frames to output");

			break;

		default:
			IMX_VPU_API_ERROR("error while trying to retrieve frame:  codec state %s (%d)", codec_state_to_string(codec_state), codec_state);
			break;
	}

	*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE;

	if (decoder->stream_buffer_fill_level == 0)
	{
		if (decoder->drain_mode_enabled)
		{
			/* We cannot call endofstream() for all codecs. Particularly
			 * the VP9 codec's endofstream() function has bugs and is not
			 * really needed. So, only call it if necessary. */
			if (decoder->use_endofstream_function)
			{
				/* endofstream() initiates the actual drain. For this
				 * reason, we do not yet report IMX_VPU_API_DEC_OUTPUT_CODE_EOS
				 * here - that will happen in imx_vpu_api_dec_decode(). */
				codec_state = decoder->codec->endofstream(decoder->codec);
				IMX_VPU_API_DEBUG("endofstream(): %s (%d)", codec_state_to_string(codec_state), codec_state);
				return (codec_state == CODEC_OK) ? IMX_VPU_API_DEC_RETURN_CODE_OK : IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}
			else
			{
				IMX_VPU_API_DEBUG("stream buffer empty, endofstream() is not to be called, and drain mode is enabled; we are at the end of stream");
				decoder->end_of_stream_reached = TRUE;
				decoder->drain_mode_enabled = FALSE;
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
				return IMX_VPU_API_DEC_RETURN_CODE_OK;
			}
		}
		else
		{
			decoder->encoded_data_available = FALSE;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}
	}

	do
	{
		IMX_VPU_API_LOG(
			"scanning for frames in the stream buffer; read offset %zu write offset %zu fill level %zu",
			decoder->stream_buffer_read_offset,
			decoder->stream_buffer_write_offset,
			decoder->stream_buffer_fill_level
		);

		stream_buffer.bus_data = decoder->stream_buffer_virtual_address + decoder->stream_buffer_read_offset;
		stream_buffer.bus_address = (OSAL_BUS_WIDTH)(decoder->stream_buffer_physical_address + decoder->stream_buffer_read_offset);
		stream_buffer.streamlen = decoder->stream_buffer_fill_level;
		stream_buffer.allocsize = decoder->stream_buffer_size;

		scan_ret = decoder->codec->scanframe(decoder->codec, &stream_buffer, &first_offset_ptr, &last_offset_ptr);
		if ((scan_ret == -1) || (first_offset_ptr == last_offset_ptr))
		{
			IMX_VPU_API_LOG("scanning for frames in stream buffer found nothing");
			decoder->encoded_data_available = FALSE;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}

		IMX_VPU_API_LOG("found frame, offsets:  first %zu  last %zu  associated frame entry index: %zu", (size_t)(first_offset_ptr), (size_t)(last_offset_ptr), decoder->last_pushed_frame_entry_index);

		stream_buffer.streamlen = last_offset_ptr - first_offset_ptr;

		stream_buffer.bus_data = decoder->stream_buffer_virtual_address + decoder->stream_buffer_read_offset + first_offset_ptr;
		stream_buffer.buf_data = decoder->stream_buffer_virtual_address;
		stream_buffer.bus_address = (OSAL_BUS_WIDTH)(decoder->stream_buffer_physical_address + decoder->stream_buffer_read_offset + first_offset_ptr);
		stream_buffer.buf_address = (OSAL_BUS_WIDTH)(decoder->stream_buffer_physical_address);
		stream_buffer.sliceInfoNum = decoder->slice_info_nr;
		stream_buffer.pSliceInfo = (OMX_U8 *)(&(decoder->slice_info[0]));
		stream_buffer.picId = (OMX_U32)(decoder->last_pushed_frame_entry_index);

		codec_state = decoder->codec->decode(decoder->codec, &stream_buffer, &num_used_input_bytes, &frame);
		IMX_VPU_API_LOG("decode() result:  codec state %s (%d)  num used input bytes %zu", codec_state_to_string(codec_state), codec_state, (size_t)(num_used_input_bytes));

		num_used_input_bytes += first_offset_ptr;

		assert(decoder->stream_buffer_fill_level >= num_used_input_bytes);
		decoder->stream_buffer_fill_level -= num_used_input_bytes;
		decoder->stream_buffer_read_offset += num_used_input_bytes;
		if (decoder->stream_buffer_read_offset >= decoder->stream_buffer_size)
			decoder->stream_buffer_read_offset -= decoder->stream_buffer_size;

		switch (codec_state)
		{
			case CODEC_OK:
				break;

			case CODEC_NEED_MORE:
			case CODEC_BUFFER_EMPTY:
				if (decoder->stream_buffer_fill_level == 0)
					do_loop = FALSE;
				decoder->encoded_data_available = FALSE;
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
				break;

			case CODEC_PENDING_FLUSH:
				/* This is reached when the video parameters change.
				 * The decoder needs to get rid of any remaining decoded
				 * video frames at this point before it can continue,
				 * because it needs to empty its buffer pool and request
				 * new framebuffers to be added (the old ones are no longer
				 * used since they may have incorrect size due to changed
				 * resolution for example). So, we exit the loop here and
				 * set IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED
				 * as the output code. That way, the caller will know to
				 * reopen the decoder, get rid of old framebuffers, and
				 * allocate new ones when requested. */
				do_loop = FALSE;
				IMX_VPU_API_DEBUG("decoder is in a pending-flush state -> video params changed");
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED;
				break;

			case CODEC_NO_DECODING_BUFFER:
				do_loop = FALSE;
				IMX_VPU_API_DEBUG("could not decode because there is no available framebuffer; requesting more framebuffers");
				decoder->num_framebuffers_to_be_added = 1;
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER;
				break;

			case CODEC_HAS_FRAME:
			{
				/* This return value means that a decoded frame has been
				 * produced. It does _not_ necessarily mean that the
				 * decoded frame is actually available. if decode() returns
				 * CODEC_HAS_FRAME, but the subsequent getframe() call
				 * doesn't, then this is an internal frame that will not be
				 * output by the codec. With some codecs, such internal frames
				 * must be skipped by players. One example is a VP8 altref frame.
				 * So, here, just set the flag that the decoded frame was
				 * reported, and exit the loop. The next imx_vpu_api_dec_decode()
				 * call will call getframe(), and there, the flag will be
				 * evaluated along with the getframe() return value.
				 *
				 * Note that it is unrelated to the CODEC_PIC_SKIPPED case. Both
				 * cases report skipped frames, but the similarities end there. */

				do_loop = FALSE;
				decoder->decoded_frame_reported = TRUE;
				IMX_VPU_API_LOG("decoded frame is available");

				break;
			}

			case CODEC_WAITING_FRAME_BUFFER:
				do_loop = FALSE;
				if (decoder->has_new_stream_info)
				{
					/* Clear out old framebuffers and frame entries, since
					 * they are not needed anymore. Furthermore, if they
					 * remained, they'd be causing errors during video
					 * parameter changes, because the code in
					 * imx_vpu_api_dec_add_framebuffers_to_pool() would
					 * not work correctly (specifically, the indices into
					 * fb_dma_buffers would not match with the framebuffer
					 * entries).
					 *
					 * This also means that at this point, any previously
					 * added framebuffer is no longer added to the VPU's
					 * pool, so these old framebuffers can be safely discarded
					 * at this point. */
					imx_vpu_api_dec_clear_added_framebuffers(decoder);

					if (!imx_vpu_api_dec_get_new_stream_info(decoder))
						return IMX_VPU_API_DEC_RETURN_CODE_ERROR;

					/* Turn off this flag again to make sure we don't
					 * wrongly announce new stream info even though there
					 * isn't any new one. */
					decoder->has_new_stream_info = FALSE;

					IMX_VPU_API_LOG("new stream info was seen earlier, and new framebuffers are needed");
					decoder->num_framebuffers_to_be_added = decoder->stream_info.min_num_required_framebuffers;
					*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE;
				}
				else
				{
					IMX_VPU_API_LOG("more framebuffers are needed for decoding");
					decoder->num_framebuffers_to_be_added = 1;
					*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER;
				}
				break;

			case CODEC_HAS_INFO:
			{
				/* We cannot call imx_vpu_api_dec_get_new_stream_info() here
				 * right away, because some information may not be available
				 * yet. This is for example the case with WMV3 content and
				 * the minimum framebuffer count. If we retrieved the info
				 * right here, the framebuffer count would be set to 0, even
				 * though it should be a nonzero value. By calling it later,
				 * when CODEC_WAITING_FRAME_BUFFER is returned, we make sure
				 * that _all_ information is available. */

				decoder->has_new_stream_info = TRUE;

				break;
			}

			case CODEC_PIC_SKIPPED:
#ifdef HAVE_IMXVPUDEC_HANTRO_CODEC_ERROR_FRAME
			case CODEC_ERROR_FRAME:
#endif
			{
				FrameEntry *frame_entry;

				if ((decoder->last_pushed_frame_entry_index != INVALID_FRAME_ENTRY_INDEX) || (decoder->last_pushed_frame_entry_index < decoder->num_frame_entries))
				{
					frame_entry = &(decoder->frame_entries[decoder->last_pushed_frame_entry_index]);
					decoder->skipped_frame_reason = IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME;
#ifdef HAVE_IMXVPUDEC_HANTRO_CODEC_ERROR_FRAME
					if (codec_state == CODEC_PIC_SKIPPED)
						decoder->skipped_frame_reason = IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_CORRUPTED_FRAME;
#endif
					decoder->skipped_frame_context = frame_entry->context;
					decoder->skipped_frame_pts = frame_entry->pts;
					decoder->skipped_frame_dts = frame_entry->dts;
					frame_entry->occupied = FALSE;

					decoder->encoded_data_available = FALSE;

					*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;

					IMX_VPU_API_LOG("frame at entry index %zu with context %p PTS %" PRIu64 " DTS %" PRIu64 " got skipped", decoder->last_pushed_frame_entry_index, frame_entry->context, frame_entry->pts, frame_entry->dts);
				}
				else
				{
					IMX_VPU_API_ERROR("Could not get context for skipped frame; last pushed frame entry index is invalid (%zu)", decoder->last_pushed_frame_entry_index);
					ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
				}

				do_loop = FALSE;
				break;
			}

			case CODEC_ERROR_STREAM_NOT_SUPPORTED:
				IMX_VPU_API_ERROR("this bitstream is not supported");
				ret = IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_BITSTREAM;
				do_loop = FALSE;
				break;

			default:
				IMX_VPU_API_ERROR("decoding failure:  codec state %s (%d)", codec_state_to_string(codec_state), codec_state);
				ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
				do_loop = FALSE;
				break;
		}
	}
	while (do_loop);

	return ret;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_get_decoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiRawFrame *decoded_frame)
{
	FrameEntry *frame_entry;
	FramebufferEntry *framebuffer_entry;

	assert(decoder != NULL);
	assert(decoded_frame != NULL);

	if ((decoder->decoded_frame_entry_index == INVALID_FRAME_ENTRY_INDEX) || (decoder->decoded_frame_fb_entry_index == INVALID_FRAME_ENTRY_INDEX))
	{
		IMX_VPU_API_ERROR("cannot get decoded frame because no decoded frame is available");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	assert(decoder->decoded_frame_fb_entry_index < decoder->num_framebuffer_entries);

	frame_entry = &(decoder->frame_entries[decoder->decoded_frame_entry_index]);
	framebuffer_entry = &(decoder->framebuffer_entries[decoder->decoded_frame_fb_entry_index]);

	IMX_VPU_API_LOG("got frame with context %p PTS %" PRIu64 " DTS %" PRIu64 " frame entry index %zu framebuffer entry index %zu", frame_entry->context, frame_entry->pts, frame_entry->dts, decoder->decoded_frame_entry_index, decoder->decoded_frame_fb_entry_index);

	decoded_frame->fb_dma_buffer = framebuffer_entry->fb_dma_buffer;
	decoded_frame->fb_context = framebuffer_entry->fb_context;
	decoded_frame->context = frame_entry->context;
	decoded_frame->pts = frame_entry->pts;
	decoded_frame->dts = frame_entry->dts;
	/* The Hantro decoder does not specify the frame type. */
	decoded_frame->frame_types[0] = IMX_VPU_API_FRAME_TYPE_UNKNOWN;
	decoded_frame->frame_types[1] = IMX_VPU_API_FRAME_TYPE_UNKNOWN;
	/* The Hantro decoder does not specify how the fields are
	 * arranged. It seems to use bottom-field-first for all formats. */
	decoded_frame->interlacing_mode = IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST;

	frame_entry->occupied = FALSE;

	decoder->decoded_frame_entry_index = INVALID_FRAME_ENTRY_INDEX;
	decoder->decoded_frame_fb_entry_index = INVALID_FRAME_ENTRY_INDEX;

	return IMX_VPU_API_DEC_RETURN_CODE_OK;
}


void imx_vpu_api_dec_return_framebuffer_to_decoder(ImxVpuApiDecoder *decoder, ImxDmaBuffer *fb_dma_buffer)
{
	CODEC_STATE codec_state;
	BUFFER buffer = { 0 };
	imx_physical_address_t physical_address;
	FramebufferEntry *framebuffer_entry;
	size_t fb_entry_index;

	physical_address = imx_dma_buffer_get_physical_address(fb_dma_buffer);

	fb_entry_index = imx_vpu_api_dec_find_framebuffer_entry_index(decoder, physical_address);
	if ((fb_entry_index == INVALID_FRAME_ENTRY_INDEX) || (fb_entry_index >= decoder->num_framebuffer_entries))
	{
		IMX_VPU_API_ERROR("could not find framebuffer entry for the given DMA buffer");
		return;
	}

	framebuffer_entry = &(decoder->framebuffer_entries[fb_entry_index]);
	assert(framebuffer_entry != NULL);

	buffer.bus_data = framebuffer_entry->mapped_virtual_address;
	buffer.bus_address = (OSAL_BUS_WIDTH)physical_address;

	codec_state = decoder->codec->pictureconsumed(decoder->codec, &buffer);
	if (codec_state != CODEC_OK)
		IMX_VPU_API_ERROR("could not return framebuffer to decoder:  codec state %s (%d)", codec_state_to_string(codec_state), codec_state);
}


void imx_vpu_api_dec_get_skipped_frame_info(ImxVpuApiDecoder *decoder, ImxVpuApiDecSkippedFrameReasons *reason, void **context, uint64_t *pts, uint64_t *dts)
{
	if (reason != NULL)
		*reason = decoder->skipped_frame_reason;
	if (context != NULL)
		*context = decoder->skipped_frame_context;
	if (pts != NULL)
		*pts = decoder->skipped_frame_pts;
	if (dts != NULL)
		*dts = decoder->skipped_frame_dts;
}
