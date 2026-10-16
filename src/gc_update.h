/*
 * Game Compressor - self-update from the project's GitHub releases.
 *
 * The payload (not the TV app, which has no TLS stack) looks up the latest
 * release, downloads game-compressor.elf and PPSA99290.ffpkg over HTTPS,
 * checks them against the release's SHA256SUMS, and puts them in place:
 *   - the ELF in /data/game-compressor/ (the TV app starts the newest copy),
 *   - the app image over the installed PPSA99290.ffpkg (taking effect when the
 *     app is reopened; a folder copy is never touched).
 */

#pragma once

#include "websrv.h"

/* GET /api/gc/update -> current state. */
int gc_update_status_request(const http_request_t *req);
/* POST /api/gc/update/check, POST /api/gc/update/install. */
int gc_update_action_request(const http_request_t *req);
