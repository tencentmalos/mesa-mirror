/*
 * Copyright © 2019 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "tu_cs.h"

#include "tu_device.h"
#include "tu_rmv.h"
#include "tu_suballoc.h"

#include "util/simple_mtx.h"

uint32_t tu_cs_fail_sink[TU_CS_FAIL_SINK_SIZE];

/* TU_DEBUG=csbocache: BOs released by tu_cs_reset/tu_cs_finish are kept and
 * handed to the next tu_cs_add_bo of the same size and kind, instead of being
 * freed and allocated again. A command stream then keeps reusing the same
 * GPU addresses and pages, and no IOMMU unmap (with its TLB invalidation) is
 * issued for command memory while the GPU runs.
 */
#define TU_CS_BO_CACHE_SLOTS 256
#define TU_CS_BO_CACHE_BYTES (96ull << 20)

struct tu_cs_bo_cache_slot {
   struct tu_device *device;
   struct tu_bo *bo;
   uint64_t seq;
   bool read_only;
};

static simple_mtx_t tu_cs_bo_cache_mtx = SIMPLE_MTX_INITIALIZER;
static struct tu_cs_bo_cache_slot tu_cs_bo_cache[TU_CS_BO_CACHE_SLOTS];
static uint64_t tu_cs_bo_cache_bytes;
static uint64_t tu_cs_bo_cache_seq;

static struct tu_bo *
tu_cs_bo_cache_get(struct tu_device *device, uint64_t bytes, bool read_only)
{
   struct tu_bo *bo = NULL;
   int best = -1;

   simple_mtx_lock(&tu_cs_bo_cache_mtx);
   for (int i = 0; i < TU_CS_BO_CACHE_SLOTS; i++) {
      const struct tu_cs_bo_cache_slot *slot = &tu_cs_bo_cache[i];
      if (!slot->bo || slot->device != device || slot->read_only != read_only ||
          slot->bo->size < bytes || slot->bo->size >= bytes + 65536)
         continue;
      /* Oldest first: the longest time since the GPU last used it. */
      if (best < 0 || slot->seq < tu_cs_bo_cache[best].seq)
         best = i;
   }
   if (best >= 0) {
      bo = tu_cs_bo_cache[best].bo;
      tu_cs_bo_cache[best].bo = NULL;
      tu_cs_bo_cache_bytes -= bo->size;
   }
   simple_mtx_unlock(&tu_cs_bo_cache_mtx);
   return bo;
}

static void
tu_cs_release_bo(struct tu_cs *cs, struct tu_bo *bo, bool read_only)
{
   if (TU_DEBUG(CS_BO_CACHE) && bo->map && p_atomic_read(&bo->refcnt) == 1) {
      simple_mtx_lock(&tu_cs_bo_cache_mtx);
      if (tu_cs_bo_cache_bytes + bo->size <= TU_CS_BO_CACHE_BYTES) {
         for (int i = 0; i < TU_CS_BO_CACHE_SLOTS; i++) {
            struct tu_cs_bo_cache_slot *slot = &tu_cs_bo_cache[i];
            if (slot->bo)
               continue;
            slot->device = cs->device;
            slot->bo = bo;
            slot->seq = tu_cs_bo_cache_seq++;
            slot->read_only = read_only;
            tu_cs_bo_cache_bytes += bo->size;
            simple_mtx_unlock(&tu_cs_bo_cache_mtx);
            return;
         }
      }
      simple_mtx_unlock(&tu_cs_bo_cache_mtx);
   }
   tu_bo_finish(cs->device, bo);
}

void
tu_cs_bo_cache_release_device(struct tu_device *device)
{
   struct tu_bo *bos[TU_CS_BO_CACHE_SLOTS];
   unsigned count = 0;

   simple_mtx_lock(&tu_cs_bo_cache_mtx);
   for (int i = 0; i < TU_CS_BO_CACHE_SLOTS; i++) {
      struct tu_cs_bo_cache_slot *slot = &tu_cs_bo_cache[i];
      if (slot->bo && slot->device == device) {
         bos[count++] = slot->bo;
         tu_cs_bo_cache_bytes -= slot->bo->size;
         slot->bo = NULL;
      }
   }
   simple_mtx_unlock(&tu_cs_bo_cache_mtx);

   for (unsigned i = 0; i < count; i++)
      tu_bo_finish(device, bos[i]);
}

/* There is a limit to IB size supported by HW,
 * which appears to be 0x0fffff.
 */
inline uint32_t
tu_sanitize_ib_size(uint32_t size)
{
   return MIN2(size, 0x0fffff);
}

/* TU_DEBUG=cs_skip_head: command streams of 64 KiB and larger get 8 KiB of
 * extra space at the start of their BO that is never written. Every
 * recording then starts on the BO's third page instead of its first, which
 * moves where draw state first lands in a fresh page.
 */
#define TU_CS_HEAD_SKIP_DWORDS 2048
#define TU_CS_HEAD_SKIP_MIN_DWORDS 16384

static inline uint32_t
tu_cs_head_skip(uint32_t size)
{
   return TU_DEBUG(CS_SKIP_HEAD) && size >= TU_CS_HEAD_SKIP_MIN_DWORDS
             ? TU_CS_HEAD_SKIP_DWORDS : 0;
}

static inline uint32_t
tu_cs_bo_head_skip(const struct tu_bo *bo)
{
   return TU_DEBUG(CS_SKIP_HEAD) &&
                bo->size / sizeof(uint32_t) >=
                   TU_CS_HEAD_SKIP_MIN_DWORDS + TU_CS_HEAD_SKIP_DWORDS
             ? TU_CS_HEAD_SKIP_DWORDS : 0;
}

/**
 * Initialize a command stream.
 */
void
tu_cs_init(struct tu_cs *cs,
           struct tu_device *device,
           enum tu_cs_mode mode,
           uint32_t initial_size, const char *name)
{
   assert(mode != TU_CS_MODE_EXTERNAL);

   memset(cs, 0, sizeof(*cs));

   cs->device = device;
   cs->mode = mode;
   cs->next_bo_size = initial_size;
   cs->name = name;
}

/**
 * Initialize a command stream as a wrapper to an external buffer.
 */
void
tu_cs_init_external(struct tu_cs *cs, struct tu_device *device,
                    uint32_t *start, uint32_t *end, uint64_t iova,
                    bool writeable)
{
   memset(cs, 0, sizeof(*cs));

   cs->device = device;
   cs->mode = TU_CS_MODE_EXTERNAL;
   cs->start = cs->reserved_end = cs->cur = start;
   cs->end = end;
   cs->external_iova = iova;
   cs->writeable = writeable;
}

/**
 * Initialize a sub-command stream as a wrapper to an externally sub-allocated
 * buffer.
 */
void
tu_cs_init_suballoc(struct tu_cs *cs, struct tu_device *device,
                    struct tu_suballoc_bo *suballoc_bo)
{
   uint32_t *start = (uint32_t *) tu_suballoc_bo_map(suballoc_bo);
   uint32_t *end = start + (suballoc_bo->size >> 2);

   memset(cs, 0, sizeof(*cs));
   cs->device = device;
   cs->mode = TU_CS_MODE_SUB_STREAM;
   cs->start = cs->reserved_end = cs->cur = start;
   cs->end = end;
   cs->refcount_bo = tu_bo_get_ref(suballoc_bo->bo);
}

/**
 * Finish and release all resources owned by a command stream.
 */
void
tu_cs_finish(struct tu_cs *cs)
{
   for (uint32_t i = 0; i < cs->read_only.bo_count; ++i) {
      TU_RMV(resource_destroy, cs->device, cs->read_only.bos[i]);
      tu_cs_release_bo(cs, cs->read_only.bos[i], true);
   }

   for (uint32_t i = 0; i < cs->read_write.bo_count; ++i) {
      TU_RMV(resource_destroy, cs->device, cs->read_write.bos[i]);
      tu_cs_release_bo(cs, cs->read_write.bos[i], false);
   }

   if (cs->refcount_bo)
      tu_bo_finish(cs->device, cs->refcount_bo);

   free(cs->entries);
   free(cs->read_only.bos);
   free(cs->read_write.bos);
}

static struct tu_bo *
tu_cs_current_bo(const struct tu_cs *cs)
{
   if (cs->refcount_bo) {
      return cs->refcount_bo;
   } else {
      const struct tu_bo_array *bos = cs->writeable ? &cs->read_write : &cs->read_only;
      assert(bos->bo_count);
      return bos->bos[bos->bo_count - 1];
   }
}

/**
 * Get the offset of the command packets emitted since the last call to
 * tu_cs_add_entry.
 */
static uint32_t
tu_cs_get_offset(const struct tu_cs *cs)
{
   const struct tu_bo_array *bos = cs->writeable ? &cs->read_write : &cs->read_only;
   return (cs->refcount_bo || bos->bo_count != 0) ? cs->start - (uint32_t *) tu_cs_current_bo(cs)->map : 0;
}

/**
 * Get the iova for the next dword to be emitted. Useful after
 * tu_cs_reserve_space() to create a patch point that can be overwritten on
 * the GPU.
 */
uint64_t
tu_cs_get_cur_iova(const struct tu_cs *cs)
{
   if (unlikely(cs->status != VK_SUCCESS))
      return 0;

   if (cs->mode == TU_CS_MODE_EXTERNAL)
      return cs->external_iova + ((char *) cs->cur - (char *) cs->start);
   return tu_cs_current_bo(cs)->iova + ((char *) cs->cur - (char *) tu_cs_current_bo(cs)->map);
}

/*
 * Allocate and add a BO to a command stream.  Following command packets will
 * be emitted to the new BO.
 */
static VkResult
tu_cs_add_bo(struct tu_cs *cs, uint32_t size)
{
   /* no BO for TU_CS_MODE_EXTERNAL */
   assert(cs->mode != TU_CS_MODE_EXTERNAL);
   /* No adding more BOs if suballocating from a suballoc_bo. */
   assert(!cs->refcount_bo);

   /* no dangling command packet */
   assert(tu_cs_is_empty(cs));

   struct tu_bo_array *bos = cs->writeable ? &cs->read_write : &cs->read_only;

   /* grow cs->bos if needed */
   if (bos->bo_count == bos->bo_capacity) {
      uint32_t new_capacity = MAX2(4, 2 * bos->bo_capacity);
      struct tu_bo **new_bos = (struct tu_bo **)
         realloc(bos->bos, new_capacity * sizeof(struct tu_bo *));
      if (!new_bos)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      bos->bo_capacity = new_capacity;
      bos->bos = new_bos;
   }

   const uint32_t skip = tu_cs_head_skip(size);
   /* TU_DEBUG=csbocache hands out whole cached BOs; cs_skip_head is not
    * applied to them.
    */
   struct tu_bo *new_bo = TU_DEBUG(CS_BO_CACHE) ?
      tu_cs_bo_cache_get(cs->device, size * sizeof(uint32_t), !cs->writeable) : NULL;
   if (new_bo) {
      TU_RMV(cmd_buffer_bo_create, cs->device, new_bo);
      bos->bos[bos->bo_count++] = new_bo;
      cs->start = cs->cur = cs->reserved_end = (uint32_t *) new_bo->map;
      cs->end = cs->start + size;
      return VK_SUCCESS;
   }

   /* TU_DEBUG=csalign64k: give command stream BOs a 64 KiB aligned GPU VA,
    * so that no last-level page table cache line (8 PTEs, 32 KiB of VA) is
    * shared with another BO. On A8XX the CP has been seen to fetch zeros for
    * the second page of a command BO whose first PTEs share a line with a
    * BO that was freed earlier.
    */
   VkResult result =
      tu_bo_init_new_explicit_iova(
         cs->device, NULL, &new_bo, (size + skip) * sizeof(uint32_t),
         TU_DEBUG(CS_ALIGN_64K) ? 64 * 1024 : 0, 0,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         (enum tu_bo_alloc_flags)(COND(!cs->writeable,
                                       TU_BO_ALLOC_GPU_READ_ONLY) |
                                  TU_BO_ALLOC_ALLOW_DUMP |
                                  tu_bo_ib_flags(cs->device)),
         NULL, cs->name);
   if (result != VK_SUCCESS) {
      return result;
   }

   result = tu_bo_map(cs->device, new_bo, NULL);
   if (result != VK_SUCCESS) {
      tu_bo_finish(cs->device, new_bo);
      return result;
   }

   TU_RMV(cmd_buffer_bo_create, cs->device, new_bo);

   bos->bos[bos->bo_count++] = new_bo;

   cs->start = cs->cur = cs->reserved_end = (uint32_t *) new_bo->map + skip;
   cs->end = cs->start + size;

   return VK_SUCCESS;
}

/**
 * Reserve an IB entry.
 */
static VkResult
tu_cs_reserve_entry(struct tu_cs *cs)
{
   /* entries are only for TU_CS_MODE_GROW */
   assert(cs->mode == TU_CS_MODE_GROW);

   /* grow cs->entries if needed */
   if (cs->entry_count == cs->entry_capacity) {
      uint32_t new_capacity = MAX2(4, cs->entry_capacity * 2);
      struct tu_cs_entry *new_entries = (struct tu_cs_entry *)
         realloc(cs->entries, new_capacity * sizeof(struct tu_cs_entry));
      if (!new_entries)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      cs->entry_capacity = new_capacity;
      cs->entries = new_entries;
   }

   return VK_SUCCESS;
}


/**
 * Add an IB entry for the command packets emitted since the last call to this
 * function.
 */
static void
tu_cs_add_entry(struct tu_cs *cs)
{
   /* entries are only for TU_CS_MODE_GROW */
   assert(cs->mode == TU_CS_MODE_GROW);

   /* disallow empty entry */
   assert(!tu_cs_is_empty(cs));

   /*
    * because we disallow empty entry, tu_cs_add_bo and tu_cs_reserve_entry
    * must both have been called
    */
   assert(cs->writeable ? cs->read_write.bo_count : cs->read_only.bo_count);
   assert(cs->entry_count < cs->entry_capacity);

   /* add an entry for [cs->start, cs->cur] */
   cs->entries[cs->entry_count++] = (struct tu_cs_entry) {
      .bo = tu_cs_current_bo(cs),
      .size = tu_cs_get_size(cs) * sizeof(uint32_t),
      .offset = tu_cs_get_offset(cs) * sizeof(uint32_t),
   };

   cs->start = cs->cur;
}

/**
 * same behavior as tu_cs_emit_call but without the indirect
 */
VkResult
tu_cs_add_entries(struct tu_cs *cs, struct tu_cs *target)
{
   VkResult result;

   assert(cs->mode == TU_CS_MODE_GROW);
   assert(target->mode == TU_CS_MODE_GROW);

   if (!tu_cs_is_empty(cs))
      tu_cs_add_entry(cs);

   for (unsigned i = 0; i < target->entry_count; i++) {
      result = tu_cs_reserve_entry(cs);
      if (result != VK_SUCCESS)
         return result;
      cs->entries[cs->entry_count++] = target->entries[i];
   }

   return VK_SUCCESS;
}

/**
 * Begin (or continue) command packet emission.  This does nothing but sanity
 * checks currently.  \a cs must not be in TU_CS_MODE_SUB_STREAM mode.
 */
void
tu_cs_begin(struct tu_cs *cs)
{
   assert(cs->mode != TU_CS_MODE_SUB_STREAM);
   assert(tu_cs_is_empty(cs) || cs->status != VK_SUCCESS);
}

/**
 * End command packet emission.  This adds an IB entry when \a cs is in
 * TU_CS_MODE_GROW mode.
 */
void
tu_cs_end(struct tu_cs *cs)
{
   assert(cs->mode != TU_CS_MODE_SUB_STREAM);

   if (unlikely(cs->status != VK_SUCCESS))
      return;

   if (cs->mode == TU_CS_MODE_GROW && !tu_cs_is_empty(cs))
      tu_cs_add_entry(cs);
}

void
tu_cs_set_writeable(struct tu_cs *cs, bool writeable)
{
   assert(cs->mode == TU_CS_MODE_GROW || cs->mode == TU_CS_MODE_SUB_STREAM);

   if (unlikely(cs->status != VK_SUCCESS))
      return;

   if (cs->writeable != writeable) {
      assert(!cs->cond_stack_depth);
      if (cs->mode == TU_CS_MODE_GROW && !tu_cs_is_empty(cs))
         tu_cs_add_entry(cs);
      struct tu_bo_array *old_bos = cs->writeable ? &cs->read_write : &cs->read_only;
      struct tu_bo_array *new_bos = writeable ? &cs->read_write : &cs->read_only;

      old_bos->start = cs->start;
      cs->start = cs->cur = cs->reserved_end = new_bos->start;
      if (new_bos->bo_count) {
         struct tu_bo *bo = new_bos->bos[new_bos->bo_count - 1];
         cs->end = (uint32_t *) bo->map +
                   tu_sanitize_ib_size(bo->size / sizeof(uint32_t));
      } else {
         cs->end = NULL;
      }

      cs->writeable = writeable;
   }
}

/**
 * Begin command packet emission to a sub-stream.  \a cs must be in
 * TU_CS_MODE_SUB_STREAM mode.
 *
 * Return \a sub_cs which is in TU_CS_MODE_EXTERNAL mode.  tu_cs_begin and
 * tu_cs_reserve_space are implied and \a sub_cs is ready for command packet
 * emission.
 */
VkResult
tu_cs_begin_sub_stream_aligned(struct tu_cs *cs, uint32_t count,
                               uint32_t size, struct tu_cs *sub_cs)
{
   assert(cs->mode == TU_CS_MODE_SUB_STREAM);
   assert(size);

   /* TU_DEBUG=ib2pagesplit: keep a sub stream that fits in a page (draw
    * states read by the DDE) within one page, see tu_cs_page_split().
    */
   if (unlikely(cs->page_split) && cs->cur && count * size < 1024) {
      const uint32_t left = 1024 - ((tu_cs_get_cur_iova(cs) & 4095) / 4);
      if (left < count * size && tu_cs_get_space(cs) >= left + count * size) {
         cs->cur += left;
         cs->start = cs->cur;
      }
   }

   VkResult result;
   if (tu_cs_get_space(cs) < count * size) {
      /* When we have to allocate a new BO, assume that the alignment of the
       * BO is sufficient.
       */
      result = tu_cs_reserve_space(cs, count * size);
   } else {
      result = tu_cs_reserve_space(cs, count * size + (size - tu_cs_get_offset(cs)) % size);
      cs->start += (size - tu_cs_get_offset(cs)) % size;
   }
   if (result != VK_SUCCESS) {
      /* Freshly declared, nothing owned yet so safe to zero. */
      memset(sub_cs, 0, sizeof(*sub_cs));
      tu_cs_fail(sub_cs, result);
      return result;
   }

   cs->cur = cs->start;

   tu_cs_init_external(sub_cs, cs->device, cs->cur, cs->reserved_end,
                       tu_cs_get_cur_iova(cs), cs->writeable);
   tu_cs_begin(sub_cs);
   tu_cs_reserve_space(sub_cs, count * size);

   return VK_SUCCESS;
}

/**
 * Allocate count*size dwords, aligned to size dwords.
 * \a cs must be in TU_CS_MODE_SUB_STREAM mode.
 *
 */
VkResult
tu_cs_alloc(struct tu_cs *cs,
            uint32_t count,
            uint32_t size,
            struct tu_cs_memory *memory)
{
   assert(cs->mode == TU_CS_MODE_SUB_STREAM);
   assert(size && size <= 1024);

   if (!count) {
      /* If you allocated no memory, you'd better not use the iova for anything
       * (but it's left aligned for sanity).
       */
      memory->map = NULL;
      memory->iova = 0xdead0000;
      memory->writeable = false;
      return VK_SUCCESS;
   }

   /* TODO: smarter way to deal with alignment? */

   VkResult result = tu_cs_reserve_space(cs, count * size + (size-1));
   if (result != VK_SUCCESS)
      return result;

   struct tu_bo *bo = tu_cs_current_bo(cs);
   size_t offset = align(tu_cs_get_offset(cs), size);

   memory->map = (uint32_t *) bo->map + offset;
   memory->iova = bo->iova + offset * sizeof(uint32_t);
   memory->writeable = cs->writeable;

   cs->start = cs->cur = (uint32_t*) bo->map + offset + count * size;

   return VK_SUCCESS;
}

/**
 * End command packet emission to a sub-stream.  \a sub_cs becomes invalid
 * after this call.
 *
 * Return an IB entry for the sub-stream.  The entry has the same lifetime as
 * \a cs.
 */
struct tu_cs_entry
tu_cs_end_sub_stream(struct tu_cs *cs, struct tu_cs *sub_cs)
{
   assert(cs->mode == TU_CS_MODE_SUB_STREAM);

   /* Most callers of tu_cs_begin_sub_stream() don't check its VkResult and
    * call us unconditionally afterwards. So, we need to handle the case 
    * where cs/sub_cs is invalid due to a prior failure.
    */
   if (unlikely(cs->status != VK_SUCCESS || sub_cs->status != VK_SUCCESS))
      return (struct tu_cs_entry) {};

   assert(sub_cs->start == cs->cur && sub_cs->end == cs->reserved_end);
   tu_cs_sanity_check(sub_cs);

   tu_cs_end(sub_cs);

   cs->cur = sub_cs->cur;

   struct tu_cs_entry entry = {
      .bo = tu_cs_current_bo(cs),
      .size = tu_cs_get_size(cs) * sizeof(uint32_t),
      .offset = tu_cs_get_offset(cs) * sizeof(uint32_t),
   };

   cs->start = cs->cur;

   return entry;
}

/**
 * Reserve space from a command stream for \a reserved_size uint32_t values.
 * This never fails when \a cs has mode TU_CS_MODE_EXTERNAL.
 */
VkResult
tu_cs_reserve_space(struct tu_cs *cs, uint32_t reserved_size)
{
   if (unlikely(cs->status != VK_SUCCESS))
      return cs->status;

   if (tu_cs_get_space(cs) < reserved_size) {
      if (cs->mode == TU_CS_MODE_EXTERNAL) {
         UNREACHABLE("cannot grow external buffer");
         cs->status = VK_ERROR_OUT_OF_HOST_MEMORY;
         return cs->status;
      }

      /* add an entry for the exiting command packets */
      if (!tu_cs_is_empty(cs)) {
         /* no direct command packet for TU_CS_MODE_SUB_STREAM */
         assert(cs->mode != TU_CS_MODE_SUB_STREAM);

         tu_cs_add_entry(cs);
      }

      for (uint32_t i = 0; i < cs->cond_stack_depth; i++) {
         /* Subtract one here to account for the DWORD field itself. */
         *cs->cond_dwords[i] = cs->cur - cs->cond_dwords[i] - 1;

         /* space for CP_COND_REG_EXEC in next bo */
         reserved_size += 3;
      }

      /* switch to a new BO */
      uint32_t new_size = MAX2(cs->next_bo_size, reserved_size);
      VkResult result = tu_cs_add_bo(cs, new_size);
      if (result != VK_SUCCESS) {
         tu_cs_fail(cs, result);
         return cs->status;
      }

      if (cs->cond_stack_depth) {
         cs->reserved_end = cs->cur + reserved_size;
      }

      /* Re-emit CP_COND_REG_EXECs */
      for (uint32_t i = 0; i < cs->cond_stack_depth; i++) {
         tu_cs_emit_pkt7(cs, CP_COND_REG_EXEC, 2);
         tu_cs_emit(cs, cs->cond_flags[i]);

         cs->cond_dwords[i] = cs->cur;

         /* Emit dummy DWORD field here */
         tu_cs_emit(cs, RENDER_MODE_CP_COND_REG_EXEC_1_DWORDS(0));
      }

      /* Double the size for the next bo. */
      new_size = tu_sanitize_ib_size(new_size << 1);
      if (cs->next_bo_size < new_size)
         cs->next_bo_size = new_size;
   }

   assert(tu_cs_get_space(cs) >= reserved_size);
   cs->reserved_end = cs->cur + reserved_size;

   if (cs->mode == TU_CS_MODE_GROW) {
      /* reserve an entry for the next call to this function or tu_cs_end */
      VkResult result = tu_cs_reserve_entry(cs);
      if (result != VK_SUCCESS)
         tu_cs_fail(cs, result);
      return cs->status;
   }

   return VK_SUCCESS;
}

void
tu_cs_align_draw_state(struct tu_cs *cs, uint16_t cnt)
{
   if (!cnt || cnt >= 1024 || cs->status != VK_SUCCESS)
      return;

   tu_cs_reserve(cs, cnt + 1);
   if (cs->status != VK_SUCCESS)
      return;

   uint32_t remaining = 1024 - ((tu_cs_get_cur_iova(cs) & 4095) / 4);
   if (remaining >= cnt + 1)
      return;

   tu_cs_reserve(cs, remaining + cnt + 1);
   if (cs->status != VK_SUCCESS)
      return;
   remaining = 1024 - ((tu_cs_get_cur_iova(cs) & 4095) / 4);
   if (remaining >= cnt + 1)
      return;

   static std::atomic<uint32_t> logged{0};
   if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
      mesa_logi("SDS page align: iova=0x%" PRIx64 " payload=%u pad=%u",
                tu_cs_get_cur_iova(cs), cnt, remaining);

   tu_cs_emit(cs, pm4_pkt7_hdr(CP_NOP, remaining - 1));
   for (uint32_t i = 1; i < remaining; i++)
      tu_cs_emit(cs, 0);
}

/**
 * TU_DEBUG=ib2pagesplit workaround for A8XX: the CP has been seen to fetch
 * zeros instead of the second page of an IB2 that crosses a 4 KiB boundary,
 * although the memory holds the right commands and no SMMU fault is raised.
 * When the next reservation would cross the boundary, pad the current page
 * with a NOP and start a new IB entry on the next page, so that every IB of
 * the stream lies within one page. Conditional execution regions are closed
 * at the split and reopened after it, like on a BO switch. Reservations of a
 * page or more cannot be kept within a page and are left alone.
 */
void
tu_cs_page_split(struct tu_cs *cs, uint32_t reserved_size)
{
   assert(cs->mode == TU_CS_MODE_GROW);
   if (cs->status != VK_SUCCESS || !cs->cur)
      return;

   const uint32_t cond_size = 3 * cs->cond_stack_depth;
   if (reserved_size + cond_size >= 1024)
      return;

   const uint32_t left = 1024 - ((tu_cs_get_cur_iova(cs) & 4095) / 4);
   if (reserved_size <= left)
      return;

   /* Not enough room in this BO: tu_cs_reserve_space() switches to a new,
    * page-aligned BO anyway.
    */
   if (tu_cs_get_space(cs) < left + reserved_size + cond_size)
      return;

   if (tu_cs_is_empty(cs)) {
      /* Nothing to end yet: start the next entry on the next page. */
      cs->cur += left;
      cs->start = cs->cur;
      cs->reserved_end = cs->cur;
      return;
   }

   if (tu_cs_reserve_entry(cs) != VK_SUCCESS) {
      tu_cs_fail(cs, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   for (uint32_t i = 0; i < cs->cond_stack_depth; i++) {
      /* Subtract one here to account for the DWORD field itself. */
      *cs->cond_dwords[i] = cs->cur - cs->cond_dwords[i] - 1;
   }

   cs->reserved_end = cs->cur + left;
   tu_cs_emit(cs, pm4_pkt7_hdr(CP_NOP, left - 1));
   for (uint32_t i = 1; i < left; i++)
      tu_cs_emit(cs, 0);
   tu_cs_add_entry(cs);

   static std::atomic<uint32_t> logged{0};
   if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
      mesa_logi("IB2 page split (%s): next IB starts at 0x%" PRIx64 ", padded %u dwords",
                cs->name, tu_cs_get_cur_iova(cs), left);

   cs->reserved_end = cs->cur + cond_size;
   for (uint32_t i = 0; i < cs->cond_stack_depth; i++) {
      tu_cs_emit(cs, pm4_pkt7_hdr(CP_COND_REG_EXEC, 2));
      tu_cs_emit(cs, cs->cond_flags[i]);
      cs->cond_dwords[i] = cs->cur;
      /* Emit dummy DWORD field here */
      tu_cs_emit(cs, RENDER_MODE_CP_COND_REG_EXEC_1_DWORDS(0));
   }
   cs->reserved_end = cs->cur;
}

/**
 * Reset a command stream to its initial state.  This discards all comand
 * packets in \a cs, but does not necessarily release all resources.
 */
void
tu_cs_reset(struct tu_cs *cs)
{
   cs->status = VK_SUCCESS;

   if (cs->mode == TU_CS_MODE_EXTERNAL) {
      assert(!cs->read_only.bo_count && !cs->read_write.bo_count &&
             !cs->refcount_bo && !cs->entry_count);
      cs->reserved_end = cs->cur = cs->start;
      return;
   }

   for (uint32_t i = 0; i + 1 < cs->read_only.bo_count; ++i) {
      TU_RMV(resource_destroy, cs->device, cs->read_only.bos[i]);
      tu_cs_release_bo(cs, cs->read_only.bos[i], true);
   }

   for (uint32_t i = 0; i + 1 < cs->read_write.bo_count; ++i) {
      TU_RMV(resource_destroy, cs->device, cs->read_write.bos[i]);
      tu_cs_release_bo(cs, cs->read_write.bos[i], false);
   }

   assert(!cs->writeable);

   if (cs->read_only.bo_count) {
      cs->read_only.bos[0] = cs->read_only.bos[cs->read_only.bo_count - 1];
      cs->read_only.bo_count = 1;

      struct tu_bo *bo = cs->read_only.bos[0];
      cs->start = cs->cur = cs->reserved_end =
         (uint32_t *) bo->map + tu_cs_bo_head_skip(bo);
      cs->end = (uint32_t *) bo->map +
                tu_sanitize_ib_size(bo->size / sizeof(uint32_t));
   }

   if (cs->read_write.bo_count) {
      cs->read_write.bos[0] = cs->read_write.bos[cs->read_write.bo_count - 1];
      cs->read_write.bo_count = 1;

      cs->read_write.start = (uint32_t *) cs->read_write.bos[0]->map +
                             tu_cs_bo_head_skip(cs->read_write.bos[0]);
   }

   cs->entry_count = 0;
}

uint64_t
tu_cs_emit_data_nop(struct tu_cs *cs,
                    const uint32_t *data,
                    uint32_t size,
                    uint32_t align_dwords)
{
   if (unlikely(cs->status != VK_SUCCESS))
      return 0;

   uint32_t total_size = size + (align_dwords - 1);
   tu_cs_emit_pkt7(cs, CP_NOP, total_size);

   uint64_t iova = tu_cs_get_cur_iova(cs);
   uint64_t iova_aligned = align64(iova, align_dwords * sizeof(uint32_t));
   size_t offset = (iova_aligned - iova) / sizeof(uint32_t);
   cs->cur += offset;
   memcpy(cs->cur, data, size * sizeof(uint32_t));

   cs->cur += total_size - offset;

   return iova + offset * sizeof(uint32_t);
}

void
tu_cs_emit_debug_string(struct tu_cs *cs, const char *string, int len)
{
   assert(cs->mode == TU_CS_MODE_GROW);

   /* max packet size is 0x3fff dwords */
   len = MIN2(len, 0x3fff * 4);

   tu_cs_emit_pkt7(cs, CP_NOP, align(len, 4) / 4);
   const uint32_t *buf = (const uint32_t *) string;

   tu_cs_emit_array(cs, buf, len / 4);
   buf += len / 4;
   len = len % 4;

   /* copy remainder bytes without reading past end of input string */
   if (len > 0) {
      uint32_t w = 0;
      memcpy(&w, buf, len);
      tu_cs_emit(cs, w);
   }
}

void
tu_cs_emit_debug_magic_strv(struct tu_cs *cs,
                            uint32_t magic,
                            const char *fmt,
                            va_list args)
{
   int fmt_len = vsnprintf(NULL, 0, fmt, args);
   int len = 4 + fmt_len + 1;
   char *string = (char *) malloc(len);

   /* format: <magic><formatted string>\0 */
   *(uint32_t *) string = magic;
   vsnprintf(string + 4, fmt_len + 1, fmt, args);

   tu_cs_emit_debug_string(cs, string, len);
   free(string);
}

__attribute__((format(printf, 2, 3))) void
tu_cs_emit_debug_msg(struct tu_cs *cs, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   tu_cs_emit_debug_magic_strv(cs, CP_NOP_MESG, fmt, args);
   va_end(args);
}

void
tu_cs_trace_start(struct u_trace_context *utctx,
                  void *cs,
                  const char *fmt,
                  ...)
{
   va_list args;
   va_start(args, fmt);
   tu_cs_emit_debug_magic_strv((struct tu_cs *) cs, CP_NOP_BEGN, fmt, args);
   va_end(args);
}

void
tu_cs_trace_end(struct u_trace_context *utctx, void *cs, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   tu_cs_emit_debug_magic_strv((struct tu_cs *) cs, CP_NOP_END, fmt, args);
   va_end(args);
}

void
tu_cs_trace_singular(struct u_trace_context *utctx, void *cs, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   tu_cs_emit_debug_magic_strv((struct tu_cs *) cs, CP_NOP_MESG, fmt, args);
   va_end(args);
}

tu_crb
tu_cs::crb(uint32_t nregs)
{
   return tu_crb(this, nregs);
}
