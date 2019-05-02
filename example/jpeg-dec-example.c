/* example for how to use the imxvpuapi decoder interface to decode JPEGs
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
#include "main.h"
#include "imxvpuapi2/imxvpuapi2_jpeg.h"



/* This is a simple example of how to decode JPEGs with the imxvpuapi library.
 * It reads the given JPEG file and configures the VPU to decode JPEG data.
 * Then, the decoded pixels are written to the output file, as raw pixels.
 *
 * Note that using the JPEG decoder is optional, and it is perfectly OK to use
 * the lower-level video decoder API for JPEGs as well. (In fact, this is what
 * the JPEG decoder does internally.) The JPEG decoder is considerably easier
 * to use and requires less boilerplate code, however. */



struct _Context
{
	/* Input/output file handles. */
	FILE *jpeg_input_file, *raw_output_file;

	/* The actual VPU JPEG decoder. */
	ImxVpuApiJpegDecoder *jpeg_decoder;

	/* DMA buffer allocator for the decoder's framebuffer pool and
	 * for output frames. */
	ImxDmaBufferAllocator *allocator;
};


Context* init(FILE *input_file, FILE *output_file)
{
	int err;
	Context *ctx;

	ctx = calloc(1, sizeof(Context));
	ctx->jpeg_input_file = input_file;
	ctx->raw_output_file = output_file;

	/* Set up the DMA buffer allocator. We use this to allocate framebuffers
	 * and the stream buffer for the decoder. */
	ctx->allocator = imx_dma_buffer_allocator_new(&err);
	if (ctx->allocator == NULL)
	{
		fprintf(stderr, "could not create DMA buffer allocator: %s (%d)\n", strerror(err), err);
		goto error;
	}

	/* Open the JPEG decoder. */
	if (!imx_vpu_api_jpeg_dec_open(&(ctx->jpeg_decoder), ctx->allocator))
	{
		fprintf(stderr, "could not open VPU JPEG decoder\n");
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
	int err;
	long size;
	void *buf;
	ImxVpuApiJpegDecInfo const *info;
	uint8_t *mapped_virtual_address;


	/* Determine size of the input file to be able to read all of its bytes in one go. */
	fseek(ctx->jpeg_input_file, 0, SEEK_END);
	size = ftell(ctx->jpeg_input_file);
	fseek(ctx->jpeg_input_file, 0, SEEK_SET);


	/* Allocate buffer for the input data, and read the data into it. */
	buf = malloc(size);
	fread(buf, 1, size, ctx->jpeg_input_file);

	fprintf(stderr, "encoded input frame:  size: %ld byte\n", size);

	/* Perform the actual JPEG decoding. */
	info = imx_vpu_api_jpeg_dec_decode(ctx->jpeg_decoder, buf, size);
	if (info == NULL)
	{
		free(buf);
		fprintf(stderr, "could not decode this JPEG image\n");
		return RETVAL_ERROR;
	}

	/* Input data is not needed anymore, so free the input buffer */
	free(buf);


	fprintf(
		stderr,
		"aligned frame size: %zu x %zu pixel  actual frame size: %zu x %zu pixel  Y/Cb/Cr stride: %zu/%zu/%zu  Y/Cb/Cr size: %zu/%zu/%zu  Y/Cb/Cr offset: %zu/%zu/%zu  color format: %s\n",
		info->framebuffer_metrics->aligned_frame_width, info->framebuffer_metrics->aligned_frame_height,
		info->framebuffer_metrics->actual_frame_width, info->framebuffer_metrics->actual_frame_height,
		info->framebuffer_metrics->y_stride, info->framebuffer_metrics->uv_stride, info->framebuffer_metrics->uv_stride,
		info->framebuffer_metrics->y_size, info->framebuffer_metrics->uv_size, info->framebuffer_metrics->uv_size,
		info->framebuffer_metrics->y_offset, info->framebuffer_metrics->u_offset, info->framebuffer_metrics->v_offset,
		imx_vpu_api_color_format_string(info->color_format)
	);


	/* Map the DMA buffer of the decoded picture, write out the decoded pixels, and unmap the buffer again. */
	fprintf(stderr, "decoded output picture:  writing %zu byte\n", info->total_frame_size);
	mapped_virtual_address = imx_dma_buffer_map(info->fb_dma_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_READ, &err);
	if (mapped_virtual_address == NULL)
	{
		fprintf(stderr, "could not map decoded frame DMA buffer: %s (%d)\n", strerror(err), err);
		return RETVAL_ERROR;
	}
	fwrite(mapped_virtual_address, 1, info->total_frame_size, ctx->raw_output_file);
	imx_dma_buffer_unmap(info->fb_dma_buffer);


	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	if (ctx == NULL)
		return;

	/* Shut down the JPEG decoder */
	imx_vpu_api_jpeg_dec_close(ctx->jpeg_decoder);

	free(ctx);
}
