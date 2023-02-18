#include <linux/ipu.h>

/* ipu.h #defines these types for some reason, even though these
 * definitions aren't used anywhere within ipu.h. However, they
 * do cause collisions with the datatype definitions in stdint.h,
 * so we must get rid of them before including the other headers. */
#ifdef uint8_t
#undef uint8_t
#endif
#ifdef uint16_t
#undef uint16_t
#endif
#ifdef uint32_t
#undef uint32_t
#endif

#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "imxvpuapi2_imx6_coda_ipu.h"


static uint32_t get_ipu_pixel_format(ImxVpuApiColorFormat color_format)
{
	switch (color_format)
	{
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV420_8BIT: return IPU_PIX_FMT_YUV420P;
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT:  return IPU_PIX_FMT_NV12;
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV411_8BIT: return IPU_PIX_FMT_YUV410P;
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV422_HORIZONTAL_8BIT: return IPU_PIX_FMT_YUV422P;
		case IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV422_HORIZONTAL_8BIT: return IPU_PIX_FMT_NV16;
		case IMX_VPU_API_COLOR_FORMAT_FULLY_PLANAR_YUV444_8BIT: return IPU_PIX_FMT_YUV444P;
		case IMX_VPU_API_COLOR_FORMAT_YUV400_8BIT: return IPU_PIX_FMT_GREY;
		default: return 0;
	}
}


int imx_vpu_api_imx6_coda_open_ipu_voda_fd(void)
{
	int ipu_vdoa_fd = open("/dev/mxc_ipu", O_RDWR, 0);
	if (ipu_vdoa_fd < 0)
	{
		IMX_VPU_API_ERROR("could not open /dev/mxc_ipu: %s (%d)", strerror(errno), errno);
		return -1;
	}

	IMX_VPU_API_TRACE("opened IPU VDOA file descriptor %d", ipu_vdoa_fd);

	return ipu_vdoa_fd;
}


void imx_vpu_api_imx6_coda_close_ipu_voda_fd(int fd)
{
	if (fd < 0)
		return;

	close(fd);

	IMX_VPU_API_TRACE("closed IPU VDOA file descriptor %d", fd);
}


BOOL imx_vpu_api_imx6_coda_detile_and_copy_frame_with_ipu_vdoa(
	int ipu_vdoa_fd,
	ImxDmaBuffer *src_fb_dma_buffer,
	ImxDmaBuffer *dest_fb_dma_buffer,
	size_t total_padded_input_width, size_t total_padded_input_height,
	size_t total_padded_output_width, size_t total_padded_output_height,
	size_t actual_frame_width, size_t actual_frame_height,
	ImxVpuApiColorFormat color_format
)
{
	int ioctl_ret;
	struct ipu_task task = { 0 };

	imx_physical_address_t src_paddr = imx_dma_buffer_get_physical_address(src_fb_dma_buffer);
	imx_physical_address_t dest_paddr = imx_dma_buffer_get_physical_address(dest_fb_dma_buffer);

	task.overlay_en = 0;
	task.priority = IPU_TASK_PRIORITY_NORMAL;
	task.task_id = IPU_TASK_ID_ANY;
	task.timeout = 0;

	IMX_VPU_API_LOG(
		"ipu task:  total padded input/output size %zux%zu / %zux%zu  actual size %zux%zu  src/dest paddr %" IMX_PHYSICAL_ADDRESS_FORMAT "/%" IMX_PHYSICAL_ADDRESS_FORMAT "  output color format: %s",
		total_padded_input_width, total_padded_input_height,
		total_padded_output_width, total_padded_output_height,
		actual_frame_width, actual_frame_height,
		src_paddr, dest_paddr,
		imx_vpu_api_color_format_string(color_format)
	);

	task.input.width = total_padded_input_width;
	task.input.height = total_padded_input_height;
	task.input.format = IPU_PIX_FMT_TILED_NV12;
	task.input.crop.pos.x = 0;
	task.input.crop.pos.y = 0;
	task.input.crop.w = total_padded_input_width;
	task.input.crop.h = total_padded_input_height;
	task.input.paddr = src_paddr;
	task.input.paddr_n = 0;
	task.input.deinterlace.enable = 0;
	task.input.deinterlace.motion = HIGH_MOTION;

	task.output.width = total_padded_output_width;
	task.output.height = total_padded_output_height;
	task.output.format = get_ipu_pixel_format(color_format);
	task.output.rotate = IPU_ROTATE_NONE;
	task.output.crop.pos.x = 0;
	task.output.crop.pos.y = 0;
	task.output.crop.w = total_padded_output_width;
	task.output.crop.h = total_padded_output_height;
	task.output.paddr = dest_paddr;

	if (task.output.format == 0)
	{
		IMX_VPU_API_ERROR("IPU does not support pixel format %s (%d)", imx_vpu_api_color_format_string(color_format), color_format);
		return FALSE;
	}

	if ((ioctl_ret = ioctl(ipu_vdoa_fd, IPU_QUEUE_TASK, &task)) == -1)
	{
		IMX_VPU_API_ERROR("queuing IPU task failed: %s (%d)", strerror(errno), errno);
		return FALSE;
	}

	return TRUE;
}
