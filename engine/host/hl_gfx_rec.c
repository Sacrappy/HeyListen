/* (HeyListen native engine) The OS layer's side of direct emission: native producers write the back end's command
   records into a command buffer's own segment (engine/native: hl/c/hl_gfx_rec.h, hl/gfx/Records.h;
   docs/native-engine/direct-emission.md, bricks A and B).

   The surface is nvn_native.c's (nvn_rec_*: open a command buffer, append a record as the NVN procedures do, the
   recording's mark, its bytes, a rewind, a checked guest read); this file hands it to the native side and reads the
   switches, at the first call of the native render-state function (HL_GFX_STATE=1):
     HL_GFX_REC=0        (the default) the render state goes through the NVN procedures, as before
     HL_GFX_REC=1        the render state written as records directly: no NVN procedure called. What the 29 procedures
                         did besides their record is done or not needed (direct-emission.md section 8): the command
                         buffer's accounting (cmd_used, hw_used, last_rec: GetCommandMemoryUsed) kept by the append; the
                         state objects set on native copies, not in the function's dead stack frame
     HL_GFX_REC_CHECK=1  the record check: at every call the NVN procedures record as before, the native records go to a
                         scratch segment, and the two are compared byte for byte with the accounting; with HL_GFX_REC=1
                         the native records are the live ones and the procedures' are compared, then dropped
   The counters go to the log every 10 s ("hl gfx rec: ..."); the first eight mismatches in full. */
#include "hl_host.h"
#include "hl/c/hl_gfx_rec.h"
#include <stdio.h>
#include <stdlib.h>

void *nvn_rec_open(uint64_t cmd);                                                   /* nvn_native.c */
void *nvn_rec_append(void *cmd, uint32_t op, uint32_t payload, uint32_t hw);
void nvn_rec_mark(void *cmd, hl_rec_mark *m);
const uint8_t *nvn_rec_bytes(void *cmd, uint64_t offset);
void nvn_rec_rewind(void *cmd, const hl_rec_mark *m);
const void *nvn_rec_guest(uint64_t a, uint64_t n);

static void *rec_open(void *user, uint64_t cmd) { (void)user; return nvn_rec_open(cmd); }
static void *rec_append(void *user, void *cmd, uint32_t op, uint32_t payload, uint32_t hw) {
    (void)user;
    return nvn_rec_append(cmd, op, payload, hw);
}
static void rec_mark(void *user, void *cmd, hl_rec_mark *m) { (void)user; nvn_rec_mark(cmd, m); }
static const uint8_t *rec_bytes(void *user, void *cmd, uint64_t offset) { (void)user; return nvn_rec_bytes(cmd, offset); }
static void rec_rewind(void *user, void *cmd, const hl_rec_mark *m) { (void)user; nvn_rec_rewind(cmd, m); }
static const void *rec_guest(void *user, uint64_t a, uint64_t n) { (void)user; return nvn_rec_guest(a, n); }
static void rec_log(void *user, const char *line) { (void)user; hos_log("%s", line); }

#define HL_GFX_REC_DEFAULT 0   /* (off until the main PC's validation runs) */

static int switch_of(const char *name, int dflt) {
    const char *e = getenv(name);
    return e && e[0] ? atoi(e) : dflt;
}
static int rec_mode(void *user) {
    (void)user;
    const int write = switch_of("HL_GFX_REC", HL_GFX_REC_DEFAULT) == 1, check = switch_of("HL_GFX_REC_CHECK", 0) != 0;
    hos_log("hl gfx rec: the render state %s%s", write ? "written as records (HL_GFX_REC=1)" : "through the NVN procedures",
            !check ? ""
            : write ? "; the record check: the NVN procedures' records compared with the native ones, then dropped"
                    : "; the record check: native records into a scratch segment compared with the NVN procedures'");
    return (write ? HL_REC_WRITE : 0) | (check ? HL_REC_CHECK : 0);
}

static const hl_gfx_rec_host rec_host = {NULL, rec_open, rec_append, rec_mark, rec_bytes, rec_rewind, rec_guest, rec_mode, rec_log};

__attribute__((constructor)) static void hl_gfx_rec_hand_over(void) { hl_gfx_rec_register(&rec_host); }
