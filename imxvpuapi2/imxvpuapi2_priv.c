#include <string.h>
#include <stdint.h>
#include "imxvpuapi2_priv.h"


/* h.264 access unit delimiter data */
uint8_t const h264_aud[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };
size_t const h264_aud_size = sizeof(h264_aud);


/* These quantization tables are from the JPEG specification, section K.1 */

uint8_t const jpeg_quantization_table_luma[64] =
{
	16,  11,  10,  16,  24,  40,  51,  61,
	12,  12,  14,  19,  26,  58,  60,  55,
	14,  13,  16,  24,  40,  57,  69,  56,
	14,  17,  22,  29,  51,  87,  80,  62,
	18,  22,  37,  56,  68, 109, 103,  77,
	24,  35,  55,  64,  81, 104, 113,  92,
	49,  64,  78,  87, 103, 121, 120, 101,
	72,  92,  95,  98, 112, 100, 103,  99
};

uint8_t const jpeg_quantization_table_chroma[64] =
{
	17,  18,  24,  47,  99,  99,  99,  99,
	18,  21,  26,  66,  99,  99,  99,  99,
	24,  26,  56,  99,  99,  99,  99,  99,
	47,  66,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99,
	99,  99,  99,  99,  99,  99,  99,  99
};

/* Natural order -> zig zag order
 * The quantization tables above are in natural order
 * but should be applied in zig zag order.
 */
uint8_t const jpeg_zigzag_pattern[64] =
{
        0,   1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63
};


/* JPEG marker definitions, needed for JPEG header parsing */

/* Start Of Frame markers, non-differential, Huffman coding */
#define JPEG_MARKER_SOF0      0xc0  /* Baseline DCT */
#define JPEG_MARKER_SOF1      0xc1  /* Extended sequential DCT */
#define JPEG_MARKER_SOF2      0xc2  /* Progressive DCT */
#define JPEG_MARKER_SOF3      0xc3  /* Lossless */

/* Start Of Frame markers, differential, Huffman coding */
#define JPEG_MARKER_SOF5      0xc5
#define JPEG_MARKER_SOF6      0xc6
#define JPEG_MARKER_SOF7      0xc7

/* Start Of Frame markers, non-differential, arithmetic coding */
#define JPEG_MARKER_JPG       0xc8  /* Reserved */
#define JPEG_MARKER_SOF9      0xc9
#define JPEG_MARKER_SOF10     0xca
#define JPEG_MARKER_SOF11     0xcb

/* Start Of Frame markers, differential, arithmetic coding */
#define JPEG_MARKER_SOF13     0xcd
#define JPEG_MARKER_SOF14     0xce
#define JPEG_MARKER_SOF15     0xcf

/* Restart interval termination */
#define JPEG_MARKER_RST0      0xd0  /* Restart ... */
#define JPEG_MARKER_RST1      0xd1
#define JPEG_MARKER_RST2      0xd2
#define JPEG_MARKER_RST3      0xd3
#define JPEG_MARKER_RST4      0xd4
#define JPEG_MARKER_RST5      0xd5
#define JPEG_MARKER_RST6      0xd6
#define JPEG_MARKER_RST7      0xd7

#define JPEG_MARKER_SOI       0xd8  /* Start of image */
#define JPEG_MARKER_EOI       0xd9  /* End Of Image */
#define JPEG_MARKER_SOS       0xda  /* Start Of Scan */

#define JPEG_MARKER_DHT       0xc4  /* Huffman Table(s) */
#define JPEG_MARKER_DAC       0xcc  /* Algorithmic Coding Table */
#define JPEG_MARKER_DQT       0xdb  /* Quantisation Table(s) */
#define JPEG_MARKER_DNL       0xdc  /* Number of lines */
#define JPEG_MARKER_DRI       0xdd  /* Restart Interval */
#define JPEG_MARKER_DHP       0xde  /* Hierarchical progression */
#define JPEG_MARKER_EXP       0xdf

#define JPEG_MARKER_APP0      0xe0  /* Application marker */
#define JPEG_MARKER_APP1      0xe1
#define JPEG_MARKER_APP2      0xe2
#define JPEG_MARKER_APP13     0xed
#define JPEG_MARKER_APP14     0xee
#define JPEG_MARKER_APP15     0xef

#define JPEG_MARKER_JPG0      0xf0  /* Reserved ... */
#define JPEG_MARKER_JPG13     0xfd
#define JPEG_MARKER_COM       0xfe  /* Comment */

#define JPEG_MARKER_TEM       0x01


void imx_vpu_api_insert_vp8_ivf_sequence_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height)
{
	int i = 0;
	/* At this point in time, these values are unknown, so just use defaults */
	uint32_t const fps_numerator = 1, fps_denominator = 1, num_frames = 0;

	/* DKIF signature */
	header[i++] = 'D';
	header[i++] = 'K';
	header[i++] = 'I';
	header[i++] = 'F';

	/* Version number (has to be 0) */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, 0);

	/* Size of the header, in bytes */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, VP8_SEQUENCE_HEADER_SIZE);

	/* Codec FourCC ("VP80") */
	header[i++] = 'V';
	header[i++] = 'P';
	header[i++] = '8';
	header[i++] = '0';

	/* Frame width and height, in pixels */
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, frame_width);
	WRITE_16BIT_LE_AND_INCR_IDX(header, i, frame_height);

	/* Frame rate numerator and denominator */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, fps_numerator);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, fps_denominator);

	/* Number of frames */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, num_frames);

	/* Unused bytes */
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, 0);
}


void imx_vpu_api_insert_vp8_ivf_frame_header(uint8_t *header, size_t main_data_size, uint64_t pts)
{
	int i = 0;
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, main_data_size);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, (pts >> 0) & 0xFFFFFFFF);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, (pts >> 32) & 0xFFFFFFFF);
}


void imx_vpu_api_insert_wmv3_sequence_layer_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height, size_t main_data_size, uint8_t const *codec_data)
{
	/* Header as specified in the VC-1 specification, Annex J and L,
	 * L.2 , Sequence Layer. */

	/* We deviate a bit from the spec here. The spec mentions a
	 * value 0xC5 for the constant byte. Bit #6 is set in 0xC5,
	 * and this bit #6 specifies that RCV V2 is used. We however
	 * use RCV V1, so we have to clear that bit, leaving us with
	 * a byte value that is 0x85, not 0xC5. */
	/* XXX: The other bits of this byte are less clear. The code
	 * from imx-vpuwrap indicates that bit #7 enables an
	 * "extended header", while at least bits 0-2 specify a
	 * "codec version". Nothing more is known at this point. */
	uint32_t const constant_byte = 0x85;
	/* 0xFFFFFF is special value denoting an infinite sequence.
	 * Since the number of frames isn't known at this point,
	 * use that to not have to require any frame count here. */
	uint32_t const num_frames = 0xFFFFFF;
	uint32_t const struct_c_values = (constant_byte << 24) | num_frames;
	uint32_t const ext_header_length = 4;

	int i = 0;

	WRITE_32BIT_LE_AND_INCR_IDX(header, i, struct_c_values);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, ext_header_length);

	memcpy(&(header[i]), codec_data, 4);
	i += 4;

	WRITE_32BIT_LE_AND_INCR_IDX(header, i, frame_height);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, frame_width);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, main_data_size);
}


void imx_vpu_api_insert_wmv3_frame_layer_header(uint8_t *header, size_t main_data_size)
{
	/* Header as specified in the VC-1 specification, Annex J and L,
	 * L.3 , Frame Layer. This is an RCV1 frame layer header however,
	 * not an RCV2 one (which is what the VC-1 specification describes).
	 * RCV1 frame layer headers only contain the framesize. RCV2 ones
	 * contain the framesize, the key frame indicator, and a timestamp.
	 * Since we generate RCV1 headers, just store the framesize. */
	WRITE_32BIT_LE(header, 0, main_data_size);
}


void imx_vpu_api_insert_vc1_frame_layer_header(uint8_t *header, uint8_t *main_data, size_t *actual_header_length)
{
	static uint8_t const start_code_prefix[3] = { 0x00, 0x00, 0x01 };

	/* Detect if a start code is present; if not, insert one.
	 * Detection works according to SMPTE 421M Annex E E.2.1:
	 * If the first two bytes are 0x00, and the third byte is
	 * 0x01, then this is a start code. Otherwise, it isn't
	 * one, and a frame start code is inserted. */
	if (memcmp(main_data, start_code_prefix, 3) != 0)
	{
		static uint8_t const frame_start_code[4] = { 0x00, 0x00, 0x01, 0x0D };
		memcpy(header, frame_start_code, 4);
		*actual_header_length = 4;
	}
	else
		*actual_header_length = 0;
}


void imx_vpu_api_insert_divx3_frame_header(uint8_t *header, unsigned int frame_width, unsigned int frame_height)
{
	int i = 0;
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, frame_width);
	WRITE_32BIT_LE_AND_INCR_IDX(header, i, frame_height);
}


int imx_vpu_api_parse_jpeg_header(void *jpeg_data, size_t jpeg_data_size, BOOL semi_planar_output, unsigned int *width, unsigned int *height, ImxVpuApiColorFormat *color_format)
{
	uint8_t *jpeg_data_start = jpeg_data;
	uint8_t *jpeg_data_end = jpeg_data_start + jpeg_data_size;
	uint8_t *jpeg_data_cur = jpeg_data_start;
	int found_info = 0;

#define READ_UINT8(value) do \
	{ \
		(value) = *jpeg_data_cur; \
		++jpeg_data_cur; \
	} \
	while (0)
#define READ_UINT16(value) do \
	{ \
		uint16_t w = *((uint16_t *)jpeg_data_cur); \
		jpeg_data_cur += 2; \
		(value) = ( ((w & 0xff) << 8) | ((w & 0xff00) >> 8) ); \
	} \
	while (0)

	while (jpeg_data_cur < jpeg_data_end)
	{
		uint8_t marker;

		/* Marker is preceded by byte 0xFF */
		if (*(jpeg_data_cur++) != 0xff)
			break;

		READ_UINT8(marker);
		if (marker == JPEG_MARKER_SOS)
			break;

		switch (marker)
		{
			case JPEG_MARKER_SOI:
				break;
			case JPEG_MARKER_DRI:
				jpeg_data_cur += 4;
				break;

			case JPEG_MARKER_SOF2:
			{
				IMX_VPU_API_ERROR("progressive JPEGs are not supported");
				return 0;
			}

			case JPEG_MARKER_SOF0:
			{
				uint16_t length;
				uint8_t num_components;
				uint8_t block_width[3], block_height[3];

				READ_UINT16(length);
				length -= 2;
				IMX_VPU_API_LOG("marker: %#lx length: %u", (unsigned long)marker, length);

				jpeg_data_cur++;
				READ_UINT16(*height);
				READ_UINT16(*width);

				if ((*width) > 8192)
				{
					IMX_VPU_API_ERROR("width of %u pixels exceeds the maximum of 8192", *width);
					return 0;
				}

				if ((*height) > 8192)
				{
					IMX_VPU_API_ERROR("height of %u pixels exceeds the maximum of 8192", *height);
					return 0;
				}

				READ_UINT8(num_components);

				if (num_components <= 3)
				{
					for (int i = 0; i < num_components; ++i)
					{
						uint8_t b;
						++jpeg_data_cur;
						READ_UINT8(b);
						block_width[i] = (b & 0xf0) >> 4;
						block_height[i] = (b & 0x0f);
						++jpeg_data_cur;
					}
				}

				if (num_components > 3)
				{
					IMX_VPU_API_ERROR("JPEGs with %d components are not supported", (int)num_components);
					return 0;
				}
				if (num_components == 3)
				{
					int temp = (block_width[0] * block_height[0]) / (block_width[1] * block_height[1]);

					if ((temp == 4) && (block_height[0] == 2))
						*color_format = semi_planar_output ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT;
					else if ((temp == 2) && (block_height[0] == 1))
						*color_format = semi_planar_output ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT;
					else if ((temp == 2) && (block_height[0] == 2))
						*color_format = semi_planar_output ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_VERTICAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_VERTICAL_8BIT;
					else if ((temp == 1) && (block_height[0] == 1))
						*color_format = semi_planar_output ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT;
					else
						*color_format = IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT;
				}
				else
					*color_format = IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT;

				IMX_VPU_API_LOG("width: %u  height: %u  number of components: %d", *width, *height, (int)num_components);

				found_info = 1;

				break;
			}

			default:
			{
				uint16_t length;
				READ_UINT16(length);
				length -= 2;
				IMX_VPU_API_LOG("marker: %#lx length: %u", (unsigned long)marker, length);
				jpeg_data_cur += length;
			}
		}
	}

	return found_info;
}
