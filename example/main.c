/* main code used by all examples
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

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <getopt.h>
#include "main.h"


static void logging_fn(ImxVpuApiLogLevel level, char const *file, int const line, char const *fn, const char *format, ...)
{
	va_list args;

	char const *lvlstr = "";
	switch (level)
	{
		case IMX_VPU_API_LOG_LEVEL_ERROR: lvlstr = "ERROR"; break;
		case IMX_VPU_API_LOG_LEVEL_WARNING: lvlstr = "WARNING"; break;
		case IMX_VPU_API_LOG_LEVEL_INFO: lvlstr = "info"; break;
		case IMX_VPU_API_LOG_LEVEL_DEBUG: lvlstr = "debug"; break;
		case IMX_VPU_API_LOG_LEVEL_TRACE: lvlstr = "trace"; break;
		case IMX_VPU_API_LOG_LEVEL_LOG: lvlstr = "log"; break;
		default: break;
	}

	fprintf(stderr, "%s:%d (%s)   %s: ", file, line, fn, lvlstr);

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	fprintf(stderr, "\n");
}


static void usage(char *progname)
{
	static char options[] =
		"\t-i input file\n"
		"\t-o output file\n"
		;

	fprintf(stderr, "usage:\t%s [option]\n\noption:\n%s\n", progname, options);
}


static int parse_args(int argc, char **argv, char **input_filename, char **output_filename)
{
	int opt;

	*input_filename = NULL;
	*output_filename = NULL;

	while ((opt = getopt(argc, argv, "i:o:")) != -1)
	{
		switch (opt)
		{
			case 'i':
				*input_filename = optarg;
				break;
			case 'o':
				*output_filename = optarg;
				break;
			default:
				usage(argv[0]);
				return 0;
		}
	}

	if (*input_filename == NULL)
	{
		fprintf(stderr, "Missing input filename\n\n");
		usage(argv[0]);
		return RETVAL_ERROR;
	}

	if (*output_filename == NULL)
	{
		fprintf(stderr, "Missing output filename\n\n");
		usage(argv[0]);
		return RETVAL_ERROR;
	}

	return RETVAL_OK;
}




int main(int argc, char *argv[])
{
	FILE *input_file = NULL;
	FILE *output_file = NULL;
	char *input_filename = NULL;
	char *output_filename = NULL;
	Context *ctx = NULL;
	int ret = 0;

	if (parse_args(argc, argv, &input_filename, &output_filename) != RETVAL_OK)
	{
		ret = 1;
		goto cleanup;
	}

	input_file = fopen(input_filename, "rb");
	if (input_file == NULL)
	{
		fprintf(stderr, "Opening %s for reading failed: %s\n", input_filename, strerror(errno));
		ret = 1;
		goto cleanup;
	}

	output_file = fopen(output_filename, "wb");
	if (output_file == NULL)
	{
		fprintf(stderr, "Opening %s for writing failed: %s\n", output_filename, strerror(errno));
		ret = 1;
		goto cleanup;
	}

	imx_vpu_api_set_logging_threshold(IMX_VPU_API_LOG_LEVEL_TRACE);
	imx_vpu_api_set_logging_function(logging_fn);

	if ((ctx = init(input_file, output_file)) == NULL)
	{
		ret = 1;
		goto cleanup;
	}

	if (run(ctx) == RETVAL_ERROR)
		ret = 1;

cleanup:
	if (ctx != NULL)
		shutdown(ctx);

	if (input_file != NULL)
		fclose(input_file);
	if (output_file != NULL)
		fclose(output_file);

	return ret;
}
