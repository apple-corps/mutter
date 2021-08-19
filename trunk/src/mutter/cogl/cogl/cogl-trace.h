/*
 * Copyright 2018 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef COGL_TRACE_H
#define COGL_TRACE_H

#include <glib.h>
#include <stdint.h>
#include <errno.h>

#include "cogl/cogl-defines.h"
#include "cogl/cogl-macros.h"

#ifdef COGL_HAS_TRACING

typedef struct _CoglTraceContext CoglTraceContext;

typedef struct _CoglTraceHead
{
  uint64_t begin_time;
  const char *name;
} CoglTraceHead;

COGL_EXPORT
GPrivate cogl_trace_thread_data;
COGL_EXPORT
CoglTraceContext *cogl_trace_context;
COGL_EXPORT
GMutex cogl_trace_mutex;

COGL_EXPORT void
cogl_set_tracing_enabled_on_thread_with_fd (GMainContext *main_context,
                                            const char   *group,
                                            int           fd);

COGL_EXPORT void
cogl_set_tracing_enabled_on_thread (GMainContext *main_context,
                                    const char   *group,
                                    const char   *filename);

COGL_EXPORT void
cogl_set_tracing_disabled_on_thread (GMainContext *main_context);

static inline void
cogl_trace_begin (CoglTraceHead *head,
                  const char    *name)
{
  head->begin_time = g_get_monotonic_time () * 1000;
  head->name = name;
}

COGL_EXPORT void
cogl_trace_end (CoglTraceHead *head);

static inline void
cogl_auto_trace_end_helper (CoglTraceHead **head)
{
  if (*head)
    cogl_trace_end (*head);
}

#define COGL_TRACE_BEGIN(Name, description) \
  CoglTraceHead CoglTrace##Name = { 0 }; \
  if (g_private_get (&cogl_trace_thread_data)) \
    cogl_trace_begin (&CoglTrace##Name, description); \

#define COGL_TRACE_END(Name)\
  if (g_private_get (&cogl_trace_thread_data)) \
    cogl_trace_end (&CoglTrace##Name);

#define COGL_TRACE_BEGIN_SCOPED(Name, description) \
  CoglTraceHead CoglTrace##Name = { 0 }; \
  __attribute__((cleanup (cogl_auto_trace_end_helper))) \
    CoglTraceHead *ScopedCoglTrace##Name = NULL; \
  if (g_private_get (&cogl_trace_thread_data)) \
    { \
      cogl_trace_begin (&CoglTrace##Name, description); \
      ScopedCoglTrace##Name = &CoglTrace##Name; \
    }

#else /* COGL_HAS_TRACING */

#include <stdio.h>

#define COGL_TRACE_BEGIN(Name, description) (void) 0
#define COGL_TRACE_END(Name) (void) 0
#define COGL_TRACE_BEGIN_SCOPED(Name, description) (void) 0

COGL_EXPORT void
cogl_set_tracing_enabled_on_thread_with_fd (void       *data,
                                            const char *group,
                                            int         fd);
COGL_EXPORT void
cogl_set_tracing_enabled_on_thread (void       *data,
                                    const char *group,
                                    const char *filename);
COGL_EXPORT void
cogl_set_tracing_disabled_on_thread (void *data);

#endif /* COGL_HAS_TRACING */

#endif /* COGL_TRACE_H */
