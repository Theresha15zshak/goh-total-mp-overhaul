-- One row per player who has ever reported in. last_seen: unix seconds of the latest heartbeat.
-- key_hash: SHA-256 (hex) of the key the player's DLL made at its first start; the first heartbeat with a
-- key claims the SteamID, later ones must bring the same key. The key itself is never stored.
CREATE TABLE IF NOT EXISTS players (
  steam_id  TEXT PRIMARY KEY,
  name      TEXT NOT NULL,
  version   TEXT NOT NULL,
  last_seen INTEGER NOT NULL,
  key_hash  TEXT
);
CREATE INDEX IF NOT EXISTS players_seen ON players(last_seen);
