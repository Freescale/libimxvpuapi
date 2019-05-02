/* example for how to use the imxvpuapi encoder interface to encode JPEGs
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



/* This is a simple example of how to encode JPEGs with the imxvpuapi library.
 * It reads the given raw YUV file and configures the VPU to encode JPEG data.
 * Then, the encoded JPEG data is written to the output file.
 *
 * This example expects as input a file with uncompressed 320x240 i420 frames.
 *
 * Note that using the JPEG encoder is optional, and it is perfectly OK to use
 * the lower-level video encoder API for JPEGs as well. (In fact, this is what
 * the JPEG encoder does internally.) The JPEG encoder is considerably easier
 * to use and requires less boilerplate code, however. */


#define FRAME_WIDTH 768
#define FRAME_HEIGHT 576
#define COLOR_FORMAT IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT


struct _Context
{
	/* Input/output file handles. */
	FILE *raw_input_file, *jpeg_output_file;

	/* The actual VPU JPEG encoder. */
	ImxVpuApiJpegEncoder *jpeg_encoder;

	/* DMA buffer allocator for the encoder's framebuffer pool and
	 * for output frames. */
	ImxDmaBufferAllocator *allocator;

	ImxVpuApiFramebufferMetrics const *fb_metrics;
};


Context* init(FILE *input_file, FILE *output_file)
{
	int err;
	Context *ctx;
	ImxVpuApiJpegEncParams params;

	ctx = calloc(1, sizeof(Context));
	ctx->raw_input_file = input_file;
	ctx->jpeg_output_file = output_file;


	/* Set up the DMA buffer allocator. We use this to allocate framebuffers
	 * and the stream buffer for the decoder. */
	ctx->allocator = imx_dma_buffer_allocator_new(&err);
	if (ctx->allocator == NULL)
	{
		fprintf(stderr, "could not create DMA buffer allocator: %s (%d)\n", strerror(err), err);
		goto error;
	}


	/* Open the JPEG encoder. */
	if (!imx_vpu_api_jpeg_enc_open(&(ctx->jpeg_encoder), ctx->allocator))
	{
		fprintf(stderr, "could not open VPU JPEG encoder\n");
		goto error;
	}


	/* Configure the JPEG encoder. */
	memset(&params, 0, sizeof(params));
	params.frame_width = FRAME_WIDTH;
	params.frame_height = FRAME_HEIGHT;
	params.color_format = COLOR_FORMAT;
	params.quality_factor = 5;
	if (!imx_vpu_api_jpeg_enc_set_params(ctx->jpeg_encoder, &params))
	{
		fprintf(stderr, "could not set JPEG encoding parameters\n");
		goto error;
	}


	ctx->fb_metrics = imx_vpu_api_jpeg_enc_get_framebuffer_metrics(ctx->jpeg_encoder);


finish:
	return ctx;

error:
	shutdown(ctx);
	ctx = NULL;
	goto finish;
}


Retval run(Context *ctx)
{
	Retval ret = RETVAL_OK;
	int err;
	long size;
	void *encoded_jpeg_data = NULL;
	ImxDmaBuffer *frame_dma_buffer = NULL;
	uint8_t *mapped_virtual_address;
	size_t encoded_data_size;


	/* Determine size of the input file to be able to read all of its bytes in one go. */
	fseek(ctx->raw_input_file, 0, SEEK_END);
	size = ftell(ctx->raw_input_file);
	fseek(ctx->raw_input_file, 0, SEEK_SET);


	/* Allocate a DMA buffer for the raw input frame. */
	frame_dma_buffer = imx_dma_buffer_allocate(ctx->allocator, size, 1, &err);
	if (frame_dma_buffer == NULL)
	{
		fprintf(stderr, "could not allocate DMA buffer for raw input frame: %s (%d)\n", strerror(err), err);
		goto error;
	}


	/* Map the DMA buffer and read the raw input data into it. */
	mapped_virtual_address = imx_dma_buffer_map(frame_dma_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE, &err);
	if (mapped_virtual_address == NULL)
	{
		fprintf(stderr, "could not map raw frame DMA buffer: %s (%d)\n", strerror(err), err);
		goto error;
	}
	fread(mapped_virtual_address, 1, size, ctx->raw_input_file);
	imx_dma_buffer_unmap(frame_dma_buffer);


	/* Encode the frame and write the encoded data to the output file. */
	if (!imx_vpu_api_jpeg_enc_encode(ctx->jpeg_encoder, frame_dma_buffer, &encoded_data_size))
	{
		fprintf(stderr, "could not encode frame to JPEG");
		goto error;
	}

	encoded_jpeg_data = malloc(encoded_data_size);
	assert(encoded_jpeg_data != NULL);

	if (!imx_vpu_api_jpeg_enc_get_encoded_data(ctx->jpeg_encoder, encoded_jpeg_data))
	{
		fprintf(stderr, "could not retrieve encoded JPEG data");
		goto error;
	}

	fwrite(encoded_jpeg_data, 1, encoded_data_size, ctx->jpeg_output_file);


finish:
	free(encoded_jpeg_data);
	if (frame_dma_buffer != NULL)
		imx_dma_buffer_deallocate(frame_dma_buffer);

	return ret;


error:
	ret = RETVAL_ERROR;
	goto finish;
}


void shutdown(Context *ctx)
{
	if (ctx == NULL)
		return;

	/* Shut down the JPEG encoder */
	imx_vpu_api_jpeg_enc_close(ctx->jpeg_encoder);

	free(ctx);
}
