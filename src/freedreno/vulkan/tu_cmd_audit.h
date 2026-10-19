/*
 * Copyright © 2026 shadPS4 contributors
 * SPDX-License-Identifier: MIT
 *
 * TU_DEBUG=cmdaudit: record what each KGSL submission referenced at submit
 * time (IB2 address, size, PM4 header validity, content hash) and the
 * lifetime of every BO, then dump it to the log once when the device is
 * lost. Used to tell apart "the command stream was not complete or changed
 * after submit" from "the CP read something other than what was written".
 */

#ifndef TU_CMD_AUDIT_H
#define TU_CMD_AUDIT_H

#include "tu_common.h"

struct tu_bo;
struct tu_cmd_buffer;
struct tu_device;
struct tu_queue;

void tu_cmd_audit_submit(struct tu_queue *queue,
                         struct tu_cmd_buffer **cmd_buffers,
                         uint32_t cmdbuf_count);

/* retired/pending are only meaningful for frees of BOs that a recorded
 * submission referenced (audit_ctx != 0). */
void tu_cmd_audit_bo_event(const struct tu_bo *bo, bool alloc,
                           const char *name, uint32_t retired, bool pending);

void tu_cmd_audit_dump(const char *reason);

#endif /* TU_CMD_AUDIT_H */
