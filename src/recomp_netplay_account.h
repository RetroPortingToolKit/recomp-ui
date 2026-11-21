/*
 * recomp_netplay_account.h -- point the sign-in client at the lobby host.
 *
 * rnet_auth derives its HTTP host from the ws:// lobby URL handed to
 * rnet_account_init. Nothing calls that on its own, and with the host left
 * blank every /auth POST dies in getaddrinfo("") -- surfaced to the player as
 * "Could not reach the lobby server to start the sign-in", whichever server is
 * configured. Both netplay backends (recomp_netplay_host.c and psxrecomp's own
 * ae_np_*) call this from their launcher pump, so the rule lives once.
 *
 * `lobby_url` must be the RESOLVED url -- the backend's default_url callback,
 * which falls back to the built-in lobby when the player never saved one. An
 * empty url is ignored rather than guessed at; passing the raw "saved URL"
 * string is exactly the bug this exists to prevent.
 *
 * Header-only on purpose: it needs only recomp_net/auth.h, so an engine with
 * its own lobby backend takes it without linking recomp_launcher_netplay.
 *
 * Re-inits only when the URL changes: the login worker thread reads the host
 * while a sign-in is in flight, and re-initialising it every pump would race.
 * rnet_account_pump (redeem a stored key) is the caller's, after this.
 */
#ifndef RECOMP_NETPLAY_ACCOUNT_H
#define RECOMP_NETPLAY_ACCOUNT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "recomp_net/auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve `leaf` beside the executable; return 1 on success. NULL leaves the
 * secret cwd-relative (rnet_auth migrates an old cwd file on first load). */
typedef int (*RecompNetplayExeDirPathFn)(void *ctx, const char *leaf,
                                         char *out, size_t cap);

static inline void recomp_netplay_account_sync(const char *lobby_url,
                                               RecompNetplayExeDirPathFn exe_dir_path,
                                               void *ctx)
{
  static char s_url[256];
  if (!lobby_url || !lobby_url[0] || strcmp(lobby_url, s_url) == 0) return;
  /* Anchor the secret to the executable directory before the first init, so
   * the same install does not sign itself out depending on the launch cwd. */
  if (!s_url[0] && exe_dir_path) {
    char secret_path[512];
    if (exe_dir_path(ctx, "netplay_secret", secret_path, sizeof(secret_path)))
      rnet_account_set_secret_path(secret_path);
  }
  snprintf(s_url, sizeof(s_url), "%s", lobby_url);
  rnet_account_init(lobby_url);
}

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NETPLAY_ACCOUNT_H */
