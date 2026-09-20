/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_cmd_buffer.h"

#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "nvk_image_view.h"
#include "nvk_mme.h"
#include "nvk_physical_device.h"

#include "nvk_queue.h"
#include "nvkmd/nvkmd.h"

#include "util/format/u_format.h"
#include "vk_format.h"

#include "nvif/cl9097.h"
#include "nv_push_cl9097.h"
#include "drf.h"

/* Zero-bandwidth clear: the table is GPU-global and entries are never freed,
 * so a value is only added once it has been cleared with twice in a row.
 */

static bool
nvk_zbc_color_format(enum pipe_format format, uint8_t *fmt_out)
{
   switch (format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A8B8G8R8;
      return true;
   case PIPE_FORMAT_R8G8B8A8_SRGB:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A8BL8GL8RL8;
      return true;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A8R8G8B8;
      return true;
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A8RL8GL8BL8;
      return true;
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A2B10G10R10;
      return true;
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_A2R10G10B10;
      return true;
   case PIPE_FORMAT_R11G11B10_FLOAT:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_BF10GF11RF11;
      return true;
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_RF16_GF16_BF16_AF16;
      return true;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      *fmt_out = FERMI_A_ZBC_COLOR_V0_FMT_RF32_GF32_BF32_AF32;
      return true;
   default:
      return false;
   }
}

static struct nvkmd_ctx *
nvk_device_3d_ctx(struct nvk_device *dev)
{
   vk_foreach_queue(vk_queue, &dev->vk) {
      struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);

      if (queue->engines & NVKMD_ENGINE_3D)
         return queue->exec_ctx;
   }

   return NULL;
}

static void
nvk_zbc_add_color(struct nvk_device *dev, VkFormat vk_format,
                  const VkClearColorValue *color)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvk_zbc_table *zbc = &dev->zbc;
   struct nvk_zbc_color entry = { };

   if (!pdev->nvkmd->kmd_info.has_compression ||
       !nvk_compression_needs_big_pages(pdev) ||
       (pdev->debug_flags & NVK_DEBUG_NO_ZBC))
      return;

   const enum pipe_format p_format = nvk_format_to_pipe_format(vk_format);
   if (p_format == PIPE_FORMAT_NONE ||
       !nvk_zbc_color_format(p_format, &entry.format))
      return;

   /* the kernel refuses these, which would read as a full table */
   for (unsigned i = 0; i < 4; i++) {
      if (!isfinite(color->float32[i]))
         return;
   }

   memcpy(entry.ds, color->uint32, sizeof(entry.ds));

   simple_mtx_lock(&zbc->mutex);

   if (zbc->colors_full)
      goto unlock;

   for (uint32_t i = 0; i < zbc->color_count; i++) {
      if (!memcmp(&zbc->colors[i], &entry, sizeof(entry)))
         goto unlock;
   }

   if (memcmp(&zbc->color_seen, &entry, sizeof(entry))) {
      zbc->color_seen = entry;
      goto unlock;
   }

   struct nvkmd_ctx *ctx = nvk_device_3d_ctx(dev);
   if (ctx == NULL || !nvkmd_ctx_set_zbc_color(ctx, entry.format, entry.ds)) {
      zbc->colors_full = true;
      goto unlock;
   }

   assert(zbc->color_count < NVK_ZBC_MAX_ENTRIES);
   zbc->colors[zbc->color_count++] = entry;
   zbc->colors_full = zbc->color_count == NVK_ZBC_MAX_ENTRIES;

unlock:
   simple_mtx_unlock(&zbc->mutex);
}

static void
nvk_zbc_add_depth(struct nvk_device *dev, VkFormat vk_format, float depth)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvk_zbc_table *zbc = &dev->zbc;
   const uint32_t entry = fui(depth);

   if (vk_format != VK_FORMAT_D32_SFLOAT || !isfinite(depth))
      return;

   if (!pdev->nvkmd->kmd_info.has_compression ||
       !nvk_compression_needs_big_pages(pdev) ||
       (pdev->debug_flags & NVK_DEBUG_NO_ZBC))
      return;

   simple_mtx_lock(&zbc->mutex);

   if (zbc->depths_full)
      goto unlock;

   for (uint32_t i = 0; i < zbc->depth_count; i++) {
      if (zbc->depths[i] == entry)
         goto unlock;
   }

   if (!zbc->depth_seen_valid || zbc->depth_seen != entry) {
      zbc->depth_seen = entry;
      zbc->depth_seen_valid = true;
      goto unlock;
   }

   struct nvkmd_ctx *ctx = nvk_device_3d_ctx(dev);
   if (ctx == NULL || !nvkmd_ctx_set_zbc_depth(ctx, entry)) {
      zbc->depths_full = true;
      goto unlock;
   }

   assert(zbc->depth_count < NVK_ZBC_MAX_ENTRIES);
   zbc->depths[zbc->depth_count++] = entry;
   zbc->depths_full = zbc->depth_count == NVK_ZBC_MAX_ENTRIES;

unlock:
   simple_mtx_unlock(&zbc->mutex);
}

void
nvk_mme_clear(struct mme_builder *b)
{
   struct mme_value payload = mme_load(b);
   struct mme_value view_mask = nvk_mme_load_scratch(b, VIEW_MASK);

   const uint32_t arr_idx = 1 << DRF_LO(NV9097_CLEAR_SURFACE_RT_ARRAY_INDEX);

   mme_if(b, ieq, view_mask, mme_zero()) {
      struct mme_value layer_count = mme_load(b);

      mme_loop(b, layer_count) {
         mme_mthd(b, NV9097_CLEAR_SURFACE);
         mme_emit(b, payload);

         mme_add_to(b, payload, payload, mme_imm(arr_idx));
      }
      mme_free_reg(b, layer_count);
   }

   mme_if(b, ine, view_mask, mme_zero()) {
      struct mme_value bit = mme_mov(b, mme_imm(1));

      mme_loop(b, mme_imm(32)) {
         mme_if(b, ine, mme_and(b, view_mask, bit), mme_zero()) {
            mme_mthd(b, NV9097_CLEAR_SURFACE);
            mme_emit(b, payload);
         }

         mme_add_to(b, payload, payload, mme_imm(arr_idx));
         mme_sll_to(b, bit, bit, mme_imm(1));
      }
      mme_free_reg(b, bit);
   }

   mme_free_reg(b, payload);
   mme_free_reg(b, view_mask);
}

const struct nvk_mme_test_case nvk_mme_clear_tests[] = {{
   .init = (struct nvk_mme_mthd_data[]) {
      { NVK_SET_MME_SCRATCH(VIEW_MASK), 0 },
      { }
   },
   .params = (uint32_t[]) { 0x3c, 5 },
   .expected = (struct nvk_mme_mthd_data[]) {
      { NV9097_CLEAR_SURFACE, 0x003c },
      { NV9097_CLEAR_SURFACE, 0x043c },
      { NV9097_CLEAR_SURFACE, 0x083c },
      { NV9097_CLEAR_SURFACE, 0x0c3c },
      { NV9097_CLEAR_SURFACE, 0x103c },
      { }
   },
}, {
   .init = (struct nvk_mme_mthd_data[]) {
      { NVK_SET_MME_SCRATCH(VIEW_MASK), 0xb },
      { }
   },
   .params = (uint32_t[]) { 0x3c },
   .expected = (struct nvk_mme_mthd_data[]) {
      { NV9097_CLEAR_SURFACE, 0x03c },
      { NV9097_CLEAR_SURFACE, 0x43c },
      { NV9097_CLEAR_SURFACE, 0xc3c },
      { }
   },
}, {}};

void
nvk_mme_update_window_clip(struct mme_builder *b)
{
   struct mme_value is_drawing = mme_load(b);
   struct mme_value window_clip_enabled = nvk_mme_load_scratch(b, WINDOW_CLIP_ENABLED);

   struct mme_value new_value = mme_and(b, window_clip_enabled, is_drawing);
   mme_mthd(b, NV9097_SET_WINDOW_CLIP_ENABLE);
   mme_emit(b, new_value);

   mme_free_reg(b, new_value);
   mme_free_reg(b, window_clip_enabled);
   mme_free_reg(b, is_drawing);
}

static void
emit_clear_rects(struct nvk_cmd_buffer *cmd,
                 int color_att,
                 bool clear_depth,
                 bool clear_stencil,
                 uint32_t rect_count,
                 const VkClearRect *rects)
{
   struct nvk_rendering_state *render = &cmd->state.gfx.render;

   struct nv_push *p = nvk_cmd_buffer_push(cmd, rect_count * 6);

   for (uint32_t r = 0; r < rect_count; r++) {
      P_MTHD(p, NV9097, SET_CLEAR_RECT_HORIZONTAL);
      P_NV9097_SET_CLEAR_RECT_HORIZONTAL(p, {
         .xmin = rects[r].rect.offset.x,
         .xmax = rects[r].rect.offset.x + rects[r].rect.extent.width,
      });
      P_NV9097_SET_CLEAR_RECT_VERTICAL(p, {
         .ymin = rects[r].rect.offset.y,
         .ymax = rects[r].rect.offset.y + rects[r].rect.extent.height,
      });

      uint32_t payload;
      V_NV9097_CLEAR_SURFACE(payload, {
         .z_enable       = clear_depth,
         .stencil_enable = clear_stencil,
         .r_enable       = color_att >= 0,
         .g_enable       = color_att >= 0,
         .b_enable       = color_att >= 0,
         .a_enable       = color_att >= 0,
         .mrt_select     = color_att >= 0 ? color_att : 0,
         .rt_array_index = rects[r].baseArrayLayer,
      });

      P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_CLEAR));
      P_INLINE_DATA(p, payload);
      if (render->view_mask == 0) {
         P_INLINE_DATA(p, rects[r].layerCount);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearAttachments(VkCommandBuffer commandBuffer,
                        uint32_t attachmentCount,
                        const VkClearAttachment *pAttachments,
                        uint32_t rectCount,
                        const VkClearRect *pRects)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_rendering_state *render = &cmd->state.gfx.render;

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 4 + attachmentCount * 4);

   P_IMMD(p, NV9097, SET_CLEAR_SURFACE_CONTROL, {
      .respect_stencil_mask   = RESPECT_STENCIL_MASK_FALSE,
      .use_clear_rect         = USE_CLEAR_RECT_TRUE,
      .use_scissor0           = USE_SCISSOR0_FALSE,
      .use_viewport_clip0     = USE_VIEWPORT_CLIP0_FALSE,
   });

   /* VK_EXT_discard_rects doesn't influence clears */
   const bool stash_window_clip =
      BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DR_ENABLE) || dyn->dr.enable;
   if (stash_window_clip) {
      /* Disable WINDOW_CLIP while we clear */
      P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_UPDATE_WINDOW_CLIP));
      P_INLINE_DATA(p, 0);
   }

   bool clear_depth = false, clear_stencil = false;
   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         P_IMMD(p, NV9097, SET_Z_CLEAR_VALUE,
                fui(pAttachments[i].clearValue.depthStencil.depth));
         nvk_zbc_add_depth(dev, render->depth_att.vk_format,
                           pAttachments[i].clearValue.depthStencil.depth);
         clear_depth = true;
      }

      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         P_IMMD(p, NV9097, SET_STENCIL_CLEAR_VALUE,
                pAttachments[i].clearValue.depthStencil.stencil & 0xff);
         clear_stencil = true;
      }
   }

   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pAttachments[i].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
         continue;

      if (pAttachments[i].colorAttachment == VK_ATTACHMENT_UNUSED)
         continue;

      VkClearColorValue color = pAttachments[i].clearValue.color;

      if (pAttachments[i].colorAttachment < render->color_att_count) {
         nvk_zbc_add_color(dev,
            render->color_att[pAttachments[i].colorAttachment].vk_format,
            &color);
      }

      p = nvk_cmd_buffer_push(cmd, 5);

      P_MTHD(p, NV9097, SET_COLOR_CLEAR_VALUE(0));
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 0, color.uint32[0]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 1, color.uint32[1]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 2, color.uint32[2]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 3, color.uint32[3]);

      emit_clear_rects(cmd, pAttachments[i].colorAttachment,
                       clear_depth, clear_stencil, rectCount, pRects);

      /* We only need to clear depth/stencil once */
      clear_depth = clear_stencil = false;
   }

   /* No color clears */
   if (clear_depth || clear_stencil)
      emit_clear_rects(cmd, -1, clear_depth, clear_stencil, rectCount, pRects);

   if (stash_window_clip) {
      p = nvk_cmd_buffer_push(cmd, 2);
      /* Restore stashed WINDOW_CLIP value */
      P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_UPDATE_WINDOW_CLIP));
      P_INLINE_DATA(p, 1);
   }
}

static VkImageViewType
render_view_type(VkImageType image_type, unsigned layer_count)
{
   switch (image_type) {
   case VK_IMAGE_TYPE_1D:
      return layer_count == 1 ? VK_IMAGE_VIEW_TYPE_1D :
                                VK_IMAGE_VIEW_TYPE_1D_ARRAY;
   case VK_IMAGE_TYPE_2D:
      return layer_count == 1 ? VK_IMAGE_VIEW_TYPE_2D :
                                VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
   default:
      UNREACHABLE("Invalid image type");
   }
}

static void
clear_image(struct nvk_cmd_buffer *cmd,
            struct nvk_image *image,
            VkImageLayout image_layout,
            VkFormat format,
            const VkClearValue *clear_value,
            uint32_t range_count,
            const VkImageSubresourceRange *ranges)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   ASSERTED VkResult result;

   for (uint32_t r = 0; r < range_count; r++) {
      const uint32_t level_count =
         vk_image_subresource_level_count(&image->vk, &ranges[r]);

      for (uint32_t l = 0; l < level_count; l++) {
         const uint32_t level = ranges[r].baseMipLevel + l;

         const VkExtent3D level_extent =
            vk_image_mip_level_extent(&image->vk, level);

         uint32_t base_array_layer, layer_count;
         if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
            base_array_layer = 0;
            layer_count = level_extent.depth;
         } else {
            base_array_layer = ranges[r].baseArrayLayer;
            layer_count = vk_image_subresource_layer_count(&image->vk,
                                                           &ranges[r]);
         }

         const VkImageViewUsageCreateInfo view_usage_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = (ranges[r].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) ?
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT :
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         };
         const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &view_usage_info,
            .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
            .image = nvk_image_to_handle(image),
            .viewType = render_view_type(image->vk.image_type, layer_count),
            .format = format,
            .subresourceRange = {
               .aspectMask = image->vk.aspects,
               .baseMipLevel = level,
               .levelCount = 1,
               .baseArrayLayer = base_array_layer,
               .layerCount = layer_count,
            },
         };

         struct nvk_image_view view;
         result = nvk_image_view_init(dev, &view, &view_info);
         assert(result == VK_SUCCESS);

         VkRenderingInfo render = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {
               .offset = { 0, 0 },
               .extent = { level_extent.width, level_extent.height },
            },
            .layerCount = layer_count,
         };

         VkRenderingAttachmentInfo vk_att = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = nvk_image_view_to_handle(&view),
            .imageLayout = image_layout,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = *clear_value,
         };

         if (ranges[r].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            render.colorAttachmentCount = 1;
            render.pColorAttachments = &vk_att;
         }
         if (ranges[r].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
            render.pDepthAttachment = &vk_att;
         if (ranges[r].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
            render.pStencilAttachment = &vk_att;

         nvk_CmdBeginRendering(nvk_cmd_buffer_to_handle(cmd), &render);

         const VkRenderingEndInfoKHR end = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR,
         };
         nvk_CmdEndRendering2KHR(nvk_cmd_buffer_to_handle(cmd), &end);

         nvk_image_view_finish(dev, &view);
      }
   }
}

static VkFormat
vk_packed_int_format_for_size(unsigned size_B)
{
   switch (size_B) {
   case 1:  return VK_FORMAT_R8_UINT;
   case 2:  return VK_FORMAT_R16_UINT;
   case 4:  return VK_FORMAT_R32_UINT;
   case 8:  return VK_FORMAT_R32G32_UINT;
   case 16: return VK_FORMAT_R32G32B32A32_UINT;
   default: UNREACHABLE("Invalid image format size");
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearColorImage(VkCommandBuffer commandBuffer,
                       VkImage _image,
                       VkImageLayout imageLayout,
                       const VkClearColorValue *pColor,
                       uint32_t rangeCount,
                       const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VK_FROM_HANDLE(nvk_image, image, _image);

   VkClearValue clear_value = {
      .color = *pColor,
   };

   VkFormat vk_format = image->vk.format;
   if (vk_format == VK_FORMAT_R64_UINT || vk_format == VK_FORMAT_R64_SINT)
      vk_format = VK_FORMAT_R32G32_UINT;

   enum pipe_format p_format = nvk_format_to_pipe_format(vk_format);
   assert(p_format != PIPE_FORMAT_NONE);

   if (!nil_format_supports_color_targets(&pdev->info, p_format)) {
      memset(&clear_value, 0, sizeof(clear_value));
      util_format_pack_rgba(p_format, clear_value.color.uint32,
                            pColor->uint32, 1);

      unsigned bpp = util_format_get_blocksize(p_format);
      vk_format = vk_packed_int_format_for_size(bpp);
   }

   clear_image(cmd, image, imageLayout, vk_format,
               &clear_value, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                              VkImage _image,
                              VkImageLayout imageLayout,
                              const VkClearDepthStencilValue *pDepthStencil,
                              uint32_t rangeCount,
                              const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, image, _image);

   const VkClearValue clear_value = {
      .depthStencil = *pDepthStencil,
   };

   clear_image(cmd, image, imageLayout, image->vk.format,
               &clear_value, rangeCount, pRanges);
}
