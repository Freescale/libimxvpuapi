#ifndef H264_UTILS_H____
#define H264_UTILS_H____

#include <stdio.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


typedef struct
{
	FILE *fin;
	uint8_t *in_buffer;
	size_t in_buffer_allocated_size, in_buffer_data_size;
	size_t au_start_offset, au_end_offset;
	int au_finished, first_au;
}
h264_context;


void h264_ctx_init(h264_context *ctx, FILE *fin);
void h264_ctx_cleanup(h264_context *ctx);
int h264_ctx_read_access_unit(h264_context *ctx);


#ifdef __cplusplus
}
#endif


#endif
