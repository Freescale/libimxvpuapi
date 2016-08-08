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
 * can be played with VLC or mplayer for example.
 *
 * It demonstrates the alternative write-callback style encoding, compared
 * to encode-example.c (= write_output_data is set instead of
 * acquire_output_buffer and finish_output_buffer). */



#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define FRAME_SIZE ((FRAME_WIDTH) * (FRAME_HEIGHT) * 12 / 8)
#define COLOR_FORMAT IMX_VPU_COLOR_FORMAT_YUV420
#define FPS_N 25
#define FPS_D 1


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


	/* Set the open params. Use the default values (note that memset must still
	 * be called to ensure all values are set to 0 initially; the
	 * imx_vpu_enc_set_default_open_params() function does not do this!).
	 * Then, set a bitrate of 0 kbps, which tells the VPU to use constant quality
	 * mode instead (controlled by the quant_param field in ImxVpuEncParams).
	 * Frame width & height are also necessary, as are the frame rate numerator
	 * and denominator. Also, access unit delimiters are enabled to demonstrate
	 * their use. */
	memset(&open_params, 0, sizeof(open_params));
	imx_vpu_enc_set_default_open_params(IMX_VPU_CODEC_FORMAT_H264, &open_params);
	open_params.bitrate = 0;
	open_params.frame_width = FRAME_WIDTH;
	open_params.frame_height = FRAME_HEIGHT;
	open_params.frame_rate_numerator = FPS_N;
	open_params.frame_rate_denominator = FPS_D;
	open_params.codec_params.h264_params.enable_access_unit_delimiters = 1;


	/* Load the VPU firmware */
	imx_vpu_enc_load();

	/* Retrieve information about the required bitstream buffer and allocate one based on this */
	imx_vpu_enc_get_bitstream_buffer_info(&(ctx->bitstream_buffer_size), &(ctx->bitstream_buffer_alignment));
	ctx->bitstream_buffer = imx_vpu_dma_buffer_allocate(
		imx_vpu_enc_get_default_allocator(),
		ctx->bitstream_buffer_size,
		ctx->bitstream_buffer_alignment,
		0
	);

	/* Open an encoder instance, using the previously allocated bitstream buffer */
	imx_vpu_enc_open(&(ctx->vpuenc), &open_params, ctx->bitstream_buffer);


	/* Retrieve the initial information to allocate framebuffers for the
	 * encoding process (unlike with decoding, these framebuffers are used
	 * only internally by the encoder as temporary storage; encoded data
	 * doesn't go in there, nor do raw input frames) */
	imx_vpu_enc_get_initial_info(ctx->vpuenc, &(ctx->initial_info));

	ctx->num_framebuffers = ctx->initial_info.min_num_required_framebuffers;
	fprintf(stderr, "num framebuffers: %u\n", ctx->num_framebuffers);

	/* Using the initial information, calculate appropriate framebuffer sizes */
	imx_vpu_calc_framebuffer_sizes(COLOR_FORMAT, FRAME_WIDTH, FRAME_HEIGHT, ctx->initial_info.framebuffer_alignment, 0, 0, &(ctx->calculated_sizes));
	fprintf(
		stderr,
		"calculated sizes:  frame width&height: %dx%d  Y stride: %u  CbCr stride: %u  Y size: %u  CbCr size: %u  MvCol size: %u  total size: %u\n",
		ctx->calculated_sizes.aligned_frame_width, ctx->calculated_sizes.aligned_frame_height,
		ctx->calculated_sizes.y_stride, ctx->calculated_sizes.cbcr_stride,
		ctx->calculated_sizes.y_size, ctx->calculated_sizes.cbcr_size, ctx->calculated_sizes.mvcol_size,
		ctx->calculated_sizes.total_size
	);


	/* Allocate memory blocks for the framebuffer and DMA buffer structures,
	 * and allocate the DMA buffers themselves */

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

	/* allocate DMA buffers for the raw input frames. Since the encoder can only read
	 * raw input pixels from a DMA memory region, it is necessary to allocate one,
	 * and later copy the pixels into it. In production, it is generally a better
	 * idea to make sure that the raw input frames are already placed in DMA memory
	 * (either allocated by imx_vpu_dma_buffer_allocate() or by some other means of
	 * getting DMA / physically contiguous memory with known physical addresses). */
	ctx->input_fb_dmabuffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->calculated_sizes.total_size, ctx->initial_info.framebuffer_alignment, 0);
	imx_vpu_fill_framebuffer_params(&(ctx->input_framebuffer), &(ctx->calculated_sizes), ctx->input_fb_dmabuffer, 0);

	/* Actual registration is done here. From this moment on, the VPU knows which buffers to use for
	 * storing temporary frames into. This call must not be done again until encoding is shut down. */
	imx_vpu_enc_register_framebuffers(ctx->vpuenc, ctx->framebuffers, ctx->num_framebuffers);

	return ctx;
}


int write_output_data(void *context, uint8_t const *data, uint32_t size, ImxVpuEncodedFrame *encoded_frame)
{
	FILE *f;
	
	((void)(encoded_frame));

	f = (FILE *)context;

	fwrite(data, 1, size, f);

	return 1;
}


Retval run(Context *ctx)
{
	ImxVpuRawFrame input_frame;
	ImxVpuEncodedFrame output_frame;
	ImxVpuEncParams enc_params;
	unsigned int output_code;

	/* Set up the input frame. The only field that needs to be
	 * set is the input framebuffer. The encoder will read from it.
	 * The rest can remain zero/NULL. */
	memset(&input_frame, 0, sizeof(input_frame));
	input_frame.framebuffer = &(ctx->input_framebuffer);

	/* Set the encoding parameters for this frame. quant_param 0 is
	 * the highest quality in h.264 constant quality encoding mode.
	 * (The range in h.264 is 0-51, where 0 is best quality and worst
	 * compression, and 51 vice versa.)
	 * Also pass the file handle to the output buffer context so
	 * the write_output_data() function can use it. */
	memset(&enc_params, 0, sizeof(enc_params));
	enc_params.quant_param = 0;
	enc_params.write_output_data = write_output_data;
	enc_params.output_buffer_context = ctx->fout;

	/* Set up the output frame. Simply setting all fields to zero/NULL
	 * is enough. The encoder will fill in data. */
	memset(&output_frame, 0, sizeof(output_frame));

	/* Read input i420 frames and encode them until the end of the input file is reached */
	for (;;)
	{
		uint8_t *mapped_virtual_address;

		/* Read uncompressed pixels into the input DMA buffer */
		mapped_virtual_address = imx_vpu_dma_buffer_map(ctx->input_fb_dmabuffer, IMX_VPU_MAPPING_FLAG_WRITE);
		fread(mapped_virtual_address, 1, FRAME_SIZE, ctx->fin);
		imx_vpu_dma_buffer_unmap(ctx->input_fb_dmabuffer);

		/* Stop encoding if EOF was reached */
		if (feof(ctx->fin))
			break;

		/* The actual encoding. It internally calls write_output_data()
		 * whenever it has new data to output. */
		imx_vpu_enc_encode(ctx->vpuenc, &input_frame, &output_frame, &enc_params, &output_code);
	}

	return RETVAL_OK;
}


void shutdown(Context *ctx)
{
	unsigned int i;

	/* Close the previously opened encoder instance */
	imx_vpu_enc_close(ctx->vpuenc);

	/* Free all allocated memory (both regular and DMA memory) */
	imx_vpu_dma_buffer_deallocate(ctx->input_fb_dmabuffer);
	free(ctx->framebuffers);
	for (i = 0; i < ctx->num_framebuffers; ++i)
		imx_vpu_dma_buffer_deallocate(ctx->fb_dmabuffers[i]);
	free(ctx->fb_dmabuffers);
	imx_vpu_dma_buffer_deallocate(ctx->bitstream_buffer);

	/* Unload the VPU firmware */
	imx_vpu_enc_unload();

	free(ctx);
}
