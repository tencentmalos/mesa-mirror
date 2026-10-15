/*
 * Copyright © 2026 shadPS4 contributors
 * SPDX-License-Identifier: MIT
 */

#include "tu_cmd_audit.h"

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
   const struct tu_bo *bo; /* containing BO, NULL if not owned by the cmdbuf */
   uint64_t bo_iova;
   uint32_t gem;
   uint32_t size_dw;
   int32_t first_bad;      /* dword index of the first malformed header, -1 if none */
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

static simple_mtx_t audit_mtx = SIMPLE_MTX_INITIALIZER;
static struct audit_submit *audit_submits;
static uint64_t audit_submit_seq;
static struct audit_bo_event *audit_events;
static uint64_t audit_event_count;
static uint32_t audit_live_reports;
static bool audit_dumped;

static bool
audit_init_locked(void)
{
   if (!audit_submits) {
      audit_submits = (struct audit_submit *)
         calloc(AUDIT_SUBMITS, sizeof(struct audit_submit));
      audit_events = (struct audit_bo_event *)
         calloc(AUDIT_BO_EVENTS, sizeof(struct audit_bo_event));
   }
   return audit_submits && audit_events;
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
 * CP_INDIRECT_BUFFER targets are reported through on_ib.
 */
template <typename F>
static int32_t
audit_walk(const uint32_t *dw, uint32_t count, F &&on_ib)
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

   struct audit_bo_ref refs[AUDIT_MAX_BOS];

   for (uint32_t c = 0; c < cmdbuf_count; c++) {
      struct tu_cmd_buffer *cmd = cmd_buffers[c];
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
         const uint32_t *ib1 =
            (const uint32_t *) ((const char *) entry->bo->map + entry->offset);
         const uint32_t ib1_dw = entry->size / 4;
         const int32_t bad1 = audit_walk(ib1, ib1_dw, [&](uint64_t iova, uint32_t size_dw) {
            if (s->ib2_count >= AUDIT_IB2_PER_SUB) {
               s->ib2_dropped++;
               return;
            }
            struct audit_ib2 *r = &s->ib2[s->ib2_count++];
            r->iova = iova;
            r->size_dw = size_dw;
            r->first_bad = -1;
            struct tu_bo *bo = audit_find(refs, ref_count, iova, (uint64_t) size_dw * 4);
            r->bo = bo;
            if (!bo) {
               s->ib2_unresolved++;
               return;
            }
            r->bo_iova = bo->iova;
            r->gem = bo->gem_handle;
            const uint32_t *ib2 =
               (const uint32_t *) ((const char *) bo->map + (iova - bo->iova));
            r->first_bad = audit_walk(ib2, size_dw, [](uint64_t, uint32_t) {});
            r->hash = audit_hash(ib2, size_dw);
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
         });
         if (bad1 >= 0) {
            s->ib1_bad++;
            if (audit_live_reports < 32) {
               audit_live_reports++;
               mesa_loge("cmdaudit: submit seq=%" PRIu64 " ctx=%u ts=%u IB1 0x%" PRIx64
                         " size=%u malformed at dword %d (0x%08x) at submit",
                         s->seq, ctx, ts, entry->bo->iova + entry->offset, ib1_dw,
                         bad1, ib1[bad1]);
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
   if (pending && audit_live_reports < 32) {
      audit_live_reports++;
      mesa_loge("cmdaudit: freeing BO gem=%u iova=0x%" PRIx64 " size=0x%" PRIx64
                " still referenced by ctx=%u ts=%u (retired %u)",
                bo->gem_handle, bo->iova, bo->size, bo->audit_ctx, bo->audit_ts, retired);
   }
   simple_mtx_unlock(&audit_mtx);
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

   mesa_loge("cmdaudit: DUMP reason=%s submits=%" PRIu64 " bo_events=%" PRIu64,
             reason, audit_submit_seq, audit_event_count);

   const uint64_t nsub = MIN2(audit_submit_seq, (uint64_t) AUDIT_SUBMITS);
   for (uint64_t k = 0; k < nsub; k++) {
      const uint64_t seq = audit_submit_seq - 1 - k;
      const struct audit_submit *s = &audit_submits[seq % AUDIT_SUBMITS];
      uint32_t changed = 0, freed = 0;
      for (uint32_t i = 0; i < s->ib2_count; i++) {
         const struct audit_ib2 *r = &s->ib2[i];
         if (!r->bo)
            continue;
         if (r->bo->gem_handle != r->gem || r->bo->iova != r->bo_iova || !r->bo->map) {
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
                      seq, r->iova, r->size_dw, r->hash, h,
                      audit_walk(ib2, r->size_dw, [](uint64_t, uint32_t) {}));
         }
      }
      mesa_loge("cmdaudit: S seq=%" PRIu64 " ctx=%u ts=%u age=%" PRIu64 "us ib1=%u ib1_bad=%u ib2=%u"
                " dropped=%u unresolved=%u bad_at_submit=%u changed=%u freed=%u",
                seq, s->ctx, s->ts, (now - s->t_ns) / 1000, s->ib1_count, s->ib1_bad,
                s->ib2_count, s->ib2_dropped, s->ib2_unresolved, s->ib2_bad, changed, freed);
      /* Full IB2 list for the newest submissions, to match the KGSL snapshot. */
      if (k < 8) {
         char line[512];
         int len = 0;
         for (uint32_t i = 0; i < s->ib2_count; i++) {
            const struct audit_ib2 *r = &s->ib2[i];
            len += snprintf(line + len, sizeof(line) - len, " %" PRIx64 ":%u:%c",
                            r->iova, r->size_dw,
                            !r->bo ? 'u' : r->first_bad >= 0 ? 'B' : 'k');
            if (len > 440 || i + 1 == s->ib2_count) {
               mesa_loge("cmdaudit:  L seq=%" PRIu64 "%s", seq, line);
               len = 0;
            }
         }
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
      if (!overlap && k > 256)
         continue;
      mesa_loge("cmdaudit: E%s %s t=-%" PRIu64 "us gem=%u iova=0x%" PRIx64 " size=0x%" PRIx64
                " seq=%" PRIu64 " ctx=%u last_ts=%u retired=%u%s name=%s",
                overlap ? "*" : " ", ev->alloc ? "alloc" : "free ", (now - ev->t_ns) / 1000,
                ev->gem, ev->iova, ev->size, ev->submit_seq, ev->ctx, ev->last_ts,
                ev->retired, ev->pending ? " PENDING" : "", ev->name);
   }
   mesa_loge("cmdaudit: DUMP end");
   simple_mtx_unlock(&audit_mtx);
}
