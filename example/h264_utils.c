#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "h264_utils.h"


void h264_ctx_init(h264_context *ctx, FILE *fin)
{
	ctx->fin = fin;

	ctx->in_buffer = NULL;
	ctx->in_buffer_allocated_size = 0;
	ctx->in_buffer_data_size = 0;

	ctx->au_start_offset = 0;
	ctx->au_end_offset = 0;
	ctx->au_finished = 0;
	ctx->first_au = 1;
}


void h264_ctx_cleanup(h264_context *ctx)
{
	if (ctx->in_buffer != NULL)
		free(ctx->in_buffer);
}


int h264_ctx_read_access_unit(h264_context *ctx)
{
	static size_t const alloc_step_size = (256*1024);
	static size_t const read_size = (64*1024);
	int num_aud_found = 0;
	size_t cur_offset = 0;

	if (ctx->au_finished)
	{
		unsigned int data_size = ctx->in_buffer_data_size - ctx->au_end_offset;
		memmove(ctx->in_buffer, ctx->in_buffer + ctx->au_end_offset, data_size);
		ctx->in_buffer_data_size = data_size;
		ctx->au_start_offset = 0;
		ctx->au_end_offset = 0;
		ctx->au_finished = 0;
	}

	while (1)
	{
		uint8_t *newptr;
		size_t num_read;

		while ((ctx->in_buffer != NULL) && (cur_offset < ctx->in_buffer_data_size))
		{
			uint8_t const *bytes = &(ctx->in_buffer[cur_offset]);

			if ((bytes[0] == 0x00) && (bytes[1] == 0x00) && (bytes[2] == 0x01))
			{
				if ((bytes[3] & 0xF) == 0x09)
				{
					++num_aud_found;
					if (num_aud_found == 1)
					{
						if (ctx->first_au)
							ctx->au_start_offset = 0;
						else
							ctx->au_start_offset = cur_offset;
					}
					else if (num_aud_found == 2)
					{
						ctx->au_end_offset = cur_offset;
						ctx->au_finished = 1;
						ctx->first_au = 0;
						//fprintf(stderr, "AU finished, start %zu end %zu (%zu bytes)\n", ctx->au_start_offset, ctx->au_end_offset, ctx->au_end_offset - ctx->au_start_offset);
						return 1;
					}
				}
				cur_offset += 4;
			}
			else
				++cur_offset;
		}

		//fprintf(stderr, "need to read more bytes, current stat: %zu bytes %zu allocated\n", ctx->in_buffer_data_size, ctx->in_buffer_allocated_size);

		if ((ctx->in_buffer_data_size + read_size) >= ctx->in_buffer_allocated_size)
		{
			newptr = realloc(ctx->in_buffer, ctx->in_buffer_allocated_size + alloc_step_size);
			if (newptr == NULL)
			{
				ctx->au_end_offset = cur_offset;
				return 0;
			}
			ctx->in_buffer = newptr;
			ctx->in_buffer_allocated_size += alloc_step_size;
		}

		num_read = fread(ctx->in_buffer + ctx->in_buffer_data_size, 1, read_size, ctx->fin);
		if (num_read < 4) /* 4 because of the 3-byte start code prefix + the NAL type */
		{
			ctx->au_end_offset = cur_offset;
			if (ferror(ctx->fin))
				fprintf(stderr, "Reading failed: %s", strerror(errno));
			return 0;
		}
		ctx->in_buffer_data_size += num_read;
	}

	return 0;
}

