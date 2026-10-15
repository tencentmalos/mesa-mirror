/*
 * Copyright © 2026 shadPS4 contributors
 * SPDX-License-Identifier: MIT
 */

#include "tu_cmd_audit.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util/os_time.h"
#include "util/simple_mtx.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_queue.h"

/* One record per IB2 call found in a submitted IB1. */
struct audit_ib2 {
   uint64_t iova;
   uint64_t hash;
   const struct tu_bo *bo; /* containing BO, NULL if no live BO contains it */
   uint64_t bo_iova;
   uint32_t gem;
   uint32_t size_dw;
   int32_t first_bad;      /* dword index of the first malformed header, -1 if none */
   bool global;            /* found in the live BO table, not owned by the cmdbuf */
};

#define AUDIT_SUBMITS      32
#define AUDIT_IB2_PER_SUB  1024
#define AUDIT_BO_EVENTS    4096

struct audit_submit {
   uint64_t seq;
   uint64_t t_ns;
   uint32_t ctx;
   uint32_t ts;
   uint32_t ib1_count;
   uint32_t ib1_bad;
   uint32_t ib2_count;     /* recorded */
   uint32_t ib2_dropped;   /* beyond AUDIT_IB2_PER_SUB */
   uint32_t ib2_unresolved;
   uint32_t ib2_bad;
   struct audit_ib2 ib2[AUDIT_IB2_PER_SUB];
};

struct audit_bo_event {
   uint64_t t_ns;
   uint64_t iova;
   uint64_t size;
   uint64_t submit_seq;
   char name[24];
   uint32_t gem;
   uint32_t ctx;
   uint32_t last_ts;
   uint32_t retired;
   bool alloc;
   bool pending;
};

/* Every BO that is currently allocated, sorted by iova, so that addresses in
 * CP_SET_DRAW_STATE groups and IB2 calls that point outside the command
 * buffer's own BOs (pipelines, suballocators) can be resolved.
 */
struct audit_page {
   uint64_t first_seq;
   uint64_t last_seq;
   uint32_t refs;     /* submissions whose IB1/IB2/draw states touched the page */
   uint32_t last_ts;
};

struct audit_live {
   uint64_t iova;
   uint64_t size;
   const struct tu_bo *bo;
   struct audit_page *pages; /* allocated when first referenced */
   uint64_t alloc_seq;
   uint64_t alloc_ns;
   uint32_t gem;
   char name[20];
};

/* Content recorded for the newest submissions: every unique IB1 entry, IB2
 * call and draw-state group, with a copy of the dwords at submit time.
 */
enum audit_item_kind { ITEM_IB1, ITEM_IB2, ITEM_SDS };

enum audit_item_state {
   STATE_OK,
   STATE_UNRESOLVED, /* no live BO contains the range */
   STATE_UNMAPPED,   /* BO not CPU mapped, content not checked */
};

struct audit_item {
   uint64_t iova;
   uint64_t hash;
   uint64_t bo_iova;
   const struct tu_bo *bo;
   uint32_t gem;
   uint32_t size_dw;
   uint32_t d0;        /* CP_SET_DRAW_STATE dword 0 for draw-state groups */
   uint32_t parent;    /* IB2 call index + 1 for groups found in an IB2, 0 for IB1 */
   uint32_t copy_dw;   /* offset of the submit-time copy in the slot arena, ~0 if none */
   int32_t first_bad;
   /* History of the first and last page of the item before this submission. */
   uint32_t page[2];
   uint32_t page_refs[2];
   uint64_t page_last_seq[2];
   uint64_t bo_alloc_seq;
   uint64_t bo_alloc_ns;
   uint8_t kind;
   uint8_t state;
   char name[20];
};

#define AUDIT_CONTENT_SUBMITS   8
#define AUDIT_ITEMS_PER_SUB     16384
#define AUDIT_ARENA_DW          (1u << 20) /* 4 MiB per submission */
#define AUDIT_DEDUPE_SLOTS      32768
#define AUDIT_FILE_SUBMITS      3

struct audit_content {
   uint64_t seq;
   uint32_t item_count;
   uint32_t item_dropped;
   uint32_t sds_dup;
   uint32_t arena_used;
   uint32_t arena_full;
   struct audit_item items[AUDIT_ITEMS_PER_SUB];
   uint32_t arena[AUDIT_ARENA_DW];
};

static simple_mtx_t audit_mtx = SIMPLE_MTX_INITIALIZER;
static struct audit_submit *audit_submits;
static uint64_t audit_submit_seq;
static struct audit_bo_event *audit_events;
static uint64_t audit_event_count;
static uint32_t audit_live_reports;
static bool audit_dumped;

static struct audit_live *audit_lives;
static uint32_t audit_live_count;
static uint32_t audit_live_cap;

static struct audit_content *audit_contents;
static uint64_t *audit_dedupe_key;
static uint32_t *audit_dedupe_gen;
static uint32_t audit_dedupe_cur;

static bool
audit_init_locked(void)
{
   if (!audit_submits) {
      audit_submits = (struct audit_submit *)
         calloc(AUDIT_SUBMITS, sizeof(struct audit_submit));
      audit_events = (struct audit_bo_event *)
         calloc(AUDIT_BO_EVENTS, sizeof(struct audit_bo_event));
      audit_contents = (struct audit_content *)
         calloc(AUDIT_CONTENT_SUBMITS, sizeof(struct audit_content));
      audit_dedupe_key = (uint64_t *) calloc(AUDIT_DEDUPE_SLOTS, sizeof(uint64_t));
      audit_dedupe_gen = (uint32_t *) calloc(AUDIT_DEDUPE_SLOTS, sizeof(uint32_t));
   }
   return audit_submits && audit_events && audit_contents &&
          audit_dedupe_key && audit_dedupe_gen;
}

static uint64_t
audit_hash(const uint32_t *dw, uint32_t count)
{
   uint64_t h = 0xcbf29ce484222325ull;
   for (uint32_t i = 0; i < count; i++) {
      h ^= dw[i];
      h *= 0x100000001b3ull;
   }
   return h;
}

/* Walks pkt4/pkt7 headers. Returns the index of the first header that is not
 * a well-formed pkt4/pkt7 (or whose payload runs past the end), -1 otherwise.
 * CP_INDIRECT_BUFFER targets are reported through on_ib, every enabled
 * CP_SET_DRAW_STATE group through on_sds.
 */
template <typename FIb, typename FSds>
static int32_t
audit_walk(const uint32_t *dw, uint32_t count, FIb &&on_ib, FSds &&on_sds)
{
   uint32_t i = 0;
   while (i < count) {
      const uint32_t h = dw[i];
      uint32_t cnt;
      if ((h >> 28) == 7) {
         const uint32_t op = (h >> 16) & 0x7f;
         cnt = h & 0x3fff;
         if (h != pm4_pkt7_hdr(op, cnt))
            return i;
         if (i + 1 + cnt > count)
            return i;
         if (op == CP_INDIRECT_BUFFER && cnt == 3) {
            on_ib(dw[i + 1] | ((uint64_t)dw[i + 2] << 32), dw[i + 3] & 0xfffff);
         } else if (op == CP_SET_DRAW_STATE) {
            for (uint32_t g = 0; g + 3 <= cnt; g += 3) {
               const uint32_t d0 = dw[i + 1 + g];
               const uint64_t addr = dw[i + 2 + g] | ((uint64_t)dw[i + 3 + g] << 32);
               /* COUNT 0, DISABLE or DISABLE_ALL_GROUPS: nothing is read. */
               if ((d0 & 0xffff) && !(d0 & (0x3u << 17)))
                  on_sds(d0, addr);
            }
         }
      } else if ((h >> 28) == 4) {
         const uint32_t reg = (h >> 8) & 0x3ffff;
         cnt = h & 0x7f;
         if (cnt == 0 || cnt >= 0x7f || reg == 0 || h != pm4_pkt4_hdr(reg, cnt))
            return i;
         if (i + 1 + cnt > count)
            return i;
      } else {
         return i;
      }
      i += 1 + cnt;
   }
   return -1;
}

static int32_t
audit_check(const uint32_t *dw, uint32_t count)
{
   return audit_walk(dw, count, [](uint64_t, uint32_t) {}, [](uint32_t, uint64_t) {});
}

struct audit_bo_ref {
   struct tu_bo *bo;
};

#define AUDIT_MAX_BOS 256

static void
audit_collect_cs(struct tu_cs *cs, struct audit_bo_ref *refs, uint32_t *count)
{
   const struct tu_bo_array *arrays[2] = { &cs->read_only, &cs->read_write };
   for (unsigned a = 0; a < 2; a++) {
      for (uint32_t i = 0; i < arrays[a]->bo_count && *count < AUDIT_MAX_BOS; i++) {
         struct tu_bo *bo = arrays[a]->bos[i];
         if (bo && bo->map)
            refs[(*count)++].bo = bo;
      }
   }
   if (cs->refcount_bo && cs->refcount_bo->map && *count < AUDIT_MAX_BOS)
      refs[(*count)++].bo = cs->refcount_bo;
}

static struct tu_bo *
audit_find(struct audit_bo_ref *refs, uint32_t count, uint64_t iova, uint64_t bytes)
{
   for (uint32_t i = 0; i < count; i++) {
      struct tu_bo *bo = refs[i].bo;
      if (iova >= bo->iova && iova + bytes <= bo->iova + bo->size)
         return bo;
   }
   return NULL;
}

/* Index of the last live entry whose iova is <= iova, or -1. */
static int32_t
audit_live_floor(uint64_t iova)
{
   int32_t lo = 0, hi = (int32_t) audit_live_count - 1, best = -1;
   while (lo <= hi) {
      const int32_t mid = (lo + hi) / 2;
      if (audit_lives[mid].iova <= iova) {
         best = mid;
         lo = mid + 1;
      } else {
         hi = mid - 1;
      }
   }
   return best;
}

static struct audit_live *
audit_live_find(uint64_t iova, uint64_t bytes)
{
   const int32_t i = audit_live_floor(iova);
   if (i < 0)
      return NULL;
   struct audit_live *l = &audit_lives[i];
   return iova + bytes <= l->iova + l->size ? l : NULL;
}

static void
audit_live_insert(const struct tu_bo *bo, const char *name)
{
   if (!bo->iova || !bo->size)
      return;
   if (audit_live_count == audit_live_cap) {
      const uint32_t cap = audit_live_cap ? audit_live_cap * 2 : 1024;
      struct audit_live *n = (struct audit_live *)
         realloc(audit_lives, cap * sizeof(*n));
      if (!n)
         return;
      audit_lives = n;
      audit_live_cap = cap;
   }
   const int32_t floor = audit_live_floor(bo->iova);
   const uint32_t at = (uint32_t) (floor + 1);
   memmove(&audit_lives[at + 1], &audit_lives[at],
           (audit_live_count - at) * sizeof(*audit_lives));
   struct audit_live *l = &audit_lives[at];
   l->iova = bo->iova;
   l->size = bo->size;
   l->bo = bo;
   l->pages = NULL;
   l->alloc_seq = audit_submit_seq;
   l->alloc_ns = os_time_get_nano();
   l->gem = bo->gem_handle;
   snprintf(l->name, sizeof(l->name), "%s", name ? name : "");
   audit_live_count++;
}

static void
audit_live_remove(const struct tu_bo *bo)
{
   int32_t i = audit_live_floor(bo->iova);
   if (i < 0 || audit_lives[i].iova != bo->iova || audit_lives[i].gem != bo->gem_handle) {
      /* The iova can change after allocation (lazy sparse VMA): fall back to
       * the BO pointer.
       */
      i = -1;
      for (uint32_t k = 0; k < audit_live_count; k++) {
         if (audit_lives[k].bo == bo && audit_lives[k].gem == bo->gem_handle) {
            i = (int32_t) k;
            break;
         }
      }
      if (i < 0)
         return;
   }
   free(audit_lives[i].pages);
   memmove(&audit_lives[i], &audit_lives[i + 1],
           (audit_live_count - i - 1) * sizeof(*audit_lives));
   audit_live_count--;
}

/* Returns true the first time a key is seen for the current submission. */
static bool
audit_dedupe_first(uint64_t key)
{
   uint32_t slot = (uint32_t) ((key * 0x9e3779b97f4a7c15ull) >> 49) & (AUDIT_DEDUPE_SLOTS - 1);
   for (uint32_t probe = 0; probe < 64; probe++) {
      const uint32_t s = (slot + probe) & (AUDIT_DEDUPE_SLOTS - 1);
      if (audit_dedupe_gen[s] != audit_dedupe_cur) {
         audit_dedupe_gen[s] = audit_dedupe_cur;
         audit_dedupe_key[s] = key;
         return true;
      }
      if (audit_dedupe_key[s] == key)
         return false;
   }
   return true; /* table crowded: record a duplicate rather than miss one */
}

/* Records that submission seq touched [iova, iova + bytes) of l and returns
 * the history of the first and last page as it was before.
 */
static void
audit_live_touch(struct audit_live *l, uint64_t iova, uint64_t bytes, uint64_t seq,
                 uint32_t ts, struct audit_item *it)
{
   const uint32_t npages = (uint32_t) ((l->size + 4095) / 4096);
   if (!l->pages) {
      l->pages = (struct audit_page *) calloc(npages, sizeof(*l->pages));
      if (!l->pages)
         return;
   }
   const uint32_t first = (uint32_t) ((iova - l->iova) / 4096);
   const uint32_t last = MIN2((uint32_t) ((iova + MAX2(bytes, 4) - 1 - l->iova) / 4096), npages - 1);
   if (it) {
      it->page[0] = first;
      it->page[1] = last;
      it->page_refs[0] = l->pages[first].refs;
      it->page_last_seq[0] = l->pages[first].last_seq;
      it->page_refs[1] = l->pages[last].refs;
      it->page_last_seq[1] = l->pages[last].last_seq;
      it->bo_alloc_seq = l->alloc_seq;
      it->bo_alloc_ns = l->alloc_ns;
   }
   for (uint32_t p = first; p <= last; p++) {
      struct audit_page *pg = &l->pages[p];
      if (pg->refs && pg->last_seq == seq)
         continue;
      if (!pg->refs)
         pg->first_seq = seq;
      pg->refs++;
      pg->last_seq = seq;
      pg->last_ts = ts;
   }
}

/* Records one IB1 entry, IB2 call or draw-state group of the current
 * submission and copies its dwords. Returns the copy (or the live mapping
 * when the arena is full), NULL if the content cannot be read.
 */
static const uint32_t *
audit_record_item(struct audit_content *c, uint32_t ts, uint8_t kind, uint64_t iova,
                  uint32_t size_dw, uint32_t d0, uint32_t parent, const struct tu_bo *bo,
                  struct audit_item **out)
{
   *out = NULL;
   if (c->item_count >= AUDIT_ITEMS_PER_SUB) {
      c->item_dropped++;
      return NULL;
   }
   struct audit_item *it = &c->items[c->item_count++];
   memset(it, 0, sizeof(*it));
   it->iova = iova;
   it->size_dw = size_dw;
   it->d0 = d0;
   it->parent = parent;
   it->kind = kind;
   it->first_bad = -1;
   it->copy_dw = ~0u;
   *out = it;

   struct audit_live *l = audit_live_find(iova, (uint64_t) size_dw * 4);
   if (l)
      audit_live_touch(l, iova, (uint64_t) size_dw * 4, c->seq, ts, it);
   if (bo) {
      it->bo = bo;
      it->bo_iova = bo->iova;
      it->gem = bo->gem_handle;
   } else if (l) {
      it->bo = l->bo;
      it->bo_iova = l->iova;
      it->gem = l->gem;
   } else {
      it->state = STATE_UNRESOLVED;
      return NULL;
   }
   if (l)
      snprintf(it->name, sizeof(it->name), "%s", l->name);
   if (!it->bo->map) {
      it->state = STATE_UNMAPPED;
      return NULL;
   }
   const uint32_t *src =
      (const uint32_t *) ((const char *) it->bo->map + (iova - it->bo_iova));
   if (c->arena_used + size_dw <= AUDIT_ARENA_DW) {
      it->copy_dw = c->arena_used;
      memcpy(&c->arena[c->arena_used], src, (size_t) size_dw * 4);
      c->arena_used += size_dw;
      src = &c->arena[it->copy_dw];
   } else {
      c->arena_full++;
   }
   it->hash = audit_hash(src, size_dw);
   return src;
}

void
tu_cmd_audit_submit(struct tu_queue *queue,
                    struct tu_cmd_buffer **cmd_buffers,
                    uint32_t cmdbuf_count)
{
   const uint32_t ctx = queue->msm_queue_id;
   const uint32_t ts = (uint32_t) p_atomic_read(&queue->fence);

   simple_mtx_lock(&audit_mtx);
   if (!audit_init_locked()) {
      simple_mtx_unlock(&audit_mtx);
      return;
   }

   struct audit_submit *s = &audit_submits[audit_submit_seq % AUDIT_SUBMITS];
   memset(s, 0, offsetof(struct audit_submit, ib2));
   s->seq = audit_submit_seq++;
   s->t_ns = os_time_get_nano();
   s->ctx = ctx;
   s->ts = ts;

   struct audit_content *c = &audit_contents[s->seq % AUDIT_CONTENT_SUBMITS];
   c->seq = s->seq;
   c->item_count = 0;
   c->item_dropped = 0;
   c->sds_dup = 0;
   c->arena_used = 0;
   c->arena_full = 0;
   if (++audit_dedupe_cur == 0)
      audit_dedupe_cur = 1;

   /* Draw-state groups: dedupe by address and size, the same pipeline and
    * dynamic states are referenced by many draws and every GMEM bin.
    */
   auto on_sds = [&](uint32_t parent) {
      return [&, parent](uint32_t d0, uint64_t addr) {
         const uint32_t count = d0 & 0xffff;
         if (!audit_dedupe_first(addr | ((uint64_t) count << 44) | (1ull << 62))) {
            c->sds_dup++;
            return;
         }
         struct audit_item *it;
         const uint32_t *dw = audit_record_item(c, ts, ITEM_SDS, addr, count, d0, parent, NULL, &it);
         if (!dw || !it)
            return;
         it->first_bad = audit_check(dw, count);
         if (it->first_bad >= 0 && audit_live_reports < 32) {
            audit_live_reports++;
            mesa_loge("cmdaudit: submit seq=%" PRIu64 " ctx=%u ts=%u draw state group %u at 0x%" PRIx64
                      " count=%u malformed at dword %d (0x%08x) at submit, BO gem=%u %s",
                      s->seq, ctx, ts, (d0 >> 24) & 0x3f, addr, count, it->first_bad,
                      dw[it->first_bad], it->gem, it->name);
         }
      };
   };

   struct audit_bo_ref refs[AUDIT_MAX_BOS];

   for (uint32_t cb = 0; cb < cmdbuf_count; cb++) {
      struct tu_cmd_buffer *cmd = cmd_buffers[cb];
      uint32_t ref_count = 0;
      audit_collect_cs(&cmd->cs, refs, &ref_count);
      audit_collect_cs(&cmd->draw_cs, refs, &ref_count);
      audit_collect_cs(&cmd->tile_store_cs, refs, &ref_count);
      audit_collect_cs(&cmd->draw_epilogue_cs, refs, &ref_count);
      audit_collect_cs(&cmd->sub_cs, refs, &ref_count);
      audit_collect_cs(&cmd->pre_chain.draw_cs, refs, &ref_count);
      audit_collect_cs(&cmd->pre_chain.draw_epilogue_cs, refs, &ref_count);

      /* These BOs are referenced by this submission until it retires. */
      for (uint32_t i = 0; i < ref_count; i++) {
         p_atomic_set(&refs[i].bo->audit_ts, ts);
         p_atomic_set(&refs[i].bo->audit_ctx, ctx);
      }

      for (uint32_t e = 0; e < cmd->cs.entry_count; e++) {
         const struct tu_cs_entry *entry = &cmd->cs.entries[e];
         s->ib1_count++;
         if (!entry->bo || !entry->bo->map || entry->bo->gem_handle == 0) {
            s->ib1_bad++;
            if (audit_live_reports < 32) {
               audit_live_reports++;
               mesa_loge("cmdaudit: submit seq=%" PRIu64 " ctx=%u ts=%u IB1 entry %u refers to a freed or unmapped BO",
                         s->seq, ctx, ts, e);
            }
            continue;
         }
         const uint64_t ib1_iova = entry->bo->iova + entry->offset;
         const uint32_t ib1_dw = entry->size / 4;
         struct audit_item *ib1_item;
         const uint32_t *ib1 = audit_record_item(c, ts, ITEM_IB1, ib1_iova, ib1_dw, 0, 0,
                                                 entry->bo, &ib1_item);
         if (!ib1)
            ib1 = (const uint32_t *) ((const char *) entry->bo->map + entry->offset);
         const int32_t bad1 = audit_walk(ib1, ib1_dw, [&](uint64_t iova, uint32_t size_dw) {
            if (s->ib2_count >= AUDIT_IB2_PER_SUB) {
               s->ib2_dropped++;
               return;
            }
            const uint32_t ib2_index = s->ib2_count;
            struct audit_ib2 *r = &s->ib2[s->ib2_count++];
            r->iova = iova;
            r->size_dw = size_dw;
            r->first_bad = -1;
            r->global = false;
            const struct tu_bo *bo = audit_find(refs, ref_count, iova, (uint64_t) size_dw * 4);
            if (!bo) {
               const struct audit_live *l = audit_live_find(iova, (uint64_t) size_dw * 4);
               if (l && l->bo->map) {
                  bo = l->bo;
                  r->global = true;
               }
            }
            r->bo = bo;
            if (!bo) {
               s->ib2_unresolved++;
               return;
            }
            r->bo_iova = bo->iova;
            r->gem = bo->gem_handle;
            const uint32_t *ib2 =
               (const uint32_t *) ((const char *) bo->map + (iova - bo->iova));
            /* GMEM replays the same draw IB2 once per bin: copy and walk the
             * draw states of each IB2 once.
             */
            if (audit_dedupe_first(iova | ((uint64_t) size_dw << 44) | (1ull << 63))) {
               struct audit_item *ib2_item;
               const uint32_t *copy = audit_record_item(c, ts, ITEM_IB2, iova, size_dw, 0, 0, bo, &ib2_item);
               if (copy)
                  ib2 = copy;
               r->first_bad = audit_walk(ib2, size_dw, [](uint64_t, uint32_t) {},
                                         on_sds(ib2_index + 1));
               if (ib2_item)
                  ib2_item->first_bad = r->first_bad;
               r->hash = copy ? ib2_item->hash : audit_hash(ib2, size_dw);
            } else {
               r->first_bad = audit_check(ib2, size_dw);
               r->hash = audit_hash(ib2, size_dw);
            }
            if (r->first_bad >= 0) {
               s->ib2_bad++;
               if (audit_live_reports < 32) {
                  audit_live_reports++;
                  mesa_loge("cmdaudit: submit seq=%" PRIu64 " ctx=%u ts=%u IB2 0x%" PRIx64
                            " size=%u malformed at dword %d (0x%08x) at submit, BO gem=%u iova=0x%" PRIx64,
                            s->seq, ctx, ts, iova, size_dw, r->first_bad,
                            ib2[r->first_bad], bo->gem_handle, bo->iova);
               }
            }
         }, on_sds(0));
         if (ib1_item)
            ib1_item->first_bad = bad1;
         if (bad1 >= 0) {
            s->ib1_bad++;
            if (audit_live_reports < 32) {
               audit_live_reports++;
               mesa_loge("cmdaudit: submit seq=%" PRIu64 " ctx=%u ts=%u IB1 0x%" PRIx64
                         " size=%u malformed at dword %d (0x%08x) at submit",
                         s->seq, ctx, ts, ib1_iova, ib1_dw, bad1, ib1[bad1]);
            }
         }
      }
   }
   simple_mtx_unlock(&audit_mtx);
}

void
tu_cmd_audit_bo_event(const struct tu_bo *bo, bool alloc, const char *name,
                      uint32_t retired, bool pending)
{
   simple_mtx_lock(&audit_mtx);
   if (!audit_init_locked()) {
      simple_mtx_unlock(&audit_mtx);
      return;
   }
   struct audit_bo_event *ev = &audit_events[audit_event_count++ % AUDIT_BO_EVENTS];
   ev->t_ns = os_time_get_nano();
   ev->iova = bo->iova;
   ev->size = bo->size;
   ev->submit_seq = audit_submit_seq;
   snprintf(ev->name, sizeof(ev->name), "%s", name ? name : "");
   ev->gem = bo->gem_handle;
   ev->ctx = bo->audit_ctx;
   ev->last_ts = bo->audit_ts;
   ev->retired = retired;
   ev->alloc = alloc;
   ev->pending = pending;
   if (alloc)
      audit_live_insert(bo, name);
   else
      audit_live_remove(bo);
   if (pending && audit_live_reports < 32) {
      audit_live_reports++;
      mesa_loge("cmdaudit: freeing BO gem=%u iova=0x%" PRIx64 " size=0x%" PRIx64
                " still referenced by ctx=%u ts=%u (retired %u)",
                bo->gem_handle, bo->iova, bo->size, bo->audit_ctx, bo->audit_ts, retired);
   }
   simple_mtx_unlock(&audit_mtx);
}

static const char *
audit_kind_name(uint8_t kind)
{
   return kind == ITEM_IB1 ? "IB1" : kind == ITEM_IB2 ? "IB2" : "SDS";
}

static void
audit_hex(FILE *f, const char *tag, const uint32_t *dw, uint32_t count)
{
   for (uint32_t i = 0; i < count; i += 8) {
      fprintf(f, "  %s %04x:", tag, i);
      for (uint32_t j = i; j < count && j < i + 8; j++)
         fprintf(f, " %08x", dw[j]);
      fprintf(f, "\n");
   }
}

/* Compares the submit-time copies of one submission with memory now. */
static void
audit_dump_content(const struct audit_content *c, const struct audit_submit *s, FILE *f,
                   bool detail)
{
   uint32_t changed = 0, freed = 0, bad = 0, unresolved = 0, unmapped = 0, sds = 0;
   uint32_t reported = 0;
   for (uint32_t i = 0; i < c->item_count; i++) {
      const struct audit_item *it = &c->items[i];
      sds += it->kind == ITEM_SDS;
      unresolved += it->state == STATE_UNRESOLVED;
      unmapped += it->state == STATE_UNMAPPED;
      bad += it->first_bad >= 0;
      const uint32_t *copy = it->copy_dw != ~0u ? &c->arena[it->copy_dw] : NULL;
      const char *now_state = "";
      const uint32_t *now = NULL;
      int32_t diff = -1;
      if (it->state == STATE_OK) {
         const struct audit_live *l = audit_live_find(it->iova, (uint64_t) it->size_dw * 4);
         if (!l || l->gem != it->gem || l->iova != it->bo_iova || !l->bo->map) {
            freed++;
            now_state = " FREED";
         } else {
            now = (const uint32_t *) ((const char *) l->bo->map + (it->iova - l->iova));
            if (audit_hash(now, it->size_dw) != it->hash) {
               changed++;
               now_state = " CHANGED";
               if (copy) {
                  for (uint32_t k = 0; k < it->size_dw; k++) {
                     if (copy[k] != now[k]) {
                        diff = (int32_t) k;
                        break;
                     }
                  }
               }
            }
         }
      }
      const bool interesting = it->first_bad >= 0 || now_state[0];
      if (f && (detail || interesting)) {
         fprintf(f, "%s %s iova=0x%" PRIx64 " size=%u", interesting ? "!" : " ",
                 audit_kind_name(it->kind), it->iova, it->size_dw);
         if (it->kind == ITEM_SDS)
            fprintf(f, " group=%u flags=0x%02x from=%s%u", (it->d0 >> 24) & 0x3f,
                    (it->d0 >> 16) & 0xff, it->parent ? "IB2#" : "IB1", it->parent ? it->parent - 1 : 0);
         fprintf(f, " bo=0x%" PRIx64 "+0x%" PRIx64 " gem=%u name=%s state=%s bad_at_submit=%d%s",
                 it->bo_iova, it->iova - it->bo_iova, it->gem, it->name,
                 it->state == STATE_OK ? "ok" : it->state == STATE_UNRESOLVED ? "unresolved" : "unmapped",
                 it->first_bad, now_state);
         if (diff >= 0)
            fprintf(f, " first_diff=%d (0x%08x -> 0x%08x)", diff, copy[diff], now[diff]);
         if (it->state != STATE_UNRESOLVED)
            fprintf(f, " bo_alloc_seq=%" PRIu64 " page%u_prev_refs=%u last_seq=%" PRIu64
                    " page%u_prev_refs=%u last_seq=%" PRIu64,
                    it->bo_alloc_seq, it->page[0], it->page_refs[0], it->page_last_seq[0],
                    it->page[1], it->page_refs[1], it->page_last_seq[1]);
         fprintf(f, "\n");
         if (copy && (detail || interesting))
            audit_hex(f, "S", copy, it->size_dw);
         if (now && now_state[0] && copy)
            audit_hex(f, "N", now, it->size_dw);
      }
      if (interesting && reported < 16) {
         reported++;
         mesa_loge("cmdaudit:  seq=%" PRIu64 " %s 0x%" PRIx64 " size=%u group=%u bo=%s gem=%u bad_at_submit=%d%s"
                   " first_diff=%d",
                   c->seq, audit_kind_name(it->kind), it->iova, it->size_dw,
                   it->kind == ITEM_SDS ? (it->d0 >> 24) & 0x3f : 0, it->name, it->gem,
                   it->first_bad, now_state, diff);
      }
   }
   mesa_loge("cmdaudit: C seq=%" PRIu64 " ts=%u items=%u sds=%u sds_dup=%u dropped=%u arena=%u/%u full=%u"
             " bad_at_submit=%u unresolved=%u unmapped=%u changed=%u freed=%u",
             c->seq, s->ts, c->item_count, sds, c->sds_dup, c->item_dropped, c->arena_used,
             AUDIT_ARENA_DW, c->arena_full, bad, unresolved, unmapped, changed, freed);
   if (f)
      fprintf(f, "C seq=%" PRIu64 " ts=%u items=%u sds=%u sds_dup=%u dropped=%u arena=%u full=%u"
              " bad_at_submit=%u unresolved=%u unmapped=%u changed=%u freed=%u\n",
              c->seq, s->ts, c->item_count, sds, c->sds_dup, c->item_dropped, c->arena_used,
              c->arena_full, bad, unresolved, unmapped, changed, freed);
}

void
tu_cmd_audit_dump(const char *reason)
{
   simple_mtx_lock(&audit_mtx);
   if (audit_dumped || !audit_submits) {
      simple_mtx_unlock(&audit_mtx);
      return;
   }
   audit_dumped = true;
   const uint64_t now = os_time_get_nano();

   /* TU_CMD_AUDIT_FILE, else the app's cache directory (Android app processes
    * are named after their package).
    */
   char base[256] = "";
   if (const char *path = getenv("TU_CMD_AUDIT_FILE")) {
      snprintf(base, sizeof(base), "%s", path);
   } else if (FILE *cmdline = fopen("/proc/self/cmdline", "r")) {
      char pkg[128] = "";
      if (fgets(pkg, sizeof(pkg), cmdline) && pkg[0] && !strchr(pkg, '/')) {
         if (char *colon = strchr(pkg, ':'))
            *colon = 0; /* "package:process" */
         snprintf(base, sizeof(base), "/data/data/%s/cache/cmdaudit", pkg);
      }
      fclose(cmdline);
   }
   FILE *f = NULL;
   if (base[0]) {
      char name[512];
      snprintf(name, sizeof(name), "%s.%d.txt", base, (int) getpid());
      f = fopen(name, "w");
      if (!f)
         mesa_loge("cmdaudit: cannot open %s: %s", name, strerror(errno));
   }

   mesa_loge("cmdaudit: DUMP reason=%s submits=%" PRIu64 " bo_events=%" PRIu64 " live_bos=%u file=%s",
             reason, audit_submit_seq, audit_event_count, audit_live_count, f ? "yes" : "no");
   if (f)
      fprintf(f, "DUMP reason=%s submits=%" PRIu64 " bo_events=%" PRIu64 " live_bos=%u\n",
              reason, audit_submit_seq, audit_event_count, audit_live_count);

   const uint64_t nsub = MIN2(audit_submit_seq, (uint64_t) AUDIT_SUBMITS);
   for (uint64_t k = 0; k < nsub; k++) {
      const uint64_t seq = audit_submit_seq - 1 - k;
      const struct audit_submit *s = &audit_submits[seq % AUDIT_SUBMITS];
      uint32_t changed = 0, freed = 0, global = 0;
      for (uint32_t i = 0; i < s->ib2_count; i++) {
         const struct audit_ib2 *r = &s->ib2[i];
         if (!r->bo)
            continue;
         global += r->global;
         const struct audit_live *l = audit_live_find(r->iova, (uint64_t) r->size_dw * 4);
         if (!l || l->gem != r->gem || l->iova != r->bo_iova || !r->bo->map) {
            freed++;
            mesa_loge("cmdaudit:  seq=%" PRIu64 " IB2 0x%" PRIx64 " size=%u BO gem=%u freed since submit",
                      seq, r->iova, r->size_dw, r->gem);
            continue;
         }
         const uint32_t *ib2 =
            (const uint32_t *) ((const char *) r->bo->map + (r->iova - r->bo_iova));
         const uint64_t h = audit_hash(ib2, r->size_dw);
         if (h != r->hash) {
            changed++;
            mesa_loge("cmdaudit:  seq=%" PRIu64 " IB2 0x%" PRIx64 " size=%u CHANGED since submit"
                      " (hash %016" PRIx64 " -> %016" PRIx64 ", now malformed at %d)",
                      seq, r->iova, r->size_dw, r->hash, h, audit_check(ib2, r->size_dw));
         }
      }
      mesa_loge("cmdaudit: S seq=%" PRIu64 " ctx=%u ts=%u age=%" PRIu64 "us ib1=%u ib1_bad=%u ib2=%u"
                " dropped=%u unresolved=%u global=%u bad_at_submit=%u changed=%u freed=%u",
                seq, s->ctx, s->ts, (now - s->t_ns) / 1000, s->ib1_count, s->ib1_bad,
                s->ib2_count, s->ib2_dropped, s->ib2_unresolved, global, s->ib2_bad, changed, freed);
      if (f)
         fprintf(f, "S seq=%" PRIu64 " ctx=%u ts=%u age=%" PRIu64 "us ib1=%u ib1_bad=%u ib2=%u"
                 " dropped=%u unresolved=%u global=%u bad_at_submit=%u changed=%u freed=%u\n",
                 seq, s->ctx, s->ts, (now - s->t_ns) / 1000, s->ib1_count, s->ib1_bad,
                 s->ib2_count, s->ib2_dropped, s->ib2_unresolved, global, s->ib2_bad, changed, freed);
      /* Full IB2 list for the newest submissions, to match the KGSL snapshot. */
      if (k < 8) {
         char line[512];
         int len = 0;
         for (uint32_t i = 0; i < s->ib2_count; i++) {
            const struct audit_ib2 *r = &s->ib2[i];
            len += snprintf(line + len, sizeof(line) - len, " %" PRIx64 ":%u:%c",
                            r->iova, r->size_dw,
                            !r->bo ? 'u' : r->first_bad >= 0 ? 'B' : r->global ? 'g' : 'k');
            if (len > 440 || i + 1 == s->ib2_count) {
               mesa_loge("cmdaudit:  L seq=%" PRIu64 "%s", seq, line);
               if (f)
                  fprintf(f, " L seq=%" PRIu64 "%s\n", seq, line);
               len = 0;
            }
         }
      }
      if (k < AUDIT_CONTENT_SUBMITS) {
         const struct audit_content *c = &audit_contents[seq % AUDIT_CONTENT_SUBMITS];
         if (c->seq == seq)
            audit_dump_content(c, s, f, k < AUDIT_FILE_SUBMITS);
      }
   }

   /* BO lifetime events: everything touching the address ranges of the IB2
    * BOs of the newest submissions (whatever its age in the ring), plus the
    * most recent events.
    */
   uint64_t ranges[64][2];
   uint32_t range_count = 0;
   for (uint64_t k = 0; k < MIN2(nsub, (uint64_t) 8); k++) {
      const struct audit_submit *s = &audit_submits[(audit_submit_seq - 1 - k) % AUDIT_SUBMITS];
      for (uint32_t i = 0; i < s->ib2_count && range_count < ARRAY_SIZE(ranges); i++) {
         const struct audit_ib2 *r = &s->ib2[i];
         if (!r->bo)
            continue;
         bool known = false;
         for (uint32_t j = 0; j < range_count; j++)
            known |= ranges[j][0] == r->bo_iova;
         if (!known) {
            ranges[range_count][0] = r->bo_iova;
            ranges[range_count][1] = r->bo_iova + r->bo->size;
            range_count++;
         }
      }
   }
   const uint64_t nev = MIN2(audit_event_count, (uint64_t) AUDIT_BO_EVENTS);
   for (uint64_t k = nev; k > 0; k--) {
      const uint64_t idx = audit_event_count - k;
      const struct audit_bo_event *ev = &audit_events[idx % AUDIT_BO_EVENTS];
      bool overlap = false;
      for (uint32_t j = 0; j < range_count; j++)
         overlap |= ev->iova < ranges[j][1] && ranges[j][0] < ev->iova + ev->size;
      if (f)
         fprintf(f, "E%s %s t=-%" PRIu64 "us gem=%u iova=0x%" PRIx64 " size=0x%" PRIx64
                 " seq=%" PRIu64 " ctx=%u last_ts=%u retired=%u%s name=%s\n",
                 overlap ? "*" : " ", ev->alloc ? "alloc" : "free ", (now - ev->t_ns) / 1000,
                 ev->gem, ev->iova, ev->size, ev->submit_seq, ev->ctx, ev->last_ts,
                 ev->retired, ev->pending ? " PENDING" : "", ev->name);
      if (!overlap && k > 256)
         continue;
      mesa_loge("cmdaudit: E%s %s t=-%" PRIu64 "us gem=%u iova=0x%" PRIx64 " size=0x%" PRIx64
                " seq=%" PRIu64 " ctx=%u last_ts=%u retired=%u%s name=%s",
                overlap ? "*" : " ", ev->alloc ? "alloc" : "free ", (now - ev->t_ns) / 1000,
                ev->gem, ev->iova, ev->size, ev->submit_seq, ev->ctx, ev->last_ts,
                ev->retired, ev->pending ? " PENDING" : "", ev->name);
   }
   if (f) {
      for (uint32_t i = 0; i < audit_live_count; i++)
         fprintf(f, "B 0x%" PRIx64 " size=0x%" PRIx64 " gem=%u mapped=%d name=%s\n",
                 audit_lives[i].iova, audit_lives[i].size, audit_lives[i].gem,
                 audit_lives[i].bo->map != NULL, audit_lives[i].name);
      fclose(f);
   }
   mesa_loge("cmdaudit: DUMP end");
   simple_mtx_unlock(&audit_mtx);
}
