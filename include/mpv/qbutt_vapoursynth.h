/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * mpv is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 */

#ifndef MPV_QBUTT_VAPOURSYNTH_H_
#define MPV_QBUTT_VAPOURSYNTH_H_

#include <stddef.h>

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VSAPI VSAPI;
typedef struct VSCore VSCore;
typedef struct VSNode VSNode;

/**
 * Construct the output of the vapoursynth-native video filter.
 *
 * The filter calls this function on mpv's filter thread. api, core, input and
 * config are borrowed for this call. Return one owning video-node reference
 * from the supplied core, or NULL with a NUL-terminated UTF-8 error message in
 * error (at most error_size bytes, including the terminator).
 *
 * Do not request frames during construction. Nodes and native functions must
 * retain their dependencies and release their state through VapourSynth's
 * destruction callbacks. The filter drains all asynchronous frame requests
 * before releasing the graph and core on reset, seek, EOF or destruction.
 */
typedef VSNode *(*mpv_qbutt_vapoursynth_graph_fn)(
    const VSAPI *api, VSCore *core, VSNode *input, const char *config,
    char *error, size_t error_size);

/**
 * Register the graph constructor for this mpv core exactly once, before
 * mpv_initialize(). The function must remain valid until core termination
 * completes. Registration is shared by handles belonging to this core and
 * does not affect other cores.
 *
 * The vapoursynth-native filter's file option identifies the native
 * VapourSynth core library. user-data is passed unchanged as config.
 *
 * @return 0 on success; MPV_ERROR_INVALID_PARAMETER for a NULL constructor,
 *         repeated registration or an initialized core;
 *         MPV_ERROR_UNSUPPORTED if native VapourSynth support is disabled.
 *
 * This extension belongs to qbutt's pinned mpv engine, not the upstream API.
 */
MPV_EXPORT int mpv_qbutt_set_vapoursynth_graph_factory(
    mpv_handle *ctx, mpv_qbutt_vapoursynth_graph_fn factory);

#ifdef __cplusplus
}
#endif

#endif
