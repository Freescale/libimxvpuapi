/* YUV4MPEG2 (y4m) IO code used by all examples
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

#ifndef Y4M_IO_H_____________
#define Y4M_IO_H_____________

#include <stddef.h>
#include <stdio.h>
#include "imxvpuapi2/imxvpuapi2.h"


typedef struct
{
	size_t width, height;
	size_t y_stride, uv_stride;
	unsigned int fps_num, fps_denom;
	unsigned par_num, par_denom;

	ImxVpuApiInterlacingMode interlacing;

	ImxVpuApiColorFormat color_format;
	int use_semi_planar_uv;


	FILE *file;
	int frame_token_seen;
	size_t y_size[2], uv_size[2];
}
Y4MContext;


int y4m_init(FILE *file, Y4MContext *context, int read_y4m);
int y4m_read_frame(Y4MContext *context, void *y_dest, void *u_dest, void *v_dest);
int y4m_write_frame(Y4MContext const *context, void const *y_src, void const *u_src, void const *v_src);


#endif
