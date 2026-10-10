#ifndef TU_QUEUE_SCOPE_H
#define TU_QUEUE_SCOPE_H

#include "compiler/shader_enums.h"

static inline mesa_scope
tu_acquire_release_max_scope(bool multiple_real_queues)
{
   return multiple_real_queues ? SCOPE_WORKGROUP : SCOPE_QUEUE_FAMILY;
}

#endif
