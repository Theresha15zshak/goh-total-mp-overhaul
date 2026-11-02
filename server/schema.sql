-- One row per player who has ever reported in. last_seen: unix seconds of the latest heartbeat.
CREATE TABLE IF NOT EXISTS players (
  steam_id  TEXT PRIMARY KEY,
  name      TEXT NOT NULL,
  version   TEXT NOT NULL,
  last_seen INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS players_seen ON players(last_seen);
