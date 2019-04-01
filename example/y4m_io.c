#include <stdlib.h>
#include <string.h>
#include "y4m_io.h"


int y4m_init(FILE *file, Y4MContext *context, int read_y4m)
{
	char token[1000];

	context->file = file;

	if (read_y4m)
	{
		if ((fscanf(file, "%s ", token) <= 0) || (strncmp(token, "YUV4MPEG2", 9) != 0))
			return 0;

		while (fscanf(file, "%s ", token) > 0)
		{
			if (strncmp(token, "FRAME", 5) == 0)
			{
				context->frame_token_seen = 1;
				break;
			}

			switch (token[0])
			{
				case 'W':
				{
					context->width = atoi(&(token[1]));
					if (context->width == 0)
						return 0;
					break;
				}

				case 'H':
				{
					context->height = atoi(&(token[1]));
					if (context->height == 0)
						return 0;
					break;
				}

				case 'F':
				{
					if (sscanf(&(token[1]), "%u:%u", &(context->fps_num), &(context->fps_denom)) <= 0)
						return 0;
					break;
				}

				case 'I':
				{
					switch (token[1])
					{
						case 'p': context->interlacing = IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING; break;
						case 't': context->interlacing = IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_FIRST; break;
						case 'b': context->interlacing = IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST; break;
						case 'm':
						default: context->interlacing = IMX_VPU_API_INTERLACING_MODE_UNKNOWN; break;
					}
					break;
				}

				case 'A':
				{
					if (sscanf(&(token[1]), "%u:%u", &(context->par_num), &(context->par_denom)) <= 0)
						return 0;
					break;
				}

				case 'C':
				{
					if (strncmp(&(token[1]), "420", 3) == 0)
					{
						context->color_format = context->use_semi_planar_uv ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT;
					}
					else if (strncmp(&(token[1]), "422", 3) == 0)
					{
						context->color_format = context->use_semi_planar_uv ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT;
					}
					else if (strncmp(&(token[1]), "444", 3) == 0)
					{
						context->color_format = context->use_semi_planar_uv ? IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT : IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT;
					}
					else
						return 0;
					break;
				}

				default:
					break;
			}
		}
	}
	else
	{
		char interlacing_char;
		char const *colorspace_str;

		switch (context->interlacing)
		{
			case IMX_VPU_API_INTERLACING_MODE_NO_INTERLACING: interlacing_char = 'p'; break;
			case IMX_VPU_API_INTERLACING_MODE_TOP_FIELD_FIRST: interlacing_char = 't'; break;
			case IMX_VPU_API_INTERLACING_MODE_BOTTOM_FIELD_FIRST: interlacing_char = 'b'; break;
			case IMX_VPU_API_INTERLACING_MODE_UNKNOWN: interlacing_char = 'p'; break;
			default:
				return 0;
		}

		switch (context->color_format)
		{
			case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
			case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
				colorspace_str = "420";
				break;

			case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
			case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
				colorspace_str = "422";
				break;

			case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
			case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
				colorspace_str = "444";
				break;

			default:
				return 0;
		}

		context->use_semi_planar_uv = imx_vpu_api_is_color_format_semi_planar(context->color_format);

		if (fprintf(file, "YUV4MPEG2 W%zu H%zu F%u:%u I%c A%u:%u C%s\n", context->width, context->height, context->fps_num, context->fps_denom, interlacing_char, context->par_num, context->par_denom, colorspace_str) < 0)
			return 0;
	}

	switch (context->color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:
			context->y_size[0] = context->width;
			context->y_size[1] = context->height;
			context->uv_size[0] = context->width / 2;
			context->uv_size[1] = context->height / 2;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT:
			context->y_size[0] = context->width;
			context->y_size[1] = context->height;
			context->uv_size[0] = context->width;
			context->uv_size[1] = context->height / 2;
			break;

		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT:
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV444_8BIT:
			context->y_size[0] = context->width;
			context->y_size[1] = context->height;
			context->uv_size[0] = context->width;
			context->uv_size[1] = context->height;
			break;

		default:
			return 0;
	}

	return 1;
}


int y4m_read_frame(Y4MContext *context, void *y_dest, void *u_dest, void *v_dest)
{
	size_t y;
	uint8_t *dest;

	if (!context->frame_token_seen)
	{
		char token[1000];
		if (fread(token, 1, 5, context->file) < 5)
			return 0;

		if (strncmp(token, "FRAME", 5) != 0)
		{
			return 0;
		}

		for (;;)
		{
			int c = fgetc(context->file);

			if (c < 0)
				return 0;
			else if (c == '\n')
				break;
		}
	}

	context->frame_token_seen = 0;

	dest = y_dest;
	for (y = 0; y < context->y_size[1]; ++y)
	{
		if (fread(dest, 1, context->y_size[0], context->file) < context->y_size[0])
			return 0;
		dest += context->y_stride;
	}

	if (context->use_semi_planar_uv)
	{
		int plane;

		for (plane = 0; plane < 2; ++plane)
		{
			dest = u_dest;
			for (y = 0; y < context->uv_size[1]; ++y)
			{
				size_t x;
				for (x = 0; x < context->uv_size[0]; ++x)
				{
					int b = fgetc(context->file);
					if (b < 0)
						return 0;
					dest[(x * 2) + plane] = b;
				}
				dest += context->uv_stride;
			}
		}
	}
	else
	{
		dest = u_dest;
		for (y = 0; y < context->uv_size[1]; ++y)
		{
			if (fread(dest, 1, context->uv_size[0], context->file) < context->uv_size[0])
				return 0;
			dest += context->uv_stride;
		}

		dest = v_dest;
		for (y = 0; y < context->uv_size[1]; ++y)
		{
			if (fread(dest, 1, context->uv_size[0], context->file) < context->uv_size[0])
				return 0;
			dest += context->uv_stride;
		}
	}

	return 1;
}


int y4m_write_frame(Y4MContext const *context, void const *y_src, void const *u_src, void const *v_src)
{
	size_t y;
	uint8_t const *src;

	fprintf(context->file, "FRAME\n");

	src = y_src;
	for (y = 0; y < context->y_size[1]; ++y)
	{
		if (fwrite(src, 1, context->y_size[0], context->file) < context->y_size[0])
			return 0;
		src += context->y_stride;
	}

	if (context->use_semi_planar_uv)
	{
		int plane;

		for (plane = 0; plane < 2; ++plane)
		{
			src = u_src;
			for (y = 0; y < context->uv_size[1]; ++y)
			{
				size_t x;
				for (x = 0; x < context->uv_size[0]; ++x)
				{
					if (fputc(src[(x * 2) + plane], context->file) < 0)
						return 0;
				}
				src += context->uv_stride;
			}
		}
	}
	else
	{
		src = u_src;
		for (y = 0; y < context->uv_size[1]; ++y)
		{
			if (fwrite(src, 1, context->uv_size[0], context->file) < context->uv_size[0])
				return 0;
			src += context->uv_stride;
		}

		src = v_src;
		for (y = 0; y < context->uv_size[1]; ++y)
		{
			if (fwrite(src, 1, context->uv_size[0], context->file) < context->uv_size[0])
				return 0;
			src += context->uv_stride;
		}
	}

	return 1;
}
