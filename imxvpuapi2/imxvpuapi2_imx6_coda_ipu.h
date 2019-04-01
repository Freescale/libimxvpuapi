#ifndef IMXVPUAPI2_IMX6_CODA_IPU_H
#define IMXVPUAPI2_IMX6_CODA_IPU_H

#include <imxdmabuffer/imxdmabuffer.h>
#include "imxvpuapi2.h"
#include "imxvpuapi2_priv.h"


int imx_vpu_api_imx6_coda_open_ipu_voda_fd(void);
void imx_vpu_api_imx6_coda_close_ipu_voda_fd(int fd);

BOOL imx_vpu_api_imx6_coda_detile_and_copy_frame_with_ipu_vdoa(
	int ipu_vdoa_fd,
	ImxDmaBuffer *src_fb_dma_buffer,
	ImxDmaBuffer *dest_fb_dma_buffer,
	size_t total_padded_input_width, size_t total_padded_input_height,
	size_t total_padded_output_width, size_t total_padded_output_height,
	size_t actual_frame_width, size_t actual_frame_height,
	ImxVpuApiColorFormat color_format
);


#endif /* IMXVPUAPI2_IMX6_CODA_IPU_H */
