/* Simplified API for JPEG en- and decoding with the NXP i.MX SoC
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


/* This is a convenience interface for simple en- and decoding of JPEG data.
 * For merely en/decoding JPEGs, having to set up a VPU en/decoder involves
 * a considerable amount of boilerplate code. This interface takes care of
 * these details, and presents a much simpler interface focused on this one
 * task: to en/decode JPEGs. */

#ifndef IMXVPUAPI2_JPEG_H
#define IMXVPUAPI2_JPEG_H

#include "imxvpuapi2.h"


#ifdef __cplusplus
extern "C" {
#endif


/*****************************************************/
/******* JPEG DECODER STRUCTURES AND FUNCTIONS *******/
/*****************************************************/


/* Information about the result of a successful JPEG decoding. */
typedef struct
{
	/* Metrics for the decoded frame. */
	ImxVpuApiFramebufferMetrics const *framebuffer_metrics;

	/* DMA buffer containing the pixels of the decoded frame. */
	ImxDmaBuffer *fb_dma_buffer;

	/* Color format of the decoded frame. */
	ImxVpuApiColorFormat color_format;

	/* Total size of a decoded frame, in bytes. This is less than or equal to
	 * the size of fb_dma_buffer. It is recommended to use this value instead
	 * of the fb_dma_buffer size to determine how many bytes make up the frame,
	 * since fb_dma_buffer may have additional internal data appended to the
	 * frame's pixels. */
	size_t total_frame_size;
}
ImxVpuApiJpegDecInfo;


/* Opaque JPEG decoder structure. */
typedef struct _ImxVpuApiJpegDecoder ImxVpuApiJpegDecoder;


/* Opens a new VPU JPEG decoder instance.
 *
 * @param jpeg_decoder Pointer to a ImxVpuApiJpegDecoder pointer that will be
 *        set to point to the new decoder instance. Must not be NULL.
 * @param dma_buffer_allocator DMA buffer allocator to use for internal DMA
 *        buffer allocations. Must not be NULL. The allocator must exist at
 *        least until after the decoder instance was closed.
 * @return Nonzero if the decoder instance was set up successfully.
 *         Zero otherwise.
 */
int imx_vpu_api_jpeg_dec_open(ImxVpuApiJpegDecoder **jpeg_decoder, ImxDmaBufferAllocator *dma_buffer_allocator);

/* Closes a VPU JPEG decoder instance.
 *
 * After an instance was closed, it is gone and cannot be used anymore. Trying to
 * close the same instance multiple times results in undefined behavior.
 *
 * @param jpeg_decoder JPEG decoder instance. Must not be NULL.
 */
void imx_vpu_api_jpeg_dec_close(ImxVpuApiJpegDecoder *jpeg_decoder);

/* Decodes JPEG data.
 *
 * Information about the decoded image is returned as a pointer to an internal
 * structure. This structure is read-only; do not attempt to modify its contents,
 * or to free() the returned pointer. The pointer is no longer to be used after
 * either the decoder was closed or another frame was decoded.
 *
 * If decoding failed, the decoder is not to be used anymore, and must be closed.
 *
 * @param jpeg_decoder JPEG decoder instance. Must not be NULL.
 * @param jpeg_data The data to decode. Must not be NULL.
 * @param jpeg_data_size Size of the data to decode. Must be nonzero.
 * @return Pointer to an ImxVpuApiJpegDecInfo struct containing information
 *         about the decoded frame, or NULL if decoding failed.
 */
ImxVpuApiJpegDecInfo const * imx_vpu_api_jpeg_dec_decode(ImxVpuApiJpegDecoder *jpeg_decoder, uint8_t const *jpeg_data, size_t const jpeg_data_size);




/*****************************************************/
/******* JPEG ENCODER STRUCTURES AND FUNCTIONS *******/
/*****************************************************/


typedef struct
{
	/* Width and height of the input frame. These are the actual sizes;
	 * they will be aligned internally if necessary. These sizes must
	 * not be zero. */
	size_t frame_width, frame_height;

	/* Color format of the input frame. */
	ImxVpuApiColorFormat color_format;

	/* Quality factor for JPEG encoding. 1 = best compression, 100 = best quality.
	 * This is the exact same quality factor as used by libjpeg. */
	unsigned int quality_factor;
}
ImxVpuApiJpegEncParams;


/* Opaque JPEG encoder structure. */
typedef struct _ImxVpuApiJpegEncoder ImxVpuApiJpegEncoder;


/* Opens a new VPU JPEG encoder instance.
 *
 * @param jpeg_encoder Pointer to a ImxVpuApiJpegEncoder pointer that will be
 *        set to point to the new encoder instance. Must not be NULL.
 * @param dma_buffer_allocator DMA buffer allocator to use for internal DMA
 *        buffer allocations. Must not be NULL. The allocator must exist at
 *        least until after the encoder instance was closed.
 * @return Nonzero if the encoder instance was set up successfully.
 *         Zero otherwise.
 */
int imx_vpu_api_jpeg_enc_open(ImxVpuApiJpegEncoder **jpeg_encoder, ImxDmaBufferAllocator *dma_buffer_allocator);

/* Closes a VPU JPEG encoder instance.
 *
 * After an instance was closed, it is gone and cannot be used anymore. Trying to
 * close the same instance multiple times results in undefined behavior.
 *
 * @param jpeg_decoder JPEG encoder instance. Must not be NULL.
 */
void imx_vpu_api_jpeg_enc_close(ImxVpuApiJpegEncoder *jpeg_encoder);

/* Sets the encoding parameters.
 *
 * This needs to be called at least once (right after opening a JPEG encoder
 * instance). It also needs to be called whenever at least one of the parameters
 * change.
 *
 * Internally, this reopens the encoder. If this fails, the return value is zero.
 *
 * @param jpeg_encoder JPEG encoder instance. Must not be NULL.
 * @param params Pointer to a structure with encoding parameters.
 *        Must not be NULL.
 * @return Nonzero if the call was successful, zero otherwise.
 *         If it is zero, then the encoder instance cannot be used anymore,
 *         and must be closed.
 */
int imx_vpu_api_jpeg_enc_set_params(ImxVpuApiJpegEncoder *jpeg_encoder, ImxVpuApiJpegEncParams const *params);

/* Retrieves the required metrics for input framebuffers.
 *
 * The framebuffers that hold the frames to be encoded must be structured according
 * to these metrics, since the encoder expects plane offsets etc. to be just as
 * specified by the metrics.
 *
 * @param jpeg_encoder JPEG encoder instance. Must not be NULL.
 * @return Framebuffer metrics for the frames to be encoded.
 */
ImxVpuApiFramebufferMetrics const * imx_vpu_api_jpeg_enc_get_framebuffer_metrics(ImxVpuApiJpegEncoder *jpeg_encoder);

/* Encodes a frame.
 *
 * Prior to this call, imx_vpu_api_jpeg_enc_set_params() must be called to set the
 * encoding parameters. It is also recommended to get the framebuffer metrics for
 * the input frames by calling imx_vpu_api_jpeg_enc_get_framebuffer_metrics().
 *
 * This function does not immediately return the encoded frame. Instead, it returns
 * the size of the encoded frame data in bytes. This gives the caller the chance to
 * prepare a suitably sized buffer where the encoded data is written to by calling
 * imx_vpu_api_jpeg_enc_get_encoded_data() afterwards.
 *
 * This must not be called again until imx_vpu_api_jpeg_enc_get_encoded_data() was
 * called to retrieve the previously encoded frame. In other words, after successfully
 * encoding a frame, imx_vpu_api_jpeg_enc_get_encoded_data() must be called before
 * another frame can be encoded.
 *
 * While imx_vpu_api_jpeg_enc_get_encoded_data() must be called at least once
 * before calling this, it only needs to be called again if the parameters change,
 * that is, it does not need to be called before every frame encoding.
 *
 * @param jpeg_encoder JPEG encoder instance. Must not be NULL.
 * @param frame_dma_buffer DMA buffer of the framebuffer with the frame to be
 *        encoded. Must not be NULL.
 * @param encoded_data_size Pointer to a variable to write the encoded data size
 *        in bytes to. Must not be NULL.
 * @return Nonzero if the call was successful, zero otherwise.
 *         If it is zero, then the encoder instance cannot be used anymore,
 *         and must be closed.
 */
int imx_vpu_api_jpeg_enc_encode(ImxVpuApiJpegEncoder *jpeg_encoder, ImxDmaBuffer *frame_dma_buffer, size_t *encoded_data_size);

/* Retrieves the encoded data.
 *
 * This must not be called until a frame was encoded by imx_vpu_api_jpeg_enc_encode().
 * Once a frame was encoded, this function can be called to write the encoded data
 * to a system memory buffer pointed to by encoded_data_dest. Said buffer must be at
 * least as large as the value of encoded_data_size that imx_vpu_api_jpeg_enc_encode()
 * outputs (in bytes) to avoid buffer overflows.
 *
 * Once this was called, it must not be called again until another frame was encoded.
 *
 * @param jpeg_encoder JPEG encoder instance. Must not be NULL.
 * @param encoded_data_dest Pointer to the memory buffer the encoded data shall be
 *        written to. Must not be NULL.
 * @return Nonzero if the call was successful, zero otherwise.
 *         If it is zero, then the encoder instance cannot be used anymore,
 *         and must be closed.
 */
int imx_vpu_api_jpeg_enc_get_encoded_data(ImxVpuApiJpegEncoder *jpeg_encoder, void *encoded_data_dest);


#ifdef __cplusplus
}
#endif


#endif
