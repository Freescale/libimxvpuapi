/* example for how to use the imxvpuapi decoder interface to decode JPEGs
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



/* This is a simple example of how to decode JPEGs with the imxvpuapi library.
 * It reads the given JPEG file and configures the VPU to decode MJPEG data.
 * Then, the decoded pixels are written to the output file, in PPM format. */



struct _Context
{
	FILE *fin, *fout;

	ImxVpuDecoder *vpudec;

	ImxVpuDMABuffer *bitstream_buffer;
	size_t bitstream_buffer_size;
	unsigned int bitstream_buffer_alignment;

	ImxVpuDecInitialInfo initial_info;

	ImxVpuFramebuffer *framebuffers;
	ImxVpuDMABuffer **fb_dmabuffers;
	unsigned int num_framebuffers;
	ImxVpuFramebufferSizes calculated_sizes;
};


Context* init(FILE *input_file, FILE *output_file)
{
	Context *ctx;
	ImxVpuDecOpenParams open_params;

	ctx = calloc(1, sizeof(Context));
	ctx->fin = input_file;
	ctx->fout = output_file;

	open_params.codec_format = IMX_VPU_CODEC_FORMAT_MJPEG;
	open_params.frame_width = 0;
	open_params.frame_height = 0;

	imx_vpu_dec_load();
	imx_vpu_dec_get_bitstream_buffer_info(&(ctx->bitstream_buffer_size), &(ctx->bitstream_buffer_alignment));
	ctx->bitstream_buffer = imx_vpu_dma_buffer_allocate(imx_vpu_dec_get_default_allocator(), ctx->bitstream_buffer_size, ctx->bitstream_buffer_alignment, 0);
	imx_vpu_dec_open(&(ctx->vpudec), &open_params, ctx->bitstream_buffer);

	return ctx;
}


Retval run(Context *ctx)
{
	unsigned int output_code;

	{
		long size;
		void *buf;

		fseek(ctx->fin, 0, SEEK_END);
		size = ftell(ctx->fin);
		fseek(ctx->fin, 0, SEEK_SET);

		buf = malloc(size);
		fread(buf, 1, size, ctx->fin);

		ImxVpuEncodedFrame encoded_frame;
		encoded_frame.data.virtual_address = buf;
		encoded_frame.data_size = size;
		/* Codec data is out-of-band data that is typically stored in a separate space
		 * in containers for each elementary stream; JPEG data does not need it */
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;

		fprintf(stderr, "encoded input frame:  size: %u byte\n", encoded_frame.data_size);

		/* Perform the actual decoding */
		imx_vpu_dec_decode(ctx->vpudec, &encoded_frame, &output_code);

		free(buf);
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
			"initial info:  size: %ux%u pixel  rate: %u/%u  min num required framebuffers: %u  interlacing: %d  framebuffer alignment: %u  color format: ",
			ctx->initial_info.frame_width,
			ctx->initial_info.frame_height,
			ctx->initial_info.frame_rate_numerator,
			ctx->initial_info.frame_rate_denominator,
			ctx->initial_info.min_num_required_framebuffers,
			ctx->initial_info.interlacing,
			ctx->initial_info.framebuffer_alignment
		);
		switch (ctx->initial_info.color_format)
		{
			case IMX_VPU_COLOR_FORMAT_YUV420: fprintf(stderr, "YUV 4:2:0"); break;
			case IMX_VPU_COLOR_FORMAT_YUV422_HORIZONTAL: fprintf(stderr, "YUV 4:2:2 horizontal"); break;
			case IMX_VPU_COLOR_FORMAT_YUV422_VERTICAL: fprintf(stderr, "YUV 4:2:2 vertical"); break;
			case IMX_VPU_COLOR_FORMAT_YUV444: fprintf(stderr, "YUV 4:4:4"); break;
			case IMX_VPU_COLOR_FORMAT_YUV400: fprintf(stderr, "YUV 4:0:0 (8-bit grayscale)"); break;
		}
		fprintf(stderr, "\n");

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

			imx_vpu_fill_framebuffer_params(&(ctx->framebuffers[i]), &(ctx->calculated_sizes), ctx->fb_dmabuffers[i], 0);
		}

		/* Actual registration is done here. From this moment on, the VPU knows which buffers to use for
		 * storing decoded pictures into. This call must not be done again until decoding is shut down or
		 * IMX_VPU_DEC_OUTPUT_CODE_INITIAL_INFO_AVAILABLE is set again. */
		imx_vpu_dec_register_framebuffers(ctx->vpudec, ctx->framebuffers, ctx->num_framebuffers);
	}

	/* Enable drain mode. All available input data is
	 * inserted. Now We want one output picture. */
	imx_vpu_dec_enable_drain_mode(ctx->vpudec, 1);

	/* Get the decoded picture out of the VPU */
	{
		ImxVpuEncodedFrame encoded_frame;

		/* In drain mode there is no input data */
		encoded_frame.data.virtual_address = NULL;
		encoded_frame.data_size = 0;
		encoded_frame.codec_data = NULL;
		encoded_frame.codec_data_size = 0;
		encoded_frame.context = NULL;

		imx_vpu_dec_decode(ctx->vpudec, &encoded_frame, &output_code);

		/* A decoded picture is available for further processing. Retrieve it, do something
		 * with it, and once the picture is no longer needed, mark it as displayed. This
		 * marks it internally as available for further decoding by the VPU. */
		if (output_code & IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE)
		{
			ImxVpuPicture decoded_picture;
			uint8_t *mapped_virtual_address;
			size_t num_out_byte = ctx->calculated_sizes.y_size + ctx->calculated_sizes.cbcr_size * 2;

			/* This call retrieves information about the decoded picture, including
			 * a pointer to the corresponding framebuffer structure. This must not be called more
			 * than once after IMX_VPU_DEC_OUTPUT_CODE_DECODED_PICTURE_AVAILABLE was set. */
			imx_vpu_dec_get_decoded_picture(ctx->vpudec, &decoded_picture);
			fprintf(stderr, "decoded output picture:  writing %u byte", num_out_byte);

			/* Map buffer to the local address space, dump the decoded frame to file,
			 * and unmap again. The decoded frame uses the I420 color format for all
			 * bitstream formats (h.264, MPEG2 etc.), with one exception; with motion JPEG data,
			 * the format can be different. See imxvpuapi.h for details. */
			mapped_virtual_address = imx_vpu_dma_buffer_map(decoded_picture.framebuffer->dma_buffer, IMX_VPU_MAPPING_FLAG_READ_ONLY);
			fwrite(mapped_virtual_address, 1, num_out_byte, ctx->fout);
			imx_vpu_dma_buffer_unmap(decoded_picture.framebuffer->dma_buffer);

			/* Mark the framebuffer as displayed, thus returning it to the list of
			 *framebuffers available for decoding. */
			imx_vpu_dec_mark_framebuffer_as_displayed(ctx->vpudec, decoded_picture.framebuffer);
		}
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

	free(ctx);
}
