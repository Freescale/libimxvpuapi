/* Simplified API for JPEG en- and decoding with the Freescale i.MX SoC
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


/* This is a convenience interface for simple en- and decoding of JPEG data.
 * For merely en/decoding JPEGs, having to set up a VPU en/decoder involves
 * a considerable amount of boilerplate code. This interface takes care of
 * these details, and presents a much simpler interface focused on this one
 * task: to en/decode JPEGs. */

#ifndef IMXVPUAPI_JPEG_H
#define IMXVPUAPI_JPEG_H

#include "imxvpuapi.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct
{
	/* Width and height of VPU framebuffers are aligned to internal boundaries.
	 * The frame consists of the actual image pixels and extra padding pixels.
	 * aligned_frame_width / aligned_frame_height specify the full width/height
	 * including the padding pixels, and actual_frame_width / actual_frame_height
	 * specify the width/height without padding pixels. */
	unsigned int aligned_frame_width, aligned_frame_height;
	unsigned int actual_frame_width, actual_frame_height;

	/* Stride and size of the Y, Cr, and Cb planes. The Cr and Cb planes always
	 * have the same stride and size. */
	unsigned int y_stride, cbcr_stride;
	unsigned int y_size, cbcr_size;

	/* Offset from the start of a framebuffer's memory, in bytes. Note that the
	 * Cb and Cr offset values are *not* the same, unlike the stride and size ones. */
	unsigned int y_offset, cb_offset, cr_offset;

	/* Framebuffer containing the pixels of the decoded frame. */
	ImxVpuFramebuffer *framebuffer;

	/* Color format of the decoded frame. */
	ImxVpuColorFormat color_format;
}
ImxVpuJPEGDecInfo;


typedef struct _ImxVpuJPEGDecoder ImxVpuJPEGDecoder;

/* Opens a new VPU JPEG decoder instance.
 *
 * Internally, this function calls imx_vpu_dec_load().
 *
 * If dma_buffer_allocator is NULL, the default decoder allocator is used.
 *
 * num_extra_framebuffers is used for instructing this function to allocate this many
 * more framebuffers. Usually this value is zero, but in certain cases where many
 * JPEGs need to be decoded quickly, or the DMA buffers of decoded frames need to
 * be kept around elsewhere, having more framebuffers available can be helpful.
 * Note though that more framebuffers also means more DMA memory consumption.
 * If unsure, keep this to zero. */
ImxVpuDecReturnCodes imx_vpu_jpeg_dec_open(ImxVpuJPEGDecoder **jpeg_decoder, ImxVpuDMABufferAllocator *dma_buffer_allocator, unsigned int num_extra_framebuffers);

/* Closes a JPEG decoder instance. Trying to close the same instance multiple times results in undefined behavior. */
ImxVpuDecReturnCodes imx_vpu_jpeg_dec_close(ImxVpuJPEGDecoder *jpeg_decoder);

/* Determines if the VPU can decode a frame at this moment.
 *
 * The return value depends on how many framebuffers in the decoder are free.
 * If enough framebuffers are free, this returns 1, otherwise 0.
 *
 * For simple decoding schemes where one frame is decoded, then displayed or
 * consumed in any other way, and then returned to the decoder by calling
 * imx_vpu_jpeg_dec_frame_finished(), this function does not have to be used,
 * since in this case, there will always be enough free framebuffers.
 * If however the consumption of the decoded frame occurs in a different thread
 * than the decoding, it makes sense to use this function in order to wait
 * until enough framebfufers are free (typically implemented by using mutexes
 * and thread condition variables). Also, in this case, this function is more
 * likely to return 1 the more extra framebuffers were requested in the
 * imx_vpu_jpeg_dec_open() call.
 */
int imx_vpu_jpeg_dec_can_decode(ImxVpuJPEGDecoder *jpeg_decoder);

/* Decodes a JPEG frame.
 *
 * jpeg_data must be set to the memory block that contains the encoded JPEG data,
 * and jpeg_data_size must be set to the size of that block, in bytes. After this
 * call, use the imx_vpu_jpeg_dec_get_info() function to retrieve information about
 * the decoded frame.
 *
 * The VPU decoder only consumes baseline JPEG data. Progressive encoding is not supported. */
ImxVpuDecReturnCodes imx_vpu_jpeg_dec_decode(ImxVpuJPEGDecoder *jpeg_decoder, uint8_t const *jpeg_data, size_t const jpeg_data_size);

/* Retrieves information about the decoded JPEG frame.
 *
 * The ImxVpuJPEGDecInfo's fields will be set to those of the decoded frame. In particular,
 * info's framebuffer pointer will be set to point to the framebuffer containing the
 * decoded frame. Be sure to pass this pointer to imx_vpu_jpeg_dec_frame_finished() once
 * the frame's pixels are no longer needed.
 *
 * Note that the return value of the previous imx_vpu_jpeg_dec_decode() call can be
 * IMX_VPU_DEC_RETURN_CODE_OK even though the framebuffer pointer retrieved here is NULL.
 * This is the case when not enough free framebuffers are present. It is recommended to
 * check the return value of the imx_vpu_jpeg_dec_can_decode() function before calling
 * imx_vpu_jpeg_dec_decode(), unless the decoding sequence is simple (like in the example
 * mentioned in the imx_vpu_jpeg_dec_can_decode() description).
 *
 * This function must not be called before imx_vpu_jpeg_dec_decode() , since otherwise,
 * there is no information available (it is read in the decoding step). */
void imx_vpu_jpeg_dec_get_info(ImxVpuJPEGDecoder *jpeg_decoder, ImxVpuJPEGDecInfo *info);

/* Inform the JPEG decoder that a previously decoded frame is no longer being used.
 *
 * This function must always be called once the user is done with a frame, otherwise
 * the VPU cannot reclaim this ramebuffer, and will eventually run out of internal
 * framebuffers to decode into. */
ImxVpuDecReturnCodes imx_vpu_jpeg_dec_frame_finished(ImxVpuJPEGDecoder *jpeg_decoder, ImxVpuFramebuffer *framebuffer);




typedef struct
{
	/* Frame width and height of the input frame. These are the actual sizes;
	 * they will be aligned internally if necessary. These sizes must not be
	 * zero. */
	unsigned int frame_width, frame_height;
	/* Quality factor for JPEG encoding. 1 = best compression, 100 = best quality.
	 * This is the exact same quality factor as used by libjpeg. */
	unsigned int quality_factor;

	/* Color format of the input frame. */
	ImxVpuColorFormat color_format;

	/* Functions for acquiring and finishing output buffers. See the
	 * typedef documentations in imxvpuapi.h for details about how
	 * these functions should behave. */
	ImxVpuEncAcquireOutputBuffer acquire_output_buffer;
	ImxVpuEncFinishOutputBuffer finish_output_buffer;
	void *output_buffer_context;
}
ImxVpuJPEGEncParams;


typedef struct _ImxVpuJPEGEncoder ImxVpuJPEGEncoder;

/* Opens a new VPU JPEG encoder instance.
 *
 * Internally, this function calls imx_vpu_enc_load().
 *
 * If dma_buffer_allocator is NULL, the default encoder allocator is used.
 */
ImxVpuEncReturnCodes imx_vpu_jpeg_enc_open(ImxVpuJPEGEncoder **jpeg_encoder, ImxVpuDMABufferAllocator *dma_buffer_allocator);

/* Closes a JPEG encoder instance. Trying to close the same instance multiple times results in undefined behavior. */
ImxVpuEncReturnCodes imx_vpu_jpeg_enc_close(ImxVpuJPEGEncoder *jpeg_encoder);

/* Encodes a raw input frame.
 *
 * params must be filled with valid values; frame width and height must not be zero.
 * framebuffer contains the raw input pixels to encode. Its stride and offset values
 * must be valid, and its dma_buffer pointer must point to a DMA buffer that contains
 * the pixel data.
 *
 * During encoding, the encoder will call params->acquire_output_buffer() to acquire
 * an output buffer and put encoded JPEG data into. Once encoding is done, the
 * params->finish_output_buffer() function is called. This is *not* to be confused with
 * a memory deallocation function; it is instead typically used to notify the caller
 * that the encoder won't touch the acquired buffer's contents anymore. It is guaranteed
 * that finish_output_buffer() is called if acquire_output_buffer() was called earlier.
 *
 * If acquired_handle is non-NULL, then the poiner it refers to will be set to the handle
 * produced by acquire_output_buffer(), even if imx_vpu_jpeg_enc_encode() exits with an
 * error (unless said error occurred *before* the acquire_output_buffer() call, in which
 * case *acquired_handle will be set to NULL). If output_buffer_size is non-NULL, the
 * size value it points to will be set to the number of bytes of the encoded JPEG data.
 *
 * The VPU encoder only produces baseline JPEG data. Progressive encoding is not supported. */
ImxVpuEncReturnCodes imx_vpu_jpeg_enc_encode(ImxVpuJPEGEncoder *jpeg_encoder, ImxVpuFramebuffer const *framebuffer, ImxVpuJPEGEncParams const *params, void **acquired_handle, size_t *output_buffer_size);


#ifdef __cplusplus
}
#endif


#endif
