#include <config.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"
#include "imxvpuapi2_imx8m_hantro.h"




#ifdef IMXVPUAPI2_VPU_HAS_ENCODER




#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>

#include <imxdmabuffer/imxdmabuffer.h>

/* This is necessary to turn off these warning that originate in OMX_Core.h :
 *   "ISO C restricts enumerator values to range of ‘int’""    */
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "encoder/codec.h"
#include "encoder/encoder.h"
#include "encoder/encoder_h264.h"
#include "encoder/encoder_vp8.h"
#include "encoder/encoder.h"
#include "vsi_vendor_ext.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


/* Define the stream buffer size to be able to hold one big
 * uncompressed YUV 4:4:4 frame, plus some extra headroom.
 * This is more than what the encoder will ever produce,
 * which is needed to prevent the encoder from running out
 * of stream buffer memory. */
#define VPU_ENC_MIN_REQUIRED_STREAM_BUFFER_SIZE  (1920*1088*3 + 262144)
#define STREAM_BUFFER_PHYSADDR_ALIGNMENT         (0x10)
#define STREAM_BUFFER_SIZE_ALIGNMENT             (1024)
#define FRAME_WIDTH_ALIGNMENT                    (16)
#define FRAME_HEIGHT_ALIGNMENT                   (2)
/* It is not documented anywhere, but this port index
 * is used by the H1 encoder for output. */
#define OMX_H1_OUTPUT_PORT_INDEX                 (1)


static char const * codec_state_to_string(CODEC_STATE codec_state)
{
	switch (codec_state)
	{
		case CODEC_OK: return "CODEC_OK";
		case CODEC_CODED_INTRA: return "CODEC_CODED_INTRA";
		case CODEC_CODED_PREDICTED: return "CODEC_CODED_PREDICTED";
		case CODEC_CODED_SLICE: return "CODEC_CODED_SLICE";
		case CODEC_ERROR_HW_TIMEOUT: return "CODEC_ERROR_HW_TIMEOUT";
		case CODEC_ERROR_HW_BUS_ERROR: return "CODEC_ERROR_HW_BUS_ERROR";
		case CODEC_ERROR_HW_RESET: return "CODEC_ERROR_HW_RESET";
		case CODEC_ERROR_SYSTEM: return "CODEC_ERROR_SYS";
		case CODEC_ERROR_UNSPECIFIED: return "CODEC_ERROR_UNSPECIFIED";
		case CODEC_ERROR_RESERVED: return "CODEC_ERROR_RESERVED";
		case CODEC_ERROR_INVALID_ARGUMENT: return "CODEC_ERROR_INVALID_ARGUMENT";
		case CODEC_ERROR_BUFFER_OVERFLOW: return "CODEC_ERROR_BUFFER_OVERFLOW";
		case CODEC_ERROR_INVALID_STATE: return "CODEC_ERROR_INVALID_STATE";
		case CODEC_ERROR_UNSUPPORTED_SETTING: return "CODEC_ERROR_UNSUPPORTED_SETTING";
		default:
			return "<unknown>";
	}
}


/* We need to know the maximum number of allowed macroblocks per frame
 * per h.264 level for encoding, since the Hantro H1 encoder checks
 * for these. We then check in imx_vpu_api_enc_open() if the number
 * of macroblocks in the frames to be encoded is withing the limits
 * defined here. If not, a lower level is chosen. */

typedef struct
{
	ImxVpuApiH264Level level;
	size_t count;
}
H264MaxMacroblockCount;

/* The macroblock count figures are taken from the h.264 specification,
 * table A.1 "level limits". */
static H264MaxMacroblockCount const h264_max_macroblock_count_table[] =
{
	{ IMX_VPU_API_H264_LEVEL_1,   99 },
	{ IMX_VPU_API_H264_LEVEL_1B,  99 },
	{ IMX_VPU_API_H264_LEVEL_1_1, 396 },
	{ IMX_VPU_API_H264_LEVEL_1_2, 396 },
	{ IMX_VPU_API_H264_LEVEL_1_3, 396 },
	{ IMX_VPU_API_H264_LEVEL_2,   396 },
	{ IMX_VPU_API_H264_LEVEL_2_1, 792 },
	{ IMX_VPU_API_H264_LEVEL_2_2, 1620 },
	{ IMX_VPU_API_H264_LEVEL_3,   1620 },
	{ IMX_VPU_API_H264_LEVEL_3_1, 3600 },
	{ IMX_VPU_API_H264_LEVEL_3_2, 5120 },
	{ IMX_VPU_API_H264_LEVEL_4,   8192 },
	{ IMX_VPU_API_H264_LEVEL_4_1, 8192 },
	{ IMX_VPU_API_H264_LEVEL_4_2, 8704 },
	{ IMX_VPU_API_H264_LEVEL_5,   22080 },
	{ IMX_VPU_API_H264_LEVEL_5_1, 36864 }
	/* Profiles 5.2 to 6.2 are not supported by the Hantro H1 encoder */
};

static size_t const h264_max_macroblock_count_table_size = sizeof(h264_max_macroblock_count_table) / sizeof(H264MaxMacroblockCount);




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


struct _ImxVpuApiEncoder
{
	/* Hantro encoder that is in use. */
	ENCODER_PROTOTYPE *encoder;

	/* Stream buffer (called "bitstream buffer" in the VPU documentation).
	 * Holds data coming from the encoder. */
	ImxDmaBuffer *stream_buffer;
	/* Due to the way the Hantro encoder operates, we have to map the
	 * stream buffer until it is encoded and no longer staged.
	 * The mapped virtual address is stored in this field. */
	uint8_t *stream_buffer_virtual_address;
	/* Physical address of the stream buffer. Stored here to avoid
	 * redundant imx_dma_buffer_get_physical_address() calls. */
	imx_physical_address_t stream_buffer_physical_address;
	/* Size of the stream buffer, in bytes. Stored here to avoid
	 * redundant imx_dma_buffer_get_size() calls. */
	size_t stream_buffer_size;

	/* Copy of the open_params passed to imx_vpu_api_enc_open(). */
	ImxVpuApiEncOpenParams open_params;

	/* Stream information that is generated by imx_vpu_api_enc_open(). */
	ImxVpuApiEncStreamInfo stream_info;

	/* Hantro encoder configuration. This gets written to by the encoder as
	 * well. Also, changes to it (the bitrate for example) do influence the
	 * next frame encoding. */
	VIDEO_ENCODER_CONFIG encoder_config;

	/* Flag to keep track of the drain mode. That mode does not really
	 * exist with this encoder, since nothing queues up (frames that get
	 * pushed into the encoder are immediately output in encoded form).
	 * Still, for fulfilling the API requirement, keep the flag around. */
	BOOL drain_mode_enabled;

	/* h.264 SPS/PPS header data generated by the encoder. This is
	 * prepended to the main frame data if has_header is set to TRUE. */
	uint8_t *header_data;
	size_t header_data_size;

	/* TRUE if a header generated by the encoder is also included in the
	 * data of the encoded frame that will be output next. This is needed
	 * for setting the has_header field in ImxVpuApiEncEncodedFrame. */
	BOOL has_header;

	/* TRUE if the next frame shall be forcibly encoded as an I/IDR frame.
	 * This is used after flushing to make sure the next frame is an
	 * I frame. */
	BOOL force_I_frame;

	/* How many bytes of encoded frame data are currently stored in
	 * the stream buffer. This number is always less than or equal to
	 * stream_buffer_size. */
	size_t num_bytes_in_stream_buffer;

	/* The raw frame that is staged for encoding. */
	ImxVpuApiRawFrame staged_raw_frame;
	/* Due to the way the Hantro encoder operates, we have to map the
	 * raw frame's DMA buffer until it is encoded and no longer staged.
	 * The mapped virtual address is stored in this field. */
	uint8_t *staged_raw_frame_virtual_address;
	/* Physical address of the staged raw frame. Stored here to avoid
	 * redundant imx_dma_buffer_get_physical_address() calls. */
	imx_physical_address_t staged_raw_frame_physical_address;
	/* TRUE if a frame is staged, FALSE otherwise (the staged frame
	 * fields above are invalid if this is FALSE). */
	BOOL staged_raw_frame_set;

	/* Temporary value shared internally between imx_vpu_api_enc_encode()
	 * and imx_vpu_api_enc_get_encoded_frame(). It is shared because of
	 * the VP8 partition information inside. */
	STREAM_BUFFER encoding_stream;

	/* TRUE is an encoded frame is available, FALSE otherwise.
	 * If set to FALSE, then the fields below about the encoded frame
	 * are invalid. */
	BOOL encoded_frame_available;
	/* Context, PTS, DTS copied from the input raw frame. */
	void *encoded_frame_context;
	uint64_t encoded_frame_pts, encoded_frame_dts;
	/* What encoded frame type the input raw frame was encoded into.
	 * Filled in imx_vpu_api_enc_encode(). */
	ImxVpuApiFrameType encoded_frame_type;
	/* Size of the resulting encoded frame, in bytes. If a header
	 * was prepended, then its size is included in this. This value
	 * is used for setting the data_size field of the
	 * ImxVpuApiEncEncodedFrame structure when getting the encoded
	 * frame with imx_vpu_api_enc_get_encoded_frame(). */
	size_t encoded_frame_data_size;
};


/* Utility functions to fill various config structures. */
static void imx_vpu_api_enc_set_basic_encoder_config(ImxVpuApiEncoder *encoder);
static void imx_vpu_api_enc_set_common_encoder_config(ImxVpuApiEncoder *encoder, ENCODER_COMMON_CONFIG *common_config);
static void imx_vpu_api_enc_set_rate_control_config(ImxVpuApiEncoder *encoder, RATE_CONTROL_CONFIG *rate_control_config, unsigned int default_quant, unsigned int min_quant, unsigned int max_quant);
static void imx_vpu_api_enc_set_pre_processor_config(ImxVpuApiEncoder *encoder, PRE_PROCESSOR_CONFIG *pp_config);



/* Static, invariant global & compression format information. */

static ImxVpuApiCompressionFormat const enc_supported_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_VP8,
	IMX_VPU_API_COMPRESSION_FORMAT_H264
};

static ImxVpuApiEncGlobalInfo const enc_global_info = {
	.flags = IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_HAS_ENCODER | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_HANTRO,
	.min_required_stream_buffer_size = VPU_ENC_MIN_REQUIRED_STREAM_BUFFER_SIZE,
	.required_stream_buffer_physaddr_alignment = STREAM_BUFFER_PHYSADDR_ALIGNMENT,
	.required_stream_buffer_size_alignment = STREAM_BUFFER_SIZE_ALIGNMENT,
	.supported_compression_formats = enc_supported_compression_formats,
	.num_supported_compression_formats = sizeof(enc_supported_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};

ImxVpuApiEncGlobalInfo const * imx_vpu_api_enc_get_global_info(void)
{
	return &enc_global_info;
}


static ImxVpuApiColorFormat const enc_supported_basic_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT
};

static ImxVpuApiVP8SupportDetails const enc_vp8_support_details = {
	.parent = {
		.min_width = 132, .max_width = 1920,
		.min_height = 96, .max_height = 1088,
		.supported_color_formats = enc_supported_basic_color_formats,
		.num_supported_color_formats = sizeof(enc_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat),
		.min_quantization = 0, .max_quantization = 127
	},

	.supported_profiles = (1 << IMX_VPU_API_VP8_PROFILE_0)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_1)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_2)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_3)
};

static ImxVpuApiH264SupportDetails const enc_h264_support_details = {
	.parent = {
		.min_width = 132, .max_width = 1920,
		.min_height = 96, .max_height = 1088,
		.supported_color_formats = enc_supported_basic_color_formats,
		.num_supported_color_formats = sizeof(enc_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat),
		.min_quantization = 1, .max_quantization = 51
	},

	.max_constrained_baseline_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
	.max_baseline_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
	.max_main_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
	.max_high_profile_level = IMX_VPU_API_H264_LEVEL_5_1,
	.max_high10_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,

	.flags = 0
};


ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_enc_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&enc_vp8_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&enc_h264_support_details);

		default:
			return NULL;
	}

	return NULL;
}



void imx_vpu_api_enc_set_default_open_params(ImxVpuApiCompressionFormat compression_format, ImxVpuApiColorFormat color_format, size_t frame_width, size_t frame_height, ImxVpuApiEncOpenParams *open_params)
{
	assert(open_params != NULL);

	open_params->frame_width = frame_width;
	open_params->frame_height = frame_height;
	open_params->compression_format = compression_format;
	open_params->color_format = color_format;
	open_params->bitrate = 256;
	open_params->quantization = 0;
	open_params->gop_size = 16;
	open_params->min_intra_refresh_mb_count = 0;
	open_params->frame_rate_numerator = 25;
	open_params->frame_rate_denominator = 1;

	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			open_params->format_specific_params.h264_params.profile = IMX_VPU_API_H264_PROFILE_CONSTRAINED_BASELINE;
			open_params->format_specific_params.h264_params.level = IMX_VPU_API_H264_LEVEL_5_1;
			open_params->format_specific_params.h264_params.enable_access_unit_delimiters = 0;
			break;
		default:
			break;
	}
}


static void imx_vpu_api_enc_set_basic_encoder_config(ImxVpuApiEncoder *encoder)
{
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	BOOL use_rate_control = (open_params->bitrate != 0);
	VIDEO_ENCODER_CONFIG *encoder_config = &(encoder->encoder_config);

	/* The encoder's OpenMAX interface bits are written against OpenMax IL
	 * version 1.1.2.0, so set up all structures with that version number. */
#define INIT_OMX_ENC_CONFIG_PARAM_VERSION(param) \
	do \
	{ \
		(param).nSize = sizeof(param); \
		(param).nVersion.s.nVersionMajor = 0x1; \
		(param).nVersion.s.nVersionMinor = 0x1; \
		(param).nVersion.s.nRevision = 0x2; \
		(param).nVersion.s.nStep = 0x0; \
	} \
	while (0)
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->avc);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->avcIdr);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->deblocking);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->ec);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->bitrate);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->stab);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->videoQuantization);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->rotation);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->crop);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->intraRefreshVop);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->vp8);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->vp8Ref);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->adaptiveRoi);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->temporalLayer);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->intraArea);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->roi1Area);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->roi2Area);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->roi1DeltaQP);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->roi2DeltaQP);
	INIT_OMX_ENC_CONFIG_PARAM_VERSION(encoder_config->intraRefresh);
#undef INIT_OMX_ENC_CONFIG_PARAM_VERSION

	/* Set up rate control information. We only set up the bitrate and
	 * the rate control type here. The quantization factors (relevant
	 * if rate control is disabled) are defined per-format, since their
	 * valid range is format specific. */
	// TODO: Try out VBR, and if it is useful, add options for it.
	encoder_config->bitrate.eControlRate = use_rate_control ? OMX_Video_ControlRateConstant : OMX_Video_ControlRateDisable;
	/* We specify the bitrate in kbps, the encoder expects bps, so multiply by 1000. */
	encoder_config->bitrate.nTargetBitrate = open_params->bitrate * 1000;

	/* Make sure the crop rectangle encompasses the entire frame,
	 * excluding any extra padding rows/columns. */
	encoder_config->crop.nLeft = 0;
	encoder_config->crop.nTop = 0;
	encoder_config->crop.nWidth = open_params->frame_width;
	encoder_config->crop.nHeight = open_params->frame_height;

	/* Configure quantization level if constant-quantization encoding is used
	 * (meaning, no rate control). */
	if (!use_rate_control)
	{
		encoder_config->videoQuantization.nPortIndex = OMX_H1_OUTPUT_PORT_INDEX;
		encoder_config->videoQuantization.nQpI = open_params->quantization;
		encoder_config->videoQuantization.nQpP = open_params->quantization;
		encoder_config->videoQuantization.nQpB = 0; /* The H1 encoder does not use B frames. */
	}

	/* Configure intra refresh if it is enabled. The H1 encoder only supports
	 * cyclic refresh, so we don't bother with the adaptive refresh parameters. */
	if (open_params->min_intra_refresh_mb_count > 0)
	{
		encoder_config->intraRefresh.nPortIndex = OMX_H1_OUTPUT_PORT_INDEX;
		encoder_config->intraRefresh.eRefreshMode = OMX_VIDEO_IntraRefreshCyclic;
		encoder_config->intraRefresh.nCirMBs = open_params->min_intra_refresh_mb_count;
	}
}


static void imx_vpu_api_enc_set_common_encoder_config(ImxVpuApiEncoder *encoder, ENCODER_COMMON_CONFIG *common_config)
{
	double frame_rate;
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	ImxVpuApiFramebufferMetrics *fb_metrics = &(encoder->stream_info.frame_encoding_framebuffer_metrics);

	frame_rate = (double)(open_params->frame_rate_numerator) / (double)(open_params->frame_rate_denominator);

	/* The common config expects the _aligned_ width/height, that is, the
	 * dimensions _with_ the padding rows/columns included. */
	common_config->nOutputWidth = fb_metrics->aligned_frame_width;
	common_config->nOutputHeight = fb_metrics->aligned_frame_height;
	/* Encode the frame rate ratio in fixed-point 16.16 format. */
	common_config->nInputFramerate = FLOAT_Q16(frame_rate);
}


static void imx_vpu_api_enc_set_rate_control_config(ImxVpuApiEncoder *encoder, RATE_CONTROL_CONFIG *rate_control_config, unsigned int default_quant, unsigned int min_quant, unsigned int max_quant)
{
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	VIDEO_ENCODER_CONFIG *encoder_config = &(encoder->encoder_config);
	BOOL use_rate_control = (open_params->bitrate != 0);

	rate_control_config->nQpDefault = use_rate_control ? default_quant : open_params->quantization;
	rate_control_config->nQpMin = use_rate_control ? min_quant : open_params->quantization;
	rate_control_config->nQpMax = use_rate_control ? max_quant : open_params->quantization;
	rate_control_config->eRateControl = encoder_config->bitrate.eControlRate;
	rate_control_config->nTargetBitrate = encoder_config->bitrate.nTargetBitrate;
	switch (encoder_config->bitrate.eControlRate)
	{
		case OMX_Video_ControlRateVariable:
		case OMX_Video_ControlRateVariableSkipFrames:
			rate_control_config->nPictureRcEnabled = 1;
			rate_control_config->nMbRcEnabled = 1;
			rate_control_config->nHrdEnabled = 0;
			break;

		case OMX_Video_ControlRateConstant:
		case OMX_Video_ControlRateConstantSkipFrames:
			rate_control_config->nPictureRcEnabled = 1;
			rate_control_config->nMbRcEnabled = 1;
			rate_control_config->nHrdEnabled = 1;
			break;

		case OMX_Video_ControlRateDisable:
		default:
			rate_control_config->nPictureRcEnabled = 0;
			rate_control_config->nMbRcEnabled = 0;
			rate_control_config->nHrdEnabled = 0;
			break;
	}
}


static void imx_vpu_api_enc_set_pre_processor_config(ImxVpuApiEncoder *encoder, PRE_PROCESSOR_CONFIG *pp_config)
{
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	ImxVpuApiFramebufferMetrics *fb_metrics = &(encoder->stream_info.frame_encoding_framebuffer_metrics);

	/* The preprocessor config expects the _aligned_ width/height, that
	 * is, the dimensions _with_ the padding rows/columns included. */
	pp_config->origWidth = fb_metrics->aligned_frame_width;
	pp_config->origHeight = fb_metrics->aligned_frame_height;
	pp_config->xOffset = 0;
	pp_config->yOffset = 0;
	pp_config->angle = 0;
	pp_config->frameStabilization = OMX_FALSE;
	switch (open_params->color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
			pp_config->formatType = OMX_COLOR_FormatYUV420Planar;
			break;

		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
			pp_config->formatType = OMX_COLOR_FormatYUV420SemiPlanar;
			break;

		default:
			assert(FALSE);
	}
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_open(ImxVpuApiEncoder **encoder, ImxVpuApiEncOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	int err;
	ImxVpuApiEncReturnCodes ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
	ImxVpuApiFramebufferMetrics *fb_metrics;
	BOOL semi_planar;
	VIDEO_ENCODER_CONFIG *encoder_config;
	size_t stream_buffer_size;

	assert(encoder != NULL);
	assert(open_params != NULL);
	assert(stream_buffer != NULL);


	/* Check that the allocated stream buffer is big enough */
	{
		stream_buffer_size = imx_dma_buffer_get_size(stream_buffer);
		if (stream_buffer_size < VPU_ENC_MIN_REQUIRED_STREAM_BUFFER_SIZE) 
		{
			IMX_VPU_API_ERROR("stream buffer size is %zu bytes; need at least %zu bytes", stream_buffer_size, (size_t)VPU_ENC_MIN_REQUIRED_STREAM_BUFFER_SIZE);
			return IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE;
		}
	}


	/* Allocate encoder instance. */
	*encoder = malloc(sizeof(ImxVpuApiEncoder));
	assert((*encoder) != NULL);


	/* Set default encoder values. */
	memset(*encoder, 0, sizeof(ImxVpuApiEncoder));


	/* Map the stream buffer. We need to keep it mapped always so we can
	 * keep updating it. It is mapped as readwrite so we can shift data
	 * inside it later with memmove() if necessary. */
	(*encoder)->stream_buffer_virtual_address = imx_dma_buffer_map(stream_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE | IMX_DMA_BUFFER_MAPPING_FLAG_READ, &err);
	if ((*encoder)->stream_buffer_virtual_address == NULL)
	{
			IMX_VPU_API_ERROR("mapping stream buffer to virtual address space failed: %s (%d)", strerror(err), err);
			ret = IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
	}

	(*encoder)->stream_buffer_physical_address = imx_dma_buffer_get_physical_address(stream_buffer);
	(*encoder)->stream_buffer_size = stream_buffer_size;
	(*encoder)->stream_buffer = stream_buffer;


	/* Make a copy of the open_params for later use. */
	(*encoder)->open_params = *open_params;


	fb_metrics = &((*encoder)->stream_info.frame_encoding_framebuffer_metrics);

	fb_metrics->actual_frame_width = open_params->frame_width;
	fb_metrics->actual_frame_height = open_params->frame_height;
	fb_metrics->aligned_frame_width = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, FRAME_WIDTH_ALIGNMENT);
	fb_metrics->aligned_frame_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, FRAME_HEIGHT_ALIGNMENT);
	fb_metrics->y_stride = fb_metrics->aligned_frame_width;
	fb_metrics->y_size = fb_metrics->y_stride * fb_metrics->aligned_frame_height;

	semi_planar = imx_vpu_api_is_color_format_semi_planar(open_params->color_format);

	switch (open_params->color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride / 2;
			fb_metrics->uv_size = fb_metrics->y_size / 4;
			break;

		default:
			/* User specified an unknown format. */
			IMX_VPU_API_ERROR("unknown/unsupported color format %s (%d)", imx_vpu_api_color_format_string(open_params->color_format), open_params->color_format);
			ret = IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COLOR_FORMAT;
			goto cleanup;
	}

	/* Adjust the uv_stride and uv_size values in case we are using semi-planar chroma. */
	if (semi_planar)
	{
		fb_metrics->uv_stride *= 2;
		fb_metrics->uv_size *= 2;
	}

	fb_metrics->y_offset = 0;
	fb_metrics->u_offset = fb_metrics->y_size;
	fb_metrics->v_offset = fb_metrics->u_offset + fb_metrics->uv_size;

	/* The Hantro H1 encoder does not use a framebuffer pool, so set this to 0. */
	(*encoder)->stream_info.min_num_required_framebuffers = 0;
	(*encoder)->stream_info.min_framebuffer_size = (semi_planar ? fb_metrics->u_offset : fb_metrics->v_offset) + fb_metrics->uv_size;
	(*encoder)->stream_info.framebuffer_alignment = 1; // TODO
	(*encoder)->stream_info.frame_rate_numerator = open_params->frame_rate_numerator;
	(*encoder)->stream_info.frame_rate_denominator = open_params->frame_rate_denominator;


	/* Next, configure the encoder. This is split between setting up the
	 * encoder_config (which is accessed and updated during encoding)
	 * and the codec specifig configuration.
	 * Also, fill the stream_info's format_specific_params field here. */

	encoder_config = &((*encoder)->encoder_config);

	imx_vpu_api_enc_set_basic_encoder_config(*encoder);

	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
		{
			size_t i, macroblocks_per_frame;
			ImxVpuApiH264Level level;
			H264_CONFIG h264_config;
			OMX_VIDEO_PARAM_AVCTYPE *avc = &(encoder_config->avc);
			OMX_VIDEO_CONFIG_AVCINTRAPERIOD *avc_idr = &(encoder_config->avcIdr);
			OMX_PARAM_DEBLOCKINGTYPE *deblocking = &(encoder_config->deblocking);


			(*encoder)->stream_info.format_specific_params.h264_params = open_params->format_specific_params.h264_params;


			/* Make sure SPS and PPS NALUs are prepended to IDR frames to
			 * facilitate seeking as well as allowing the use of the
			 * encoded stream even in the middle of streaming (for example,
			 * because someone just started watching an h.264 live stream). */
			encoder_config->prependSPSPPSToIDRFrames = OMX_TRUE;


			/* Misc h.264 configuration. */

			avc->nPortIndex = OMX_H1_OUTPUT_PORT_INDEX;
			avc->nSliceHeaderSpacing = 0;
			avc->nPFrames = open_params->gop_size;
			avc->nBFrames = 0; /* The H1 encoder does not use B frames. */
			avc->bUseHadamard = OMX_FALSE; /* Use the regular 4x4 h.264 transform. */
			avc->nRefFrames = 1;
			avc->nRefIdx10ActiveMinus1 = 0;
			avc->nRefIdx11ActiveMinus1 = 0;
			avc->bEnableUEP = OMX_FALSE;
			avc->bEnableFMO = OMX_FALSE;
			avc->bEnableASO = OMX_FALSE;
			avc->bEnableRS = OMX_FALSE;
			avc->nAllowedPictureTypes = OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;
			avc->bFrameMBsOnly = OMX_FALSE;
			avc->bMBAFF = OMX_FALSE;
			avc->bEntropyCodingCABAC = OMX_FALSE;
			avc->bWeightedPPrediction = OMX_FALSE;
			avc->nWeightedBipredicitonMode = OMX_FALSE;
			avc->bconstIpred = OMX_FALSE;
			avc->bDirect8x8Inference = OMX_FALSE;
			avc->bDirectSpatialTemporal = OMX_FALSE;
			avc->nCabacInitIdc = OMX_FALSE;
			avc->eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;


			/* Configure the h.264 profile. */

			switch (open_params->format_specific_params.h264_params.profile)
			{
				case IMX_VPU_API_H264_PROFILE_CONSTRAINED_BASELINE:
					avc->eProfile = OMX_VIDEO_AVCProfileBaseline;
					break;

				case IMX_VPU_API_H264_PROFILE_BASELINE:
					avc->eProfile = OMX_VIDEO_AVCProfileBaseline;
					avc->bEnableFMO = OMX_TRUE;
					avc->bEnableASO = OMX_TRUE;
					avc->bEnableRS = OMX_TRUE;
					break;

				case IMX_VPU_API_H264_PROFILE_MAIN:
					avc->eProfile = OMX_VIDEO_AVCProfileMain;
					break;

				case IMX_VPU_API_H264_PROFILE_HIGH:
					avc->eProfile = OMX_VIDEO_AVCProfileHigh;
					break;

				default:
					/* User specified an unknown format. */
					IMX_VPU_API_ERROR("unknown/unsupported h.264 profile");
					ret = IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT_PARAMS;
					goto cleanup;
			}


			/* Configure the h.264 level. */

			level = open_params->format_specific_params.h264_params.level;

			/* Make sure the level that was specified is actually able to
			 * handle the number of macroblocks in the input frames. */
			macroblocks_per_frame = ((open_params->frame_width + 15) / 16) * ((open_params->frame_height + 15) / 16);
			for (i = 0; i < h264_max_macroblock_count_table_size; ++i)
			{
				H264MaxMacroblockCount const *level_mb_count_entry = &(h264_max_macroblock_count_table[i]);
				/* Look for the matching level entry. */
				if (level != level_mb_count_entry->level)
					continue;

				/* Now check if our macroblock count is allowed within that level. */
				if (macroblocks_per_frame <= level_mb_count_entry->count)
				{
					/* Macroblock count is acceptable within this level. We are okay. */
					break;
				}
				else if ((i + 1) < h264_max_macroblock_count_table_size)
				{
					/* Try the next level. */
					level = (level_mb_count_entry + 1)->level;
				}
				else
				{
					/* We tried all levels, and yet our macroblock count is too high. */
					IMX_VPU_API_ERROR("frame macroblock count is too high for the encoder; cannot encode");
					ret = IMX_VPU_API_ENC_RETURN_CODE_FRAMES_TOO_LARGE;
				}
			}

			if (open_params->format_specific_params.h264_params.level != level)
			{
				IMX_VPU_API_DEBUG(
					"adjusted h.264 level from %s to %s due to the frame macroblock count %zu not being supported by the originally specified level",
					imx_vpu_api_h264_level_string(open_params->format_specific_params.h264_params.level),
					imx_vpu_api_h264_level_string(level),
					macroblocks_per_frame
				);

				/* Store corrected level into the stream_info h264 params
				 * to inform the user about the change. */
				(*encoder)->stream_info.format_specific_params.h264_params.level = level;
			}

			switch (level)
			{
				case IMX_VPU_API_H264_LEVEL_1:   avc->eLevel = OMX_VIDEO_AVCLevel1;  break;
				case IMX_VPU_API_H264_LEVEL_1B:  avc->eLevel = OMX_VIDEO_AVCLevel1b; break;
				case IMX_VPU_API_H264_LEVEL_1_1: avc->eLevel = OMX_VIDEO_AVCLevel11; break;
				case IMX_VPU_API_H264_LEVEL_1_2: avc->eLevel = OMX_VIDEO_AVCLevel12; break;
				case IMX_VPU_API_H264_LEVEL_1_3: avc->eLevel = OMX_VIDEO_AVCLevel13; break;
				case IMX_VPU_API_H264_LEVEL_2:   avc->eLevel = OMX_VIDEO_AVCLevel2;  break;
				case IMX_VPU_API_H264_LEVEL_2_1: avc->eLevel = OMX_VIDEO_AVCLevel21; break;
				case IMX_VPU_API_H264_LEVEL_2_2: avc->eLevel = OMX_VIDEO_AVCLevel22; break;
				case IMX_VPU_API_H264_LEVEL_3:   avc->eLevel = OMX_VIDEO_AVCLevel3;  break;
				case IMX_VPU_API_H264_LEVEL_3_1: avc->eLevel = OMX_VIDEO_AVCLevel31; break;
				case IMX_VPU_API_H264_LEVEL_3_2: avc->eLevel = OMX_VIDEO_AVCLevel32; break;
				case IMX_VPU_API_H264_LEVEL_4:   avc->eLevel = OMX_VIDEO_AVCLevel4;  break;
				case IMX_VPU_API_H264_LEVEL_4_1: avc->eLevel = OMX_VIDEO_AVCLevel41; break;
				case IMX_VPU_API_H264_LEVEL_4_2: avc->eLevel = OMX_VIDEO_AVCLevel42; break;
				case IMX_VPU_API_H264_LEVEL_5:   avc->eLevel = OMX_VIDEO_AVCLevel5;  break;
				case IMX_VPU_API_H264_LEVEL_5_1: avc->eLevel = OMX_VIDEO_AVCLevel51; break;
				default:
					/* User specified an unknown format. */
					IMX_VPU_API_ERROR("unknown/unsupported h.264 level");
					ret = IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT_PARAMS;
					goto cleanup;
			}


			/* Configure the encoder to emit an IDR frame at the beginning of each GOP. */
			avc_idr->nPFrames = open_params->gop_size;
			avc_idr->nIDRPeriod = open_params->gop_size;

			/* Enable in-loop deblocking for better encoding. */
			deblocking->nPortIndex = OMX_H1_OUTPUT_PORT_INDEX;
			deblocking->bDeblocking = OMX_TRUE;


			/* Now setup the h.264 specific configuration. In this step, some config
			 * details are copied from the generic config that was initialized above. */

			memset(&h264_config, 0, sizeof(h264_config));

			h264_config.h264_config.eProfile = avc->eProfile;
			h264_config.h264_config.eLevel = avc->eLevel;
			h264_config.nPFrames = avc->nPFrames;
			h264_config.bDisableDeblocking = !(deblocking->bDeblocking);

			imx_vpu_api_enc_set_common_encoder_config(*encoder, &(h264_config.common_config));
			imx_vpu_api_enc_set_pre_processor_config(*encoder, &(h264_config.pp_config));
			/* Valid h.264 quantization range is 0-51. Set a default quantization value
			 * of 31 for cases when rate control is enabled (the quantization 31 is then
			 * used at the beginning of the encoding process). The default quantization
			 * value is ignored if rate control is disabled. */
			imx_vpu_api_enc_set_rate_control_config(*encoder, &(h264_config.rate_config), 31, 0, 51);


			/* Now create the h.264 encoder. */
			(*encoder)->encoder = HantroHwEncOmx_encoder_create_h264(&h264_config);
			if ((*encoder)->encoder == NULL)
			{
				IMX_VPU_API_ERROR("could not create h.264 encoder");
				ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
				goto cleanup;
			}


			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
		{
			VP8_CONFIG vp8_config;
			OMX_VIDEO_PARAM_VP8TYPE *vp8 = &(encoder_config->vp8);
			OMX_VIDEO_VP8REFERENCEFRAMETYPE* vp8_ref = &(encoder_config->vp8Ref);


			(*encoder)->stream_info.format_specific_params.vp8_params = open_params->format_specific_params.vp8_params;


			/* Misc VP8 configuration. */

			vp8->nPortIndex = OMX_H1_OUTPUT_PORT_INDEX;
			/* Pick the main profile. This is described in the OpenMAX IL
			 * VP8 extension specification. The other allowed values do
			 * not make sense here, so just use the main profile. */
			vp8->eProfile = OMX_VIDEO_VP8ProfileMain;
			/* Use profile #0, which implies bicubic filtering and a "normal"
			 * loop filter. See RFC 6386 section 9.1 for details.
			 * Confusingly, the OpenMAX IL VP8 extension specification
			 * does not refer to this as a profile, and instead uses this
			 * term for something else (see above). */
			vp8->eLevel = OMX_VIDEO_VP8Level_Version0;
			/* TODO: DCT partitions are used for multithreaded decoding. We do
			 * not yet support this feature. */
			vp8->nDCTPartitions = 0;
			/* TODO: Error resilient mode is mainly useful for video telephony.
			 * Not used yet. */
			vp8->bErrorResilientMode = OMX_FALSE;

			vp8_ref->bPreviousFrameRefresh = OMX_TRUE;
			vp8_ref->bUsePreviousFrame = OMX_TRUE;

			/* We do not support golden or alternate frames. */
			vp8_ref->bGoldenFrameRefresh = OMX_FALSE;
			vp8_ref->bUseGoldenFrame = OMX_FALSE;
			vp8_ref->bAlternateFrameRefresh = OMX_FALSE;
			vp8_ref->bUseAlternateFrame = OMX_FALSE;


			/* Now setup the VP8 specific configuration. In this step, some config
			 * details are copied from the generic config that was initialized above. */

			memset(&vp8_config, 0, sizeof(vp8_config));

			vp8_config.vp8_config.eProfile = vp8->eProfile;
			vp8_config.vp8_config.eLevel = vp8->eLevel;
			vp8_config.vp8_config.nDCTPartitions = vp8->nDCTPartitions;
			vp8_config.vp8_config.bErrorResilientMode = vp8->bErrorResilientMode;

			imx_vpu_api_enc_set_common_encoder_config(*encoder, &(vp8_config.common_config));
			imx_vpu_api_enc_set_pre_processor_config(*encoder, &(vp8_config.pp_config));
			/* Valid h.264 quantization range is 0-127. Set a default quantization value
			 * of 26 for cases when rate control is enabled (the quantization 26 is then
			 * used at the beginning of the encoding process). The default quantization
			 * value is ignored if rate control is disabled. */
			imx_vpu_api_enc_set_rate_control_config(*encoder, &(vp8_config.rate_config), 26, 0, 127);


			/* Now create the VP8 encoder. */
			(*encoder)->encoder = HantroHwEncOmx_encoder_create_vp8(&vp8_config);
			if ((*encoder)->encoder == NULL)
			{
				IMX_VPU_API_ERROR("could not create VP8 encoder");
				ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
				goto cleanup;
			}


			break;
		}

		default:
		IMX_VPU_API_ERROR("invalid/unsupported compression format %s", imx_vpu_api_compression_format_string(open_params->compression_format));
			ret = IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT;
			goto cleanup;
	}


	/* Start the stream by producing bitstream headers. */

	{
		CODEC_STATE codec_state;
		/* NOTE: This encoding_stream structure is unrelated to the one shared
		 * between imx_vpu_api_enc_encode() and imx_vpu_api_enc_get_encoded_frame(). */
		STREAM_BUFFER encoding_stream;

		memset(&encoding_stream, 0, sizeof(encoding_stream));
		encoding_stream.bus_data = (OMX_U8 *)((*encoder)->stream_buffer_virtual_address);
		encoding_stream.bus_address = (OSAL_BUS_WIDTH)((*encoder)->stream_buffer_physical_address);
		encoding_stream.buf_max_size = (*encoder)->stream_buffer_size;

		/* The stream_start() function takes care of generating the headers
		 * and writing them into the stream buffer. */
		codec_state = (*encoder)->encoder->stream_start((*encoder)->encoder, &encoding_stream);
		if (codec_state != CODEC_OK)
		{
			IMX_VPU_API_ERROR("could not start encoded stream: %s (%d)", codec_state_to_string(codec_state), codec_state);
			ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
			goto finish;
		}

		/* Copy the header data so we can insert it later if necessary. */
		(*encoder)->header_data = malloc(encoding_stream.streamlen);
		memcpy((*encoder)->header_data, (*encoder)->stream_buffer_virtual_address, encoding_stream.streamlen);
		(*encoder)->header_data_size = encoding_stream.streamlen;

		(*encoder)->has_header = TRUE;
	}


	/* Finish & cleanup. */
finish:
	if (ret == IMX_VPU_API_ENC_RETURN_CODE_OK)
		IMX_VPU_API_DEBUG("successfully opened encoder");

	return ret;

cleanup:
	if ((*encoder) != NULL)
	{
		if ((*encoder)->stream_buffer_virtual_address != NULL)
			imx_dma_buffer_unmap((*encoder)->stream_buffer);
		free(*encoder);
		*encoder = NULL;
	}

	goto finish;
}


void imx_vpu_api_enc_close(ImxVpuApiEncoder *encoder)
{
	assert(encoder != NULL);

	IMX_VPU_API_DEBUG("closing encoder");

	if (encoder->staged_raw_frame_set)
		imx_dma_buffer_unmap(encoder->staged_raw_frame.fb_dma_buffer);

	if (encoder->encoder != NULL)
		encoder->encoder->destroy(encoder->encoder);

	if (encoder->stream_buffer != NULL)
		imx_dma_buffer_unmap(encoder->stream_buffer);

	free(encoder->header_data);

	free(encoder);
}


ImxVpuApiEncStreamInfo const * imx_vpu_api_enc_get_stream_info(ImxVpuApiEncoder *encoder)
{
	assert(encoder != NULL);
	return &(encoder->stream_info);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_add_framebuffers_to_pool(ImxVpuApiEncoder *encoder, ImxDmaBuffer **fb_dma_buffers, size_t num_framebuffers)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffers);
	IMX_VPU_API_UNUSED_PARAM(num_framebuffers);
	IMX_VPU_API_ERROR("tried to add framebuffers, but this encoder does not use a framebuffer pool");
	return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
}


void imx_vpu_api_enc_enable_drain_mode(ImxVpuApiEncoder *encoder)
{
	/* The H1 encoder does not really have a drain mode. We simply simulate
	 * it by storing the drain_mode_enabled flag. Aside from using it to
	 * be able to return something in imx_vpu_api_enc_is_drain_mode_enabled(),
	 * we use this for checks in imx_vpu_api_enc_push_raw_frame() and
	 * imx_vpu_api_enc_encode(). */
	assert(encoder != NULL);
	encoder->drain_mode_enabled = TRUE;
}


int imx_vpu_api_enc_is_drain_mode_enabled(ImxVpuApiEncoder *encoder)
{
	assert(encoder != NULL);
	return encoder->drain_mode_enabled;
}


void imx_vpu_api_enc_flush(ImxVpuApiEncoder *encoder)
{
	/* Force the first frame after the flush to be an intra frame. This
	 * makes sure that decoders can show a video signal right away
	 * after the encoder got flushed. */
	encoder->force_I_frame = TRUE;
	encoder->staged_raw_frame_set = FALSE;
	encoder->encoded_frame_available = FALSE;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_bitrate(ImxVpuApiEncoder *encoder, unsigned int bitrate)
{
	if (encoder->open_params.bitrate == 0)
	{
		IMX_VPU_API_ERROR("rate control disabled in the imx_vpu_api_enc_open() parameters");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	IMX_VPU_API_TRACE("setting bitrate to %u kbps");

	/* We specify the bitrate in kbps, the encoder expects bps, so multiply by 1000. */
	encoder->encoder_config.bitrate.nTargetBitrate = bitrate * 1000;

	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_push_raw_frame(ImxVpuApiEncoder *encoder, ImxVpuApiRawFrame const *raw_frame)
{
	int err;

	assert(encoder != NULL);
	assert(raw_frame != NULL);

	/* Pushing a frame into the encoder makes no sense if drain mode is on. */
	if (encoder->drain_mode_enabled)
	{
		IMX_VPU_API_ERROR("tried to push a raw frame after drain mode was enabled");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	if (encoder->staged_raw_frame_set)
	{
		IMX_VPU_API_ERROR("tried to push a raw frame before a previous one was encoded");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	IMX_VPU_API_DEBUG("staged raw frame");

	/* Stage the raw frame. We cannot use it here right away, since the CODA
	 * encoder has no separate function to push raw frames into it. Instead,
	 * just keep track of it here, and actually use it in imx_vpu_api_enc_encode(). */
	encoder->staged_raw_frame = *raw_frame;
	encoder->staged_raw_frame_physical_address = imx_dma_buffer_get_physical_address(encoder->staged_raw_frame.fb_dma_buffer);
	encoder->staged_raw_frame_virtual_address = imx_dma_buffer_map(encoder->staged_raw_frame.fb_dma_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_READ | IMX_DMA_BUFFER_MAPPING_FLAG_WRITE, &err);
	if (encoder->staged_raw_frame_virtual_address == NULL)
	{
		IMX_VPU_API_ERROR("could not map the raw frame's DMA buffer: %s (%d)", strerror(err), err);
		return IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
	}

	encoder->staged_raw_frame_set = TRUE;

	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_encode(ImxVpuApiEncoder *encoder, size_t *encoded_frame_size, ImxVpuApiEncOutputCodes *output_code)
{
	ImxVpuApiEncReturnCodes ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	STREAM_BUFFER *encoding_stream = &(encoder->encoding_stream);
	FRAME frame;
	CODEC_STATE codec_state;
	BOOL forced_I_frame;

	assert(encoder != NULL);
	assert(encoded_frame_size != NULL);
	assert(output_code != NULL);

	/* As explained, the H1 encoder does not really have a "drain mode", since
	 * frames are immediately encoded (there are no queued frames inside the
	 * encoder). For this reason, once the simulated "drain mode" is enabled,
	 * the encoder has nothing left to encode. */
	if (encoder->drain_mode_enabled)
	{
		*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_EOS;
		ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
		goto finish;
	}

	if (encoder->encoded_frame_available)
	{
		IMX_VPU_API_ERROR("cannot encode new frame before the old one was retrieved");
		ret = IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
		goto finish;
	}

	if (!(encoder->staged_raw_frame_set))
	{
		IMX_VPU_API_TRACE("no data left to encode");
		*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
		ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
		goto finish;
	}

	*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE;


	IMX_VPU_API_LOG("encoding raw_frame with physical address %" IMX_PHYSICAL_ADDRESS_FORMAT, encoder->staged_raw_frame_physical_address);


	/* In here, we set up the necessary encoding parameters and count the
	 * number of bytes that will make up the encoded frame. We do _not_
	 * retrieve the encoded frame here right away. The encoder writes the
	 * encoded frame's bytes into the stream buffer. We record its length
	 * here and set encoded_frame_size to that count to allow the user to
	 * allocate a buffer that is big enough for that encoded frame. Then,
	 * in imx_vpu_api_enc_get_encoded_frame(), we actually retrieve the
	 * encoded frame data.
	 * (Also, a header is prepended there if necessary.) */

	*encoded_frame_size = 0;
	encoder->num_bytes_in_stream_buffer = 0;

	if (encoder->has_header)
	{
		IMX_VPU_API_LOG("header needs %zu byte", encoder->header_data_size);
		*encoded_frame_size += encoder->header_data_size;
	}

	forced_I_frame = ((encoder->staged_raw_frame.frame_types[0] & IMX_VPU_API_FRAME_TYPE_I) || (encoder->staged_raw_frame.frame_types[0] & IMX_VPU_API_FRAME_TYPE_IDR))
	              || (encoder->force_I_frame);

	memset(&frame, 0, sizeof(frame));
	memset(encoding_stream, 0, sizeof(STREAM_BUFFER));

	encoding_stream->bus_data = (OMX_U8 *)(encoder->stream_buffer_virtual_address);
	encoding_stream->bus_address = (OSAL_BUS_WIDTH)(encoder->stream_buffer_physical_address);
	encoding_stream->buf_max_size = encoder->stream_buffer_size;

	frame.fb_bus_data = encoder->staged_raw_frame_virtual_address;
	frame.fb_bus_address = encoder->staged_raw_frame_physical_address;
	frame.frame_type = forced_I_frame ? INTRA_FRAME : PREDICTED_FRAME;
	frame.bitrate = encoder->encoder_config.bitrate.nTargetBitrate;


	/* Perform the actual encoding. */

	codec_state = encoder->encoder->encode(encoder->encoder, &frame, encoding_stream, &(encoder->encoder_config));
	switch (codec_state)
	{
		case CODEC_OK:
		case CODEC_CODED_SLICE:
			break;

		case CODEC_CODED_INTRA:
			encoder->encoded_frame_type = IMX_VPU_API_FRAME_TYPE_I;
			break;

		case CODEC_CODED_PREDICTED:
			encoder->encoded_frame_type = IMX_VPU_API_FRAME_TYPE_P;
			break;

		default:
			IMX_VPU_API_ERROR("could not encode frame: %s (%d)", codec_state_to_string(codec_state), codec_state);
			ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
			goto finish;
	}


	/* Count the number of bytes of the encoded frame. This differs between
	 * formats. h.264 is encoded as one contiguous dataset, while the encoded
	 * VP8 data is spread amongst partitions that have to be accessed
	 * individually. */

	if (open_params->compression_format ==  IMX_VPU_API_COMPRESSION_FORMAT_VP8)
	{
		unsigned int i;
		for (i = 0;  i < 9; ++i)
		{
			IMX_VPU_API_LOG("VP8 partition #%u contains %u byte", i, (unsigned int)(encoding_stream->streamSize[i]));
			encoder->num_bytes_in_stream_buffer += encoding_stream->streamSize[i];
		}
	}
	else
		encoder->num_bytes_in_stream_buffer += encoding_stream->streamlen;

	*encoded_frame_size += encoder->num_bytes_in_stream_buffer;
	assert(encoder->num_bytes_in_stream_buffer <= encoding_stream->buf_max_size);
	IMX_VPU_API_LOG("encoded frame (excluding any header) has a size of %u byte", (unsigned int)(encoder->num_bytes_in_stream_buffer));


	/* Copy over metadata from the raw frame to the encoded frame. Since the
	 * encoder does not perform any kind of delay or reordering, this is
	 * appropriate, because in that case, one input frame always immediately
	 * leads to one output frame. */
	encoder->encoded_frame_context = encoder->staged_raw_frame.context;
	encoder->encoded_frame_pts = encoder->staged_raw_frame.pts;
	encoder->encoded_frame_dts = encoder->staged_raw_frame.dts;
	encoder->encoded_frame_data_size = *encoded_frame_size;
	encoder->encoded_frame_available = TRUE;

	encoder->force_I_frame = FALSE;

	*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE;


finish:
	imx_dma_buffer_unmap(encoder->staged_raw_frame.fb_dma_buffer);
	encoder->staged_raw_frame_set = FALSE;
	return ret;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	ImxVpuApiEncOpenParams *open_params = &(encoder->open_params);
	uint8_t *encoded_data = encoded_frame->data;
	STREAM_BUFFER *encoding_stream = &(encoder->encoding_stream);

	assert(encoder != NULL);
	assert(encoded_frame != NULL);
	assert(encoded_frame->data != NULL);

	if (!(encoder->encoded_frame_available))
	{
		IMX_VPU_API_ERROR("cannot retrieve encoded frame since there is none");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	if (encoder->has_header)
	{
		memcpy(encoded_data, encoder->header_data, encoder->header_data_size);
		encoded_data += encoder->header_data_size;
	}


	/* As explained in imx_vpu_api_enc_encode(), h.264 data can be accessed
	 * in one go, while VP8 data is spread amongst partitions that we have
	 * to access individually. */

	if (open_params->compression_format ==  IMX_VPU_API_COMPRESSION_FORMAT_VP8)
	{
		unsigned int i;
		for (i = 0;  i < 9; ++i)
		{
			if (encoding_stream->streamSize[i] == 0)
				continue;

			memcpy(encoded_data, encoding_stream->pOutBuf[i], encoding_stream->streamSize[i]);
			encoded_data += encoding_stream->streamSize[i];
		}
	}
	else
		memcpy(encoded_data, encoder->stream_buffer_virtual_address, encoder->num_bytes_in_stream_buffer);


	/* Copy encoded frame metadata. */

	encoded_frame->data_size = encoder->encoded_frame_data_size;
	encoded_frame->has_header = encoder->has_header;
	encoded_frame->frame_type = encoder->encoded_frame_type;
	encoded_frame->context = encoder->encoded_frame_context;
	encoded_frame->pts = encoder->encoded_frame_pts;
	encoded_frame->dts = encoder->encoded_frame_dts;


	/* Reset some flags for the next imx_vpu_api_enc_encode() call,
	 * since we are done with this frame. */
	encoder->encoded_frame_available = FALSE;
	encoder->num_bytes_in_stream_buffer = 0;
	encoder->has_header = FALSE;


	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}




#else /* not IMXVPUAPI2_VPU_HAS_ENCODER */




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




#endif /* IMXVPUAPI2_VPU_HAS_ENCODER */
