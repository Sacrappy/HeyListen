/* (mkw-recomp) Included first in every source of the WebGPU renderer (nx2/build.py, -include): the functions the
   game executable calls are renamed rx_* -> wgr_*, and host/rx_backend.c defines the rx_* names, sending each call
   to this renderer or to the Vulkan one (mkw_vk.dll, RX_BACKEND=vulkan). */
#pragma once
#define rx_start wgr_start
#define rx_resize wgr_resize
#define rx_describe wgr_describe
#define rx_stats wgr_stats
#define rx_nvn_submit wgr_nvn_submit
#define rx_nvn_fence wgr_nvn_fence
#define rx_nvn_pushed wgr_nvn_pushed
#define rx_nvn_replayed wgr_nvn_replayed
#define rx_nvn_wait wgr_nvn_wait
#define rx_wait_writebacks_reading wgr_wait_writebacks_reading
#define rx_nvn_finish wgr_nvn_finish
#define rx_nvn_present wgr_nvn_present
#define rx_nvn_register_texture wgr_nvn_register_texture
#define rx_nvn_register_sampler wgr_nvn_register_sampler
#define rx_nvn_set_texture_pool_size wgr_nvn_set_texture_pool_size
#define rx_nvn_texture_released wgr_nvn_texture_released
#define rx_nvn_program_released wgr_nvn_program_released
#define rx_nvn_read_texels wgr_nvn_read_texels
#define rx_nvn_write_texels wgr_nvn_write_texels
#define rx_nvn_invalidate wgr_nvn_invalidate
