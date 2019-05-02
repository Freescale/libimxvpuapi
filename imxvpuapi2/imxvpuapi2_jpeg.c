/* Simplified API for JPEG en- and decoding with the NXP i.MX SoC
 * Copyright (C) 2019 Carlos Rafael Giani
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
#include "imxvpuapi2_priv.h"
#include "imxvpuapi2_jpeg.h"




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


static void deallocate_dma_buffers(ImxDmaBuffer **dma_buffers, size_t num_dma_buffers)
{
	size_t i;
	for (i = 0; i < num_dma_buffers; ++i)
	{
		if (dma_buffers[i] == NULL)
			continue;

		imx_dma_buffer_deallocate(dma_buffers[i]);
		dma_buffers[i] = NULL;
	}
}


static BOOL add_framebuffers_to_array(ImxDmaBuffer ***fb_dma_buffers, size_t *num_framebuffers, ImxDmaBufferAllocator *dma_buffer_allocator, size_t framebuffer_size, size_t framebuffer_alignment, size_t num_framebuffers_to_add)
{
	BOOL ret = TRUE;
	size_t old_num_framebuffers;
	size_t new_num_framebuffers;
	ImxDmaBuffer **new_fb_pool_dmabuffers_array;
	int err;
	size_t i;

	assert(fb_dma_buffers != NULL);
	assert(num_framebuffers != NULL);

	old_num_framebuffers = *num_framebuffers;
	new_num_framebuffers = old_num_framebuffers + num_framebuffers_to_add;

	new_fb_pool_dmabuffers_array = realloc(*fb_dma_buffers, new_num_framebuffers * sizeof(ImxDmaBuffer *));
	assert(new_fb_pool_dmabuffers_array != NULL);
	memset(new_fb_pool_dmabuffers_array + old_num_framebuffers, 0, num_framebuffers_to_add * sizeof(ImxDmaBuffer *));

	*fb_dma_buffers = new_fb_pool_dmabuffers_array;
	*num_framebuffers = new_num_framebuffers;

	for (i = old_num_framebuffers; i < new_num_framebuffers; ++i)
	{
		(*fb_dma_buffers)[i] = imx_dma_buffer_allocate(
			dma_buffer_allocator,
			framebuffer_size,
			framebuffer_alignment,
			&err
		);
		if ((*fb_dma_buffers)[i] == NULL)
		{
			IMX_VPU_API_ERROR("could not allocate DMA buffer for FB pool framebuffer: %s (%d)", strerror(err), err);
			ret = FALSE;
			goto finish;
		}
	}

finish:
	return ret;
}




/****************************/
/******* JPEG DECODER *******/
/****************************/


struct _ImxVpuApiJpegDecoder
{
	ImxVpuApiDecoder *decoder;

	ImxDmaBufferAllocator *dma_buffer_allocator;

	ImxDmaBuffer *stream_buffer;

	ImxVpuApiDecGlobalInfo const *global_info;
	ImxVpuApiDecOpenParams open_params;
	ImxVpuApiDecStreamInfo stream_info;

	ImxDmaBuffer **fb_dma_buffers;
	size_t num_framebuffers;

	ImxDmaBuffer *output_dma_buffer;
	ImxDmaBuffer *fb_dma_buffer_to_return;

	ImxVpuApiJpegDecInfo jpeg_dec_info;
};


static BOOL imx_vpu_api_jpeg_dec_add_framebuffers(ImxVpuApiJpegDecoder *jpeg_decoder, size_t num_framebuffers_to_add);
static void imx_vpu_api_jpeg_dec_deallocate_fb_dma_buffers(ImxVpuApiJpegDecoder *jpeg_decoder);


static BOOL imx_vpu_api_jpeg_dec_add_framebuffers(ImxVpuApiJpegDecoder *jpeg_decoder, size_t num_framebuffers_to_add)
{
	ImxVpuApiDecReturnCodes dec_ret;
	size_t old_num_framebuffers;

	if (num_framebuffers_to_add == 0)
		return TRUE;

	old_num_framebuffers = jpeg_decoder->num_framebuffers;

	if (!add_framebuffers_to_array(
		&(jpeg_decoder->fb_dma_buffers),
		&(jpeg_decoder->num_framebuffers),
		jpeg_decoder->dma_buffer_allocator,
		jpeg_decoder->stream_info.min_fb_pool_framebuffer_size,
		jpeg_decoder->stream_info.fb_pool_framebuffer_alignment,
		num_framebuffers_to_add
	))
		return FALSE;

	dec_ret = imx_vpu_api_dec_add_framebuffers_to_pool(jpeg_decoder->decoder, &(jpeg_decoder->fb_dma_buffers[old_num_framebuffers]), NULL, num_framebuffers_to_add);
	if (dec_ret != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		IMX_VPU_API_LOG("could not add framebuffers to VPU pool: %s", imx_vpu_api_dec_return_code_string(dec_ret));
		return FALSE;
	}

	return TRUE;
}


static void imx_vpu_api_jpeg_dec_deallocate_fb_dma_buffers(ImxVpuApiJpegDecoder *jpeg_decoder)
{
	if (jpeg_decoder->fb_dma_buffers != NULL)
	{
		deallocate_dma_buffers(jpeg_decoder->fb_dma_buffers, jpeg_decoder->num_framebuffers);
		jpeg_decoder->fb_dma_buffers = NULL;
		jpeg_decoder->num_framebuffers = 0;
	}
}


int imx_vpu_api_jpeg_dec_open(ImxVpuApiJpegDecoder **jpeg_decoder, ImxDmaBufferAllocator *dma_buffer_allocator)
{
	int ret = TRUE;
	ImxVpuApiDecReturnCodes dec_ret;
	ImxVpuApiDecOpenParams *open_params;

	assert(jpeg_decoder != NULL);
	assert(dma_buffer_allocator != NULL);

	*jpeg_decoder = malloc(sizeof(ImxVpuApiJpegDecoder));
	assert((*jpeg_decoder) != NULL);

	memset(*jpeg_decoder, 0, sizeof(ImxVpuApiJpegDecoder));
	(*jpeg_decoder)->dma_buffer_allocator = dma_buffer_allocator;
	(*jpeg_decoder)->global_info = imx_vpu_api_dec_get_global_info();
	assert((*jpeg_decoder)->global_info != NULL);

	(*jpeg_decoder)->stream_buffer = imx_dma_buffer_allocate(
		dma_buffer_allocator,
		(*jpeg_decoder)->global_info->min_required_stream_buffer_size,
		(*jpeg_decoder)->global_info->required_stream_buffer_physaddr_alignment,
		0
	);
	if ((*jpeg_decoder)->stream_buffer == NULL)
		goto error;

	open_params = &((*jpeg_decoder)->open_params);
	memset(open_params, 0, sizeof(ImxVpuApiDecOpenParams));
	open_params->compression_format = IMX_VPU_API_COMPRESSION_FORMAT_JPEG;
	open_params->flags = 0;

	if ((dec_ret = imx_vpu_api_dec_open(&((*jpeg_decoder)->decoder), open_params, (*jpeg_decoder)->stream_buffer)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		IMX_VPU_API_ERROR("could not open JPEG decoder: %s", imx_vpu_api_dec_return_code_string(dec_ret));
		goto error;
	}

finish:
	return ret;

error:
	imx_vpu_api_jpeg_dec_close(*jpeg_decoder);
	ret = FALSE;
	goto finish;
}


void imx_vpu_api_jpeg_dec_close(ImxVpuApiJpegDecoder *jpeg_decoder)
{
	if (jpeg_decoder == NULL)
		return;

	if (jpeg_decoder->decoder != NULL)
		imx_vpu_api_dec_close(jpeg_decoder->decoder);

	imx_vpu_api_jpeg_dec_deallocate_fb_dma_buffers(jpeg_decoder);

	if (jpeg_decoder->output_dma_buffer != NULL)
		imx_dma_buffer_deallocate(jpeg_decoder->output_dma_buffer);

	if (jpeg_decoder->stream_buffer != NULL)
		imx_dma_buffer_deallocate(jpeg_decoder->stream_buffer);

	free(jpeg_decoder);
}


ImxVpuApiJpegDecInfo const * imx_vpu_api_jpeg_dec_decode(ImxVpuApiJpegDecoder *jpeg_decoder, uint8_t const *jpeg_data, size_t const jpeg_data_size)
{
	ImxVpuApiDecOutputCodes output_code;
	ImxVpuApiDecReturnCodes dec_ret;
	ImxVpuApiEncodedFrame encoded_frame;
	BOOL do_loop = TRUE;

	assert(jpeg_decoder != NULL);

	if (jpeg_decoder->fb_dma_buffer_to_return != NULL)
	{
		imx_vpu_api_dec_return_framebuffer_to_decoder(jpeg_decoder->decoder, jpeg_decoder->fb_dma_buffer_to_return);
		jpeg_decoder->fb_dma_buffer_to_return = NULL;
	}

	encoded_frame.data = (uint8_t *)jpeg_data;
	encoded_frame.data_size = jpeg_data_size;

	if ((dec_ret = imx_vpu_api_dec_push_encoded_frame(jpeg_decoder->decoder, &encoded_frame)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		IMX_VPU_API_ERROR("could not push JPEG data into decoder: %s", imx_vpu_api_dec_return_code_string(dec_ret));
		goto error;
	}

	jpeg_decoder->jpeg_dec_info.fb_dma_buffer = NULL;

	do
	{
		if ((dec_ret = imx_vpu_api_dec_decode(jpeg_decoder->decoder, &output_code)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
		{
			IMX_VPU_API_ERROR("could not decode JPEG: %s", imx_vpu_api_dec_return_code_string(dec_ret));
			goto error;
		}

		switch (output_code)
		{
			case IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_EOS:
				do_loop = FALSE;
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE:
			{
				ImxVpuApiFramebufferMetrics const *fb_metrics = &(jpeg_decoder->stream_info.decoded_frame_framebuffer_metrics);
				ImxVpuApiDecStreamInfo const *stream_info = imx_vpu_api_dec_get_stream_info(jpeg_decoder->decoder);

				imx_vpu_api_jpeg_dec_deallocate_fb_dma_buffers(jpeg_decoder);

				jpeg_decoder->stream_info = *stream_info;
				jpeg_decoder->jpeg_dec_info.framebuffer_metrics = fb_metrics;
				jpeg_decoder->jpeg_dec_info.color_format = stream_info->color_format;
				jpeg_decoder->jpeg_dec_info.total_frame_size = (imx_vpu_api_is_color_format_semi_planar(stream_info->color_format) ? fb_metrics->u_offset : fb_metrics->v_offset) + fb_metrics->uv_size;

				if (!imx_vpu_api_jpeg_dec_add_framebuffers(jpeg_decoder, stream_info->min_num_required_framebuffers))
				{
					IMX_VPU_API_ERROR("could not add %zu framebuffer(s) to decoder", stream_info->min_num_required_framebuffers);
					goto error;
				}

				if (!(jpeg_decoder->global_info->flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL))
				{
					int err;

					if (jpeg_decoder->output_dma_buffer != NULL)
					{
						imx_dma_buffer_deallocate(jpeg_decoder->output_dma_buffer);
						jpeg_decoder->output_dma_buffer = NULL;
					}

					jpeg_decoder->output_dma_buffer = imx_dma_buffer_allocate(
						jpeg_decoder->dma_buffer_allocator,
						jpeg_decoder->stream_info.min_output_framebuffer_size,
						jpeg_decoder->stream_info.output_framebuffer_alignment,
						&err
					);
					if (jpeg_decoder->output_dma_buffer == NULL)
					{
						IMX_VPU_API_ERROR("could not allocate DMA buffer for FB pool framebuffer: %s (%d)", strerror(err), err);
						goto error;
					}

					imx_vpu_api_dec_set_output_frame_dma_buffer(jpeg_decoder->decoder, jpeg_decoder->output_dma_buffer, NULL);
				}

				break;
			}

			case IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:
			{
				if (!imx_vpu_api_jpeg_dec_add_framebuffers(jpeg_decoder, 1))
				{
					IMX_VPU_API_ERROR("could not add framebuffer to decoder");
					goto error;
				}

				break;
			}

			case IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE:
			{
				ImxVpuApiRawFrame decoded_frame;

				if ((dec_ret = imx_vpu_api_dec_get_decoded_frame(jpeg_decoder->decoder, &decoded_frame)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
				{
					IMX_VPU_API_ERROR("imx_vpu_api_dec_get_decoded_frame() failed: %s", imx_vpu_api_dec_return_code_string(dec_ret));
					goto error;
				}

				jpeg_decoder->jpeg_dec_info.fb_dma_buffer = decoded_frame.fb_dma_buffer;
				jpeg_decoder->fb_dma_buffer_to_return = decoded_frame.fb_dma_buffer;

				break;
			}

			case IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:
				do_loop = FALSE;
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED:
				break;

			default:
				IMX_VPU_API_ERROR("unknown/unhandled output code %s (%d)", imx_vpu_api_dec_output_code_string(output_code));
				goto error;
		}
	}
	while (do_loop);

	return &(jpeg_decoder->jpeg_dec_info);

error:
	return NULL;
}




/****************************/
/******* JPEG DECODER *******/
/****************************/


struct _ImxVpuApiJpegEncoder
{
	ImxVpuApiEncoder *encoder;

	ImxDmaBufferAllocator *dma_buffer_allocator;

	ImxDmaBuffer *stream_buffer;

	ImxVpuApiEncGlobalInfo const *global_info;
	ImxVpuApiEncOpenParams open_params;
	ImxVpuApiEncStreamInfo stream_info;

	ImxDmaBuffer **fb_dma_buffers;
	size_t num_framebuffers;

	ImxDmaBuffer *input_dmabuffer;

	BOOL has_encoded_frame;
};


static BOOL imx_vpu_api_jpeg_enc_open_internal(ImxVpuApiJpegEncoder *jpeg_encoder);
static void imx_vpu_api_jpeg_enc_close_internal(ImxVpuApiJpegEncoder *jpeg_encoder);
static BOOL imx_vpu_api_jpeg_enc_add_framebuffers(ImxVpuApiJpegEncoder *jpeg_encoder, size_t num_framebuffers_to_add);



static BOOL imx_vpu_api_jpeg_enc_open_internal(ImxVpuApiJpegEncoder *jpeg_encoder)
{
	int err;
	ImxVpuApiEncReturnCodes enc_ret;
	ImxVpuApiEncStreamInfo const *stream_info;

	assert(jpeg_encoder != NULL);

	enc_ret = imx_vpu_api_enc_open(&(jpeg_encoder->encoder), &(jpeg_encoder->open_params), jpeg_encoder->stream_buffer);
	if (enc_ret != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		IMX_VPU_API_ERROR("imx_vpu_api_enc_open() failed: %s", imx_vpu_api_enc_return_code_string(enc_ret));
		return FALSE;
	}

	stream_info = imx_vpu_api_enc_get_stream_info(jpeg_encoder->encoder);
	assert(stream_info != NULL);
	jpeg_encoder->stream_info = *stream_info;

	if (!imx_vpu_api_jpeg_enc_add_framebuffers(jpeg_encoder, stream_info->min_num_required_framebuffers))
		return FALSE;

	jpeg_encoder->input_dmabuffer = imx_dma_buffer_allocate(jpeg_encoder->dma_buffer_allocator, stream_info->min_framebuffer_size, stream_info->framebuffer_alignment, &err);
	if (jpeg_encoder->input_dmabuffer == NULL)
	{
		IMX_VPU_API_ERROR("could not allocate DMA buffer for input framebuffer: %s (%d)", strerror(err), err);
		return FALSE;
	}

	return TRUE;
}


static void imx_vpu_api_jpeg_enc_close_internal(ImxVpuApiJpegEncoder *jpeg_encoder)
{
	assert(jpeg_encoder != NULL);

	if (jpeg_encoder->encoder != NULL)
	{
		imx_vpu_api_enc_close(jpeg_encoder->encoder);
		jpeg_encoder->encoder = NULL;
	}

	if (jpeg_encoder->fb_dma_buffers != NULL)
	{
		deallocate_dma_buffers(jpeg_encoder->fb_dma_buffers, jpeg_encoder->num_framebuffers);
		jpeg_encoder->fb_dma_buffers = NULL;
		jpeg_encoder->num_framebuffers = 0;
	}

	if (jpeg_encoder->input_dmabuffer != NULL)
	{
		imx_dma_buffer_deallocate(jpeg_encoder->input_dmabuffer);
		jpeg_encoder->input_dmabuffer = NULL;
	}
}


static BOOL imx_vpu_api_jpeg_enc_add_framebuffers(ImxVpuApiJpegEncoder *jpeg_encoder, size_t num_framebuffers_to_add)
{
	ImxVpuApiEncReturnCodes enc_ret;
	size_t old_num_framebuffers;

	if (num_framebuffers_to_add == 0)
		return TRUE;

	old_num_framebuffers = jpeg_encoder->num_framebuffers;

	if (!add_framebuffers_to_array(
		&(jpeg_encoder->fb_dma_buffers),
		&(jpeg_encoder->num_framebuffers),
		jpeg_encoder->dma_buffer_allocator,
		jpeg_encoder->stream_info.min_framebuffer_size,
		jpeg_encoder->stream_info.framebuffer_alignment,
		num_framebuffers_to_add
	))
		return FALSE;

	enc_ret = imx_vpu_api_enc_add_framebuffers_to_pool(jpeg_encoder->encoder, jpeg_encoder->fb_dma_buffers + old_num_framebuffers, num_framebuffers_to_add);
	if (enc_ret != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		IMX_VPU_API_LOG("could not add framebuffers to VPU pool: %s", imx_vpu_api_enc_return_code_string(enc_ret));
		return FALSE;
	}

	return TRUE;
}


int imx_vpu_api_jpeg_enc_open(ImxVpuApiJpegEncoder **jpeg_encoder, ImxDmaBufferAllocator *dma_buffer_allocator)
{
	int ret = TRUE;

	assert(jpeg_encoder != NULL);

	*jpeg_encoder = malloc(sizeof(ImxVpuApiJpegEncoder));
	assert((*jpeg_encoder) != NULL);

	memset(*jpeg_encoder, 0, sizeof(ImxVpuApiJpegEncoder));
	(*jpeg_encoder)->dma_buffer_allocator = dma_buffer_allocator;
	(*jpeg_encoder)->global_info = imx_vpu_api_enc_get_global_info();
	assert((*jpeg_encoder)->global_info != NULL);

	(*jpeg_encoder)->stream_buffer = imx_dma_buffer_allocate(
		dma_buffer_allocator,
		(*jpeg_encoder)->global_info->min_required_stream_buffer_size,
		(*jpeg_encoder)->global_info->required_stream_buffer_physaddr_alignment,
		0
	);
	if ((*jpeg_encoder)->stream_buffer == NULL)
		goto error;

finish:
	return ret;

error:
	imx_vpu_api_jpeg_enc_close(*jpeg_encoder);
	ret = FALSE;
	goto finish;
}


void imx_vpu_api_jpeg_enc_close(ImxVpuApiJpegEncoder *jpeg_encoder)
{
	if (jpeg_encoder == NULL)
		return;

	imx_vpu_api_jpeg_enc_close_internal(jpeg_encoder);

	if (jpeg_encoder->stream_buffer != NULL)
		imx_dma_buffer_deallocate(jpeg_encoder->stream_buffer);

	free(jpeg_encoder);
}


int imx_vpu_api_jpeg_enc_set_params(ImxVpuApiJpegEncoder *jpeg_encoder, ImxVpuApiJpegEncParams const *params)
{
	int ret = TRUE;
	ImxVpuApiEncOpenParams *open_params;

	assert(jpeg_encoder != NULL);
	assert(params != NULL);

	open_params = &(jpeg_encoder->open_params);
	memset(open_params, 0, sizeof(ImxVpuApiEncOpenParams));
	imx_vpu_api_enc_set_default_open_params(IMX_VPU_API_COMPRESSION_FORMAT_JPEG, params->color_format, params->frame_width, params->frame_height, open_params);
	/* See the ImxVpuApiCompressionFormatSupportDetails documentation for
	 * an explanation of this calculation. */
	open_params->quantization = 100 - params->quality_factor;

	imx_vpu_api_jpeg_enc_close_internal(jpeg_encoder);
	ret = imx_vpu_api_jpeg_enc_open_internal(jpeg_encoder);

	return ret;
}


ImxVpuApiFramebufferMetrics const * imx_vpu_api_jpeg_enc_get_framebuffer_metrics(ImxVpuApiJpegEncoder *jpeg_encoder)
{
	return &(jpeg_encoder->stream_info.frame_encoding_framebuffer_metrics);
}


int imx_vpu_api_jpeg_enc_encode(ImxVpuApiJpegEncoder *jpeg_encoder, ImxDmaBuffer *frame_dma_buffer, size_t *encoded_data_size)
{
	BOOL ret = TRUE;
	ImxVpuApiEncReturnCodes enc_ret;
	ImxVpuApiEncOutputCodes output_code;
	BOOL do_loop = TRUE;
	ImxVpuApiRawFrame raw_frame = {
		.fb_dma_buffer = frame_dma_buffer,
		.frame_types = { IMX_VPU_API_FRAME_TYPE_UNKNOWN, IMX_VPU_API_FRAME_TYPE_UNKNOWN }
	};

	assert(jpeg_encoder != NULL);
	assert(frame_dma_buffer != NULL);
	assert(encoded_data_size != NULL);


	if ((enc_ret = imx_vpu_api_enc_push_raw_frame(jpeg_encoder->encoder, &raw_frame)) != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		IMX_VPU_API_ERROR("could not push raw input data into encoder: %s", imx_vpu_api_enc_return_code_string(enc_ret));
		goto error;
	}


	do
	{
		/* Perform an encoding step. */
		if ((enc_ret = imx_vpu_api_enc_encode(jpeg_encoder->encoder, encoded_data_size, &output_code)) != IMX_VPU_API_ENC_RETURN_CODE_OK)
		{
			IMX_VPU_API_ERROR("could not encode JPEG: %s", imx_vpu_api_enc_return_code_string(enc_ret));
			goto error;
		}

		IMX_VPU_API_LOG("encode step finished, output code: %s", imx_vpu_api_enc_output_code_string(output_code));

		switch (output_code)
		{
			case IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:
				break;

			case IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:
			{
				if (!imx_vpu_api_jpeg_enc_add_framebuffers(jpeg_encoder, 1))
				{
					IMX_VPU_API_ERROR("could not add framebuffer to encoder");
					goto error;
				}

				break;
			}

			case IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE:
			{
				if (jpeg_encoder->has_encoded_frame)
				{
					IMX_VPU_API_ERROR("internal error: there is already an encoded frame");
					goto error;
				}

				jpeg_encoder->has_encoded_frame = TRUE;
				do_loop = FALSE;

				break;
			}

			case IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:
			{
				if (!jpeg_encoder->has_encoded_frame)
				{
					IMX_VPU_API_ERROR("internal error: no frame encoded yet, and encoder needs more data");
					goto error;
				}

				do_loop = FALSE;
				break;
			}

			case IMX_VPU_API_ENC_OUTPUT_CODE_EOS:
			{
				if (!jpeg_encoder->has_encoded_frame)
				{
					IMX_VPU_API_ERROR("internal error: no frame encoded yet, and encoder reported EOS");
					goto error;
				}

				do_loop = FALSE;
				break;
			}

			default:
				IMX_VPU_API_ERROR("unknown/unhandled output code %s (%d)", imx_vpu_api_enc_output_code_string(output_code));
				goto error;
		}
	}
	while (do_loop);


finish:
	return ret;

error:
	ret = FALSE;
	goto finish;
}


int imx_vpu_api_jpeg_enc_get_encoded_data(ImxVpuApiJpegEncoder *jpeg_encoder, void *encoded_data_dest)
{
	ImxVpuApiEncReturnCodes enc_ret;
	ImxVpuApiEncodedFrame encoded_frame;

	assert(jpeg_encoder != NULL);
	assert(encoded_data_dest != NULL);

	encoded_frame.data = encoded_data_dest;

	enc_ret = imx_vpu_api_enc_get_encoded_frame(jpeg_encoder->encoder, &encoded_frame);
	if (enc_ret != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		IMX_VPU_API_ERROR("could not get encoded frame: %s", imx_vpu_api_enc_return_code_string(enc_ret));
		return FALSE;
	}

	jpeg_encoder->has_encoded_frame = FALSE;

	return TRUE;
}


