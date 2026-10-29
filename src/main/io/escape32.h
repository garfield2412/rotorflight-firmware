/* Experimental ESCape32 session contract. GPL-3.0-or-later. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESCAPE32_CONTRACT_VERSION 1
#define ESCAPE32_MAX_REQUEST 14
#define ESCAPE32_REPLY_SIZE 32
#define ESCAPE32_OWNER_TIMEOUT_MS 10000
#define ESCAPE32_CLI_TIMEOUT_MS 2000
#define ESCAPE32_KEY_COUNT 9

// Client must use a fresh nonzero nonce for each OPEN intent and retain it
// only for retries. Tokens are salted per boot, not authentication credentials.

typedef enum {
    ESC32_CAPS, ESC32_OPEN, ESC32_STATUS, ESC32_GET, ESC32_SET, ESC32_SAVE, ESC32_CLOSE
} escape32Operation_e;

typedef enum {
    ESC32_OK, ESC32_UNSUPPORTED, ESC32_BUSY, ESC32_INVALID_SESSION,
    ESC32_INVALID_REQUEST, ESC32_POWER_CYCLE_NEEDED, ESC32_TIMEOUT,
    ESC32_PROTOCOL_ERROR, ESC32_NORMALIZED, ESC32_OUTCOME_UNKNOWN,
    ESC32_VERIFY_FAILED, ESC32_RANGE, ESC32_STALE_REQUEST
} escape32Result_e;

typedef enum {
    ESC32_IDLE, ESC32_WAIT_POWER, ESC32_IDENTIFY, ESC32_READY, ESC32_READ,
    ESC32_WRITING, ESC32_SAVING, ESC32_VERIFY, ESC32_ERROR, ESC32_RESTART_REQUIRED
} escape32State_e;

enum {
    ESC32_READONLY = 1, ESC32_DIRTY = 2, ESC32_RESTART = 4,
    ESC32_VALUE_VALID = 8, ESC32_TERMINAL = 16
};

enum {
    ESC32_CORE_PRESENT = 1, ESC32_VALIDATED_CLI = 2,
    ESC32_CAN_READ = 4, ESC32_CAN_WRITE = 8
};

/*
 * Adapter boundary, NOT a board driver. There is deliberately no production
 * adapter yet: serial_escserial's KISS path is TX-only and its HAL timer restore
 * does not establish safe recovery. Setting USE_ESCAPE32_FORWARD_PROGRAMMING
 * exposes a core-only CAPS reply, never a claim of hardware support.
 *
 * A future validated adapter must own ALL relevant motor/timer/DMA resources,
 * maintain an arming inhibit, and implement nonblocking CLI entry/RX/cleanup.
 * No callback may block. acquire must fail without touching pins if unsafe.
 * acquire/release must flush old RX/TX data and detach stale callbacks: bytes
 * from a previous failed transaction must never enter the next session.
 * ready reports CLI transport readiness after the documented power-up window.
 * release returns true only if normal motor operation was positively recovered;
 * false retains the arming inhibit. No automatic override restoration.
 */
typedef struct {
    uint16_t validatedTargets;
    bool writable;
    bool (*safe)(void *ctx);
    bool (*acquire)(void *ctx, uint8_t target);
    bool (*ready)(void *ctx);
    bool (*write)(void *ctx, const char *data, size_t length);
    int (*read)(void *ctx); /* -1 when no byte is available */
    bool (*release)(void *ctx);
    void (*inhibit)(void *ctx, bool enabled);
} escape32Adapter_t;

void escape32Init(const escape32Adapter_t *adapter, void *context);
void escape32Process(uint32_t nowMs);
/* Always bounded: replyCapacity must be at least ESCAPE32_REPLY_SIZE. */
bool escape32Handle(uint32_t owner, const uint8_t *request, size_t requestLength,
    uint8_t *reply, size_t replyCapacity, uint32_t nowMs);
