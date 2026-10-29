/* Standalone host harness: no ARM hardware headers or third-party test runner. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/escape32.h"

typedef struct {
    bool safe, ready, recover, inhibited, autoReply, normalize;
    unsigned acquired, released, writes, saves;
    char rx[2048];
    size_t head, tail;
    char lastCommand[64];
    int32_t values[ESCAPE32_KEY_COUNT];
    const char *identity;
} fake_t;

static const char *keys[] = {
    "revdir", "timing", "freq_min", "freq_max", "duty_spup", "duty_ramp", "duty_rate", "prot_temp", "prot_curr"
};
static uint8_t reply[ESCAPE32_REPLY_SIZE];
static uint8_t request[ESCAPE32_MAX_REQUEST];
static uint32_t token;
static uint32_t now;
static fake_t fake;

static void put16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void put32(uint8_t *p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void feed(fake_t *f, const char *s)
{
    const size_t n = strlen(s);
    assert(f->head + n < sizeof(f->rx));
    memcpy(f->rx + f->head, s, n);
    f->head += n;
}

static bool safe(void *ctx) { return ((fake_t *)ctx)->safe; }
static bool acquire(void *ctx, uint8_t target)
{
    fake_t *f = ctx;
    assert(target == 0);
    assert(f->inhibited);
    ++f->acquired;
    return true;
}
static bool ready(void *ctx) { return ((fake_t *)ctx)->ready; }
static bool release(void *ctx) { fake_t *f = ctx; ++f->released; return f->recover; }
static void inhibit(void *ctx, bool enabled) { ((fake_t *)ctx)->inhibited = enabled; }
static int readByte(void *ctx)
{
    fake_t *f = ctx;
    if (f->tail == f->head) {
        f->tail = f->head = 0;
        return -1;
    }
    return (unsigned char)f->rx[f->tail++];
}
static bool writeBytes(void *ctx, const char *s, size_t n)
{
    fake_t *f = ctx;
    assert(n < sizeof(f->lastCommand));
    memcpy(f->lastCommand, s, n);
    f->lastCommand[n] = 0;
    ++f->writes;
    if (!strcmp(s, "save\n")) {
        ++f->saves;
    }
    if (!f->autoReply) {
        return true;
    }
    if (!strcmp(s, "info\n")) {
        feed(f, f->identity);
        feed(f, "\nTemp: 22C\nVolt: 12.00V\nCurr: 0.00A\nCsum: 0mAh\nERPM: 0\nOK\n");
    } else if (!strcmp(s, "save\n")) {
        feed(f, "OK\n");
    } else {
        for (unsigned key = 0; key < ESCAPE32_KEY_COUNT; ++key) {
            char cmd[64], response[64];
            snprintf(cmd, sizeof(cmd), "get %s\n", keys[key]);
            if (!strcmp(s, cmd)) {
                snprintf(response, sizeof(response), "%s: %ld\nOK\n", keys[key], (long)f->values[key]);
                feed(f, response);
                return true;
            }
            snprintf(cmd, sizeof(cmd), "set %s ", keys[key]);
            if (!strncmp(s, cmd, strlen(cmd))) {
                const int32_t value = (int32_t)strtol(s + strlen(cmd), NULL, 10);
                f->values[key] = f->normalize && key == 1 ? 16 : value;
                snprintf(response, sizeof(response), "%s: %ld\nOK\n", keys[key], (long)f->values[key]);
                feed(f, response);
                return true;
            }
        }
        assert(!"Unexpected CLI command");
    }
    return true;
}

static escape32Adapter_t adapter = {
    .validatedTargets = 1, .writable = true, .safe = safe, .acquire = acquire,
    .ready = ready, .write = writeBytes, .read = readByte, .release = release, .inhibit = inhibit
};

static void reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.safe = fake.ready = fake.recover = fake.autoReply = true;
    fake.identity = "ESCape32 rev17.2 [TEST]";
    const int32_t initial[] = {0, 16, 24, 48, 15, 0, 30, 0, 0};
    memcpy(fake.values, initial, sizeof(initial));
    escape32Init(&adapter, &fake);
    now = 100;
    token = 0;
}

static void call(size_t n)
{
    assert(escape32Handle(7, request, n, reply, sizeof(reply), now));
}

static void header(uint8_t op, uint16_t id)
{
    memset(request, 0, sizeof(request));
    request[0] = 1;
    request[1] = op;
    put32(request + 2, token);
    put16(request + 6, id);
}

static void status(void)
{
    header(ESC32_STATUS, 0);
    call(8);
}

static void pump(void)
{
    for (unsigned i = 0; i < 40; ++i) {
        escape32Process(++now);
    }
    status();
}

static void openSession(void)
{
    header(ESC32_OPEN, 1);
    request[8] = 0;
    request[9] = 1;
    put32(request + 10, 0x12345678);
    call(14);
    assert(reply[2] == ESC32_BUSY && reply[3] == ESC32_WAIT_POWER);
    token = get32(reply + 4);
    assert(token && fake.acquired == 1 && fake.inhibited);
    pump();
    assert(reply[2] == ESC32_OK && reply[3] == ESC32_READY && reply[15] == 1);
}

static void closeSession(uint16_t id)
{
    header(ESC32_CLOSE, id);
    call(8);
    assert(reply[2] == ESC32_OK && reply[3] == ESC32_IDLE && !fake.inhibited);
}

static void testDefaults(void)
{
    escape32Init(NULL, NULL);
    header(ESC32_CAPS, 0);
    call(2);
    assert(reply[0] == 1 && reply[2] == ESC32_OK && reply[10] == 255);
    assert(reply[11] == 17 && reply[26] == 1 && reply[28] == 0 && reply[20] == 0);
    assert(reply[21] == 14 && reply[22] == 32 && reply[24] == 16 && reply[25] == 39);
    call(3);
    assert(reply[2] == ESC32_INVALID_REQUEST);
    request[0] = 2;
    call(2);
    assert(reply[2] == ESC32_INVALID_REQUEST);
    assert(!escape32Handle(7, request, 2, reply, 31, now));
    assert(escape32Handle(7, NULL, 0, reply, sizeof(reply), now));
    assert(reply[2] == ESC32_INVALID_REQUEST);
}

static void testSession(void)
{
    reset();
    openSession();
    const unsigned writes = fake.writes;
    header(ESC32_OPEN, 1);
    put32(request + 2, 0);
    request[9] = 1;
    put32(request + 10, 0x12345678);
    call(14);
    assert(get32(reply + 4) == token && fake.acquired == 1 && fake.writes == writes);
    header(ESC32_GET, 2);
    request[8] = 1;
    call(9);
    assert(reply[2] == ESC32_BUSY && !(reply[11] & ESC32_TERMINAL));
    call(9);
    assert(fake.writes == writes + 1);
    request[8] = 2;
    call(9);
    assert(reply[2] == ESC32_INVALID_REQUEST);
    pump();
    assert(reply[2] == ESC32_OK && get32(reply + 16) == 16 && (reply[11] & ESC32_VALUE_VALID));
    status();
    assert(get32(reply + 16) == 16);
    header(ESC32_SET, 3);
    request[8] = 1;
    put32(request + 9, 18);
    fake.normalize = true;
    call(13);
    pump();
    assert(reply[2] == ESC32_NORMALIZED && get32(reply + 16) == 16 && (reply[11] & ESC32_DIRTY));
    assert(fake.writes == writes + 11); /* GET + SET + all nine dependent reads */
    header(ESC32_SAVE, 4);
    call(8);
    call(8);
    assert(fake.saves == 1);
    pump();
    assert(reply[2] == ESC32_OK && !(reply[11] & ESC32_DIRTY));
    header(ESC32_SAVE, 4);
    call(8);
    assert(fake.saves == 1);
    header(ESC32_GET, 3);
    call(9);
    assert(reply[2] == ESC32_STALE_REQUEST);
    header(ESC32_STATUS, 0);
    put32(request + 2, token + 1);
    call(8);
    assert(reply[2] == ESC32_INVALID_SESSION);
    header(ESC32_STATUS, 0);
    assert(escape32Handle(8, request, 8, reply, sizeof(reply), now));
    assert(reply[2] == ESC32_INVALID_SESSION);
    closeSession(5);
    call(8);
    assert(fake.released == 1);
    header(ESC32_OPEN, 1);
    put32(request + 2, 0);
    request[9] = 1;
    put32(request + 10, 0x12345678);
    call(14);
    assert(fake.acquired == 1 && reply[3] == ESC32_IDLE);
    const uint32_t oldToken = token;
    put32(request + 10, 0x87654321);
    call(14);
    token = get32(reply + 4);
    assert(token && token != oldToken && fake.acquired == 2);
    closeSession(2);
}

static void testInvalidAndWatchdog(void)
{
    reset();
    fake.safe = false;
    header(ESC32_OPEN, 1);
    request[9] = 1;
    put32(request + 10, 1);
    call(14);
    assert(reply[2] == ESC32_BUSY && fake.acquired == 0);
    fake.safe = true;
    openSession();
    header(ESC32_SET, 2);
    request[8] = 1;
    put32(request + 9, 32);
    const unsigned writes = fake.writes;
    call(13);
    assert(reply[2] == ESC32_RANGE && fake.writes == writes);
    /* CAPS by any owner must not refresh the session watchdog. */
    now += 9999;
    header(ESC32_CAPS, 0);
    call(2);
    now += 2;
    escape32Process(now);
    status();
    assert(reply[2] == ESC32_TIMEOUT && fake.released == 1);
    closeSession(2); /* released tombstone still closable */
}

static void testParser(void)
{
    reset();
    openSession();
    fake.autoReply = false;
    header(ESC32_GET, 2);
    request[8] = 1;
    call(9);
    feed(&fake, "tim");
    escape32Process(++now);
    status();
    assert(reply[2] == ESC32_BUSY);
    feed(&fake, "ing: 16\nO");
    escape32Process(++now);
    status();
    assert(reply[2] == ESC32_BUSY);
    feed(&fake, "K\n");
    pump();
    assert(reply[2] == ESC32_OK && get32(reply + 16) == 16);
    header(ESC32_GET, 3);
    request[8] = 1;
    call(9);
    feed(&fake, "timing: 999999999999999999999999999999999999\nOK\n");
    pump();
    assert(reply[2] == ESC32_PROTOCOL_ERROR && fake.released == 1);
    closeSession(4);

    reset();
    fake.identity = "ESCape32 rev99.0 [UNKNOWN]";
    header(ESC32_OPEN, 1);
    request[9] = 1;
    put32(request + 10, 2);
    call(14);
    token = get32(reply + 4);
    pump();
    assert(reply[15] == 0 && (reply[11] & ESC32_READONLY));
    header(ESC32_GET, 2);
    call(9);
    assert(reply[2] == ESC32_UNSUPPORTED);
    closeSession(2);
}

static void testSafetyAndUnknownOutcome(void)
{
    reset();
    openSession();
    const unsigned writes = fake.writes;
    fake.safe = false;
    header(ESC32_SET, 2);
    request[8] = 1;
    put32(request + 9, 18);
    call(13);
    assert(reply[2] == ESC32_BUSY && fake.writes == writes && fake.released == 1);
    closeSession(2);

    reset();
    openSession();
    fake.autoReply = false;
    header(ESC32_SAVE, 2);
    call(8);
    now += ESCAPE32_CLI_TIMEOUT_MS;
    escape32Process(now);
    status();
    assert(reply[2] == ESC32_OUTCOME_UNKNOWN && fake.saves == 1 && fake.released == 1);
    header(ESC32_SAVE, 2);
    call(8);
    assert(reply[2] == ESC32_OUTCOME_UNKNOWN && fake.saves == 1);
    closeSession(3);
}

static void testRestartRequired(void)
{
    reset();
    openSession();
    fake.recover = false;
    header(ESC32_CLOSE, 2);
    call(8);
    assert(reply[2] == ESC32_POWER_CYCLE_NEEDED && reply[3] == ESC32_RESTART_REQUIRED);
    assert(fake.inhibited && (reply[11] & ESC32_RESTART));
    call(8);
    assert(fake.released == 1);
    header(ESC32_OPEN, 1);
    put32(request + 2, 0);
    request[9] = 1;
    put32(request + 10, 3);
    call(14);
    assert(reply[2] == ESC32_BUSY && fake.acquired == 1);
}

int main(void)
{
    testDefaults();
    testSession();
    testInvalidAndWatchdog();
    testParser();
    testSafetyAndUnknownOutcome();
    testRestartRequired();
    puts("ESCape32 portable session tests passed");
    return 0;
}
