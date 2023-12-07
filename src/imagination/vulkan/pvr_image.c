/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_formats.h"
#include "pvr_private.h"
#include "pvr_tex_state.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_format.h"
#include "vk_image.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "drm-uapi/drm_fourcc.h"

static void pvr_image_init_memlayout(struct pvr_image *image)
{
   switch (image->vk.tiling) {
   default:
      unreachable("bad VkImageTiling");
   case VK_IMAGE_TILING_OPTIMAL:
      if (image->vk.wsi_legacy_scanout)
         image->memlayout = PVR_MEMLAYOUT_LINEAR;
      else if (image->vk.image_type == VK_IMAGE_TYPE_3D)
         image->memlayout = PVR_MEMLAYOUT_3DTWIDDLED;
      else
         image->memlayout = PVR_MEMLAYOUT_TWIDDLED;
      break;
   case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:
   case VK_IMAGE_TILING_LINEAR:
      image->memlayout = PVR_MEMLAYOUT_LINEAR;
      break;
   }
}

static void pvr_image_init_physical_extent(struct pvr_image *image)
{
   assert(image->memlayout != PVR_MEMLAYOUT_UNDEFINED);

   /* clang-format off */
   if (image->vk.mip_levels > 1 ||
      image->memlayout == PVR_MEMLAYOUT_TWIDDLED ||
      image->memlayout == PVR_MEMLAYOUT_3DTWIDDLED) {
      /* clang-format on */
      image->physical_extent.width =
         util_next_power_of_two(image->vk.extent.width);
      image->physical_extent.height =
         util_next_power_of_two(image->vk.extent.height);
      image->physical_extent.depth =
         util_next_power_of_two(image->vk.extent.depth);
   } else {
      assert(image->memlayout == PVR_MEMLAYOUT_LINEAR);
      image->physical_extent = image->vk.extent;
   }
}

static void pvr_image_setup_mip_levels(struct pvr_image *image)
{
   const uint32_t extent_alignment =
      image->vk.image_type == VK_IMAGE_TYPE_3D ? 4 : 1;
   const unsigned int cpp = vk_format_get_blocksize(image->vk.format);
   VkExtent3D extent =
      vk_image_extent_to_elements(&image->vk, image->physical_extent);

   /* Mip-mapped textures that are non-dword aligned need dword-aligned levels
    * so they can be TQd from.
    */
   const uint32_t level_alignment = image->vk.mip_levels > 1 ? 4 : 1;

   assert(image->vk.mip_levels <= ARRAY_SIZE(image->mip_levels));

   image->layer_size = 0;

   for (uint32_t i = 0; i < image->vk.mip_levels; i++) {
      struct pvr_mip_level *mip_level = &image->mip_levels[i];

      mip_level->pitch = cpp * ALIGN(extent.width, extent_alignment);
      mip_level->height_pitch = ALIGN(extent.height, extent_alignment);
      mip_level->size = image->vk.samples * mip_level->pitch *
                        mip_level->height_pitch *
                        ALIGN(extent.depth, extent_alignment);
      mip_level->size = ALIGN(mip_level->size, level_alignment);
      mip_level->offset = image->layer_size;

      image->layer_size += mip_level->size;

      extent.height = u_minify(extent.height, 1);
      extent.width = u_minify(extent.width, 1);
      extent.depth = u_minify(extent.depth, 1);
   }

   /* The hw calculates layer strides as if a full mip chain up until 1x1x1
    * were present so we need to account for that in the `layer_size`.
    */
   while (extent.height != 1 || extent.width != 1 || extent.depth != 1) {
      const uint32_t height_pitch = ALIGN(extent.height, extent_alignment);
      const uint32_t pitch = cpp * ALIGN(extent.width, extent_alignment);

      image->layer_size += image->vk.samples * pitch * height_pitch *
                           ALIGN(extent.depth, extent_alignment);

      extent.height = u_minify(extent.height, 1);
      extent.width = u_minify(extent.width, 1);
      extent.depth = u_minify(extent.depth, 1);
   }

   /* TODO: It might be useful to store the alignment in the image so it can be
    * checked (via an assert?) when setting
    * RGX_CR_TPU_TAG_CEM_4K_FACE_PACKING_EN, assuming this is where the
    * requirement comes from.
    */
   if (image->vk.array_layers > 1)
      image->layer_size = ALIGN(image->layer_size, image->alignment);

   image->size = image->layer_size * image->vk.array_layers;
}

static VkResult
create_image(struct pvr_device *device, const VkImageCreateInfo *pCreateInfo,
             const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   struct pvr_image *image;

   image =
      vk_image_create(&device->vk, pCreateInfo, pAllocator, sizeof(*image));
   if (!image)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* All images aligned to 4k, in case of arrays/CEM.
    * Refer: pvr_GetImageMemoryRequirements for further details.
    */
   image->alignment = 4096U;

   /* Initialize the image using the saved information from pCreateInfo */
   pvr_image_init_memlayout(image);
   pvr_image_init_physical_extent(image);
   pvr_image_setup_mip_levels(image);

   *pImage = pvr_image_to_handle(image);

   return VK_SUCCESS;
}

static struct pvr_image *
pvr_wsi_get_image_from_swapchain(VkSwapchainKHR swapchain, uint32_t index)
{
   VkImage _image = wsi_common_get_image(swapchain, index);
   PVR_FROM_HANDLE(pvr_image, image, _image);
   return image;
}

static VkResult
create_image_from_swapchain(struct pvr_device *device,
                            const VkImageCreateInfo *pCreateInfo,
                            const VkImageSwapchainCreateInfoKHR *swapchain_info,
                            const VkAllocationCallbacks *pAllocator,
                            VkImage *pImage)
{
   struct pvr_image *swapchain_image =
      pvr_wsi_get_image_from_swapchain(swapchain_info->swapchain, 0);
   assert(swapchain_image);

   VkImageCreateInfo local_create_info = *pCreateInfo;
   local_create_info.pNext = NULL;

   /* Added by wsi code. */
   local_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   /* The spec requires TILING_OPTIMAL as input, but the swapchain image may
    * privately use a different tiling.  See spec anchor
    * #swapchain-wsi-image-create-info .
    */
   assert(local_create_info.tiling == VK_IMAGE_TILING_OPTIMAL);
   local_create_info.tiling = swapchain_image->vk.tiling;

   VkImageDrmFormatModifierListCreateInfoEXT local_modifier_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1,
      .pDrmFormatModifiers = &swapchain_image->vk.drm_format_mod,
   };

   if (swapchain_image->vk.drm_format_mod != DRM_FORMAT_MOD_INVALID)
      __vk_append_struct(&local_create_info, &local_modifier_info);

   assert(swapchain_image->vk.image_type == local_create_info.imageType);
   assert(swapchain_image->vk.format == local_create_info.format);
   assert(swapchain_image->vk.extent.width == local_create_info.extent.width);
   assert(swapchain_image->vk.extent.height == local_create_info.extent.height);
   assert(swapchain_image->vk.extent.depth == local_create_info.extent.depth);
   assert(swapchain_image->vk.array_layers == local_create_info.arrayLayers);
   assert(swapchain_image->vk.samples == local_create_info.samples);
   assert(swapchain_image->vk.tiling == local_create_info.tiling);
   assert((swapchain_image->vk.usage & local_create_info.usage) ==
          local_create_info.usage);

   return create_image(device, &local_create_info, pAllocator, pImage);
}


VkResult pvr_CreateImage(VkDevice _device,
                         const VkImageCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkImage *pImage)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   const VkImageSwapchainCreateInfoKHR *swapchain_info =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_SWAPCHAIN_CREATE_INFO_KHR);

   if (swapchain_info && swapchain_info->swapchain != VK_NULL_HANDLE)
      return create_image_from_swapchain(device, pCreateInfo, swapchain_info,
                                         pAllocator, pImage);

   return create_image(device, pCreateInfo, pAllocator, pImage);
}

void pvr_DestroyImage(VkDevice _device,
                      VkImage _image,
                      const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   PVR_FROM_HANDLE(pvr_image, image, _image);

   if (!image)
      return;

   if (image->vma)
      pvr_unbind_memory(device, image->vma);

   vk_image_destroy(&device->vk, pAllocator, &image->vk);
}

/* clang-format off */
/* Consider a 4 page buffer object.
 *   _________________________________________
 *  |         |          |         |          |
 *  |_________|__________|_________|__________|
 *                  |
 *                  \__ offset (0.5 page size)
 *
 *                  |___size(2 pages)____|
 *
 *            |__VMA size required (3 pages)__|
 *
 *                  |
 *                  \__ returned dev_addr = vma + offset % page_size
 *
 *   VMA size = align(size + offset % page_size, page_size);
 *
 *   Note: the above handling is currently divided between generic
 *   driver code and winsys layer. Given are the details of how this is
 *   being handled.
 *   * As winsys vma allocation interface does not have offset information,
 *     it can not calculate the extra size needed to adjust for the unaligned
 *     offset. So generic code is responsible for allocating a VMA that has
 *     extra space to deal with the above scenario.
 *   * Remaining work of mapping the vma to bo is done by vma_map interface,
 *     as it contains offset information, we don't need to do any adjustments
 *     in the generic code for this part.
 *
 *  TODO: Look into merging heap_alloc and vma_map into single interface.
 */
/* clang-format on */

VkResult pvr_BindImageMemory2(VkDevice _device,
                              uint32_t bindInfoCount,
                              const VkBindImageMemoryInfo *pBindInfos)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   uint32_t i;

   for (i = 0; i < bindInfoCount; i++) {
      PVR_FROM_HANDLE(pvr_device_memory, mem, pBindInfos[i].memory);
      PVR_FROM_HANDLE(pvr_image, image, pBindInfos[i].image);

      VkResult result = pvr_bind_memory(device,
                                        mem,
                                        pBindInfos[i].memoryOffset,
                                        image->size,
                                        image->alignment,
                                        &image->vma,
                                        &image->dev_addr);
      if (result != VK_SUCCESS) {
         while (i--) {
            PVR_FROM_HANDLE(pvr_image, image, pBindInfos[i].image);

            pvr_unbind_memory(device, image->vma);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

void pvr_get_image_subresource_layout(const struct pvr_image *image,
                                      const VkImageSubresource *subresource,
                                      VkSubresourceLayout *layout)
{
   const struct pvr_mip_level *mip_level =
      &image->mip_levels[subresource->mipLevel];

   pvr_assert(subresource->mipLevel < image->vk.mip_levels);
   pvr_assert(subresource->arrayLayer < image->vk.array_layers);

   layout->offset =
      subresource->arrayLayer * image->layer_size + mip_level->offset;
   layout->rowPitch = mip_level->pitch;
   layout->depthPitch = mip_level->pitch * mip_level->height_pitch;
   layout->arrayPitch = image->layer_size;
   layout->size = mip_level->size;
}

void pvr_GetImageSubresourceLayout(VkDevice device,
                                   VkImage _image,
                                   const VkImageSubresource *subresource,
                                   VkSubresourceLayout *layout)
{
   PVR_FROM_HANDLE(pvr_image, image, _image);

   pvr_get_image_subresource_layout(image, subresource, layout);
}

static void pvr_adjust_non_compressed_view(const struct pvr_image *image,
                                           struct pvr_texture_state_info *info)
{
   const uint32_t base_level = info->base_level;

   if (!vk_format_is_compressed(image->vk.format) ||
       vk_format_is_compressed(info->format))
      return;

   /* Cannot use the image state, as the miplevel sizes for an
    * uncompressed chain view may not decrease by 2 each time compared to the
    * compressed one e.g. (22x22,11x11,5x5) -> (6x6,3x3,2x2)
    * Instead manually apply an offset and patch the size
    */
   info->extent.width = u_minify(info->extent.width, base_level);
   info->extent.height = u_minify(info->extent.height, base_level);
   info->extent.depth = u_minify(info->extent.depth, base_level);
   info->extent = vk_image_extent_to_elements(&image->vk, info->extent);
   info->offset += image->mip_levels[base_level].offset;
   info->base_level = 0;
}

VkResult pvr_CreateImageView(VkDevice _device,
                             const VkImageViewCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkImageView *pView)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   struct pvr_texture_state_info info;
   unsigned char input_swizzle[4];
   const uint8_t *format_swizzle;
   const struct pvr_image *image;
   struct pvr_image_view *iview;
   VkResult result;

   iview = vk_image_view_create(&device->vk,
                                false /* driver_internal */,
                                pCreateInfo,
                                pAllocator,
                                sizeof(*iview));
   if (!iview)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   image = pvr_image_view_get_image(iview);

   info.type = iview->vk.view_type;
   info.base_level = iview->vk.base_mip_level;
   info.mip_levels = iview->vk.level_count;
   info.extent = image->vk.extent;
   info.aspect_mask = iview->vk.aspects;
   info.is_cube = (info.type == VK_IMAGE_VIEW_TYPE_CUBE ||
                   info.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
   info.array_size = iview->vk.layer_count;
   info.offset = iview->vk.base_array_layer * image->layer_size;
   info.mipmaps_present = (image->vk.mip_levels > 1) ? true : false;
   info.stride = image->physical_extent.width;
   info.tex_state_type = PVR_TEXTURE_STATE_SAMPLE;
   info.mem_layout = image->memlayout;
   info.flags = 0;
   info.sample_count = image->vk.samples;
   info.addr = image->dev_addr;

   info.format = pCreateInfo->format;
   /* info.format = iview->vk.view_format; */

   /* assert(util_is_power_of_two_nonzero(info.aspect_mask)); */

   pvr_adjust_non_compressed_view(image, &info);

   vk_component_mapping_to_pipe_swizzle(iview->vk.swizzle, input_swizzle);
#if 0
   format_swizzle = pvr_get_format_swizzle(info.format);
#else
   enum pipe_format pipe_format = vk_format_to_pipe_format(iview->vk.view_format);
   if (util_format_is_depth_or_stencil(pipe_format)) {
      switch (pipe_format) {
      case PIPE_FORMAT_S8_UINT:
         pipe_format = PIPE_FORMAT_R8_UINT;
         break;

      case PIPE_FORMAT_Z24X8_UNORM:
         break;

      case PIPE_FORMAT_Z16_UNORM:
         break;

      /* TODO: This is both depth and stencil, how to choose? */
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
         break;

      case PIPE_FORMAT_Z32_FLOAT:
         /* pipe_format = PIPE_FORMAT_R32_FLOAT; */
         break;

      default:
         break;
      }
   }
   format_swizzle = util_format_description(pipe_format)->swizzle;
#endif
   util_format_compose_swizzles(format_swizzle, input_swizzle, info.swizzle);

   result = pvr_pack_tex_state(device,
                               &info,
                               iview->texture_state[info.tex_state_type]);
   if (result != VK_SUCCESS)
      goto err_vk_image_view_destroy;

   /* Create an additional texture state for cube type if storage
    * usage flag is set.
    */
   if (info.is_cube && image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      info.tex_state_type = PVR_TEXTURE_STATE_STORAGE;

      result = pvr_pack_tex_state(device,
                                  &info,
                                  iview->texture_state[info.tex_state_type]);
      if (result != VK_SUCCESS)
         goto err_vk_image_view_destroy;
   }

   if (image->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
      /* Attachment state is created as if the mipmaps are not supported, so the
       * baselevel is set to zero and num_mip_levels is set to 1. Which gives an
       * impression that this is the only level in the image. This also requires
       * that width, height and depth be adjusted as well. Given
       * iview->vk.extent is already adjusted for base mip map level we use it
       * here.
       */
      /* TODO: Investigate and document the reason for above approach. */
      info.extent = iview->vk.extent;

      info.mip_levels = 1;
      info.mipmaps_present = false;
      info.stride = u_minify(image->physical_extent.width, info.base_level);
      info.base_level = 0;
      info.tex_state_type = PVR_TEXTURE_STATE_ATTACHMENT;

      if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
          iview->vk.view_type == VK_IMAGE_VIEW_TYPE_2D) {
         info.type = VK_IMAGE_VIEW_TYPE_3D;
      } else {
         info.type = iview->vk.view_type;
      }

      result = pvr_pack_tex_state(device,
                                  &info,
                                  iview->texture_state[info.tex_state_type]);
      if (result != VK_SUCCESS)
         goto err_vk_image_view_destroy;
   }

   *pView = pvr_image_view_to_handle(iview);

   return VK_SUCCESS;

err_vk_image_view_destroy:
   vk_image_view_destroy(&device->vk, pAllocator, &iview->vk);

   return result;
}

void pvr_DestroyImageView(VkDevice _device,
                          VkImageView _iview,
                          const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   PVR_FROM_HANDLE(pvr_image_view, iview, _iview);

   if (!iview)
      return;

   vk_image_view_destroy(&device->vk, pAllocator, &iview->vk);
}

VkResult pvr_CreateBufferView(VkDevice _device,
                              const VkBufferViewCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkBufferView *pView)
{
   PVR_FROM_HANDLE(pvr_buffer, buffer, pCreateInfo->buffer);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   struct pvr_texture_state_info info;
   const uint8_t *format_swizzle;
   struct pvr_buffer_view *bview;
   VkResult result;

   bview = vk_buffer_view_create(&device->vk,
                                 pCreateInfo,
                                 pAllocator,
                                 sizeof(*bview));
   if (!bview)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* If the remaining size of the buffer is not a multiple of the element
    * size of the format, the nearest smaller multiple is used.
    */
   bview->vk.range -=
      bview->vk.range % vk_format_get_blocksize(bview->vk.format);

   /* The range of the buffer view shouldn't be smaller than one texel. */
   assert(bview->vk.range >= vk_format_get_blocksize(bview->vk.format));

   bview->num_rows = DIV_ROUND_UP(bview->vk.elements, PVR_BUFFER_VIEW_WIDTH);

   info.base_level = 0U;
   info.mip_levels = 1U;
   info.mipmaps_present = false;
   info.extent.width = PVR_BUFFER_VIEW_WIDTH;
   info.extent.height = bview->num_rows;
   info.extent.depth = 0U;
   info.sample_count = 1U;
   info.stride = PVR_BUFFER_VIEW_WIDTH;
   info.offset = 0U;
   info.addr = PVR_DEV_ADDR_OFFSET(buffer->dev_addr, pCreateInfo->offset);
   info.mem_layout = PVR_MEMLAYOUT_LINEAR;
   info.is_cube = false;
   info.type = VK_IMAGE_VIEW_TYPE_2D;
   info.tex_state_type = PVR_TEXTURE_STATE_SAMPLE;
   info.format = bview->vk.format;
   info.flags = PVR_TEXFLAGS_INDEX_LOOKUP;
   info.aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;

   if (PVR_HAS_FEATURE(&device->pdevice->dev_info, tpu_array_textures))
      info.array_size = 1U;

   format_swizzle = pvr_get_format_swizzle(info.format);
   memcpy(info.swizzle, format_swizzle, sizeof(info.swizzle));

   result = pvr_pack_tex_state(device, &info, bview->texture_state);
   if (result != VK_SUCCESS)
      goto err_vk_buffer_view_destroy;

   *pView = pvr_buffer_view_to_handle(bview);

   return VK_SUCCESS;

err_vk_buffer_view_destroy:
   vk_object_free(&device->vk, pAllocator, bview);

   return result;
}

void pvr_DestroyBufferView(VkDevice _device,
                           VkBufferView bufferView,
                           const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_buffer_view, bview, bufferView);
   PVR_FROM_HANDLE(pvr_device, device, _device);

   if (!bview)
      return;

   vk_buffer_view_destroy(&device->vk, pAllocator, &bview->vk);
}

VkResult
pvr_GetImageDrmFormatModifierPropertiesEXT(
   VkDevice device, VkImage _image,
   VkImageDrmFormatModifierPropertiesEXT *pProperties)
{
//   VK_FROM_HANDLE(pvr_image, image, _image); TODO

   assert(pProperties->sType ==
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT);

   pProperties->drmFormatModifier = DRM_FORMAT_MOD_LINEAR; // TODO image->pimage.layout.modifier; ??
   return VK_SUCCESS;
}
