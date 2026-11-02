-- 2026-11-02: rated matches (src/matches.js). Everyone in a reported match gets a profile and ratings,
-- with the mod or without; only players with the mod (and their key) can report.
CREATE TABLE IF NOT EXISTS profiles (
  steam_id TEXT PRIMARY KEY,
  name     TEXT NOT NULL,
  updated  INTEGER NOT NULL
);
-- one track per ladder ('1v1' | 'team') and nation ('all' or the army played)
CREATE TABLE IF NOT EXISTS ratings (
  steam_id TEXT NOT NULL,
  ladder   TEXT NOT NULL,
  nation   TEXT NOT NULL,
  mu       REAL NOT NULL,
  sigma    REAL NOT NULL,
  games    INTEGER NOT NULL,
  wins     INTEGER NOT NULL,
  PRIMARY KEY (steam_id, ladder, nation)
);
CREATE INDEX IF NOT EXISTS ratings_board ON ratings(ladder, nation, mu);
-- status: 'rated' (counted), 'disputed' (reports disagree on the winner: taken back out)
CREATE TABLE IF NOT EXISTS matches (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  fp         TEXT NOT NULL,
  mode       TEXT NOT NULL,
  map        TEXT NOT NULL,
  size       INTEGER NOT NULL,
  ladder     TEXT NOT NULL,
  duration_s INTEGER NOT NULL,
  winner     TEXT NOT NULL,
  ended_at   INTEGER NOT NULL,
  status     TEXT NOT NULL,
  reports    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS matches_fp ON matches(fp, ended_at);
-- each player's two tracks before and after the match, so a disputed match can be taken back out
CREATE TABLE IF NOT EXISTS match_players (
  match_id INTEGER NOT NULL,
  steam_id TEXT NOT NULL,
  name     TEXT NOT NULL,
  nation   TEXT NOT NULL,
  side     TEXT NOT NULL,
  won      INTEGER NOT NULL,
  mu_a0 REAL, sg_a0 REAL, mu_a1 REAL, sg_a1 REAL,
  mu_n0 REAL, sg_n0 REAL, mu_n1 REAL, sg_n1 REAL,
  PRIMARY KEY (match_id, steam_id)
);
CREATE INDEX IF NOT EXISTS match_players_player ON match_players(steam_id, match_id);
CREATE TABLE IF NOT EXISTS reports (
  match_id INTEGER NOT NULL,
  reporter TEXT NOT NULL,
  winner   TEXT NOT NULL,
  at       INTEGER NOT NULL,
  PRIMARY KEY (match_id, reporter)
);
CREATE INDEX IF NOT EXISTS reports_by ON reports(reporter, at);
