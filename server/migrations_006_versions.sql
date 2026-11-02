-- 2026-11-02: the mod's official builds (src/versions.js, filled by tools/release.py)
CREATE TABLE IF NOT EXISTS mod_versions (
  sha256      TEXT PRIMARY KEY,
  version     TEXT NOT NULL,
  kind        TEXT NOT NULL,
  released_at INTEGER NOT NULL,
  current     INTEGER NOT NULL
);
