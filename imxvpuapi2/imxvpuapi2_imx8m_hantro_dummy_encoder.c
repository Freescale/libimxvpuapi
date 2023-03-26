#include <config.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"


static ImxVpuApiEncGlobalInfo const enc_global_info = {
	.flags = 0,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_HANTRO,
	.min_required_stream_buffer_size = 0,
	.required_stream_buffer_physaddr_alignment = 0,
	.required_stream_buffer_size_alignment = 0,
	.supported_compression_formats = NULL,
	.num_supported_compression_formats = 0,
};


ImxVpuApiEncGlobalInfo const * imx_vpu_api_enc_get_global_info(void)
{
	return &enc_global_info;
}


ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_enc_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	IMX_VPU_API_UNUSED_PARAM(compression_format);
	return NULL;
}


void imx_vpu_api_enc_set_default_open_params(ImxVpuApiCompressionFormat compression_format, ImxVpuApiColorFormat color_format, size_t frame_width, size_t frame_height, ImxVpuApiEncOpenParams *open_params)
{
	IMX_VPU_API_UNUSED_PARAM(compression_format);
	IMX_VPU_API_UNUSED_PARAM(color_format);
	IMX_VPU_API_UNUSED_PARAM(frame_width);
	IMX_VPU_API_UNUSED_PARAM(frame_height);
	IMX_VPU_API_UNUSED_PARAM(open_params);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_open(ImxVpuApiEncoder **encoder, ImxVpuApiEncOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(open_params);
	IMX_VPU_API_UNUSED_PARAM(stream_buffer);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


void imx_vpu_api_enc_close(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
}


ImxVpuApiEncStreamInfo const * imx_vpu_api_enc_get_stream_info(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	return NULL;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_add_framebuffers_to_pool(ImxVpuApiEncoder *encoder, ImxDmaBuffer **fb_dma_buffers, size_t num_framebuffers)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffers);
	IMX_VPU_API_UNUSED_PARAM(num_framebuffers);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


void imx_vpu_api_enc_enable_drain_mode(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
}


int imx_vpu_api_enc_is_drain_mode_enabled(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	return 0;
}


void imx_vpu_api_enc_flush(ImxVpuApiEncoder *encoder)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_bitrate(ImxVpuApiEncoder *encoder, unsigned int bitrate)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(bitrate);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_quantization(ImxVpuApiEncoder *encoder, unsigned int quantization)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(quantization);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_push_raw_frame(ImxVpuApiEncoder *encoder, ImxVpuApiRawFrame const *raw_frame)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(raw_frame);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_encode(ImxVpuApiEncoder *encoder, size_t *encoded_frame_size, ImxVpuApiEncOutputCodes *output_code)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(encoded_frame_size);
	IMX_VPU_API_UNUSED_PARAM(output_code);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(encoded_frame);
	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}
