/* Experimental, bounded ESCape32 CLI session engine. GPL-3.0-or-later. */
#include "escape32.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    int32_t min;
    int32_t max;
} setting_t;

/* Raw integers from ESCape32 17.3 commit
 * 1d718c143380c3eb7e6581478990e1496632e5b8, byte-identical to 17.2 at
 * 734097d2825deb49328342335e931ec18ce5823d. Shared prog.c blob:
 * 21885a08a0d0d1e6c022e8db91d1685a24589127; checkcfg/util.c blob:
 * d450d1f4a9a569701914ec327666b05111d38d43. Never AM32 offsets. */
static const setting_t settings[ESCAPE32_KEY_COUNT] = {
    {"revdir", 0, 1}, {"timing", 1, 31}, {"freq_min", 16, 48},
    {"freq_max", 16, 96}, {"duty_spup", 1, 100}, {"duty_ramp", 0, 100},
    {"duty_rate", 1, 100}, {"prot_temp", 0, 140}, {"prot_curr", 0, 999}
};

enum { LINE_SIZE = 80, FRAME_LIMIT = 512, RX_BUDGET = 64 };

static struct {
    const escape32Adapter_t *adapter;
    void *context;
    bool active;
    bool inhibited;
    bool entered;
    bool dirty;
    bool terminal;
    bool valueValid;
    uint32_t owner;
    uint32_t token;
    uint32_t generation;
    uint32_t bootSeed;
    uint32_t nonce;
    uint32_t heartbeat;
    uint32_t transactionStart;
    uint16_t openRequest;
    uint16_t requestId;
    uint8_t target;
    uint8_t operation;
    uint8_t state;
    uint8_t result;
    uint8_t key;
    uint8_t revision;
    uint8_t patch;
    uint8_t schema;
    int32_t requestedValue;
    int32_t value;
    int32_t values[ESCAPE32_KEY_COUNT];
    uint8_t verifyKey;
    uint8_t lastRequest[ESCAPE32_MAX_REQUEST];
    size_t lastRequestLength;
    char line[LINE_SIZE];
    size_t lineLength;
    size_t frameLength;
    bool gotValue;
    bool gotIdentity;
    int32_t parsedValue;
} session;

static uint16_t read16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const uint8_t *p)
{
    return read16(p) | (uint32_t)read16(p + 2) << 16;
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void write32(uint8_t *p, uint32_t value)
{
    write16(p, value);
    write16(p + 2, value >> 16);
}

static bool elapsed(uint32_t now, uint32_t start, uint32_t duration)
{
    return now - start >= duration;
}

static bool adapterValid(void)
{
    const escape32Adapter_t *a = session.adapter;
    return a && a->validatedTargets && a->safe && a->acquire && a->ready &&
        a->write && a->read && a->release && a->inhibit;
}

static bool writable(void)
{
    return adapterValid() && session.adapter->writable && session.schema == 1;
}

static uint16_t capabilities(void)
{
    return ESC32_CORE_PRESENT | (adapterValid() ? ESC32_VALIDATED_CLI | ESC32_CAN_READ |
        (session.adapter->writable ? ESC32_CAN_WRITE : 0) : 0);
}

static void replyBase(uint8_t *reply, uint8_t operation, uint8_t result)
{
    memset(reply, 0, ESCAPE32_REPLY_SIZE);
    reply[0] = ESCAPE32_CONTRACT_VERSION;
    reply[1] = operation;
    reply[2] = result;
    reply[10] = 0xFF;
    reply[11] = ESC32_READONLY | ESC32_TERMINAL;
    reply[12] = 0xFF;
    reply[20] = adapterValid() ? 1 : 0;
    reply[21] = ESCAPE32_MAX_REQUEST;
    reply[22] = ESCAPE32_REPLY_SIZE;
    write16(reply + 24, ESCAPE32_OWNER_TIMEOUT_MS);
    write16(reply + 26, capabilities());
    write16(reply + 28, adapterValid() ? session.adapter->validatedTargets : 0);
}

static void replySnapshot(uint8_t *reply, uint8_t operation)
{
    replyBase(reply, operation, session.result);
    reply[3] = session.state;
    write32(reply + 4, session.token);
    write16(reply + 8, session.requestId);
    reply[10] = session.target;
    reply[11] = (session.active && writable() ? 0 : ESC32_READONLY) |
        (session.dirty ? ESC32_DIRTY : 0) |
        (session.inhibited && !session.active ? ESC32_RESTART : 0) |
        (session.valueValid ? ESC32_VALUE_VALID : 0) |
        (session.terminal ? ESC32_TERMINAL : 0);
    reply[12] = session.key;
    reply[13] = session.revision;
    reply[14] = session.patch;
    reply[15] = session.schema;
    write32(reply + 16, (uint32_t)session.value);
}

static void releaseSession(uint8_t result)
{
    if (session.active) {
        const bool recovered = session.adapter->release(session.context);
        session.active = false;
        if (recovered) {
            session.adapter->inhibit(session.context, false);
            session.inhibited = false;
        }
    }
    session.terminal = true;
    session.valueValid = false;
    session.state = session.inhibited ? ESC32_RESTART_REQUIRED : ESC32_IDLE;
    session.result = session.inhibited && result == ESC32_OK ? ESC32_POWER_CYCLE_NEEDED : result;
}

void escape32Init(const escape32Adapter_t *adapter, void *context)
{
    /* Startup/test configuration only. Never reinitialize an owned live line. */
    if (session.active || session.inhibited) {
        return;
    }
    const uint32_t generation = session.generation;
    const uint32_t bootSeed = session.bootSeed;
    memset(&session, 0, sizeof(session));
    session.generation = generation;
    session.bootSeed = bootSeed;
    session.adapter = adapter;
    session.context = context;
    session.target = 0xFF;
    session.key = 0xFF;
    session.terminal = true;
}

static void resetParser(uint32_t now)
{
    session.lineLength = 0;
    session.frameLength = 0;
    session.gotValue = false;
    session.gotIdentity = false;
    session.transactionStart = now;
}

static bool command(const char *data, uint32_t now)
{
    resetParser(now);
    return session.adapter->write(session.context, data, strlen(data));
}

static void fail(uint8_t result)
{
    /* No more CLI traffic after framing/timeout/unknown write outcome. */
    if ((session.state == ESC32_WRITING || session.state == ESC32_SAVING || session.state == ESC32_VERIFY) &&
        (result == ESC32_PROTOCOL_ERROR || result == ESC32_TIMEOUT)) {
        result = ESC32_OUTCOME_UNKNOWN;
    }
    releaseSession(result);
}

static bool getKey(uint8_t key, uint32_t now)
{
    char tx[32];
    const int len = snprintf(tx, sizeof(tx), "get %s\n", settings[key].name);
    return len > 0 && (size_t)len < sizeof(tx) && command(tx, now);
}

/* Decimal parser rejects overflow before multiply, whitespace and suffixes. */
static bool parseInteger(const char *text, int32_t *value)
{
    bool negative = false;
    if (*text == '-') {
        negative = true;
        ++text;
    }
    if (*text < '0' || *text > '9') {
        return false;
    }
    uint32_t n = 0;
    const uint32_t limit = negative ? (uint32_t)INT32_MAX + 1u : INT32_MAX;
    while (*text >= '0' && *text <= '9') {
        const uint8_t digit = *text++ - '0';
        if (n > (limit - digit) / 10) {
            return false;
        }
        n = n * 10 + digit;
    }
    if (*text) {
        return false;
    }
    *value = negative ? (int32_t)(0u - n) : (int32_t)n;
    return true;
}

static bool parseIdentity(const char *line)
{
    static const char prefix[] = "ESCape32 rev";
    if (strncmp(line, prefix, sizeof(prefix) - 1)) {
        return false;
    }
    const char *p = line + sizeof(prefix) - 1;
    unsigned revision = 0, patch = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        revision = revision * 10 + (*p++ - '0');
        if (revision > 255) {
            return false;
        }
    }
    if (*p++ != '.' || *p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        patch = patch * 10 + (*p++ - '0');
        if (patch > 255) {
            return false;
        }
    }
    if (p[0] != ' ' || p[1] != '[' || !p[2]) {
        return false;
    }
    const char *end = strchr(p + 2, ']');
    if (!end || end == p + 2 || end[1]) {
        return false;
    }
    session.revision = revision;
    session.patch = patch;
    session.schema = revision == 17 && (patch == 2 || patch == 3) ? 1 : 0;
    return true;
}

static void completed(uint32_t now)
{
    switch (session.state) {
    case ESC32_IDENTIFY:
        if (!session.gotIdentity) {
            fail(ESC32_PROTOCOL_ERROR);
            return;
        }
        session.state = ESC32_READY;
        session.result = ESC32_OK;
        session.terminal = true;
        return;
    case ESC32_READ:
        if (!session.gotValue) {
            fail(ESC32_PROTOCOL_ERROR);
            return;
        }
        session.value = session.parsedValue;
        session.valueValid = true;
        session.state = ESC32_READY;
        session.result = ESC32_OK;
        session.terminal = true;
        return;
    case ESC32_WRITING:
        if (!session.gotValue) {
            fail(ESC32_OUTCOME_UNKNOWN);
            return;
        }
        session.dirty = true;
        session.state = ESC32_VERIFY;
        session.verifyKey = 0;
        if (!getKey(0, now)) {
            fail(ESC32_OUTCOME_UNKNOWN);
        }
        return;
    case ESC32_SAVING:
        /* CLI save completion is not proof of persistence after power loss. */
        session.state = ESC32_READY;
        session.result = ESC32_OK;
        session.terminal = true;
        session.dirty = false;
        return;
    case ESC32_VERIFY:
        if (!session.gotValue) {
            fail(ESC32_VERIFY_FAILED);
            return;
        }
        session.values[session.verifyKey] = session.parsedValue;
        if (++session.verifyKey < ESCAPE32_KEY_COUNT) {
            if (!getKey(session.verifyKey, now)) {
                fail(ESC32_OUTCOME_UNKNOWN);
            }
            return;
        }
        session.value = session.values[session.key];
        if (session.values[2] > session.values[3]) {
            fail(ESC32_VERIFY_FAILED);
            return;
        }
        session.valueValid = true;
        session.state = ESC32_READY;
        session.result = session.value == session.requestedValue ? ESC32_OK : ESC32_NORMALIZED;
        session.terminal = true;
        return;
    default:
        fail(ESC32_PROTOCOL_ERROR);
    }
}

static void lineReceived(uint32_t now)
{
    session.line[session.lineLength] = 0;
    if (!strcmp(session.line, "ERROR")) {
        fail(session.state == ESC32_WRITING || session.state == ESC32_SAVING ?
            ESC32_OUTCOME_UNKNOWN : ESC32_PROTOCOL_ERROR);
    } else if (!strcmp(session.line, "OK")) {
        completed(now);
    } else if (session.state == ESC32_IDENTIFY) {
        if (!strncmp(session.line, "ESCape32", 8)) {
            if (session.gotIdentity || !parseIdentity(session.line)) {
                fail(ESC32_PROTOCOL_ERROR);
            } else {
                session.gotIdentity = true;
            }
        } else if (strncmp(session.line, "Temp: ", 6) && strncmp(session.line, "Volt: ", 6) &&
            strncmp(session.line, "Curr: ", 6) && strncmp(session.line, "Csum: ", 6) &&
            strncmp(session.line, "ERPM: ", 6)) {
            fail(ESC32_PROTOCOL_ERROR);
        }
    } else if (session.state == ESC32_READ || session.state == ESC32_WRITING || session.state == ESC32_VERIFY) {
        const uint8_t key = session.state == ESC32_VERIFY ? session.verifyKey : session.key;
        const size_t n = strlen(settings[key].name);
        if (session.gotValue || strncmp(session.line, settings[key].name, n) ||
            session.line[n] != ':' || session.line[n + 1] != ' ' ||
            !parseInteger(session.line + n + 2, &session.parsedValue) ||
            session.parsedValue < settings[key].min || session.parsedValue > settings[key].max) {
            fail(ESC32_PROTOCOL_ERROR);
        } else {
            session.gotValue = true;
        }
    } else {
        fail(ESC32_PROTOCOL_ERROR);
    }
    session.lineLength = 0;
}

void escape32Process(uint32_t now)
{
    if (!session.active) {
        return;
    }
    if (!session.adapter->safe(session.context)) {
        fail(ESC32_BUSY);
        return;
    }
    if (elapsed(now, session.heartbeat, ESCAPE32_OWNER_TIMEOUT_MS)) {
        fail(session.state == ESC32_WRITING || session.state == ESC32_SAVING ||
            session.state == ESC32_VERIFY ? ESC32_OUTCOME_UNKNOWN : ESC32_TIMEOUT);
        return;
    }
    if (session.state == ESC32_WAIT_POWER) {
        if (session.adapter->ready(session.context)) {
            session.entered = true;
            session.state = ESC32_IDENTIFY;
            if (!command("info\n", now)) {
                fail(ESC32_PROTOCOL_ERROR);
            }
        } else if (elapsed(now, session.transactionStart, ESCAPE32_OWNER_TIMEOUT_MS)) {
            fail(ESC32_TIMEOUT);
        }
        return;
    }
    if (session.state == ESC32_READY) {
        return;
    }
    if (elapsed(now, session.transactionStart, ESCAPE32_CLI_TIMEOUT_MS)) {
        fail(session.state == ESC32_WRITING || session.state == ESC32_SAVING ||
            session.state == ESC32_VERIFY ? ESC32_OUTCOME_UNKNOWN : ESC32_TIMEOUT);
        return;
    }
    for (unsigned i = 0; i < RX_BUDGET && session.active && session.state != ESC32_READY; ++i) {
        const int ch = session.adapter->read(session.context);
        if (ch < 0) {
            break;
        }
        if (++session.frameLength > FRAME_LIMIT || ch > 127 || ch == 0 || ch == '\r') {
            fail(ESC32_PROTOCOL_ERROR);
            break;
        }
        if (ch == '\n') {
            lineReceived(now);
        } else if (session.lineLength + 1 < sizeof(session.line)) {
            session.line[session.lineLength++] = (char)ch;
        } else {
            fail(ESC32_PROTOCOL_ERROR);
        }
    }
}

static size_t expectedLength(uint8_t operation)
{
    switch (operation) {
    case ESC32_CAPS: return 2;
    case ESC32_OPEN: return 14;
    case ESC32_GET: return 9;
    case ESC32_SET: return 13;
    case ESC32_STATUS:
    case ESC32_SAVE:
    case ESC32_CLOSE: return 8;
    default: return 0;
    }
}

static void remember(const uint8_t *request, size_t length)
{
    memcpy(session.lastRequest, request, length);
    session.lastRequestLength = length;
    session.requestId = read16(request + 6);
    session.operation = request[1];
}

bool escape32Handle(uint32_t owner, const uint8_t *request, size_t length,
    uint8_t *reply, size_t capacity, uint32_t now)
{
    if (!reply || capacity < ESCAPE32_REPLY_SIZE) {
        return false;
    }
    const uint8_t op = request && length >= 2 ? request[1] : 0xFF;
    replyBase(reply, op, ESC32_INVALID_REQUEST);
    if (!request || length < 2 || request[0] != ESCAPE32_CONTRACT_VERSION ||
        length != expectedLength(op)) {
        return true;
    }
    if (op == ESC32_CAPS) {
        replyBase(reply, op, ESC32_OK);
        return true;
    }
    const uint32_t token = read32(request + 2);
    const uint16_t id = read16(request + 6);
    // A late valid packet cannot revive an owner whose lease already expired
    // merely because the scheduler has not run its next periodic tick yet.
    if (session.active && elapsed(now, session.heartbeat, ESCAPE32_OWNER_TIMEOUT_MS)) {
        fail(session.state == ESC32_WRITING || session.state == ESC32_SAVING ||
            session.state == ESC32_VERIFY ? ESC32_OUTCOME_UNKNOWN : ESC32_TIMEOUT);
    }
    if (op == ESC32_OPEN) {
        const uint8_t target = request[8];
        const uint32_t nonce = read32(request + 10);
        if (token || !id || !nonce || request[9] != 1) {
            return true;
        }
        /* Retain OPEN replay identity even after a lost CLOSE reply. A late
         * OPEN retry must never reacquire pins after a completed session. */
        if (session.token && owner == session.owner && nonce == session.nonce) {
            if (target != session.target || id != session.openRequest) {
                return true;
            }
            if (session.active) {
                session.heartbeat = now;
            }
            replySnapshot(reply, op);
            return true;
        }
        if (session.active || session.inhibited) {
            replyBase(reply, op, ESC32_BUSY);
            return true;
        }
        if (!adapterValid()) {
            replyBase(reply, op, ESC32_UNSUPPORTED);
            return true;
        }
        if (target >= 16 || !(session.adapter->validatedTargets & (1u << target))) {
            replyBase(reply, op, ESC32_UNSUPPORTED);
            return true;
        }
        if (!session.adapter->safe(session.context)) {
            replyBase(reply, op, ESC32_BUSY);
            return true;
        }
        /* All callbacks exist and capability was validated before pin changes. */
        session.adapter->inhibit(session.context, true);
        session.inhibited = true;
        if (!session.adapter->acquire(session.context, target)) {
            session.adapter->inhibit(session.context, false);
            session.inhibited = false;
            replyBase(reply, op, ESC32_BUSY);
            return true;
        }
        session.active = true;
        session.entered = false;
        session.owner = owner;
        // Fresh client nonce on the first OPEN salts the boot's token sequence.
        // The odd stride is bijective over u32 until generation wrap. This
        // reduces reboot-stale collisions without claiming authentication.
        if (!session.generation) {
            session.bootSeed = nonce;
        }
        do {
            session.token = session.bootSeed + ++session.generation * UINT32_C(0x9E3779B9);
        } while (!session.token);
        session.nonce = nonce;
        session.openRequest = id;
        session.target = target;
        session.heartbeat = now;
        session.transactionStart = now;
        session.state = ESC32_WAIT_POWER;
        session.result = ESC32_BUSY;
        session.terminal = false;
        session.dirty = false;
        session.valueValid = false;
        session.key = 0xFF;
        session.revision = session.patch = session.schema = 0;
        remember(request, length);
        replySnapshot(reply, op);
        return true;
    }
    if (!session.token || token != session.token || owner != session.owner) {
        replyBase(reply, op, ESC32_INVALID_SESSION);
        return true;
    }
    if (op == ESC32_STATUS) {
        if (id && id != session.requestId) {
            replyBase(reply, op, ESC32_STALE_REQUEST);
            return true;
        }
        if (session.active) {
            session.heartbeat = now;
        }
        replySnapshot(reply, op);
        return true;
    }
    if (!id) {
        return true;
    }
    if (id == session.requestId) {
        if (length == session.lastRequestLength && !memcmp(request, session.lastRequest, length)) {
            if (session.active) {
                session.heartbeat = now;
            }
            replySnapshot(reply, op);
        }
        return true;
    }
    if (id < session.requestId) {
        replyBase(reply, op, ESC32_STALE_REQUEST);
        return true;
    }
    if (op == ESC32_CLOSE) {
        remember(request, length);
        releaseSession(ESC32_OK);
        replySnapshot(reply, op);
        return true;
    }
    if (!session.active) {
        replyBase(reply, op, ESC32_INVALID_SESSION);
        return true;
    }
    if (!session.terminal || session.state != ESC32_READY) {
        replyBase(reply, op, ESC32_BUSY);
        return true;
    }
    if (!session.adapter->safe(session.context)) {
        fail(ESC32_BUSY);
        replySnapshot(reply, op);
        return true;
    }
    if (!session.schema || !adapterValid()) {
        replyBase(reply, op, ESC32_UNSUPPORTED);
        return true;
    }
    if (op == ESC32_GET || op == ESC32_SET) {
        if (request[8] >= ESCAPE32_KEY_COUNT) {
            return true;
        }
        if (op == ESC32_SET) {
            if (!writable()) {
                replyBase(reply, op, ESC32_UNSUPPORTED);
                return true;
            }
            const int32_t value = (int32_t)read32(request + 9);
            if (value < settings[request[8]].min || value > settings[request[8]].max) {
                replyBase(reply, op, ESC32_RANGE);
                return true;
            }
            session.requestedValue = value;
        }
        session.key = request[8];
    } else if (op == ESC32_SAVE && !writable()) {
        replyBase(reply, op, ESC32_UNSUPPORTED);
        return true;
    }
    remember(request, length);
    session.heartbeat = now;
    session.result = ESC32_BUSY;
    session.terminal = false;
    session.valueValid = false;
    if (op == ESC32_GET) {
        session.state = ESC32_READ;
        if (!getKey(session.key, now)) {
            fail(ESC32_PROTOCOL_ERROR);
        }
    } else if (op == ESC32_SET) {
        char tx[48];
        const int n = snprintf(tx, sizeof(tx), "set %s %ld\n", settings[session.key].name, (long)session.requestedValue);
        session.state = ESC32_WRITING;
        /* May already have changed RAM once write() is attempted. */
        session.dirty = true;
        if (n <= 0 || (size_t)n >= sizeof(tx) || !command(tx, now)) {
            fail(ESC32_OUTCOME_UNKNOWN);
        }
    } else if (op == ESC32_SAVE) {
        session.state = ESC32_SAVING;
        session.key = 0xFF;
        if (!command("save\n", now)) {
            fail(ESC32_OUTCOME_UNKNOWN);
        }
    }
    replySnapshot(reply, op);
    return true;
}
