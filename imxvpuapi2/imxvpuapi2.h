#ifndef IMXVPUAPI2_H
#define IMXVPUAPI2_H

#include <stddef.h>
#include <stdint.h>
#include <imxdmabuffer/imxdmabuffer.h>


#ifdef __cplusplus
extern "C" {
#endif


/***********************/
/******* LOGGING *******/
/***********************/


/* Log levels. */
typedef enum
{
	IMX_VPU_API_LOG_LEVEL_ERROR = 0,
	IMX_VPU_API_LOG_LEVEL_WARNING = 1,
	IMX_VPU_API_LOG_LEVEL_INFO = 2,
	IMX_VPU_API_LOG_LEVEL_DEBUG = 3,
	IMX_VPU_API_LOG_LEVEL_LOG = 4,
	IMX_VPU_API_LOG_LEVEL_TRACE = 5
}
ImxVpuApiLogLevel;

/* Function pointer type for logging functions.
 *
 * This function is invoked by IMX_VPU_API_LOG() macro calls. This macro also
 * passes the name of the source file, the line in that file, and the function
 * name where the logging occurs to the logging function (over the file, line,
 * and fn arguments, respectively). Together with the log level, custom logging
 * functions can output this metadata, or use it for log filtering etc.*/
typedef void (*ImxVpuApiLoggingFunc)(ImxVpuApiLogLevel level, char const *file, int const line, char const *fn, const char *format, ...);

/* Defines the threshold for logging. Logs with lower priority are discarded.
 * By default, the threshold is set to IMX_VPU_API_LOG_LEVEL_INFO. */
void imx_vpu_api_set_logging_threshold(ImxVpuApiLogLevel threshold);

/* Defines a custom logging function.
 * If logging_fn is NULL, logging is disabled. This is the default value. */
void imx_vpu_api_set_logging_function(ImxVpuApiLoggingFunc logging_fn);




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


/* Size of reserved area in structs. This exists to be able to
 * safely extend structs without breaking ABI compatibility. */
#define IMX_VPU_API_RESERVED_SIZE 64


/* Utility macro for creating FourCCs. Mainly used for the hardware_type
 * field from the ImxVpuApiDecGlobalInfo struct. */
#define IMX_VPU_API_MAKE_FOURCC_UINT32(a,b,c,d) ( \
		  (((uint32_t)(a)) << 0) \
		| (((uint32_t)(b)) << 8) \
		| (((uint32_t)(c)) << 16) \
		| (((uint32_t)(d)) << 24) \
		)


/* FourCC identifying the hardware as a Hantro codec. */
#define IMX_VPU_API_HARDWARE_TYPE_HANTRO  IMX_VPU_API_MAKE_FOURCC_UINT32('H','T','R','O')
#define IMX_VPU_API_HARDWARE_TYPE_CODA960 IMX_VPU_API_MAKE_FOURCC_UINT32('C','9','6','0')


/* Possible frame types. */
typedef enum
{
	/* Unknown frame type. */
	IMX_VPU_API_FRAME_TYPE_UNKNOWN = 0,
	/* Frame is an I (= intra) frame. These can be used as keyframes / sync points. */
	IMX_VPU_API_FRAME_TYPE_I,
	/* Frame is a P (= predicted) frame. */
	IMX_VPU_API_FRAME_TYPE_P,
	/* Frame is a B (= bidirectionally predicted) frame. */
	IMX_VPU_API_FRAME_TYPE_B,
	/* Frame is an IDR frame. These can be used as key frames / sync points. */
	IMX_VPU_API_FRAME_TYPE_IDR,
	/* Frame is a B frame (see above), but all of its macroblocks are intra coded.
	 * Cannot be used as a keyframe / sync point. */
	IMX_VPU_API_FRAME_TYPE_BI,
	/* Frame was skipped, meaning the result is identical to the previous frame.
	 * This is different to IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED in that a
	 * decoded frame was produced and is available, but the decoder internally
	 * skipped the decoded frame and just returned the same pixels from the
	 * previous frame instead. */
	IMX_VPU_API_FRAME_TYPE_SKIP
}
ImxVpuApiFrameType;

/* Returns a human-readable description of the frame type. Useful for logging. */
char const *imx_vpu_api_frame_type_string(ImxVpuApiFrameType frame_type);


/* Valid interlacing modes. When interlacing is used, each frame is made of one
 * or two interlaced fields (in almost all cases, it's two fields). Rows with
 * odd Y coordinates belong to the top field, rows with even Y coordinates to
 * the bottom.
 *
 * Some video sources send the top field first, some send the bottom first,
 * some send only the top or bottom fields. If both fields got transmitted, it
 * is important to know which field was transmitted first to establish a correct
 * temporal order. This is because in interlacing, the top and bottom fields do
 * not contain the data from the same frame. If the top field came first, then
 * the top field contains rows from a time t, and the bottom field from a time
 * t+1. For operations like deinterlacing, knowing the right temporal order
 * might be essential. */
typedef enum
{
	/* Unknown interlacing mode.*/
	IMX_VPU_API_INTERLACING_MODE_UNKNOWN = 0,
	/* Frame is progressive. It does not use interlacing. */
	IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING,
	/* Top field (= odd rows) came first. */
	IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_FIRST,
	/* Bottom field (= even rows) came first. */
	IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST,
	/* Only the top field was transmitted (even rows are empty). */
	IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_ONLY,
	/* Only the bottom field was transmitted (odd rows are empty). */
	IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_ONLY
}
ImxVpuApiInterlacingMode;

/* Returns a human-readable description of the interlacing mode. Useful for logging. */
char const *imx_vpu_api_interlacing_mode_string(ImxVpuApiInterlacingMode mode);


/* Compression format to use for en/decoding. */
typedef enum
{
	/* JPEG / motion JPEG. */
	IMX_VPU_API_COMPRESSION_FORMAT_JPEG = 0,
	/* WebP. */
	IMX_VPU_API_COMPRESSION_FORMAT_WEBP,
	/* MPEG-2 part 2. (Forwards compatible with MPEG-1 video.) */
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG2,
	/* MPEG-4 part 2, also known as MPEG-4 Visual. */
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG4,
	/* h.263. */
	IMX_VPU_API_COMPRESSION_FORMAT_H263,
	/* h.264, also known as AVC and MPEG-4 Part 10.
	 * Only byte-stream h.264 streams are supported. */
	IMX_VPU_API_COMPRESSION_FORMAT_H264,
	/* h.265, also known as HEVC and MPEG-H part 2.
	 * Only byte-stream h.265 streams are supported. */
	IMX_VPU_API_COMPRESSION_FORMAT_H265,
	/* WMV3, also known as Windows Media Video 9.
	 * Compatible with VC-1 simple and main profiles. */
	IMX_VPU_API_COMPRESSION_FORMAT_WMV3,
	/* VC-1, also known as Windows Media Video 9 Advanced Profile. */
	IMX_VPU_API_COMPRESSION_FORMAT_WVC1,
	/* VP6. */
	IMX_VPU_API_COMPRESSION_FORMAT_VP6,
	/* VP7. */
	IMX_VPU_API_COMPRESSION_FORMAT_VP7,
	/* VP8. */
	IMX_VPU_API_COMPRESSION_FORMAT_VP8,
	/* VP9. */
	IMX_VPU_API_COMPRESSION_FORMAT_VP9,
	/* AVS (Audio and Video Coding Standard). Chinese standards,
	 * similar to MPEG. */
	IMX_VPU_API_COMPRESSION_FORMAT_AVS,
	/* RealVideo 8 (associated FourCC: RV30). */
	IMX_VPU_API_COMPRESSION_FORMAT_RV30,
	/* RealVideo 9 & 10 (associated FourCC: RV40). */
	IMX_VPU_API_COMPRESSION_FORMAT_RV40,
	/* DivX 3. */
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX3,
	/* DivX 4. */
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX4,
	/* DivX 5 and 6. */
	IMX_VPU_API_COMPRESSION_FORMAT_DIVX5,
	/* Sorenson Spark. */
	IMX_VPU_API_COMPRESSION_FORMAT_SORENSON_SPARK,

	NUM_IMX_VPU_API_COMPRESSION_FORMATS
}
ImxVpuApiCompressionFormat;

/* Returns a human-readable description of the compression format.
 * Useful for logging. */
char const *imx_vpu_api_compression_format_string(ImxVpuApiCompressionFormat format);


/* Color format for raw frames. Semi planar formats have one dedicated plane
 * for the Y (luma) components, and a second plane with U and V (chroma)
 * components interleaved. Fully planar formats have three dedicated planes,
 * one for Y, U, and V each.
 *
 * Whether a frame is fully planar or semi planar depends on the codec, not the
 * compression format. For example, a codec might decode JPEG as a fully planar
 * or semi planar frame.
 *
 * 10-bit formats are fully packed. That is, there are no padding bits in
 * between the components. 4 Y components are packed in 40 bits spread across
 * 5 consecutive bytes:
 *
 *   Y0 0-9 ; Y1 10-19 ; Y2 20-29 ; Y3 30-39
 *   Y0 is spread across bytes #0 and #1; Y1 across #1 and #2; etc.
 *
 * Chroma planes in fully and semi planar 10-bit formats are fully packed too.
 *
 * The single exception to this is the Microsoft P010 format. The P010/P016
 * spec defines 16 bits per component. In case of P010, the upper 10 bits are
 * used, and the lower 6 bits are unused (typically set to zero).
 *
 * Note that the YUV422, YUV411, YUV400, YUV444 formats are only supported by
 * the JPEG format. (VP9 supports YUV422 and YUV444 with some profiles.) */
typedef enum
{
	/* Fully planar YUV 4:2:0, 8-bit. Typically known as I420. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT = 0,
	/* Fully planar YUV 4:2:0, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_10BIT,
	/* Semi planar YUV 4:2:0, 8-bit. Typically known as NV12. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	/* Semi planar YUV 4:2:0, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_10BIT,
	/* Fully planar YUV 4:1:1, 8-bit. Known as Y41B. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_8BIT,
	/* Fully planar YUV 4:1:1, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_10BIT,
	/* Semi planar YUV 4:1:1, 8-bit. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_8BIT,
	/* Semi planar YUV 4:1:1, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV411_10BIT,
	/* Fully planar YUV 4:2:2, 8-bit. Chroma components are shared
	 * horizontally. Known as Y42B. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT,
	/* Fully planar YUV 4:2:2, 10-bit. Chroma components are shared
	 * horizontally. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_10BIT,
	/* Semi planar YUV 4:2:2, 8-bit. Chroma components are shared
	 * horizontally. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT,
	/* Semi planar YUV 4:2:2, 10-bit. Chroma components are shared
	 * horizontally. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_10BIT,
	/* Fully planar YUV 4:2:2, 8-bit. Chroma components are shared
	 * vertically. This one is uncommon, and only found in a few
	 * JPEG files. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT,
	/* Fully planar YUV 4:2:2, 10-bit. Chroma components are shared
	 * vertically. This one is uncommon, and only found in a few
	 * JPEG files. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_10BIT,
	/* Semi planar YUV 4:2:2, 8-bit. Chroma components are shared
	 * vertically. This one is uncommon, and only found in a few
	 * JPEG files. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT,
	/* Semi planar YUV 4:2:2, 10-bit. Chroma components are shared
	 * vertically. This one is uncommon, and only found in a few
	 * JPEG files. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_10BIT,
	/* Fully planar YUV 4:4:4, 8-bit. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT,
	/* Fully planar YUV 4:4:4, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_10BIT,
	/* Semi planar YUV 4:4:4, 8-bit. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT,
	/* Semi planar YUV 4:4:4, 10-bit. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_10BIT,
	/* Semi planar YUV 4:2:0 P010 format, 10-bit, from Microsoft. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_P010_10BIT,
	/* Semi planar YUV 4:0:0, 8-bit. This is a grayscale frame;
	 * no chroma data, just one Y plane. */
	IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT,
	/* Semi planar YUV 4:0:0, 10-bit. This is a grayscale frame;
	 * no chroma data, just one Y plane. */
	IMX_VPU_API_COLOR_FORMAT_YUV400_10BIT,

	/* VeriSilicon Hantro G2 semi-planar 4x4 tiled YUV 4:2:0, 8-bit.
	 * The 4x4 pixel tiles are stored in a row-major layout. */
	IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_8BIT,
	/* VeriSilicon Hantro G2 semi-planar 4x4 tiled YUV 4:2:0, 10-bit.
	 * The 4x4 pixel tiles are stored in a row-major layout. */
	IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_4x4TILED_10BIT,
	/* VeriSilicon Hantro G1 semi-planar 8x4 tiled YUV 4:2:0, 8-bit.
	 * The 8x4 pixel tiles are stored in a row-major layout. */
	IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_8BIT,
	/* VeriSilicon Hantro G1 semi-planar 8x4 tiled YUV 4:2:0, 10-bit.
	 * The 8x4 pixel tiles are stored in a row-major layout. */
	IMX_VPU_API_HANTRO_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x4TILED_10BIT,

	/* Amphion semi-planar 8x128 tiled YUV 4:2:0, 8-bit.
	 * This tile layout uses vertical 8x128 strips. Each strip has
	 * 16 tiles. Each tile has 8x8 pixels. The pixels inside these
	 * tiles are stored in row-major layout. The first tile of the
	 * first strip covers the pixels in the frame (0,0)- (7,7).
	 * The second tile in the first strip, (0,8) - (7,15). The
	 * 16th tile in the first strip, (0,112) - (7,127). Next comes
	 * the first tile of the second strip, (8,0) - (15,7) etc. */
	IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_8BIT,

	/* Amphion semi-planar 8x128 tiled YUV 4:2:0, 8-bit.
	 * This tile layout uses vertical 8x128 strips. Each strip has
	 * 16 tiles. Each tile has 8x8 pixels. The pixels inside these
	 * tiles are stored in row-major layout. The first tile of the
	 * first strip covers the pixels in the frame (0,0)- (7,7).
	 * The second tile in the first strip, (0,8) - (7,15). The
	 * 16th tile in the first strip, (0,112) - (7,127). Next comes
	 * the first tile of the second strip, (8,0) - (15,7) etc. */
	IMX_VPU_API_AMPHION_COLOR_FORMAT_YUV420_SEMI_PLANAR_8x128TILED_10BIT,

	/* Packed YUV 4:2:2, 8-bit. This has a single plane.
	 * Values are stored in U0-Y0-V0-Y1 order (0,1 denoting pixels). */
	IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_UYVY_8BIT,
	/* Packed YUV 4:2:2, 8-bit. This has a single plane.
	 * Values are stored in Y0-U0-Y1-V0 order (0,1 denoting pixels). */
	IMX_VPU_API_COLOR_FORMAT_PACKED_YUV422_YUYV_8BIT,

	/* RGB 5:6:5, 16 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_RGB565,
	/* BGR 5:6:5, 16 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_BGR565,
	/* RGB 4:4:4, 12 bits per pixel. The 4 MSB are unused. */
	IMX_VPU_API_COLOR_FORMAT_RGB444,
	/* ARGB 4:4:4:4, 16 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_ARGB4444,
	/* ARGB 1:5:5:5, 16 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_ARGB1555,
	/* RGBA 8:8:8:8, 32 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_RGBA8888,
	/* BGRA 8:8:8:8, 32 bits per pixel. */
	IMX_VPU_API_COLOR_FORMAT_BGRA8888
}
ImxVpuApiColorFormat;


/* Returns a human-readable description of the color format.
 * Useful for logging. */
char const *imx_vpu_api_color_format_string(ImxVpuApiColorFormat color_format);

/* Returns 1 if format is semi-planar, 0 otherwise.
 *
 * Grayscale formats are not considered semi-planar.
 */
int imx_vpu_api_is_color_format_semi_planar(ImxVpuApiColorFormat color_format);

/* Returns 1 if format is an RGB format, 0 otherwise. */
int imx_vpu_api_is_color_format_rgb(ImxVpuApiColorFormat color_format);

/* Returns 1 if format is a 10-bit format, 0 otherwise. */
int imx_vpu_api_is_color_format_10bit(ImxVpuApiColorFormat color_format);

/* Returns 1 if format is a tiled format, 0 otherwise. */
int imx_vpu_api_is_color_format_tiled(ImxVpuApiColorFormat color_format);


typedef struct
{
	/* Frame width and height, aligned to pixel boundaries (typically 8 or 16).
	 * If the actual width/height values are not aligned, then rows and/or
	 * columns are added to the frame to round up the width/height to make them
	 * align to the pixel boundaries.
	 *
	 * When decoding, the decoder produces frames with such extra padding
	 * rows/columns. The aligned width/height fields here are automatically
	 * filled by the decoder.
	 *
	 * When encoding, the encoder automatically fills the aligned width/height
	 * fields here based on the actual frame width/height. The caller then has
	 * to make sure that the width/height of frames to be encoded is set to the
	 * aligned width/height sizes, and only the upper left subset of the frame
	 * defined by the actual width/height sizes contains the actual frame. */
	size_t aligned_frame_width, aligned_frame_height;

	/* Frame width and height, without any alignment. These are the width and
	 * height of the actual frame, that is, without padding rows/columns. These
	 * values are always smaller or equal than their aligned counterparts above,
	 * never larger. The source of these values are the open_params when encoding,
	 * and the open_params or bitstream metadata when decoding. */
	size_t actual_frame_width, actual_frame_height;

	/* Plane stride sizes, in bytes. The U and V planes always
	 * use the same stride, so they share the same value.
	 * When RGB(A) formats or packed YUV formats are used, the stride of the
	 * single plane is stored in y_stride, and uv_stride is unused. */
	size_t y_stride, uv_stride;

	/* Size of the Y and U/V planes, in bytes.
	 * If the color format of the frame pixels is semi-planar, then uv_size
	 * specifies the size of one plane where both U and V values are stored in
	 * interleaved fashion, otherwise it specifies the size of the separate U
	 * and V planes. In other words, if the video frames are semi planar, then
	 * uv_size will be twice as large compared to when frames are fully planar.
	 * When RGB(A) formats or packed YUV formats are used, the size of the
	 * single plane is stored in y_size, and uv_size is unused. */
	size_t y_size, uv_size;

	/* These define the starting offsets of each plane relative to the start
	 * of the buffer. Specified in bytes. With semi-planar frames, the v_offset
	 * has no meaning, and u_offset is the offset of the combined interleaved
	 * U/V plane.
	 * When RGB(A) formats or packed YUV formats are used, the offset to the
	 * single plane is stored in y_offset, and u_offset and v_offset are unused. */
	size_t y_offset, u_offset, v_offset;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiFramebufferMetrics;


/* Structure with details about encoded frames. When decoding, these are
 * the input structures. When encoding, these are the output structures. */
typedef struct
{
	/* When decoding, data must point to the system memory block which has
	 * encoded frame data that shall be consumed by the VPU. This will then
	 * be used by imx_vpu_api_dec_decode().
	 *
	 * When encoding, the user must set this to point to a buffer that is large
	 * enough to hold the encoded data. The size of that data is given to
	 * the encoded_frame_size output argument of imx_vpu_api_enc_encode(). */
	uint8_t *data;

	/* Size of the encoded data, in bytes.
	 *
	 * When decoding, this is set by the user, and is the size of the encoded
	 * data that is pointed to by data. This will then be used by
	 * imx_vpu_api_dec_decode().
	 *
	 * When encoding, the encoder sets this to the size of the encoded data,
	 * in bytes. This is set by the encoder to the same value as the
	 * encoded_frame_size output argument of imx_vpu_api_enc_encode(). */
	size_t data_size;

	/* Nonzero if header data was prepended to the encoded frame data. */
	int has_header;

	/* Frame type (I, P, B, ..) of the encoded frame. Filled by the encoder.
	 * Unused by the decoder. */
	ImxVpuApiFrameType frame_type;

	/* User-defined pointer. The library does not modify this value.
	 * This pointer and the one from the corresponding raw frame will have
	 * the same value. The library will pass it through. It can be used to
	 * identify which raw frame is associated with this encoded frame
	 * for example. */
	void *context;

	/* User-defined timestamps. These are here for convenience. In many
	 * cases, the context one wants to associate with raw/encoded frames
	 * is a PTS-DTS pair. If only the context pointer were available, users
	 * would have to create a separate data structure containing PTS & DTS
	 * values for each context. Since this use case is very common, these
	 * two fields are added to the frame structure. Just like the context
	 * pointer, this encoded frame and the associated raw frame will have
	 * the same PTS-DTS values. It is also perfectly OK to not use them,
	 * and just use the context pointer instead, or vice versa. */
	uint64_t pts, dts;
}
ImxVpuApiEncodedFrame;


/* Structure with details about raw, uncompressed frames. When decoding, these
 * are the output structures. When encoding, these are the input structures. */
typedef struct
{
	/* When decoding:
	 * If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
	 * flag is set in ImxVpuApiDecGlobalInfo, then this is set by the decoder
	 * to the DMA buffer it decoded into. This is done when calling the
	 * imx_vpu_dec_decode() function. Any prior value is overwritten.
	 *
	 * If that flag is not set, then this is set to the DMA buffer previously set
	 * with imx_vpu_api_dec_set_output_frame_dma_buffer().
	 *
	 * Also see the documentation of that flag for details.
	 *
	 * When encoding, this is set by the user to point to the DMA buffer which
	 * contains the framebuffer to encode. */
	ImxDmaBuffer *fb_dma_buffer;

	/* User-defined framebuffer context pointer. Unused by the encoder.
	 * The decoder sets this value to the context pointer of the framebuffer
	 * this frame was decoded into (that is, the framebuffer that uses the
	 * fb_dma_buffer as DMA buffer). This is the framebuffer context that is
	 * passed to the decoder in imx_vpu_api_dec_add_framebuffers_to_pool().
	 * It is useful if the user wants to associate some context with this
	 * framebuffer. Not to be confused by the context pointer below. */
	void *fb_context;

	/* Frame types (I, P, B, ..). During decoding, this is set by the decoder.
	 * During encoding, if the first type is set to IMX_VPU_API_FRAME_TYPE_I
	 * or IMX_VPU_API_FRAME_TYPE_IDR, then the encoder will be forced to create
	 * an intra frame. (The encoder ignores the second type.)
	 * In case of interlaced content, the first frame type corresponds to the
	 * first field, the second type to the second field. For progressive content,
	 * both types are set to the same value.
	 * Not all decoders return frame types. In such cases, these types are set
	 * to IMX_VPU_API_FRAME_TYPE_UNKNOWN. */
	ImxVpuApiFrameType frame_types[2];

	/* Interlacing mode (top-first, bottom-first..). Unused by the encoder. */
	ImxVpuApiInterlacingMode interlacing_mode;

	/* User-defined context pointer. The library does not modify this value.
	 * This pointer and the one from the corresponding encoded frame will have
	 * the same value. The library will pass then through. It can be used to
	 * identify which raw frame is associated with this encoded frame for
	 * example. Not to be confused with the fb_context pointer. */
	void *context;

	/* User-defined timestamps. These are here for convenience. In many
	 * cases, the context one wants to associate with raw/encoded frames
	 * is a PTS-DTS pair. If only the context pointer were available, users
	 * would have to create a separate data structure containing PTS & DTS
	 * values for each context. Since this use case very is common, these
	 * two fields are added to the frame structure. Just like the context
	 * pointer, this encoded frame and the associated raw frame will have
	 * the same PTS-DTS values. It is also perfectly OK to not use them, and
	 * just use the context pointer instead, or vice versa. */
	uint64_t pts, dts;
}
ImxVpuApiRawFrame;


/* Details about support for a compression format by the codec.
 * These are static values that are defined for individual codecs, like
 * the Hantro codec, or the CODA codec. They describe in greater detail
 * the level of support the codec has for a given compression format.
 * For example, a codec may only support VP8 video with up to 1280x720
 * pixels. Or, it may only support YUV 420 8-bit frames etc.
 *
 * This struct contains the basic information that is common to all codecs.
 * Specialized structures for some individual formats also exist. These
 * inherit this structure.
 *
 * imx_vpu_api_dec_get_compression_format_support_details() is used for
 * querying these compression format support details. Calling this function
 * to query a format that is not in the list of supported codec formats
 * (defined in the ImxVpuApiDecGlobalInfo and ImxVpuApiEncGlobalInfo
 * structures) leads to undefined behavior. */
typedef struct
{
	/* Minimum and maximum width of frames. */
	size_t min_width, max_width;
	/* Minimum and maximum height of frames. */
	size_t min_height, max_height;

	/* Array of supported color formats. When decoding, the format that is
	 * actually used depends on the decoder itself, the input bitstream,
	 * and on flags passed in the open params of imx_vpu_api_dec_open().
	 * When encoding, this specifies what formats the input frames have.*/
	ImxVpuApiColorFormat const *supported_color_formats;
	size_t num_supported_color_formats;

	/* Minimum and maximum quantization, defining the valid range
	 * of quantization values for this format. Note that the maximum
	 * value is commonly smaller than the minimum one. In case of JPEG,
	 * this is the inverse of the JPEG 1..100 quality range. So,
	 * for JPEG, quantization = 100 - jpeg_quality .
	 * Not relevant for decoding. */
	unsigned int min_quantization, max_quantization;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiCompressionFormatSupportDetails;


/* h.264 profiles. Used by the encoder. */
typedef enum
{
	IMX_VPU_API_H264_PROFILE_CONSTRAINED_BASELINE = 0,
	IMX_VPU_API_H264_PROFILE_BASELINE,
	IMX_VPU_API_H264_PROFILE_MAIN,
	IMX_VPU_API_H264_PROFILE_HIGH,
	IMX_VPU_API_H264_PROFILE_HIGH10
}
ImxVpuApiH264Profile;

char const * imx_vpu_api_h264_profile_string(ImxVpuApiH264Profile profile);

/* h.264 levels, as specified in ISO/IEC 14496-10 table A-1. */
typedef enum
{
	IMX_VPU_API_H264_LEVEL_UNDEFINED = 0,
	IMX_VPU_API_H264_LEVEL_1,
	IMX_VPU_API_H264_LEVEL_1B,
	IMX_VPU_API_H264_LEVEL_1_1,
	IMX_VPU_API_H264_LEVEL_1_2,
	IMX_VPU_API_H264_LEVEL_1_3,
	IMX_VPU_API_H264_LEVEL_2,
	IMX_VPU_API_H264_LEVEL_2_1,
	IMX_VPU_API_H264_LEVEL_2_2,
	IMX_VPU_API_H264_LEVEL_3,
	IMX_VPU_API_H264_LEVEL_3_1,
	IMX_VPU_API_H264_LEVEL_3_2,
	IMX_VPU_API_H264_LEVEL_4,
	IMX_VPU_API_H264_LEVEL_4_1,
	IMX_VPU_API_H264_LEVEL_4_2,
	IMX_VPU_API_H264_LEVEL_5,
	IMX_VPU_API_H264_LEVEL_5_1,
	IMX_VPU_API_H264_LEVEL_5_2,
	IMX_VPU_API_H264_LEVEL_6,
	IMX_VPU_API_H264_LEVEL_6_1,
	IMX_VPU_API_H264_LEVEL_6_2
}
ImxVpuApiH264Level;

char const * imx_vpu_api_h264_level_string(ImxVpuApiH264Level level);

/* Flags for further support details. */
typedef enum
{
	/* Decoders: If set, then h.264 access units are supported, otherwise
	 * only streams without access units can be processed.
	 *
	 * Encoders: If set, then h.264 access units are produced if the
	 * enable_access_unit_delimiters field in ImxVpuApiEncH264Params
	 * is nonzero. If zero, or if this flag is not set, then the encoded
	 * h.264 streams will contain no access units regardless of whether
	 * or not enable_access_unit_delimiters is nonzero. */
	IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED = (1 << 0),
	/* If set, then h.264 access unit are required for processing.
	 * Only relevant for decoders. */
	IMX_VPU_API_H264_FLAG_ACCESS_UNITS_REQUIRED = (1 << 1)
}
ImxVpuApiH264Flags;

/* Additional h.264 specific codec support details. */
typedef struct
{
	/* Basic support information. */
	ImxVpuApiCompressionFormatSupportDetails parent;

	/* The maximum levels supported for each profile. If a profile is not
	 * supported, its level is set to IMX_VPU_API_H264_LEVEL_UNDEFINED. */
	ImxVpuApiH264Level max_constrained_baseline_profile_level;
	ImxVpuApiH264Level max_baseline_profile_level;
	ImxVpuApiH264Level max_main_profile_level;
	ImxVpuApiH264Level max_high_profile_level;
	ImxVpuApiH264Level max_high10_profile_level;

	/* Bitwise OR combination of flags from ImxVpuApiH264Flags. */
	uint32_t flags;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiH264SupportDetails;


/* h.265 profiles. Used by the encoder. */
typedef enum
{
	IMX_VPU_API_H265_PROFILE_MAIN = 0,
	IMX_VPU_API_H265_PROFILE_MAIN10
}
ImxVpuApiH265Profile;

char const * imx_vpu_api_h265_profile_string(ImxVpuApiH265Profile profile);

/* h.265 levels, as specified in ITU-T Rec. H.265 table A.6. */
typedef enum
{
	IMX_VPU_API_H265_LEVEL_UNDEFINED = 0,
	IMX_VPU_API_H265_LEVEL_1,
	IMX_VPU_API_H265_LEVEL_2,
	IMX_VPU_API_H265_LEVEL_2_1,
	IMX_VPU_API_H265_LEVEL_3,
	IMX_VPU_API_H265_LEVEL_3_1,
	IMX_VPU_API_H265_LEVEL_4,
	IMX_VPU_API_H265_LEVEL_4_1,
	IMX_VPU_API_H265_LEVEL_5,
	IMX_VPU_API_H265_LEVEL_5_1,
	IMX_VPU_API_H265_LEVEL_5_2,
	IMX_VPU_API_H265_LEVEL_6,
	IMX_VPU_API_H265_LEVEL_6_1,
	IMX_VPU_API_H265_LEVEL_6_2
}
ImxVpuApiH265Level;

char const * imx_vpu_api_h265_level_string(ImxVpuApiH265Level level);

/* h.265 tiers. Used by the encoder. */
typedef enum
{
	IMX_VPU_API_H265_TIER_MAIN = 0,
	IMX_VPU_API_H265_TIER_HIGH
}
ImxVpuApiH265Tier;

char const * imx_vpu_api_h265_tier_string(ImxVpuApiH265Tier tier);

/* Flags for further support details. */
typedef enum
{
	/* If set, then h.265 access unit are supported, otherwise
	 * only streams without access unit can be processed. */
	IMX_VPU_API_H265_FLAG_ACCESS_UNITS_SUPPORTED = (1 << 0),
	/* If set, then h.264 access unit are required for processing. */
	IMX_VPU_API_H265_FLAG_ACCESS_UNITS_REQUIRED = (1 << 1),
	/* Encoder can produce main-tier h.265 data. Unused by the decoder. */
	IMX_VPU_API_H265_FLAG_SUPPORTS_MAIN_TIER = (1 << 2),
	/* Encoder can produce high-tier h.265 data. Unused by the decoder. */
	IMX_VPU_API_H265_FLAG_SUPPORTS_HIGH_TIER = (1 << 3)
}
ImxVpuApiH265Flags;

/* Additional h.265 specific codec support details. */
typedef struct
{
	/* Basic support information. */
	ImxVpuApiCompressionFormatSupportDetails parent;

	/* The maximum levels supported for each profile. If a profile is not
	 * supported, its level is set to IMX_VPU_API_H265_LEVEL_UNDEFINED. */
	ImxVpuApiH265Level max_main_profile_level;
	ImxVpuApiH265Level max_main10_profile_level;

	/* Bitwise OR combination of flags from ImxVpuApiH265Flags. */
	uint32_t flags;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiH265SupportDetails;


/* VP8 profiles as described in the RFC 6386 section 9.1. */
typedef enum
{
	IMX_VPU_API_VP8_PROFILE_0 = 0,
	IMX_VPU_API_VP8_PROFILE_1,
	IMX_VPU_API_VP8_PROFILE_2,
	IMX_VPU_API_VP8_PROFILE_3
}
ImxVpuApiVP8Profile;

/* Returns the profile as an integer. For example,
 * IMX_VPU_API_VP8_PROFILE_2 returns 2.
 * Invalid values return -1. */
int imx_vpu_api_vp8_profile_number(ImxVpuApiVP8Profile profile);

/* Additional VP8 specific codec support details. */
typedef struct
{
	/* Basic support information. */
	ImxVpuApiCompressionFormatSupportDetails parent;

	/* Bitwise OR combination of values from ImxVpuApiVP8Profile that
	 * specify which profiles are supported. To check for profile
	 * support, do this:
	 *
	 * is_supported = supported_profiles & (1 << vp8_profile_enum) */
	uint32_t supported_profiles;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiVP8SupportDetails;


/* VP9 profiles as described in the VP9 specification, section 7.2. */
typedef enum
{
	IMX_VPU_API_VP9_PROFILE_0 = 0,
	IMX_VPU_API_VP9_PROFILE_1,
	IMX_VPU_API_VP9_PROFILE_2,
	IMX_VPU_API_VP9_PROFILE_3
}
ImxVpuApiVP9Profile;

/* Returns the profile as an integer. For example,
 * IMX_VPU_API_VP9_PROFILE_2 returns 2.
 * Invalid values return -1. */
int imx_vpu_api_vp9_profile_number(ImxVpuApiVP9Profile profile);

/* Additional VP9 specific codec support details. */
typedef struct
{
	/* Basic support information. */
	ImxVpuApiCompressionFormatSupportDetails parent;

	/* Bitwise OR combination of values from ImxVpuApiVP9Profile that
	 * specify which profiles are supported. To check for profile
	 * support, do this:
	 *
	 * is_supported = supported_profiles & (1 << vp9_profile_enum) */
	uint32_t supported_profiles;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiVP9SupportDetails;




/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* OVERVIEW ABOUT DECODING
 *
 *
 * INITIALIZING THE DECODER
 *
 * First, a stream buffer must be allocated. This is a DMA buffer that
 * is allocated with libimxdmabuffer's imx_dma_buffer_allocate(). The required
 * size and alignment can be retrieved from the min_required_stream_buffer_size
 * and required_stream_buffer_physaddr_alignment fields that are present in
 * the ImxVpuApiDecGlobalInfo structure. To get the const global instance
 * of that structure, use imx_vpu_api_dec_get_global_info().
 *
 * NOTE: If min_required_stream_buffer_size is 0, then this first step
 * is to be skipped, since this then means that the VPU does not need
 * a stream buffer.
 *
 * Once the stream buffer is allocated, an ImxVpuApiDecOpenParams structure
 * must be initialized. At the very least, its compression_format field
 * must be set to a valid value. Be sure to set any unused field to zero.
 *
 * The decoder can then be created by calling imx_vpu_api_dec_open(). That
 * function needs the stream buffer and ImxVpuApiDecOpenParams structure.
 *
 *
 * MAIN DECODING LOOP
 *
 * The decoding loop goes as follows:
 *
 * 1. Fill an ImxVpuApiEncodedFrame instance with information about the frame
 *    that is to be decoded. If there are no more frames to encode, continue
 *    at step 5.
 * 2. Feed that frame into the VPU by calling imx_vpu_api_dec_push_encoded_frame().
 * 3. If the stream info was already retrieved, and in the global info, the
 *    IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL flag
 *    is not set, set the previously allocated output DMA buffer as the
 *    decoder's output buffer with imx_vpu_api_dec_set_output_frame_dma_buffer().
 * 4. Call imx_vpu_api_dec_decode() and get its output code.
 * 5. - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED,
 *      go back to step 1.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE,
 *      just go back to step 4.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_EOS, go to step 6.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE,
 *      the stream info must be retrieved with imx_vpu_api_dec_get_stream_info().
 *      The stream info contains details about the necessary framebuffers. At this
 *      point, the user must allocate at least as many framebuffers as indicated
 *      by the min_num_required_framebuffers field in ImxVpuApiDecStreamInfo.
 *      (min_num_required_framebuffers can be 0 if the VPU does not use buffer
 *      pool, or performs allocation for its buffer pool internally.)
 *      The details about the framebuffers like its minimum size and alignment are
 *      found in that structure as well. After allocating the buffers, they are
 *      added to the VPU decoder with imx_vpu_api_dec_add_framebuffers_to_pool().
 *      Additionally, an "output buffer" must be allocated at this point if the
 *      IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL in
 *      the ImxVpuApiDecGlobalInfo structure is not set (see step 3 above).
 *      The output buffer's size must be at least min_output_framebuffer_size
 *      from the ImxVpuApiDecStreamInfo structure.
 *      Then go back to step 4.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER,
 *      allocate an extra framebuffer and add it to the VPU decoder's pool,
 *      just like in step 5 above (except add just 1 buffer here, not the amount
 *      added in that step). Then go back to step 4.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE,
 *      retrieve the decoded frame with imx_vpu_api_dec_get_decoded_frame(), and
 *      then go back to step 4.
 *      Once that decoded frame is fully processed by the code and is no longer
 *      needed, and IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 *      is not set in the ImxVpuApiDecGlobalInfo structure, then call
 * 	    imx_vpu_api_dec_return_framebuffer_to_decoder() to return the buffer that
 *      contains the no longer needed frame back to the VPU's pool.
 *    - If the output code is IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED, retrieve
 *      the details about the skipped frame with imx_vpu_api_dec_get_skipped_frame_info(),
 *      then go back to step 4.
 *    - if the output code is IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED,
 *      drain the decoder as explained below, close the current decoder with
 *      imx_vpu_api_dec_close(), deallocate all framebuffers and the output buffer
 *      (if present), open a new decoder with imx_vpu_api_dec_open(), and go back
 *      to step 2, feedin the same data into the encoder that was previously fed
 *      and which produced this output code.
 * 5. Drain the decoder as explained below.
 * 6. Exit the loop.
 *
 *
 * DRAINING THE DECODER
 *
 * NOTE: This refers to the full draining that is done when the stream ends
 * and is done before the decoder is closed. Consult the documentation at
 * imx_vpu_api_dec_enable_drain_mode() for details about this.
 *
 * Draining is done by first calling imx_vpu_api_dec_enable_drain_mode(),
 * followed by repeatedly calling imx_vpu_api_dec_decode() until until one
 * of the terminating cases occur:
 *
 *   1. The IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED output
 *      code is returned
 *   2. The IMX_VPU_API_DEC_OUTPUT_CODE_EOS output code is returned
 *   3. A return code that indicates an error is returned
 *
 * Once EOS has been reported, imx_vpu_api_dec_is_drain_mode_enabled()
 * will return 0 again, since the drain mode is automatically turned off
 * when the end of stream is reached.
 *
 *
 * SHUTTING DOWN THE DECODER
 *
 * The decoder is shut down by calling imx_vpu_api_dec_close(). Once
 * that was called, the framebuffers that were previously added to the
 * VPU decoder's pool are no longer used, and can be deallocated. Note
 * that calling imx_vpu_api_dec_close() will also drop any decoded
 * frame data that may be queued in the decoder. Consider draining
 * the decoder before shutting it down.
 */




/* Opaque decoder structure. */
typedef struct _ImxVpuApiDecoder ImxVpuApiDecoder;


/* Return codes. Only IMX_VPU_API_DEC_RETURN_CODE_OK indicates a success;
 * the other codes all indicate an error. Unless otherwise note, an error
 * implies that the decoder is no longer able to operate correctly and
 * needs to be discarded with imx_vpu_api_dec_close(). */
typedef enum
{
	/* Operation finished successfully. */
	IMX_VPU_API_DEC_RETURN_CODE_OK = 0,
	/* Input parameters were invalid. This is intended as a catch-all for errors
	 * with input parameters that aren't described well by other return codes. */
	IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS,
	/* Accessing DMA memory failed. This happens when trying to memory-map
	 * DMA memory unsuccessfully. */
	IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR,
	/* User tried to call imx_vpu_api_dec_open() with a compression format
	 * set in the open_params that is not supported by the codec. In other
	 * words, the specified format is not included in the ImxVpuApiDecGlobalInfo
	 * supported_compression_formats list. */
	IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT,
	/* Extra header data in the open params passed to imx_vpu_api_dec_open()
	 * is either invalid or missing. */
	IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA,
	/* User tried to pass a stream buffer to imx_vpu_api_enc_open() that is
	 * not large enough. */
	IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE,
	/* The input bitstream is not supported. One example is when the user tries
	 * to decode VP9 data that uses an unsupported profile, or trying to decode
	 * h.264 high10 profile data on a codec that only supports baseline profile
	 * h.264 data etc. */
	IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_BITSTREAM,
	/* imx_vpu_api_dec_add_framebuffers_to_pool() was called with insufficient
	 * number of buffers. When calling imx_vpu_api_dec_add_framebuffers_to_pool()
	 * after the IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE output
	 * code was returned by imx_vpu_api_dec_decode(), the number of buffers
	 * given to imx_vpu_api_dec_add_framebuffers_to_pool() must be at least the
	 * number of required framebuffers as indicated by ImxVpuApiDecStreamInfo's
	 * min_num_required_framebuffers field. */
	IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS,
	/* Functions were not called in the right sequence or at inappropriate times.
	 * For example, trying to call imx_vpu_api_dec_decode() again, without adding
	 * framebuffers to the pool, might trigger this. */
	IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL,
	/* Internal driver or hardware timeout. */
	IMX_VPU_API_DEC_RETURN_CODE_TIMEOUT,
	/* General return code for when an error occurs. This is used as a catch-all
	 * for when the other error return codes do not match the error.
	 * Consult the log output if this is returned. */
	IMX_VPU_API_DEC_RETURN_CODE_ERROR
}
ImxVpuApiDecReturnCodes;

/* Returns a human-readable description of the return code.
 * Useful for logging. */
char const * imx_vpu_api_dec_return_code_string(ImxVpuApiDecReturnCodes code);


/* Decoding output codes. These are returned by imx_vpu_api_dec_decode() only.
 * This function returns an output code only if its return code is
 * IMX_VPU_API_DEC_RETURN_CODE_OK.
 *
 * Output codes describe the result of the decoding operation in greater
 * detail, and are essential for deciding the next necessary decoding step.
 *
 * After each imx_vpu_api_dec_decode() call, in order to continue decoding
 * correctly, imx_vpu_api_dec_decode() has to be called again, in a loop,
 * until one of these output codes are returned:
 *
 *   - IMX_VPU_API_DEC_OUTPUT_CODE_EOS : The loop has to stop, since the
 *     end of stream was reached.
 *   - IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED : Input data has
 *     to be supplied before decoding can continue, by calling
 *     imx_vpu_api_dec_push_encoded_frame(). If there is currently no more
 *     data, the loop has to be stopped and re-entered once more encoded
 *     input frames are available.
 *
 * The reason for this is that there is no guaranteed 1:1 relationship
 * between input and output during decoding. In other words, pushing an
 * encoded frame into the decoder may require more than one step to be
 * undertaken afterwards (like adding framebuffers).
 *
 * The loop also has to be stopped if imx_vpu_api_dec_decode() returns
 * an error, or if imx_vpu_api_dec_flush() was called.
 */
typedef enum
{
	/* Decoding did not (yet) yield output. Additional
	 * imx_vpu_api_dec_decode() calls might. */
	IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE = 0,
	/* Decoder encountered the end of stream. No further decoding is possible.
	 * The only valid API call at this point is imx_vpu_api_dec_close(). */
	IMX_VPU_API_DEC_OUTPUT_CODE_EOS,
	/* Decoder encountered new stream information. This happens in the
	 * beginning. The user has to retrieve the stream information by calling
	 * imx_vpu_api_dec_get_stream_info(), and allocate and add at least as
	 * many framebuffers as indicated by the min_num_required_framebuffers
	 * field in ImxVpuApiDecStreamInfo.
	 *
	 * Decoding cannot continue until the framebuffers were added to the pool
	 * (unless the required amount of framebuffers for the pool is 0). */
	IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE,
	/* The decoder needs one additional framebuffer in its pool for decoding.
	 * The user is supposed to allocate one extra framebuffer and add it by
	 * calling imx_vpu_api_dec_add_framebuffers_to_pool(). Decoding cannot
	 * continue until a framebuffer was added. This output code only occurs
	 * after IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE was
	 * returned some time earlier. */
	IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER,
	/* Decoding yielded a fully decoded raw frame. The user has to call
	 * imx_vpu_api_dec_get_decoded_frame() to retrieve the frame. Until
	 * that function was called, decoding cannot continue. */
	IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE,
	/* No more encoded input data is available in the decoder's stream
	 * buffer. The user is now expected to feed more encoded input data
	 * by calling imx_vpu_api_dec_push_encoded_frame(). Then the decoding
	 * can continue. */
	IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED,
	/* Similar to IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE,
	 * except that a frame was skipped and is not available. The user is
	 * informed about this to adjust timestamps etc. The pts/dts/context
	 * values that would normally be set in ImxVpuApiRawFrame are available
	 * by calling imx_vpu_api_dec_get_skipped_frame_info(). This makes
	 * it possible to know which encoded input frame was skipped.
	 * A frame is skipped when it is not needed or not meant to be shown.
	 * For example, some VP8/VP9 frames may be alt-reference frames or
	 * "golden frames", neither or which are supposed to be shown. */
	IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED,

	/* With some compression formats, the video parameters (width, height etc.)
	 * can change mid-stream. For example, in h.264 and h.265 bitstreams,
	 * additional SPS/PPS NALUs can be present that contain such changes.
	 * If a video parameter change occurs, this output code is emitted. The
	 * user then has to drain the decoder, deallocate any previously allocated
	 * framebuffers that were added to the decoder's framebuffer pool (they
	 * got removed from the pool at this point, so it is safe to discard them),
	 * and reopen the decoder (since the existing decoder's configuration
	 * is fixed, and is incompatible with the newly changed video parameters).
	 * Then, the encoded data that produced this output code must be fed into
	 * the new decoder instance, after which decoding proceeds as if it had
	 * started from the beginning (that is, new stream info is announced,
	 * framebuffers must be added etc.) */
	IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED
}
ImxVpuApiDecOutputCodes;

/* Returns a human-readable description of the output code.
 * Useful for logging. */
char const * imx_vpu_api_dec_output_code_string(ImxVpuApiDecOutputCodes code);


/* Reasons for why a frame was skipped. */
typedef enum
{
	/* Frame was skipped because its data was corrupted somehow and thus
	 * cannot be decoded properly. Followup frames may or may still be
	 * readable, so this does not necessarily indicate a fatal error. */
	IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_CORRUPTED_FRAME = 0,
	/* Frame was skipped because it is internal and only to be decoded,
	 * not shown. One example is a VP8/VP9 alt-ref frame. */
	IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME
}
ImxVpuApiDecSkippedFrameReasons;

/* Returns a human-readable description of the return code.
 * Useful for logging. */
char const * imx_vpu_api_dec_skipped_frame_reason_string(ImxVpuApiDecSkippedFrameReasons reason);


/* Flags for use in ImxVpuApiDecOpenParams. */
typedef enum
{
	/* Allow the decoder to perform frame reordering. This is relevant for
	 * compression formats like h.264 which may have frames that are not
	 * encoded in the same order they are shown. If users do not want to
	 * do the frame reordering by themselves, they should set this flag. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING = (1 << 0),
	/* Produce tiled output frames if possible. These frames have data that
	 * uses a tile-based layout instead of the usual row based one. The
	 * exact structure of the tiles is platform specific. If this is set,
	 * and the codec can produce tiled frames, then the color_format field
	 * in ImxVpuApiDecStreamInfo will be set to a platform/codec specific
	 * tiled format. Consult codec specific libimxvpuapi headers for more. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_TILED_OUTPUT = (1 << 1),
	/* Enable 10-bit decoding. If this is not set, then the decoder will
	 * always produce output frames with 8-bit data, even if the input
	 * bitstream actually contains 10-bit data (the decoder then downscales
	 * the values of the Y/U/V components from 10 to 8 bits). Setting this
	 * flag with a decoder that does not support 10-bit output does nothing. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_10BIT_DECODING = (1 << 2),
	/* Enable MPEG-4 deblocking filter. Setting this makes sense if no
	 * external deblocking filter is applied to MPEG-4 output. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MPEG4_DEBLOCKING = (1 << 3),
	/* Use h.264 MVC for 3D video if the decoder supports it and if the
	 * input bitstream contains 3D data. Decoders that don't support MVC
	 * ignore this flag. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_MVC = (1 << 4),
	/* If this is set, then the U and V chroma components are interleaved in
	 * one shared chroma plane, otherwise they are separated in their own
	 * planes. That is, the format of the decoded frames is a semi planar
	 * one if this is set. Note that decoders may or may not respect this
	 * flag. Some decoders can only produce semi planar, others only fully
	 * planar data etc. So, always check the color_format value in the
	 * ImxVpuApiDecStreamInfo structure.
	 * If IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SUGGESTED_COLOR_FORMAT is
	 * set, the suggested color format will be tried first. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT = (1 << 5),
	/* If this is set, then the decoder initialization may make use of the
	 * suggested_color_format value ImxVpuApiDecOpenParams. That format
	 * is tried first. This takes priority over the other flags
	 * IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT,
	 * IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_TILED_OUTPUT, and
	 * IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_10BIT_DECODING. If the suggested
	 * format can be used by the decoder, these flags are ignored. Otherwise,
	 * if the suggested format is unusable for the decoder, these flags are
	 * processed as usual. */
	IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SUGGESTED_COLOR_FORMAT = (1 << 6),
}
ImxVpuApiDecOpenParamsFlags;

/* Parameters for opening a decoder. */
typedef struct
{
	/* The compression format of the bitstream to decode. */
	ImxVpuApiCompressionFormat compression_format;

	/* Bitwise OR combination of flags from ImxVpuApiDecOpenParamsFlags. */
	uint32_t flags;

	/* These are necessary with some compression formats which do not store
	 * the width and height in the bitstream or in the extra header data.
	 * If the format does store them, these values can be set to zero.
	 * If in doubt, always set these to valid values. Formats that extract
	 * width and height from bitstream/extra header data then just ignore
	 * these two values. These are _unaligned_ width/height sizes, excluding
	 * any existing padding row/columns. */
	size_t frame_width, frame_height;

	/* Out-of-band header data, typically stored separately in container
	 * formats such as Matroska or MP4. If such data is available, set this
	 * pointer to refer to it, and extra_header_data_size to the size of
	 * the data, in bytes.
	 *
	 * Note that this data is not copied internally, so the memory block
	 * extra_header_data points to must exist at least until the decoder
	 * is closed. Otherwise, undefined behavior occurs. */
	uint8_t const *extra_header_data;
	size_t extra_header_data_size;

	/* A color format the decoder should use for its output. Only valid if
	 * the IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SUGGESTED_COLOR_FORMAT flag
	 * is set. Even then, the decoder is allowed to ignored this value. */
	ImxVpuApiColorFormat suggested_color_format;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE - sizeof(ImxVpuApiColorFormat)];
}
ImxVpuApiDecOpenParams;


/* Metadata describing HDR information, like the CIE coordinates for the
 * primaries, the mastering luminance etc.
 * Only h.265 streams can provide this metadata.  */
typedef struct
{
	/* Normalized X/Y chromaticity coordinates of the red/green/blue components
	 * according to CIE 1931. The X coordinates are normalized to the range
	 * 0 .. xy_range[0] , the Y coordinates to the range 0 .. xy_range[1]. That
	 * is, The value of xy_range[0] corresponds to the 1.0 X coordinate, and
	 * xy_range[1] to the 1.0 Y coordinate. */
	uint32_t red_primary_x;
	uint32_t red_primary_y;
	uint32_t green_primary_x;
	uint32_t green_primary_y;
	uint32_t blue_primary_x;
	uint32_t blue_primary_y;
	/* Normalized X/Y chromaticity coordinates of the white point, according to
	 * the CIE 1931 definition. These are normalized just like the red/green/blue
	 * component coordinates above. */
	uint32_t white_point_x;
	uint32_t white_point_y;

	/* Range of the X/Y chromaticity coordinates. These are the coordinate values
	* that correspond to CIE coordinates 1.0. */
	uint32_t xy_range[2];

	/* The nominal minimum and maximum mastering luminance. The units are 0.0001
	* candelas per square meter. */
	uint32_t min_mastering_luminance;
	uint32_t max_mastering_luminance;

	/* If not 0, this indicates an upper bound on the maximum light level among
	 * all individual samples in a 4:4:4 representation of red, green, and blue
	 * colour primary intensities (in the linear light domain) for the frames
	 * of the video sequence, in units of candelas per square metre. */
	uint32_t max_content_light_level;
	/* If not 0, this indicates an upper bound on the maximum average light level
	 * among the samples in a 4:4:4 representation of red, green, and blue colour
	 * primary intensities (in the linear light domain) for any individual frame
	 * of the video sequence, in units of candelas per square metre. If the
	 * frame contains regions that are not visually relevant to the actual video,
	 * such as additional black region for letterboxed videos, then the average
	 * corresponds to the visually relevant region in the frame only. */
	uint32_t max_frame_average_light_level;
}
ImxVpuApiDecHDRMetadata;

/* Color description information.
 * Only h.265 streams can provide this metadata.  */
typedef struct
{
	/* Chromaticity coordinates of the source primaries as specified in Table E.3
	 * of the h.265 spec, in terms of the CIE 1931 definition of x and y as
	 * specified in ISO 11664-1. */
	uint32_t color_primaries;
	/* Reference opto-electronic transfer characteristic function as specified
	 * in the h.265 spec. */
	uint32_t transfer_characteristics;
	/* Matrix coefficients used in deriving luma and chroma signals from the green,
	 * blue, and red, or Y, Z, and X primaries, as specified in Table E.5 of the
	 * h.265 spec. */
	uint32_t matrix_coefficients;
}
ImxVpuApiDecColorDescription;

/* Location of chroma sample information, as specified in the h.265 section E.3.1. */
typedef struct
{
	uint32_t chroma_sample_loc_type_top_field;
	uint32_t chroma_sample_loc_type_bottom_field;
}
ImxVpuApiDecLocationOfChromaInfo;

/* Bitwise-OR combinable flags specifying characteristics about the stream. */
typedef enum
{
	/* If set, this means that one plane is used for both U and
	 * V values, which are interleaved. Otherwise, there are two separate
	 * planes for U and V. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES = (1 << 0),
	/* If set, the decoded frames are interlaced. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_INTERLACED = (1 << 1),
	/* If set, the decoded frames contain HDR 10-bit data. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_10BIT = (1 << 2),
	/* If set, the hdr_metadata structure contains valid information. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_HDR_METADATA_AVAILABLE = (1 << 3),
	/* If set, the color_description structure contains valid information. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_COLOR_DESCRIPTION_AVAILABLE = (1 << 4),
	/* If set, the location_of_chroma_info structure contains valid information. */
	IMX_VPU_API_DEC_STREAM_INFO_FLAG_LOCATION_OF_CHROMA_INFO_AVAILABLE = (1 << 5)
}
ImxVpuApiDecStreamInfoFlags;

/* Information about a new stream. Users can get this by calling
 * imx_vpu_api_dec_get_stream_info(). This structure is necessary for the user
 * to allocate and add new framebuffers when imx_vpu_api_dec_decode() returns
 * the output code IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE. */
typedef struct
{
	/* Minimum required size of DMA buffer memory, in bytes, for framebuffers that
	 * get added to the decoder's framebuffer pool.
	 * It always holds that min_output_framebuffer_size <= min_fb_pool_framebuffer_size . */
	size_t min_fb_pool_framebuffer_size;
	/* Minimum required size of DMA buffer memory, in bytes, for framebuffers that
	 * are set as output buffers via imx_vpu_api_dec_set_output_frame_dma_buffer().
	 * If IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
	 * ImxVpuApiDecGlobalInfo is set, then this value is the same as the one
	 * from min_fb_pool_framebuffer_size.
	 * It always holds that min_output_framebuffer_size <= min_fb_pool_framebuffer_size . */
	size_t min_output_framebuffer_size;

	/* Required physical address alignment for the DMA buffers of framebuffers
	 * that get added to the decoder's framebuffer pool. 0 and 1 mean that the
	 * address does not have to be aligned in any special way. */
	size_t fb_pool_framebuffer_alignment;
	/* Required physical address alignment for the DMA buffers of framebuffers
	 * that are set as output buffers via imx_vpu_api_dec_set_output_frame_dma_buffer().
	 * 0 and 1 mean that the address does not have to be aligned in any special way.
	 * If IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL in
	 * ImxVpuApiDecGlobalInfo is set, then this value is the same as the one
	 * from fb_pool_framebuffer_alignment. */
	size_t output_framebuffer_alignment;

	/* Metrics of decoded framebuffers. These refer to the frames that
	 * are returned by imx_vpu_api_dec_get_decoded_frame(). */
	ImxVpuApiFramebufferMetrics decoded_frame_framebuffer_metrics;

	/* Crop rectangle. This rectangle describes the region within the frame that
	 * contains the pixels of interest. This not only excludes any padding
	 * rows/columns, but some of the actual frame pixels as well. This is used
	 * in some camera recordings for example.
	 * The crop rectangle is never larger than the entire actual frame. If
	 * has_crop_rectangle is 0, the values are always set to
	 * 0/0/actual_frame_width/actual_frame_height (the latter two coming from
	 * the decoded_frame_framebuffer_metrics). */
	int has_crop_rectangle;
	size_t crop_left, crop_top, crop_width, crop_height;

	/* Frame rate ratio. If this value is not available, these fields
	 * are set both to 0. */
	unsigned int frame_rate_numerator, frame_rate_denominator;

	/* The minimum amount of framebuffers that must be added to the decoder's
	 * framebuffer pool in order for decoding to proceed. Adding more than
	 * this amount is okay. Adding less will cause decoding to fail, and the
	 * decoder will have to be closed.
	 * (min_num_required_framebuffers can be 0 if the VPU does not use buffer
	 * pool, or performs allocation for its buffer pool internally.) */
	size_t min_num_required_framebuffers;

	/* Color format of the decoded frames. */
	ImxVpuApiColorFormat color_format;

	/* Additional information about color primaries, white point, dynamic
	 * range, etc. These currently are available for h.264 and h.265 only.
	 * For more details, consult the h.265 specification, Annex D and E. */
	uint32_t video_full_range_flag;
	ImxVpuApiDecHDRMetadata hdr_metadata;
	ImxVpuApiDecColorDescription color_description;
	ImxVpuApiDecLocationOfChromaInfo location_of_chroma_info;

	/* Bitwise OR combination of flags from ImxVpuApiDecStreamInfoFlags. */
	uint32_t flags;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiDecStreamInfo;


/* Bitwise-OR combinable flags specifying global, invariant characteristics
 * about the decoder. */
typedef enum
{
	/* If set, the underlying codec can decode. Some hardware codecs can
	 * only decode or only encode, which is why this flag exists. If this
	 * flag is not set, then all the other global info values as well as
	 * the compression format support details and decoder functions are
	 * not usable. Using them leads to undefined behavior then. */
	IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER = (1 << 0),
	/* If set, the decoder can produce semi planar output frames. At least this
	 * or IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED are
	 * always set.
	 * Also see IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT. */
	IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED = (1 << 1),
	/* If set, the decoder can produce fully planar output frames. At least this
	 * or IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED are
	 * always set.
	 * Also see IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT. */
	IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED = (1 << 2),
	/* If set, then the decoded frames are written into buffers that are part
	 * of the VPU's buffer pool. Such buffers must not be modified, since the
	 * VPU uses them as references for decoding P- and B-frames, so modifying
	 * the contents of these decoded frames would result in corrupted output.
	 *
	 * Also, if this is set, then the buffer the decoded frame was decoded
	 * to must be returned to the pool once the user does not need that frame
	 * anymore, by calling imx_vpu_api_dec_return_framebuffer_to_decoder() .
	 *
	 * If this is zero, then the decoded frame is *not* stored in a buffer that
	 * is part of the VPU's buffer pool. Instead, the user must set an output
	 * frame DMA buffer by calling imx_vpu_api_dec_set_output_frame_dma_buffer().
	 * This output frame will contain the decoded frame and can be modified in
	 * any way the user wants, since is it not part of the VPU's buffer pool.
	 * The output frame must be set before imx_vpu_api_dec_decode() is called.
	 * Also, calling imx_vpu_api_dec_return_framebuffer_to_decoder() to return
	 * the buffer is not needed, since there is nothing to return. In fact,
	 * imx_vpu_api_dec_return_framebuffer_to_decoder() is a no-op if this flag
	 * is not set. */
	IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL = (1 << 3)
}
ImxVpuApiDecGlobalInfoFlags;

/* Global, static, invariant information about the underlying decoder.
 * Retrieve this by calling imx_vpu_api_dec_get_global_info(). */
typedef struct
{
	/* Bitwise OR combination of the flags from ImxVpuApiDecGlobalInfoFlags. */
	uint32_t flags;

	/* FourCC identifying the underlying hardware codec. This is useful
	 * in combination with platform specific tiled color formats. Also see
	 * IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_TILED_OUTPUT for that. */
	uint32_t hardware_type;

	/* Minimum required size for the stream buffer, in bytes.
	 * If this is 0, then the VPU decoder does not use a stream buffer. */
	size_t min_required_stream_buffer_size;
	/* Required alignment for the stream buffer' physical address, in bytes.
	 * A value of 0 or 1 indicates that no alignment is required. */
	size_t required_stream_buffer_physaddr_alignment;
	/* Required alignment for the stream buffer's size, in bytes.
	 * A value of 0 or 1 indicates that no alignment is required.
	 * The value of min_required_stream_buffer_size is already aligned
	 * to this value. */
	size_t required_stream_buffer_size_alignment;

	/* Array of compression formats this decoder supports. */
	ImxVpuApiCompressionFormat const *supported_compression_formats;
	size_t num_supported_compression_formats;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiDecGlobalInfo;


/* Returns global invariant information about the decoder.
 *
 * The information can be called at any time, but typically is called at
 * least before calling imx_vpu_api_dec_open() to be able to allocate
 * a suitable stream buffer and set up the open params properly.
 *
 * See ImxVpuApiDecGlobalInfo for details about the global information.
 *
 * @return Pointer to the global information. The returned pointer refers
 *         to a static const data block, so do not try to modify or
 *         free() it. This return value is never NULL.
 */
ImxVpuApiDecGlobalInfo const * imx_vpu_api_dec_get_global_info(void);

/* Returns global, invariant details about the decoder's support for this
 * compression format.
 *
 * For h.264, h.265, VP8, and VP9, additional support details are available.
 * To get these, cast the return value to ImxVpuApiH264SupportDetails,
 * ImxVpuApiH265SupportDetails, ImxVpuApiVP8SupportDetails, and
 * ImxVpuApiVP9SupportDetails, respectively.
 *
 * @return Pointer to the support details. The returned pointer refers to a
 *         const data block, so do not try to modify or free() it.
 *         This return value is never NULL, unless the ImxVpuApiDecGlobalInfo
 *         flags indicate that decoding is not supported by the hardware.
 */
ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_dec_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format);

/* Opens a new decoder instance.
 *
 * The stream buffer must have the alignment and minimum size indicated by the
 * values returned by imx_vpu_api_dec_get_global_info().
 *
 * @param decoder Pointer to a ImxVpuApiDecoder pointer that will be set to point
 *        to the new decoder instance. Must not be NULL.
 * @param open_params Parameters for opening a new decoder instance.
 *        Must not be NULL.
 * @param stream_buffer Stream buffer to be used in the decoding process.
 *        Must not be NULL unless the minimum stream buffer size is 0.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_DEC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_ERROR: Unspecified error. Typically indicates an
 * internal problem of the underlying decoder.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR: Could not access memory
 * from the stream buffer.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS: Extra header data is invalid, or
 * stream buffer size is insufficient.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT: The compression
 * format specified in open_params is not supported by this decoder. Check the
 * supported_compression_formats list in ImxVpuApiDecGlobalInfo.
 */
ImxVpuApiDecReturnCodes imx_vpu_api_dec_open(ImxVpuApiDecoder **decoder, ImxVpuApiDecOpenParams *open_params, ImxDmaBuffer *stream_buffer);

/* Closes a decoder instance.
 *
 * After an instance was closed, it is gone and cannot be used anymore. Trying to
 * close the same instance multiple times results in undefined behavior.
 *
 * @param decoder Decoder instance. Must not be NULL.
 */
void imx_vpu_api_dec_close(ImxVpuApiDecoder *decoder);

/* Returns information about the current stream.
 *
 * The return value of this function is only valid after imx_vpu_api_dec_decode()
 * returned the output code IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE .
 *
 * It is valid to call this function multiple times after that output code was
 * returned. The function then just returns the same stream information again.
 *
 * The returned pointer refers to an internal structure containing the stream
 * information, so the contents must not be modified.
 *
 * The stream information structure changes if an imx_vpu_api_dec_decode()
 * call again returns IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @return Information about the current stream. Do not try to modify its contents,
 *         and do not try to free() the returned pointer. This value is never NULL,
 *         unless the ImxVpuApiDecGlobalInfo flags indicate that decoding is no
 *         supported by the hardware. But, the contents are only valid after the
 *         output code mentioned above is returned.
 */
ImxVpuApiDecStreamInfo const * imx_vpu_api_dec_get_stream_info(ImxVpuApiDecoder *decoder);

/* Adds framebuffers to the decoder's buffer pool.
 *
 * A framebuffer is made of one DMA buffer where decoded frames are stored into,
 * and one user-defined framebuffer context pointer.
 *
 * This function must not be called until imx_vpu_api_dec_decode() returns either
 * one of these two output codes:
 *
 * 1. IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE
 * 2. IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER
 *
 * In case of output code #1, the user needs to pass at least as many
 * framebuffers to the decoder as indicated by the min_num_required_framebuffers
 * field of the ImxVpuApiDecStreamInfo structure.
 *
 * In case of output code #2, the user must pass exactly one framebuffer
 * to this function.
 *
 * User-defined context pointers are useful for when each framebuffer needs to
 * be associated with some additional context. One example would be an OpenGL
 * texture ID in case the DMA buffer is used as the texture's backing store.
 * Later, when calling imx_vpu_api_dec_get_decoded_frame(), the fb_context
 * field of the ImxVpuApiRawFrame structure filled by that function then
 * contains the context pointer of the framebuffer the frame was decoded into.
 * This can be used for identifying which framebuffer this is for example.
 * This context pointer is not to be confused with the non-framebuffer context
 * pointers in the ImxVpuApiRawFrame and ImxVpuApiEncodedFrame structures.
 * These other context pointers are used for associating encoded frames with
 * their corresponding decoded raw frames.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param fb_dma_buffers Array containing the framebuffer DMA buffers to add
 *        to the decoder instance's pool. The array size must match the value
 *        of num_framebuffers. Must not be NULL.
 *        After this function call is done, this array can be discarded,
 *        since this function copies the array's context pointers internally.
 *        However, the referred DMA buffers themselves must continue to exist
 *        until the decoder is closed.
 * @param fb_contexts Array containing context pointers for each framebuffer
 *        to add. Can be NULL if no context pointers are needed, otherwise
 *        this array's size must match the value of num_framebuffers.
 *        After this function call is done, this array can be discarded,
 *        since this function copies the array's context pointers internally.
 * @param num_framebuffers How many framebuffers to add. Must at least be 1.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_DEC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL: Function was called even though
 * the output code of the last imx_vpu_api_dec_decode() did not indicate that
 * additional framebuffers are needed.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR: Could not access memory
 * from the DMA buffers.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_PARAMS: Values in the DMA buffer array
 * entries are invalid.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS: Number of framebuffers
 * as indicated by num_framebuffers is insufficient.
 */
ImxVpuApiDecReturnCodes imx_vpu_api_dec_add_framebuffers_to_pool(ImxVpuApiDecoder *decoder, ImxDmaBuffer **fb_dma_buffers, void **fb_contexts, size_t num_framebuffers);

/* Enables the drain mode.
 *
 * Draining consists of two parts: First, any queued encoded input frame
 * is decoded by the VPU. Second, any decoded but not yet retrieved output frame
 * is retrieved and output. The first part can be done simply by repeatedly
 * calling imx_vpu_api_dec_decode() until one of these terminating cases occur:
 *
 *   1. The IMX_VPU_API_DEC_OUTPUT_CODE_EOS output code
 *      is returned
 *   2. The IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED output code
 *      is returned
 *   3. A return code that indicates an error is returned
 *
 * No special mode is needed for this.
 *
 * The second part however requires the "drain mode". In this mode, the VPU
 * will be instructed to not expect any encoded input frames anymore, and instead
 * provide any queued decoded frame it may still have, until its internal queues
 * are fully emptied.
 *
 * Once this mode is enabled, imx_vpu_api_dec_decode() has to be until it reports
 * the IMX_VPU_API_ENC_OUTPUT_CODE_EOS output code. After EOS is reached, the drain
 * mode is automatically turned off again, and decoding new encoded frames can again
 * take place.
 *
 * @param decoder Decoder instance. Must not be NULL.
 */
void imx_vpu_api_dec_enable_drain_mode(ImxVpuApiDecoder *decoder);

/* Checks if drain mode is enabled.
 *
 * 1 = enabled. 0 = disabled.
 *
 * @param decoder Decoder instance. Must not be NULL.
 */
int imx_vpu_api_dec_is_drain_mode_enabled(ImxVpuApiDecoder *decoder);

/* Flushes the decoder.
 *
 * Any internal undecoded or queued frames are discarded.
 *
 * If this is called before framebuffers were added to the decoder instance,
 * then this function does nothing.
 *
 * An ongoing drain mode is turned off by this function.
 *
 * After calling this function, before calling imx_vpu_api_dec_decode(),
 * it is necessary to feed in data with imx_vpu_api_dec_push_encoded_frame(),
 * since, as mentioned above, any previously pushed/queued encoded frames
 * also get discarded by the flush operation.
 *
 * @param decoder Decoder instance. Must not be NULL.
 */
void imx_vpu_api_dec_flush(ImxVpuApiDecoder *decoder);

/* Pushes encoded frame data into the decoder's stream buffer.
 *
 * Only complete frames can be pushed in. That is, no partial data is allowed.
 * If partial data is pushed in, then the decoder is in an undefined state.
 * If the incoming data consists of partial frames, then it is up to the user
 * to make sure that these parts are assembled into complete frames before
 * calling this function.
 *
 * This function needs to be called at the beginning, before the first
 * imx_vpu_api_dec_decode() call, and when imx_vpu_api_dec_decode() returns
 * the output code IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED .
 *
 * This function is not to be called when the drain mode is enabled.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_DEC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL: Tried to call this before
 * the previously pushed encoded frame was decoded, or tried to call this
 * in drain mode.
 */
ImxVpuApiDecReturnCodes imx_vpu_api_dec_push_encoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiEncodedFrame *encoded_frame);

/* Sets the DMA buffer for decoded frames.
 *
 * If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * is set in the ImxVpuApiDecGlobalInfo structure, this function does nothing,
 * because in that case, frames are decoded into a framebuffer from the
 * decoder's pool.
 *
 * Otherwise, it sets the DMA buffer the decoded frames shall be written to.
 * It must be called before calling imx_vpu_api_dec_decode(), and must not be
 * called again until after imx_vpu_api_dec_get_decoded_frame() was called,
 * otherwise the frame pixels won't be written into the specified DMA buffer
 * correctly. The DMA buffer's size has to be at least as large as the field
 * min_output_framebuffer_size from the stream info indicates. This also
 * means that this must not be called until a stream info was reported. The
 * set output DMA buffer must not be deallocated until the frame was decoded
 * and retrieved by calling imx_vpu_api_dec_get_decoded_frame(), since until
 * then, the decoder will write pixels into that buffer.
 *
 * fb_context is the equivalent of the fb_contexts array passed to
 * imx_vpu_api_dec_add_framebuffers_to_pool(). Since the output frame DMA
 * buffer is not part of the VPU's framebuffer pool, the fb_context field
 * in the ImxVpuRawFrame instance filled by imx_vpu_api_dec_get_decoded_frame()
 * cannot be set to one of the context pointers from the pool's fb_context
 * array. So, it is set to the value of the fb_context argument instead.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param output_frame_dma_buffer Output frame DMA buffer to use.
 *        Must not be NULL.
 * @param fb_context Framebuffer context to use.
 */
void imx_vpu_api_dec_set_output_frame_dma_buffer(ImxVpuApiDecoder *decoder, ImxDmaBuffer *output_frame_dma_buffer, void *fb_context);

/* Performs a decoding step.
 *
 * This function is used in conjunction with imx_vpu_api_dec_push_encoded_frame().
 * For correct decoding, the user has to first call imx_vpu_api_dec_push_encoded_frame()
 * to feed in one encoded frame, and then repeatedly call imx_vpu_api_dec_decode()
 * until at least one of these cases occur:
 *
 *   1. The IMX_VPU_API_DEC_OUTPUT_CODE_EOS output code
 *      is returned
 *   2. The IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED output code
 *      is returned
 *   3. A return code that indicates an error is returned
 *   4. imx_vpu_api_dec_flush() was called
 *
 * In cases #1 and #3, decoding cannot continue, and the decoder has to be
 * closed. In cases #2 and #4, imx_vpu_api_dec_push_encoded_frame() must be
 * called again because the decoder ran out of data to decode. Afterwards, the
 * user can continue calling imx_vpu_api_dec_decode() again.
 *
 * In all other cases, the user is expected to act according to the returned
 * output code, and call imx_vpu_api_dec_decode(). Also check the
 * ImxVpuApiDecOutputCodes documentation for details.
 *
 * If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * flag in ImxVpuApiDecGlobalInfo is not set, then the user must set an output
 * frame DMA buffer before calling imx_vpu_api_dec_decode() if not already done.
 * This is done by calling imx_vpu_api_dec_set_output_frame_dma_buffer().
 * Frames will then be decoded into that DMA buffer. This function needs to be
 * called again if imx_vpu_api_dec_get_decoded_frame() is called (after the
 * output code indicated that a decoded frame is available).
 *
 * Repeatedly calling imx_vpu_api_dec_decode() until the output code
 * IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED is needed is a valid way
 * of partially draining a decoder without having to enable the drain mode. See
 * imx_vpu_api_dec_enable_drain_mode() for details.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param output_code Pointer to an ImxVpuApiDecOutputCodes enum that will be
 *        set to the output code of this decoding step. Must not be NULL.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_DEC_RETURN_CODE_OK: Success. Check the output_code for details.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_ERROR: Internal decoding error. Consult log output.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL: The following are possible:
 * - A frame was previously decoded,  but imx_vpu_api_dec_get_decoded_frame()
 *   was not called before this function was called again.
 * - IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL is
 *   not set in the ImxVpuApiDecGlobalInfo structure, and prior to calling
 *   imx_vpu_api_dec_decode(), imx_vpu_api_dec_set_output_frame_dma_buffer()
 *   was not called.
 * - A previous imx_vpu_api_dec_decode() call return as the output code
 *   IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE, but framebuffers
 *   were not added to the pool prior to the current imx_vpu_api_dec_decode()
 *   call.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_BITSTREAM: The encoded bitstream
 * cannot be decoded because it is not supported. This can for example happen
 * when trying to decode an h.264 stream with an unsupported profile.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_TIMEOUT: VPU timeout occurred during decoding
 * because the hardware is already busy with some other operation and is not
 * available for decoding.
 */
ImxVpuApiDecReturnCodes imx_vpu_api_dec_decode(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code);

/* Get details about a decoded frame.
 *
 * This must not be called until imx_vpu_api_dec_decode() returns the output
 * code IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE . Once this
 * happens, the user *must* call this function. Decoding cannot continue
 * until the decoded frame is retrieved with this function. Also, this function
 * cannot be called more than once after that output code was returned by
 * the prior imx_vpu_api_dec_decode() call. This is because this function
 * modifies internal states.
 *
 * If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * flag is set in ImxVpuApiDecGlobalInfo, then this function will set the
 * fb_dma_buffer field of decoded_frame to point to the DMA buffer of the
 * framebuffer from the decoder's pool where the frame was decoded into.
 * If the flag is not set, then the user must set an output frame by calling
 * imx_vpu_api_dec_set_output_frame_dma_buffer() before decoding with
 * imx_vpu_api_dec_decode(). imx_vpu_api_dec_get_decoded_frame() then sets
 * fb_dma_buffer to the DMA buffer passed to this function, and the fb_context
 * is set to the fb_context passed to that function.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param decoded_frame Pointer to ImxVpuApiRawFrame structure to fill details
 *        about the decoded frame into. Must not be NULL.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_DEC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL: Function was called before a
 * frame was decoded, or it was called more than once between decoding frames.
 */
ImxVpuApiDecReturnCodes imx_vpu_api_dec_get_decoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiRawFrame *decoded_frame);

/* Returns a framebuffer to the decoder's pool.
 *
 * This function only needs to be called if in ImxVpuApiDecGlobalInfo, the flag
 * IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL is set.
 * The decoder will then decode frames into framebuffers that are from its
 * internal pool and mark these as in use. Therefore, these framebuffers need
 * to be returned to the pool once they are no longer needed so that the decoder
 * knows they are no longer in use and can reuse them. Otherwise, the decoder
 * may keep requesting new framebuffers (because all of the existing ones are
 * in use), and eventually the system will run out of memory.
 *
 * If the IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL
 * flag is not set in ImxVpuApiDecGlobalInfo, this function does nothing.
 *
 * The framebuffer is returned by specifying its DMA buffer (fb_dma_buffer).
 *
 * Do not try to return the same framebuffer DMA buffer multiple times, otherwise
 * the decoder is in an undefined state. Also do not try to "return" a framebuffer
 * that isn't part of the VPU's framebuffer pool. In other words, only ever return
 * the DMA buffer that was specified by imx_vpu_api_dec_get_decoded_frame(), and
 * don't try to return it more than once after the imx_vpu_api_dec_get_decoded_frame()
 * call.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param fb_dma_buffer DMA buffer of the framebuffer to return to the pool.
 */
void imx_vpu_api_dec_return_framebuffer_to_decoder(ImxVpuApiDecoder *decoder, ImxDmaBuffer *fb_dma_buffer);

/* Retrieves information about a skipped frame.
 *
 * This should only be called after imx_vpu_api_dec_decode() returned the output
 * code IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED . Otherwise, this function's
 * return values are undefined.
 *
 * The context/pts/dts values are the ones from the ImxVpuApiEncodedFrame that
 * was skipped.
 *
 * @param decoder Decoder instance. Must not be NULL.
 * @param reason Pointer to ImxVpuApiDecSkippedFrameReasonse enum vlaue
 *        that shall be set to specify the reason for why the frame was skipped.
 *        Can be NULL if not needed.
 * @param context Pointer to the context that shall be set to the context of the
 *        skipped frame. Can be NULL if not needed.
 * @param pts Pointer to the PTS that shall be set to the PTS of the
 *        skipped frame. Can be NULL if not needed.
 * @param dts Pointer to the DTS that shall be set to the DTS of the
 *        skipped frame. Can be NULL if not needed.
 */
void imx_vpu_api_dec_get_skipped_frame_info(ImxVpuApiDecoder *decoder, ImxVpuApiDecSkippedFrameReasons *reason, void **context, uint64_t *pts, uint64_t *dts);




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* OVERVIEW ABOUT ENCODING
 *
 *
 * INITIALIZING THE ENCODER
 *
 * First, a stream buffer must be allocated. This is a DMA buffer that
 * is allocated with libimxdmabuffer's imx_dma_buffer_allocate(). The required
 * size and alignment can be retrieved from the min_required_stream_buffer_size
 * and required_stream_buffer_physaddr_alignment fields that are present in
 * the ImxVpuApiEncGlobalInfo structure. To get the const global instance
 * of that structure, use imx_vpu_api_enc_get_global_info().
 *
 * NOTE: If min_required_stream_buffer_size is 0, then this first step
 * is to be skipped, since this then means that the VPU does not need
 * a stream buffer.
 *
 * Once the stream buffer is allocated, an ImxVpuApiEncOpenParams structure
 * must be initialized. Typically, using imx_vpu_api_enc_set_default_open_params()
 * to fill that structure with default values and then optionally tweaking
 * some of that structure's values is the preferred way to initialize it.
 *
 * The encoder can then be created by calling imx_vpu_api_enc_open(). That
 * function needs the stream buffer and ImxVpuApiEncOpenParams structure.
 *
 * Immediately after creating the encoder, it is necessary to retrieve the
 * stream info by calling imx_vpu_api_enc_get_stream_info(). That structure
 * contains several details about the VPU framebuffers like their metrics,
 * the minimum framebuffer pool size etc. If that structure's
 * min_num_required_framebuffers field is nonzero, then the VPU uses a framebuffer
 * pool for its encoding. That pool is not used for output, just for internal
 * temporary data for the encoder's work. At least as many framebuffers as
 * indicated by that field must be allocated and added to the VPU encoder's
 * pool. The framebuffer size in bytes can be looked up in the strema info's
 * min_framebuffer_size field (also look at framebuffer_alignment), and the
 * framebuffer must be a DMA buffer, allocated with imx_dma_buffer_allocate().
 * Once allocated, imx_vpu_api_enc_add_framebuffers_to_pool() must be called
 * to add the buffer to the pool. The buffer stays in the pool until the
 * encoder is closed.
 *
 *
 * MAIN ENCODING LOOP
 *
 * The encoding loop goes as follows:
 *
 * 1. Fill an ImxVpuApiRawFrame instance with information about the frame
 *    that is to be encoded. If there are no more frames to encode, continue
 *    at step 5.
 * 2. Feed that frame into the VPU by calling imx_vpu_api_enc_push_raw_frame().
 * 3. Call imx_vpu_api_enc_encode() and get its output code.
 * 4. - If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED,
 *      go back to step 1.
 *    - If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE,
 *      just go back to step 3.
 *    - If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE,
 *      use imx_vpu_api_enc_get_encoded_frame() to retrieve the encoded frame,
 *      output the encoded frame, then go back to step 3.
 *    - If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER,
 *      add a framebuffer with imx_vpu_api_enc_add_framebuffers_to_pool(), then
 *      go back to step 3.
 *    - If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_FRAME_SKIPPED, retrieve
 *      the details about the skipped frame with imx_vpu_api_enc_get_skipped_frame_info(),
 *      then go back to step 3.
 * 5. Drain the encoder as explained below.
 * 6. Exit the loop.
 *
 *
 * DRAINING THE ENCODER
 *
 * To drain the encoder, all that is needed is to repeatedly call
 * imx_vpu_api_enc_encode() (_without_ any imx_vpu_api_enc_push_raw_frame()
 * calls) until one of the terminating cases occur:
 *
 *   1. The IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED output
 *      code is returned
 *   2. A return code that indicates an error is returned
 *
 *
 * SHUTTING DOWN THE ENCODER
 *
 * The encoder is shut down by calling imx_vpu_api_enc_close(). Once
 * that was called, the framebuffers that were previously added to the
 * VPU encoder's pool are no longer used, and can be deallocated. Note
 * that calling imx_vpu_api_enc_close() will also drop any encoded
 * frame data that may be queued in the encoder. Consider draining
 * the encoder before shutting it down.
 */


/* Opaque encoder structure. */
typedef struct _ImxVpuApiEncoder ImxVpuApiEncoder;


/* Return codes. Only IMX_VPU_API_ENC_RETURN_CODE_OK indicates a success;
 * the other codes all indicate an error. Unless otherwise note, an error
 * implies that the encoder is no longer able to operate correctly and
 * needs to be discarded with imx_vpu_api_enc_close(). */
typedef enum
{
	/* Operation finished successfully. */
	IMX_VPU_API_ENC_RETURN_CODE_OK = 0,
	/* Input parameters were invalid. This is intended as a catch-all for errors
	 * with input parameters that aren't described well by other return codes. */
	IMX_VPU_API_ENC_RETURN_CODE_INVALID_PARAMS,
	/* Accessing DMA memory failed. This happens when trying to memory-map
	 * DMA memory unsuccessfully. */
	IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR,
	/* User tried to call imx_vpu_api_enc_open() with a compression format
	 * set in the open_params that is not supported by the codec. In other
	 * words, the specified format is not included in the ImxVpuApiEncGlobalInfo
	 * supported_compression_formats list. */
	IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT,
	/* The format specific parameters in open_params are not supported by
	 * the encoder. */
	IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT_PARAMS,
	/* User tried to call imx_vpu_api_enc_open() with a color format
	 * set in the open_params that is not supported by the codec. In other
	 * words, the specified color format is not included in the list of supported
	 * color formats for the given compression format. */
	IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COLOR_FORMAT,
	/* User tried to pass a stream buffer to imx_vpu_api_enc_open() that is
	 * not large enough. */
	IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE,
	/* imx_vpu_api_enc_add_framebuffers_to_pool() was called with insufficient
	 * number of buffers. Right after having opened the encoder with
	 * imx_vpu_api_enc_open(), the user is supposed to get the minimum number
	 * of required framebuffers by calling imx_vpu_api_enc_get_stream_info(),
	 * and add at least this many framebuffers to the encoder's pool by calling
	 * imx_vpu_api_enc_add_framebuffers_to_pool() (unless the minimum number is 0).
	 * If fewer than that number are added, this error occurs. */
	IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS,
	/* The incoming frames cannot be encoded because they are too large for
	 * the encoder to handle. */
	IMX_VPU_API_ENC_RETURN_CODE_FRAMES_TOO_LARGE,
	/* Functions were not called in the right sequence or at inappropriate times.
	 * For example, trying to call imx_vpu_api_enc_push_raw_frame() again, before
	 * the previously pushed raw frame was encoded, will trigger this. */
	IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL,
	/* Internal driver or hardware timeout. */
	IMX_VPU_API_ENC_RETURN_CODE_TIMEOUT,
	/* General return code for when an error occurs. This is used as a catch-all
	 * for when the other error return codes do not match the error. */
	IMX_VPU_API_ENC_RETURN_CODE_ERROR
}
ImxVpuApiEncReturnCodes;

/* Returns a human-readable description of the return code.
 * Useful for logging. */
char const * imx_vpu_api_enc_return_code_string(ImxVpuApiEncReturnCodes code);

/* Encoding output codes. These are returned by imx_vpu_api_enc_encode() only.
 * This function returns an output code only if its return code is
 * IMX_VPU_API_ENC_RETURN_CODE_OK.
 *
 * Output codes describe the result of the encoding operation in greater
 * detail, and are essential for deciding the next necessary encoding step.
 */
typedef enum
{
	/* Encoding did not (yet) yield output. Additional
	 * imx_vpu_api_enc_encode() calls might. */
	IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE = 0,
	/* The encoder needs one additional framebuffer in its pool for encoding.
	 * The user is supposed to allocate one extra framebuffer and add it by
	 * calling imx_vpu_api_enc_add_framebuffers_to_pool(). Encoding cannot
	 * continue until a framebuffer was added. This output code only occurs
	 * after framebuffers were initially added to the pool, right after
	 * the imx_vpu_api_enc_open() call (hence the "additional"). */
	IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER,
	/* Encoded yielded a fully encoded frame. The user has to call
	 * imx_vpu_api_enc_get_encoded_frame() to retrieve the frame. Until that
	 * function was called, encoding cannot continue. */
	IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE,
	/* No more raw frame input data is available in the encoder. The user is
	 * now expected to call imx_vpu_api_enc_push_raw_frame() to feed in
	 * a new raw frame for encoding. Then the encoding can continue. */
	IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED,
	/* DEPRECATED. This output code is not used anymore, and kept here for
	 * backwards compatibility with existing code. Do not use in new code. */
	IMX_VPU_API_ENC_OUTPUT_CODE_EOS,
	/* Encoder skipped the frame to preserve bitrate. Only used when the
	 * configured bitrate is nonzero and frameskipping is enabled (see
	 * ImxVpuApiEncOpenParams). */
	IMX_VPU_API_ENC_OUTPUT_CODE_FRAME_SKIPPED
}
ImxVpuApiEncOutputCodes;

/* Returns a human-readable description of the output code.
 * Useful for logging. */
char const * imx_vpu_api_enc_output_code_string(ImxVpuApiEncOutputCodes code);

/* MPEG-4 part 2 specific encoder parameters. */
typedef struct
{
	/* If set to 1, MPEG-4 data partitioning mode is enabled. If set to
	 * 0, it is disabled. */
	int enable_data_partitioning;
	/* If set to 1, additional reversible variable length codes for
	 * increased resilience are added. If 0, they are omitted. */
	int enable_reversible_vlc;
	/* The mechanism to use for switching between two VLC's for intra
	 * coeffient encoding, as described in ISO/IEC 14496-2 section 6.3.6.
	 * Valid range is 0 to 7. */
	unsigned int intra_dc_vlc_thr;
	/* If set to 1, it enables the header extension code. 0 disables it. */
	int enable_hec;
	/* The MPEG-4 part 2 standard version ID. Valid values are 1 and 2. */
	unsigned int version_id;
}
ImxVpuApiEncMPEG4OpenParams;

/* h.263 specific encoder parameters. */
typedef struct
{
	/* 1 = Annex.I support is enabled. 0 = disabled. */
	int enable_annex_i;
	/* 1 = Annex.J support is enabled. 0 = disabled. */
	int enable_annex_j;
	/* 1 = Annex.K support is enabled. 0 = disabled. */
	int enable_annex_k;
	/* 1 = Annex.T support is enabled. 0 = disabled. */
	int enable_annex_t;
}
ImxVpuApiEncH263OpenParams;

/* h.264 specific flags for use in ImxVpuApiEncOpenParamsFlags. */
typedef enum
{
	/* If this flag is set, the encoder will use a full video input
	 * signal range. The valid Y, U, and V values will be 0-255. If
	 * this is not set, restricted ranges are used: the Y range is
	 * 16-235, the U and V ranges are 16-240. */
	IMX_VPU_API_ENC_H264_OPEN_PARAMS_FLAG_FULL_VIDEO_RANGE = (1 << 10)
}
ImxVpuApiEncH264OpenParamsFlags;

/* h.264 specific encoder parameters. */
typedef struct
{
	/* h.264 profile that the input frames shall be encoded to. */
	ImxVpuApiH264Profile profile;

	/* h.264 level that the input frames shall be encoded to.
	 * If set to IMX_VPU_API_H264_LEVEL_UNDEFINED, the maximum possible
	 * level according to the given bitrate and resolution is picked. */
	ImxVpuApiH264Level level;

	/* If set to 1, the encoder produces access unit delimiters.
	 * If set to 0, this is disabled. Default value is 0.
	 * This has no effect if IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED
	 * is not set in the ImxVpuApiH264SupportDetails structure. */
	int enable_access_unit_delimiters;
}
ImxVpuApiEncH264OpenParams;

/* h.265 specific encoder parameters. */
typedef struct
{
	/* h.265 profile that the input frames shall be encoded to. */
	ImxVpuApiH265Profile profile;

	/* h.265 level that the input frames shall be encoded to. */
	ImxVpuApiH265Level level;

	/* h.265 tier to use for encoding. */
	ImxVpuApiH265Tier tier;

	/* If set to 1, the encoder produces access unit delimiters.
	 * If set to 0, this is disabled. Default value is 0.
	 * This has no effect if IMX_VPU_API_H265_FLAG_ACCESS_UNITS_SUPPORTED
	 * is not set in the ImxVpuApiH264SupportDetails structure. */
	int enable_access_unit_delimiters;
}
ImxVpuApiEncH265OpenParams;

typedef enum
{
	IMX_VPU_API_ENC_VP8_PARTITION_COUNT_1,
	IMX_VPU_API_ENC_VP8_PARTITION_COUNT_2,
	IMX_VPU_API_ENC_VP8_PARTITION_COUNT_4,
	IMX_VPU_API_ENC_VP8_PARTITION_COUNT_8
}
ImxVpuApiEncVP8PartitionCount;

/* Returns the partition count as an integer. For example,
 * IMX_VPU_API_ENC_VP8_PARTITION_COUNT_4 returns 4.
 * Invalid values return -1. */
int imx_vpu_api_vp8_partition_count_number(ImxVpuApiEncVP8PartitionCount partition_count);

/* VP8 specific encoder parameters. */
typedef struct
{
	/* VP8 profile that the input frames shall be encoded to. */
	ImxVpuApiVP8Profile profile;

	/* How many partitions to create. Streams with more than one partition
	 * allow for multi-threaded decoding (partitions are decoded in parallel). */
	ImxVpuApiEncVP8PartitionCount partition_count;

	/* If nonzero, error resilient mode is enabled. This mode is useful when
	 * the video signal is transmitted over protocols like UDP which may lose
	 * data. Recommended for low latency video signals over lossy channels,
	 * like in video conferencing or other low latency live feeds. Not
	 * recommended outside of such cases. */
	int error_resilient_mode;
}
ImxVpuApiEncVP8OpenParams;

/* Flags for use in ImxVpuApiEncOpenParamsFlags. */
typedef enum
{
	/* Allow encoder to skip frames in order to maintain the bitrate.
	 * If this flag is set, and the encoder skips a frame, then the
	 * ImxVpuApiEncodedFrame result from imx_vpu_api_enc_get_encoded_frame()
	 * will have its frame_type field set to IMX_VPU_API_FRAME_TYPE_SKIP,
	 * and the output code of imx_vpu_api_enc_encode() will be set to
	 * IMX_VPU_API_ENC_OUTPUT_CODE_FRAME_SKIPPED. */
	IMX_VPU_API_ENC_OPEN_PARAMS_FLAG_ALLOW_FRAMESKIPPING = (1 << 0),
	/* Use intra refresh as an alternative to the classic I/IDR and
	 * GOP based encoding. */
	IMX_VPU_API_ENC_OPEN_PARAMS_FLAG_USE_INTRA_REFRESH = (1 << 1),
}
ImxVpuApiEncOpenParamsFlags;

/* Parameters for opening a enccoder. */
typedef struct
{
	/* Width and height of frames to encode, in pixels. These are the
	 * width and height of the frames _without_ padding rows/columns. */
	size_t frame_width, frame_height;

	/* The compression format to encode the video frames with. */
	ImxVpuApiCompressionFormat compression_format;

	/* Color format of the frames to encode. */
	ImxVpuApiColorFormat color_format;

	/* Frame rate ratio. */
	unsigned int frame_rate_numerator, frame_rate_denominator;

	/* Bitrate to use for encoding, in kbps. If this is set to 0, then rate
	 * control is disabled, and constant quantization encoding is used instead.
	 * With some formats like JPEG, this is always treated as if it were 0. */
	unsigned int bitrate;
	/* Constant quantization to use for encoding. This is only  used if bitrate
	 * is set to 0. The valid range depends on the encoding format. The valid
	 * quantization range is described in the format support details. Call
	 * imx_vpu_api_enc_get_compression_format_support_details() to get them. */
	unsigned int quantization;
	/* Size for the group of pictures (GOP). Some formats do not know the notion
	 * of a GOP. In these cases, the gop_size value is used as an interval for I
	 * frames instead. If the IMX_VPU_API_ENC_OPEN_PARAMS_FLAG_USE_INTRA_REFRESH
	 * flag is set, this is used as a factor by the encoder for deciding how
	 * many intra macroblocks are encoded per frame. Must be at least 1.
	 * 1 would mean that exactly one picture (the beginning I frame) is present.
	 * 2 would mean 1 beginning I frame and 1 additional P frame etc. */
	unsigned int gop_size;
	/* How many macroblocks at least to encode as intra macroblocks in every
	 * P frame. If this is set to 0, intra macroblocks are not used. Not all
	 * formats use this parameter.
	 * DEPRECATED. Use IMX_VPU_API_ENC_OPEN_PARAMS_FLAG_USE_INTRA_REFRESH instead. */
	unsigned int min_intra_refresh_mb_count;
	/* How many GOPs pass until a closed GOP is enforced. If for example this
	 * is set to 3, then the bitstream is encoded such that no frames from
	 * GOPs #0 #1 #2 can be referred to be GOPs #3 #4 etc. If this is an h.264
	 * stream, then there will be an IDR frame between GOPs #2 and #3. A
	 * closed_gop_interval value of 0 disables such enforced intervals.
	 * A value of 1 forces each and every GOP to be closed. This value is
	 * ignored if the codec does not know the notion of a GOP.
	 * Default value is 0. */
	unsigned int closed_gop_interval;

	/* Format specific parameters. Consult the individual documentation for more. */
	union
	{
		ImxVpuApiEncMPEG4OpenParams mpeg4_open_params;
		ImxVpuApiEncH263OpenParams h263_open_params;
		ImxVpuApiEncH264OpenParams h264_open_params;
		ImxVpuApiEncH265OpenParams h265_open_params;
		ImxVpuApiEncVP8OpenParams vp8_open_params;
	}
	format_specific_open_params;

	/* If set to a value > 0, then this quantization parameter is used
	 * for intra frames during CBR encoding. This can be useful for
	 * tuning the size variations of intra vs predicted frames and smoothen
	 * out jumps in size. Depending on the transmission channel, intra
	 * frames that are significantly larger than predicted frames can
	 * be problematic. If this is set to 0, rate control calculates
	 * this value on its own.
	 * This value has no effect when rate control is disabled. */
	int fixed_intra_quantization;

	/* Bitwise OR combination of flags from ImxVpuApiEncOpenParamsFlags. */
	uint32_t flags;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE - sizeof(unsigned int) - sizeof(int) - sizeof(uint32_t)];
}
ImxVpuApiEncOpenParams;

typedef struct
{
	/* The minimum amount of framebuffers that must be added to the encoder's
	 * framebuffer pool in order for encoding to proceed. Adding more than
	 * this amount is okay. Adding less will cause encoding to fail, and the
	 * encoding will have to be closed. Some encoders may not need a pool
	 * at all. These encoders set this value to 0. */
	size_t min_num_required_framebuffers;

	/* Minimum size of framebuffer DMA buffers, both of those for the encoder's
	 * framebuffer pool, and those that are pushed into the encoder by calling
	 * imx_vpu_api_enc_push_raw_frame(). */
	size_t min_framebuffer_size;

	/* Required physical address alignment for the DMA buffers of framebuffers
	 * that get added to the encoder's framebuffer pool, and for the DMA
	 * buffers that are pushed into the encoder.  0 and 1 mean that the
	 * address does not have to be aligned in any special way. */
	size_t framebuffer_alignment;

	/* Frame rate ratio. */
	unsigned int frame_rate_numerator, frame_rate_denominator;

	/* Framebuffer metrics of the frames to encode. */
	ImxVpuApiFramebufferMetrics frame_encoding_framebuffer_metrics;

	/* Format specific parameters. Consult the individual documentation for more.
	 * These are copied from the corresponding parameters in ImxVpuApiEncOpenParams,
	 * but might have been modified afterwards if necessary. For example, the
	 * h.264 level might be adjusted by the encoder in case it is too high. */
	union
	{
		ImxVpuApiEncMPEG4OpenParams mpeg4_open_params;
		ImxVpuApiEncH263OpenParams h263_open_params;
		ImxVpuApiEncH264OpenParams h264_open_params;
		ImxVpuApiEncH265OpenParams h265_open_params;
		ImxVpuApiEncVP8OpenParams vp8_open_params;
	}
	format_specific_open_params;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiEncStreamInfo;

typedef enum
{
	/* If set, the underlying codec can encode. Some hardware codecs can
	 * only decode or only encode, which is why this flag exists. */
	IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_HAS_ENCODER = (1 << 0),
	/* If set, the encoder can accept semi planar input frames. At least this
	 * or IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED are
	 * always set. */
	IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED = (1 << 1),
	/* If set, the encoder can accept fully planar input frames. At least this
	 * or IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED are
	 * always set. */
	IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED = (1 << 2),
	/* If set, the encoder can also handle at least some of the RGB formats
	 * in ImxVpuApiColorFormat as input data. */
	IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_ENCODER_SUPPORTS_RGB_FORMATS = (1 << 3)
}
ImxVpuApiEncGlobalInfoFlags;

typedef struct
{
	/* Bitwise OR combination of the flags from ImxVpuApiEncGlobalInfoFlags. */
	uint32_t flags;

	/* FourCC identifying the underlying hardware codec. */
	uint32_t hardware_type;

	/* Minimum required size for the stream buffer, in bytes.
	 * If this is 0, then the VPU encoder does not use a stream buffer. */
	size_t min_required_stream_buffer_size;
	/* Required alignment for the stream buffer' physical address, in bytes.
	 * A value of 0 or 1 indicates that no alignment is required. */
	size_t required_stream_buffer_physaddr_alignment;
	/* Required alignment for the stream buffer's size, in bytes.
	 * A value of 0 or 1 indicates that no alignment is required.
	 * The value of min_required_stream_buffer_size is already aligned
	 * to this value. */
	size_t required_stream_buffer_size_alignment;

	/* Array of compression formats this encoder supports. */
	ImxVpuApiCompressionFormat const *supported_compression_formats;
	size_t num_supported_compression_formats;

	/* Reserved bytes for ABI compatibility. */
	uint8_t reserved[IMX_VPU_API_RESERVED_SIZE];
}
ImxVpuApiEncGlobalInfo;


/* Returns global invariant information about the encoder.
 *
 * The information can be called at any time, but typically is called at
 * least before calling imx_vpu_api_enc_open() to be able to allocate
 * a suitable stream buffer and set up the open params properly.
 *
 * See ImxVpuApiEncGlobalInfo for details about the global information.
 *
 * @return Pointer to the global information. The returned pointer refers
 *         to a static const data block, so do not try to modify or
 *         free() it. This return value is never NULL.
 */
ImxVpuApiEncGlobalInfo const * imx_vpu_api_enc_get_global_info(void);

/* Returns global, invariant details about the encoder's support for this
 * compression format.
 *
 * For h.264 and VP8, additional support details are available.
 * To get these, cast the return value to ImxVpuApiH264SupportDetails and
 * ImxVpuApiVP8SupportDetails, respectively.
 *
 * @return Pointer to the support details. The returned pointer refers to a
 *         const data block, so do not try to modify or free() it.
 *         This return value is never NULL, unless the ImxVpuApiEncGlobalInfo
 *         flags indicate that encoding is not supported by the hardware.
 */
ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_enc_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format);

/* Sets the fields of open_params to valid defaults.
 *
 * This is useful if the caller wants to modify only a few fields (or none at all).
 * For details about the arguments, look up the corresponding fields in
 * ImxVpuApiEncOpenParams.
 *
 * @param compression_format Compression format to set in the open params.
 *        Format specific parameters will be set according to this argument.
 * @param color_format Color formats to set in the open params.
 * @param frame_width Frame width to set in the open params.
 * @param frame_height Frame height to set in the open params.
 * @param open_params Pointer to ImxVpuApiEncOpenParams instance to fill with
 *        defaults. Must not be NULL:
 */
void imx_vpu_api_enc_set_default_open_params(ImxVpuApiCompressionFormat compression_format, ImxVpuApiColorFormat color_format, size_t frame_width, size_t frame_height, ImxVpuApiEncOpenParams *open_params);

/* Opens a new encoder instance.
 *
 * The stream buffer must have the alignment and minimum size indicated by the
 * values returned by imx_vpu_api_enc_get_global_info().
 *
 * After opening the encoder, retrieve the stream info by calling
 * imx_vpu_api_enc_get_stream_info(). This is necessary for checking if and
 * how many framebuffers have to be added to the encoder's framebuffer pool.
 * Then, allocate framebuffer DMA buffers, and add them to the encoder's pool
 * by calling imx_vpu_api_enc_add_framebuffers_to_pool().
 * Only after that is done can frame encoding begin.
 *
 * @param encoder Pointer to a ImxVpuApiEncoder pointer that will be set to point
 *        to the new encoder instance. Must not be NULL.
 * @param open_params Parameters for opening a new encoder instance.
 *        Must not be NULL.
 * @param stream_buffer Stream buffer to be used in the encoding process.
 *        Must not be NULL unless the minimum stream buffer size is 0.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Typically indicates an
 * internal problem of the underlying encoder.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR: Could not access memory
 * from the stream buffer.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_PARAMS: Some params other than the ones
 * handled by the other return codes below are invalid. Consult the log output
 * for more details.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT: Compression format
 * in open_params is unsupported. Consult imx_vpu_api_enc_get_global_info() to
 * get the list of supported compression formats.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COLOR_FORMAT: Color format
 * in open_params is unsupported. Consult the ImxVpuApiCompressionFormatSupportDetails
 * for the given compression format to get a list of supported color formats.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE: stream_buffer
 * is not large enough. Consult imx_vpu_api_enc_get_global_info() to get the
 * minimum required stream buffer size.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT: The compression
 * format specified in open_params is not supported by this encoder. Check the
 * supported_compression_formats list in ImxVpuApiEncGlobalInfo.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT_PARAMS: The
 * compression format itself is supported by the encoder, but its parameters
 * aren't. For example, if an unsupported h.264 profile is specified in the
 * format specific section of open_params, this is returned.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_open(ImxVpuApiEncoder **encoder, ImxVpuApiEncOpenParams *open_params, ImxDmaBuffer *stream_buffer);

/* Closes an encoder instance.
 *
 * After an instance was closed, it is gone and cannot be used anymore. Trying to
 * close the same instance multiple times results in undefined behavior.
 *
 * @param encoder Encoder instance. Must not be NULL.
 */
void imx_vpu_api_enc_close(ImxVpuApiEncoder *encoder);

/* Returns information about the stream to be encoded.
 *
 * Unlike its counterpart in the decoder, this function must be called _before_
 * any data is pushed into the encoder (by calling imx_vpu_api_enc_push_raw_frame()).
 * The encoder stream info contains all the necessary information for allocating
 * input frame DMA buffers and for knowing how many framebuffers must be added to
 * the pool. Without a pool, the encoder cannot encode (unless the minimum required
 * number of framebuffers is 0 in the stream info).
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @return Information about the stream to be encoded. Do not try to modify its
 *         contents, and do not try to free() the returned pointer. This value is
 *         never NULL, unless the ImxVpuApiEncGlobalInfo flags indicate that
 *         encoding is not supported by the hardware.
 */
ImxVpuApiEncStreamInfo const * imx_vpu_api_enc_get_stream_info(ImxVpuApiEncoder *encoder);

/* Adds framebuffers to the encoder's buffer pool.
 *
 * A framebuffer is made of one DMA buffer where encoded frames are stored into.
 * Unlike the decoder's pool, this one does not use framebuffer context pointers.
 *
 * Not all encoders have a framebuffer pool. If the min_num_required_framebuffers
 * field in ImxVpuApiEncStreamInfo is 0, then this function should not be called
 * unless IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER is returned by
 * imx_vpu_api_enc_encode() later.
 *
 * This function must be called in two cases:
 *
 * 1. Right after an encoder instance was opened with imx_vpu_api_enc_open()
 *    (look up the encoder stream info to know how many framebuffers are needed)
 * 2. When IMX_VPU_API_ENC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER is returned
 *    by imx_vpu_api_enc_encode() (add exactly one framebuffer then)
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param fb_dma_buffers Array containing the framebuffer DMA buffers to add
 *        to the encoder instance's pool. The array size must match the value
 *        of num_framebuffers. Must not be NULL.
 *        After this function call is done, this array can be discarded,
 *        since this function copies the array's context pointers internally.
 *        However, the referred DMA buffers themselves must continue to exist
 *        until the encoder is closed.
 * @param num_framebuffers How many framebuffers to add. Must at least be 1.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR: Could not access memory
 * from the DMA buffers.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: Function was called even though
 * neither one of the cases outline above apply.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS: Number of framebuffers
 * as indicated by num_framebuffers is insufficient.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_add_framebuffers_to_pool(ImxVpuApiEncoder *encoder, ImxDmaBuffer **fb_dma_buffers, size_t num_framebuffers);

/* DEPRECATED FUNCTION. Do not use it in new code. See the documentation at the
 * start of the encoder section above for an explanation on how to drain properly. */
void imx_vpu_api_enc_enable_drain_mode(ImxVpuApiEncoder *encoder);

/* DEPRECATED FUNCTION. Do not use it in new code. See the
 * ImxVpuApiEncoder documentation for how to drain properly. */
int imx_vpu_api_enc_is_drain_mode_enabled(ImxVpuApiEncoder *encoder);

/* Flushes the encoder.
 *
 * Any internal unencoded or queued frames are discarded.
 *
 * If this is called before framebuffers were added to the encoder instance,
 * then this function does nothing (unless the encoder does not use a
 * framebuffer pool).
 *
 * After calling this function, before calling imx_vpu_api_enc_encode(),
 * it is necessary to feed in a new frame with imx_vpu_api_enc_push_raw_frame(),
 * since, as mentioned above, any previously pushed/queued raw frames
 * also get discarded by the flush operation.
 *
 * @param encoder Encoder instance. Must not be NULL.
 */
void imx_vpu_api_enc_flush(ImxVpuApiEncoder *encoder);

/* Sets the current encoding bitrate to this new value, in kbps.
 *
 * Any raw frames that are pushed into the encoder after this was called
 * will get encoded with this bitrate.
 *
 * Switching to constant quantization mode on the fly is not supported,
 * so the minimum valid value is 1.
 *
 * If constant quantization mode was
 * enabled in the encoder's ImxVpuApiEncOpenParams, then calling this
 * function returns IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param bitrate New bitrate to use, in kbps. Must be at least 1.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: Function was called even
 * though rate control was disabled in the encoder's ImxVpuApiEncOpenParams.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_bitrate(ImxVpuApiEncoder *encoder, unsigned int bitrate);

/* Sets the current encoding frame rate to this new value, in kbps.
 *
 * Any raw frames that are pushed into the encoder after this was called
 * will get encoded with this frame rate.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param frame_rate_numerator Numerator of new frame rate to use.
 * @param frame_rate_denominator Denominator of new frame rate to use.
 *        Must be at least 1.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_frame_rate(ImxVpuApiEncoder *encoder, unsigned int frame_rate_numerator, unsigned int frame_rate_denominator);

/* Pushes a new raw frame to be encoded.
 *
 * This function needs to be called right after the encoder was opened and
 * the framebuffer pool was filled with enough framebuffers (unless the encoder
 * doesn't have a framebuffer pool).
 *
 * The incoming raw frames must conform to the framebuffer metrics specified
 * in the encoder stream info returned by imx_vpu_api_enc_get_stream_info().
 *
 * This function is not to be called when the drain mode is enabled.
 *
 * The ImxVpuApiRawFrame instance pointed to by raw_frame must remain
 * valid until imx_vpu_api_enc_encode() is called.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: Tried to call this before
 * the previously pushed raw frame was encoded.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR: Could not access the
 * raw frame's DMA buffer memory.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_push_raw_frame(ImxVpuApiEncoder *encoder, ImxVpuApiRawFrame const *raw_frame);

/* Performs an encoding step.
 *
 * This function is used in conjunction with imx_vpu_api_enc_push_raw_frame().
 * For correct encoding, the user has to first call imx_vpu_api_enc_push_raw_frame()
 * to feed in one raw frame, and then repeatedly call imx_vpu_api_enc_encode()
 * until at least one of these cases occur:
 *
 *   1. The IMX_VPU_API_ENC_OUTPUT_CODE_EOS output code
 *      is returned
 *   2. The IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED output code
 *      is returned
 *   3. A return code that indicates an error is returned
 *   4. imx_vpu_api_enc_flush() was called
 *
 * In cases #1 and #3, encoding cannot continue, and the encoder has to be
 * closed. In cases #2 and #4, imx_vpu_api_enc_push_raw_frame() must be
 * called again because the encoder ran out of data to encode. Afterwards, the
 * user can continue calling imx_vpu_api_enc_encode() again.
 *
 * In all other cases, the user is expected to act according to the returned
 * output code, and call imx_vpu_api_enc_encode() again. Also check the
 * ImxVpuApiEncOutputCodes documentation for details.
 *
 * If the output code is IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE,
 * the value pointed to by encoded_frame_size will be set to the size of the
 * encoded frame, in bytes. The user must then retrieve the encoded frame
 * by calling imx_vpu_api_enc_get_encoded_frame() and pass a buffer to that
 * function that is at least as large as the number of bytes that the value
 * pointed to by encoded_frame_size was set to.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param encoded_frame_size Pointer to a size_t value that will be set to the
 *        size of the encoded frame, in bytes. Must not be NULL.
 * @param output_code Pointer to an ImxVpuApiEncOutputCodes enum that will be
 *        set to the output code of this encoding step. Must not be NULL.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: A frame was previously encoded,
 * but imx_vpu_api_enc_get_encoded_frame() was not called before this function
 * was called again, or this function was called before framebuffers were added
 * to the encoder's framebuffer pool.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_TIMEOUT: VPU timeout occurred during encoding
 * because the hardware is already busy with some other operation and is not
 * available for encoding.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_encode(ImxVpuApiEncoder *encoder, size_t *encoded_frame_size, ImxVpuApiEncOutputCodes *output_code);

/* Get details about an encoded frame.
 *
 * This must not be called until imx_vpu_api_enc_encode() returns the output
 * code IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE . Once this
 * happens, the user *must* call this function. Encoding cannot continue
 * until the encoded frame is retrieved with this function. Also, this function
 * cannot be called more than once after that output code was returned by
 * the prior imx_vpu_api_enc_encode() call. This is because this function
 * modifies internal states.
 *
 * The data field in encoded_frame must be set to point to a memory block
 * that is at least as large as the encoded_frame_size return value from
 * the preceding imx_vpu_api_enc_encode() specified.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param encoded_frame Pointer to ImxVpuApiEncodedFrame structure to fill
 *        details  the decoded frame into. Must not be NULL. Its data field
 *        must be set to point to a memory block that the encoded frame data
 *        will be written into (see above).
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: Function was called before a
 * frame was encoded, or it was called more than once between encoding frames.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame);

/* Get details about an encoded frame.
 *
 * This works just like imx_vpu_api_enc_get_encoded_frame(), with an additional
 * return value that specifies whether this frame is a sync point, that is,
 * it can be used as a starting point for decoding. A sync point can be an
 * I or IDR frame, but also can be the start of an intra refresh interval.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param encoded_frame Pointer to ImxVpuApiEncodedFrame structure to fill
 *        details  the decoded frame into. Must not be NULL. Its data field
 *        must be set to point to a memory block that the encoded frame data
 *        will be written into (see above).
 * @param is_sync_point If non-NULL, points to an integer that is nonzero
 *        if this is a sync point, and zero otherwise.
 * @return Return code indicating the outcome. Valid values:
 *
 * IMX_VPU_API_ENC_RETURN_CODE_OK: Success.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_ERROR: Unspecified error. Consult log output.
 *
 * IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL: Function was called before a
 * frame was encoded, or it was called more than once between encoding frames.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame_ext(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame, int *is_sync_point);

/* Retrieves information about a skipped frame.
 *
 * This should only be called after imx_vpu_api_enc_decode() returned the output
 * code IMX_VPU_API_ENC_OUTPUT_CODE_FRAME_SKIPPED . Otherwise, this function's
 * return values are undefined.
 *
 * The context/pts/dts values are the ones from the ImxVpuApiRawFrame that
 * was skipped.
 *
 * @param encoder Encoder instance. Must not be NULL.
 * @param context Pointer to the context that shall be set to the context of the
 *        skipped frame. Can be NULL if not needed.
 * @param pts Pointer to the PTS that shall be set to the PTS of the
 *        skipped frame. Can be NULL if not needed.
 * @param dts Pointer to the DTS that shall be set to the DTS of the
 *        skipped frame. Can be NULL if not needed.
 */
ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_skipped_frame_info(ImxVpuApiEncoder *encoder, void **context, uint64_t *pts, uint64_t *dts);


#ifdef __cplusplus
}
#endif


#endif
