#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"


/***********************/
/******* LOGGING *******/
/***********************/


static void default_logging_fn(ImxVpuApiLogLevel level, char const *file, int const line, char const *fn, const char *format, ...)
{
	IMX_VPU_API_UNUSED_PARAM(level);
	IMX_VPU_API_UNUSED_PARAM(file);
	IMX_VPU_API_UNUSED_PARAM(line);
	IMX_VPU_API_UNUSED_PARAM(fn);
	IMX_VPU_API_UNUSED_PARAM(format);
}

ImxVpuApiLogLevel imx_vpu_api_cur_log_level_threshold = IMX_VPU_API_LOG_LEVEL_ERROR;
ImxVpuApiLoggingFunc imx_vpu_api_cur_logging_fn = default_logging_fn;

void imx_vpu_api_set_logging_function(ImxVpuApiLoggingFunc logging_fn)
{
	imx_vpu_api_cur_logging_fn = (logging_fn != NULL) ? logging_fn : default_logging_fn;
}

void imx_vpu_api_set_logging_threshold(ImxVpuApiLogLevel threshold)
{
	imx_vpu_api_cur_log_level_threshold = threshold;
}




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


char const *imx_vpu_api_frame_type_string(ImxVpuApiFrameType frame_type)
{
	switch (frame_type)
	{
		case IMX_VPU_API_FRAME_TYPE_I:    return "I";
		case IMX_VPU_API_FRAME_TYPE_P:    return "P";
		case IMX_VPU_API_FRAME_TYPE_B:    return "B";
		case IMX_VPU_API_FRAME_TYPE_IDR:  return "IDR";
		case IMX_VPU_API_FRAME_TYPE_BI:   return "BI";
		case IMX_VPU_API_FRAME_TYPE_SKIP: return "SKIP";
		case IMX_VPU_API_FRAME_TYPE_UNKNOWN:
		default: return "<unknown>";
	}
}


char const *imx_vpu_api_interlacing_mode_string(ImxVpuApiInterlacingMode mode)
{
	switch (mode)
	{
		case IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING:     return "no interlacing";
		case IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_FIRST:    return "top field first";
		case IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST: return "bottom field first";
		case IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_ONLY:     return "top field only";
		case IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_ONLY:  return "bottom field only";
		case IMX_VPU_API_FRAME_TYPE_UNKNOWN:
		default: return "<unknown>";
	}
}


char const *imx_vpu_api_compression_format_string(ImxVpuApiCompressionFormat format)
{
	switch (format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:           return "JPEG";
		case IMX_VPU_API_COMPRESSION_FORMAT_WEBP:           return "WebP";
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG2:          return "MPEG-2 part 2";
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:          return "MPEG-4 part 2";
		case IMX_VPU_API_COMPRESSION_FORMAT_H263:           return "h.263";
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:           return "h.264 / AVC";
		case IMX_VPU_API_COMPRESSION_FORMAT_H265:           return "h.265 / HEVC";
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:           return "WMV3 / Windows Media Video 9";
		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:           return "VC-1 / Windows Media Video 9 Advanced Profile";
		case IMX_VPU_API_COMPRESSION_FORMAT_VP6:            return "VP6";
		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:            return "VP8";
		case IMX_VPU_API_COMPRESSION_FORMAT_VP9:            return "VP9";
		case IMX_VPU_API_COMPRESSION_FORMAT_AVS:            return "AVS";
		case IMX_VPU_API_COMPRESSION_FORMAT_RV30:           return "RealVideo 8 (RV30)";
		case IMX_VPU_API_COMPRESSION_FORMAT_RV40:           return "RealVideo 9 & 10 (RV40)";
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX3:          return "DivX 3";
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX4:          return "DivX 4";
		case IMX_VPU_API_COMPRESSION_FORMAT_DIVX5:          return "DivX 5";
		case IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK: return "Sorenson Spark";
		default: return "<unknown>";
	}
}


char const *imx_vpu_api_color_format_string(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:             return "fully planar YUV 4:2:0 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_10BIT:            return "fully planar YUV 4:2:0 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:              return "semi planar YUV 4:2:0 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT:             return "semi planar YUV 4:2:0 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_8BIT:             return "fully planar YUV 4:1:1 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_10BIT:            return "fully planar YUV 4:1:1 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_8BIT:              return "semi planar YUV 4:1:1 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_10BIT:             return "semi planar YUV 4:1:1 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:  return "fully planar YUV 4:2:2 horizontal 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_10BIT: return "fully planar YUV 4:2:2 horizontal 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:   return "semi planar YUV 4:2:2 horizontal 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT:  return "semi planar YUV 4:2:2 horizontal 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT:    return "fully planar YUV 2:2:4 vertical 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_10BIT:   return "fully planar YUV 2:2:4 vertical 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:     return "semi planar YUV 2:2:4 vertical 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT:    return "semi planar YUV 2:2:4 vertical 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:             return "fully planar YUV 4:4:4 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_10BIT:            return "fully planar YUV 4:4:4 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:              return "semi planar YUV 4:4:4 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT:             return "semi planar YUV 4:4:4 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT:               return "semi planar YUV 4:2:0 Microsoft P010 10-bit";
		case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT:                          return "YUV 4:0:0 (8-bit grayscale)";
		case IMX_VPU_API_COLOR_FORMAT_YUV400_10BIT:                         return "YUV 4:0:0 (10-bit grayscale)";

		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT:     return "VeriSilicon Hantro G2 semi planar 4x4 tiled YUV 4:2:0 8-bit";
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT:    return "VeriSilicon Hantro G2 semi planar 4x4 tiled YUV 4:2:0 10-bit";
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT:     return "VeriSilicon Hantro G1 semi planar 8x4 tiled YUV 4:2:0 8-bit";
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT:    return "VeriSilicon Hantro G1 semi planar 8x4 tiled YUV 4:2:0 10-bit";

		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_8BIT:  return "Amphion semi planar 8x128 tiled YUV 4:2:0 8-bit";
		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_10BIT: return "Amphion semi planar 8x128 tiled YUV 4:2:0 10-bit";

		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_UYVY_8BIT:              return "packed YUV 4:2:2 U0-Y0-V0-Y1 8-bit";
		case IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_YUYV_8BIT:              return "packed YUV 4:2:2 Y0-U0-Y1-V0 8-bit";

		case IMX_VPU_API_COLOR_FORMAT_RGB565:   return "RGB 5:6:5 (16 bits per pixel)";
		case IMX_VPU_API_COLOR_FORMAT_BGR565:   return "BGR 5:6:5 (16 bits per pixel)";
		case IMX_VPU_API_COLOR_FORMAT_ARGB1555: return "ARGB 1:5:5:5 (15 bits per pixel, 1 MSB padding)";
		case IMX_VPU_API_COLOR_FORMAT_RGBA8888: return "RGBA 8:8:8:8 (32 bits per pixel)";
		case IMX_VPU_API_COLOR_FORMAT_BGRA8888: return "BGRA 8:8:8:8 (32 bits per pixel)";

		default: return "<unknown>";
	}
}


int imx_vpu_api_is_color_format_semi_planar(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT:
			return 1;

		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT:
			return 1;

		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_8BIT:
		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_10BIT:
			return 1;

		default:
			break;
	}

	return 0;
}


int imx_vpu_api_is_color_format_rgb(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_RGB565:
		case IMX_VPU_API_COLOR_FORMAT_BGR565:
		case IMX_VPU_API_COLOR_FORMAT_RGB444:
		case IMX_VPU_API_COLOR_FORMAT_ARGB4444:
		case IMX_VPU_API_COLOR_FORMAT_ARGB1555:
		case IMX_VPU_API_COLOR_FORMAT_RGBA8888:
		case IMX_VPU_API_COLOR_FORMAT_BGRA8888:
			return 1;

		default:
			break;
	}

	return 0;
}


int imx_vpu_api_is_color_format_10bit(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT:
		case IMX_VPU_API_COLOR_FORMAT_YUV400_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT:
		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_10BIT:
			return 1;

		default:
			break;
	}

	return 0;
}


int imx_vpu_api_is_color_format_tiled(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT:
		case IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT:
		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_8BIT:
		case IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_10BIT:
			return 1;

		default:
			return 0;
	}
}


char const * imx_vpu_api_h264_profile_string(ImxVpuApiH264Profile profile)
{
	switch (profile)
	{
		case IMX_VPU_API_H264_PROFILE_CONSTRAINED_BASELINE: return "constrained baseline";
		case IMX_VPU_API_H264_PROFILE_BASELINE: return "baseline";
		case IMX_VPU_API_H264_PROFILE_MAIN: return "main";
		case IMX_VPU_API_H264_PROFILE_HIGH: return "high";
		case IMX_VPU_API_H264_PROFILE_HIGH10: return "high10";
		default: return "<unknown>";
	}
}


char const * imx_vpu_api_h264_level_string(ImxVpuApiH264Level level)
{
	switch (level)
	{
		case IMX_VPU_API_H264_LEVEL_UNDEFINED: return "<undefined>";
		case IMX_VPU_API_H264_LEVEL_1:   return "1";
		case IMX_VPU_API_H264_LEVEL_1B:  return "1b";
		case IMX_VPU_API_H264_LEVEL_1_1: return "1.1";
		case IMX_VPU_API_H264_LEVEL_1_2: return "1.2";
		case IMX_VPU_API_H264_LEVEL_1_3: return "1.3";
		case IMX_VPU_API_H264_LEVEL_2:   return "2";
		case IMX_VPU_API_H264_LEVEL_2_1: return "2.1";
		case IMX_VPU_API_H264_LEVEL_2_2: return "2.2";
		case IMX_VPU_API_H264_LEVEL_3:   return "3";
		case IMX_VPU_API_H264_LEVEL_3_1: return "3.1";
		case IMX_VPU_API_H264_LEVEL_3_2: return "3.2";
		case IMX_VPU_API_H264_LEVEL_4:   return "4";
		case IMX_VPU_API_H264_LEVEL_4_1: return "4.1";
		case IMX_VPU_API_H264_LEVEL_4_2: return "4.2";
		case IMX_VPU_API_H264_LEVEL_5:   return "5";
		case IMX_VPU_API_H264_LEVEL_5_1: return "5.1";
		case IMX_VPU_API_H264_LEVEL_5_2: return "5.2";
		case IMX_VPU_API_H264_LEVEL_6:   return "6";
		case IMX_VPU_API_H264_LEVEL_6_1: return "6.1";
		case IMX_VPU_API_H264_LEVEL_6_2: return "6.2";
		default: return "<unknown>";
	}
}


char const * imx_vpu_api_h265_level_string(ImxVpuApiH265Level level)
{
	switch (level)
	{
		case IMX_VPU_API_H265_LEVEL_UNDEFINED: return "<undefined>";
		case IMX_VPU_API_H265_LEVEL_1:   return "1";
		case IMX_VPU_API_H265_LEVEL_2:   return "2";
		case IMX_VPU_API_H265_LEVEL_2_1: return "2.1";
		case IMX_VPU_API_H265_LEVEL_3:   return "3";
		case IMX_VPU_API_H265_LEVEL_3_1: return "3.1";
		case IMX_VPU_API_H265_LEVEL_4:   return "4";
		case IMX_VPU_API_H265_LEVEL_4_1: return "4.1";
		case IMX_VPU_API_H265_LEVEL_5:   return "5";
		case IMX_VPU_API_H265_LEVEL_5_1: return "5.1";
		case IMX_VPU_API_H265_LEVEL_5_2: return "5.2";
		case IMX_VPU_API_H265_LEVEL_6:   return "6";
		case IMX_VPU_API_H265_LEVEL_6_1: return "6.1";
		case IMX_VPU_API_H265_LEVEL_6_2: return "6.2";
		default: return "<unknown>";
	}
}


int imx_vpu_api_vp8_profile_number(ImxVpuApiVP8Profile profile)
{
	switch (profile)
	{
		case IMX_VPU_API_VP8_PROFILE_0: return 0;
		case IMX_VPU_API_VP8_PROFILE_1: return 1;
		case IMX_VPU_API_VP8_PROFILE_2: return 2;
		case IMX_VPU_API_VP8_PROFILE_3: return 3;
		default: return -1;
	}
}


int imx_vpu_api_vp9_profile_number(ImxVpuApiVP9Profile profile)
{
	switch (profile)
	{
		case IMX_VPU_API_VP9_PROFILE_0: return 0;
		case IMX_VPU_API_VP9_PROFILE_1: return 1;
		case IMX_VPU_API_VP9_PROFILE_2: return 2;
		case IMX_VPU_API_VP9_PROFILE_3: return 3;
		default: return -1;
	}
}




/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


char const * imx_vpu_api_dec_return_code_string(ImxVpuApiDecReturnCodes code)
{
	switch (code)
	{
		case IMX_VPU_API_DEC_RETURN_CODE_OK:                              return "ok";
		case IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS:                  return "invalid parameters";
		case IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR:         return "DMA memory access error";
		case IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT:  return "unsupported compression format";
		case IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA:       return "invalid extra header data";
		case IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE: return "insufficient stream buffer size";
		case IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_BITSTREAM:           return "unsupported bitstream format";
		case IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS:       return "insufficient framebuffers";
		case IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL:                    return "invalid call";
		case IMX_VPU_API_DEC_RETURN_CODE_TIMEOUT:                         return "timeout";
		case IMX_VPU_API_DEC_RETURN_CODE_ERROR:                           return "error";
		default: return "<unknown>";
	}
}


char const * imx_vpu_api_dec_output_code_string(ImxVpuApiDecOutputCodes code)
{
	switch (code)
	{
		case IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:     return "no output yet available";
		case IMX_VPU_API_DEC_OUTPUT_CODE_EOS:                         return "eos";
		case IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE:   return "new stream info available";
		case IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER: return "need additional framebuffer";
		case IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE:     return "decoded frame available";
		case IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:      return "more input data needed";
		case IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED:               return "frame skipped";
		case IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED:    return "video parameters changed";
		default: return "<unknown>";
	}
}


char const * imx_vpu_api_dec_skipped_frame_reason_string(ImxVpuApiDecSkippedFrameReasons reason)
{
	switch (reason)
	{
		case IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_CORRUPTED_FRAME: return "corrupted frame";
		case IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME:  return "internal frame";
		default: return "<unknown>";
	}
}




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


char const * imx_vpu_api_enc_return_code_string(ImxVpuApiEncReturnCodes code)
{
	switch (code)
	{
		case IMX_VPU_API_ENC_RETURN_CODE_OK:                                    return "ok";
		case IMX_VPU_API_ENC_RETURN_CODE_INVALID_PARAMS:                        return "invalid parameters";
		case IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR:               return "DMA memory access error";
		case IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT:        return "unsupported compression format";
		case IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT_PARAMS: return "unsupported compression format parameters";
		case IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COLOR_FORMAT:              return "unsupported color format";
		case IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE:       return "insufficient stream buffer size";
		case IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS:             return "insufficient framebuffers";
		case IMX_VPU_API_ENC_RETURN_CODE_FRAMES_TOO_LARGE:                      return "frames are too large";
		case IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL:                          return "invalid call";
		case IMX_VPU_API_ENC_RETURN_CODE_TIMEOUT:                               return "timeout";
		case IMX_VPU_API_ENC_RETURN_CODE_ERROR:                                 return "error";
		default: return "<unknown>";
	}
}


char const * imx_vpu_api_enc_output_code_string(ImxVpuApiEncOutputCodes code)
{
	switch (code)
	{
		case IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:             return "no output yet available";
		case IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:         return "need additional framebuffer";
		case IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE:             return "encoded frame available";
		case IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:              return "more input data needed";
		case IMX_VPU_API_ENC_OUTPUT_CODE_EOS:                                 return "eos";
		default: return "<unknown>";
	}
}


int imx_vpu_api_vp8_partition_count_number(ImxVpuApiEncVP8PartitionCount partition_count)
{
	switch (partition_count)
	{
		case IMX_VPU_API_ENC_VP8_PARTITION_COUNT_1: return 1;
		case IMX_VPU_API_ENC_VP8_PARTITION_COUNT_2: return 2;
		case IMX_VPU_API_ENC_VP8_PARTITION_COUNT_4: return 4;
		case IMX_VPU_API_ENC_VP8_PARTITION_COUNT_8: return 8;
		default: return -1;
	}
}
