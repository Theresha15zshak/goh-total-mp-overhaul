-- 2026-11-06: players' camouflages for the others in a match (src/skins.js)
CREATE TABLE IF NOT EXISTS skin_pics (
  hash  TEXT PRIMARY KEY,
  data  BLOB NOT NULL,
  size  INTEGER NOT NULL,
  owner TEXT NOT NULL,
  at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS skin_pics_owner ON skin_pics(owner);
CREATE TABLE IF NOT EXISTS skin_sets (
  steam_id TEXT PRIMARY KEY,
  list     TEXT NOT NULL,
  hash     TEXT NOT NULL,
  at       INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS skin_rooms (
  room     TEXT NOT NULL,
  number   INTEGER NOT NULL,
  steam_id TEXT NOT NULL,
  at       INTEGER NOT NULL,
  PRIMARY KEY (room, number)
);
