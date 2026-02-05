#ifndef YOS_GPU_H
#define YOS_GPU_H

#include <stdint.h>
#include <yos/ioctl.h>

#define YOS_GPU_IOC_TYPE 'G'
#define YOS_GPU_ABI_VERSION 1u

#define YOS_GPU_INFO_FLAG_ACTIVE 1u
#define YOS_GPU_INFO_FLAG_VIRGL  2u

#define YOS_GPU_MAX_SCANOUTS 16u

#define YOS_GPU_FORMAT_B8G8R8X8_UNORM 2u

typedef struct {
    uint32_t abi_version;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t scanout_id;
    uint32_t reserved0;
} yos_gpu_info_t;

typedef struct {
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} yos_gpu_resource_create_2d_t;

typedef struct {
    uint32_t resource_id;
    int32_t  shm_fd;
    uint32_t shm_offset;
    uint32_t size_bytes;
} yos_gpu_resource_attach_shm_t;

typedef struct {
    uint32_t scanout_id;
    uint32_t resource_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} yos_gpu_set_scanout_t;

typedef struct {
    uint32_t resource_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} yos_gpu_rect_t;

typedef struct {
    uint32_t resource_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint64_t offset;
} yos_gpu_transfer_to_host_2d_t;

typedef struct {
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
} yos_gpu_resource_create_3d_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
} yos_gpu_box_t;

typedef struct {
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
    yos_gpu_box_t box;
    uint64_t offset;
} yos_gpu_transfer_host_3d_t;

typedef struct {
    uint32_t dst_resource_id;
    uint32_t dst_level;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t dst_z;
    uint32_t src_resource_id;
    uint32_t src_level;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} yos_gpu_copy_region_3d_t;

#define YOS_GPU_GET_INFO                _YOS_IOR(YOS_GPU_IOC_TYPE, 0x00, yos_gpu_info_t)
#define YOS_GPU_RESOURCE_CREATE_2D      _YOS_IOW(YOS_GPU_IOC_TYPE, 0x01, yos_gpu_resource_create_2d_t)
#define YOS_GPU_RESOURCE_ATTACH_SHM     _YOS_IOW(YOS_GPU_IOC_TYPE, 0x02, yos_gpu_resource_attach_shm_t)
#define YOS_GPU_RESOURCE_DETACH_BACKING _YOS_IOW(YOS_GPU_IOC_TYPE, 0x03, uint32_t)
#define YOS_GPU_RESOURCE_UNREF          _YOS_IOW(YOS_GPU_IOC_TYPE, 0x04, uint32_t)
#define YOS_GPU_SET_SCANOUT             _YOS_IOW(YOS_GPU_IOC_TYPE, 0x05, yos_gpu_set_scanout_t)
#define YOS_GPU_TRANSFER_TO_HOST_2D     _YOS_IOW(YOS_GPU_IOC_TYPE, 0x06, yos_gpu_transfer_to_host_2d_t)
#define YOS_GPU_RESOURCE_FLUSH          _YOS_IOW(YOS_GPU_IOC_TYPE, 0x07, yos_gpu_rect_t)
#define YOS_GPU_RESOURCE_CREATE_3D      _YOS_IOW(YOS_GPU_IOC_TYPE, 0x08, yos_gpu_resource_create_3d_t)
#define YOS_GPU_TRANSFER_TO_HOST_3D     _YOS_IOW(YOS_GPU_IOC_TYPE, 0x09, yos_gpu_transfer_host_3d_t)
#define YOS_GPU_RESOURCE_COPY_REGION_3D _YOS_IOW(YOS_GPU_IOC_TYPE, 0x0A, yos_gpu_copy_region_3d_t)

#endif
