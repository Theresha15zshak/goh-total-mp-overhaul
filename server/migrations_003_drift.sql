-- 2026-11-02: uncertainty grows with time away (ratings.last_at) and repeats of the same players within a day
-- count for less (matches.pkey: their SteamIDs sorted, comma separated)
ALTER TABLE ratings ADD COLUMN last_at INTEGER;
ALTER TABLE matches ADD COLUMN pkey TEXT;
CREATE INDEX IF NOT EXISTS matches_pkey ON matches(pkey, ended_at);
UPDATE ratings SET last_at = (SELECT max(m.ended_at) FROM match_players mp JOIN matches m ON m.id = mp.match_id
  WHERE mp.steam_id = ratings.steam_id AND m.ladder = ratings.ladder AND m.status = 'rated');
UPDATE matches SET pkey = (SELECT group_concat(steam_id, ',') FROM
  (SELECT steam_id FROM match_players WHERE match_id = matches.id ORDER BY steam_id));
