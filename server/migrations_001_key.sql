-- 2026-11-02: player keys (schema.sql has the column for a new database)
ALTER TABLE players ADD COLUMN key_hash TEXT;
