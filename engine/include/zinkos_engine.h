#ifndef ZINKOS_ENGINE_H
#define ZINKOS_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZinkosEngine ZinkosEngine;

/// Create engine. target_ip must be null-terminated. Returns NULL on failure.
ZinkosEngine* zinkos_engine_create(const char* target_ip, uint16_t target_port);

/// Destroy engine. Safe to call with NULL.
void zinkos_engine_destroy(ZinkosEngine* engine);

/// Start engine. Returns 0 on success, -1 on failure.
int32_t zinkos_engine_start(ZinkosEngine* engine);

/// Stop engine. Returns 0 on success, -1 on failure.
int32_t zinkos_engine_stop(ZinkosEngine* engine);

/// Write interleaved S16LE frames. RT-safe (no alloc, no lock, no syscall).
/// Returns number of frames written.
uint32_t zinkos_engine_write_frames(ZinkosEngine* engine, const int16_t* data, uint32_t frame_count);

/// Get state: 0=Stopped, 1=Starting, 2=Running, 3=Stopping.
uint32_t zinkos_engine_get_state(ZinkosEngine* engine);

#ifdef __cplusplus
}
#endif

#endif // ZINKOS_ENGINE_H
