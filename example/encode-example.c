/* example for how to use the imxvpuapi encoder interface
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#include "main.h"
#include "y4m_io.h"


/* This is a simple example of how to encode with the imxvpuapi library.
 * It reads raw frames from Y4M data, encodes it to h.264, and dumps the
 * encoded frames to a file. */


#define FRAME_CONTEXT_START          0x1000


struct _Context
{
	/* Input/output file handles. */
	FILE *y4m_input_file;
	FILE *h264_output_file;

	/* Y4M I/O for raw input frames. */
	Y4MContext y4m_context;

	/* DMA buffer allocator for the encoder's framebuffer pool and
	 * for output frames. */
	ImxDmaBufferAllocator *allocator;

	/* The actual VPU encoder. */
	ImxVpuApiEncoder *encoder;

	/* Stream buffer, used during encoding. */
	ImxDmaBuffer *stream_buffer;

	/* Pointer to the encoder's global information.
	 * Since this information is constant, it is not strictly
	 * necessary to keep a pointer to it here. This is just done
	 * for sake of convenience and clarity. */
	ImxVpuApiEncGlobalInfo const *enc_global_info;

	/* Copy of information about the stream that is to be encoded. This
	 * stream info becomes available right after opening the encoder
	 * instance with imx_vpu_api_enc_open(). The stream info is then
	 * available by calling imx_vpu_api_enc_get_stream_info(). */
	ImxVpuApiEncStreamInfo stream_info;

	/* Framebuffer DMA buffers that are part of the encoder's
	 * framebuffer pool. These are used only internally by the encoder,
	 * and not for delivering encoded output frames. Also, not all
	 * encoders use a framebuffer pool. */
	ImxDmaBuffer **fb_pool_dmabuffers;
	size_t num_fb_pool_framebuffers;

	/* DMA buffer for raw input frames to encode. */
	ImxDmaBuffer *input_dmabuffer;

	/* Buffer for containing encoded frames. Unlike other buffers,
	 * this is not a DMA buffer, it is regular system memory. */
	void *encoded_frame_buffer;
	size_t encoded_frame_buffer_size;

	/* A counter used here to simulate a frame context. Corresponding
	 * input and output frames have the same context. */
	unsigned int frame_context_counter;
};


/* Allocates the DMA buffers for the encoders's framebuffer pool. This is
 * called once the stream info is known, and is always called. We only
 * _add_ framebuffers here, since framebuffers cannot be removed from an
 * encoder's pool once added (until the encoder is closed). */
static int allocate_and_add_fb_pool_framebuffers(Context *ctx, size_t num_fb_pool_framebuffers_to_add)
{
	int ret = 1;
	size_t old_num_fb_pool_framebuffers;
	size_t new_num_fb_pool_framebuffers;
	ImxDmaBuffer **new_fb_pool_dmabuffers_array;
	ImxVpuApiEncReturnCodes enc_ret;
	int err;
	size_t i;

	/* Nothing to add? Skip to the end. */
	if (num_fb_pool_framebuffers_to_add == 0)
		goto finish;

	/* Calculate new number of framebuffer and keep track of the old one for
	 * reallocation and for knowing where to add the new ones in the array. */
	old_num_fb_pool_framebuffers = ctx->num_fb_pool_framebuffers;
	new_num_fb_pool_framebuffers = old_num_fb_pool_framebuffers + num_fb_pool_framebuffers_to_add;

	/* Reallocate to make space for more framebuffer DMA buffer pointers. */
	new_fb_pool_dmabuffers_array = realloc(ctx->fb_pool_dmabuffers, new_num_fb_pool_framebuffers * sizeof(ImxDmaBuffer *));
	assert(new_fb_pool_dmabuffers_array != NULL);
	/* Set the new array elements to an initial value of 0. */
	memset(new_fb_pool_dmabuffers_array + old_num_fb_pool_framebuffers, 0, num_fb_pool_framebuffers_to_add * sizeof(ImxDmaBuffer *));

	/* Store the new array. */
	ctx->fb_pool_dmabuffers = new_fb_pool_dmabuffers_array;
	ctx->num_fb_pool_framebuffers = new_num_fb_pool_framebuffers;

	/* Allocate new framebuffers and fill the FB context array. */
	for (i = old_num_fb_pool_framebuffers; i < ctx->num_fb_pool_framebuffers; ++i)
	{
		ctx->fb_pool_dmabuffers[i] = imx_dma_buffer_allocate(
			ctx->allocator,
			ctx->stream_info.min_framebuffer_size,
			ctx->stream_info.framebuffer_alignment,
			&err
		);
		if (ctx->fb_pool_dmabuffers[i] == NULL)
		{
			fprintf(stderr, "could not allocate DMA buffer for FB pool framebuffer: %s (%d)\n", strerror(err), err);
			ret = 0;
			goto finish;
		}
	}

	/* Add the newly allocated framebuffers to the encoder's pool. */
	enc_ret = imx_vpu_api_enc_add_framebuffers_to_pool(ctx->encoder, ctx->fb_pool_dmabuffers + old_num_fb_pool_framebuffers, num_fb_pool_framebuffers_to_add);
	if (enc_ret != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		fprintf(stderr, "could not add framebuffers to VPU pool: %s\n", imx_vpu_api_enc_return_code_string(enc_ret));
		ret = 0;
		goto finish;
	}


finish:
	return ret;
}


/* Deallocate all previously allocated framebuffers. Before calling this,
 * the encoder must have been closed, since otherwise, deallocating its
 * FB pool framebuffer DMA buffers could cause undefined behavior. */
static void deallocate_framebuffers(Context *ctx)
{
	size_t i;

	for (i = 0; i < ctx->num_fb_pool_framebuffers; ++i)
	{
		ImxDmaBuffer *dmabuffer = ctx->fb_pool_dmabuffers[i];
		if (dmabuffer != NULL)
			imx_dma_buffer_deallocate(dmabuffer);
	}
	free(ctx->fb_pool_dmabuffers);

	ctx->num_fb_pool_framebuffers = 0;
	ctx->fb_pool_dmabuffers = NULL;
}


/* Resize the system memory buffer that will hold the encoded data.
 * This is called after imx_vpu_api_enc_encode() returns the output
 * code IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE, but
 * before imx_vpu_api_enc_get_encoded_frame() is called, to make sure
 * there is enough space for the encoded data. */
static void resize_encoded_frame_buffer(Context *ctx, size_t new_size)
{
	void *new_buffer;

	if (ctx->encoded_frame_buffer_size >= new_size)
		return;

	new_buffer = realloc(ctx->encoded_frame_buffer, new_size);
	assert(new_buffer != NULL);

	ctx->encoded_frame_buffer = new_buffer;
	ctx->encoded_frame_buffer_size = new_size;
}


/* Push a raw input frame into the encoder. In this example, we have to
 * copy frame pixels into a DMA buffer (input_dmabuffer). CPU-based
 * copies like this one are not strictly necessary; if raw frames are
 * already stored in DMA buffers, they can be used directly, facilitating
 * a "zero-copy"-esque approach that avoids unnecessary CPU usage (the
 * encoder can pull the frame pixels through DMA channels). */
static Retval push_raw_input_frame(Context *ctx)
{
	ImxVpuApiRawFrame raw_input_frame;
	uint8_t *mapped_virtual_address;
	int y4m_ret;
	ImxVpuApiFramebufferMetrics const *fb_metrics;
	ImxVpuApiEncReturnCodes enc_ret;

	fb_metrics = &(ctx->stream_info.frame_encoding_framebuffer_metrics);

	/* Set up the input frame. The only field that needs to be
	 * set is the input framebuffer. The encoder will read from it.
	 * The rest can remain zero/NULL. */
	memset(&raw_input_frame, 0, sizeof(raw_input_frame));
	raw_input_frame.fb_dma_buffer = ctx->input_dmabuffer;
	raw_input_frame.context = (void*)((uintptr_t)(ctx->frame_context_counter));

	ctx->frame_context_counter++;

	fprintf(stderr, "pushing raw frame with context %p into encoder\n", raw_input_frame.context);

	/* Read uncompressed pixels into the input DMA buffer */
	mapped_virtual_address = imx_dma_buffer_map(ctx->input_dmabuffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE, NULL);
	y4m_ret = y4m_read_frame(
		&(ctx->y4m_context),
		mapped_virtual_address + fb_metrics->y_offset,
		mapped_virtual_address + fb_metrics->u_offset,
		mapped_virtual_address + fb_metrics->v_offset
	);
	imx_dma_buffer_unmap(ctx->input_dmabuffer);
	if (!y4m_ret)
		return RETVAL_EOS;

	if ((enc_ret = imx_vpu_api_enc_push_raw_frame(ctx->encoder, &raw_input_frame)) != IMX_VPU_API_ENC_RETURN_CODE_OK)
	{
		fprintf(stderr, "could not push raw frame into encoder: %s\n", imx_vpu_api_enc_return_code_string(enc_ret));
		return RETVAL_ERROR;
	}

	return RETVAL_OK;
}


/* Encode the previously inserted raw frame. The core of this function
 * is the imx_vpu_api_enc_encode() call, which is repeatedly called until
 * another raw input frame needs to be pushed into the encoder, EOS is
 * reported, an error occurs, or the encoder is flushed by the
 * imx_vpu_api_enc_flush() call (which we do not use in this example). */
static Retval encode_raw_frame(Context *ctx)
{
	ImxVpuApiEncOutputCodes output_code;
	size_t encoded_frame_size;
	ImxVpuApiEncReturnCodes enc_ret;
	Retval retval = RETVAL_OK;
	int do_loop = 1;

	do
	{
		/* Perform an encoding step. */
		if ((enc_ret = imx_vpu_api_enc_encode(ctx->encoder, &encoded_frame_size, &output_code)) != IMX_VPU_API_ENC_RETURN_CODE_OK)
		{
			fprintf(stderr, "imx_vpu_api_enc_encode failed(): %s\n", imx_vpu_api_enc_return_code_string(enc_ret));
			return RETVAL_ERROR;
		}

		switch (output_code)
		{
			case IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:
				/* Encoder did not produce an encoded frame yet, and has nothing
				 * else to report. Continue calling imx_vpu_api_enc_encode(). */
				break;

			case IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:
				/* Encoder needs one more framebuffer added to its pool.
				 * Add one, otherwise encoding cannot continue. Then continue
				 * calling imx_vpu_api_enc_encode(). */

				if (!allocate_and_add_fb_pool_framebuffers(ctx, 1))
				{
					retval = RETVAL_ERROR;
					do_loop = 0;
				}

				break;

			case IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE:
			{
				/* Encoder produced an encoded frame. First, make sure that
				 * we have a buffer big enough for that data. Then, retrieve
				 * the encoded frame data, and output it. We have to retrieve
				 * the encoded data, otherwise encoding cannot continue. */

				ImxVpuApiEncodedFrame output_frame;

				resize_encoded_frame_buffer(ctx, encoded_frame_size);

				memset(&output_frame, 0, sizeof(output_frame));
				output_frame.data = ctx->encoded_frame_buffer;
				output_frame.data_size = encoded_frame_size;

				if ((enc_ret = imx_vpu_api_enc_get_encoded_frame(ctx->encoder, &output_frame)) != IMX_VPU_API_ENC_RETURN_CODE_OK)
				{
					fprintf(stderr, "could not retrieve encoded frame: %s\n", imx_vpu_api_enc_return_code_string(enc_ret));
					return RETVAL_ERROR;
				}

				fprintf(stderr, "got encoded frame with %zu byte and context %p from encoder\n", output_frame.data_size, output_frame.context);

				fwrite(output_frame.data, 1, output_frame.data_size, ctx->h264_output_file);

				break;
			}

			case IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:
				/* Encoding cannot continue, since the encoded ran out of
				 * raw frames to encode. Exit the loop so that in the
				 * run() function, push_raw_input_frame() can be called
				 * again to push a new raw frame into the encoder. Then
				 * encoding can continue. */
				do_loop = 0;
				break;

			case IMX_VPU_API_ENC_OUTPUT_CODE_EOS:
				/* Encoder reached the end-of-stream. No more frames can be encoded.
				 * At this point, all that can be done is to close the decoder,
				 * so exit the loop. */
				fprintf(stderr, "VPU reports EOS; no more encoded frames available\n");
				retval = RETVAL_EOS;
				do_loop = 0;
				break;

			default:
				fprintf(stderr, "UNKNOWN OUTPUT CODE %s (%d)\n", imx_vpu_api_enc_output_code_string(output_code), (int)output_code);
				assert(0);
		}

	}
	while (do_loop);

	return retval;
}


Context* init(FILE *input_file, FILE *output_file)
{
	size_t i;
	int err;
	Context *ctx;
	ImxVpuApiEncOpenParams open_params;
	ImxVpuApiEncReturnCodes enc_ret;
	ImxVpuApiEncStreamInfo const *enc_stream_info;
	uint32_t enc_flags;

	ctx = calloc(1, sizeof(Context));
	ctx->y4m_input_file = input_file;
	ctx->h264_output_file = output_file;
	ctx->frame_context_counter = FRAME_CONTEXT_START;

	/* Retrieve global, static, invariant information about the encoder. */
	ctx->enc_global_info = imx_vpu_api_enc_get_global_info();
	assert(ctx->enc_global_info != NULL);

	enc_flags = ctx->enc_global_info->flags;

	/* Check that the codec actually supports encoding. */
	if (!(enc_flags & IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_HAS_ENCODER))
	{
		fprintf(stderr, "HW codec does not support encoding!\n");
		goto error;
	}

	/* Print global encoder information. */
	fprintf(stderr, "global encoder information:\n");
	fprintf(stderr, "semi planar frames supported: %d\n", !!(enc_flags & IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED));
	fprintf(stderr, "fully planar frames supported: %d\n", !!(enc_flags & IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED));
	fprintf(stderr, "min required stream buffer size: %zu\n", ctx->enc_global_info->min_required_stream_buffer_size);
	fprintf(stderr, "required stream buffer physaddr alignment: %zu\n", ctx->enc_global_info->required_stream_buffer_physaddr_alignment);
	fprintf(stderr, "required stream buffer size alignment: %zu\n", ctx->enc_global_info->required_stream_buffer_size_alignment);
	fprintf(stderr, "num supported compression formats: %zu\n", ctx->enc_global_info->num_supported_compression_formats);
	for (i = 0; i < ctx->enc_global_info->num_supported_compression_formats; ++i)
		fprintf(stderr, "  %s\n", imx_vpu_api_compression_format_string(ctx->enc_global_info->supported_compression_formats[i]));

	/* If the encoder can handle semi-planar frames, produce such frames.
	 * This is done by setting use_semi_planar_uv to a nonzero value.
	 * In that case, y4m_init() will produce a semi-planar color
	 * format, and y4m_read_frame() will read the frames into input_dmabuffer
	 * in a semi-planar fashion (that is, it will interleave U and V pixels
	 * in a combined UV plane). */
	ctx->y4m_context.use_semi_planar_uv = (ctx->enc_global_info->flags & IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED);
	if (!y4m_init(ctx->y4m_input_file, &(ctx->y4m_context), 1))
	{
		fprintf(stderr, "could not open Y4M input file\n");
		goto error;
	}

	/* Set up the DMA buffer allocator. We use this to allocate framebuffers
	 * and the stream buffer for the encoder. */
	ctx->allocator = imx_dma_buffer_allocator_new(&err);
	if (ctx->allocator == NULL)
	{
		fprintf(stderr, "could not create DMA buffer allocator: %s (%d)\n", strerror(err), err);
		goto error;
	}

	memset(&(open_params), 0, sizeof(open_params));
	imx_vpu_api_enc_set_default_open_params(IMX_VPU_API_COMPRESSION_FORMAT_H264, ctx->y4m_context.color_format, ctx->y4m_context.width, ctx->y4m_context.height, &open_params);

	/* Allocate the stream buffer that is used throughout the encoding process. */
	ctx->stream_buffer = imx_dma_buffer_allocate(
		ctx->allocator,
		ctx->enc_global_info->min_required_stream_buffer_size,
		ctx->enc_global_info->required_stream_buffer_physaddr_alignment,
		0
	);
	assert(ctx->stream_buffer != NULL);

	/* Open an encoder instance, using the previously allocated stream buffer. */
	enc_ret = imx_vpu_api_enc_open(&(ctx->encoder), &open_params, ctx->stream_buffer);
	if (enc_ret != IMX_VPU_API_ENC_RETURN_CODE_OK)
		goto error;

	/* Get the stream info from the encoder. */
	enc_stream_info = imx_vpu_api_enc_get_stream_info(ctx->encoder);
	ctx->stream_info = *enc_stream_info;

	/* Set the strides to make sure we can read frames from the Y4M output file. */
	ctx->y4m_context.y_stride = ctx->stream_info.frame_encoding_framebuffer_metrics.y_stride;
	ctx->y4m_context.uv_stride = ctx->stream_info.frame_encoding_framebuffer_metrics.uv_stride;

	if (!allocate_and_add_fb_pool_framebuffers(ctx, enc_stream_info->min_num_required_framebuffers))
	{
		fprintf(stderr, "Could not allocate %zu framebuffer(s)\n", enc_stream_info->min_num_required_framebuffers);
		goto error;
	}

	ctx->input_dmabuffer = imx_dma_buffer_allocate(ctx->allocator, enc_stream_info->min_framebuffer_size, enc_stream_info->framebuffer_alignment, &err);
	if (ctx->input_dmabuffer == NULL)
	{
		fprintf(stderr, "could not allocate DMA buffer for input framebuffer: %s (%d)\n", strerror(err), err);
		goto error;
	}


finish:
	return ctx;

error:
	shutdown(ctx);
	ctx = NULL;
	goto finish;
}


Retval run(Context *ctx)
{
	/* Feed frames to encoder & encode & output, until we run out of input data. */
	for (;;)
	{
		Retval ret;

		/* Push raw input frame into the encoder. */
		ret = push_raw_input_frame(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret != RETVAL_OK)
			return RETVAL_ERROR;

		/* Encode the previously pushed input data. */
		ret = encode_raw_frame(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
			return RETVAL_ERROR;
	}

	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	if (ctx == NULL)
		return;

	/* Close the previously opened encoder instance. */
	if (ctx->encoder != NULL)
		imx_vpu_api_enc_close(ctx->encoder);

	/* Free all allocated memory. */
	deallocate_framebuffers(ctx);
	if (ctx->input_dmabuffer)
		imx_dma_buffer_deallocate(ctx->input_dmabuffer);
	if (ctx->stream_buffer != NULL)
		imx_dma_buffer_deallocate(ctx->stream_buffer);

	/* Discard the DMA buffer allocator. */
	if (ctx->allocator != NULL)
		imx_dma_buffer_allocator_destroy(ctx->allocator);

	free(ctx);
}
