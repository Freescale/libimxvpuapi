/* imxvpuapi implementation on top of the Freescale imx-vpu library
 * Copyright (C) 2015 Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */


#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vpu_lib.h>
#include <vpu_io.h>
#include "imxvpuapi.h"
#include "imxvpuapi_priv.h"




/***********************************************/
/******* COMMON STRUCTURES AND FUNCTIONS *******/
/***********************************************/


#ifndef TRUE
#define TRUE (1)
#endif


#ifndef FALSE
#define FALSE (0)
#endif


#ifndef BOOL
#define BOOL int
#endif


/* This catches fringe cases where somebody passes a
 * non-null value as TRUE that is not the same as TRUE */
#define TO_BOOL(X) ((X) ? TRUE : FALSE)


#define MIN_NUM_FREE_FB_REQUIRED 6
#define FRAME_ALIGN 16

#define VPU_MEMORY_ALIGNMENT         0x8
#define VPU_BITSTREAM_BUFFER_SIZE    (1024*1024*3)
#define VPU_MAX_SLICE_BUFFER_SIZE    (1920*1088*15/20)
#define VPU_PS_SAVE_BUFFER_SIZE      (1024*512)
#define VPU_VP8_MB_PRED_BUFFER_SIZE  (68*(1920*1088/256))

#define VP8_SEQUENCE_HEADER_SIZE  32
#define VP8_FRAME_HEADER_SIZE     12

#define WMV3_RCV_SEQUENCE_LAYER_SIZE (6 * 4)
#define WMV3_RCV_FRAME_LAYER_SIZE    4

#define VC1_NAL_FRAME_LAYER_MAX_SIZE   4
#define VC1_IS_NOT_NAL(ID)             (( (ID) & 0x00FFFFFF) != 0x00010000)

#define VPU_WAIT_TIMEOUT             500 /* milliseconds to wait for frame completion */
#define VPU_MAX_TIMEOUT_COUNTS       4   /* how many timeouts are allowed in series */


static unsigned long vpu_init_inst_counter = 0;


static BOOL imx_vpu_load(void)
{
	IMX_VPU_TRACE("VPU init instance counter: %lu", vpu_init_inst_counter);
	if (vpu_init_inst_counter != 0)
		return IMX_VPU_DEC_RETURN_CODE_OK;

	if (vpu_Init(NULL) == RETCODE_SUCCESS)
	{
		IMX_VPU_TRACE("loaded VPU");
		++vpu_init_inst_counter;
		return IMX_VPU_DEC_RETURN_CODE_OK;
	}
	else
	{
		IMX_VPU_ERROR("loading VPU failed");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}
}


static BOOL imx_vpu_unload(void)
{
	IMX_VPU_TRACE("VPU init instance counter: %lu", vpu_init_inst_counter);
	if (vpu_init_inst_counter == 0)
		return IMX_VPU_DEC_RETURN_CODE_OK;

	vpu_UnInit();

	IMX_VPU_TRACE("unloaded VPU");
	--vpu_init_inst_counter;
	return IMX_VPU_DEC_RETURN_CODE_OK;
}


static ImxVpuPicType convert_pic_type(ImxVpuCodecFormat codec_format, int pic_type)
{
	switch (codec_format)
	{
		case IMX_VPU_CODEC_FORMAT_H264:
		case IMX_VPU_CODEC_FORMAT_H264_MVC:
			if ((pic_type & 0x01) == 0)
				return IMX_VPU_PIC_TYPE_IDR;
			else
			{
				switch ((pic_type >> 1) & 0x03)
				{
					case 0: return IMX_VPU_PIC_TYPE_I;
					case 1: return IMX_VPU_PIC_TYPE_P;
					case 2: case 3: return IMX_VPU_PIC_TYPE_B;
					default: break;
				}
			}
			break;

		case IMX_VPU_CODEC_FORMAT_WMV3:
			switch (pic_type & 0x07)
			{
				case 0: return IMX_VPU_PIC_TYPE_I;
				case 1: return IMX_VPU_PIC_TYPE_P;
				case 2: return IMX_VPU_PIC_TYPE_BI;
				case 3: return IMX_VPU_PIC_TYPE_B;
				case 4: return IMX_VPU_PIC_TYPE_SKIP;
				default: break;
			}
			break;

		/*case IMX_VPU_CODEC_FORMAT_WVC1: // TODO
			break;*/

		default:
			switch (pic_type)
			{
				case 0: return IMX_VPU_PIC_TYPE_I;
				case 1: return IMX_VPU_PIC_TYPE_P;
				case 2: case 3: return IMX_VPU_PIC_TYPE_B;
				default: break;
			}
	}

	return IMX_VPU_PIC_TYPE_UNKNOWN;
}




/**************************************************/
/******* ALLOCATOR STRUCTURES AND FUNCTIONS *******/
/**************************************************/


/*********** Default allocator ***********/



typedef struct
{
	ImxVpuDMABuffer parent;
	vpu_mem_desc mem_desc;

	/* Not the same as mem_desc->size
	 * the value in mem_desc is potentially larger due to alignment */
	size_t size;

	uint8_t*            aligned_virtual_address;
	imx_vpu_phys_addr_t aligned_physical_address;
}
DefaultDMABuffer;


typedef struct
{
	ImxVpuDMABufferAllocator parent;
}
DefaultDMABufferAllocator;


static ImxVpuDMABuffer* default_dmabufalloc_allocate(ImxVpuDMABufferAllocator *allocator, size_t size, unsigned int alignment, unsigned int flags)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);
	IMXVPUAPI_UNUSED_PARAM(flags);

	DefaultDMABuffer *dmabuffer = IMX_VPU_ALLOC(sizeof(DefaultDMABuffer));
	if (dmabuffer == NULL)
	{
		IMX_VPU_ERROR("allocating heap block for DMA buffer failed");
		return NULL;
	}

	dmabuffer->mem_desc.size = size;

	if (alignment == 0)
		alignment = 1;
	if (alignment > 1)
		dmabuffer->mem_desc.size += alignment;

	dmabuffer->parent.allocator = allocator;
	dmabuffer->size = size;

	if (IOGetPhyMem(&(dmabuffer->mem_desc)) == RETCODE_FAILURE)
	{
		IMX_VPU_FREE(dmabuffer, sizeof(DefaultDMABuffer));
		IMX_VPU_ERROR("allocating %d bytes of physical memory failed", size);
		return NULL;
	}
	else
		IMX_VPU_TRACE("allocated %d bytes of physical memory", size);

	if (IOGetVirtMem(&(dmabuffer->mem_desc)) == RETCODE_FAILURE)
	{
		IOFreePhyMem(&(dmabuffer->mem_desc));
		IMX_VPU_FREE(dmabuffer, sizeof(DefaultDMABuffer));
		IMX_VPU_ERROR("retrieving virtual address for physical memory failed");
		return NULL;
	}
	else
		IMX_VPU_TRACE("retrieved virtual address for physical memory");

	dmabuffer->aligned_virtual_address = (uint8_t *)IMX_VPU_ALIGN_VAL_TO((uint8_t *)(dmabuffer->mem_desc.virt_uaddr), alignment);
	dmabuffer->aligned_physical_address = (imx_vpu_phys_addr_t)IMX_VPU_ALIGN_VAL_TO((imx_vpu_phys_addr_t)(dmabuffer->mem_desc.phy_addr), alignment);

	IMX_VPU_TRACE("virtual address:  0x%x  aligned: %p", dmabuffer->mem_desc.virt_uaddr, dmabuffer->aligned_virtual_address);
	IMX_VPU_TRACE("physical address: 0x%x  aligned: %" IMX_VPU_PHYS_ADDR_FORMAT, dmabuffer->mem_desc.phy_addr, dmabuffer->aligned_physical_address);

	return (ImxVpuDMABuffer *)dmabuffer;
}


static void default_dmabufalloc_deallocate(ImxVpuDMABufferAllocator *allocator, ImxVpuDMABuffer *buffer)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);

	DefaultDMABuffer *defaultbuf = (DefaultDMABuffer *)buffer;

	if (IOFreePhyMem(&(defaultbuf->mem_desc)) != 0)
		IMX_VPU_ERROR("deallocating %d bytes of physical memory failed", defaultbuf->size);
	else
		IMX_VPU_TRACE("deallocated %d bytes of physical memory", defaultbuf->size);
}


static void default_dmabufalloc_map(ImxVpuDMABufferAllocator *allocator, ImxVpuDMABuffer *buffer, uint8_t **virtual_address, imx_vpu_phys_addr_t *physical_address, unsigned int flags)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);
	IMXVPUAPI_UNUSED_PARAM(flags);

	DefaultDMABuffer *defaultbuf = (DefaultDMABuffer *)buffer;
	*virtual_address = defaultbuf->aligned_virtual_address;
	*physical_address = defaultbuf->aligned_physical_address;
}


static void default_dmabufalloc_unmap(ImxVpuDMABufferAllocator *allocator, ImxVpuDMABuffer *buffer)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);
	IMXVPUAPI_UNUSED_PARAM(buffer);
}


int default_dmabufalloc_get_fd(ImxVpuDMABufferAllocator *allocator, ImxVpuDMABuffer *buffer)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);
	IMXVPUAPI_UNUSED_PARAM(buffer);
	return -1;
}


size_t default_dmabufalloc_get_size(ImxVpuDMABufferAllocator *allocator, ImxVpuDMABuffer *buffer)
{
	IMXVPUAPI_UNUSED_PARAM(allocator);
	DefaultDMABuffer *defaultbuf = (DefaultDMABuffer *)buffer;
	return defaultbuf->size;
}


static DefaultDMABufferAllocator default_dma_buffer_allocator =
{
	{
		default_dmabufalloc_allocate,
		default_dmabufalloc_deallocate,
		default_dmabufalloc_map,
		default_dmabufalloc_unmap,
		default_dmabufalloc_get_fd,
		default_dmabufalloc_get_size
	}
};




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


void imx_vpu_calc_framebuffer_sizes(ImxVpuColorFormat color_format, unsigned int frame_width, unsigned int frame_height, unsigned int framebuffer_alignment, int uses_interlacing, ImxVpuFramebufferSizes *calculated_sizes)
{
	int alignment;

	assert(calculated_sizes != NULL);
	assert(frame_width > 0);
	assert(frame_height > 0);

	calculated_sizes->aligned_frame_width = IMX_VPU_ALIGN_VAL_TO(frame_width, FRAME_ALIGN);
	if (uses_interlacing)
		calculated_sizes->aligned_frame_height = IMX_VPU_ALIGN_VAL_TO(frame_height, (2 * FRAME_ALIGN));
	else
		calculated_sizes->aligned_frame_height = IMX_VPU_ALIGN_VAL_TO(frame_height, FRAME_ALIGN);

	calculated_sizes->y_stride = calculated_sizes->aligned_frame_width;
	calculated_sizes->y_size = calculated_sizes->y_stride * calculated_sizes->aligned_frame_height;

	switch (color_format)
	{
		case IMX_VPU_COLOR_FORMAT_YUV420:
			calculated_sizes->cbcr_stride = calculated_sizes->y_stride / 2;
			calculated_sizes->cbcr_size = calculated_sizes->mvcol_size = calculated_sizes->y_size / 4;
			break;
		case IMX_VPU_COLOR_FORMAT_YUV422_HORIZONTAL:
			calculated_sizes->cbcr_stride = calculated_sizes->y_stride / 2;
			calculated_sizes->cbcr_size = calculated_sizes->mvcol_size = calculated_sizes->y_size / 2;
			break;
		case IMX_VPU_COLOR_FORMAT_YUV444:
			calculated_sizes->cbcr_stride = calculated_sizes->y_stride;
			calculated_sizes->cbcr_size = calculated_sizes->mvcol_size = calculated_sizes->y_size;
			break;
		case IMX_VPU_COLOR_FORMAT_YUV400:
			/* TODO: check if this is OK */
			calculated_sizes->cbcr_stride = 0;
			calculated_sizes->cbcr_size = calculated_sizes->mvcol_size = 0;
			break;
		default:
			assert(FALSE);
	}

	alignment = framebuffer_alignment;
	if (alignment > 1)
	{
		calculated_sizes->y_size = IMX_VPU_ALIGN_VAL_TO(calculated_sizes->y_size, alignment);
		calculated_sizes->cbcr_size = IMX_VPU_ALIGN_VAL_TO(calculated_sizes->cbcr_size, alignment);
		calculated_sizes->mvcol_size = IMX_VPU_ALIGN_VAL_TO(calculated_sizes->mvcol_size, alignment);
	}

	calculated_sizes->total_size = calculated_sizes->y_size + calculated_sizes->cbcr_size + calculated_sizes->cbcr_size + calculated_sizes->mvcol_size + alignment;
}


void imx_vpu_fill_framebuffer_params(ImxVpuFramebuffer *framebuffer, ImxVpuFramebufferSizes *calculated_sizes, ImxVpuDMABuffer *fb_dma_buffer, void* context)
{
	assert(framebuffer != NULL);
	assert(calculated_sizes != NULL);

	framebuffer->dma_buffer = fb_dma_buffer;
	framebuffer->context = context;
	framebuffer->y_stride = calculated_sizes->y_stride;
	framebuffer->cbcr_stride = calculated_sizes->cbcr_stride;
	framebuffer->y_offset = 0;
	framebuffer->cb_offset = calculated_sizes->y_size;
	framebuffer->cr_offset = calculated_sizes->y_size + calculated_sizes->cbcr_size;
	framebuffer->mvcol_offset = calculated_sizes->y_size + calculated_sizes->cbcr_size * 2;
}




/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


struct _ImxVpuDecoder
{
	DecHandle handle;

	ImxVpuDMABuffer *bitstream_buffer;
	uint8_t *bitstream_buffer_virtual_address;
	imx_vpu_phys_addr_t bitstream_buffer_physical_address;

	ImxVpuCodecFormat codec_format;
	unsigned int picture_width, picture_height;

	unsigned int num_framebuffers, num_used_framebuffers;
	FrameBuffer *internal_framebuffers;
	ImxVpuFramebuffer *framebuffers;
	void **context_for_frames;

	BOOL main_header_pushed;

	BOOL drain_mode_enabled;
	BOOL drain_eos_sent_to_vpu;

	DecInitialInfo initial_info;
	BOOL initial_info_available;

	DecOutputInfo dec_output_info;
	int available_decoded_pic_idx;
};


#define IMX_VPU_DEC_HANDLE_ERROR(MSG_START, RET_CODE) \
	imx_vpu_dec_handle_error_full(__FILE__, __LINE__, __FUNCTION__, (MSG_START), (RET_CODE))


static ImxVpuDecReturnCodes imx_vpu_dec_handle_error_full(char const *fn, int linenr, char const *funcn, char const *msg_start, RetCode ret_code);
static ImxVpuDecReturnCodes imx_vpu_dec_get_initial_info_internal(ImxVpuDecoder *decoder);

static void imx_vpu_dec_insert_vp8_ivf_main_header(uint8_t *header, unsigned int pic_width, unsigned int pic_height);
static void imx_vpu_dec_insert_vp8_ivf_frame_header(uint8_t *header, unsigned int main_data_size);

static void imx_vpu_dec_insert_wmv3_sequence_layer_header(uint8_t *header, unsigned int pic_width, unsigned int pic_height, unsigned int main_data_size, uint8_t *codec_data);
static void imx_vpu_dec_insert_wmv3_frame_layer_header(uint8_t *header, unsigned int main_data_size);

static void imx_vpu_dec_insert_vc1_frame_layer_header(uint8_t *header, uint8_t *main_data, unsigned int *actual_header_length);

static ImxVpuDecReturnCodes imx_vpu_dec_insert_frame_headers(ImxVpuDecoder *decoder, uint8_t *codec_data, unsigned int codec_data_size, uint8_t *main_data, unsigned int main_data_size);

static ImxVpuDecReturnCodes imx_vpu_dec_push_input_data(ImxVpuDecoder *decoder, void *data, unsigned int data_size);


static ImxVpuDecReturnCodes imx_vpu_dec_handle_error_full(char const *fn, int linenr, char const *funcn, char const *msg_start, RetCode ret_code)
{
	switch (ret_code)
	{
		case RETCODE_SUCCESS:
			return IMX_VPU_DEC_RETURN_CODE_OK;

		case RETCODE_FAILURE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: failure", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_INVALID_HANDLE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: invalid handle", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_HANDLE;

		case RETCODE_INVALID_PARAM:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: invalid parameters", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_INVALID_COMMAND:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: invalid command", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_ROTATOR_OUTPUT_NOT_SET:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: rotation enabled but rotator output buffer not set", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_ROTATOR_STRIDE_NOT_SET:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: rotation enabled but rotator stride not set", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_FRAME_NOT_COMPLETE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: frame decoding operation not complete", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_INVALID_FRAME_BUFFER:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: frame buffers are invalid", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_INSUFFICIENT_FRAME_BUFFERS:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: not enough frame buffers specified (must be equal to or larger than the minimum number reported by imx_vpu_dec_get_initial_info)", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_INVALID_STRIDE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: invalid stride - check Y stride values of framebuffers (must be a multiple of 8 and equal to or larger than the picture width)", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_WRONG_CALL_SEQUENCE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: wrong call sequence", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE;

		case RETCODE_CALLED_BEFORE:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: already called before (may not be called more than once in a VPU instance)", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ALREADY_CALLED;

		case RETCODE_NOT_INITIALIZED:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: VPU is not initialized", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE;

		case RETCODE_DEBLOCKING_OUTPUT_NOT_SET:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: deblocking activated but deblocking information not available", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_NOT_SUPPORTED:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: feature not supported", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_REPORT_BUF_NOT_SET:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: data report buffer address not set", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;

		case RETCODE_FAILURE_TIMEOUT:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: timeout", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_MEMORY_ACCESS_VIOLATION:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: memory access violation", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		case RETCODE_JPEG_EOS:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: MJPEG end-of-stream reached", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_OK;

		case RETCODE_JPEG_BIT_EMPTY:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: MJPEG bit buffer empty - cannot parse header", msg_start);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;

		default:
			IMX_VPU_ERROR_FULL(fn, linenr, funcn, "%s: unknown error 0x%x", msg_start, ret_code);
			return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}
}


char const * imx_vpu_dec_error_string(ImxVpuDecReturnCodes code)
{
	switch (code)
	{
		case IMX_VPU_DEC_RETURN_CODE_OK:                        return "ok";
		case IMX_VPU_DEC_RETURN_CODE_ERROR:                     return "unspecified error";
		case IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS:            return "invalid params";
		case IMX_VPU_DEC_RETURN_CODE_INVALID_HANDLE:            return "invalid handle";
		case IMX_VPU_DEC_RETURN_CODE_INVALID_FRAMEBUFFER:       return "invalid framebuffer";
		case IMX_VPU_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS: return "insufficient framebuffers";
		case IMX_VPU_DEC_RETURN_CODE_INVALID_STRIDE:            return "invalid stride";
		case IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE:       return "wrong call sequence";
		case IMX_VPU_DEC_RETURN_CODE_TIMEOUT:                   return "timeout";
		case IMX_VPU_DEC_RETURN_CODE_ALREADY_CALLED:            return "already called";
		default: return "<unknown>";
	}
}


ImxVpuDecReturnCodes imx_vpu_dec_load(void)
{
	return imx_vpu_load() ? IMX_VPU_DEC_RETURN_CODE_OK : IMX_VPU_DEC_RETURN_CODE_ERROR;
}


ImxVpuDecReturnCodes imx_vpu_dec_unload(void)
{
	return imx_vpu_unload() ? IMX_VPU_DEC_RETURN_CODE_OK : IMX_VPU_DEC_RETURN_CODE_ERROR;
}


ImxVpuDMABufferAllocator* imx_vpu_dec_get_default_allocator(void)
{
	return (ImxVpuDMABufferAllocator*)(&default_dma_buffer_allocator);
}


void imx_vpu_dec_get_bitstream_buffer_info(size_t *size, unsigned int *alignment)
{
	*size = VPU_BITSTREAM_BUFFER_SIZE + VPU_MAX_SLICE_BUFFER_SIZE + VPU_PS_SAVE_BUFFER_SIZE;
	*alignment = VPU_MEMORY_ALIGNMENT;
}


ImxVpuDecReturnCodes imx_vpu_dec_open(ImxVpuDecoder **decoder, ImxVpuDecOpenParams *open_params, ImxVpuDMABuffer *bitstream_buffer)
{
	ImxVpuDecReturnCodes ret;
	DecOpenParam dec_open_param;
	RetCode dec_ret;

	assert(decoder != NULL);
	assert(open_params != NULL);
	assert(bitstream_buffer != NULL);


	/* Allocate decoder instance */
	*decoder = IMX_VPU_ALLOC(sizeof(ImxVpuDecoder));
	if ((*decoder) == NULL)
	{
		IMX_VPU_ERROR("allocating memory for decoder object failed");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}


	/* Set default decoder values */
	memset(*decoder, 0, sizeof(ImxVpuDecoder));
	(*decoder)->available_decoded_pic_idx = -1;


	/* Map the bitstream buffer. This mapping will persist until the decoder is closed. */
	imx_vpu_dma_buffer_map(bitstream_buffer, &((*decoder)->bitstream_buffer_virtual_address),  &((*decoder)->bitstream_buffer_physical_address), 0);


	/* Fill in values into the VPU's decoder open param structure */
	memset(&dec_open_param, 0, sizeof(dec_open_param));
	switch (open_params->codec_format)
	{
		case IMX_VPU_CODEC_FORMAT_H264:
		case IMX_VPU_CODEC_FORMAT_H264_MVC:
			dec_open_param.bitstreamFormat = STD_AVC;
			dec_open_param.reorderEnable = open_params->enable_frame_reordering;
			break;
		case IMX_VPU_CODEC_FORMAT_MPEG2:
			dec_open_param.bitstreamFormat = STD_MPEG2;
			break;
		case IMX_VPU_CODEC_FORMAT_MPEG4:
			dec_open_param.bitstreamFormat = STD_MPEG4;
			dec_open_param.mp4Class = 0;
			break;
		case IMX_VPU_CODEC_FORMAT_H263:
			dec_open_param.bitstreamFormat = STD_H263;
			break;
		case IMX_VPU_CODEC_FORMAT_WMV3:
			dec_open_param.bitstreamFormat = STD_VC1;
			break;
		case IMX_VPU_CODEC_FORMAT_WVC1:
			dec_open_param.bitstreamFormat = STD_VC1;
			dec_open_param.reorderEnable = 1;
			break;
		case IMX_VPU_CODEC_FORMAT_MJPEG:
			dec_open_param.bitstreamFormat = STD_MJPG;
			break;
		case IMX_VPU_CODEC_FORMAT_VP8:
			dec_open_param.bitstreamFormat = STD_VP8;
			dec_open_param.reorderEnable = 1;
			break;
		default:
			break;
	}

	dec_open_param.bitstreamBuffer = (*decoder)->bitstream_buffer_physical_address;
	dec_open_param.bitstreamBufferSize = imx_vpu_dma_buffer_get_size(bitstream_buffer);
	dec_open_param.qpReport = 0;
	dec_open_param.mp4DeblkEnable = 0;
	dec_open_param.chromaInterleave = 0;
	dec_open_param.filePlayEnable = 0;
	dec_open_param.picWidth = open_params->frame_width;
	dec_open_param.picHeight = open_params->frame_height;
	dec_open_param.avcExtension = (open_params->codec_format == IMX_VPU_CODEC_FORMAT_H264_MVC);
	dec_open_param.dynamicAllocEnable = 0;
	dec_open_param.streamStartByteOffset = 0;
	dec_open_param.mjpg_thumbNailDecEnable = 0;
	dec_open_param.psSaveBuffer = (*decoder)->bitstream_buffer_physical_address + VPU_BITSTREAM_BUFFER_SIZE + VPU_MAX_SLICE_BUFFER_SIZE;
	dec_open_param.psSaveBufferSize = VPU_PS_SAVE_BUFFER_SIZE;
	dec_open_param.mapType = 0;
	dec_open_param.tiled2LinearEnable = 0; // this must ALWAYS be 0, otherwise VPU hangs eventually (it is 0 in the wrapper except for MX6X)
	dec_open_param.bitstreamMode = 1;
	dec_open_param.jpgLineBufferMode = (open_params->codec_format == IMX_VPU_CODEC_FORMAT_MJPEG);


	/* Now actually open the decoder instance */
	IMX_VPU_TRACE("opening decoder");
	dec_ret = vpu_DecOpen(&((*decoder)->handle), &dec_open_param);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not open decoder", dec_ret);
	if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
		goto cleanup;

	(*decoder)->codec_format = open_params->codec_format;
	(*decoder)->bitstream_buffer = bitstream_buffer;
	(*decoder)->picture_width = open_params->frame_width;
	(*decoder)->picture_height = open_params->frame_height;


	/* Finish & cleanup (in case of error) */
finish:
	if (ret == IMX_VPU_DEC_RETURN_CODE_OK)
		IMX_VPU_TRACE("successfully opened decoder");

	return ret;

cleanup:
	imx_vpu_dma_buffer_unmap(bitstream_buffer);
	IMX_VPU_FREE(*decoder, sizeof(ImxVpuDecoder));
	*decoder = NULL;

	goto finish;
}


ImxVpuDecReturnCodes imx_vpu_dec_close(ImxVpuDecoder *decoder)
{
	ImxVpuDecReturnCodes ret;
	RetCode dec_ret;

	assert(decoder != NULL);

	IMX_VPU_TRACE("closing decoder");


	// TODO: call vpu_DecGetOutputInfo() for all started frames


	/* Flush the VPU bit buffer */
	dec_ret = vpu_DecBitBufferFlush(decoder->handle);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not flush decoder", dec_ret);

	/* Signal EOS to the decoder by passing 0 as size to vpu_DecUpdateBitstreamBuffer() */
	dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, 0);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not signal EOS to the decoder", dec_ret);

	/* Now, actually close the decoder */
	dec_ret = vpu_DecClose(decoder->handle);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not close decoder", dec_ret);


	/* Remaining cleanup */

	if (decoder->internal_framebuffers != NULL)
		IMX_VPU_FREE(decoder->internal_framebuffers, sizeof(FrameBuffer) * decoder->num_framebuffers);	
	if (decoder->context_for_frames != NULL)
		IMX_VPU_FREE(decoder->context_for_frames, sizeof(void*) * decoder->num_framebuffers);	

	IMX_VPU_FREE(decoder, sizeof(ImxVpuDecoder));

	if (ret == IMX_VPU_DEC_RETURN_CODE_OK)
		IMX_VPU_TRACE("closed decoder");

	return ret;
}


ImxVpuDecReturnCodes imx_vpu_dec_enable_drain_mode(ImxVpuDecoder *decoder, int enabled)
{
	assert(decoder != NULL);

	if (decoder->drain_mode_enabled == TO_BOOL(enabled))
		return IMX_VPU_DEC_RETURN_CODE_OK;

	decoder->drain_mode_enabled = TO_BOOL(enabled);
	if (enabled)
		decoder->drain_eos_sent_to_vpu = FALSE;

	return IMX_VPU_DEC_RETURN_CODE_OK;
}


int imx_vpu_dec_is_drain_mode_enabled(ImxVpuDecoder *decoder)
{
	assert(decoder != NULL);
	return decoder->drain_mode_enabled;
}


ImxVpuDecReturnCodes imx_vpu_dec_flush(ImxVpuDecoder *decoder)
{
	ImxVpuDecReturnCodes ret;
	RetCode dec_ret;
	unsigned int i;

	assert(decoder != NULL);

	IMX_VPU_TRACE("flushing decoder");


	/* If any framebuffers are not yet marked as displayed, do so here */
	for (i = 0; i < decoder->num_framebuffers; ++i)
		imx_vpu_dec_mark_framebuffer_as_displayed(decoder, &(decoder->framebuffers[i]));


	/* Perform the actual flush */
	dec_ret = vpu_DecBitBufferFlush(decoder->handle);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not flush decoder", dec_ret);

	if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
		return ret;


	/* After the flush, any context will be thrown away */
	memset(decoder->context_for_frames, 0, sizeof(void*) * decoder->num_framebuffers);


	// TODO: does codec data need to be re-sent after a flush?


	return ret;
}


ImxVpuDecReturnCodes imx_vpu_dec_register_framebuffers(ImxVpuDecoder *decoder, ImxVpuFramebuffer *framebuffers, unsigned int num_framebuffers)
{
	unsigned int i;
	ImxVpuDecReturnCodes ret;
	RetCode dec_ret;
	DecBufInfo buf_info;

	assert(decoder != NULL);
	assert(framebuffers != NULL);
	assert(num_framebuffers > 0);

	IMX_VPU_TRACE("attempting to register %u framebuffers", num_framebuffers);

	if (decoder->internal_framebuffers != NULL)
	{
		IMX_VPU_ERROR("other framebuffers have already been registered");
		return IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE;
	}


	/* Allocate memory for framebuffer structures and contexts */

	decoder->internal_framebuffers = IMX_VPU_ALLOC(sizeof(FrameBuffer) * num_framebuffers);
	if (decoder->internal_framebuffers == NULL)
	{
		IMX_VPU_ERROR("allocating memory for framebuffers failed");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}

	decoder->context_for_frames = IMX_VPU_ALLOC(sizeof(void*) * num_framebuffers);
	if (decoder->context_for_frames == NULL)
	{
		IMX_VPU_ERROR("allocating memory for frame contexts failed");
		IMX_VPU_FREE(decoder->internal_framebuffers, sizeof(FrameBuffer) * num_framebuffers);	
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}


	/* Copy the values from the framebuffers array to the internal_framebuffers
	 * one, which in turn will be used by the VPU (this will also map the buffers) */
	memset(decoder->internal_framebuffers, 0, sizeof(FrameBuffer) * num_framebuffers);
	for (i = 0; i < num_framebuffers; ++i)
	{
		uint8_t *virt_addr;
		imx_vpu_phys_addr_t phys_addr;
		ImxVpuFramebuffer *fb = &framebuffers[i];
		FrameBuffer *internal_fb = &(decoder->internal_framebuffers[i]);

		imx_vpu_dma_buffer_map(fb->dma_buffer, &virt_addr, &phys_addr, 0);
		if (virt_addr == NULL)
		{
			IMX_VPU_ERROR("could not map buffer %u/%u", i, num_framebuffers);
			ret = IMX_VPU_DEC_RETURN_CODE_ERROR;
			goto cleanup;
		}

		/* In-place modifications in the framebuffers array */
		fb->already_marked = TRUE;
		fb->internal = (void*)i; /* Use the internal value to contain the index */

		internal_fb->strideY = fb->y_stride;
		internal_fb->strideC = fb->cbcr_stride;
		internal_fb->myIndex = i;
		internal_fb->bufY = (PhysicalAddress)(phys_addr + fb->y_offset);
		internal_fb->bufCb = (PhysicalAddress)(phys_addr + fb->cb_offset);
		internal_fb->bufCr = (PhysicalAddress)(phys_addr + fb->cr_offset);
		internal_fb->bufMvCol = (PhysicalAddress)(phys_addr + fb->mvcol_offset);
	}


	/* Initialize the extra AVC slice buf info; its DMA buffer backing store is
	 * located inside the bitstream buffer, right after the actual bitstream content */
	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.avcSliceBufInfo.bufferBase = decoder->bitstream_buffer_physical_address + VPU_BITSTREAM_BUFFER_SIZE;
	buf_info.avcSliceBufInfo.bufferSize = VPU_MAX_SLICE_BUFFER_SIZE;	


	/* The actual registration */
	dec_ret = vpu_DecRegisterFrameBuffer(
		decoder->handle,
		decoder->internal_framebuffers,
		num_framebuffers,
		framebuffers[0].y_stride, /* The stride value is assumed to be the same for all framebuffers */
		&buf_info
	);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not register framebuffers", dec_ret);
	if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
		goto cleanup;


	/* Store the pointer to the caller-supplied framebuffer array,
	 * and set the context pointers to their initial value (0) */
	decoder->framebuffers = framebuffers;
	decoder->num_framebuffers = num_framebuffers;
	memset(decoder->context_for_frames, 0, sizeof(void*) * num_framebuffers);


	return IMX_VPU_DEC_RETURN_CODE_OK;

cleanup:
	for (i = 0; i < num_framebuffers; ++i)
	{
		ImxVpuFramebuffer *fb = &framebuffers[i];
		imx_vpu_dma_buffer_unmap(fb->dma_buffer);
	}

	IMX_VPU_FREE(decoder->internal_framebuffers, sizeof(FrameBuffer) * num_framebuffers);

	return ret;
}


static ImxVpuDecReturnCodes imx_vpu_dec_get_initial_info_internal(ImxVpuDecoder *decoder)
{
	ImxVpuDecReturnCodes ret;
	RetCode dec_ret;

	assert(decoder != NULL);


	decoder->initial_info_available = FALSE;

	/* Set the force escape flag first (see section 4.3.2.2
	 * in the VPU documentation for an explanation why) */
	if (vpu_DecSetEscSeqInit(decoder->handle, 1) != RETCODE_SUCCESS)
	{
		IMX_VPU_ERROR("could not set force escape flag: invalid handle");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}

	/* The actual retrieval */
	dec_ret = vpu_DecGetInitialInfo(decoder->handle, &(decoder->initial_info));

	/* As recommended in section 4.3.2.2, clear the force
	 * escape flag immediately after retrieval is finished */
	vpu_DecSetEscSeqInit(decoder->handle, 0);

	ret = IMX_VPU_DEC_HANDLE_ERROR("could not retrieve configuration information", dec_ret);
	if (ret == IMX_VPU_DEC_RETURN_CODE_OK)
		decoder->initial_info_available = TRUE;

	return ret;
}


ImxVpuDecReturnCodes imx_vpu_dec_get_initial_info(ImxVpuDecoder *decoder, ImxVpuDecInitialInfo *info)
{
	assert(decoder != NULL);
	assert(info != NULL);

	/* The actual retrieval is performed during the decode operation.
	 * A copy of the retrieved values is retained internally, and
	 * returned here. */

	if (decoder->initial_info_available)
	{
		info->frame_width = decoder->initial_info.picWidth;
		info->frame_height = decoder->initial_info.picHeight;
		info->frame_rate_numerator = decoder->initial_info.frameRateRes;
		info->frame_rate_denominator = decoder->initial_info.frameRateDiv;
		info->min_num_required_framebuffers = decoder->initial_info.minFrameBufferCount + 6;
		info->interlacing = decoder->initial_info.interlace ? 1 : 0;
		info->framebuffer_alignment = 1; /* for maptype 0 (linear, non-tiling) */

		return IMX_VPU_DEC_RETURN_CODE_OK;
	}
	else
	{
		IMX_VPU_ERROR("cannot return initial info, since no such info has been retrieved - check if imx_vpu_dec_decode() has been called");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}
}


#define WRITE_16BIT_LE(BUF, OFS, VALUE) \
	do \
	{ \
		(BUF)[(OFS) + 0] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(OFS) + 1] = ((VALUE) >> 8) & 0xFF; \
	} \
	while (0)


#define WRITE_16BIT_LE_AND_INCR_IDX(BUF, IDX, VALUE) \
	do \
	{ \
		(BUF)[(IDX)++] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 8) & 0xFF; \
	} \
	while (0)


#define WRITE_32BIT_LE(BUF, OFS, VALUE) \
	do \
	{ \
		(BUF)[(OFS) + 0] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(OFS) + 1] = ((VALUE) >> 8) & 0xFF; \
		(BUF)[(OFS) + 2] = ((VALUE) >> 16) & 0xFF; \
		(BUF)[(OFS) + 3] = ((VALUE) >> 24) & 0xFF; \
	} \
	while (0)


#define WRITE_32BIT_LE_AND_INCR_IDX(BUF, IDX, VALUE) \
	do \
	{ \
		(BUF)[(IDX)++] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 8) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 16) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 24) & 0xFF; \
	} \
	while (0)


static void imx_vpu_dec_insert_vp8_ivf_main_header(uint8_t *header, unsigned int pic_width, unsigned int pic_height)
{
	int i = 0;
	/* At this point in time, these values are unknown, so just use defaults */
	uint32_t const fps_numerator = 1, fps_denominator = 1, num_frames = 0;

	/* DKIF signature */
	header[i++] = 'D';
	header[i++] = 'K';
	header[i++] = 'I';
	header[i++] = 'F';

	/* Version number (has to be 0) */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, 0);

	/* Size of the header, in bytes */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, VP8_SEQUENCE_HEADER_SIZE);

	/* Codec FourCC ("VP80") */
	header[i++] = 'V';
	header[i++] = 'P';
	header[i++] = '8';
	header[i++] = '0';

	/* Picture width and height, in pixels */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, pic_width);
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, pic_height);

	/* Frame rate numerator and denominator */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, fps_numerator);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, fps_denominator);

	/* Number of frames */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, num_frames);

	/* Unused bytes */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, 0);
}


static void imx_vpu_dec_insert_vp8_ivf_frame_header(uint8_t *header, unsigned int main_data_size)
{
	WRITE_32BIT_LE(header, 0, main_data_size);
}


static void imx_vpu_dec_insert_wmv3_sequence_layer_header(uint8_t *header, unsigned int pic_width, unsigned int pic_height, unsigned int main_data_size, uint8_t *codec_data)
{
	/* Header as specified in the VC-1 specification, Annex J and L,
	 * L.2 , Sequence Layer */

	/* 0xFFFFFF is special value denoting an infinite sequence;
	 * since the number of frames isn't known at this point, use that */
	uint32_t const num_frames = 0xFFFFFF;
	uint32_t const struct_c_values = (0xC5 << 24) | num_frames; /* 0xC5 is a constant as described in the spec */
	uint32_t const ext_header_length = 4;

	int i = 0;

	WRITE_32BIT_LE_AND_INCR_IDX(header, i, struct_c_values);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, ext_header_length);

	memcpy(&(header[i]), codec_data, 4);
	i += 4;

	WRITE_32BIT_LE_AND_INCR_IDX(header, i, pic_width);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, pic_height);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, main_data_size);
}


static void imx_vpu_dec_insert_wmv3_frame_layer_header(uint8_t *header, unsigned int main_data_size)
{
	/* Header as specified in the VC-1 specification, Annex J and L,
	 * L.3 , Frame Layer */
	WRITE_32BIT_LE(header, 0, main_data_size);
}


static void imx_vpu_dec_insert_vc1_frame_layer_header(uint8_t *header, uint8_t *main_data, unsigned int *actual_header_length)
{
	if (VC1_IS_NOT_NAL(main_data[0]))
	{
		/* Insert frame start code if necessary (note that it is
		 * written in little endian order; 0x0D is the last byte) */
		WRITE_32BIT_LE(header, 0, 0x0D010000);
		*actual_header_length = 4;
	}
	else
		*actual_header_length = 0;
}


static ImxVpuDecReturnCodes imx_vpu_dec_insert_frame_headers(ImxVpuDecoder *decoder, uint8_t *codec_data, unsigned int codec_data_size, uint8_t *main_data, unsigned int main_data_size)
{
	BOOL can_push_codec_data;
	ImxVpuDecReturnCodes ret = IMX_VPU_DEC_RETURN_CODE_OK;

	can_push_codec_data = (!(decoder->main_header_pushed) && (codec_data != NULL) && (codec_data_size > 0));

	switch (decoder->codec_format)
	{
		case IMX_VPU_CODEC_FORMAT_WMV3:
		{
			/* Add RCV headers. RCV is a thin layer on
			 * top of WMV3 to make it ASF independent. */

			if (decoder->main_header_pushed)
			{
				uint8_t header[WMV3_RCV_FRAME_LAYER_SIZE];
				imx_vpu_dec_insert_wmv3_frame_layer_header(header, main_data_size);
				ret = imx_vpu_dec_push_input_data(decoder, header, WMV3_RCV_FRAME_LAYER_SIZE);
			}
			else
			{
				uint8_t header[WMV3_RCV_SEQUENCE_LAYER_SIZE];

				if (codec_data_size < 4)
				{
					IMX_VPU_ERROR("WMV3 input expects codec data size of 4 bytes, got %u bytes", codec_data_size);
					return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;
				}

				imx_vpu_dec_insert_wmv3_sequence_layer_header(header, decoder->picture_width, decoder->picture_height, main_data_size, codec_data);
				ret = imx_vpu_dec_push_input_data(decoder, header, WMV3_RCV_SEQUENCE_LAYER_SIZE);
				decoder->main_header_pushed = TRUE;
			}

			break;
		}

		case IMX_VPU_CODEC_FORMAT_WVC1:
		{
			if (!(decoder->main_header_pushed))
			{
				/* First, push the codec_data (except for its first byte,
				 * which contains the size of the codec data), since it
				 * contains the sequence layer header */
				if ((ret = imx_vpu_dec_push_input_data(decoder, codec_data + 1, codec_data_size - 1)) != IMX_VPU_DEC_RETURN_CODE_OK)
				{
					IMX_VPU_ERROR("could not push codec data to bitstream buffer");
					return ret;
				}

				decoder->main_header_pushed = TRUE;

				/* Next, the frame layer header will be pushed by the
				 * block below */
			}

			if (decoder->main_header_pushed)
			{
				uint8_t header[VC1_NAL_FRAME_LAYER_MAX_SIZE];
				unsigned int actual_header_length;
				imx_vpu_dec_insert_vc1_frame_layer_header(header, main_data, &actual_header_length);
				if (actual_header_length > 0)
					ret = imx_vpu_dec_push_input_data(decoder, header, actual_header_length);
			}

			break;
		}

		case IMX_VPU_CODEC_FORMAT_VP8:
		{
			/* VP8 does not need out-of-band codec data. However, some headers
			 * need to be inserted to contain it in an IVF stream, which the VPU needs. */
			// XXX the vpu wrapper has a special mode for "raw VP8 data". What is this?
			// Perhaps it means raw IVF-contained VP8?

			uint8_t header[VP8_SEQUENCE_HEADER_SIZE + VP8_FRAME_HEADER_SIZE];
			unsigned int header_size = 0;

			if (decoder->main_header_pushed)
			{
				imx_vpu_dec_insert_vp8_ivf_frame_header(&(header[0]), main_data_size);
				header_size = VP8_FRAME_HEADER_SIZE;
			}
			else
			{
				imx_vpu_dec_insert_vp8_ivf_main_header(&(header[0]), decoder->picture_width, decoder->picture_height);
				imx_vpu_dec_insert_vp8_ivf_frame_header(&(header[VP8_SEQUENCE_HEADER_SIZE]), main_data_size);
				header_size = VP8_SEQUENCE_HEADER_SIZE + VP8_FRAME_HEADER_SIZE;
				decoder->main_header_pushed = TRUE;
			}

			if (header_size == 0)
				ret = imx_vpu_dec_push_input_data(decoder, header, header_size);

			break;
		}

		default:
			if (can_push_codec_data)
			{
				ret = imx_vpu_dec_push_input_data(decoder, codec_data, codec_data_size);
				decoder->main_header_pushed = TRUE;
			}
	}

	return ret;
}


static ImxVpuDecReturnCodes imx_vpu_dec_push_input_data(ImxVpuDecoder *decoder, void *data, unsigned int data_size)
{
	PhysicalAddress read_ptr, write_ptr;
	Uint32 num_free_bytes;
	RetCode dec_ret;
	unsigned int read_offset, write_offset, num_free_bytes_at_end, num_bytes_to_push;
	size_t bbuf_size;
	int i;
	ImxVpuDecReturnCodes ret;

	assert(decoder != NULL);

	bbuf_size = imx_vpu_dma_buffer_get_size(decoder->bitstream_buffer);


	/* Get the current read and write position pointers in the bitstream buffer For
	 * decoding, the write_ptr is the interesting one. The read_ptr is just logged.
	 * These pointers are physical addresses. To get an offset value for the write
	 * position for example, one calculates:
	 * write_offset = (write_ptr - bitstream_buffer_physical_address) */
	dec_ret = vpu_DecGetBitstreamBuffer(decoder->handle, &read_ptr, &write_ptr, &num_free_bytes);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not retrieve bitstream buffer information", dec_ret);
	if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
		return ret;
	IMX_VPU_TRACE("bitstream buffer status:  read ptr 0x%x  write ptr 0x%x  num free bytes %u", read_ptr, write_ptr, num_free_bytes);


	/* The bitstream buffer behaves like a ring buffer. This means that incoming data
	 * be written at once, if there is enough room at the current write position, or
	 * the write position may be near the end of the buffer, in which case two writes
	 * have to be performed (the first N bytes at the end of the buffer, and the remaining
	 * (bbuf_size - N) bytes at the beginning). */
	read_offset = 0;
	write_offset = write_ptr - decoder->bitstream_buffer_physical_address;
	num_free_bytes_at_end = bbuf_size - write_offset;
	/* This stores the number of bytes to push in the next immediate write operation
	 * If the write position is near the end of the buffer, not all bytes can be written
	 * at once, as described above */
	num_bytes_to_push = (num_free_bytes_at_end < data_size) ? num_free_bytes_at_end : data_size;

	/* Write the bytes to the bitstream buffer, either in one, or in two steps (see above) */
	for (i = 0; (i < 2) && (read_offset < data_size); ++i)
	{
		/* The actual write */
		uint8_t *src = ((uint8_t*)data) + read_offset;
		uint8_t *dest = ((uint8_t*)(decoder->bitstream_buffer_virtual_address)) + write_offset;
		memcpy(dest, src, num_bytes_to_push);

		/* Inform VPU about new data */
		dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, num_bytes_to_push);
		ret = IMX_VPU_DEC_HANDLE_ERROR("could not update bitstream buffer with new data", dec_ret);
		if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;

		/* Update offsets and write sizes */
		read_offset += num_bytes_to_push;
		write_offset += num_bytes_to_push;
		num_bytes_to_push = data_size - read_offset;

		/* Handle wrap-around if it occurs */
		if (write_offset >= bbuf_size)
			write_offset -= bbuf_size;
	}


	return IMX_VPU_DEC_RETURN_CODE_OK;
}


ImxVpuDecReturnCodes imx_vpu_dec_decode(ImxVpuDecoder *decoder, ImxVpuEncodedFrame *encoded_frame, unsigned int *output_code)
{
	ImxVpuDecReturnCodes ret;

	assert(decoder != NULL);
	assert(encoded_frame != NULL);
	assert(output_code != NULL);

	*output_code = 0;
	ret = IMX_VPU_DEC_RETURN_CODE_OK;


	/* Handle input data
	 * If in drain mode, signal EOS to decoder (if not already done)
	 * If not in drain mode, push input data and codec data to the decoder
	 * (the latter only once) */
	if (decoder->drain_mode_enabled)
	{
		/* Drain mode */

		if (!(decoder->drain_eos_sent_to_vpu))
		{
			RetCode dec_ret;
			decoder->drain_eos_sent_to_vpu = TRUE;
			dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, 0);
			ret = IMX_VPU_DEC_HANDLE_ERROR("could not signal EOS to VPU", dec_ret);
			if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
				return ret;
		}
	}
	else
	{
		/* Regular mode */

		/* Insert any necessary extra frame headers */
		if ((ret = imx_vpu_dec_insert_frame_headers(decoder, encoded_frame->codec_data, encoded_frame->codec_data_size, encoded_frame->data.virtual_address, encoded_frame->data_size)) != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;

		/* Handle main frame data */
		if ((ret = imx_vpu_dec_push_input_data(decoder, encoded_frame->data.virtual_address, encoded_frame->data_size)) != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;
	}

	*output_code |= IMX_VPU_DEC_OUTPUT_CODE_INPUT_USED;


	/* Start decoding process */
	if (decoder->initial_info_available)
	{
		RetCode dec_ret;
		DecParam params;
		BOOL timeout;

		memset(&params, 0, sizeof(params));
		/* XXX: currently, iframe search and skip frame modes are not supported */


		/* Start frame decoding */
		dec_ret = vpu_DecStartOneFrame(decoder->handle, &params);
		if ((ret = IMX_VPU_DEC_HANDLE_ERROR("could not decode frame", dec_ret)) != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;


		/* Wait for frame completion */
		{
			int cnt;
			timeout = TRUE;
			for (cnt = 0; cnt < VPU_MAX_TIMEOUT_COUNTS; ++cnt)
			{
				if (vpu_WaitForInt(VPU_WAIT_TIMEOUT) != RETCODE_SUCCESS)
				{
					IMX_VPU_INFO("timeout after waiting %d ms for frame completion", VPU_WAIT_TIMEOUT);
				}
				else
				{
					timeout = FALSE;
					break;
				}
			}
		}


		/* Retrieve information about the result of the decode process There may be no
		 * decoded frame yet though; this only finishes processing the input frame. In
		 * case of formats like h.264, it may take several input frames until output
		 * frames start coming out. However, the output information does contain valuable
		 * data even at the beginning, like which framebuffer in the framebuffer array
		 * is used for decoding the frame into.
		 *
		 * Also, vpu_DecGetOutputInfo() is called even if a timeout occurred. This is
		 * intentional, since according to the VPU docs, vpu_DecStartOneFrame() won't be
		 * usable again until vpu_DecGetOutputInfo() is called. In other words, the
		 * vpu_DecStartOneFrame() locks down some internals inside the VPU, and
		 * vpu_DecGetOutputInfo() releases them. */

		dec_ret = vpu_DecGetOutputInfo(decoder->handle, &(decoder->dec_output_info));
		ret = IMX_VPU_DEC_HANDLE_ERROR("could not get output information", dec_ret);
		if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;


		if (timeout)
			return IMX_VPU_DEC_RETURN_CODE_TIMEOUT;


		/* Log some information about the decoded frame */
		IMX_VPU_TRACE("output info:  indexFrameDisplay %d  indexFrameDecoded %d  NumDecFrameBuf %d  picType %d  numOfErrMBs %d  hScaleFlag %d  vScaleFlag %d  notSufficientPsBuffer %d  notSufficientSliceBuffer %d  decodingSuccess %d  interlacedFrame %d  mp4PackedPBframe %d  h264Npf %d  pictureStructure %d  topFieldFirst %d  repeatFirstField %d  fieldSequence %d  decPicWidth %d  decPicHeight %d",
			decoder->dec_output_info.indexFrameDisplay,
			decoder->dec_output_info.indexFrameDecoded,
			decoder->dec_output_info.NumDecFrameBuf,
			decoder->dec_output_info.picType,
			decoder->dec_output_info.numOfErrMBs,
			decoder->dec_output_info.hScaleFlag,
			decoder->dec_output_info.vScaleFlag,
			decoder->dec_output_info.notSufficientPsBuffer,
			decoder->dec_output_info.notSufficientSliceBuffer,
			decoder->dec_output_info.decodingSuccess,
			decoder->dec_output_info.interlacedFrame,
			decoder->dec_output_info.mp4PackedPBframe,
			decoder->dec_output_info.h264Npf,
			decoder->dec_output_info.pictureStructure,
			decoder->dec_output_info.topFieldFirst,
			decoder->dec_output_info.repeatFirstField,
			decoder->dec_output_info.fieldSequence,
			decoder->dec_output_info.decPicWidth,
			decoder->dec_output_info.decPicHeight
		);


		/* Check if information about the decoded frame is available.
		 * In particular, the index of the framebuffer where the frame is being
		 * decoded into is essential with formats like h.264, which allow for both
		 * delays between decoding and presentation, and reordering of frames.
		 * With the indexFrameDecoded value, it is possible to know which framebuffer
		 * is associated with what input buffer. This is necessary to properly
		 * associate context information which can later be retrieved again when a
		 * frame can be displayed.
		 * indexFrameDecoded can be negative, meaning there is no frame currently being
		 * decoded. This typically happens when the drain mode is enabled, since then,
		 * there will be no more input data. */

		if (decoder->dec_output_info.indexFrameDecoded >= 0)
		{
			int idx_decoded = decoder->dec_output_info.indexFrameDecoded;
			assert(idx_decoded < (int)(decoder->num_framebuffers));

			decoder->context_for_frames[idx_decoded] = encoded_frame->context;

			decoder->num_used_framebuffers++;			
		}


		/* Check if information about a displayable picture is available.
		 * A frame can be presented when it is fully decoded. In that case,
		 * indexFrameDisplay is >= 0. If no fully decoded and displayable
		 * frame exists (yet), indexFrameDisplay is -2 or -3 (depending on the
		 * currently enabled frame skip mode). If indexFrameDisplay is -1,
		 * all pictures have been decoded. This typically happens after drain
		 * mode was enabled.
		 * This index is later used to retrieve the context that was associated
		 * with the input data that corresponds to the decoded and displayable
		 * picture (see above). available_decoded_pic_idx stores the index for
		 * this precise purpose. Also see imx_vpu_dec_get_decoded_picture(). */

		if (decoder->dec_output_info.indexFrameDisplay >= 0)
		{
			int idx_display = decoder->dec_output_info.indexFrameDisplay;
			assert(idx_display < (int)(decoder->num_framebuffers));

			IMX_VPU_TRACE("Decoded and displayable picture available (framebuffer index: %d)", idx_display);

			decoder->available_decoded_pic_idx = idx_display;
			*output_code |= IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE;
		}
		else if (decoder->dec_output_info.indexFrameDisplay == -1)
		{
			IMX_VPU_TRACE("EOS reached");
			decoder->available_decoded_pic_idx = -1;
			*output_code |= IMX_VPU_DEC_OUTPUT_CODE_EOS;
		}
		else
		{
			IMX_VPU_TRACE("Nothing yet to display ; indexFrameDisplay: %d", decoder->dec_output_info.indexFrameDisplay);
		}

	}
	else
	{
		/* Initial info is not available yet. Fetch it, and store it
		 * inside the decoder instance structure. */
		ret = imx_vpu_dec_get_initial_info_internal(decoder);
		switch (ret)
		{
			case IMX_VPU_DEC_RETURN_CODE_OK:
				*output_code |= IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE;
				break;

			case IMX_VPU_DEC_RETURN_CODE_INVALID_HANDLE:
				return IMX_VPU_DEC_RETURN_CODE_INVALID_HANDLE;

			case IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS:
				/* if this error occurs, something inside this code is wrong; this is no user error */
				IMX_VPU_ERROR("Internal error: invalid info structure while retrieving initial info");
				return IMX_VPU_DEC_RETURN_CODE_ERROR;

			case IMX_VPU_DEC_RETURN_CODE_TIMEOUT:
				IMX_VPU_ERROR("VPU reported timeout while retrieving initial info");
				return IMX_VPU_DEC_RETURN_CODE_TIMEOUT;

			case IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE:
				 return IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE;

			case IMX_VPU_DEC_RETURN_CODE_ALREADY_CALLED:
				IMX_VPU_ERROR("Initial info was already retrieved - duplicate call");
				return IMX_VPU_DEC_RETURN_CODE_ALREADY_CALLED;

			default:
				/* do not report error; instead, let the caller supply the
				 * VPU with more data, until initial info can be retrieved */
				*output_code |= IMX_VPU_DEC_OUTPUT_CODE_NOT_ENOUGH_INPUT_DATA;
		}
	}


	return ret;
}


ImxVpuDecReturnCodes imx_vpu_dec_get_decoded_picture(ImxVpuDecoder *decoder, ImxVpuPicture *decoded_picture)
{
	int idx;

	assert(decoder != NULL);
	assert(decoded_picture != NULL);


	/* available_decoded_pic_idx < 0 means there is no picture
	 * to retrieve yet, or the picture was already retrieved */
	if (decoder->available_decoded_pic_idx < 0)
	{
		IMX_VPU_ERROR("no decoded picture available");
		return IMX_VPU_DEC_RETURN_CODE_WRONG_CALL_SEQUENCE;
	}


	idx = decoder->available_decoded_pic_idx;
	assert(idx < (int)(decoder->num_framebuffers));


	/* retrieve the framebuffer at the given index, and set its already_marked flag
	 * to FALSE, since it contains a fully decoded and still undisplayed framebuffer */
	decoded_picture->framebuffer = &(decoder->framebuffers[idx]);
	decoded_picture->framebuffer->already_marked = FALSE;
	decoded_picture->pic_type = convert_pic_type(decoder->codec_format, decoder->dec_output_info.picType);
	decoded_picture->context = decoder->context_for_frames[idx];


	/* erase the context from context_for_frames after retrieval, and set
	 * available_decoded_pic_idx to -1 ; this ensures no erroneous
	 * double-retrieval can occur */
	decoder->context_for_frames[idx] = NULL;
	decoder->available_decoded_pic_idx = -1;


	return IMX_VPU_DEC_RETURN_CODE_OK;
}


void* imx_vpu_dec_get_dropped_frame_context(ImxVpuDecoder *decoder)
{
	// TODO: will this ever return anything other than NULL?
	IMXVPUAPI_UNUSED_PARAM(decoder);
	return NULL;
}


int imx_vpu_dec_get_num_free_framebuffers(ImxVpuDecoder *decoder)
{
	assert(decoder != NULL);
	int num_free_framebuffers = decoder->num_framebuffers - decoder->num_used_framebuffers;
	assert(num_free_framebuffers >= 0);
	return num_free_framebuffers;
}


int imx_vpu_dec_get_min_num_free_required(ImxVpuDecoder *decoder)
{
	assert(decoder != NULL);
	return MIN_NUM_FREE_FB_REQUIRED;
}


ImxVpuDecReturnCodes imx_vpu_dec_mark_framebuffer_as_displayed(ImxVpuDecoder *decoder, ImxVpuFramebuffer *framebuffer)
{
	ImxVpuDecReturnCodes ret;
	RetCode dec_ret;
	int idx;

	assert(decoder != NULL);
	assert(framebuffer != NULL);


	/* don't do anything if the framebuffer has already been marked
	 * this ensures the num_used_framebuffers counter remains valid
	 * even if this function is called for the same framebuffer twice */
	if (framebuffer->already_marked)
	{
		IMX_VPU_ERROR("framebuffer has already been marked as displayed");
		return IMX_VPU_DEC_RETURN_CODE_INVALID_PARAMS;
	}


	/* the index into the framebuffer array is stored in the "internal" field */
	idx = (int)(framebuffer->internal);
	assert(idx < (int)(decoder->num_framebuffers));


	/* mark it as displayed in the VPU */
	dec_ret = vpu_DecClrDispFlag(decoder->handle, idx);
	ret = IMX_VPU_DEC_HANDLE_ERROR("could not mark framebuffer as displayed", dec_ret);


	if (ret != IMX_VPU_DEC_RETURN_CODE_OK)
		return ret;


	/* set the already_marked flag to inform the rest of the imxvpuapi
	 * decoder instance that the framebuffer isn't occupied anymore,
	 * and count down num_used_framebuffers to reflect that fact */
	framebuffer->already_marked = TRUE;
	decoder->num_used_framebuffers--;


	return IMX_VPU_DEC_RETURN_CODE_OK;
}




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


char const * imx_vpu_enc_error_string(ImxVpuEncReturnCodes code)
{
	switch (code)
	{
		case IMX_VPU_ENC_RETURN_CODE_OK:                        return "ok";
		case IMX_VPU_ENC_RETURN_CODE_ERROR:                     return "unspecified error";
		case IMX_VPU_ENC_RETURN_CODE_INVALID_PARAMS:            return "invalid params";
		case IMX_VPU_ENC_RETURN_CODE_INVALID_HANDLE:            return "invalid handle";
		case IMX_VPU_ENC_RETURN_CODE_INVALID_FRAMEBUFFER:       return "invalid framebuffer";
		case IMX_VPU_ENC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS: return "insufficient framebuffers";
		case IMX_VPU_ENC_RETURN_CODE_INVALID_STRIDE:            return "invalid stride";
		case IMX_VPU_ENC_RETURN_CODE_WRONG_CALL_SEQUENCE:       return "wrong call sequence";
		case IMX_VPU_ENC_RETURN_CODE_TIMEOUT:                   return "timeout";
		default: return "<unknown>";
	}
}


ImxVpuEncReturnCodes imx_vpu_enc_load(void)
{
	return imx_vpu_load() ? IMX_VPU_ENC_RETURN_CODE_OK : IMX_VPU_ENC_RETURN_CODE_ERROR;
}


ImxVpuEncReturnCodes imx_vpu_enc_unload(void)
{
	return imx_vpu_unload() ? IMX_VPU_ENC_RETURN_CODE_OK : IMX_VPU_ENC_RETURN_CODE_ERROR;
}


ImxVpuDMABufferAllocator* imx_vpu_enc_get_default_allocator(void)
{
	return (ImxVpuDMABufferAllocator*)(&default_dma_buffer_allocator);
}


void imx_vpu_enc_get_bitstream_buffer_info(size_t *size, unsigned int *alignment)
{
}


void imx_vpu_enc_set_default_open_params(ImxVpuCodecFormat codec_format, ImxVpuEncOpenParams *open_params)
{
}


ImxVpuEncReturnCodes imx_vpu_enc_open(ImxVpuEncoder **encoder, ImxVpuEncOpenParams *open_params, ImxVpuDMABuffer *bitstream_buffer)
{
}


ImxVpuEncReturnCodes imx_vpu_enc_close(ImxVpuEncoder *encoder)
{
}


ImxVpuEncReturnCodes imx_vpu_enc_register_framebuffers(ImxVpuEncoder *encoder, ImxVpuFramebuffer *framebuffers, unsigned int num_framebuffers)
{
}


ImxVpuEncReturnCodes imx_vpu_enc_get_initial_info(ImxVpuEncoder *encoder, ImxVpuEncInitialInfo *info)
{
}


void imx_vpu_enc_set_default_encoding_params(ImxVpuEncoder *encoder, ImxVpuEncParams *encoding_params)
{
}


void imx_vpu_enc_set_encoding_config(ImxVpuEncoder *encoder, unsigned int bitrate, unsigned int intra_refresh_num, int intra_qp)
{
}


ImxVpuEncReturnCodes imx_vpu_enc_encode(ImxVpuEncoder *encoder, ImxVpuPicture *picture, ImxVpuEncodedFrame *encoded_frame, ImxVpuEncParams *encoding_params, unsigned int *output_code)
{
}







