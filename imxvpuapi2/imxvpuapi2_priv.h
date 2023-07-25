#ifndef IMXVPUAPI2_PRIV_H
#define IMXVPUAPI2_PRIV_H

#include <stdint.h>
#include "imxvpuapi2.h"


#ifdef __cplusplus
extern "C" {
#endif


#ifndef TRUE
#define TRUE (1)
#endif


#ifndef FALSE
#define FALSE (0)
#endif


#ifndef BOOL
#define BOOL int
#endif


#define IMX_VPU_API_UNUSED_PARAM(x) ((void)(x))


#define IMX_VPU_API_ALIGN_VAL_TO(LENGTH, ALIGN_SIZE)  ( ((uintptr_t)(((uint8_t*)(LENGTH)) + (ALIGN_SIZE) - 1) / (ALIGN_SIZE)) * (ALIGN_SIZE) )


#define IMX_VPU_API_ERROR_FULL(FILE_, LINE_, FUNCTION_, ...)   do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_ERROR)   { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_ERROR,   FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)
#define IMX_VPU_API_WARNING_FULL(FILE_, LINE_, FUNCTION_, ...) do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_WARNING) { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_WARNING, FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)
#define IMX_VPU_API_INFO_FULL(FILE_, LINE_, FUNCTION_, ...)    do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_INFO)    { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_INFO,    FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)
#define IMX_VPU_API_DEBUG_FULL(FILE_, LINE_, FUNCTION_, ...)   do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_DEBUG)   { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_DEBUG,   FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)
#define IMX_VPU_API_LOG_FULL(FILE_, LINE_, FUNCTION_, ...)     do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_LOG)     { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_LOG,     FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)
#define IMX_VPU_API_TRACE_FULL(FILE_, LINE_, FUNCTION_, ...)   do { if (imx_vpu_api_cur_log_level_threshold >= IMX_VPU_API_LOG_LEVEL_TRACE)   { imx_vpu_api_cur_logging_fn(IMX_VPU_API_LOG_LEVEL_TRACE,   FILE_, LINE_, FUNCTION_, __VA_ARGS__); } } while(0)


#define IMX_VPU_API_ERROR(...)    IMX_VPU_API_ERROR_FULL  (__FILE__, __LINE__, __func__, __VA_ARGS__)
#define IMX_VPU_API_WARNING(...)  IMX_VPU_API_WARNING_FULL(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define IMX_VPU_API_INFO(...)     IMX_VPU_API_INFO_FULL   (__FILE__, __LINE__, __func__, __VA_ARGS__)
#define IMX_VPU_API_DEBUG(...)    IMX_VPU_API_DEBUG_FULL  (__FILE__, __LINE__, __func__, __VA_ARGS__)
#define IMX_VPU_API_LOG(...)      IMX_VPU_API_LOG_FULL    (__FILE__, __LINE__, __func__, __VA_ARGS__)
#define IMX_VPU_API_TRACE(...)    IMX_VPU_API_TRACE_FULL  (__FILE__, __LINE__, __func__, __VA_ARGS__)


extern ImxVpuApiLogLevel imx_vpu_api_cur_log_level_threshold;
extern ImxVpuApiLoggingFunc imx_vpu_api_cur_logging_fn;


#define VP8_SEQUENCE_HEADER_SIZE  32
#define VP8_FRAME_HEADER_SIZE     12

#define WMV3_RCV_SEQUENCE_LAYER_HEADER_SIZE (6 * 4)
#define WMV3_RCV_FRAME_LAYER_HEADER_SIZE    4

#define VC1_NAL_FRAME_LAYER_HEADER_MAX_SIZE   4

#define DIVX3_FRAME_HEADER_SIZE  (4 + 4)

#define WEBP_FRAME_HEADER_SIZE  20

/* JFIF APP0 segment size (16 bytes), including the size (2 bytes) of its marker */
#define JPEG_JFIF_APP0_SEGMENT_SIZE  (16+2)


extern uint8_t const h264_aud[];
extern size_t const h264_aud_size;


extern uint8_t const jpeg_quantization_table_luma[64];
extern uint8_t const jpeg_quantization_table_chroma[64];
extern uint8_t const jpeg_zigzag_pattern[64];
extern uint8_t const jpeg_jfif_app0_segment[JPEG_JFIF_APP0_SEGMENT_SIZE];


#define READ_16BIT_BE(BUF, OFS) \
	( (((uint16_t)((BUF)[(OFS) + 0])) << 8) \
	| (((uint16_t)((BUF)[(OFS) + 1])) << 0) )

#define READ_32BIT_BE(BUF, OFS) \
	( (((uint32_t)((BUF)[(OFS) + 0])) << 24) \
	| (((uint32_t)((BUF)[(OFS) + 1])) << 16) \
	| (((uint32_t)((BUF)[(OFS) + 2])) <<  8) \
	| (((uint32_t)((BUF)[(OFS) + 3])) <<  0) )

#define READ_32BIT_LE(BUF, OFS) \
	( (((uint32_t)((BUF)[(OFS) + 0])) <<  0) \
	| (((uint32_t)((BUF)[(OFS) + 1])) <<  8) \
	| (((uint32_t)((BUF)[(OFS) + 2])) << 16) \
	| (((uint32_t)((BUF)[(OFS) + 3])) << 24) )


#define WRITE_16BIT_LE(BUF, OFS, VALUE) \
	do \
	{ \
		(BUF)[(OFS) + 0] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(OFS) + 1] = ((VALUE) >> 8) & 0xFF; \
	} \
	while (0)


#define WRITE_16BIT_LE_AND_INCR_IDX(BUF, IDX, VALUE) \
	do \
	{ \
		(BUF)[(IDX)++] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 8) & 0xFF; \
	} \
	while (0)


#define WRITE_32BIT_LE(BUF, OFS, VALUE) \
	do \
	{ \
		(BUF)[(OFS) + 0] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(OFS) + 1] = ((VALUE) >> 8) & 0xFF; \
		(BUF)[(OFS) + 2] = ((VALUE) >> 16) & 0xFF; \
		(BUF)[(OFS) + 3] = ((VALUE) >> 24) & 0xFF; \
	} \
	while (0)


#define WRITE_32BIT_LE_AND_INCR_IDX(BUF, IDX, VALUE) \
	do \
	{ \
		(BUF)[(IDX)++] = ((VALUE) >> 0) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 8) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 16) & 0xFF; \
		(BUF)[(IDX)++] = ((VALUE) >> 24) & 0xFF; \
	} \
	while (0)


#define FOURCC_FORMAT "c%c%c%c"

#define FOURCC_PTR_ARGS(FOURCC) \
		  ((char)(((uint8_t const *)(FOURCC))[0])) \
		, ((char)(((uint8_t const *)(FOURCC))[1])) \
		, ((char)(((uint8_t const *)(FOURCC))[2])) \
		, ((char)(((uint8_t const *)(FOURCC))[3]))


void imx_vpu_api_insert_vp8_ivf_sequence_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height);
void imx_vpu_api_insert_vp8_ivf_frame_header(uint8_t *header, size_t main_data_size, uint64_t pts);

void imx_vpu_api_insert_wmv3_sequence_layer_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height, size_t main_data_size, uint8_t const *codec_data);
void imx_vpu_api_insert_wmv3_frame_layer_header(uint8_t *header, size_t main_data_size);

void imx_vpu_api_insert_vc1_frame_layer_header(uint8_t *header, uint8_t *main_data, size_t *actual_header_length);

void imx_vpu_api_insert_divx3_frame_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height);

int imx_vpu_api_parse_jpeg_header(void *jpeg_data, size_t jpeg_data_size, BOOL semi_planar_output, unsigned int *width, unsigned int *height, ImxVpuApiColorFormat *color_format);

ImxVpuApiH264Level imx_vpu_api_estimate_max_h264_level(int width, int height, int bitrate, int fps_num, int fps_denom, ImxVpuApiH264Profile profile);
ImxVpuApiH265Level imx_vpu_api_estimate_max_h265_level(int width, int height, int bitrate, int fps_num, int fps_denom, ImxVpuApiH265Profile profile);


#ifdef __cplusplus
}
#endif


#endif /* IMXVPUAPI2_PRIV_H */
