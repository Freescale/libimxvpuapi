/* example for how to use the imxvpuapi decoder interface
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


#include <stdio.h>
#include <stdlib.h>
#include "main.h"
#include "h264_utils.h"



/* This is a simple example of how to decode with the imxvpuapi library.
 * It decodes h.264 byte stream data and dumps the decoded frames to a file.
 * Also look into imxvpuapi.h for documentation. */



struct _Context
{
	FILE *fin, *fout;

	h264_context h264_ctx;

	ImxVpuDecoder *vpudec;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuDecInitialInfo initial_info;

	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers;
	ImxVpuFramebufferSizes calculated_sizes;

	unsigned int frame_id_counter;
};


Context* init(FILE *input_file, FILE *output_file)
{
	Context *ctx;
	ImxVpuDecOpenParams open_params;

	ctx = calloc(1, sizeof(Context));
	ctx->fin = input_file;
	ctx->fout = output_file;
	ctx->frame_id_counter = 100;

	h264_ctx_init(&(ctx->h264_ctx), ctx->fin);

	open_params.codec_format = IMX_VPU_CODEC_FORMAT_H264;
	open_params.frame_width = 0;
	open_params.frame_height = 0;
	open_params.enable_frame_reordering = 1;

	imx_vpu_dec_load();
	imx_vpu_dec_get_bitstream_buffer_info(&(ctx->bitstream_buffer_size), &(ctx->bitstream_buffer_alignment));
	ctx->bitstream_buffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->bitstream_buffer_size, ctx->bitstream_buffer_alignment, 0);
	imx_vpu_dec_open(&(ctx->vpudec), &open_params, ctx->bitstream_buffer);

	return ctx;
}


static int decode_frame(Context *ctx)
{
	ImxVpuEncodedFrame encoded_frame;
	int ok;
	unsigned int output_code;
	ImxVpuDecReturnCodes ret;

	if (imx_vpu_dec_is_drain_mode_enabled(ctx->vpudec))
	{
		/* In drain mode there is no input data */
		encoded_frame.data.virtual_address = NULL;
		encoded_frame.data_size = 0;
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;
		encoded_frame.context = NULL;

		ok = 1;
	}
	else
	{
		/* Regular mode; read input data and feed it to the decoder */

		ok = h264_ctx_read_access_unit(&(ctx->h264_ctx));

		if (ctx->h264_ctx.au_end_offset <= ctx->h264_ctx.au_start_offset)
			return RETVAL_EOS;

		encoded_frame.data.virtual_address = ctx->h264_ctx.in_buffer + ctx->h264_ctx.au_start_offset;
		encoded_frame.data_size = ctx->h264_ctx.au_end_offset - ctx->h264_ctx.au_start_offset;
		/* Codec data is out-of-band data that is typically stored in a separate space in
		 * containers for each elementary stream; h.264 byte-stream does not need it */
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;
		/* The frame id counter is used to give the encoded frames an example context.
		 * The context of an encoded frame is a user-defined pointer that is passed along
		 * to the corresponding decoded picture. This way, it can be determined which
		 * decoded picture is the result of what encoded frame.
		 * For example purposes (to be able to print some log output), the context
		 * is just a monotonically increasing integer. */
		encoded_frame.context = (void *)((uintptr_t)(ctx->frame_id_counter));

		fprintf(stderr, "encoded input frame:  frame id: 0x%x  size: %u byte\n", ctx->frame_id_counter, encoded_frame.data_size);
	}

	/* Perform the actual decoding */
	if ((ret = imx_vpu_dec_decode(ctx->vpudec, &encoded_frame, &output_code)) != IMX_VPU_DEC_RETURN_CODE_OK)
	{
		fprintf(stderr, "imx_vpu_dec_decode() failed: %s\n", imx_vpu_dec_error_string(ret));
		return RETVAL_ERROR;
	}

	/* Initial info is now available; this usually happens right after the
	 * first frame is decoded, and this is the situation where one must register
	 * output framebuffers, which the decoder then uses like a buffer pool for
	 * picking buffers to decode frame into */
	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE)
	{
		unsigned int i;

		imx_vpu_dec_get_initial_info(ctx->vpudec, &(ctx->initial_info));
		fprintf(
			stderr,
			"initial info:  size: %ux%u pixel  rate: %u/%u  min num required framebuffers: %u  interlacing: %d  framebuffer alignment: %u\n",
			ctx->initial_info.frame_width,
			ctx->initial_info.frame_height,
			ctx->initial_info.frame_rate_numerator,
			ctx->initial_info.frame_rate_denominator,
			ctx->initial_info.min_num_required_framebuffers,
			ctx->initial_info.interlacing,
			ctx->initial_info.framebuffer_alignment
		);

		ctx->num_framebuffers = ctx->initial_info.min_num_required_framebuffers;

		imx_vpu_calc_framebuffer_sizes(ctx->initial_info.color_format, ctx->initial_info.frame_width, ctx->initial_info.frame_height, ctx->initial_info.framebuffer_alignment, ctx->initial_info.interlacing, &(ctx->calculated_sizes));
		fprintf(
			stderr,
			"calculated sizes:  frame width&height: %dx%d  Y stride: %u  CbCr stride: %u  Y size: %u  CbCr size: %u  MvCol size: %u  total size: %u\n",
			ctx->calculated_sizes.aligned_frame_width, ctx->calculated_sizes.aligned_frame_height,
			ctx->calculated_sizes.y_stride, ctx->calculated_sizes.cbcr_stride,
			ctx->calculated_sizes.y_size, ctx->calculated_sizes.cbcr_size, ctx->calculated_sizes.mvcol_size,
			ctx->calculated_sizes.total_size
		);

		ctx->framebuffers = malloc(sizeof(ImxVpuFramebuffer) * ctx->num_framebuffers);
		ctx->fb_dmabuffers = malloc(sizeof(ImxVpuDMABuffer*) * ctx->num_framebuffers);

		for (i = 0; i < ctx->num_framebuffers; ++i)
		{
			/* Allocate a DMA buffer for each framebuffer. It is possible to specify alternate allocators;
			 * all that is required is that the allocator provides physically contiguous memory
			 * (necessary for DMA transfers) and respecs the alignment value. */
			ctx->fb_dmabuffers[i] = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->calculated_sizes.total_size, ctx->initial_info.framebuffer_alignment, 0);

			/* The last parameter (the one with 0x2000 + i) is the context data for the framebuffers in the pool.
			 * It is possible to attach user-defined context data to them. Note that it is not related to the
			 * context data in en- and decoded pictures. For purposes of demonstrations, the context pointer
			 * is just a simple monotonically increasing integer. First framebuffer has context 0x2000, second 0x2001 etc. */
			imx_vpu_fill_framebuffer_params(&(ctx->framebuffers[i]), &(ctx->calculated_sizes), ctx->fb_dmabuffers[i], (void*)((uintptr_t)(0x2000 + i)));
		}

		/* Actual registration is done here. From this moment on, the VPU knows which buffers to use for
		 * storing decoded pictures into. This call must not be done again until decoding is shut down or
		 * IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE is set again. */
		imx_vpu_dec_register_framebuffers(ctx->vpudec, ctx->framebuffers, ctx->num_framebuffers);
	}

	/* A decoded picture is available for further processing. Retrieve it, do something
	 * with it, and once the picture is no longer needed, mark it as displayed. This
	 * marks it internally as available for further decoding by the VPU. */
	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE)
	{
		ImxVpuPicture decoded_picture;
		unsigned int frame_id;
		uint8_t *mapped_virtual_address;
		imx_vpu_phys_addr_t mapped_physical_address;
		size_t num_out_byte = ctx->calculated_sizes.y_size + ctx->calculated_sizes.cbcr_size * 2;

		/* This call retrieves information about the decoded picture, including
		 * a pointer to the corresponding framebuffer structure. This must not be called more
		 * than once after IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE was set. */
		imx_vpu_dec_get_decoded_picture(ctx->vpudec, &decoded_picture);
		frame_id = (unsigned int)((uintptr_t)(decoded_picture.context));
		fprintf(stderr, "decoded output picture:  frame id: 0x%x  writing %u byte\n", frame_id, num_out_byte);

		/* Map buffer to the local address space, dump the decoded frame to file,
		 * and unmap again. The decoded frame uses the I420 color format for all
		 * bitstream formats (h.264, MPEG2 etc.), with one exception; with motion JPEG data,
		 * the format can be different. See imxvpuapi.h for details. */
		imx_vpu_dma_buffer_map(decoded_picture.framebuffer->dma_buffer, &mapped_virtual_address, &mapped_physical_address, IMX_VPU_MAPPING_FLAG_READ_ONLY);
		fwrite(mapped_virtual_address, 1, num_out_byte, ctx->fout);
		imx_vpu_dma_buffer_unmap(decoded_picture.framebuffer->dma_buffer);

		/* Mark the framebuffer as displayed, thus returning it to the list of
		 *framebuffers available for decoding. */
		imx_vpu_dec_mark_framebuffer_as_displayed(ctx->vpudec, decoded_picture.framebuffer);
	}
	else if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DROPPED)
	{
		/* A frame was dropped. The context of the dropped frame can be retrieved
		 * if this is necessary for timestamping etc. */
		unsigned int dropped_frame_id = (unsigned int)((uintptr_t)(imx_vpu_dec_get_dropped_frame_context(ctx->vpudec)));
		fprintf(stderr, "dropped frame:  frame id: 0x%x\n", dropped_frame_id);
	}

	if (output_code & IMX_VPU_DEC_OUTPUT_CODE_EOS)
	{
		fprintf(stderr, "VPU reports EOS; no more decoded frames available\n");
		ok = 0;
	}

	ctx->frame_id_counter++;

	return ok ? RETVAL_OK : RETVAL_EOS;
}


Retval run(Context *ctx)
{
	/* Feed frames to decoder & decode & output, until we run out of input data */
	for (;;)
	{
		Retval ret = decode_frame(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
			return RETVAL_ERROR;
	}

	/* Enable drain mode; in this mode, any decoded pictures that are still in the
	 * decoder are output; no input data is given (since there isn't any input data anymore) */

	fprintf(stderr, "draining decoder\n");
	imx_vpu_dec_enable_drain_mode(ctx->vpudec, 1);

	for (;;)
	{
		Retval ret = decode_frame(ctx);
		if (ret == RETVAL_EOS)
			break;
		else if (ret == RETVAL_ERROR)
			return RETVAL_ERROR;
	}

	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	unsigned int i;

	imx_vpu_dec_close(ctx->vpudec);

	free(ctx->framebuffers);
	for (i = 0; i < ctx->num_framebuffers; ++i)
		imx_vpu_dma_buffer_deallocate(ctx->fb_dmabuffers[i]);
	free(ctx->fb_dmabuffers);
	imx_vpu_dma_buffer_deallocate(ctx->bitstream_buffer);

	imx_vpu_dec_unload();

	h264_ctx_cleanup(&(ctx->h264_ctx));

	free(ctx);
}
