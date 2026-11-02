-- 2026-11-02: global chat (src/chat.js). Messages are wiped every two hours; at in milliseconds.
CREATE TABLE IF NOT EXISTS chat_messages (
  id       INTEGER PRIMARY KEY AUTOINCREMENT,
  channel  TEXT NOT NULL,
  steam_id TEXT NOT NULL,
  name     TEXT NOT NULL,
  text     TEXT NOT NULL,
  at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS chat_messages_channel ON chat_messages(channel, id);
CREATE INDEX IF NOT EXISTS chat_messages_by ON chat_messages(steam_id, at);
-- kind 'mute' (may read, not write) or 'ban' (neither); until: unix seconds, 0 = for good
CREATE TABLE IF NOT EXISTS chat_bans (
  steam_id TEXT PRIMARY KEY,
  kind     TEXT NOT NULL,
  until    INTEGER NOT NULL,
  by       TEXT NOT NULL,
  at       INTEGER NOT NULL
);
-- 'epoch': bumped whenever messages are taken away, so the clients start over
CREATE TABLE IF NOT EXISTS chat_meta (
  k TEXT PRIMARY KEY,
  v INTEGER NOT NULL
);
