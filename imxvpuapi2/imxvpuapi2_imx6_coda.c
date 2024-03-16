#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>

#include <config.h>

#include <imxdmabuffer/imxdmabuffer.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"
#include "imxvpuapi2_imx6_coda_ipu.h"

#include <vpu_lib.h>
#include <vpu_io.h>




/******************************************************/
/******* MISCELLANEOUS STRUCTURES AND FUNCTIONS *******/
/******************************************************/


#define VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE          (1024*1024*3)
#define VPU_ENC_MAIN_BITSTREAM_BUFFER_SIZE          (1024*1024*1)
#define VPU_ENC_MPEG4_SCRATCH_SIZE                  (0x080000)
#define VPU_MAX_SLICE_BUFFER_SIZE                   (1920*1088*15/20)
#define VPU_PS_SAVE_BUFFER_SIZE                     (1024*512)
#define VPU_VP8_MB_PRED_BUFFER_SIZE                 (68*(1920*1088/256))
#define BITSTREAM_BUFFER_PHYSADDR_ALIGNMENT         (512)
#define BITSTREAM_BUFFER_SIZE_ALIGNMENT             (1024)
#define FRAME_PHYSADDR_ALIGNMENT                    (4096)

/* The decoder's bitstream buffer shares space with other fields,
 * to not have to allocate several DMA blocks. The actual bitstream buffer is called
 * the "main bitstream buffer". It makes up all bytes from the start of the buffer
 * and is VPU_MAIN_BITSTREAM_BUFFER_SIZE large. Bytes beyond that are codec format
 * specific data. */
#define VPU_DEC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE  (VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE + VPU_MAX_SLICE_BUFFER_SIZE + VPU_PS_SAVE_BUFFER_SIZE)

#define VPU_ENC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE  (VPU_ENC_MAIN_BITSTREAM_BUFFER_SIZE + VPU_ENC_MPEG4_SCRATCH_SIZE)

#define VPU_ENC_NUM_EXTRA_SUBSAMPLE_FRAMEBUFFERS    (2)

#define VPU_WAIT_TIMEOUT                            (500) /* milliseconds to wait for frame completion */
#define VPU_MAX_TIMEOUT_COUNTS                      (4)   /* how many timeouts are allowed in series */

#define JPEG_ENC_HEADER_DATA_MAX_SIZE  2048


/* Apparently, the minFrameBufferCount field in the VPU initial info is sometimes
 * incorrect, especially with main/high profile h.264 data and a high degree of
 * frame reordering. Cases have been observed where insufficient framebuffers were
 * reported even though enough framebuffers were registered. To work around that
 * problem, we increase the minimum framebuffer count by this constant. */
#define NUM_EXTRA_FRAMEBUFFERS_REQUIRED             (4)


/* Component tables, used by imx-vpu to fill in JPEG SOF headers as described
 * by the JPEG specification section B.2.2 and to pick the correct quantization
 * tables. There are 5 tables, one for each supported pixel format. Each table
 * contains 4 row, each row is made up of 6 byte.
 *
 * The row structure goes as follows:
 * - The first byte is the number of the component.
 * - The second and third bytes contain, respectively, the vertical and
 *   horizontal sampling factors, which are either 1 or 2. Chroma components
 *   always use sampling factors of 1, while the luma component can contain
 *   factors of value 2. For example, a horizontal luma factor of 2 means
 *   that horizontally, for a pair of 2 Y pixels there is one U and one
 *   V pixel.
 * - The fourth byte is the index of the quantization table to use. The
 *   luma component uses the table with index 0, the chroma components use
 *   the table with index 1. These indices correspond to the DC_TABLE_INDEX0
 *   and DC_TABLE_INDEX1 constants.
 *
 * Note that the fourth component row is currently unused. So are the fifth
 * and sixth bytes in each row. Still, the imx-vpu library API requires them
 * (possibly for future extensions?).
 */

static uint8_t const jpeg_enc_component_info_tables[5][4 * 6] =
{
	/* YUV 4:2:0 table. For each 2x2 patch of Y pixels,
	 * there is one U and one V pixel. */
	{ 0x00, 0x02, 0x02, 0x00, 0x00, 0x00,
	  0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x02, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },

	/* YUV 4:2:2 horizontal table. For each horizontal pair
	 * of 2 Y pixels, there is one U and one V pixel. */
	{ 0x00, 0x02, 0x01, 0x00, 0x00, 0x00,
	  0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x02, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },

	/* YUV 4:2:2 vertical table. For each vertical pair
	 * of 2 Y pixelsoz, there is one U and one V pixel. */
	{ 0x00, 0x01, 0x02, 0x00, 0x00, 0x00,
	  0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x02, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },

	/* YUV 4:4:4 table. For each Y pixel, there is one
	 * U and one V pixel. */
	{ 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
	  0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x02, 0x01, 0x01, 0x01, 0x01, 0x01,
	  0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },

	/* YUV 4:0:0 table. There are only Y pixels, no U or
	 * V ones. This is essentially grayscale. */
	{ 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
	  0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }
};


/* These Huffman tables correspond to the default tables inside the VPU library */

static uint8_t const jpeg_enc_huffman_bits_luma_dc[16] =
{
	0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t const jpeg_enc_huffman_bits_luma_ac[16] =
{
	0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
	0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D
};

static uint8_t const jpeg_enc_huffman_bits_chroma_dc[16] =
{
	0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t const jpeg_enc_huffman_bits_chroma_ac[16] =
{
	0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
	0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77
};

static uint8_t const jpeg_enc_huffman_value_luma_dc[12] =
{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B
};

static uint8_t const jpeg_enc_huffman_value_luma_ac[162] =
{
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
	0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
	0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
	0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
	0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
	0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
	0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
	0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
	0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
	0xF9, 0xFA
};

static uint8_t const jpeg_enc_huffman_value_chroma_dc[12] =
{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B
};

static uint8_t const jpeg_enc_huffman_value_chroma_ac[162] =
{
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
	0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
	0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
	0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
	0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
	0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
	0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
	0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
	0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
	0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
	0xF9, 0xFA
};




/* These functions are needed for (un)loading the CODA VPU firmware. */

static size_t vpu_init_inst_counter = 0;
static pthread_mutex_t vpu_init_inst_mutex = PTHREAD_MUTEX_INITIALIZER;


static BOOL imx_coda_vpu_load(void)
{
	BOOL retval = TRUE;

	pthread_mutex_lock(&vpu_init_inst_mutex);

	IMX_VPU_API_LOG("VPU init instance counter: %lu", vpu_init_inst_counter);

	if (vpu_init_inst_counter != 0)
	{
		++vpu_init_inst_counter;
		goto finish;
	}
	else
	{
		IMX_VPU_API_INFO("libimxvpuapi version %s vpulib backend", IMXVPUAPI2_VERSION);

		if (vpu_Init(NULL) == RETCODE_SUCCESS)
		{
			IMX_VPU_API_DEBUG("loaded VPU");
			++vpu_init_inst_counter;
			goto finish;
		}
		else
		{
			IMX_VPU_API_ERROR("loading VPU failed");
			retval = FALSE;
			goto finish;
		}
	}

finish:
	pthread_mutex_unlock(&vpu_init_inst_mutex);
	return retval;
}


static void imx_coda_vpu_unload(void)
{
	pthread_mutex_lock(&vpu_init_inst_mutex);

	IMX_VPU_API_LOG("VPU init instance counter: %lu", vpu_init_inst_counter);

	if (vpu_init_inst_counter != 0)
	{
		--vpu_init_inst_counter;

		if (vpu_init_inst_counter == 0)
		{
			vpu_UnInit();
			IMX_VPU_API_DEBUG("unloaded VPU");
		}
	}

	pthread_mutex_unlock(&vpu_init_inst_mutex);
}




/* Functions for converting CODA VPU specific values into imxvpuapi enums. */


static void convert_frame_type(ImxVpuApiCompressionFormat compression_format, int vpu_pic_type, BOOL interlaced, ImxVpuApiFrameType *frame_types)
{
	ImxVpuApiFrameType type = IMX_VPU_API_FRAME_TYPE_UNKNOWN;

	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
			/* This assumes progressive content and sets both frame
			 * types to the same value. WMV3 *does* have support for
			 * interlacing, but it has never been documented, and was
			 * deprecated by Microsoft in favor of VC-1, which officially
			 * has proper interlacing support. */
			switch (vpu_pic_type & 0x07)
			{
				case 0: type = IMX_VPU_API_FRAME_TYPE_I; break;
				case 1: type = IMX_VPU_API_FRAME_TYPE_P; break;
				case 2: type = IMX_VPU_API_FRAME_TYPE_BI; break;
				case 3: type = IMX_VPU_API_FRAME_TYPE_B; break;
				case 4: type = IMX_VPU_API_FRAME_TYPE_SKIP; break;
				default: break;
			}
			frame_types[0] = frame_types[1] = type;
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		{
			int i;
			int vpu_pic_types[2];

			if (interlaced)
			{
				vpu_pic_types[0] = (vpu_pic_type >> 0) & 0x7;
				vpu_pic_types[1] = (vpu_pic_type >> 3) & 0x7;
			}
			else
			{
				vpu_pic_types[0] = (vpu_pic_type >> 0) & 0x7;
				vpu_pic_types[1] = (vpu_pic_type >> 0) & 0x7;
			}

			for (i = 0; i < 2; ++i)
			{
				switch (vpu_pic_types[i])
				{
					case 0: frame_types[i] = IMX_VPU_API_FRAME_TYPE_I; break;
					case 1: frame_types[i] = IMX_VPU_API_FRAME_TYPE_P; break;
					case 2: frame_types[i] = IMX_VPU_API_FRAME_TYPE_BI; break;
					case 3: frame_types[i] = IMX_VPU_API_FRAME_TYPE_B; break;
					case 4: frame_types[i] = IMX_VPU_API_FRAME_TYPE_SKIP; break;
					default: frame_types[i] = IMX_VPU_API_FRAME_TYPE_UNKNOWN;
				}
			}

			break;
		}

		/* XXX: the VPU documentation indicates that picType's bit #0 is
		 * cleared if it is an IDR frame, and set if it is non-IDR, and
		 * the bits 1..3 indicate if this is an I, P, or B frame.
		 * However, tests show this to be untrue. picType actually conforms
		 * to the default case below for h.264 content as well. */

		default:
			switch (vpu_pic_type)
			{
				case 0: type = IMX_VPU_API_FRAME_TYPE_I; break;
				case 1: type = IMX_VPU_API_FRAME_TYPE_P; break;
				case 2: case 3: type = IMX_VPU_API_FRAME_TYPE_B; break;
				default: break;
			}
			frame_types[0] = frame_types[1] = type;
	}
}


static ImxVpuApiInterlacingMode convert_interlacing_mode(ImxVpuApiCompressionFormat compression_format, DecOutputInfo *dec_output_info)
{
	if (dec_output_info->interlacedFrame)
	{
		ImxVpuApiInterlacingMode result = dec_output_info->topFieldFirst ? IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_FIRST : IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST;

		if (compression_format == IMX_VPU_API_COMPRESSION_FORMAT_H264)
		{
			switch (dec_output_info->h264Npf)
			{
				case 1: result = IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_ONLY; break;
				case 2: result = IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_ONLY; break;
				default: break;
			}
		}

		return result;
	}
	else
		return IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING;
}


static BOOL decoder_uses_semi_planar_color_format(ImxVpuApiDecOpenParams *open_params)
{
	return (open_params->compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG) || !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT);
}


static void copy_quantization_table(uint8_t *dest_table, uint8_t const *src_table, size_t num_coefficients, unsigned int scale_factor)
{
	IMX_VPU_API_LOG("quantization table:  num coefficients: %u  scale factor: %u ", num_coefficients, scale_factor);

	for (size_t i = 0; i < num_coefficients; ++i)
	{
		/* The +50 ensures rounding instead of truncation */
		long val = (((long)src_table[jpeg_zigzag_pattern[i]]) * scale_factor + 50) / 100;

		/* The VPU's JPEG encoder supports baseline data only,
		 * so no quantization matrix values above 255 are allowed */
		if (val <= 0)
			val = 1;
		else if (val >= 255)
			val = 255;

		dest_table[i] = val;
	}
}


static void set_jpeg_tables(unsigned int quality_factor, EncMjpgParam *jpeg_params)
{
	uint8_t const *component_info_table;
	unsigned int scale_factor;

	assert(jpeg_params != NULL);


	/* NOTE: The tables in structure referred to by jpeg_params must
	 * have been filled with nullbytes, and the mjpg_sourceFormat field
	 * must be valid */


	/* Copy the Huffman tables */

	memcpy(jpeg_params->huffBits[DC_TABLE_INDEX0], jpeg_enc_huffman_bits_luma_dc,   sizeof(jpeg_enc_huffman_bits_luma_dc));
	memcpy(jpeg_params->huffBits[AC_TABLE_INDEX0], jpeg_enc_huffman_bits_luma_ac,   sizeof(jpeg_enc_huffman_bits_luma_ac));
	memcpy(jpeg_params->huffBits[DC_TABLE_INDEX1], jpeg_enc_huffman_bits_chroma_dc, sizeof(jpeg_enc_huffman_bits_chroma_dc));
	memcpy(jpeg_params->huffBits[AC_TABLE_INDEX1], jpeg_enc_huffman_bits_chroma_ac, sizeof(jpeg_enc_huffman_bits_chroma_ac));

	memcpy(jpeg_params->huffVal[DC_TABLE_INDEX0], jpeg_enc_huffman_value_luma_dc,   sizeof(jpeg_enc_huffman_value_luma_dc));
	memcpy(jpeg_params->huffVal[AC_TABLE_INDEX0], jpeg_enc_huffman_value_luma_ac,   sizeof(jpeg_enc_huffman_value_luma_ac));
	memcpy(jpeg_params->huffVal[DC_TABLE_INDEX1], jpeg_enc_huffman_value_chroma_dc, sizeof(jpeg_enc_huffman_value_chroma_dc));
	memcpy(jpeg_params->huffVal[AC_TABLE_INDEX1], jpeg_enc_huffman_value_chroma_ac, sizeof(jpeg_enc_huffman_value_chroma_ac));


	/* Copy the quantization tables */

	/* Ensure the quality factor is in the 1..100 range */
	if (quality_factor < 1)
		quality_factor = 1;
	if (quality_factor > 100)
		quality_factor = 100;

	/* Using the Independent JPEG Group's formula, used in libjpeg, for generating
	 * a scale factor out of a quality factor in the 1..100 range */
	if (quality_factor < 50)
		scale_factor = 5000 / quality_factor;
	else
		scale_factor = 200 - quality_factor * 2;

	/* We use the same quant table for Cb and Cr */
	copy_quantization_table(jpeg_params->qMatTab[0], jpeg_quantization_table_luma,   sizeof(jpeg_quantization_table_luma),   scale_factor); /* Y */
	copy_quantization_table(jpeg_params->qMatTab[1], jpeg_quantization_table_chroma, sizeof(jpeg_quantization_table_chroma), scale_factor); /* Cb */
	copy_quantization_table(jpeg_params->qMatTab[2], jpeg_quantization_table_chroma, sizeof(jpeg_quantization_table_chroma), scale_factor); /* Cr */


	/* Copy the component info table (depends on the format) */

	switch (jpeg_params->mjpg_sourceFormat)
	{
		case FORMAT_420: component_info_table = jpeg_enc_component_info_tables[0]; break;
		case FORMAT_422: component_info_table = jpeg_enc_component_info_tables[1]; break;
		case FORMAT_224: component_info_table = jpeg_enc_component_info_tables[2]; break;
		case FORMAT_444: component_info_table = jpeg_enc_component_info_tables[3]; break;
		case FORMAT_400: component_info_table = jpeg_enc_component_info_tables[4]; break;
		default: assert(FALSE);
	}

	memcpy(jpeg_params->cInfoTab, component_info_table, 4 * 6);
}


static char const * retcode_to_string(RetCode ret_code)
{
	switch (ret_code)
	{
		case RETCODE_SUCCESS: return "success";
		case RETCODE_FAILURE: return "failure";
		case RETCODE_INVALID_HANDLE: return "invalid handle";
		case RETCODE_INVALID_PARAM: return "invalid parameters";
		case RETCODE_INVALID_COMMAND: return "invalid command";
		case RETCODE_ROTATOR_OUTPUT_NOT_SET: return "rotation enabled but rotator output buffer not set";
		case RETCODE_ROTATOR_STRIDE_NOT_SET: return "rotation enabled but rotator stride not set";
		case RETCODE_FRAME_NOT_COMPLETE: return "frame decoding operation not complete";
		case RETCODE_INVALID_FRAME_BUFFER: return "frame buffers are invalid";
		case RETCODE_INSUFFICIENT_FRAME_BUFFERS: return "not enough frame buffers specified";
		case RETCODE_INVALID_STRIDE: return "invalid stride - check Y stride values of framebuffers (must be a multiple of 8 and equal to or larger than the frame width)";
		case RETCODE_WRONG_CALL_SEQUENCE: return "wrong call sequence";
		case RETCODE_CALLED_BEFORE: return "already called before (may not be called more than once in a VPU instance)";
		case RETCODE_NOT_INITIALIZED: return "VPU is not initialized";
		case RETCODE_DEBLOCKING_OUTPUT_NOT_SET: return "deblocking activated but deblocking information not available";
		case RETCODE_NOT_SUPPORTED: return "feature not supported";
		case RETCODE_REPORT_BUF_NOT_SET: return "data report buffer address not set";
		case RETCODE_FAILURE_TIMEOUT: return "timeout";
		case RETCODE_MEMORY_ACCESS_VIOLATION: return "memory access violation";
		case RETCODE_JPEG_EOS: return "JPEG end-of-stream reached"; /* NOTE: JPEG EOS is not an error */
		case RETCODE_JPEG_BIT_EMPTY: return "JPEG bit buffer empty - cannot parse header";
		default: return "unknown error";
	}
}




/************************************************/
/******* DECODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* Decoding with the CODA VPU works by first reading encoded data to get
 * information about it, like the width, height, etc. Then, based on that
 * information, framebuffers are added to the VPU, creating a framebuffer
 * pool. From then on, the VPU processes encoded frames in this way:
 *
 * - The encoded frame data is pushed into the bitstream buffer.
 * - The VPU reads the encoded data and starts decoding it.
 *   This is done by the vpu_DecStartOneFrame() function.
 * - For decoding a frame, the VPU picks one framebuffer out of the pool.
 *   With the vpu_DecGetOutputInfo() function, it is possible to figure
 *   out what framebuffer the VPU picked (the indexFrameDecoded field
 *   indicates which one).
 * - Should a frame be fully decoded and be read for display, then the
 *   vpu_DecGetOutputInfo() function call from earlier will also return
 *   a valid indexFrameDisplay index.
 * - Fully decoded frames are written in a tiled layout. This is how
 *   the VPU works internally. It could detile automatically, but we
 *   don't do that. Instead, we let the IPU's VDOA handle that. The
 *   advantage is that the IPU can detile into a *different* DMA
 *   buffer (the output_frame_dma_buffer). That way, we don't have to
 *   expose framebuffers from the VPU's pool to the outside, which
 *   allows for much more flexible use cases.
 * - Once the frame has been detiled and copied with the IPU's VDOA,
 *   the framebuffer is returned to the pool vpu_DecClrDispFlag().
 *   This is necessary, since otherwise, the VPU won't pick this
 *   framebuffer again, because it doesn't know if we are done with
 *   it or if we still need it.
 *
 * To keep track of frames that are being decoded, an array of
 * DecFrameEntry structures is maintained. There is one DecFrameEntry for
 * each framebuffer in the pool. These entries contain information
 * like the context/PTS/DTS of the frame that is being decoded, the
 * pointer to the framebuffer's DMA buffer etc.
 *
 * JPEG decoding is done differently. For JPEG, there is no framebuffer
 * pool. Instead, the VPU decodes to a DMA buffer that is specified
 * by setting the JPEG rotator's output address. This also means that
 * the IPU isn't used for detiling&copying, because it is unnecessary
 * (we can specify any DMA buffer we want as the JPEG rotator output).
 * The DecFrameEntry array is still created, but with only one item. */


#define VPU_DECODER_DISPLAYIDX_ALL_FRAMES_DISPLAYED          (-1)
#define VPU_DECODER_DISPLAYIDX_SKIP_MODE_NO_FRAME_TO_DISPLAY (-2)
#define VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY           (-3)

#define VPU_DECODER_DECODEIDX_ALL_FRAMES_DECODED (-1)
#define VPU_DECODER_DECODEIDX_FRAME_NOT_DECODED  (-2)


/* Frame entries are not just occupied or free. They can be in one of three modes,
 * specifying how the framebuffer associated with this entry is being used:
 *
 * - DecFrameEntryMode_Free: framebuffer is not being used for decoding, and does not
     hold a displayable frame
 * - DecFrameEntryMode_ReservedForDecoding: framebuffer contains frame data that is
 *   being decoded; this data can not be displayed yet though
 * - DecFrameEntryMode_ContainsDisplayableFrame: framebuffer contains frame that has
 *   been fully decoded; this can be displayed
 *
 * Entries in mode DecFrameEntryMode_ReservedForDecoding do not reach the outside.
 * Only frames in DecFrameEntryMode_ContainsDisplayableFrame mode do, via the
 * imx_vpu_api_dec_get_decoded_frame() function. Inside that function, the frames
 * are detiled&copied with the IPU VDOA, except for JPEG (see explanation above). */
typedef enum
{
	DecFrameEntryMode_Free,
	DecFrameEntryMode_ReservedForDecoding,
	DecFrameEntryMode_ContainsDisplayableFrame
}
DecFrameEntryMode;


/* Structure for frames that are being decoded.
 * There is one entry for each framebuffer in the CODA framebuffer pool. */
typedef struct
{
	/* Context of the frame that is being decoded. This is set to the
	 * value of the context field in the ImxVpuApiEncodedFrame instance
	 * that was the input for this frame entry. */
	void *frame_context;
	/* PTS and DTS values of the frame that is being decoded. These are
	 * set to the values of the pts/dts fields of the ImxVpuApiEncodedFrame
	 * instance that was the input for this frame entry. */
	uint64_t pts, dts;
	/* Type of the decoded frame (I-frame, P-frame etc). This is filled
	 * once the frame has been decoded. Note that "decoded" does not
	 * automatically mean "to be displayed". It is for example possible
	 * that a frame is first decoded, but displayed later. This happens
	 * when frames are queued, or when frames need reordering. */
	ImxVpuApiFrameType frame_types[2];
	/* Interlacing mode of the decoded frame. This is filled once the
	 * frame has been decoded. See the note above about decoded frames. */
	ImxVpuApiInterlacingMode interlacing_mode;
	/* What mode this entry currently is in. */
	DecFrameEntryMode mode;

	/* DMA buffer that contains the pixels of the framebuffer this
	 * entry is associated with. */
	ImxDmaBuffer *fb_dma_buffer;
	/* Context of the framebuffer this entry is associated with. Not to
	 * be confused with frame_context. This value corresponds to the
	 * fb_contexts argument of imx_vpu_api_dec_add_framebuffers_to_pool(). */
	void *fb_context;
}
DecFrameEntry;


struct _ImxVpuApiDecoder
{
	/* Handle of the CODA VPU decoder instance. */
	DecHandle handle;

	/* Unix file descriptor of the IPU VDOA. */
	int ipu_vdoa_fd;

	/* Stream buffer (called "bitstream buffer" in the VPU documentation).
	 * Holds encoded data that shall be decoded. This includes additional
	 * header metadata that may have to be manually produced and inserted
	 * by the imx_vpu_api_dec_preprocess_input_data() function. The actual
	 * insertion of data into the stream buffer is done by calling
	 * imx_vpu_api_dec_push_input_data(). */
	ImxDmaBuffer *stream_buffer;
	uint8_t *stream_buffer_virtual_address;
	imx_physical_address_t stream_buffer_physical_address;

	/* Copy of the open_params passed to imx_vpu_api_dec_open(). */
	ImxVpuApiDecOpenParams open_params;

	/* JPEG specific states to track changes in width, height, color format.
	 * This is necessary since JPEG decoding works differently. Amongst
	 * other things, the VPU does not report JPEG format changes on its own. */
	BOOL jpeg_format_changed;
	size_t jpeg_width, jpeg_height;
	ImxVpuApiColorFormat jpeg_color_format;

	size_t y_offset, u_offset, v_offset;
	/* Offset in framebuffers from the start, in bytes, to the location
	 * of the "co-located motion vector" data. */
	size_t mvcol_offset;

	size_t total_padded_input_width, total_padded_input_height;
	size_t total_padded_output_width, total_padded_output_height;

	/* num_framebuffers: How many framebuffers there are. This is also the
	 * size of the frame_entries and internal_framebuffers arrays.
	 *
	 * num_used_framebuffers: How many framebuffers are actually in use,
	 * that is, how many of them hold fully decoded frames or are currently
	 * being written into by the VPU. */
	size_t num_framebuffers, num_used_framebuffers;
	/* internal_framebuffers: Array of framebuffers, suitable for consumption
	 *                        by the CODA VPU.
	 * frame_entries: Information necessary to track pushed encoded frames and
	 *                associate them with decoded frames. These entries also
	 *                contain information about the associated framebuffers. */
	FrameBuffer *internal_framebuffers;
	DecFrameEntry *frame_entries;
	DecFrameEntry dropped_frame_entry;

	/* Number of framebuffers that are expected to be added before decoding
	 * can continue. This is initally set to a nonzero value when initial
	 * info is received from the VPU. A value of zero indicates that no
	 * framebuffers are to be added. It is set to zero inside the function
	 * imx_vpu_api_dec_add_framebuffers_to_pool() once the framebuffers
	 * are registered. */
	size_t num_framebuffers_to_be_added;

	/* The output frame to transfer decoded pixels into. When decoding JPEG,
	 * the VPU decodes into this buffer directly. When decoding any other
	 * compression format, the VPU decodes into one of the framebuffers in its
	 * pool, then the pixels from that framebuffer are copied into this
	 * output_frame_dma_buffer. Since the VPU's native output uses a tiled
	 * layout, this copy is "free", since we anyway have to detile.
	 * output_framebuffer and output_frame_fb_context are used only during JPEG
	 * decoding. The former is passed to the JPEG rotator, the latter is
	 * passed to the fb_context field of the ImxVpuRawFrame that will later be
	 * returned by imx_vpu_api_dec_get_decoded_frame().
	 * These fields are set by imx_vpu_api_dec_set_output_frame_dma_buffer(). */
	FrameBuffer output_framebuffer;
	ImxDmaBuffer *output_frame_dma_buffer;
	void *output_frame_fb_context;

	/* The encoded frame staged by the imx_vpu_api_dec_push_encoded_frame()
	 * function. Needed since it is not known right away in what item of the
	 * frame_entries array we have to write the encoded frame's details into.
	 * Only later, during the imx_vpu_api_dec_decode() call, do we learn what
	 * item to pick. So, stage the encoded frame here so we can look up its
	 * details later when we know what item to pick. */
	ImxVpuApiEncodedFrame staged_encoded_frame;
	BOOL staged_encoded_frame_set;

	/* If TRUE, then at some point after opening a new decoder instance,
	 * some encoded data got pushed into the encoder by calling the
	 * imx_vpu_api_dec_push_encoded_frame() function. This flag is used for
	 * keeping track of whether or not VPU initial info needs to be retrieved.
	 * Unlike staged_encoded_frame_set, this one is _not_ set to FALSE when
	 * the decoder is flushed by imx_vpu_api_dec_flush(). */
	BOOL encoded_data_got_pushed;

	/* If TRUE, then the main header for the given compression format has been
	 * pushed. What this header is depends on the format. This can be a VP8
	 * IVF sequence header for example, of the extra_header data from the
	 * ImxVpuApiDecOpenParams structure. The main_header_pushed field exists
	 * to make sure such headers are not pushed into the stream buffer twice. */
	BOOL main_header_pushed;

	/* Drain mode specific fields. drain_mode_enabled is set to TRUE when
	 * imx_vpu_api_dec_enable_drain_mode() is called. drain_eos_sent_to_vpu
	 * is needed to track if the VPU was notified of the end of stream or not. */
	BOOL drain_mode_enabled;
	BOOL drain_eos_sent_to_vpu;

	/* Initial information for the encoded stream. This is given to us by the
	 * VPU, by calling vpu_DecGetInitialInfo(). Its values are copied to the
	 * stream_info instance. Once the initial info is available, the framebuffer
	 * pool can be allocated (unless we are decoding JPEG data) and the stream
	 * can be decoded. */
	DecInitialInfo initial_info;
	BOOL initial_info_available;
	ImxVpuApiDecStreamInfo stream_info;

	/* Information about the frame that is being decoded or was just decoded.
	 * Filled by the vpu_DecGetOutputInfo() function. */
	DecOutputInfo dec_output_info;
	/* If the last vpu_DecGetOutputInfo() set a valid indexFrameDisplay value
	 * in dec_output_info, then that value is copied into this field. It is
	 * then used in imx_vpu_api_dec_get_decoded_frame() to access the item
	 * in the frame_entries array that corresponds to the frame that got
	 * fully decoded and is ready for display.
	 *
	 * Since JPEG decoding does not use a framebuffer pool, tricks are done
	 * to make sure this is always set to 0 (since when decoding JPEGs, the
	 * frame_entries array always is only one item long). */
	int available_decoded_frame_idx;

	/* Information about skipped/dropped frames. */
	ImxVpuApiDecSkippedFrameReasons skipped_frame_reason;
	void *skipped_frame_context;
	uint64_t skipped_frame_pts;
	uint64_t skipped_frame_dts;
};


static BOOL imx_vpu_api_dec_preprocess_input_data(ImxVpuApiDecoder *decoder, uint8_t const *extra_header_data, size_t extra_header_data_size, uint8_t *main_data, size_t main_data_size);
static BOOL imx_vpu_api_dec_push_input_data(ImxVpuApiDecoder *decoder, void const *data, size_t data_size);

static void imx_vpu_api_dec_free_internal_arrays(ImxVpuApiDecoder *decoder);

static RetCode imx_vpu_api_dec_get_initial_info(ImxVpuApiDecoder *decoder);

static BOOL imx_vpu_api_dec_fill_stream_info_from_initial_info(ImxVpuApiDecoder *decoder, DecInitialInfo const *initial_info);
static BOOL imx_vpu_api_dec_fill_stream_info(ImxVpuApiDecoder *decoder, size_t actual_frame_width, size_t actual_frame_height, ImxVpuApiColorFormat color_format, unsigned int frame_rate_numerator, unsigned int frame_rate_denominator, size_t min_num_required_framebuffers, BOOL interlaced);



static BOOL imx_vpu_api_dec_preprocess_input_data(ImxVpuApiDecoder *decoder, uint8_t const *extra_header_data, size_t extra_header_data_size, uint8_t *main_data, size_t main_data_size)
{
	/* In here, we analyze header data, the main frame data, and if needed,
	 * insert data into the stream. This data is inserted before the main
	 * frame data, so this is the right place for inserting additional
	 * frame/sequence headers etc.
	 *
	 * The extra_header_data is out-of-band data that is compression format
	 * specific. Not all formats use this, so it may be NULL. With some
	 * formats, this extra_header_data is inserted here into the stream
	 * buffer. With other formats, this data is only analyzed, and not
	 * inserted. Some formats may also need to analyze the main frame data,
	 * which is why the main_data is supplied to this function as well.
	 * However, main_data is _not_ supposed to be modified or inserted into
	 * the stream buffer here. It is here _only_ for analysis. */

	BOOL ret = TRUE;

	assert(decoder != NULL);

	switch (decoder->open_params.compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
		{
			unsigned int jpeg_width, jpeg_height;
			ImxVpuApiColorFormat jpeg_color_format;

			/* JPEGs are a special case.
			 * The VPU does not report size or color format changes. Therefore,
			 * JPEG header have to be parsed here manually to retrieve the
			 * width, height, and color format and check if these changed.
			 * If so, invoke the initial_info_callback again. */

			if (!imx_vpu_api_parse_jpeg_header(main_data, main_data_size, decoder_uses_semi_planar_color_format(&(decoder->open_params)), &jpeg_width, &jpeg_height, &jpeg_color_format))
			{
				IMX_VPU_API_ERROR("encoded frame is not valid JPEG data");
				return FALSE;
			}

			if (decoder->initial_info_available && ((decoder->jpeg_width != jpeg_width) || (decoder->jpeg_height != jpeg_height) || (decoder->jpeg_color_format != jpeg_color_format)))
				decoder->jpeg_format_changed = TRUE;

			decoder->jpeg_width = jpeg_width;
			decoder->jpeg_height = jpeg_height;
			decoder->jpeg_color_format = jpeg_color_format;

			IMX_VPU_API_LOG("JPEG frame information:  width: %u  height: %u  format: %s  format changed: %d  initial info available: %d", jpeg_width, jpeg_height, imx_vpu_api_color_format_string(jpeg_color_format), decoder->jpeg_format_changed, decoder->initial_info_available);

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		{
			/* The CODA960 decoder requires frame headers and
			 * as sequence layer headers for WMV3 decoding, so
			 * insert them here. */
			if (decoder->main_header_pushed)
			{
				uint8_t header[WMV3_RCV_FRAME_LAYER_HEADER_SIZE];
				imx_vpu_api_insert_wmv3_frame_layer_header(header, main_data_size);
				if (!imx_vpu_api_dec_push_input_data(decoder, header, WMV3_RCV_FRAME_LAYER_HEADER_SIZE))
					ret = FALSE;
			}
			else
			{
				uint8_t header[WMV3_RCV_SEQUENCE_LAYER_HEADER_SIZE];

				assert(extra_header_data != NULL);
				assert(extra_header_data_size >= 4);

				imx_vpu_api_insert_wmv3_sequence_layer_header(header, decoder->open_params.frame_width, decoder->open_params.frame_height, main_data_size, extra_header_data);
				if (!imx_vpu_api_dec_push_input_data(decoder, header, WMV3_RCV_SEQUENCE_LAYER_HEADER_SIZE))
					ret = FALSE;
				decoder->main_header_pushed = TRUE;
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		{
			if (!(decoder->main_header_pushed))
			{
				assert(extra_header_data != NULL);
				assert(extra_header_data_size >= 1);

				/* First, push the extra_header_data (except for its first byte,
				 * which contains the size of the extra header data), since it
				 * contains the sequence layer header */
				IMX_VPU_API_LOG("pushing extra header data with %zu byte", extra_header_data_size - 1);
				if (!imx_vpu_api_dec_push_input_data(decoder, extra_header_data + 1, extra_header_data_size - 1))
				{
					IMX_VPU_API_ERROR("could not push extra header data to bitstream buffer");
					return FALSE;
				}

				decoder->main_header_pushed = TRUE;

				/* Next, the frame layer header will be pushed by the
				 * block below */
			}

			if (decoder->main_header_pushed)
			{
				uint8_t header[VC1_NAL_FRAME_LAYER_HEADER_MAX_SIZE];
				size_t actual_header_length;
				imx_vpu_api_insert_vc1_frame_layer_header(header, main_data, &actual_header_length);
				if (actual_header_length > 0)
				{
					IMX_VPU_API_LOG("pushing frame layer header with %zu byte", actual_header_length);
					if (!imx_vpu_api_dec_push_input_data(decoder, header, actual_header_length))
						ret = FALSE;
				}
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
		{
			/* VP8 does not need out-of-band codec data. However, some headers
			 * need to be inserted to contain it in an IVF stream, which the VPU needs.
			 * XXX the vpu wrapper has a special mode for "raw VP8 data". What is this?
			 * Perhaps it means raw IVF-contained VP8? */

			uint8_t header[VP8_SEQUENCE_HEADER_SIZE + VP8_FRAME_HEADER_SIZE];
			size_t header_size = 0;

			if (decoder->main_header_pushed)
			{
				imx_vpu_api_insert_vp8_ivf_frame_header(&(header[0]), main_data_size, 0);
				header_size = VP8_FRAME_HEADER_SIZE;
				IMX_VPU_API_LOG("pushing VP8 IVF frame header data with %zu byte", header_size);
			}
			else
			{
				imx_vpu_api_insert_vp8_ivf_sequence_header(
					&(header[0]),
					decoder->stream_info.decoded_frame_framebuffer_metrics.actual_frame_width,
					decoder->stream_info.decoded_frame_framebuffer_metrics.actual_frame_height
				);
				imx_vpu_api_insert_vp8_ivf_frame_header(&(header[VP8_SEQUENCE_HEADER_SIZE]), main_data_size, 0);
				header_size = VP8_SEQUENCE_HEADER_SIZE + VP8_FRAME_HEADER_SIZE;
				decoder->main_header_pushed = TRUE;
				IMX_VPU_API_LOG("pushing VP8 IVF main and frame header data with %zu byte total", header_size);
			}

			if (header_size != 0)
			{
				if (!imx_vpu_api_dec_push_input_data(decoder, header, header_size))
					ret = FALSE;
			}

			break;
		}

		default:
			if (!(decoder->main_header_pushed) && (extra_header_data != NULL) && (extra_header_data_size > 0))
			{
				if (!imx_vpu_api_dec_push_input_data(decoder, extra_header_data, extra_header_data_size))
					ret = FALSE;
				decoder->main_header_pushed = TRUE;
			}
	}

	return ret;
}


static BOOL imx_vpu_api_dec_push_input_data(ImxVpuApiDecoder *decoder, void const *data, size_t data_size)
{
	PhysicalAddress read_ptr, write_ptr;
	Uint32 num_free_bytes;
	RetCode dec_ret;
	size_t read_offset, write_offset, num_free_bytes_at_end, num_bytes_to_push;
	size_t bbuf_size;
	int i;

	assert(decoder != NULL);

	/* Only touch data within the first VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE bytes of the
	 * overall bitstream buffer, since the bytes beyond are reserved for slice and
	 * ps save data and/or VP8 data */
	bbuf_size = VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE;


	/* Get the current read and write position pointers in the bitstream buffer For
	 * decoding, the write_ptr is the interesting one. The read_ptr is just logged.
	 * These pointers are physical addresses. To get an offset value for the write
	 * position for example, one calculates:
	 * write_offset = (write_ptr - bitstream_buffer_physical_address)
	 * Also, since JPEG uses line buffer mode, this is not done for JPEG */
	if (decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		dec_ret = vpu_DecGetBitstreamBuffer(decoder->handle, &read_ptr, &write_ptr, &num_free_bytes);
		if (dec_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("could not retrieve bitstream buffer information: %s", retcode_to_string(dec_ret));
			return FALSE;
		}
		IMX_VPU_API_LOG("bitstream buffer status:  read ptr 0x%x  write ptr 0x%x  num free bytes %u", read_ptr, write_ptr, num_free_bytes);
	}


	/* The bitstream buffer behaves like a ring buffer. This means that incoming data
	 * be written at once, if there is enough room at the current write position, or
	 * the write position may be near the end of the buffer, in which case two writes
	 * have to be performed (the first N bytes at the end of the buffer, and the remaining
	 * (bbuf_size - N) bytes at the beginning).
	 * Exception: motion JPEG data. With motion JPEG, the decoder operates in the line
	 * buffer mode. Meaning that the encoded JPEG frame is always placed at the beginning
	 * of the bitstream buffer. It does not have to work like a ring buffer, since with
	 * motion JPEG, one input frame immediately produces one decoded output frame. */
	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		write_offset = 0;
	else
		write_offset = write_ptr - decoder->stream_buffer_physical_address;

	num_free_bytes_at_end = bbuf_size - write_offset;

	read_offset = 0;

	/* This stores the number of bytes to push in the next immediate write operation
	 * If the write position is near the end of the buffer, not all bytes can be written
	 * at once, as described above */
	num_bytes_to_push = (num_free_bytes_at_end < data_size) ? num_free_bytes_at_end : data_size;

	/* Write the bytes to the bitstream buffer, either in one, or in two steps (see above) */
	for (i = 0; (i < 2) && (read_offset < data_size); ++i)
	{
		/* The actual write */
		uint8_t *src = ((uint8_t*)data) + read_offset;
		uint8_t *dest = ((uint8_t*)(decoder->stream_buffer_virtual_address)) + write_offset;
		memcpy(dest, src, num_bytes_to_push);

		/* Update the bitstream buffer pointers. Since JPEG does not use the
		 * ring buffer (instead it uses the line buffer mode), update it only
		 * for non-JPEG codec formats */
		if (decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		{
			dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, num_bytes_to_push);
			if (dec_ret != RETCODE_SUCCESS)
			{
				IMX_VPU_API_ERROR("could not update bitstream buffer with new data: %s", retcode_to_string(dec_ret));
				return FALSE;
			}
		}

		/* Update offsets and write sizes */
		read_offset += num_bytes_to_push;
		write_offset += num_bytes_to_push;
		num_bytes_to_push = data_size - read_offset;

		/* Handle wrap-around if it occurs */
		if (write_offset >= bbuf_size)
			write_offset -= bbuf_size;
	}

	return TRUE;
}


static void imx_vpu_api_dec_free_internal_arrays(ImxVpuApiDecoder *decoder)
{
	if (decoder->internal_framebuffers != NULL)
	{
		free(decoder->internal_framebuffers);
		decoder->internal_framebuffers = NULL;
	}

	if (decoder->frame_entries != NULL)
	{
		free(decoder->frame_entries);
		decoder->frame_entries = NULL;
	}
}


static RetCode imx_vpu_api_dec_get_initial_info(ImxVpuApiDecoder *decoder)
{
	RetCode dec_ret;

	assert(decoder != NULL);

	decoder->initial_info_available = FALSE;

	/* Set the force escape flag first (see section 4.3.2.2
	 * in the VPU documentation for an explanation why) */
	if ((dec_ret = vpu_DecSetEscSeqInit(decoder->handle, 1)) != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not set force escape flag: %s", retcode_to_string(dec_ret));
		return RETCODE_FAILURE;
	}

	/* The actual retrieval */
	dec_ret = vpu_DecGetInitialInfo(decoder->handle, &(decoder->initial_info));

	/* As recommended in section 4.3.2.2, clear the force
	 * escape flag immediately after retrieval is finished */
	vpu_DecSetEscSeqInit(decoder->handle, 0);

	if (dec_ret == RETCODE_SUCCESS)
		decoder->initial_info_available = TRUE;
	else
		IMX_VPU_API_ERROR("vpu_DecGetInitialInfo() reports error: %s", retcode_to_string(dec_ret));

	return dec_ret;
}


static BOOL imx_vpu_api_dec_fill_stream_info_from_initial_info(ImxVpuApiDecoder *decoder, DecInitialInfo const *initial_info)
{
	BOOL ret;
	ImxVpuApiColorFormat color_format = IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT;
	size_t frame_width = initial_info->picWidth;
	size_t frame_height = initial_info->picHeight;
	BOOL semi_planar = decoder_uses_semi_planar_color_format(&(decoder->open_params));
	size_t min_num_required_framebuffers;

	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		switch (initial_info->mjpg_sourceFormat)
		{
			case FORMAT_420: color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT; break;
			case FORMAT_422: color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT; break;
			case FORMAT_224: color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT; break;
			case FORMAT_444: color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT; break;
			case FORMAT_400: color_format = IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT; break;
			default:
				IMX_VPU_API_ERROR("unknown/unsupported color format %s (%d)", imx_vpu_api_color_format_string(color_format), color_format);
				return FALSE;
		}

		if (frame_width == 0)
			frame_width = decoder->jpeg_width;
		if (frame_height == 0)
			frame_height = decoder->jpeg_height;
	}
	else
	{
		color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT;

		/* Use the frame width/height from the open params if available.
		 * The sizes from initial_info can contain padding, and we want
		 * the actual, unpadded sizes. */

		if (decoder->open_params.frame_width > 0)
			frame_width = decoder->open_params.frame_width;

		if (decoder->open_params.frame_height > 0)
			frame_height = decoder->open_params.frame_height;
	}

	/* Add the extra framebuffers to avoid decoding errors. See the documentation
	 * for the NUM_EXTRA_FRAMEBUFFERS_REQUIRED constant for more.
	 * For JPEG, we definitely do not need these extra framebuffers, so don't add
	 * them if we are decoding JPEG data. */
	min_num_required_framebuffers = initial_info->minFrameBufferCount + ((decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG) ? 0 : NUM_EXTRA_FRAMEBUFFERS_REQUIRED);

	ret = imx_vpu_api_dec_fill_stream_info(
		decoder,
		frame_width, frame_height,
		color_format,
		initial_info->frameRateRes, initial_info->frameRateDiv,
		min_num_required_framebuffers,
		!!(initial_info->interlace)
	);
	if (!ret)
		return FALSE;

	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_H264)
	{
		Rect const *crop_rect = &(initial_info->picCropRect);

		/* TODO: check for off-by-one errors */
		if ((crop_rect->left < crop_rect->right) && (crop_rect->top < crop_rect->bottom))
		{
			decoder->stream_info.has_crop_rectangle = TRUE;
			decoder->stream_info.crop_left   = crop_rect->left;
			decoder->stream_info.crop_top    = crop_rect->top;
			decoder->stream_info.crop_width  = crop_rect->right - crop_rect->left;
			decoder->stream_info.crop_height = crop_rect->bottom - crop_rect->top;
		}
	}

	return TRUE;
}


static BOOL imx_vpu_api_dec_fill_stream_info(ImxVpuApiDecoder *decoder, size_t actual_frame_width, size_t actual_frame_height, ImxVpuApiColorFormat color_format, unsigned int frame_rate_numerator, unsigned int frame_rate_denominator, size_t min_num_required_framebuffers, BOOL interlaced)
{
	/* All formats supported by the VPU have one separate
	 * luma plane, meaning that each pixel corresponds to
	 * exactly one byte in that plane. */
	static size_t const bytes_per_y_pixel = 1;

	BOOL semi_planar = decoder_uses_semi_planar_color_format(&(decoder->open_params));
	ImxVpuApiDecStreamInfo *stream_info = &(decoder->stream_info);
	ImxVpuApiFramebufferMetrics *fb_metrics = &(stream_info->decoded_frame_framebuffer_metrics);

	assert(decoder->initial_info_available);

	fb_metrics->actual_frame_width = actual_frame_width;
	fb_metrics->actual_frame_height = actual_frame_height;
	/* These alignments are a combination of the requirements from the VPU and the IPU'S VDOA. */
	fb_metrics->aligned_frame_width = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, 128);
	fb_metrics->aligned_frame_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, 32);
	fb_metrics->y_stride = fb_metrics->aligned_frame_width;
	fb_metrics->y_size = fb_metrics->y_stride * fb_metrics->aligned_frame_height;

	stream_info->has_crop_rectangle = FALSE;
	stream_info->crop_left = 0;
	stream_info->crop_top = 0;
	stream_info->crop_width = fb_metrics->actual_frame_width;
	stream_info->crop_height = fb_metrics->actual_frame_height;

	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride / 2;
			fb_metrics->uv_size = fb_metrics->y_size / 4;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride / 2;
			fb_metrics->uv_size = fb_metrics->y_size / 2;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride;
			fb_metrics->uv_size = fb_metrics->y_size;
			break;

		case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride;
			fb_metrics->uv_size = 0;
			break;

		default:
			IMX_VPU_API_ERROR("unknown/unsupported color format %s (%d)", imx_vpu_api_color_format_string(color_format), color_format);
			return FALSE;
	}

	/* Adjust the uv_stride and uv_size values
	 * in case we are using semi-planar chroma. */
	if (semi_planar)
	{
		fb_metrics->uv_stride *= 2;
		fb_metrics->uv_size *= 2;
	}

	fb_metrics->y_offset = 0;
	fb_metrics->u_offset = fb_metrics->y_size;
	fb_metrics->v_offset = fb_metrics->u_offset + fb_metrics->uv_size;
	decoder->y_offset = 0;
	decoder->u_offset = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->y_size, 8);
	decoder->v_offset = IMX_VPU_API_ALIGN_VAL_TO(decoder->u_offset + fb_metrics->uv_size, 8);
	decoder->output_framebuffer.strideY = fb_metrics->y_stride;
	decoder->output_framebuffer.strideC = fb_metrics->uv_stride;

	/* Record the offset the start of a framebuffer to the
	 * region inside a framebuffer where the co-located motion
	 * vectors are located (that's right after the chroma
	 * planes in the frame). */
	decoder->mvcol_offset = semi_planar ? fb_metrics->u_offset : fb_metrics->v_offset;
	decoder->mvcol_offset = IMX_VPU_API_ALIGN_VAL_TO(decoder->mvcol_offset + fb_metrics->uv_size, 8);

	decoder->total_padded_input_width = fb_metrics->y_stride / bytes_per_y_pixel;
	decoder->total_padded_input_height = (color_format == IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT) ? fb_metrics->aligned_frame_height : ((fb_metrics->u_offset - fb_metrics->y_offset) / fb_metrics->y_stride);
	decoder->total_padded_output_width = fb_metrics->y_stride / bytes_per_y_pixel;
	decoder->total_padded_output_height = (color_format == IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT) ? fb_metrics->aligned_frame_height : ((decoder->u_offset - decoder->y_offset) / fb_metrics->y_stride);

	/* Compute the minimum size for FB pool framebuffers and for
	 * output framebuffes. The two important difference between the
	 * two are:
	 *
	 * 1) FB pool framebuffers must use 4096-byte aligned addresses
	 *    for _all_ components (Y/U/V) to be usable by the IPU VDOA
	 *    for detiling and copying to the output framebuffer. They
	 *    also must have additional space for MvCol data.
	 * 2) Output framebuffers have no particular alignment requirements,
	 *    and need no additional MvCol data.
	 */
	stream_info->min_fb_pool_framebuffer_size = decoder->mvcol_offset + fb_metrics->uv_size;
	stream_info->min_output_framebuffer_size = (semi_planar ? fb_metrics->u_offset : fb_metrics->v_offset) + fb_metrics->uv_size;
	stream_info->fb_pool_framebuffer_alignment = FRAME_PHYSADDR_ALIGNMENT;
	stream_info->output_framebuffer_alignment = FRAME_PHYSADDR_ALIGNMENT;

	stream_info->frame_rate_numerator = frame_rate_numerator;
	stream_info->frame_rate_denominator = frame_rate_denominator;
	stream_info->min_num_required_framebuffers = min_num_required_framebuffers;
	stream_info->video_full_range_flag = 0;

	stream_info->flags = 0;
	if (semi_planar)
		stream_info->flags = IMX_VPU_API_DEC_STREAM_INFO_FLAG_SEMI_PLANAR_FRAMES;
	if (interlaced)
		stream_info->flags |= IMX_VPU_API_DEC_STREAM_INFO_FLAG_INTERLACED;

	/* Get the YUV color format. For any format other than JPEG,
	 * this will always be 4:2:0. For JPEG, it may be something
	 * else. Note that with non-JPEG formats, the VPU always uses
	 * semi-planar 4:2:0 internally, in a tiled fashion. The IPU
	 * is then used for converting to what the user actually
	 * specified (either fully-planar or semi-planar 4:2:0). */
	switch (decoder->initial_info.mjpg_sourceFormat)
	{
		case FORMAT_420:
			stream_info->color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT;
			break;

		case FORMAT_422:
			stream_info->color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT;
			break;

		case FORMAT_224:
			stream_info->color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT;
			break;

		case FORMAT_444:
			stream_info->color_format = semi_planar ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT;
			break;

		case FORMAT_400:
			stream_info->color_format = IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT;
			break;

		default:
			assert(FALSE);
	}

	/* Make sure that at least one framebuffer is allocated and registered. */
	if (stream_info->min_num_required_framebuffers < 1)
		stream_info->min_num_required_framebuffers = 1;

	return TRUE;
}


static ImxVpuApiCompressionFormat const dec_supported_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG2,
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG4,
	IMX_VPU_API_COMPRESSION_FORMAT_H263,
	IMX_VPU_API_COMPRESSION_FORMAT_H264,
	IMX_VPU_API_COMPRESSION_FORMAT_WMV3,
	IMX_VPU_API_COMPRESSION_FORMAT_WVC1,
	IMX_VPU_API_COMPRESSION_FORMAT_JPEG,
	IMX_VPU_API_COMPRESSION_FORMAT_VP8
};

static ImxVpuApiDecGlobalInfo const dec_global_info = {
	.flags = IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER | IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED | IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_CODA960,
	.min_required_stream_buffer_size = VPU_DEC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE,
	.required_stream_buffer_physaddr_alignment = BITSTREAM_BUFFER_PHYSADDR_ALIGNMENT,
	.required_stream_buffer_size_alignment = BITSTREAM_BUFFER_SIZE_ALIGNMENT,
	.supported_compression_formats = dec_supported_compression_formats,
	.num_supported_compression_formats = sizeof(dec_supported_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};

ImxVpuApiDecGlobalInfo const * imx_vpu_api_dec_get_global_info(void)
{
	/* The VP8 prediction buffer and the h.264 slice buffer & SPS/PPS (PS) buffer
	 * share the same memory space, since the decoder does not use them both
	 * at the same time. Check that the sizes are correct (slice & SPS/PPS buffer
	 * sizes must together be larger than the VP8 prediction buffer size). */
	assert(VPU_VP8_MB_PRED_BUFFER_SIZE < (VPU_MAX_SLICE_BUFFER_SIZE + VPU_PS_SAVE_BUFFER_SIZE));

	return &dec_global_info;
}


static ImxVpuApiColorFormat const dec_supported_basic_color_formats[] =
{
	/* Only semi-planar frames are supported because the IPU cannot
	 * handle detiling unless NV12 is set as the output format. */
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT
};


static ImxVpuApiColorFormat const dec_supported_jpeg_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT,
	IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT
};

static ImxVpuApiCompressionFormatSupportDetails const dec_basic_compression_format_support_details = {
	.min_width = 8, .max_width = 1920,
	.min_height = 8, .max_height = 1088,
	.supported_color_formats = dec_supported_basic_color_formats,
	.num_supported_color_formats = sizeof(dec_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat)
};

static ImxVpuApiCompressionFormatSupportDetails const dec_jpeg_support_details = {
	.min_width = 8, .max_width = 8192,
	.min_height = 8, .max_height = 8192,
	.supported_color_formats = dec_supported_jpeg_color_formats,
	.num_supported_color_formats = sizeof(dec_supported_jpeg_color_formats) / sizeof(ImxVpuApiColorFormat)
};

static ImxVpuApiH264SupportDetails const dec_h264_support_details = {
	.parent = {
		.min_width = 8, .max_width = 1920,
		.min_height = 8, .max_height = 1088,
		.supported_color_formats = dec_supported_basic_color_formats,
		.num_supported_color_formats = sizeof(dec_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat)
	},

	.max_constrained_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_main_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_high_profile_level = IMX_VPU_API_H264_LEVEL_4_1,
	.max_high10_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,

	.flags = IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED | IMX_VPU_API_H264_FLAG_ACCESS_UNITS_REQUIRED
};

static ImxVpuApiVP8SupportDetails const dec_vp8_support_details = {
	.parent = {
		.min_width = 8, .max_width = 1920,
		.min_height = 8, .max_height = 1088,
		.supported_color_formats = dec_supported_basic_color_formats,
		.num_supported_color_formats = sizeof(dec_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat)
	},

	.supported_profiles = (1 << IMX_VPU_API_VP8_PROFILE_0)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_1)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_2)
	                    | (1 << IMX_VPU_API_VP8_PROFILE_3)
};

ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_dec_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&dec_h264_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&dec_vp8_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			return &dec_jpeg_support_details;

		default:
			return &dec_basic_compression_format_support_details;
	}

	return NULL;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_open(ImxVpuApiDecoder **decoder, ImxVpuApiDecOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	int err;
	ImxVpuApiDecReturnCodes ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	DecOpenParam dec_open_param;
	RetCode dec_ret;

	assert(decoder != NULL);
	assert(open_params != NULL);
	assert(stream_buffer != NULL);


	/* Check that the allocated stream buffer is big enough */
	{
		size_t stream_buffer_size = imx_dma_buffer_get_size(stream_buffer);
		if (stream_buffer_size < VPU_DEC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE) 
		{
			IMX_VPU_API_ERROR("stream buffer size is %zu bytes; need at least %zu bytes", stream_buffer_size, (size_t)VPU_DEC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE);
			return IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE;
		}
	}


	/* Verify extra header data */
	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
		{
			if (open_params->extra_header_data == NULL)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data, but none has been set");
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA;
			}

			if (open_params->extra_header_data_size < 4)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data size of 4 bytes, got %u byte(s)", open_params->extra_header_data_size);
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA;
			}

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
		{
			if (open_params->extra_header_data == NULL)
			{
				IMX_VPU_API_ERROR("WVC1 input expects extra header data, but none has been set");
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA;
			}

			if (open_params->extra_header_data_size < 1)
			{
				IMX_VPU_API_ERROR("WMV3 input expects extra header data size of at least 1 byte, got %u byte(s)", open_params->extra_header_data_size);
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_EXTRA_HEADER_DATA;
			}

			break;
		}

		default:
			break;
	}


	/* Allocate decoder instance. */
	*decoder = malloc(sizeof(ImxVpuApiDecoder));
	assert((*decoder) != NULL);


	/* Set default decoder values. */
	memset(*decoder, 0, sizeof(ImxVpuApiDecoder));


	/* Open the IPU VDOA FD. We'll need this in 
	 * imx_vpu_api_dec_get_decoded_frame() to detile
	 * and copy the decoded frames. */
	if (((*decoder)->ipu_vdoa_fd = imx_vpu_api_imx6_coda_open_ipu_voda_fd()) < 0)
	{
		ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto cleanup;
	}


	/* Map the stream buffer. We need to keep it mapped always so we can
	 * keep updating it. It is mapped as readwrite so we can shift data
	 * inside it later with memmove() if necessary.
	 * Mapping this with IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC since
	 * the stream buffer stays mapped until the decoder is closed, and
	 * we do copy encoded data into the stream buffer. Also see the
	 * imx_dma_buffer_start_sync_session() / imx_dma_buffer_stop_sync_session()
	 * calls in imx_vpu_api_dec_push_encoded_frame(). */
	(*decoder)->stream_buffer_virtual_address = imx_dma_buffer_map(stream_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE | IMX_DMA_BUFFER_MAPPING_FLAG_READ | IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC, &err);
	if ((*decoder)->stream_buffer_virtual_address == NULL)
	{
			IMX_VPU_API_ERROR("mapping stream buffer to virtual address space failed: %s (%d)", strerror(err), err);
			ret = IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
	}

	(*decoder)->stream_buffer_physical_address = imx_dma_buffer_get_physical_address(stream_buffer);
	(*decoder)->stream_buffer = stream_buffer;


	/* Make a copy of the open_params for later use. */
	(*decoder)->open_params = *open_params;


	/* Fill in values into the VPU's decoder open param structure */
	memset(&dec_open_param, 0, sizeof(dec_open_param));
	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			dec_open_param.bitstreamFormat = STD_AVC;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG2:
			dec_open_param.bitstreamFormat = STD_MPEG2;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			dec_open_param.bitstreamFormat = STD_MPEG4;
			dec_open_param.mp4Class = 0;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_H263:
			dec_open_param.bitstreamFormat = STD_H263;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_WMV3:
			dec_open_param.bitstreamFormat = STD_VC1;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_WVC1:
			dec_open_param.bitstreamFormat = STD_VC1;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			dec_open_param.bitstreamFormat = STD_MJPG;
			break;
		case IMX_VPU_API_COMPRESSION_FORMAT_VP8:
			dec_open_param.bitstreamFormat = STD_VP8;
			break;
		default:
			IMX_VPU_API_ERROR("unknown compression format");
			ret = IMX_VPU_API_DEC_RETURN_CODE_UNSUPPORTED_COMPRESSION_FORMAT;
			goto cleanup;
	}

	dec_open_param.bitstreamBuffer = (*decoder)->stream_buffer_physical_address;
	dec_open_param.bitstreamBufferSize = VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE;
	dec_open_param.qpReport = 0;
	dec_open_param.mp4DeblkEnable = 0;
	dec_open_param.chromaInterleave = decoder_uses_semi_planar_color_format(open_params);
	dec_open_param.filePlayEnable = 0;
	dec_open_param.picWidth = open_params->frame_width;
	dec_open_param.picHeight = open_params->frame_height;
	dec_open_param.avcExtension = 0;
	dec_open_param.dynamicAllocEnable = 0;
	dec_open_param.streamStartByteOffset = 0;
	dec_open_param.mjpg_thumbNailDecEnable = 0;
	dec_open_param.psSaveBuffer = (*decoder)->stream_buffer_physical_address + VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE + VPU_MAX_SLICE_BUFFER_SIZE;
	dec_open_param.psSaveBufferSize = VPU_PS_SAVE_BUFFER_SIZE;
	/* 0 = linear map.
	 * 1 = frame tiled map.
	 * 2 = field tiled map.
	 * We don't use linear mapping, since the IPU detiled later in the
	 * imx_vpu_dec_get_decoded_frame() function. The exception is JPEG,
	 * since the VPU decodes it differently - it decodes JPEGs to the
	 * framebuffer the JPEG rotator is set to. */
	dec_open_param.mapType = (open_params->compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG) ? 0 : 1;
	 /* If this is not 0, the VPU may hang eventually (it is 0 in the NXP
	  * wrapper except for MX6X). Since we anyway don't want linear data,
	  * we keep it at 0. */
	dec_open_param.tiled2LinearEnable = 0;
	dec_open_param.bitstreamMode = 1;
	dec_open_param.reorderEnable = !!(open_params->flags & IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING);

	/* Motion-JPEG specific settings
	 * With motion JPEG, the VPU is configured to operate in line buffer mode,
	 * because it is easier to handle. During decoding, pointers to the
	 * beginning of the JPEG data inside the bitstream buffer have to be set,
	 * which is much simpler if the VPU operates in line buffer mode (one then
	 * has to only set the pointers to refer to the beginning of the bitstream
	 * buffer, since in line buffer mode, this is where the encoded frame
	 * is always placed*/
	if (open_params->compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		dec_open_param.jpgLineBufferMode = 1;
		/* This one is not mentioned in the specs for some reason,
		 * but is required for motion JPEG to work */
		dec_open_param.pBitStream = (*decoder)->stream_buffer_virtual_address;
	}
	else
		dec_open_param.jpgLineBufferMode = 0;


	/* Now actually open the decoder instance. */
	IMX_VPU_API_DEBUG("opening decoder, frame size: %u x %u pixel", open_params->frame_width, open_params->frame_height);
	imx_coda_vpu_load();
	dec_ret = vpu_DecOpen(&((*decoder)->handle), &dec_open_param);
	if (dec_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not open decoder: %s", retcode_to_string(dec_ret));
		ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		goto cleanup;
	}


	/* Finish & cleanup (the latter in case of an error). */
finish:
	if (ret == IMX_VPU_API_DEC_RETURN_CODE_OK)
		IMX_VPU_API_DEBUG("successfully opened decoder");

	return ret;

cleanup:
	if ((*decoder) != NULL)
	{
		if ((*decoder)->ipu_vdoa_fd >= 0)
		{
			imx_vpu_api_imx6_coda_close_ipu_voda_fd((*decoder)->ipu_vdoa_fd);
			(*decoder)->ipu_vdoa_fd = -1;
		}

		if ((*decoder)->stream_buffer_virtual_address != NULL)
			imx_dma_buffer_unmap((*decoder)->stream_buffer);
		free(*decoder);
		*decoder = NULL;
	}

	goto finish;
}


void imx_vpu_api_dec_close(ImxVpuApiDecoder *decoder)
{
	RetCode dec_ret;

	assert(decoder != NULL);

	IMX_VPU_API_DEBUG("closing decoder");


	/* Flush the VPU bit buffer if we registered framebuffers earlier.
	 * Calling vpu_DecBitBufferFlush() without registered framebuffers
	 * leads to a "wrong call sequence error" that, while inconsequential
	 * here (because we are closing the decoder already anyway), can
	 * alert users unnecessarily when looking at the logs.
	 * Also, flushing is not done when decoding JPEG, since it
	 * is unnecessary and can lead to errors as well. */
	if ((decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG) && (decoder->internal_framebuffers != NULL))
	{
		dec_ret = vpu_DecBitBufferFlush(decoder->handle);
		if (dec_ret != RETCODE_SUCCESS)
			IMX_VPU_API_ERROR("could not flush decoder: %s", retcode_to_string(dec_ret));
	}

	/* Signal EOS to the decoder by passing 0 as size to vpu_DecUpdateBitstreamBuffer(). */
	dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, 0);
	if (dec_ret != RETCODE_SUCCESS)
		IMX_VPU_API_ERROR("could not signal EOS to the decoder: %s", retcode_to_string(dec_ret));

	/* Now, actually close the decoder. */
	dec_ret = vpu_DecClose(decoder->handle);
	if (dec_ret != RETCODE_SUCCESS)
		IMX_VPU_API_ERROR("could not close decoder: %s", retcode_to_string(dec_ret));

	imx_coda_vpu_unload();


	/* Close the IPU VDOA FD. */
	if (decoder->ipu_vdoa_fd >= 0)
	{
		imx_vpu_api_imx6_coda_close_ipu_voda_fd(decoder->ipu_vdoa_fd);
		decoder->ipu_vdoa_fd = -1;
	}


	/* Remaining cleanup */

	if (decoder->stream_buffer != NULL)
		imx_dma_buffer_unmap(decoder->stream_buffer);

	imx_vpu_api_dec_free_internal_arrays(decoder);

	free(decoder);
}


ImxVpuApiDecStreamInfo const * imx_vpu_api_dec_get_stream_info(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	return &(decoder->stream_info);
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_add_framebuffers_to_pool(ImxVpuApiDecoder *decoder, ImxDmaBuffer **fb_dma_buffers, void **fb_contexts, size_t num_framebuffers)
{
	unsigned int i;
	ImxVpuApiDecReturnCodes ret;
	RetCode dec_ret;
	DecBufInfo buf_info;
	ImxVpuApiFramebufferMetrics *fb_metrics;

	assert(decoder != NULL);
	assert(fb_dma_buffers != NULL);
	assert(num_framebuffers >= 1);

	fb_metrics = &(decoder->stream_info.decoded_frame_framebuffer_metrics);

	/* This function is only supposed to be called after new
	 * stream info was announced, which happens exactly once
	 * after beginning to decode a new stream. */
	if (decoder->num_framebuffers_to_be_added == 0)
	{
		IMX_VPU_API_ERROR("tried to add framebuffers before it was requested");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	if (num_framebuffers < decoder->num_framebuffers_to_be_added)
	{
		IMX_VPU_API_ERROR("decoder needs %zu framebuffers to be added, got %zu", decoder->num_framebuffers_to_be_added, num_framebuffers);
		return IMX_VPU_API_DEC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS;
	}

	assert(decoder->internal_framebuffers == NULL);


	/* Allocate memory for framebuffer structures and contexts. */

	decoder->internal_framebuffers = malloc(sizeof(FrameBuffer) * num_framebuffers);
	assert(decoder->internal_framebuffers != NULL);

	decoder->frame_entries = malloc(sizeof(DecFrameEntry) * num_framebuffers);
	assert(decoder->frame_entries != NULL);

	decoder->num_framebuffers = num_framebuffers;


	/* Copy the values from the framebuffers array to the internal_framebuffers
	 * one, which in turn will be used by the VPU. */
	memset(decoder->internal_framebuffers, 0, sizeof(FrameBuffer) * num_framebuffers);
	for (i = 0; i < num_framebuffers; ++i)
	{
		uintptr_t y_address, uv_address;
		imx_physical_address_t phys_addr;
		ImxDmaBuffer *fb_dma_buffer = fb_dma_buffers[i];
		FrameBuffer *internal_fb = &(decoder->internal_framebuffers[i]);
		DecFrameEntry *frame_entry = &(decoder->frame_entries[i]);

		phys_addr = imx_dma_buffer_get_physical_address(fb_dma_buffer);
		if (phys_addr == 0)
		{
			IMX_VPU_API_ERROR("could not get physical address for DMA buffer %u/%u", i, num_framebuffers);
			ret = IMX_VPU_API_DEC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
		}

		internal_fb->strideY = fb_metrics->y_stride;
		internal_fb->strideC = fb_metrics->uv_stride;
		internal_fb->myIndex = i;

		y_address = (uintptr_t)(phys_addr + decoder->y_offset);
		uv_address = (uintptr_t)(phys_addr + decoder->u_offset);
		assert(y_address <= 0xFFFFFFFFu);
		assert(uv_address <= 0xFFFFFFFFu);
		internal_fb->bufY = (PhysicalAddress)((y_address & ~0xFFF) | (uv_address >> 20));
		internal_fb->bufCb = (PhysicalAddress)(((uv_address >> 12) & 0xFF) << 24);
		internal_fb->bufCr = 0;
		internal_fb->bufMvCol = (PhysicalAddress)(phys_addr + decoder->mvcol_offset);

		frame_entry->frame_context = NULL;
		frame_entry->mode = DecFrameEntryMode_Free;

		frame_entry->fb_dma_buffer = fb_dma_buffer;
		frame_entry->fb_context = (fb_contexts != NULL) ? fb_contexts[i] : NULL;
	}


	/* Initialize the extra AVC slice buf info; its DMA buffer backing store is
	 * located inside the bitstream buffer, right after the actual bitstream content. */
	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.avcSliceBufInfo.bufferBase = decoder->stream_buffer_physical_address + VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE;
	buf_info.avcSliceBufInfo.bufferSize = VPU_MAX_SLICE_BUFFER_SIZE;
	buf_info.vp8MbDataBufInfo.bufferBase = decoder->stream_buffer_physical_address + VPU_DEC_MAIN_BITSTREAM_BUFFER_SIZE;
	buf_info.vp8MbDataBufInfo.bufferSize = VPU_VP8_MB_PRED_BUFFER_SIZE;

	/* The actual registration. */
	if (decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		dec_ret = vpu_DecRegisterFrameBuffer(
			decoder->handle,
			decoder->internal_framebuffers,
			num_framebuffers,
			fb_metrics->y_stride,
			&buf_info
		);

		if (dec_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("vpu_DecRegisterFrameBuffer() error: %s", retcode_to_string(dec_ret));
			ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			goto cleanup;
		}
	}

	/* We just registered the framebuffers, so we don't
	 * need any more to be added. (In fact, at this point,
	 * it is not even possible to do so - VPU limitation.) */
	decoder->num_framebuffers_to_be_added = 0;

	/* Set default rotator settings for JPEG, disabling any postprocessing. */
	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		/* the datatypes are int, but this is undocumented; determined by looking
		 * into the imx-vpu library's vpu_lib.c vpu_DecGiveCommand() definition. */
		int rotation_angle = 0;
		int mirror = 0;
		int stride = fb_metrics->y_stride;

		vpu_DecGiveCommand(decoder->handle, SET_ROTATION_ANGLE, (void *)(&rotation_angle));
		vpu_DecGiveCommand(decoder->handle, SET_MIRROR_DIRECTION,(void *)(&mirror));
		vpu_DecGiveCommand(decoder->handle, SET_ROTATOR_STRIDE, (void *)(&stride));
	}

	return IMX_VPU_API_DEC_RETURN_CODE_OK;

cleanup:
	imx_vpu_api_dec_free_internal_arrays(decoder);

	return ret;
}


void imx_vpu_api_dec_enable_drain_mode(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);

	if (decoder->drain_mode_enabled)
		return;

	decoder->drain_mode_enabled = TRUE;

	/* We still need to let the VPU know about the drain more.
	 * This is done by calling vpu_DecUpdateBitstreamBuffer()
	 * during the decoding process, with a byte size of 0. */
	decoder->drain_eos_sent_to_vpu = FALSE;

	IMX_VPU_API_DEBUG("enabled decoder drain mode");
}


int imx_vpu_api_dec_is_drain_mode_enabled(ImxVpuApiDecoder *decoder)
{
	assert(decoder != NULL);
	return decoder->drain_mode_enabled;
}


void imx_vpu_api_dec_flush(ImxVpuApiDecoder *decoder)
{
	RetCode dec_ret;
	unsigned int i;

	assert(decoder != NULL);

	IMX_VPU_API_DEBUG("flushing decoder");


	if (decoder->frame_entries == NULL)
	{
		IMX_VPU_API_DEBUG("attempted to flush, but there are no framebuffers in the pool; ignoring call");
		return;
	}


	/* No need to flush anything with WMV3 data. */
	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_WMV3)
	{
		IMX_VPU_API_DEBUG("WMV3 requires no flushing to be done");
		return;
	}


	IMX_VPU_API_DEBUG("flushing decoder");


	/* Clear any framebuffers that aren't ready for display yet but
	 * are being used for decoding (since flushing will clear them). */
	for (i = 0; i < decoder->num_framebuffers; ++i)
	{
		if (decoder->frame_entries[i].mode == DecFrameEntryMode_ReservedForDecoding)
		{
			dec_ret = vpu_DecClrDispFlag(decoder->handle, i);
			if (dec_ret != RETCODE_SUCCESS)
				IMX_VPU_API_ERROR("vpu_DecClrDispFlag() error while flushing: %s", retcode_to_string(dec_ret));
			decoder->frame_entries[i].mode = DecFrameEntryMode_Free;
		}
	}


	/* Perform the actual flush. */
	dec_ret = vpu_DecBitBufferFlush(decoder->handle);
	if (dec_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("vpu_DecBitBufferFlush() error while flushing: %s", retcode_to_string(dec_ret));
	}


	/* After the flush, any context will be thrown away */
	for (i = 0; i < decoder->num_framebuffers; ++i)
		decoder->frame_entries[i].frame_context = NULL;

	decoder->jpeg_format_changed = FALSE;
	decoder->num_used_framebuffers = 0;
	decoder->staged_encoded_frame_set = FALSE;

	decoder->drain_mode_enabled = FALSE;
	decoder->drain_eos_sent_to_vpu = FALSE;

	IMX_VPU_API_DEBUG("flushed decoder");
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_push_encoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	if (decoder->drain_mode_enabled)
	{
		IMX_VPU_API_ERROR("tried to push an encoded frame after drain mode was enabled");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	if (decoder->staged_encoded_frame_set)
	{
		IMX_VPU_API_ERROR("tried to push an encoded frame before a previous one was decoded");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}

	/* Begin synced access since we have to copy the encoded
	 * data into the stream buffer. */
	imx_dma_buffer_start_sync_session(decoder->stream_buffer);

	/* Process input data first to make sure any headers are
	 * inserted and any necessary parsing is done before the
	 * main frame. */
	if (!imx_vpu_api_dec_preprocess_input_data(decoder, decoder->open_params.extra_header_data, decoder->open_params.extra_header_data_size, encoded_frame->data, encoded_frame->data_size))
		return IMX_VPU_API_DEC_RETURN_CODE_ERROR;

		/* Handle main frame data. */
	if (!imx_vpu_api_dec_push_input_data(decoder, encoded_frame->data, encoded_frame->data_size))
		return IMX_VPU_API_DEC_RETURN_CODE_ERROR;

	IMX_VPU_API_LOG("staged encoded frame");

	/* Stage the encoded frame. We cannot insert its details
	 * into the frame entries array right here, since we don't
	 * know yet what framebuffer the VPU will pick for decoding
	 * into. We'll learn that later (the DecOutputInfo's
	 * indexFrameDecoded field will tell us). */
	decoder->staged_encoded_frame = *encoded_frame;
	decoder->staged_encoded_frame_set = TRUE;

	decoder->encoded_data_got_pushed = TRUE;

	imx_dma_buffer_stop_sync_session(decoder->stream_buffer);

	return IMX_VPU_API_DEC_RETURN_CODE_OK;
}


void imx_vpu_api_dec_set_output_frame_dma_buffer(ImxVpuApiDecoder *decoder, ImxDmaBuffer *output_frame_dma_buffer, void *fb_context)
{
	assert(decoder != NULL);
	assert(output_frame_dma_buffer != NULL);

	ImxVpuApiFramebufferMetrics *fb_metrics = &(decoder->stream_info.decoded_frame_framebuffer_metrics);
	imx_physical_address_t phys_addr = imx_dma_buffer_get_physical_address(output_frame_dma_buffer);

	decoder->output_frame_dma_buffer = output_frame_dma_buffer;
	decoder->output_frame_fb_context = fb_context;

	decoder->output_framebuffer.bufY = (PhysicalAddress)(phys_addr + fb_metrics->y_offset);
	decoder->output_framebuffer.bufCb = (PhysicalAddress)(phys_addr + fb_metrics->u_offset);
	decoder->output_framebuffer.bufCr = (PhysicalAddress)(phys_addr + fb_metrics->v_offset);

	/* MvCol is not needed for output framebuffers, since the IPU is what
	 * writes to these frames, so the additional MvCol space would be wasted. */
	decoder->output_framebuffer.bufMvCol = (PhysicalAddress)(0);
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_decode(ImxVpuApiDecoder *decoder, ImxVpuApiDecOutputCodes *output_code)
{
	assert(decoder != NULL);
	assert(output_code != NULL);

	*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE;


	if (decoder->drain_mode_enabled)
	{
		/* Drain mode is enabled. Make sure the VPU is informed. */

		if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		{
			/* There is no real drain mode for motion JPEG, since there
			 * is nothing to drain (JPEG frames are never delayed - the
			 * VPU decodes them as soon as they arrive). However, the
			 * VPU also does not report an EOS. So, do this manually. */
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}

		if (!(decoder->drain_eos_sent_to_vpu))
		{
			RetCode dec_ret;
			decoder->drain_eos_sent_to_vpu = TRUE;
			/* Calling vpu_DecUpdateBitstreamBuffer() with byte count 0
			 * signals the VPU that the end of stream was reached. */
			dec_ret = vpu_DecUpdateBitstreamBuffer(decoder->handle, 0);
			if (dec_ret != RETCODE_SUCCESS)
			{
				IMX_VPU_API_ERROR("could not signal EOS to the VPU: %s", retcode_to_string(dec_ret));
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}
		}
	}

	if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		/* JPEGs are a special case.
		 * The VPU does not report size or color format changes. Therefore,
		 * JPEG header have to be parsed here manually to retrieve the
		 * width, height, and color format and check if these changed.
		 * If so, inform the caller by setting the appropriate output codes. */

		if (decoder->jpeg_format_changed)
		{
			memset(&(decoder->stream_info), 0, sizeof(decoder->stream_info));

			if (!imx_vpu_api_dec_fill_stream_info(
				decoder,
				decoder->jpeg_width,
				decoder->jpeg_height,
				decoder->jpeg_color_format,
				0, 1,
				decoder->stream_info.min_num_required_framebuffers,
				FALSE)
			)
			{
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
			}

			/* Reset the internal frame arrays and the number of framebuffers
			 * to be added, since we need a new framebuffer pool after the
			 * JPEG format changed. */
			imx_vpu_api_dec_free_internal_arrays(decoder);

			decoder->num_framebuffers_to_be_added = decoder->stream_info.min_num_required_framebuffers;

			decoder->jpeg_format_changed = FALSE;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE;

			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}
	}


	/* Check if initial info needs to be announced to the caller. */

	if (!(decoder->initial_info_available) && decoder->encoded_data_got_pushed)
	{
		/* Initial has not yet be retrieved. Fetch it, and store it
		 * inside the decoder instance structure. */
		RetCode dec_ret = imx_vpu_api_dec_get_initial_info(decoder);
		switch (dec_ret)
		{
			case RETCODE_SUCCESS:
				break;

			case RETCODE_INVALID_HANDLE:
			case RETCODE_INVALID_PARAM:
			case RETCODE_FAILURE:
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;

			case RETCODE_FAILURE_TIMEOUT:
				IMX_VPU_API_ERROR("VPU reported timeout while retrieving initial info");
				return IMX_VPU_API_DEC_RETURN_CODE_TIMEOUT;

			case RETCODE_WRONG_CALL_SEQUENCE:
			case RETCODE_CALLED_BEFORE:
				 return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;

			default:
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}

		imx_vpu_api_dec_fill_stream_info_from_initial_info(decoder, &(decoder->initial_info));

		/* Framebuffers need to be added (actually, "registered" in case of
		 * the CODA VPU) after retrieving the initial info. */
		decoder->num_framebuffers_to_be_added = decoder->stream_info.min_num_required_framebuffers;

		*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE;

		return IMX_VPU_API_DEC_RETURN_CODE_OK;
	}


	/* Start the frame decoding process. */

	{
		RetCode dec_ret;
		DecParam params;
		BOOL timeout;
		BOOL skipped_frame_is_internal = FALSE;

		if (!(decoder->drain_mode_enabled))
		{
			if (!(decoder->staged_encoded_frame_set))
			{
				IMX_VPU_API_LOG("no encoded frame staged");
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
				return IMX_VPU_API_DEC_RETURN_CODE_OK;
			}

			if (decoder->output_frame_dma_buffer == NULL)
			{
				IMX_VPU_API_ERROR("no output frame buffer set");
				return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
			}
		}

		if (decoder->initial_info_available && (decoder->frame_entries == NULL))
		{
			IMX_VPU_API_ERROR("no framebuffers have been added to the pool");
			return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
		}

		memset(&params, 0, sizeof(params));

		if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		{
			/* There is an error in the specification. It states that chunkSize
			 * is not used in the i.MX6. This is untrue; for motion JPEG, this
			 * must be nonzero. */
			params.chunkSize = decoder->staged_encoded_frame.data_size;

			/* Set the virtual and physical memory pointers that point to the
			 * start of the frame. These always point to the beginning of the
			 * bitstream buffer, because the VPU operates in line buffer mode
			 * when decoding motion JPEG data. */
			params.virtJpgChunkBase = (unsigned char *)(decoder->stream_buffer_virtual_address);
			params.phyJpgChunkBase = decoder->stream_buffer_physical_address;

			vpu_DecGiveCommand(decoder->handle, SET_ROTATOR_OUTPUT, (void *)(&(decoder->output_framebuffer)));
		}

		/* XXX: currently, iframe search and skip frame modes are not supported */


		/* Start frame decoding.
		 * The error handling code below does dummy vpu_DecGetOutputInfo() calls
		 * before exiting. This is done because according to the documentation,
		 * vpu_DecStartOneFrame() "locks out" most VPU calls until
		 * vpu_DecGetOutputInfo() is called, so this must be called *always*
		 * after vpu_DecStartOneFrame(), even if an error occurred. */
		dec_ret = vpu_DecStartOneFrame(decoder->handle, &params);

		switch (dec_ret)
		{
			case RETCODE_SUCCESS:
				break;

			case RETCODE_JPEG_BIT_EMPTY:
				/* Special handling of the insufficient data case for JPEG. */
				vpu_DecGetOutputInfo(decoder->handle, &(decoder->dec_output_info));
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
				return IMX_VPU_API_DEC_RETURN_CODE_OK;

			case RETCODE_JPEG_EOS:
				/* Special EOS handling for JPEG. This is not an error, and
				 * since we are at end-of-stream, there is no point in trying
				 * to get output info (there is no output). */
				*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
				dec_ret = RETCODE_SUCCESS;
				break;

			default:
				IMX_VPU_API_ERROR("vpu_DecStartOneFrame() error: %s", retcode_to_string(dec_ret));
				vpu_DecGetOutputInfo(decoder->handle, &(decoder->dec_output_info));
				return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}


		/* Wait for frame completion. */
		{
			int cnt;

			IMX_VPU_API_LOG("waiting for decoding completion");

			/* Wait a few times, since sometimes, it takes more than
			 * one vpu_WaitForInt() call to cover the decoding interval. */
			timeout = TRUE;
			for (cnt = 0; cnt < VPU_MAX_TIMEOUT_COUNTS; ++cnt)
			{
				if (vpu_WaitForInt(VPU_WAIT_TIMEOUT) != RETCODE_SUCCESS)
				{
					IMX_VPU_API_INFO("timeout after waiting %d ms for frame completion", VPU_WAIT_TIMEOUT);
				}
				else
				{
					timeout = FALSE;
					break;
				}
			}
		}


		/* Retrieve information about the result of the decode process There may be no
		 * decoded frame yet though; this only finishes processing the input frame. In
		 * case of formats like h.264, it may take several input frames until output
		 * frames start coming out. However, the output information does contain valuable
		 * data even at the beginning, like which framebuffer in the framebuffer array
		 * is used for decoding the frame into.
		 *
		 * Also, vpu_DecGetOutputInfo() is called even if a timeout occurred. This is
		 * intentional, since according to the VPU docs, vpu_DecStartOneFrame() won't be
		 * usable again until vpu_DecGetOutputInfo() is called. In other words, the
		 * vpu_DecStartOneFrame() locks down some internals inside the VPU, and
		 * vpu_DecGetOutputInfo() releases them. */

		dec_ret = vpu_DecGetOutputInfo(decoder->handle, &(decoder->dec_output_info));
		if (dec_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("vpu_DecGetOutputInfo() error: %s", retcode_to_string(dec_ret));
			return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}


		/* If a timeout occurred earlier, this is the correct time to abort
		 * decoding and return an error code, since vpu_DecGetOutputInfo()
		 * has been called, unlocking the VPU decoder calls. */
		if (timeout)
			return IMX_VPU_API_DEC_RETURN_CODE_TIMEOUT;


		/* Log some information about the decoded frame */
		IMX_VPU_API_LOG(
			"output info:  indexFrameDisplay %d  indexFrameDecoded %d  NumDecFrameBuf %d  picType %d  idrFlg %d  numOfErrMBs %d  hScaleFlag %d  vScaleFlag %d  notSufficientPsBuffer %d  notSufficientSliceBuffer %d  decodingSuccess %d  interlacedFrame %d  mp4PackedPBframe %d  h264Npf %d  pictureStructure %d  topFieldFirst %d  repeatFirstField %d  fieldSequence %d  decPicWidth %d  decPicHeight %d",
			decoder->dec_output_info.indexFrameDisplay,
			decoder->dec_output_info.indexFrameDecoded,
			decoder->dec_output_info.NumDecFrameBuf,
			decoder->dec_output_info.picType,
			decoder->dec_output_info.idrFlg,
			decoder->dec_output_info.numOfErrMBs,
			decoder->dec_output_info.hScaleFlag,
			decoder->dec_output_info.vScaleFlag,
			decoder->dec_output_info.notSufficientPsBuffer,
			decoder->dec_output_info.notSufficientSliceBuffer,
			decoder->dec_output_info.decodingSuccess,
			decoder->dec_output_info.interlacedFrame,
			decoder->dec_output_info.mp4PackedPBframe,
			decoder->dec_output_info.h264Npf,
			decoder->dec_output_info.pictureStructure,
			decoder->dec_output_info.topFieldFirst,
			decoder->dec_output_info.repeatFirstField,
			decoder->dec_output_info.fieldSequence,
			decoder->dec_output_info.decPicWidth,
			decoder->dec_output_info.decPicHeight
		);


		/* VP8 requires some workarounds. */
		if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_VP8)
		{
			if ((decoder->dec_output_info.indexFrameDecoded >= 0) && (decoder->dec_output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY))
			{
				/* Internal invisible frames are supposed to be used for decoding only,
				 * so if we encounter one, don't output it, and drop it instead. To that
				 * end, set the index values to resemble indices used for dropped frames
				 * to make sure the dropped frames block below thinks this frame got
				 * dropped by the VPU. */
				IMX_VPU_API_DEBUG("skip internal invisible frame for VP8");
				decoder->dec_output_info.indexFrameDecoded = VPU_DECODER_DECODEIDX_FRAME_NOT_DECODED;
				decoder->dec_output_info.indexFrameDisplay = VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY;
				skipped_frame_is_internal = TRUE;
			}
		}

		/* JPEG requires frame index adjustments, since JPEG decoding
		 * uses no framebuffer pool, and we explicitely allocated a
		 * 1-item array in imx_vpu_api_dec_open() for JPEG decoding
		 * so we can store information about the encoded JPEG frame. */
		if (decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		{
			decoder->dec_output_info.indexFrameDecoded = 0;
			decoder->dec_output_info.indexFrameDisplay = 0;
			skipped_frame_is_internal = TRUE;
		}

		/* Check if video sequence parameters changed. If so, abort any
		 * additional checks and processing; the decoder has to be drained
		 * and reopened to support the changed parameters. */
		if (decoder->dec_output_info.decodingSuccess & (1 << 20))
		{
			IMX_VPU_API_DEBUG("video sequence parameters changed");
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED;
			return IMX_VPU_API_DEC_RETURN_CODE_OK;
		}

		/* Check if there were enough output framebuffers. */
		if (decoder->dec_output_info.indexFrameDecoded == VPU_DECODER_DECODEIDX_ALL_FRAMES_DECODED)
		{
			/* This indicates an internal error. Usually this means
			 * the value of NUM_EXTRA_FRAMEBUFFERS_REQUIRED is not
			 * high enough. This is an internal error. */
			IMX_VPU_API_ERROR("internal error; not enough output framebuffers were available even though enough were added prior to decoding");
			return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}

		/* Check if decoding was incomplete (bit #0 is then 0, bit #4 1).
		 * Incomplete decoding indicates incomplete input data, and we cannot
		 * handle that (libimxvpuapi expects complete frames). This can for
		 * example happen with RTSP streams where the first frame is incomplete. */
		if (decoder->dec_output_info.decodingSuccess & (1 << 4))
		{
			decoder->skipped_frame_context = decoder->staged_encoded_frame.context;
			decoder->skipped_frame_pts = decoder->staged_encoded_frame.pts;
			decoder->skipped_frame_dts = decoder->staged_encoded_frame.dts;
			decoder->skipped_frame_reason = IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_CORRUPTED_FRAME;
			IMX_VPU_API_DEBUG("dropping frame because it is corrupted/incomplete (context: %p pts %" PRIu64 " dts %" PRIu64 ")", decoder->skipped_frame_context, decoder->skipped_frame_pts, decoder->skipped_frame_dts);
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;
			decoder->staged_encoded_frame_set = FALSE;
		}

		/* Report dropped frames. However, if the code determined the input data
		 * to be insufficient, or if the code already determined this input frame
		 * as to be skipped, don't report. */
		if (
		  (((*output_code) & (IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED | IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED)) == 0) &&
		  (decoder->dec_output_info.indexFrameDecoded == VPU_DECODER_DECODEIDX_FRAME_NOT_DECODED) &&
		  (
		    (decoder->dec_output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY) ||
		    (decoder->dec_output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_SKIP_MODE_NO_FRAME_TO_DISPLAY)
		  )
		)
		{
			decoder->skipped_frame_context = decoder->staged_encoded_frame.context;
			decoder->skipped_frame_pts = decoder->staged_encoded_frame.pts;
			decoder->skipped_frame_dts = decoder->staged_encoded_frame.dts;
			decoder->skipped_frame_reason = skipped_frame_is_internal ? IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_INTERNAL_FRAME : IMX_VPU_API_DEC_SKIPPED_FRAME_REASON_CORRUPTED_FRAME;
			IMX_VPU_API_DEBUG("frame got skipped/dropped (context: %p pts %" PRIu64 " dts %" PRIu64 ")", decoder->skipped_frame_context, decoder->skipped_frame_pts, decoder->skipped_frame_dts);
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED;
			decoder->staged_encoded_frame_set = FALSE;
		}

		/* Check if information about the decoded frame is available.
		 * In particular, the index of the framebuffer where the frame is being
		 * decoded into is essential with formats like h.264, which allow for both
		 * delays between decoding and presentation, and reordering of frames.
		 * With the indexFrameDecoded value, it is possible to know which framebuffer
		 * is associated with what input buffer. This is necessary to properly
		 * associate context information which can later be retrieved again when a
		 * frame can be displayed.
		 * indexFrameDecoded can be negative, meaning there is no frame currently being
		 * decoded. This typically happens when the drain mode is enabled, since then,
		 * there will be no more input data. */

		if (decoder->dec_output_info.indexFrameDecoded >= 0)
		{
			ImxVpuApiFrameType *frame_types;
			int idx_decoded = decoder->dec_output_info.indexFrameDecoded;
			assert(idx_decoded < (int)(decoder->num_framebuffers));

			decoder->frame_entries[idx_decoded].frame_context = decoder->staged_encoded_frame.context;
			decoder->frame_entries[idx_decoded].pts = decoder->staged_encoded_frame.pts;
			decoder->frame_entries[idx_decoded].dts = decoder->staged_encoded_frame.dts;
			decoder->frame_entries[idx_decoded].mode = DecFrameEntryMode_ReservedForDecoding;
			decoder->frame_entries[idx_decoded].interlacing_mode = convert_interlacing_mode(decoder->open_params.compression_format, &(decoder->dec_output_info));

			/* XXX: The VPU documentation seems to be incorrect about IDR types.
			 * There is an undocumented idrFlg field which is also used by the
			 * VPU wrapper. If this flag's first bit is set, then this is an IDR
			 * frame, otherwise it is a non-IDR one. The non-IDR case is then
			 * handled in the default way (see convert_frame_type() for details). */
			frame_types = &(decoder->frame_entries[idx_decoded].frame_types[0]);
			if ((decoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_H264) && (decoder->dec_output_info.idrFlg & 0x01))
				frame_types[0] = frame_types[1] = IMX_VPU_API_FRAME_TYPE_IDR;
			else
				convert_frame_type(decoder->open_params.compression_format, decoder->dec_output_info.picType, !!(decoder->dec_output_info.interlacedFrame), frame_types);

			IMX_VPU_API_LOG("staged frame reported as decoded; unstaging");
			decoder->staged_encoded_frame_set = FALSE;

			decoder->num_used_framebuffers++;
		}


		/* Check if information about a displayable frame is available.
		 * A frame can be presented when it is fully decoded. In that case,
		 * indexFrameDisplay is >= 0. If no fully decoded and displayable
		 * frame exists (yet), indexFrameDisplay is -2 or -3 (depending on the
		 * currently enabled frame skip mode). If indexFrameDisplay is -1,
		 * all frames have been decoded. This typically happens after drain
		 * mode was enabled.
		 * This index is later used to retrieve the context that was associated
		 * with the input data that corresponds to the decoded and displayable
		 * frame (see above). available_decoded_frame_idx stores the index for
		 * this precise purpose. Also see imx_vpu_dec_get_decoded_frame(). */

		if (decoder->dec_output_info.indexFrameDisplay >= 0)
		{
			DecFrameEntry *entry;
			int idx_display = decoder->dec_output_info.indexFrameDisplay;
			assert(idx_display < (int)(decoder->num_framebuffers));

			entry = &(decoder->frame_entries[idx_display]);

			IMX_VPU_API_LOG("decoded and displayable frame available (framebuffer display index: %d context: %p pts: %" PRIu64 " dts: %" PRIu64 ")", idx_display, entry->frame_context, entry->pts, entry->dts);

			entry->mode = DecFrameEntryMode_ContainsDisplayableFrame;

			decoder->available_decoded_frame_idx = idx_display;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE;
		}
		else if (decoder->dec_output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_ALL_FRAMES_DISPLAYED)
		{
			IMX_VPU_API_LOG("EOS reached");
			decoder->available_decoded_frame_idx = -1;
			decoder->drain_mode_enabled = FALSE;
			*output_code = IMX_VPU_API_DEC_OUTPUT_CODE_EOS;
		}
		else
		{
			IMX_VPU_API_LOG("nothing yet to display ; indexFrameDisplay: %d", decoder->dec_output_info.indexFrameDisplay);
		}
	}

	return IMX_VPU_API_DEC_RETURN_CODE_OK;
}


ImxVpuApiDecReturnCodes imx_vpu_api_dec_get_decoded_frame(ImxVpuApiDecoder *decoder, ImxVpuApiRawFrame *decoded_frame)
{
	ImxVpuApiDecReturnCodes ret = IMX_VPU_API_DEC_RETURN_CODE_OK;
	int idx;

	assert(decoder != NULL);
	assert(decoded_frame != NULL);


	/* available_decoded_frame_idx < 0 means there is no frame
	 * to retrieve yet, or the frame was already retrieved. */
	if (decoder->available_decoded_frame_idx < 0)
	{
		IMX_VPU_API_ERROR("no decoded frame available, or function was already called earlier");
		return IMX_VPU_API_DEC_RETURN_CODE_INVALID_CALL;
	}


	/* The code in imx_vpu_api_dec_decode() already checks for
	 * this and returns an error code if it isn't set. So, if
	 * at this point this is NULL, then something internal is
	 * wrong. */
	assert(decoder->output_frame_dma_buffer != NULL);


	idx = decoder->available_decoded_frame_idx;
	assert(idx < (int)(decoder->num_framebuffers));


	/* Detile and copy the frame out of the framebuffer pool into the output
	 * frame DMA buffer. For JPEG, this isn't necessary, since the JPEG
	 * rotator is configured to do that already. */
	if (decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		ImxVpuApiFramebufferMetrics *fb_metrics = &(decoder->stream_info.decoded_frame_framebuffer_metrics);

		if (!imx_vpu_api_imx6_coda_detile_and_copy_frame_with_ipu_vdoa(
			decoder->ipu_vdoa_fd,
			decoder->frame_entries[idx].fb_dma_buffer,
			decoder->output_frame_dma_buffer,
			decoder->total_padded_input_width, decoder->total_padded_input_height,
			decoder->total_padded_output_width, decoder->total_padded_output_height,
			fb_metrics->actual_frame_width, fb_metrics->actual_frame_height,
			decoder->stream_info.color_format
		))
		{
			IMX_VPU_API_ERROR("could not detile and copy decoded frame pixels");
			return IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}
	}


	decoded_frame->fb_dma_buffer = decoder->output_frame_dma_buffer;


	decoded_frame->fb_context = decoder->output_frame_fb_context;
	decoded_frame->frame_types[0] = decoder->frame_entries[idx].frame_types[0];
	decoded_frame->frame_types[1] = decoder->frame_entries[idx].frame_types[1];
	decoded_frame->interlacing_mode = decoder->frame_entries[idx].interlacing_mode;
	decoded_frame->context = decoder->frame_entries[idx].frame_context;
	decoded_frame->pts = decoder->frame_entries[idx].pts;
	decoded_frame->dts = decoder->frame_entries[idx].dts;


	/* Erase the context from context_for_frames after retrieval, and set
	 * available_decoded_frame_idx to -1 ; this ensures no erroneous
	 * double-retrieval can occur. */
	decoder->frame_entries[idx].frame_context = NULL;
	decoder->available_decoded_frame_idx = -1;


	/* Frame is no longer being used. */
	decoder->frame_entries[idx].mode = DecFrameEntryMode_Free;


	/* Mark it as displayed in the VPU. */
	if (decoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		RetCode dec_ret = vpu_DecClrDispFlag(decoder->handle, idx);
		if (dec_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("vpu_DecClrDispFlag() error: %s", retcode_to_string(dec_ret));
			ret = IMX_VPU_API_DEC_RETURN_CODE_ERROR;
		}
	}


	decoder->num_used_framebuffers--;


	return ret;
}


void imx_vpu_api_dec_return_framebuffer_to_decoder(ImxVpuApiDecoder *decoder, ImxDmaBuffer *fb_dma_buffer)
{
	IMX_VPU_API_UNUSED_PARAM(decoder);
	IMX_VPU_API_UNUSED_PARAM(fb_dma_buffer);
}


void imx_vpu_api_dec_get_skipped_frame_info(ImxVpuApiDecoder *decoder, ImxVpuApiDecSkippedFrameReasons *reason, void **context, uint64_t *pts, uint64_t *dts)
{
	if (reason != NULL)
		*reason = decoder->skipped_frame_reason;
	if (context != NULL)
		*context = decoder->skipped_frame_context;
	if (pts != NULL)
		*pts = decoder->skipped_frame_pts;
	if (dts != NULL)
		*dts = decoder->skipped_frame_dts;
}




/************************************************/
/******* ENCODER STRUCTURES AND FUNCTIONS *******/
/************************************************/


/* Indices in the header_data array. The array is shared between formats,
 * so the indices are only unique for the same format. */
#define ENC_HEADER_DATA_ENTRY_INDEX_H264_SPS_RBSP 0
#define ENC_HEADER_DATA_ENTRY_INDEX_H264_PPS_RBSP 1
#define ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOS_HEADER 0
#define ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VIS_HEADER 1
#define ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOL_HEADER 2
#define ENC_HEADER_DATA_MAX_NUM_ENTRY_INDICES 3

typedef struct
{
	uint8_t *data;
	size_t size;
}
EncHeaderData;

struct _ImxVpuApiEncoder
{
	EncHandle handle;

	/* Stream buffer (called "bitstream buffer" in the VPU documentation).
	 * Holds data coming from the encoder. */
	ImxDmaBuffer *stream_buffer;
	uint8_t *stream_buffer_virtual_address;
	imx_physical_address_t stream_buffer_physical_address;

	/* Copy of the open_params passed to imx_vpu_api_enc_open(). */
	ImxVpuApiEncOpenParams open_params;

	ImxVpuApiEncStreamInfo stream_info;

	/* DEPRECATED. This is kept here for backwards compatibility. */
	BOOL drain_mode_enabled;

	size_t num_framebuffers_to_be_added;

	/* Array of internal framebuffers used by the VPU for the encoding
	 * process. They store reference frames, partially reconstructed
	 * frames, etc. They are not to be accessed from the outside.
	 * The offset and stride value here are potentially different from
	 * those in stream_info.frame_encoding_framebuffer_metrics, because
	 * the internal framebuffers have different alignment requirements;
	 * both width and height must be an integer multiple of 16, while
	 * input frames have less strict alignment requirements. */
	unsigned int num_framebuffers;
	FrameBuffer *internal_framebuffers;
	size_t internal_fb_u_offset;
	size_t internal_fb_v_offset;
	size_t internal_fb_y_stride;
	size_t internal_fb_uv_stride;

	EncOutputInfo enc_output_info;
	size_t jpeg_header_size;

	BOOL prepend_header_to_frame;

	/* TRUE if the frame that is currently being encoder is the first one
	 * after calling imx_vpu_api_enc_open() or imx_vpu_api_enc_flush(). */
	BOOL first_frame;

	/* TRUE if h.264 access unit delimiters are used. */
	BOOL h264_aud_enabled;

	/* Compression format specific header data. */
	union
	{
		EncHeaderData main_header_data[ENC_HEADER_DATA_MAX_NUM_ENTRY_INDICES];

		/* This one is initialized and used differently to the other headers,
		 * so it is isolated here and not part of the array above. */
		uint8_t jpeg_header_data[JPEG_ENC_HEADER_DATA_MAX_SIZE];
	}
	headers;

	ImxVpuApiRawFrame staged_raw_frame;
	BOOL staged_raw_frame_set;

	/* Encoded frame data. */
	BOOL encoded_frame_available;
	void *encoded_frame_context;
	uint64_t encoded_frame_pts, encoded_frame_dts;
	ImxVpuApiFrameType encoded_frame_type;
	size_t encoded_frame_data_size;

	unsigned long frame_counter;
	unsigned long interval_between_idr_frames;
};

#define IMX_VPU_API_ENC_GET_STREAM_VIRT_ADDR(IMXVPUAPIENC, STREAM_PHYS_ADDR) ((IMXVPUAPIENC)->stream_buffer_virtual_address + ((PhysicalAddress)(STREAM_PHYS_ADDR) - (PhysicalAddress)((IMXVPUAPIENC)->stream_buffer_physical_address)))



static BOOL imx_vpu_api_enc_generate_all_header_data(ImxVpuApiEncoder *encoder);
static void imx_vpu_api_enc_free_all_header_data(ImxVpuApiEncoder *encoder);


static BOOL imx_vpu_api_enc_generate_header_data(ImxVpuApiEncoder *encoder, EncHeaderParam *enc_header_param, unsigned int header_data_entry, int header_type, CodecCommand codec_command, char const *description)
{
	RetCode enc_ret;
	EncHeaderData *enc_header_data;

	enc_header_data = &(encoder->headers.main_header_data[header_data_entry]);

	/* Instruct the VPU to generate the header data.
	 * It will be placed in the bitstream buffer. */
	enc_header_param->headerType = header_type;
	enc_ret = vpu_EncGiveCommand(encoder->handle, codec_command, enc_header_param);
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("header generation command failed: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		return FALSE;
	}

	assert(enc_header_param->size > 0);

	/* Allocate a memory block for the newly generated header data and
	 * copy it from the bitstream buffer into this new block. */
	enc_header_data->data = malloc(enc_header_param->size);
	assert(enc_header_data->data != NULL);
	memcpy(enc_header_data->data, encoder->stream_buffer_virtual_address + (enc_header_param->buf - encoder->stream_buffer_physical_address), enc_header_param->size);
	enc_header_data->size = enc_header_param->size;

	IMX_VPU_API_LOG("generated %s with %zu byte", description, enc_header_data->size);

	return TRUE;
}


static BOOL imx_vpu_api_enc_generate_all_header_data(ImxVpuApiEncoder *encoder)
{
	ImxVpuApiFramebufferMetrics *fb_metrics;

	fb_metrics = &(encoder->stream_info.frame_encoding_framebuffer_metrics);

	/* Now do the actual header generation. */
	switch (encoder->open_params.compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
		{
			EncHeaderParam enc_header_param;
			memset(&enc_header_param, 0, sizeof(enc_header_param));

			if (!imx_vpu_api_enc_generate_header_data(encoder, &enc_header_param, ENC_HEADER_DATA_ENTRY_INDEX_H264_SPS_RBSP, SPS_RBSP, ENC_PUT_AVC_HEADER, "h.264 SPS")
			 || !imx_vpu_api_enc_generate_header_data(encoder, &enc_header_param, ENC_HEADER_DATA_ENTRY_INDEX_H264_PPS_RBSP, PPS_RBSP, ENC_PUT_AVC_HEADER, "h.264 PPS"))
				return false;

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
		{
			unsigned int num_macroblocks_per_frame;
			unsigned int num_macroblocks_per_second;
			unsigned int w, h;
			EncHeaderParam enc_header_param;

			memset(&enc_header_param, 0, sizeof(enc_header_param));

			w = fb_metrics->actual_frame_width;
			h = fb_metrics->actual_frame_height;

			/* Calculate the number of macroblocks per second in two steps.
			 * Step 1 calculates the number of macroblocks per frame.
			 * Based on that, step 2 calculates the actual number of
			 * macroblocks per second. The "((encoder->frame_rate_denominator + 1) / 2)"
			 * part is for rounding up. */
			num_macroblocks_per_frame = ((w + 15) / 16) * ((h + 15) / 16);
			num_macroblocks_per_second = (num_macroblocks_per_frame * encoder->open_params.frame_rate_numerator + ((encoder->open_params.frame_rate_denominator + 1) / 2)) / encoder->open_params.frame_rate_denominator;

			/* Decide the user profile level indication based on the VPU
			 * documentation's section 3.2.2.4 and Annex N in ISO/IEC 14496-2 */

			if ((w <= 176) && (h <= 144) && (num_macroblocks_per_second <= 1485))
				enc_header_param.userProfileLevelIndication = 1; /* XXX: this is set to 8 in the VPU wrapper, why? */
			else if ((w <= 352) && (h <= 288) && (num_macroblocks_per_second <= 5940))
				enc_header_param.userProfileLevelIndication = 2;
			else if ((w <= 352) && (h <= 288) && (num_macroblocks_per_second <= 11880))
				enc_header_param.userProfileLevelIndication = 3;
			else if ((w <= 640) && (h <= 480) && (num_macroblocks_per_second <= 36000))
				enc_header_param.userProfileLevelIndication = 4;
			else if ((w <= 720) && (h <= 576) && (num_macroblocks_per_second <= 40500))
				enc_header_param.userProfileLevelIndication = 5;
			else
				enc_header_param.userProfileLevelIndication = 6;

			enc_header_param.userProfileLevelEnable = 1;

			IMX_VPU_API_LOG("frame size: %u x %u pixel, %u macroblocks per second => MPEG-4 user profile level indication = %d", w, h, num_macroblocks_per_second, enc_header_param.userProfileLevelIndication);

			if (!imx_vpu_api_enc_generate_header_data(encoder, &enc_header_param, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOS_HEADER, VOS_HEADER, ENC_PUT_MP4_HEADER, "MPEG-4 VOS header")
			 || !imx_vpu_api_enc_generate_header_data(encoder, &enc_header_param, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VIS_HEADER, VIS_HEADER, ENC_PUT_MP4_HEADER, "MPEG-4 VIS header")
			 || !imx_vpu_api_enc_generate_header_data(encoder, &enc_header_param, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOL_HEADER, VOL_HEADER, ENC_PUT_MP4_HEADER, "MPEG-4 VOL header"))
				return false;

			break;
		}

		/* JPEG headers are generated during encoding. */

		default:
			break;
	}

	return TRUE;
}


static void imx_vpu_api_enc_free_all_header_data(ImxVpuApiEncoder *encoder)
{
	size_t i;

	if (encoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		return;

	for (i = 0; i < ENC_HEADER_DATA_MAX_NUM_ENTRY_INDICES; ++i)
	{
		free(encoder->headers.main_header_data[i].data);
		encoder->headers.main_header_data[i].data = NULL;
		encoder->headers.main_header_data[i].size = 0;
	}

	/* JPEG header data is not dynamically allocated,
	 * so it does not need to be deallocated. */
}


static void imx_vpu_api_enc_free_internal_arrays(ImxVpuApiEncoder *encoder)
{
	if (encoder->internal_framebuffers != NULL)
	{
		free(encoder->internal_framebuffers);
		encoder->internal_framebuffers = NULL;
	}
}


static ImxVpuApiCompressionFormat const enc_supported_compression_formats[] =
{
	IMX_VPU_API_COMPRESSION_FORMAT_MPEG4,
	IMX_VPU_API_COMPRESSION_FORMAT_H263,
	IMX_VPU_API_COMPRESSION_FORMAT_H264,
	IMX_VPU_API_COMPRESSION_FORMAT_JPEG
};

static ImxVpuApiEncGlobalInfo const enc_global_info = {
	.flags = IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_HAS_ENCODER | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_SEMI_PLANAR_FRAMES_SUPPORTED | IMX_VPU_API_ENC_GLOBAL_INFO_FLAG_FULLY_PLANAR_FRAMES_SUPPORTED,
	.hardware_type = IMX_VPU_API_HARDWARE_TYPE_CODA960,
	.min_required_stream_buffer_size = VPU_ENC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE,
	.required_stream_buffer_physaddr_alignment = BITSTREAM_BUFFER_PHYSADDR_ALIGNMENT,
	.required_stream_buffer_size_alignment = BITSTREAM_BUFFER_SIZE_ALIGNMENT,
	.supported_compression_formats = enc_supported_compression_formats,
	.num_supported_compression_formats = sizeof(enc_supported_compression_formats) / sizeof(ImxVpuApiCompressionFormat)
};


ImxVpuApiEncGlobalInfo const * imx_vpu_api_enc_get_global_info(void)
{
	return &enc_global_info;
}


static ImxVpuApiColorFormat const enc_supported_basic_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT
};

static ImxVpuApiColorFormat const enc_supported_jpeg_color_formats[] =
{
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT,
	IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT,
	IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT
};

static ImxVpuApiCompressionFormatSupportDetails const enc_basic_compression_format_support_details = {
	.min_width = 48, .max_width = 1920,
	.min_height = 32, .max_height = 1088,
	.supported_color_formats = enc_supported_basic_color_formats,
	.num_supported_color_formats = sizeof(enc_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat),
	.min_quantization = 1, .max_quantization = 31
};

static ImxVpuApiCompressionFormatSupportDetails const enc_jpeg_support_details = {
	.min_width = 48, .max_width = 1920,
	.min_height = 32, .max_height = 1088,
	.supported_color_formats = enc_supported_jpeg_color_formats,
	.num_supported_color_formats = sizeof(enc_supported_jpeg_color_formats) / sizeof(ImxVpuApiColorFormat),
	.min_quantization = 0, .max_quantization = 99
};

static ImxVpuApiH264SupportDetails const enc_h264_support_details = {
	.parent = {
		.min_width = 8, .max_width = 1920,
		.min_height = 8, .max_height = 1088,
		.supported_color_formats = enc_supported_basic_color_formats,
		.num_supported_color_formats = sizeof(enc_supported_basic_color_formats) / sizeof(ImxVpuApiColorFormat),
		.min_quantization = 0, .max_quantization = 51
	},

	.max_constrained_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4,
	.max_baseline_profile_level = IMX_VPU_API_H264_LEVEL_4,
	.max_main_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,
	.max_high_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,
	.max_high10_profile_level = IMX_VPU_API_H264_LEVEL_UNDEFINED,

	.flags = IMX_VPU_API_H264_FLAG_ACCESS_UNITS_SUPPORTED
};


ImxVpuApiCompressionFormatSupportDetails const * imx_vpu_api_enc_get_compression_format_support_details(ImxVpuApiCompressionFormat compression_format)
{
	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			return (ImxVpuApiCompressionFormatSupportDetails const *)(&enc_h264_support_details);

		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			return &enc_jpeg_support_details;

		default:
			return &enc_basic_compression_format_support_details;
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
	open_params->closed_gop_interval = 0;
	open_params->frame_rate_numerator = 25;
	open_params->frame_rate_denominator = 1;

	switch (compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			open_params->format_specific_open_params.mpeg4_open_params.enable_data_partitioning = 0;
			open_params->format_specific_open_params.mpeg4_open_params.enable_reversible_vlc = 0;
			open_params->format_specific_open_params.mpeg4_open_params.intra_dc_vlc_thr = 0;
			open_params->format_specific_open_params.mpeg4_open_params.enable_hec = 0;
			open_params->format_specific_open_params.mpeg4_open_params.version_id = 2;
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H263:
			open_params->format_specific_open_params.h263_open_params.enable_annex_i = 0;
			open_params->format_specific_open_params.h263_open_params.enable_annex_j = 1;
			open_params->format_specific_open_params.h263_open_params.enable_annex_k = 0;
			open_params->format_specific_open_params.h263_open_params.enable_annex_t = 0;
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			open_params->format_specific_open_params.h264_open_params.profile = IMX_VPU_API_H264_PROFILE_CONSTRAINED_BASELINE;
			open_params->format_specific_open_params.h264_open_params.level = IMX_VPU_API_H264_LEVEL_UNDEFINED;
			open_params->format_specific_open_params.h264_open_params.enable_access_unit_delimiters = 1;
			break;
		default:
			break;
	}
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_open(ImxVpuApiEncoder **encoder, ImxVpuApiEncOpenParams *open_params, ImxDmaBuffer *stream_buffer)
{
	int err;
	ImxVpuApiEncReturnCodes ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
	EncOpenParam enc_open_param;
	RetCode enc_ret;
	ImxVpuApiFramebufferMetrics *fb_metrics;
	BOOL semi_planar;
	size_t internal_fb_aligned_width, internal_fb_aligned_height;
	size_t internal_fb_y_size, internal_fb_uv_size;

	assert(encoder != NULL);
	assert(open_params != NULL);
	assert(stream_buffer != NULL);


	/* Check that the allocated stream buffer is big enough */
	{
		size_t stream_buffer_size = imx_dma_buffer_get_size(stream_buffer);
		if (stream_buffer_size < VPU_ENC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE) 
		{
			IMX_VPU_API_ERROR("stream buffer size is %zu bytes; need at least %zu bytes", stream_buffer_size, (size_t)VPU_ENC_MIN_REQUIRED_BITSTREAM_BUFFER_SIZE);
			return IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_STREAM_BUFFER_SIZE;
		}
	}


	/* Validate the open params. */

	if (open_params->gop_size == 0)
	{
		IMX_VPU_API_ERROR("GOP size must be at least 1");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_PARAMS;
	}


	/* Allocate encoder instance. */
	*encoder = malloc(sizeof(ImxVpuApiEncoder));
	assert((*encoder) != NULL);


	/* Set default encoder values. */
	memset(*encoder, 0, sizeof(ImxVpuApiEncoder));
	(*encoder)->first_frame = TRUE;


	/* Map the stream buffer. We need to keep it mapped always so we can
	 * keep updating it. It is mapped as readwrite so we can shift data
	 * inside it later with memmove() if necessary.
	 * Mapping this with IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC since
	 * the stream buffer stays mapped until the encoder is closed, and
	 * we do copy encoded data into the stream buffer. Also see the
	 * imx_dma_buffer_start_sync_session() / imx_dma_buffer_stop_sync_session()
	 * calls in imx_vpu_api_enc_get_encoded_frame(). */
	(*encoder)->stream_buffer_virtual_address = imx_dma_buffer_map(stream_buffer, IMX_DMA_BUFFER_MAPPING_FLAG_WRITE | IMX_DMA_BUFFER_MAPPING_FLAG_READ | IMX_DMA_BUFFER_MAPPING_FLAG_MANUAL_SYNC, &err);
	if ((*encoder)->stream_buffer_virtual_address == NULL)
	{
			IMX_VPU_API_ERROR("mapping stream buffer to virtual address space failed: %s (%d)", strerror(err), err);
			ret = IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
	}

	(*encoder)->stream_buffer_physical_address = imx_dma_buffer_get_physical_address(stream_buffer);
	(*encoder)->stream_buffer = stream_buffer;


	/* Make a copy of the open_params for later use. */
	(*encoder)->open_params = *open_params;


	fb_metrics = &((*encoder)->stream_info.frame_encoding_framebuffer_metrics);

	fb_metrics->actual_frame_width = open_params->frame_width;
	fb_metrics->actual_frame_height = open_params->frame_height;
	/* The encoder requires an 8-pixel aligned width. Otherwise, corrupted frames are produced. */
	fb_metrics->aligned_frame_width = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, 8);
	/* The VPU can actually handle a vertical 2-row alignment. */
	fb_metrics->aligned_frame_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, 2);
	fb_metrics->y_stride = fb_metrics->aligned_frame_width;
	fb_metrics->y_size = fb_metrics->y_stride * fb_metrics->aligned_frame_height;

	/* Internal VPU encoder framebuffers use different alignments;
	 * both width and height must be aligned to 16. */
	internal_fb_aligned_width = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_width, 16);
	internal_fb_aligned_height = IMX_VPU_API_ALIGN_VAL_TO(fb_metrics->actual_frame_height, 16);
	internal_fb_y_size = internal_fb_aligned_width * internal_fb_aligned_height;

	(*encoder)->internal_fb_y_stride = internal_fb_aligned_width;

	semi_planar = imx_vpu_api_is_color_format_semi_planar(open_params->color_format);

	switch (open_params->color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride / 2;
			fb_metrics->uv_size = fb_metrics->y_size / 4;
			(*encoder)->internal_fb_uv_stride = (*encoder)->internal_fb_y_stride / 2;
			internal_fb_uv_size = internal_fb_y_size / 4;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride / 2;
			fb_metrics->uv_size = fb_metrics->y_size / 2;
			(*encoder)->internal_fb_uv_stride = (*encoder)->internal_fb_y_stride / 2;
			internal_fb_uv_size = internal_fb_y_size / 2;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride;
			fb_metrics->uv_size = fb_metrics->y_size;
			(*encoder)->internal_fb_uv_stride = (*encoder)->internal_fb_y_stride;
			internal_fb_uv_size = internal_fb_y_size;
			break;

		case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT:
			fb_metrics->uv_stride = fb_metrics->y_stride;
			fb_metrics->uv_size = 0;
			(*encoder)->internal_fb_uv_stride = (*encoder)->internal_fb_y_stride;
			internal_fb_uv_size = 0;
			break;

		default:
			/* User specified an unknown format. */
			IMX_VPU_API_ERROR("unknown/unsupported color format %s (%d)", imx_vpu_api_color_format_string(open_params->color_format), open_params->color_format);
			assert(false);
	}

	/* Adjust the uv_stride and uv_size values
	 * in case we are using semi-planar chroma. */
	if (semi_planar)
	{
		fb_metrics->uv_stride *= 2;
		fb_metrics->uv_size *= 2;
		(*encoder)->internal_fb_uv_stride *= 2;
		internal_fb_uv_size *= 2;
	}

	fb_metrics->y_offset = 0;
	fb_metrics->u_offset = fb_metrics->y_size;
	fb_metrics->v_offset = fb_metrics->u_offset + fb_metrics->uv_size;

	(*encoder)->internal_fb_u_offset = internal_fb_y_size;
	(*encoder)->internal_fb_v_offset = (*encoder)->internal_fb_u_offset + internal_fb_uv_size;

	(*encoder)->stream_info.min_framebuffer_size = (semi_planar ? (*encoder)->internal_fb_u_offset : (*encoder)->internal_fb_v_offset) + internal_fb_uv_size;
	(*encoder)->stream_info.framebuffer_alignment = FRAME_PHYSADDR_ALIGNMENT;

	(*encoder)->stream_info.frame_rate_numerator = open_params->frame_rate_numerator;
	(*encoder)->stream_info.frame_rate_denominator = open_params->frame_rate_denominator;


	/* Fill in values into the VPU's encoder open param structure.
	 * Also, fill the stream_info's format_specific_open_params field. */

	memset(&enc_open_param, 0, sizeof(enc_open_param));

	/* Fill in the bitstream buffer address and size.
	 * The actual bitstream buffer is a subset of the bitstream buffer that got
	 * allocated by the user. The remaining space is reserved for the MPEG-4
	 * scratch buffer. This is a trick to reduce DMA memory fragmentation;
	 * both buffers share one DMA memory block, the actual bitstream buffer
	 * comes first, followed by the scratch buffer. */
	enc_open_param.bitstreamBuffer = (*encoder)->stream_buffer_physical_address;
	enc_open_param.bitstreamBufferSize = VPU_ENC_MAIN_BITSTREAM_BUFFER_SIZE;

	/* Miscellaneous codec format independent values. These follow the defaults
	 * recommended in the NXP VPU documentation, section 3.2.2.11. */
	enc_open_param.picWidth = fb_metrics->actual_frame_width;
	enc_open_param.picHeight = fb_metrics->actual_frame_height;
	enc_open_param.frameRateInfo = (open_params->frame_rate_numerator & 0xffffUL) | (((open_params->frame_rate_denominator - 1) & 0xffffUL) << 16);
	enc_open_param.bitRate = open_params->bitrate;
	enc_open_param.initialDelay = 0;
	enc_open_param.vbvBufferSize = 0;
	enc_open_param.gopSize = open_params->gop_size;
	enc_open_param.slicemode.sliceMode = 0;
	enc_open_param.slicemode.sliceSizeMode = 0;
	enc_open_param.slicemode.sliceSize = 4000;
	enc_open_param.intraRefresh = open_params->min_intra_refresh_mb_count;
	enc_open_param.rcIntraQp = -1;
	enc_open_param.userGamma = (int)(0.75*32768);
	enc_open_param.RcIntervalMode = 0;
	enc_open_param.MbInterval = 0;
	enc_open_param.MESearchRange = 0;
	enc_open_param.MEUseZeroPmv = 0;
	enc_open_param.IntraCostWeight = 0;
	enc_open_param.chromaInterleave = !!semi_planar;
	enc_open_param.userQpMinEnable = 0;
	enc_open_param.userQpMaxEnable = 0;
	enc_open_param.userQpMin = 0;
	enc_open_param.userQpMax = 0;

	/* Reports are currently not used */
	enc_open_param.sliceReport = 0;
	enc_open_param.mbReport = 0;
	enc_open_param.mbQpReport = 0;

	/* The i.MX6 does not support dynamic allocation */
	enc_open_param.dynamicAllocEnable = 0;

	/* Ring buffer mode isn't needed, so disable it, instructing
	 * the VPU to use the line buffer mode instead */
	enc_open_param.ringBufferEnable = 0;

	/* Currently, no tiling is supported */
	enc_open_param.linear2TiledEnable = 1;
	enc_open_param.mapType = 0;

	/* Fill in codec format specific values into the VPU's encoder open param structure */
	switch (open_params->compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			enc_open_param.bitstreamFormat = STD_MPEG4;
			enc_open_param.EncStdParam.mp4Param.mp4_dataPartitionEnable = open_params->format_specific_open_params.mpeg4_open_params.enable_data_partitioning;
			enc_open_param.EncStdParam.mp4Param.mp4_reversibleVlcEnable = open_params->format_specific_open_params.mpeg4_open_params.enable_reversible_vlc;
			enc_open_param.EncStdParam.mp4Param.mp4_intraDcVlcThr = open_params->format_specific_open_params.mpeg4_open_params.intra_dc_vlc_thr;
			enc_open_param.EncStdParam.mp4Param.mp4_hecEnable = open_params->format_specific_open_params.mpeg4_open_params.enable_hec;
			enc_open_param.EncStdParam.mp4Param.mp4_verid = open_params->format_specific_open_params.mpeg4_open_params.version_id;
			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H263:
			enc_open_param.bitstreamFormat = STD_H263;
			enc_open_param.EncStdParam.h263Param.h263_annexIEnable = open_params->format_specific_open_params.h263_open_params.enable_annex_i;
			enc_open_param.EncStdParam.h263Param.h263_annexJEnable = open_params->format_specific_open_params.h263_open_params.enable_annex_j;
			enc_open_param.EncStdParam.h263Param.h263_annexKEnable = open_params->format_specific_open_params.h263_open_params.enable_annex_k;
			enc_open_param.EncStdParam.h263Param.h263_annexTEnable = open_params->format_specific_open_params.h263_open_params.enable_annex_t;

			/* The VPU does not permit any other search range for h.263 */
			enc_open_param.MESearchRange = 3;

			break;

		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
		{
			unsigned int width_remainder, height_remainder;

			(*encoder)->stream_info.format_specific_open_params.h264_open_params = open_params->format_specific_open_params.h264_open_params;

			/* Estimate the max level if none is specified. */
			if (open_params->format_specific_open_params.h264_open_params.level == IMX_VPU_API_H264_LEVEL_UNDEFINED)
			{
				ImxVpuApiH264Level level;
				level = imx_vpu_api_estimate_max_h264_level(
					fb_metrics->aligned_frame_width, fb_metrics->aligned_frame_height,
					open_params->bitrate,
					open_params->frame_rate_numerator,
					open_params->frame_rate_denominator,
					IMX_VPU_API_H264_PROFILE_BASELINE /* CODA only supports (constrained) baseline encoding */
				);
				IMX_VPU_API_DEBUG(
					"no h.264 level given; estimated level %s out of width, height, bitrate, framerate",
					imx_vpu_api_h264_level_string(level)
				);
				(*encoder)->stream_info.format_specific_open_params.h264_open_params.level = level;
			}

			enc_open_param.bitstreamFormat = STD_AVC;
			enc_open_param.EncStdParam.avcParam.avc_constrainedIntraPredFlag = 0;
			enc_open_param.EncStdParam.avcParam.avc_disableDeblk = 0;
			enc_open_param.EncStdParam.avcParam.avc_deblkFilterOffsetAlpha = 6;
			enc_open_param.EncStdParam.avcParam.avc_deblkFilterOffsetBeta = 0;
			enc_open_param.EncStdParam.avcParam.avc_chromaQpOffset = 0;

			/* The encoder outputs SPS/PPS infrequently, which is why imxvpuapi
			 * has to insert them manually before each I/IDR frame. However, by
			 * doing so, the SPS/PPS/AUD order is no longer correct. For example:
			 *
			 *   SPS PPS AUD VCL AUD VCL AUD VCL ...
			 *
			 * whereas the proper order (as for example x264 does it) should be:
			 *
			 *   AUD SPS PPS VCL AUD VCL AUD VCL ...
			 *
			 * for this reason, the automatic AUD placement is not used and the
			 * AUDs are inserted manually instead. */
			(*encoder)->h264_aud_enabled = open_params->format_specific_open_params.h264_open_params.enable_access_unit_delimiters;
			enc_open_param.EncStdParam.avcParam.avc_audEnable = 0;

			/* XXX: h.264 MVC support is currently not implemented */
			enc_open_param.EncStdParam.avcParam.mvc_extension = 0;
			enc_open_param.EncStdParam.avcParam.interview_en = 0;
			enc_open_param.EncStdParam.avcParam.paraset_refresh_en = 0;
			enc_open_param.EncStdParam.avcParam.prefix_nal_en = 0;

			/* Check if the frame fits within the 16-pixel boundaries.
			 * If not, crop the remainders. */
			width_remainder = fb_metrics->actual_frame_width & 15;
			height_remainder = fb_metrics->actual_frame_height & 15;
			enc_open_param.EncStdParam.avcParam.avc_frameCroppingFlag = (width_remainder != 0) || (height_remainder != 0);
			enc_open_param.EncStdParam.avcParam.avc_frameCropRight = width_remainder;
			enc_open_param.EncStdParam.avcParam.avc_frameCropBottom = height_remainder;

			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
		{
			enc_open_param.bitstreamFormat = STD_MJPG;

			switch (open_params->color_format)
			{
				case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
				case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
					enc_open_param.EncStdParam.mjpgParam.mjpg_sourceFormat = FORMAT_420;
					break;
				case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
				case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
					enc_open_param.EncStdParam.mjpgParam.mjpg_sourceFormat = FORMAT_422;
					break;
				case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT:
				case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT:
					enc_open_param.EncStdParam.mjpgParam.mjpg_sourceFormat = FORMAT_224;
					break;
				case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
				case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
					enc_open_param.EncStdParam.mjpgParam.mjpg_sourceFormat = FORMAT_444;
					break;
				case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT:
					enc_open_param.EncStdParam.mjpgParam.mjpg_sourceFormat = FORMAT_400;
					break;

				default:
					IMX_VPU_API_ERROR("unknown color format value %d", open_params->color_format);
					ret = IMX_VPU_API_ENC_RETURN_CODE_UNSUPPORTED_COLOR_FORMAT;
					goto cleanup;
			}

			set_jpeg_tables(100 - open_params->quantization, &(enc_open_param.EncStdParam.mjpgParam));

			enc_open_param.EncStdParam.mjpgParam.mjpg_restartInterval = 60;
			enc_open_param.EncStdParam.mjpgParam.mjpg_thumbNailEnable = 0;
			enc_open_param.EncStdParam.mjpgParam.mjpg_thumbNailWidth = 0;
			enc_open_param.EncStdParam.mjpgParam.mjpg_thumbNailHeight = 0;
			break;
		}

		default:
			break;
	}

	/* Closed GOP intervals are emulated by forcing IDR keyframes at specific intervals. */
	(*encoder)->interval_between_idr_frames = ((unsigned long)(open_params->closed_gop_interval)) * open_params->gop_size;
	(*encoder)->frame_counter = 0;


	/* Now actually open the encoder instance */
	IMX_VPU_API_DEBUG(
		"opening encoder; size of actual frame: %u x %u pixel; size of total aligned frame: %u x %u pixel",
		fb_metrics->actual_frame_width, fb_metrics->actual_frame_height,
		fb_metrics->aligned_frame_width, fb_metrics->aligned_frame_height
	);

	if (semi_planar)
	{
		IMX_VPU_API_DEBUG(
			"UV offset of input frames: %zu  UV offset of internal framebuffers: %zu",
			fb_metrics->u_offset, (*encoder)->internal_fb_u_offset
		);
	}
	else
	{
		IMX_VPU_API_DEBUG(
			"U / V offsets of input frames: %zu / %zu  U / V offset of internal framebuffers: %zu / %zu",
			fb_metrics->u_offset, fb_metrics->v_offset,
			(*encoder)->internal_fb_u_offset, (*encoder)->internal_fb_v_offset
		);
	}

	IMX_VPU_API_DEBUG("Y / UV size of input frames: %zu / %zu", fb_metrics->y_size, fb_metrics->uv_size);
	IMX_VPU_API_DEBUG("Y / UV size of internal framebuffers: %zu / %zu", internal_fb_y_size, internal_fb_uv_size);
	IMX_VPU_API_DEBUG("Y / UV stride of input frames: %zu / %zu", fb_metrics->y_stride, fb_metrics->uv_stride);
	IMX_VPU_API_DEBUG("Y / UV stride of internal framebuffers: %zu / %zu", (*encoder)->internal_fb_y_stride, (*encoder)->internal_fb_uv_stride);
	IMX_VPU_API_DEBUG("minimum framebuffer size: %zu byte(s)", (*encoder)->stream_info.min_framebuffer_size);

	imx_coda_vpu_load();
	enc_ret = vpu_EncOpen(&((*encoder)->handle), &enc_open_param);
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not open encoder: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
		goto cleanup;
	}


	/* Get initial information from the VPU to find out how many
	 * framebuffers we need at least. */
	{
		EncInitialInfo initial_info;
		memset(&initial_info, 0, sizeof(initial_info));
		enc_ret = vpu_EncGetInitialInfo((*encoder)->handle, &initial_info);
		if (enc_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("could not get initial information: %s (%d)", retcode_to_string(enc_ret), enc_ret);
			ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
			goto cleanup;
		}

		IMX_VPU_API_DEBUG("initial info min framebuffer count: %d", initial_info.minFrameBufferCount);

		(*encoder)->stream_info.min_num_required_framebuffers = initial_info.minFrameBufferCount;
		/* Reserve extra framebuffers for the subsampled images */
		if (open_params->compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
			(*encoder)->stream_info.min_num_required_framebuffers += VPU_ENC_NUM_EXTRA_SUBSAMPLE_FRAMEBUFFERS;
		(*encoder)->num_framebuffers_to_be_added = (*encoder)->stream_info.min_num_required_framebuffers;
	}


	/* Generate the header data if necessary. Do this after getting the
	 * initial info, otherwise header generation won't work. */
	if (!imx_vpu_api_enc_generate_all_header_data(*encoder))
	{
		vpu_EncClose((*encoder)->handle);
		ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
		goto cleanup;
	}


	/* With JPEG, no framebuffer pool is necessary. Instead, all that needs
	 * to be passed to vpu_EncRegisterFrameBuffer() is the stride value.
	 * Do this here, and also configure some JPEG specific bits. */
	if (open_params->compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		enc_ret = vpu_EncRegisterFrameBuffer(
			(*encoder)->handle,
			NULL,
			0,
			fb_metrics->y_stride,
			0, /* The i.MX6 does not actually need the sourceBufStride value (this is missing in the docs) */
			0,
			0,
			NULL
		);

		/* Set default rotator settings for JPEG. */
		{
			/* the datatypes are int, but this is undocumented; determined by looking
			 * into the imx-vpu library's vpu_lib.c vpu_EncGiveCommand() definition */
			int rotation_angle = 0;
			int mirror = 0;

			vpu_EncGiveCommand((*encoder)->handle, SET_ROTATION_ANGLE, (void *)(&rotation_angle));
			vpu_EncGiveCommand((*encoder)->handle, SET_MIRROR_DIRECTION,(void *)(&mirror));
		}

		/* Clear enable SOF stuff flag. */
#ifdef HAVE_IMXVPUENC_ENABLE_SOF_STUFF
		{
			int append_nullbytes_to_sof_field = 0;
			vpu_EncGiveCommand((*encoder)->handle, ENC_ENABLE_SOF_STUFF, (void*)(&append_nullbytes_to_sof_field));
		}
#endif
	}


	/* Finish & cleanup (the latter in case of an error). */
finish:
	if (ret == IMX_VPU_API_ENC_RETURN_CODE_OK)
		IMX_VPU_API_DEBUG("successfully opened encoder");

	return ret;

cleanup:
	if ((*encoder) != NULL)
	{
		imx_vpu_api_enc_free_all_header_data(*encoder);

		if ((*encoder)->stream_buffer_virtual_address != NULL)
			imx_dma_buffer_unmap((*encoder)->stream_buffer);
		free(*encoder);
		*encoder = NULL;
	}

	goto finish;
}


void imx_vpu_api_enc_close(ImxVpuApiEncoder *encoder)
{
	RetCode enc_ret;

	assert(encoder != NULL);

	IMX_VPU_API_DEBUG("closing encoder");


	/* Close the encoder handle */

	enc_ret = vpu_EncClose(encoder->handle);
	if (enc_ret == RETCODE_FRAME_NOT_COMPLETE)
	{
		/* VPU refused to close, since a frame is partially encoded.
		 * Force it to close by first resetting the handle and retry. */
		vpu_SWReset(encoder->handle, 0);
		enc_ret = vpu_EncClose(encoder->handle);
	}
	if (enc_ret != RETCODE_SUCCESS)
		IMX_VPU_API_ERROR("vpu_EncClose() error: %s (%d)", retcode_to_string(enc_ret), enc_ret);


	/* Remaining cleanup */

	imx_vpu_api_enc_free_all_header_data(encoder);

	if (encoder->stream_buffer != NULL)
		imx_dma_buffer_unmap(encoder->stream_buffer);

	imx_vpu_api_enc_free_internal_arrays(encoder);

	free(encoder);

	imx_coda_vpu_unload();
}

ImxVpuApiEncStreamInfo const * imx_vpu_api_enc_get_stream_info(ImxVpuApiEncoder *encoder)
{
	assert(encoder != NULL);
	return &(encoder->stream_info);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_add_framebuffers_to_pool(ImxVpuApiEncoder *encoder, ImxDmaBuffer **fb_dma_buffers, size_t num_framebuffers)
{
	unsigned int i;
	ImxVpuApiEncReturnCodes ret;
	RetCode enc_ret;
	ExtBufCfg scratch_cfg;
	ImxVpuApiFramebufferMetrics *fb_metrics;

	assert(encoder != NULL);
	assert(fb_dma_buffers != NULL);


	if (encoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		IMX_VPU_API_DEBUG("JPEG encoding does not use a framebuffer pool");
		return IMX_VPU_API_ENC_RETURN_CODE_OK;
	}

	assert(num_framebuffers >= 1);


	fb_metrics = &(encoder->stream_info.frame_encoding_framebuffer_metrics);


	/* NOTE: With the CODA VPU encoder, this is called only once after
	 * opening the encoder instance. As a result, the code in here
	 * is written with the assumption in mind that it is never called
	 * more than once after imx_vpu_api_enc_open(). */


	if (encoder->num_framebuffers_to_be_added == 0)
	{
		IMX_VPU_API_ERROR("no framebuffers need to be added");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	if (num_framebuffers < encoder->num_framebuffers_to_be_added)
	{
		IMX_VPU_API_ERROR("encoder needs %zu framebuffers to be added, got %zu", encoder->num_framebuffers_to_be_added, num_framebuffers);
		return IMX_VPU_API_ENC_RETURN_CODE_INSUFFICIENT_FRAMEBUFFERS;
	}


	assert(num_framebuffers >= VPU_ENC_NUM_EXTRA_SUBSAMPLE_FRAMEBUFFERS);
	num_framebuffers -= VPU_ENC_NUM_EXTRA_SUBSAMPLE_FRAMEBUFFERS;


	/* Allocate memory for framebuffer structures */

	encoder->internal_framebuffers = malloc(sizeof(FrameBuffer) * num_framebuffers);
	assert(encoder->internal_framebuffers != NULL);

	encoder->num_framebuffers = num_framebuffers;


	/* Copy the values from the framebuffers array to the internal_framebuffers
	 * ones, which in turn will be used by the VPU */
	memset(encoder->internal_framebuffers, 0, sizeof(FrameBuffer) * num_framebuffers);
	for (i = 0; i < num_framebuffers; ++i)
	{
		imx_physical_address_t phys_addr;
		ImxDmaBuffer *fb_dma_buffer = fb_dma_buffers[i];
		FrameBuffer *internal_fb = &(encoder->internal_framebuffers[i]);

		phys_addr = imx_dma_buffer_get_physical_address(fb_dma_buffer);
		if (phys_addr == 0)
		{
			IMX_VPU_API_ERROR("could not map buffer %u/%u", i, num_framebuffers);
			ret = IMX_VPU_API_ENC_RETURN_CODE_DMA_MEMORY_ACCESS_ERROR;
			goto cleanup;
		}

		internal_fb->strideY = encoder->internal_fb_y_stride;
		internal_fb->strideC = encoder->internal_fb_uv_stride;
		internal_fb->myIndex = i;
		internal_fb->bufY = (PhysicalAddress)(phys_addr);
		internal_fb->bufCb = (PhysicalAddress)(phys_addr + encoder->internal_fb_u_offset);
		internal_fb->bufCr = (PhysicalAddress)(phys_addr + encoder->internal_fb_v_offset);
		/* The encoder does not use MvCol data. */
		internal_fb->bufMvCol = 0;
	}

	/* Set up the scratch buffer information. The MPEG-4 scratch buffer
	 * is located in the same DMA buffer as the bitstream buffer
	 * (the bitstream buffer comes first, and is the largest part of
	 * the DMA buffer, followed by the scratch buffer). */
	scratch_cfg.bufferBase = encoder->stream_buffer_physical_address + VPU_ENC_MAIN_BITSTREAM_BUFFER_SIZE;
	scratch_cfg.bufferSize = VPU_ENC_MPEG4_SCRATCH_SIZE;


	/* Now register the framebuffers */

	{
		/* NOTE: The vpu_EncRegisterFrameBuffer() API changed several times
		 * in the past. To maintain compatibility with (very) old BSPs,
		 * preprocessor macros are used to adapt the code.
		 * Before vpulib version 5.3.3, vpu_EncRegisterFrameBuffer() didn't
		 * accept any extra scratch buffer information. Between 5.3.3 and
		 * 5.3.7, it accepted an ExtBufCfg argument. Starting with 5.3.7,
		 * it expects an EncExtBufInfo argument.
		 */


		/* Set up additional subsample buffers */

		ImxDmaBuffer *subsample_fb_dma_buffer_A, *subsample_fb_dma_buffer_B;
#if (VPU_LIB_VERSION_CODE >= VPU_LIB_VERSION(5, 3, 7))
		EncExtBufInfo buf_info;
		memset(&buf_info, 0, sizeof(buf_info));
		buf_info.scratchBuf = scratch_cfg;
#endif

		/* TODO: is it really necessary to use two full buffers for the
		 * subsampling buffers? They could both be placed in one
		 * buffer, thus saving memory */
		subsample_fb_dma_buffer_A = fb_dma_buffers[num_framebuffers + 0];
		subsample_fb_dma_buffer_B = fb_dma_buffers[num_framebuffers + 1];


		/* Perform the actual registration */

		enc_ret = vpu_EncRegisterFrameBuffer(
			encoder->handle,
			encoder->internal_framebuffers,
			num_framebuffers,
			fb_metrics->y_stride,
			0, /* The i.MX6 does not actually need the sourceBufStride value (this is missing in the docs) */
			imx_dma_buffer_get_physical_address(subsample_fb_dma_buffer_A),
			imx_dma_buffer_get_physical_address(subsample_fb_dma_buffer_B)
#if (VPU_LIB_VERSION_CODE >= VPU_LIB_VERSION(5, 3, 7))
			, &buf_info
#elif (VPU_LIB_VERSION_CODE >= VPU_LIB_VERSION(5, 3, 3))
			, &scratch_cfg
#endif
		);
		if (enc_ret != RETCODE_SUCCESS)
		{
			IMX_VPU_API_ERROR("could not register framebuffers: %s (%d)", retcode_to_string(enc_ret), enc_ret);
			ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
			goto cleanup;
		}
	}


	encoder->num_framebuffers_to_be_added = 0;

	return IMX_VPU_API_ENC_RETURN_CODE_OK;


cleanup:
	imx_vpu_api_enc_free_internal_arrays(encoder);

	return ret;
}


void imx_vpu_api_enc_enable_drain_mode(ImxVpuApiEncoder *encoder)
{
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
	assert(encoder != NULL);

	encoder->first_frame = TRUE;
	encoder->staged_raw_frame_set = FALSE;
	encoder->encoded_frame_available = FALSE;
	encoder->frame_counter = 0;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_bitrate(ImxVpuApiEncoder *encoder, unsigned int bitrate)
{
	RetCode enc_ret;
	int param;

	assert(encoder != NULL);

	if (encoder->open_params.bitrate == 0)
	{
		IMX_VPU_API_ERROR("rate control disabled in the imx_vpu_api_enc_open() parameters");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	IMX_VPU_API_TRACE("setting bitrate to %u kbps", bitrate);

	param = bitrate;
	enc_ret = vpu_EncGiveCommand(encoder->handle, ENC_SET_BITRATE, &param);
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not set bitrate: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		return IMX_VPU_API_ENC_RETURN_CODE_ERROR;
	}
	else
		return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_set_frame_rate(ImxVpuApiEncoder *encoder, unsigned int frame_rate_numerator, unsigned int frame_rate_denominator)
{
	RetCode enc_ret;
	int param;

	assert(encoder != NULL);
	assert(frame_rate_denominator > 0);

	IMX_VPU_API_TRACE("setting frame rate to %u/%u fps", frame_rate_numerator, frame_rate_denominator);

	param = (frame_rate_numerator & 0xffffUL) | (((frame_rate_denominator - 1) & 0xffffUL) << 16);
	enc_ret = vpu_EncGiveCommand(encoder->handle, ENC_SET_FRAME_RATE, &param);
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not set frame rate: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		return IMX_VPU_API_ENC_RETURN_CODE_ERROR;
	}
	else
		return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_push_raw_frame(ImxVpuApiEncoder *encoder, ImxVpuApiRawFrame const *raw_frame)
{
	assert(encoder != NULL);
	assert(raw_frame != NULL);

	if (encoder->staged_raw_frame_set)
	{
		IMX_VPU_API_ERROR("tried to push a raw frame before a previous one was encoded");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	IMX_VPU_API_LOG("staged raw frame");

	/* Stage the raw frame. We cannot use it here right away, since the CODA
	 * encoder has no separate function to push raw frames into it. Instead,
	 * just keep track of it here, and actually use it in imx_vpu_api_enc_encode(). */
	encoder->staged_raw_frame = *raw_frame;
	encoder->staged_raw_frame_set = TRUE;

	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_encode(ImxVpuApiEncoder *encoder, size_t *encoded_frame_size, ImxVpuApiEncOutputCodes *output_code)
{
	ImxVpuApiEncReturnCodes ret;
	RetCode enc_ret;
	EncParam enc_param;
	FrameBuffer source_framebuffer;
	imx_physical_address_t raw_frame_phys_addr;
	BOOL timeout;
	BOOL add_header;
	size_t encoded_data_size;
	ImxVpuApiFramebufferMetrics *fb_metrics;
	BOOL forced_idr_for_closed_gop = FALSE;

	assert(encoder != NULL);
	assert(encoded_frame_size != NULL);
	assert(output_code != NULL);

	if (encoder->encoded_frame_available)
	{
		IMX_VPU_API_ERROR("cannot encode new frame before the old one was retrieved");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	/* Check that we have a working framebuffer pool (except when encoding
	 * to JPEG, since the encoder does not use a framebuffer pool then). */
	if ((encoder->internal_framebuffers == NULL) && (encoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG))
	{
		IMX_VPU_API_ERROR("cannot encode anything without an initialized framebuffer pool; check that framebuffers were added");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	if (!(encoder->staged_raw_frame_set))
	{
		IMX_VPU_API_TRACE("no data left to encode");
		*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED;
		return IMX_VPU_API_ENC_RETURN_CODE_OK;
	}

	ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
	fb_metrics = &(encoder->stream_info.frame_encoding_framebuffer_metrics);
	*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE;

	if (encoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_H264)
	{
		forced_idr_for_closed_gop = (encoder->interval_between_idr_frames > 0) &&
		                            ((encoder->frame_counter % encoder->interval_between_idr_frames) == 0);
		if (forced_idr_for_closed_gop)
			IMX_VPU_API_LOG("forcing this frame to be encoded as an IDR frame to produce closed GOP");
	}

	/* Get the physical address for the raw_frame that shall be encoded. */
	raw_frame_phys_addr = imx_dma_buffer_get_physical_address(encoder->staged_raw_frame.fb_dma_buffer);
	IMX_VPU_API_LOG("encoding raw_frame with physical address %" IMX_PHYSICAL_ADDRESS_FORMAT, raw_frame_phys_addr);


	/* JPEG frames always need JPEG headers, since each frame is an independent
	 * JPEG frame. Use the VPU ENC_GET_JPEG_HEADER command to extract the header
	 * into encoder->headers.jpeg_header_data. Later, when the user calls
	 * imx_vpu_api_enc_get_encoded_frame(), that header data is prepended to the
	 * JPEG frame. */
	if (encoder->open_params.compression_format == IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
	{
		EncParamSet jpeg_param;
		memset(&jpeg_param, 0, sizeof(jpeg_param));

		jpeg_param.size = JPEG_ENC_HEADER_DATA_MAX_SIZE;
		jpeg_param.pParaSet = encoder->headers.jpeg_header_data;

		vpu_EncGiveCommand(encoder->handle, ENC_GET_JPEG_HEADER, &jpeg_param);
		IMX_VPU_API_LOG("added JPEG header with %d byte", jpeg_param.size);

		encoder->jpeg_header_size = jpeg_param.size;
	}


	/* Set up source_framebuffer to use the information from raw_frame.
	 * That way, vpu_EncStartOneFrame() encodes that raw frame. */

	memset(&source_framebuffer, 0, sizeof(source_framebuffer));

	source_framebuffer.strideY = fb_metrics->y_stride;
	source_framebuffer.strideC = fb_metrics->uv_stride;

	/* Make sure the source framebuffer has an ID that is different
	 * to the IDs of the other, registered framebuffers. */
	source_framebuffer.myIndex = encoder->num_framebuffers + 1;

	source_framebuffer.bufY = (PhysicalAddress)(raw_frame_phys_addr + fb_metrics->y_offset);
	source_framebuffer.bufCb = (PhysicalAddress)(raw_frame_phys_addr + fb_metrics->u_offset);
	source_framebuffer.bufCr = (PhysicalAddress)(raw_frame_phys_addr + fb_metrics->v_offset);
	/* The encoder does not use MvCol data. */
	source_framebuffer.bufMvCol = (PhysicalAddress)0;


	/* Initialize encoding parameters. */
	memset(&enc_param, 0, sizeof(enc_param));
	enc_param.sourceFrame = &source_framebuffer;
	enc_param.forceIPicture = !!(encoder->staged_raw_frame.frame_types[0] & IMX_VPU_API_FRAME_TYPE_I) ||
		                      !!(encoder->staged_raw_frame.frame_types[0] & IMX_VPU_API_FRAME_TYPE_IDR) ||
		                      forced_idr_for_closed_gop;
	enc_param.skipPicture = 0;
	/* The quantization parameter is already used in the
	 * set_jpeg_tables() call in imx_vpu_api_enc_open().
	 * For JPEG, the VPU ignores the quantParam field. */
	if (encoder->open_params.compression_format != IMX_VPU_API_COMPRESSION_FORMAT_JPEG)
		enc_param.quantParam = encoder->open_params.quantization;
	enc_param.enableAutoSkip = 0;


	/* Do the actual encoding. */
	enc_ret = vpu_EncStartOneFrame(encoder->handle, &enc_param);
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not start encoding frame: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
		goto finish;
	}


	/* Wait for frame completion. */
	{
		int cnt;

		IMX_VPU_API_LOG("waiting for encoding completion");

		/* Wait a few times, since sometimes, it takes more than
		 * one vpu_WaitForInt() call to cover the encoding interval. */
		timeout = TRUE;
		for (cnt = 0; cnt < VPU_MAX_TIMEOUT_COUNTS; ++cnt)
		{
			if (vpu_WaitForInt(VPU_WAIT_TIMEOUT) != RETCODE_SUCCESS)
			{
				IMX_VPU_API_INFO("timeout after waiting %d ms for frame completion", VPU_WAIT_TIMEOUT);
			}
			else
			{
				timeout = FALSE;
				break;
			}
		}
	}


	/* Retrieve information about the result of the encode process. Do so even if
	 * a timeout occurred. This is intentional, since according to the VPU docs,
	 * vpu_EncStartOneFrame() won't be usable again until vpu_EncGetOutputInfo()
	 * is called. In other words, the vpu_EncStartOneFrame() locks down some
	 * internals inside the VPU, and vpu_EncGetOutputInfo() releases them. */

	memset(&(encoder->enc_output_info), 0, sizeof(encoder->enc_output_info));
	enc_ret = vpu_EncGetOutputInfo(encoder->handle, &(encoder->enc_output_info));
	if (enc_ret != RETCODE_SUCCESS)
	{
		IMX_VPU_API_ERROR("could not get output information: %s (%d)", retcode_to_string(enc_ret), enc_ret);
		ret = IMX_VPU_API_ENC_RETURN_CODE_ERROR;
		goto finish;
	}


	/* If a timeout occurred earlier, this is the correct time to abort
	 * encoding and return an error code, since vpu_EncGetOutputInfo()
	 * has been called, unlocking the VPU encoder calls. */
	if (timeout)
	{
		ret = IMX_VPU_API_ENC_RETURN_CODE_TIMEOUT;
		goto finish;
	}


	/* Extract the frame type. (There is only one frame type; we use
	 * a 2-item array, because convert_frame_type() is also used by
	 * the decoder, which may get interlaced data with different
	 * field frame types.) */
	{
		ImxVpuApiFrameType frame_types[2];
		convert_frame_type(encoder->open_params.compression_format, encoder->enc_output_info.picType, FALSE, frame_types);
		encoder->encoded_frame_type = frame_types[0];
	}


	IMX_VPU_API_LOG(
		"output info:  bitstreamBuffer %" IMX_PHYSICAL_ADDRESS_FORMAT "  bitstreamSize %u  bitstreamWrapAround %d  skipEncoded %d  picType %d (%s)  numOfSlices %d",
		encoder->enc_output_info.bitstreamBuffer,
		encoder->enc_output_info.bitstreamSize,
		encoder->enc_output_info.bitstreamWrapAround,
		encoder->enc_output_info.skipEncoded,
		encoder->enc_output_info.picType, imx_vpu_api_frame_type_string(encoder->encoded_frame_type),
		encoder->enc_output_info.numOfSlices
	);


	/* Check if we need to prepend a header. */
	switch (encoder->open_params.compression_format)
	{
		case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
		{
			add_header = TRUE;
			break;
		}

		case IMX_VPU_API_COMPRESSION_FORMAT_H264:
		case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			add_header = encoder->first_frame
			          || (encoder->encoded_frame_type == IMX_VPU_API_FRAME_TYPE_IDR)
			          || (encoder->encoded_frame_type == IMX_VPU_API_FRAME_TYPE_I);
			break;

		default:
			add_header = FALSE;
	}


	/* Calculate the size of the encoded data. */

	encoded_data_size = encoder->enc_output_info.bitstreamSize;
	if (encoder->h264_aud_enabled)
		encoded_data_size += h264_aud_size;

	if (add_header)
	{
		switch (encoder->open_params.compression_format)
		{
			case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
				/* Add the APP0 segment size. The VPU does not write that segment
				 * on its own, so we have to manually insert it later on, and we
				 * need room in the header data array for that. */
				encoded_data_size += encoder->jpeg_header_size + JPEG_JFIF_APP0_SEGMENT_SIZE;
				break;

			case IMX_VPU_API_COMPRESSION_FORMAT_H264:
				encoded_data_size += encoder->headers.main_header_data[ENC_HEADER_DATA_ENTRY_INDEX_H264_SPS_RBSP].size
				                   + encoder->headers.main_header_data[ENC_HEADER_DATA_ENTRY_INDEX_H264_PPS_RBSP].size;
				break;

			case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
				encoded_data_size += encoder->headers.main_header_data[ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOS_HEADER].size
				                   + encoder->headers.main_header_data[ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VIS_HEADER].size
				                   + encoder->headers.main_header_data[ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOL_HEADER].size;
				break;

			default:
				break;
		}
	}


	/* Copy over metadata from the raw frame to the encoded frame. Since the
	 * encoder does not perform any kind of delay or reordering, this is
	 * appropriate, because in that case, one input frame always immediately
	 * leads to one output frame. */
	encoder->encoded_frame_context = encoder->staged_raw_frame.context;
	encoder->encoded_frame_pts = encoder->staged_raw_frame.pts;
	encoder->encoded_frame_dts = encoder->staged_raw_frame.dts;
	encoder->encoded_frame_data_size = *encoded_frame_size = encoded_data_size;
	encoder->encoded_frame_available = TRUE;

	encoder->prepend_header_to_frame = add_header;

	/* We just encoded a frame, so the next frame will not be the first one. */
	encoder->first_frame = FALSE;

	*output_code = IMX_VPU_API_ENC_OUTPUT_CODE_ENCODED_FRAME_AVAILABLE;


finish:
	if (ret == IMX_VPU_API_ENC_RETURN_CODE_OK)
		encoder->frame_counter++;

	encoder->staged_raw_frame_set = FALSE;

	return ret;
}


static BOOL check_available_space(uint8_t *write_pointer, uint8_t *write_pointer_end, size_t expected_min_available_space, char const *description)
{
	ptrdiff_t available_space = write_pointer_end - write_pointer;
	if (available_space < (ptrdiff_t)(expected_min_available_space))
	{
		IMX_VPU_API_ERROR("insufficient space in output buffer for %s: need %zu byte, got %td", description, expected_min_available_space, available_space);
		return FALSE;
	}
	else
		return TRUE;
}


static BOOL write_header_data(ImxVpuApiEncoder *encoder, unsigned int header_data_entry, uint8_t **write_pointer, uint8_t *write_pointer_end, char const *description)
{
	EncHeaderData const *enc_header_data = &(encoder->headers.main_header_data[header_data_entry]);

	if (!check_available_space(*write_pointer, write_pointer_end, enc_header_data->size, description))
		return IMX_VPU_API_ENC_RETURN_CODE_ERROR;

	memcpy(*write_pointer, enc_header_data->data, enc_header_data->size);
	(*write_pointer) += enc_header_data->size;
	IMX_VPU_API_LOG("added %s with %zu byte", description, enc_header_data->size);

	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame)
{
	return imx_vpu_api_enc_get_encoded_frame_ext(encoder, encoded_frame, NULL);
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_encoded_frame_ext(ImxVpuApiEncoder *encoder, ImxVpuApiEncodedFrame *encoded_frame, int *is_sync_point)
{
	ImxVpuApiEncReturnCodes ret = IMX_VPU_API_ENC_RETURN_CODE_OK;
	uint8_t *write_pointer, *write_pointer_end;

	assert(encoder != NULL);
	assert(encoded_frame != NULL);
	assert(encoded_frame->data != NULL);

	if (!(encoder->encoded_frame_available))
	{
		IMX_VPU_API_ERROR("cannot retrieve encoded frame since there is none");
		return IMX_VPU_API_ENC_RETURN_CODE_INVALID_CALL;
	}

	write_pointer = encoded_frame->data;
	write_pointer_end = write_pointer + encoder->encoded_frame_data_size;

	/* h.264 AUD should come before SPS/PPS header data. See the
	 * code in imx_vpu_api_enc_open() for details. */
	if (encoder->h264_aud_enabled)
	{
		if (!check_available_space(write_pointer, write_pointer_end, h264_aud_size, "h.264 AUD"))
			return IMX_VPU_API_ENC_RETURN_CODE_ERROR;

		memcpy(write_pointer, h264_aud, h264_aud_size);
		write_pointer += h264_aud_size;
	}

	/* Write the header before the actual frame if necessary. */
	if (encoder->prepend_header_to_frame)
	{
		switch (encoder->open_params.compression_format)
		{
			case IMX_VPU_API_COMPRESSION_FORMAT_H264:
			{
				if ((ret = write_header_data(encoder, ENC_HEADER_DATA_ENTRY_INDEX_H264_SPS_RBSP, &write_pointer, write_pointer_end, "h.264 SPS RBSP")) != IMX_VPU_API_ENC_RETURN_CODE_OK)
					return ret;
				if ((ret = write_header_data(encoder, ENC_HEADER_DATA_ENTRY_INDEX_H264_PPS_RBSP, &write_pointer, write_pointer_end, "h.264 PPS RBSP")) != IMX_VPU_API_ENC_RETURN_CODE_OK)
					return ret;
				break;
			}

			case IMX_VPU_API_COMPRESSION_FORMAT_MPEG4:
			{
				if ((ret = write_header_data(encoder, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOS_HEADER, &write_pointer, write_pointer_end, "MPEG-4 VOS header")) != IMX_VPU_API_ENC_RETURN_CODE_OK)
					return ret;
				if ((ret = write_header_data(encoder, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VIS_HEADER, &write_pointer, write_pointer_end, "MPEG-4 VIS header")) != IMX_VPU_API_ENC_RETURN_CODE_OK)
					return ret;
				if ((ret = write_header_data(encoder, ENC_HEADER_DATA_ENTRY_INDEX_MPEG4_VOL_HEADER, &write_pointer, write_pointer_end, "MPEG-4 VOL header")) != IMX_VPU_API_ENC_RETURN_CODE_OK)
					return ret;
				break;
			}

			case IMX_VPU_API_COMPRESSION_FORMAT_JPEG:
			{
				if (!check_available_space(
					write_pointer,
					write_pointer_end,
					encoder->jpeg_header_size + JPEG_JFIF_APP0_SEGMENT_SIZE,
					"JPEG header"
				))
					return IMX_VPU_API_ENC_RETURN_CODE_ERROR;

				/* The VPU generates headers that do not contain a JFIF APP0 segment.
				 * Some programs require either a JFIF segment or an EXIF segment,
				 * and do not work correctly if neither are present. For this reason,
				 * we must insert an APP0 segment here. The VPU-produced JPEG header
				 * data begins with the JPEG start-of-image (SOI) marker. We copy
				 * that, and right after the SOI, insert the APP0 segment, which is
				 * how valid JFIF files are structured. */

				/* Copy the start-of-image (SOI) marker, which consists of the
				 * first 2 bytes in the VPU JPEG header data. */
				*write_pointer++ = encoder->headers.jpeg_header_data[0];
				*write_pointer++ = encoder->headers.jpeg_header_data[1];

				/* Copy the JFIF APP0 segment right after the SOI. */
				memcpy(write_pointer, jpeg_jfif_app0_segment, JPEG_JFIF_APP0_SEGMENT_SIZE);
				write_pointer += JPEG_JFIF_APP0_SEGMENT_SIZE;

				/* Now copy the rest of the VPU-produced header data. We
				 * skip the first 2 bytes since these were copied already. */
				memcpy(write_pointer, encoder->headers.jpeg_header_data + 2, encoder->jpeg_header_size - 2);
				write_pointer += encoder->jpeg_header_size - 2;

				break;
			}

			default:
				break;
		}
	}

	/* Get the encoded data out of the bitstream buffer into the output buffer. */
	if (encoder->enc_output_info.bitstreamBuffer != 0)
	{
		uint8_t const *output_data_ptr;

		if (!check_available_space(write_pointer, write_pointer_end, encoder->enc_output_info.bitstreamSize, "encoded frame data"))
			return IMX_VPU_API_ENC_RETURN_CODE_ERROR;

		/* Begin synced access since we have to copy the encoded
		 * data out of the stream buffer. */
		imx_dma_buffer_start_sync_session(encoder->stream_buffer);

		output_data_ptr = IMX_VPU_API_ENC_GET_STREAM_VIRT_ADDR(encoder, encoder->enc_output_info.bitstreamBuffer);
		memcpy(write_pointer, output_data_ptr, encoder->enc_output_info.bitstreamSize);
		write_pointer += encoder->enc_output_info.bitstreamSize;

		imx_dma_buffer_stop_sync_session(encoder->stream_buffer);
	}

	encoded_frame->data_size = encoder->encoded_frame_data_size;
	encoded_frame->has_header = encoder->prepend_header_to_frame;
	encoded_frame->frame_type = encoder->encoded_frame_type;
	encoded_frame->context = encoder->encoded_frame_context;
	encoded_frame->pts = encoder->encoded_frame_pts;
	encoded_frame->dts = encoder->encoded_frame_dts;

	if (is_sync_point)
	{
		/* In h.264, only IDR frames (not I frames) are valid sync points. */

		switch (encoder->encoded_frame_type)
		{
			case IMX_VPU_API_COMPRESSION_FORMAT_H264:
				*is_sync_point = (encoder->encoded_frame_type == IMX_VPU_API_FRAME_TYPE_IDR);
				break;
			default:
				*is_sync_point = (encoder->encoded_frame_type == IMX_VPU_API_FRAME_TYPE_I);
				break;
		}
	}

	encoder->encoded_frame_available = FALSE;

	return IMX_VPU_API_ENC_RETURN_CODE_OK;
}


ImxVpuApiEncReturnCodes imx_vpu_api_enc_get_skipped_frame_info(ImxVpuApiEncoder *encoder, void **context, uint64_t *pts, uint64_t *dts)
{
	IMX_VPU_API_UNUSED_PARAM(encoder);
	IMX_VPU_API_UNUSED_PARAM(context);
	IMX_VPU_API_UNUSED_PARAM(pts);
	IMX_VPU_API_UNUSED_PARAM(dts);

	/* TODO: Frameskipping with CODA960 is not supported at this point */
}
