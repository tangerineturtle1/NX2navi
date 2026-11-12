// The relay NX2navi's co-op (coop.c) joins when coop_server is not set: the one place that default lives, like
// discord_app.h for the Rich Presence. Empty until the project hosts a relay: with no server the session stays idle and
// logs why, rather than sending a player's room code to a server nobody chose. Anyone can run tools/coop_relay.py and set
// coop_server in nx2navi.ini (docs/coop.md).
#pragma once

#define NX2NAVI_COOP_SERVER_DEFAULT ""   // "host" or "host:port"; the default port is 24680
