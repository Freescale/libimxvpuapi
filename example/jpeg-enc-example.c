/* example for how to use the imxvpuapi encoder interface to encode JPEGs
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
#include <string.h>
#include "main.h"
#include "imxvpuapi/imxvpuapi_jpeg.h"



/* This is a simple example of how to encode JPEGs with the imxvpuapi library.
 * It reads the given raw YUV file and configures the VPU to encode MJPEG data.
 * Then, the encoded JPEG data is written to the output file.
 *
 * This example expects as input a file with uncompressed 320x240 i420 frames.
 *
 * Note that using the JPEG encoder is optional, and it is perfectly OK to use
 * the lower-level video encoder API for JPEGs as well. (In fact, this is what
 * the JPEG encoder does internally.) The JPEG encoder is considerably easier
 * to use and requires less boilerplate code, however. */


#define FRAME_Y_STRIDE 320
#define FRAME_CBCR_STRIDE (320 / 2)
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define FRAME_Y_SIZE ((FRAME_WIDTH) * (FRAME_HEIGHT))
#define FRAME_CBCR_SIZE ((FRAME_WIDTH) * (FRAME_HEIGHT) / 4)
#define FRAME_TOTAL_SIZE (FRAME_Y_SIZE + FRAME_CBCR_SIZE * 2)
#define COLOR_FORMAT IMX_VPU_COLOR_FORMAT_YUV420



struct _Context
{
	FILE *fin, *fout;
	ImxVpuJPEGEncoder *jpeg_encoder;
};


Context* init(FILE *input_file, FILE *output_file)
{
	Context *ctx;

	ctx = calloc(1, sizeof(Context));
	ctx->fin = input_file;
	ctx->fout = output_file;

	/* Open the JPEG encoder */
	imx_vpu_jpeg_enc_open(&(ctx->jpeg_encoder), NULL);

	return ctx;
}


void* acquire_output_buffer(void *context, size_t size, void **acquired_handle)
{
	void *mem;

	((void)(context));

	/* In this example, "acquire" a buffer by simply allocating it with malloc() */
	mem = malloc(size);
	*acquired_handle = mem;
	fprintf(stderr, "acquired output buffer, handle %p", *acquired_handle);
	return mem;
}


void finish_output_buffer(void *context, void *acquired_handle)
{
	((void)(context));

	/* Nothing needs to be done here in this example. Just log this call. */
	fprintf(stderr, "finished output buffer, handle %p", acquired_handle);
}


Retval run(Context *ctx)
{
	ImxVpuEncReturnCodes enc_ret;
	uint8_t *mapped_virtual_address;
	ImxVpuFramebuffer framebuffer;
	ImxVpuJPEGEncParams enc_params;
	void *acquired_handle;
	size_t output_buffer_size;


	/* Initialize the input framebuffer */

	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.y_stride = FRAME_Y_STRIDE;
	framebuffer.cbcr_stride = FRAME_CBCR_STRIDE;
	framebuffer.y_offset = 0;
	framebuffer.cb_offset = FRAME_Y_SIZE;
	framebuffer.cr_offset = FRAME_Y_SIZE + FRAME_CBCR_SIZE;

	/* Allocate a DMA buffer for the input pixels. In production,
	 * it is typically more efficient to make sure the input frames
	 * already come in DMA / physically contiguous memory, so the
	 * encoder can read from them directly. */
	framebuffer.dma_buffer = imx_vpu_dma_buffer_allocate(imx_vpu_enc_get_default_allocator(), FRAME_TOTAL_SIZE, 1, 0);
	if (framebuffer.dma_buffer == NULL)
	{
		fprintf(stderr, "could not allocate DMA buffer for input framebuffer\n");
		return RETVAL_ERROR;
	}

	/* Load the input pixels into the DMA buffer */
	mapped_virtual_address = imx_vpu_dma_buffer_map(framebuffer.dma_buffer, IMX_VPU_MAPPING_FLAG_WRITE);
	fread(mapped_virtual_address, 1, FRAME_TOTAL_SIZE, ctx->fin);
	imx_vpu_dma_buffer_unmap(framebuffer.dma_buffer);


	/* Set up the encoding parameters */#

	memset(&enc_params, 0, sizeof(enc_params));
	enc_params.frame_width = FRAME_WIDTH;
	enc_params.frame_height = FRAME_HEIGHT;
	enc_params.quality_factor = 85;
	enc_params.color_format = IMX_VPU_COLOR_FORMAT_YUV420;
	enc_params.acquire_output_buffer = acquire_output_buffer;
	enc_params.finish_output_buffer = finish_output_buffer;
	enc_params.output_buffer_context = NULL;


	/* Do the actual encoding */
	enc_ret = imx_vpu_jpeg_enc_encode(ctx->jpeg_encoder, &framebuffer, &enc_params, &acquired_handle, &output_buffer_size);

	/* The framebuffer's DMA buffer isn't needed anymore, since we just
	 * did the encoding, so deallocate it */
	imx_vpu_dma_buffer_deallocate(framebuffer.dma_buffer);

	if (enc_ret != IMX_VPU_ENC_RETURN_CODE_OK)
	{
		fprintf(stderr, "could not encode this image : %s\n", imx_vpu_enc_error_string(enc_ret));
		goto finish;
	}


	/* Write out the encoded frame to the output file. The encoder
	 * will have called acquire_output_buffer(), which acquires a
	 * buffer by malloc'ing it. The "handle" in this example is
	 * just the pointer to the allocated memory. This means that
	 * here, acquired_handle is the pointer to the encoded frame
	 * data. Write it to file. In production, the acquire function
	 * could retrieve an output memory block from a buffer pool for
	 * example. */
	fwrite(acquired_handle, 1, output_buffer_size, ctx->fout);


finish:
	if (acquired_handle != NULL)
		free(acquired_handle);

	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	/* Shut down the JPEG encoder */
	imx_vpu_jpeg_enc_close(ctx->jpeg_encoder);
}
