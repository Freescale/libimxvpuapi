/* example for how to use the imxvpuapi encoder interface
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



/* This is a simple example of how to encode with the imxvpuapi library.
 * It encodes procedurally generated frames to h.264 and dumps the encoded
 * frames to a file. Also look into imxvpuapi.h for documentation.
 *
 * This expects as input a file with uncompressed 320x240 i420 frames and
 * 25 fps. The encoder outputs a byte-stream formatted h.264 stream, which
 * can be played with VLC or mplayer for example. */



#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define FRAME_SIZE ((FRAME_WIDTH) * (FRAME_HEIGHT) * 12 / 8)
#define COLOR_FORMAT IMX_VPU_COLOR_FORMAT_YUV420
#define FPS_N 25
#define FPS_D 1
#define FPS ((FPS_N) | ((FPS_D) << 16))


struct _Context
{
	FILE *fin, *fout;

	ImxVpuEncoder *vpuenc;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuEncInitialInfo initial_info;

	ImxVpuFramebuffer input_framebuffer;
	ImxVpuDMABuffer *input_fb_dmabuffer;

	ImxVpuDMABuffer *output_dmabuffer;

	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers;
	ImxVpuFramebufferSizes calculated_sizes;
};


Context* init(FILE *input_file, FILE *output_file)
{
	Context *ctx;
	ImxVpuEncOpenParams open_params;
	unsigned int i;

	ctx = calloc(1, sizeof(Context));
	ctx->fin = input_file;
	ctx->fout = output_file;

	imx_vpu_enc_set_default_open_params(IMX_VPU_CODEC_FORMAT_H264, &open_params);
	open_params.frame_width = FRAME_WIDTH;
	open_params.frame_height = FRAME_HEIGHT;
	open_params.framerate = FPS;

	imx_vpu_enc_load();
	imx_vpu_enc_get_bitstream_buffer_info(&(ctx->bitstream_buffer_size), &(ctx->bitstream_buffer_alignment));
	ctx->bitstream_buffer = imx_vpu_dma_buffer_allocate(imx_vpu_enc_get_default_allocator(), ctx->bitstream_buffer_size, ctx->bitstream_buffer_alignment, 0);
	imx_vpu_enc_open(&(ctx->vpuenc), &open_params, ctx->bitstream_buffer);
	imx_vpu_enc_get_initial_info(ctx->vpuenc, &(ctx->initial_info));

	ctx->num_framebuffers = ctx->initial_info.min_num_required_framebuffers;
	fprintf(stderr, "num framebuffers: %u\n", ctx->num_framebuffers);

	imx_vpu_calc_framebuffer_sizes(COLOR_FORMAT, FRAME_WIDTH, FRAME_HEIGHT, ctx->initial_info.framebuffer_alignment, 0, &(ctx->calculated_sizes));
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

		imx_vpu_fill_framebuffer_params(&(ctx->framebuffers[i]), &(ctx->calculated_sizes), ctx->fb_dmabuffers[i], 0);
	}

	/* allocate DMA buffers for the input and output buffers. Use total_size as size for both;
	 * the output buffer will most likely contain data later that is much smaller than the input,
	 * but just to be on the safe side, make sure that even an uncompressed frame could fit */
	ctx->input_fb_dmabuffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->calculated_sizes.total_size, ctx->initial_info.framebuffer_alignment, 0);
	imx_vpu_fill_framebuffer_params(&(ctx->input_framebuffer), &(ctx->calculated_sizes), ctx->input_fb_dmabuffer, 0);

	ctx->output_dmabuffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->calculated_sizes.total_size, ctx->initial_info.framebuffer_alignment, 0);

	/* Actual registration is done here. From this moment on, the VPU knows which buffers to use for
	 * storing temporary pictures into. This call must not be done again until encoding is shut down. */
	imx_vpu_enc_register_framebuffers(ctx->vpuenc, ctx->framebuffers, ctx->num_framebuffers);

	return ctx;
}


Retval run(Context *ctx)
{
	ImxVpuPicture input_picture;
	ImxVpuEncodedFrame output_frame;
	ImxVpuEncParams enc_params;
	unsigned int output_code;

	memset(&input_picture, 0, sizeof(input_picture));
	input_picture.framebuffer = &(ctx->input_framebuffer);

	memset(&enc_params, 0, sizeof(enc_params));
	enc_params.frame_width = FRAME_WIDTH;
	enc_params.frame_height = FRAME_HEIGHT;
	enc_params.framerate = FPS;
	enc_params.quant_param = 0;

	memset(&output_frame, 0, sizeof(output_frame));
	output_frame.data.dma_buffer = ctx->output_dmabuffer;

	/* Read input i420 frames and encode them until the end of the input file is reached */
	for (;;)
	{
		uint8_t *mapped_virtual_address;
		imx_vpu_phys_addr_t mapped_physical_address;

		/* Read uncompressed pixels into the input DMA buffer */
		imx_vpu_dma_buffer_map(ctx->input_fb_dmabuffer, &mapped_virtual_address, &mapped_physical_address, IMX_VPU_MAPPING_FLAG_WRITE_ONLY);
		fread(mapped_virtual_address, 1, FRAME_SIZE, ctx->fin);
		imx_vpu_dma_buffer_unmap(ctx->input_fb_dmabuffer);

		/* Stop encoding if EOF was reached */
		if (feof(ctx->fin))
			break;

		/* The actual encoding */
		imx_vpu_enc_encode(ctx->vpuenc, &input_picture, &output_frame, &enc_params, &output_code);

		/* Write out the encoded frame to the output file */
		imx_vpu_dma_buffer_map(ctx->output_dmabuffer, &mapped_virtual_address, &mapped_physical_address, IMX_VPU_MAPPING_FLAG_READ_ONLY);
		fwrite(mapped_virtual_address, 1, output_frame.data_size, ctx->fout);
		imx_vpu_dma_buffer_unmap(ctx->input_fb_dmabuffer);
	}

	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	unsigned int i;

	imx_vpu_enc_close(ctx->vpuenc);

	imx_vpu_dma_buffer_deallocate(ctx->input_fb_dmabuffer);
	imx_vpu_dma_buffer_deallocate(ctx->output_dmabuffer);

	free(ctx->framebuffers);
	for (i = 0; i < ctx->num_framebuffers; ++i)
		imx_vpu_dma_buffer_deallocate(ctx->fb_dmabuffers[i]);
	free(ctx->fb_dmabuffers);
	imx_vpu_dma_buffer_deallocate(ctx->bitstream_buffer);

	imx_vpu_enc_unload();

	free(ctx);
}
