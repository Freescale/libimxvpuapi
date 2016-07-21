/* Simplified API for JPEG en- and decoding with the Freescale i.MX SoC
 * Copyright (C) 2014 Carlos Rafael Giani
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
#include <string.h>
#include "imxvpuapi_jpeg.h"
#include "imxvpuapi_priv.h"




static void imx_vpu_jpeg_deallocate_dma_buffers(ImxVpuDMABuffer **dma_buffers, unsigned int num_dma_buffers)
{
	unsigned int i;
	for (i = 0; i < num_dma_buffers; ++i)
	{
		if (dma_buffers[i] != NULL)
		{
			imx_vpu_dma_buffer_deallocate(dma_buffers[i]);
			dma_buffers[i] = NULL;
		}
	}
}


/******************
 ** JPEG DECODER **
 ******************/


struct _ImxVpuJPEGDecoder
{
	ImxVpuDecoder *decoder;

	ImxVpuDMABufferAllocator *dma_buffer_allocator;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuDecInitialInfo initial_info;

	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers, num_extra_framebuffers;
	ImxVpuFramebufferSizes calculated_sizes;

	ImxVpuRawFrame raw_frame;
};


static int initial_info_callback(ImxVpuDecoder *decoder, ImxVpuDecInitialInfo *new_initial_info, unsigned int output_code, void *user_data);
static void imx_vpu_jpeg_dec_deallocate_framebuffers(ImxVpuJPEGDecoder *jpeg_decoder);


static int initial_info_callback(ImxVpuDecoder *decoder, ImxVpuDecInitialInfo *new_initial_info, unsigned int output_code, void *user_data)
{
	unsigned int i;
	ImxVpuDecReturnCodes ret;
	ImxVpuJPEGDecoder *jpeg_decoder = (ImxVpuJPEGDecoder *)user_data;

	IMXVPUAPI_UNUSED_PARAM(decoder);
	IMXVPUAPI_UNUSED_PARAM(output_code);

	imx_vpu_jpeg_dec_deallocate_framebuffers(jpeg_decoder);

	jpeg_decoder->initial_info = *new_initial_info;
	IMX_VPU_DEBUG(
		"initial info:  size: %ux%u pixel  rate: %u/%u  min num required framebuffers: %u  interlacing: %d  framebuffer alignment: %u  color format: %s",
		new_initial_info->frame_width,
		new_initial_info->frame_height,
		new_initial_info->frame_rate_numerator,
		new_initial_info->frame_rate_denominator,
		new_initial_info->min_num_required_framebuffers,
		new_initial_info->interlacing,
		new_initial_info->framebuffer_alignment,
		imx_vpu_color_format_string(new_initial_info->color_format)
	);

	jpeg_decoder->num_framebuffers = new_initial_info->min_num_required_framebuffers + jpeg_decoder->num_extra_framebuffers;

	imx_vpu_calc_framebuffer_sizes(new_initial_info->color_format, new_initial_info->frame_width, new_initial_info->frame_height, new_initial_info->framebuffer_alignment, new_initial_info->interlacing, 0, &(jpeg_decoder->calculated_sizes));
	IMX_VPU_DEBUG(
		"calculated sizes:  frame width&height: %dx%d  Y stride: %u  CbCr stride: %u  Y size: %u  CbCr size: %u  MvCol size: %u  total size: %u",
		jpeg_decoder->calculated_sizes.aligned_frame_width, jpeg_decoder->calculated_sizes.aligned_frame_height,
		jpeg_decoder->calculated_sizes.y_stride, jpeg_decoder->calculated_sizes.cbcr_stride,
		jpeg_decoder->calculated_sizes.y_size, jpeg_decoder->calculated_sizes.cbcr_size, jpeg_decoder->calculated_sizes.mvcol_size,
		jpeg_decoder->calculated_sizes.total_size
	);

	jpeg_decoder->framebuffers = IMX_VPU_ALLOC(sizeof(ImxVpuFramebuffer) * jpeg_decoder->num_framebuffers);
	jpeg_decoder->fb_dmabuffers = IMX_VPU_ALLOC(sizeof(ImxVpuDMABuffer *) * jpeg_decoder->num_framebuffers);

	memset(jpeg_decoder->framebuffers, 0, sizeof(ImxVpuFramebuffer) * jpeg_decoder->num_framebuffers);
	memset(jpeg_decoder->fb_dmabuffers, 0, sizeof(ImxVpuDMABuffer *) * jpeg_decoder->num_framebuffers);

	for (i = 0; i < jpeg_decoder->num_framebuffers; ++i)
	{
		jpeg_decoder->fb_dmabuffers[i] = imx_vpu_dma_buffer_allocate(jpeg_decoder->dma_buffer_allocator, jpeg_decoder->calculated_sizes.total_size, jpeg_decoder->initial_info.framebuffer_alignment, 0);
		if (jpeg_decoder->fb_dmabuffers[i] == NULL)
		{
			IMX_VPU_ERROR("could not allocate DMA buffer for framebuffer #%u", i);
			goto error;
		}

		imx_vpu_fill_framebuffer_params(&(jpeg_decoder->framebuffers[i]), &(jpeg_decoder->calculated_sizes), jpeg_decoder->fb_dmabuffers[i], 0);
	}

	if ((ret = imx_vpu_dec_register_framebuffers(jpeg_decoder->decoder, jpeg_decoder->framebuffers, jpeg_decoder->num_framebuffers)) != IMX_VPU_DEC_RETURN_CODE_OK)
	{
		IMX_VPU_ERROR("could not register framebuffers: %s", imx_vpu_dec_error_string(ret));
		goto error;
	}

	return 1;

error:
	imx_vpu_jpeg_deallocate_dma_buffers(jpeg_decoder->fb_dmabuffers, jpeg_decoder->num_framebuffers);
	return 0;
}


ImxVpuDecReturnCodes imx_vpu_jpeg_dec_open(ImxVpuJPEGDecoder **jpeg_decoder, ImxVpuDMABufferAllocator *dma_buffer_allocator, unsigned int num_extra_framebuffers)
{
	ImxVpuDecOpenParams open_params;
	ImxVpuDecReturnCodes ret = IMX_VPU_DEC_RETURN_CODE_OK;
	ImxVpuJPEGDecoder *jpegdec = NULL;

	assert(jpeg_decoder != NULL);

	if ((ret = imx_vpu_dec_load()) != IMX_VPU_DEC_RETURN_CODE_OK)
		return ret;

	jpegdec = IMX_VPU_ALLOC(sizeof(ImxVpuJPEGDecoder));
	if (jpegdec == NULL)
	{
		IMX_VPU_ERROR("allocating memory for JPEG decoder object failed");
		return IMX_VPU_DEC_RETURN_CODE_ERROR;
	}

	memset(jpegdec, 0, sizeof(ImxVpuJPEGDecoder));

	jpegdec->dma_buffer_allocator = (dma_buffer_allocator != NULL) ? dma_buffer_allocator : imx_vpu_dec_get_default_allocator();
	jpegdec->num_extra_framebuffers = num_extra_framebuffers;

	memset(&open_params, 0, sizeof(open_params));
	open_params.codec_format = IMX_VPU_CODEC_FORMAT_MJPEG;
	open_params.frame_width = 0;
	open_params.frame_height = 0;

	imx_vpu_dec_get_bitstream_buffer_info(&(jpegdec->bitstream_buffer_size), &(jpegdec->bitstream_buffer_alignment));
	jpegdec->bitstream_buffer = imx_vpu_dma_buffer_allocate(jpegdec->dma_buffer_allocator, jpegdec->bitstream_buffer_size, jpegdec->bitstream_buffer_alignment, 0);
	if (jpegdec->bitstream_buffer == NULL)
	{
		IMX_VPU_ERROR("could not allocate DMA buffer for bitstream buffer with %u bytes and alignment %u", jpegdec->bitstream_buffer_size, jpegdec->bitstream_buffer_alignment);
		ret = IMX_VPU_DEC_RETURN_CODE_ERROR;
		goto error;
	}

	if ((ret = imx_vpu_dec_open(&(jpegdec->decoder), &open_params, jpegdec->bitstream_buffer, initial_info_callback, jpegdec)) != IMX_VPU_DEC_RETURN_CODE_OK)
		goto error;

	*jpeg_decoder = jpegdec;

	return IMX_VPU_DEC_RETURN_CODE_OK;

error:
	if (jpegdec != NULL)
	{
		if (jpegdec->bitstream_buffer != NULL)
			imx_vpu_dma_buffer_deallocate(jpegdec->bitstream_buffer);
		IMX_VPU_FREE(jpegdec, sizeof(ImxVpuJPEGDecoder));
	}

	return ret;
}


ImxVpuDecReturnCodes imx_vpu_jpeg_dec_close(ImxVpuJPEGDecoder *jpeg_decoder)
{
	assert(jpeg_decoder != NULL);
	assert(jpeg_decoder->decoder != NULL);

	imx_vpu_dec_close(jpeg_decoder->decoder);

	imx_vpu_jpeg_dec_deallocate_framebuffers(jpeg_decoder);

	if (jpeg_decoder->bitstream_buffer != NULL)
		imx_vpu_dma_buffer_deallocate(jpeg_decoder->bitstream_buffer);

	IMX_VPU_FREE(jpeg_decoder, sizeof(ImxVpuJPEGDecoder));

	return IMX_VPU_DEC_RETURN_CODE_OK;
}


static void imx_vpu_jpeg_dec_deallocate_framebuffers(ImxVpuJPEGDecoder *jpeg_decoder)
{
	assert(jpeg_decoder != NULL);
	assert(jpeg_decoder->decoder != NULL);

	if (jpeg_decoder->framebuffers != NULL)
	{
		IMX_VPU_FREE(jpeg_decoder->framebuffers, sizeof(ImxVpuFramebuffer) * jpeg_decoder->num_framebuffers);
		jpeg_decoder->framebuffers = NULL;
	}

	if (jpeg_decoder->fb_dmabuffers != NULL)
	{
		imx_vpu_jpeg_deallocate_dma_buffers(jpeg_decoder->fb_dmabuffers, jpeg_decoder->num_framebuffers);
		IMX_VPU_FREE(jpeg_decoder->fb_dmabuffers, sizeof(ImxVpuDMABuffer *) * jpeg_decoder->num_framebuffers);
		jpeg_decoder->fb_dmabuffers = NULL;
	}
}


int imx_vpu_jpeg_dec_can_decode(ImxVpuJPEGDecoder *jpeg_decoder)
{
	return imx_vpu_dec_check_if_can_decode(jpeg_decoder->decoder);
}


ImxVpuDecReturnCodes imx_vpu_jpeg_dec_decode(ImxVpuJPEGDecoder *jpeg_decoder, uint8_t const *jpeg_data, size_t const jpeg_data_size)
{
	unsigned int output_code;
	ImxVpuDecReturnCodes ret;
	ImxVpuEncodedFrame encoded_frame;

	assert(jpeg_data != NULL);
	assert(jpeg_data_size > 0);
	assert(jpeg_decoder != NULL);
	assert(jpeg_decoder->decoder != NULL);

	memset(&encoded_frame, 0, sizeof(encoded_frame));
	encoded_frame.data = (uint8_t *)jpeg_data;
	encoded_frame.data_size = jpeg_data_size;

	if ((ret = imx_vpu_dec_decode(jpeg_decoder->decoder, &encoded_frame, &output_code)) != IMX_VPU_DEC_RETURN_CODE_OK)
		return ret;

	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE)
	{
		if ((ret = imx_vpu_dec_get_decoded_frame(jpeg_decoder->decoder, &(jpeg_decoder->raw_frame))) != IMX_VPU_DEC_RETURN_CODE_OK)
			return ret;
	}
	else
	{
		jpeg_decoder->raw_frame.framebuffer = NULL;
	}

	return IMX_VPU_DEC_RETURN_CODE_OK;
}


void imx_vpu_jpeg_dec_get_info(ImxVpuJPEGDecoder *jpeg_decoder, ImxVpuJPEGDecInfo *info)
{
	assert(jpeg_decoder != NULL);
	assert(jpeg_decoder->framebuffers != NULL);
	assert(info != NULL);

	info->aligned_frame_width = jpeg_decoder->calculated_sizes.aligned_frame_width;
	info->aligned_frame_height = jpeg_decoder->calculated_sizes.aligned_frame_height;

	info->actual_frame_width = jpeg_decoder->initial_info.frame_width;
	info->actual_frame_height = jpeg_decoder->initial_info.frame_height;

	info->y_stride = jpeg_decoder->calculated_sizes.y_stride;
	info->cbcr_stride = jpeg_decoder->calculated_sizes.cbcr_stride;

	info->y_size = jpeg_decoder->calculated_sizes.y_size;
	info->cbcr_size = jpeg_decoder->calculated_sizes.cbcr_size;

	info->y_offset = jpeg_decoder->framebuffers[0].y_offset;
	info->cb_offset = jpeg_decoder->framebuffers[0].cb_offset;
	info->cr_offset = jpeg_decoder->framebuffers[0].cr_offset;

	info->framebuffer = jpeg_decoder->raw_frame.framebuffer;

	info->color_format = jpeg_decoder->initial_info.color_format;
}


ImxVpuDecReturnCodes imx_vpu_jpeg_dec_frame_finished(ImxVpuJPEGDecoder *jpeg_decoder, ImxVpuFramebuffer *framebuffer)
{
	assert(framebuffer != NULL);
	assert(jpeg_decoder != NULL);
	assert(jpeg_decoder->decoder != NULL);

	return imx_vpu_dec_mark_framebuffer_as_displayed(jpeg_decoder->decoder, framebuffer);
}




/******************
 ** JPEG ENCODER **
 ******************/


struct _ImxVpuJPEGEncoder
{
	ImxVpuEncoder *encoder;

	ImxVpuDMABufferAllocator *dma_buffer_allocator;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuEncInitialInfo initial_info;

	unsigned int frame_width, frame_height;
	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers;
	ImxVpuFramebufferSizes calculated_sizes;

	unsigned int quality_factor;

	ImxVpuColorFormat color_format;
};


static ImxVpuEncReturnCodes imx_vpu_jpeg_enc_open_internal(ImxVpuJPEGEncoder *jpeg_encoder);
static ImxVpuEncReturnCodes imx_vpu_jpeg_enc_close_internal(ImxVpuJPEGEncoder *jpeg_encoder);
static void imx_vpu_jpeg_enc_deallocate_framebuffers(ImxVpuJPEGEncoder *jpeg_encoder);


static ImxVpuEncReturnCodes imx_vpu_jpeg_enc_open_internal(ImxVpuJPEGEncoder *jpeg_encoder)
{
	unsigned int i;
	ImxVpuEncOpenParams open_params;
	ImxVpuEncReturnCodes ret = IMX_VPU_ENC_RETURN_CODE_OK;

	assert(jpeg_encoder != NULL);
	assert(jpeg_encoder->frame_width > 0);
	assert(jpeg_encoder->frame_height > 0);
	assert(jpeg_encoder->encoder == NULL);

	imx_vpu_enc_set_default_open_params(IMX_VPU_CODEC_FORMAT_MJPEG, &open_params);
	open_params.frame_width = jpeg_encoder->frame_width;
	open_params.frame_height = jpeg_encoder->frame_height;
	open_params.color_format = jpeg_encoder->color_format;
	open_params.codec_params.mjpeg_params.quality_factor = jpeg_encoder->quality_factor;

	if ((ret = imx_vpu_enc_open(&(jpeg_encoder->encoder), &open_params, jpeg_encoder->bitstream_buffer)) != IMX_VPU_ENC_RETURN_CODE_OK)
		goto error;

	if ((ret = imx_vpu_enc_get_initial_info(jpeg_encoder->encoder, &(jpeg_encoder->initial_info))) != IMX_VPU_ENC_RETURN_CODE_OK)
		goto error;

	jpeg_encoder->num_framebuffers = jpeg_encoder->initial_info.min_num_required_framebuffers;
	jpeg_encoder->framebuffers = IMX_VPU_ALLOC(sizeof(ImxVpuFramebuffer) * jpeg_encoder->num_framebuffers);
	jpeg_encoder->fb_dmabuffers = IMX_VPU_ALLOC(sizeof(ImxVpuDMABuffer *) * jpeg_encoder->num_framebuffers);

	memset(jpeg_encoder->framebuffers, 0, sizeof(ImxVpuFramebuffer) * jpeg_encoder->num_framebuffers);
	memset(jpeg_encoder->fb_dmabuffers, 0, sizeof(ImxVpuDMABuffer *) * jpeg_encoder->num_framebuffers);

	imx_vpu_calc_framebuffer_sizes(jpeg_encoder->color_format, jpeg_encoder->frame_width, jpeg_encoder->frame_height, jpeg_encoder->initial_info.framebuffer_alignment, 0, 0, &(jpeg_encoder->calculated_sizes));

	for (i = 0; i < jpeg_encoder->num_framebuffers; ++i)
	{
		jpeg_encoder->fb_dmabuffers[i] = imx_vpu_dma_buffer_allocate(jpeg_encoder->dma_buffer_allocator, jpeg_encoder->calculated_sizes.total_size, jpeg_encoder->initial_info.framebuffer_alignment, 0);
		if (jpeg_encoder->fb_dmabuffers[i] == NULL)
		{
			IMX_VPU_ERROR("could not allocate DMA buffer for framebuffer #%u", i);
			ret = IMX_VPU_ENC_RETURN_CODE_ERROR;
			goto error;
		}

		imx_vpu_fill_framebuffer_params(&(jpeg_encoder->framebuffers[i]), &(jpeg_encoder->calculated_sizes), jpeg_encoder->fb_dmabuffers[i], 0);
	}

	if ((ret = imx_vpu_enc_register_framebuffers(jpeg_encoder->encoder, jpeg_encoder->framebuffers, jpeg_encoder->num_framebuffers)) != IMX_VPU_ENC_RETURN_CODE_OK)
	{
		IMX_VPU_ERROR("could not register framebuffers: %s", imx_vpu_enc_error_string(ret));
		goto error;
	}

	return ret;

error:
	imx_vpu_jpeg_enc_close_internal(jpeg_encoder);

	return ret;
}


static ImxVpuEncReturnCodes imx_vpu_jpeg_enc_close_internal(ImxVpuJPEGEncoder *jpeg_encoder)
{
	assert(jpeg_encoder != NULL);

	if (jpeg_encoder->encoder != NULL)
	{
		imx_vpu_enc_close(jpeg_encoder->encoder);
		jpeg_encoder->encoder = NULL;
	}

	imx_vpu_jpeg_enc_deallocate_framebuffers(jpeg_encoder);

	return IMX_VPU_ENC_RETURN_CODE_OK;
}


static void imx_vpu_jpeg_enc_deallocate_framebuffers(ImxVpuJPEGEncoder *jpeg_encoder)
{
	assert(jpeg_encoder != NULL);

	if (jpeg_encoder->framebuffers != NULL)
	{
		IMX_VPU_FREE(jpeg_encoder->framebuffers, sizeof(ImxVpuFramebuffer) * jpeg_encoder->num_framebuffers);
		jpeg_encoder->framebuffers = NULL;
	}

	if (jpeg_encoder->fb_dmabuffers != NULL)
	{
		imx_vpu_jpeg_deallocate_dma_buffers(jpeg_encoder->fb_dmabuffers, jpeg_encoder->num_framebuffers);
		IMX_VPU_FREE(jpeg_encoder->fb_dmabuffers, sizeof(ImxVpuDMABuffer *) * jpeg_encoder->num_framebuffers);
		jpeg_encoder->fb_dmabuffers = NULL;
	}
}


ImxVpuEncReturnCodes imx_vpu_jpeg_enc_open(ImxVpuJPEGEncoder **jpeg_encoder, ImxVpuDMABufferAllocator *dma_buffer_allocator)
{
	ImxVpuEncReturnCodes ret = IMX_VPU_ENC_RETURN_CODE_OK;
	ImxVpuJPEGEncoder *jpegenc = NULL;

	assert(jpeg_encoder != NULL);

	if ((ret = imx_vpu_enc_load()) != IMX_VPU_ENC_RETURN_CODE_OK)
		goto error;

	jpegenc = IMX_VPU_ALLOC(sizeof(ImxVpuJPEGEncoder));
	if (jpegenc == NULL)
	{
		IMX_VPU_ERROR("allocating memory for JPEG encoder object failed");
		ret = IMX_VPU_ENC_RETURN_CODE_ERROR;
		goto error;
	}

	memset(jpegenc, 0, sizeof(ImxVpuJPEGEncoder));

	jpegenc->dma_buffer_allocator = (dma_buffer_allocator != NULL) ? dma_buffer_allocator : imx_vpu_enc_get_default_allocator();

	imx_vpu_enc_get_bitstream_buffer_info(&(jpegenc->bitstream_buffer_size), &(jpegenc->bitstream_buffer_alignment));
	jpegenc->bitstream_buffer = imx_vpu_dma_buffer_allocate(jpegenc->dma_buffer_allocator, jpegenc->bitstream_buffer_size, jpegenc->bitstream_buffer_alignment, 0);
	if (jpegenc->bitstream_buffer == NULL)
	{
		IMX_VPU_ERROR("could not allocate DMA buffer for bitstream buffer with %u bytes and alignment %u", jpegenc->bitstream_buffer_size, jpegenc->bitstream_buffer_alignment);
		ret = IMX_VPU_ENC_RETURN_CODE_ERROR;
		goto error;
	}


	/* imx_vpu_enc_open() is called later on demand during encoding, to accomodate
	 * for potentially changing parameters like width, height, quality factor */


	*jpeg_encoder = jpegenc;

	return IMX_VPU_ENC_RETURN_CODE_OK;

error:
	if ((jpegenc != NULL) && (jpegenc->bitstream_buffer != NULL))
		imx_vpu_dma_buffer_deallocate(jpegenc->bitstream_buffer);

	return ret;
}


ImxVpuEncReturnCodes imx_vpu_jpeg_enc_close(ImxVpuJPEGEncoder *jpeg_encoder)
{
	assert(jpeg_encoder != NULL);

	imx_vpu_jpeg_enc_close_internal(jpeg_encoder);

	if (jpeg_encoder->bitstream_buffer != NULL)
		imx_vpu_dma_buffer_deallocate(jpeg_encoder->bitstream_buffer);

	imx_vpu_enc_unload();

	IMX_VPU_FREE(jpeg_encoder, sizeof(ImxVpuJPEGEncoder));

	return IMX_VPU_ENC_RETURN_CODE_OK;
}


ImxVpuEncReturnCodes imx_vpu_jpeg_enc_encode(ImxVpuJPEGEncoder *jpeg_encoder, ImxVpuFramebuffer const *framebuffer, ImxVpuJPEGEncParams const *params, void **acquired_handle, size_t *output_buffer_size)
{
	unsigned int output_code;
	ImxVpuEncParams enc_params;
	ImxVpuEncReturnCodes ret;
	ImxVpuRawFrame raw_frame;
	ImxVpuEncodedFrame encoded_frame;

	assert(framebuffer != NULL);
	assert(params != NULL);
	assert(jpeg_encoder != NULL);

	if (acquired_handle != NULL)
		*acquired_handle = NULL;

	if ((jpeg_encoder->encoder == NULL)
	 || (jpeg_encoder->frame_width != params->frame_width)
	 || (jpeg_encoder->frame_height != params->frame_height)
	 || (jpeg_encoder->quality_factor != params->quality_factor)
	 || (jpeg_encoder->color_format != params->color_format)
	)
	{
		imx_vpu_jpeg_enc_close_internal(jpeg_encoder);

		jpeg_encoder->frame_width = params->frame_width;
		jpeg_encoder->frame_height = params->frame_height;
		jpeg_encoder->quality_factor = params->quality_factor;
		jpeg_encoder->color_format = params->color_format;

		if ((ret = imx_vpu_jpeg_enc_open_internal(jpeg_encoder)) != IMX_VPU_ENC_RETURN_CODE_OK)
			return ret;
	}

	memset(&enc_params, 0, sizeof(enc_params));
	enc_params.acquire_output_buffer = params->acquire_output_buffer;
	enc_params.finish_output_buffer = params->finish_output_buffer;
	enc_params.output_buffer_context = params->output_buffer_context;

	memset(&raw_frame, 0, sizeof(raw_frame));
	raw_frame.framebuffer = (ImxVpuFramebuffer *)framebuffer;
	
	memset(&encoded_frame, 0, sizeof(encoded_frame));

	ret = imx_vpu_enc_encode(jpeg_encoder->encoder, &raw_frame, &encoded_frame, &enc_params, &output_code);

	if (acquired_handle != NULL)
		*acquired_handle = encoded_frame.acquired_handle;
	if (output_buffer_size != NULL)
		*output_buffer_size = encoded_frame.data_size;

	return ret;
}
