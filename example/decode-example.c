/* example for how to use the imxvpuapi decoder interface
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
#include "h264_utils.h"
#include "y4m_io.h"



/* This is a simple example of how to decode with the imxvpuapi library.
 * It decodes h.264-encoded frames and writes out the decoded frames to
 * a file.
 *
 * The input must be h.264 byte-stream data. Depending on the encoder,
 * it may or may not support/expect access unit delimiters. */


#define FRAME_CONTEXT_START          0x1000
#define FRAMEBUFFER_CONTEXT_START    0x2000


struct _Context
{
	/* Input/output file handles. */
	FILE *h264_input_file;
	FILE *y4m_output_file;

	/* Y4M I/O for decoded raw frames. */
	Y4MContext y4m_context;

	/* Helper structure to parse h.264 byte-stream data. */
	h264_context h264_ctx;

	/* DMA buffer allocator for the decoder's framebuffer pool and
	 * for output frames. */
	ImxDmaBufferAllocator *allocator;

	/* The actual VPU decoder. */
	ImxVpuApiDecoder *decoder;

	/* Stream buffer, used during decoding. */
	ImxDmaBuffer *stream_buffer;

	/* Pointer to the decoder's global information.
	 * Since this information is constant, it is not strictly
	 * necessary to keep a pointer to it here. This is just done
	 * for sake of convenience and clarity. */
	ImxVpuApiDecGlobalInfo const *dec_global_info;

	/* Copy of information about the stream that is to be decoded. This
	 * stream info becomes available when imx_vpu_api_dec_decode() output
	 * code is IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE . */
	ImxVpuApiDecStreamInfo stream_info;

	/* Framebuffer DMA buffers that are part of the decoder's
	 * framebuffer pool. */
	ImxDmaBuffer **fb_pool_dmabuffers;
	size_t num_fb_pool_framebuffers;

	/* Output DMA buffer. Only used if dec_global_info's
	 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
	 * flag is _not_ set. */
	ImxDmaBuffer *output_dmabuffer;

	/* A counter used here to simulate a frame context. Corresponding
	 * input and output frames have the same context. Not to be confused
	 * with a framebuffer context (see allocate_and_add_fb_pool_framebuffers). */
	unsigned int frame_context_counter;
};


/* Allocates the output DMA buffer that is used if the
 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * flag in the decoder's ImxVpuApiDecGlobalInfo is _not_ set. This is
 * called once the stream info is known. */
static int allocate_output_framebuffer(Context *ctx)
{
	int err;

	/* Discard any existing buffer first. */
	if (ctx->output_dmabuffer != NULL)
	{
		imx_dma_buffer_deallocate(ctx->output_dmabuffer);
		ctx->output_dmabuffer = NULL;
	}

	/* Allocate a new DMA buffer. We use the stream info for _output_ DMA buffers.
	 * This one can be different from the info for framebuffer pool DMA buffers.
	 * For example, some decoders need some extra space at the end of the FB pool
	 * DMA buffers for internal data, while output buffers do not ever need to
	 * make room for that (they only ever contain actual frame data). */
	ctx->output_dmabuffer = imx_dma_buffer_allocate(
		ctx->allocator,
		ctx->stream_info.min_output_framebuffer_size,
		ctx->stream_info.output_framebuffer_alignment,
		&err
	);
	if (ctx->output_dmabuffer == NULL)
	{
		fprintf(stderr, "could not allocate DMA buffer for FB pool framebuffer: %s (%d)\n", strerror(err), err);
		return 0;
	}

	return 1;
}


/* Allocates the DMA buffers for the decoder's framebuffer pool. This is
 * called once the stream info is known, and is always called, even if
 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * is not set. We only _add_ framebuffers here, since framebuffers cannot
 * be removed from a decoder's pool once added (until the decoder is closed). */
static int allocate_and_add_fb_pool_framebuffers(Context *ctx, size_t num_fb_pool_framebuffers_to_add)
{
	int ret = 1;
	size_t old_num_fb_pool_framebuffers;
	size_t new_num_fb_pool_framebuffers;
	ImxDmaBuffer **new_fb_pool_dmabuffers_array;
	void **fb_contexts = NULL;
	ImxVpuApiDecReturnCodes dec_ret;
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

	/* Allocate an array here to place the simulated FB context information
	 * in. This also shows how FB context array parameters for the
	 * imx_vpu_api_dec_add_framebuffers_to_pool() function does not continue
	 * to exist after the function call. */
	fb_contexts = malloc(num_fb_pool_framebuffers_to_add * sizeof(void *));
	assert(fb_contexts != NULL);

	/* Allocate new framebuffers and fill the FB context array. */
	for (i = old_num_fb_pool_framebuffers; i < ctx->num_fb_pool_framebuffers; ++i)
	{
		ctx->fb_pool_dmabuffers[i] = imx_dma_buffer_allocate(
			ctx->allocator,
			ctx->stream_info.min_fb_pool_framebuffer_size,
			ctx->stream_info.fb_pool_framebuffer_alignment,
			&err
		);
		if (ctx->fb_pool_dmabuffers[i] == NULL)
		{
			fprintf(stderr, "could not allocate DMA buffer for FB pool framebuffer: %s (%d)\n", strerror(err), err);
			ret = 0;
			goto finish;
		}

		fb_contexts[i - old_num_fb_pool_framebuffers] = (void *)(FRAMEBUFFER_CONTEXT_START + i);
	}

	/* Add the newly allocated framebuffers to the decoder's pool. */
	dec_ret = imx_vpu_api_dec_add_framebuffers_to_pool(ctx->decoder, ctx->fb_pool_dmabuffers + old_num_fb_pool_framebuffers, fb_contexts, num_fb_pool_framebuffers_to_add);
	if (dec_ret != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		fprintf(stderr, "could not add framebuffers to VPU pool: %s\n", imx_vpu_api_dec_return_code_string(dec_ret));
		ret = 0;
		goto finish;
	}


finish:
	free(fb_contexts);
	return ret;
}


/* Deallocate all previously allocated framebuffers. Before calling this,
 * the decoder must have been closed, since otherwise, deallocating its
 * FB pool framebuffer DMA buffers could cause undefined behavior.
 * (It is also valid to call this when the decoder reports a new stream
 * info. See decode_encoded_frames() for details.) */
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


/* Push h.264 input frame into the decoder. */
static Retval push_encoded_input_frame(Context *ctx)
{
	int ok;
	ImxVpuApiEncodedFrame encoded_frame;
	ImxVpuApiDecReturnCodes vpudec_ret;

	ok = h264_ctx_read_access_unit(&(ctx->h264_ctx));

	if (ctx->h264_ctx.au_end_offset <= ctx->h264_ctx.au_start_offset)
		return RETVAL_EOS;

	encoded_frame.data = ctx->h264_ctx.in_buffer + ctx->h264_ctx.au_start_offset;
	encoded_frame.data_size = ctx->h264_ctx.au_end_offset - ctx->h264_ctx.au_start_offset;
	encoded_frame.context = (void*)((uintptr_t)(ctx->frame_context_counter));

	ctx->frame_context_counter++;

	fprintf(stderr, "pushing encoded frame with context %p and %zu byte into encoder\n", encoded_frame.context, encoded_frame.data_size);

	vpudec_ret = imx_vpu_api_dec_push_encoded_frame(ctx->decoder, &encoded_frame);
	if (vpudec_ret != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		fprintf(stderr, "imx_vpu_api_dec_push_encoded_frame() failed: %s\n", imx_vpu_api_dec_return_code_string(vpudec_ret));
		return RETVAL_ERROR;
	}

	return ok ? RETVAL_OK : RETVAL_EOS;
}


/* Decode encoded frames that were previously pushed into the decoder
 * in the push_encoded_input_frame() function. The core of this function
 * is the imx_vpu_api_dec_decode() call, which is repeatedly called
 * until another encoded input frame needs to be pushed into the encoder,
 * EOS is reported, an error occurs, or the decoder is flushed by the
 * imx_vpu_api_dec_flush() call (which we do not use in this example). */
static Retval decode_encoded_frames(Context *ctx)
{
	ImxVpuApiDecOutputCodes output_code;
	ImxVpuApiDecReturnCodes dec_ret;
	Retval retval = RETVAL_OK;
	int do_loop = 1;

	/* output_dmabuffer is allocated in allocate_output_framebuffer(),
	 * which is only called if the ImxVpuApiDecGlobalInfo flag
	 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
	 * is set. */
	if (ctx->output_dmabuffer != NULL)
		imx_vpu_api_dec_set_output_frame_dma_buffer(ctx->decoder, ctx->output_dmabuffer, (void *)FRAMEBUFFER_CONTEXT_START);

	do
	{
		/* Perform a decoding step. */
		if ((dec_ret = imx_vpu_api_dec_decode(ctx->decoder, &output_code)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
		{
			fprintf(stderr, "imx_vpu_dec_decode() failed: %s\n", imx_vpu_api_dec_return_code_string(dec_ret));
			return RETVAL_ERROR;
		}

		switch (output_code)
		{
			case IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:
				/* Decoder did not produce a decoded frame yet, and has nothing
				 * else to report. Continue calling imx_vpu_api_dec_decode(). */
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_EOS:
				/* Decoder reached the end-of-stream. No more frames can be decoded.
				 * At this point, all that can be done is to close the decoder,
				 * so exit the loop. */
				fprintf(stderr, "VPU reports EOS; no more decoded frames available\n");
				retval = RETVAL_EOS;
				do_loop = 0;
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE:
			{
				/* There is new stream information. This indicates that the decoder
				 * found a new stream. With most formats, this happens only once,
				 * at the beginning of the stream. Some formats like h.264 may change
				 * parameters like the video resolution mid-stream. In these cases,
				 * we may enter this location again to process the new stream info.
				 *
				 * In here, we allocate DMA buffers for the FB pool. If any DMA
				 * buffers were previously allocated, we deallocate them. This is
				 * because as soon as there is a new stream info, any previous FB
				 * pool will have been torn down by the decoder, and its associated
				 * DMA buffers will no longer be in use. */

				ImxVpuApiFramebufferMetrics const *fb_metrics;
				ImxVpuApiDecStreamInfo const *stream_info = imx_vpu_api_dec_get_stream_info(ctx->decoder);

				deallocate_framebuffers(ctx);

				ctx->stream_info = *stream_info;

				if (!allocate_and_add_fb_pool_framebuffers(ctx, stream_info->min_num_required_framebuffers))
				{
					fprintf(stderr, "Could not allocate %zu framebuffer(s)\n", stream_info->min_num_required_framebuffers);
					retval = RETVAL_ERROR;
					do_loop = 0;
					break;
				}

				/* If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
				 * flag is not set, then the decoder places decoded frames into an external
				 * DMA buffer that needs to be allocated and set as the output buffer. */
				if (!(ctx->dec_global_info->flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL))
				{
					if (!allocate_output_framebuffer(ctx))
					{
						fprintf(stderr, "Could not allocate output framebuffer\n");
						retval = RETVAL_ERROR;
						do_loop = 0;
						break;
					}

					/* Set the newly allocated DMA buffer as the output buffer right here,
					 * before the next imx_vpu_api_dec_decode() call. This is necessary,
					 * otherwise the decoder has nowhere to place the decoded frame in. */
					imx_vpu_api_dec_set_output_frame_dma_buffer(ctx->decoder, ctx->output_dmabuffer, (void *)FRAMEBUFFER_CONTEXT_START);
				}

				/* Set up Y4M writer to write the decoded frames to the output file. */
				fb_metrics = &(stream_info->decoded_frame_framebuffer_metrics);
				ctx->y4m_context.width = fb_metrics->actual_frame_width;
				ctx->y4m_context.height = fb_metrics->actual_frame_height;
				ctx->y4m_context.y_stride = fb_metrics->y_stride;
				ctx->y4m_context.uv_stride = fb_metrics->uv_stride;
				ctx->y4m_context.interlacing = IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING;
				ctx->y4m_context.color_format = stream_info->color_format;
				if (!y4m_init(ctx->y4m_output_file, &(ctx->y4m_context), 0))
				{
					retval = RETVAL_ERROR;
					do_loop = 0;
					break;
				}

				break;
			}

			case IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:
				/* Decoder needs one more framebuffer added to its pool.
				 * Add one, otherwise decoding cannot continue. Then continue
				 * calling imx_vpu_api_dec_decode(). */

				if (!allocate_and_add_fb_pool_framebuffers(ctx, 1))
				{
					retval = RETVAL_ERROR;
					do_loop = 0;
				}

				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE:
			{
				/* Decoder produced a decoded frame. Get it, and write its
				 * pixels to the output Y4M file. */

				int err;
				uint8_t *virtual_address;
				ImxVpuApiRawFrame decoded_frame;
				ImxVpuApiFramebufferMetrics const *fb_metrics = &(ctx->stream_info.decoded_frame_framebuffer_metrics);

				/* Get the decoded frame. This call is mandatory; the decoder
				 * cannot continue until the decoded frame is retrieved. */
				if ((dec_ret = imx_vpu_api_dec_get_decoded_frame(ctx->decoder, &decoded_frame)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
				{
					fprintf(stderr, "imx_vpu_api_dec_get_decoded_frame() failed: %s\n", imx_vpu_api_dec_return_code_string(dec_ret));
					return RETVAL_ERROR;
				}

				virtual_address = imx_dma_buffer_map(decoded_frame.fb_dma_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_READ, &err);
				assert(virtual_address != NULL);

				fprintf(stderr, "got decoded frame\n");

				y4m_write_frame(
					&(ctx->y4m_context),
					virtual_address + fb_metrics->y_offset,
					virtual_address + fb_metrics->u_offset,
					virtual_address + fb_metrics->v_offset
				);

				imx_dma_buffer_unmap(decoded_frame.fb_dma_buffer);

				/* Return the decoded frame. After this call, the DMA buffer
				 * must not be touched, unless sometime later it is returned
				 * by the decoder in an ImxVpuApiRawFrame struct again.
				 * If however the ImxVpuApiDecGlobalInfo flag
				 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
				 * is not set, then calling this is not strictly necessary.
				 * It is not necessary however to check for that flag before
				 * calling this, since this function does nothing if the
				 * flag isn't set, so it is safe to always call it. */
				imx_vpu_api_dec_return_framebuffer_to_decoder(ctx->decoder, decoded_frame.fb_dma_buffer);

				break;
			}

			case IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:
				/* Decoding cannot continue, since the decoder ran out of
				 * encoded frames to decode. Exit the loop so that in the
				 * run() function, push_encoded_input_frame() can be called
				 * again to push a new encoded frame into the decoder. Then
				 * decoding can continue. */
				do_loop = 0;
				break;

			case IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED:
			{
				/* Frame was skipped by the decoder. Log this and continue
				 * the decoder loop. */
				ImxVpuApiDecSkippedFrameReasons reason;
				void *context;
				uint64_t pts, dts;
				imx_vpu_api_dec_get_skipped_frame_info(ctx->decoder, &reason, &context, &pts, &dts);
				fprintf(stderr, "frame got skipped:  reason %s context %p PTS %" PRIu64 " DTS %" PRIu64 "\n", imx_vpu_api_dec_skipped_frame_reason_string(reason), context, pts, dts);
				break;
			}

			default:
				fprintf(stderr, "UNKNOWN OUTPUT CODE %s (%d)\n", imx_vpu_api_dec_output_code_string(output_code), (int)output_code);
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
	ImxVpuApiDecOpenParams open_params;
	ImxVpuApiDecReturnCodes dec_ret;
	uint32_t dec_flags;

	ctx = calloc(1, sizeof(Context));
	ctx->h264_input_file = input_file;
	ctx->y4m_output_file = output_file;
	ctx->frame_context_counter = FRAME_CONTEXT_START;

	/* Retrieve global, static, invariant information about the decoder. */
	ctx->dec_global_info = imx_vpu_api_dec_get_global_info();
	assert(ctx->dec_global_info != NULL);

	dec_flags = ctx->dec_global_info->flags;

	/* Check that the codec actually supports decoding. */
	if (!(dec_flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER))
	{
		fprintf(stderr, "HW codec does not support decoding!\n");
		goto error;
	}

	/* Print global decoder information. */
	fprintf(stderr, "global decoder information:\n");
	fprintf(stderr, "semi planar frames supported: %d\n", !!(dec_flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED));
	fprintf(stderr, "fully planar frames supported: %d\n", !!(dec_flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED));
	fprintf(stderr, "decoded frames are from buffer pool: %d\n", !!(dec_flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL));
	fprintf(stderr, "min required stream buffer size: %zu\n", ctx->dec_global_info->min_required_stream_buffer_size);
	fprintf(stderr, "required stream buffer physaddr alignment: %zu\n", ctx->dec_global_info->required_stream_buffer_physaddr_alignment);
	fprintf(stderr, "required stream buffer size alignment: %zu\n", ctx->dec_global_info->required_stream_buffer_size_alignment);
	fprintf(stderr, "num supported compression formats: %zu\n", ctx->dec_global_info->num_supported_compression_formats);
	for (i = 0; i < ctx->dec_global_info->num_supported_compression_formats; ++i)
		fprintf(stderr, "  %s\n", imx_vpu_api_compression_format_string(ctx->dec_global_info->supported_compression_formats[i]));

	/* Set up the h264context helper to parse h.264 byte-stream data. */
	h264_ctx_init(&(ctx->h264_ctx), ctx->h264_input_file);

	/* Set up the DMA buffer allocator. We use this to allocate framebuffers
	 * and the stream buffer for the decoder. */
	ctx->allocator = imx_dma_buffer_allocator_new(&err);
	if (ctx->allocator == NULL)
	{
		fprintf(stderr, "could not create DMA buffer allocator: %s (%d)\n", strerror(err), err);
		goto error;
	}

	/* Set the open params. Enable frame reordering, use h.264 as the codec format.
	 * The memset() call ensures the other values are set to their default.
	 * (We do not need to set frame_width/frame_height for h.264, since these values
	 * are contained within the h.264 SPS NALUs. Also, h.264 byte-stream data does
	 * not require extra header data.) Also, enable semi-planar frames if supported. */
	memset(&(open_params), 0, sizeof(open_params));
	open_params.compression_format = IMX_VPU_API_COMPRESSION_FORMAT_H264;
	open_params.flags =
		  IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING
		| IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT
		;

	/* Allocate the stream buffer that is used throughout the decoding process. */
	ctx->stream_buffer = imx_dma_buffer_allocate(
		ctx->allocator,
		ctx->dec_global_info->min_required_stream_buffer_size,
		ctx->dec_global_info->required_stream_buffer_physaddr_alignment,
		0
	);
	assert(ctx->stream_buffer != NULL);

	/* Open a decoder instance, using the previously allocated bitstream buffer */
	if ((dec_ret = imx_vpu_api_dec_open(&(ctx->decoder), &open_params, ctx->stream_buffer)) != IMX_VPU_API_DEC_RETURN_CODE_OK)
	{
		fprintf(stderr, "could not open decoder instance: %s\n", imx_vpu_api_dec_return_code_string(dec_ret));
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
	/* Feed frames to decoder & decode & output, until we run out of input data. */
	for (;;)
	{
		Retval ret;

		/* Push encoded input frame into the decoder. */
		ret = push_encoded_input_frame(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret != RETVAL_OK)
			return RETVAL_ERROR;

		/* Decode the previously pushed input data. */
		ret = decode_encoded_frames(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
			return RETVAL_ERROR;
	}

	/* Enable drain mode. In this mode, any decoded frames that are still in the
	 * decoder are output.
	 * No input data is given, since there isn't any input data anymore. */
	fprintf(stderr, "draining decoder\n");
	imx_vpu_api_dec_enable_drain_mode(ctx->decoder);

	/* Now drain the remaining not-yet-encoded frames from the decoder. */
	for (;;)
	{
		Retval ret = decode_encoded_frames(ctx);
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

	/* Close the previously opened decoder instance. */
	if (ctx->decoder != NULL)
	{
		imx_vpu_api_dec_close(ctx->decoder);
		ctx->decoder = NULL;
	}

	/* Free all allocated memory. */
	deallocate_framebuffers(ctx);
	if (ctx->output_dmabuffer)
	{
		imx_dma_buffer_deallocate(ctx->output_dmabuffer);
		ctx->output_dmabuffer = NULL;
	}
	if (ctx->stream_buffer != NULL)
	{
		imx_dma_buffer_deallocate(ctx->stream_buffer);
		ctx->stream_buffer = NULL;
	}

	/* Discard the DMA buffer allocator. */
	if (ctx->allocator != NULL)
	{
		imx_dma_buffer_allocator_destroy(ctx->allocator);
		ctx->allocator = NULL;
	}

	/* Discard the h264context helper that was used to parse h.264 cata. */
	h264_ctx_cleanup(&(ctx->h264_ctx));

	free(ctx);
}
