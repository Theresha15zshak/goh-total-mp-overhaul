-- 2026-11-02: Steam sign-in (src/verify.js). verified_at: when Steam confirmed the SteamID is this key's;
-- players without it are anonymous (no reports, Anonymous#### in the chat).
ALTER TABLE players ADD COLUMN verified_at INTEGER;
CREATE TABLE IF NOT EXISTS verify_tokens (
  token    TEXT PRIMARY KEY,
  key_hash TEXT NOT NULL,
  at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS verify_tokens_key ON verify_tokens(key_hash, at);
-- a punishment also holds the key it was given to: an anonymous player cannot get round it with another SteamID
ALTER TABLE chat_bans ADD COLUMN key_hash TEXT;
CREATE INDEX IF NOT EXISTS chat_bans_key ON chat_bans(key_hash);
ALTER TABLE chat_messages ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;
