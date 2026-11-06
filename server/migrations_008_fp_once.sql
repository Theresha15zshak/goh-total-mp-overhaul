-- A match is one row: two reports at the same moment used to make two (see report() in src/matches.js).
-- Row 97 was the second row of match 98, both removed; its fp is set apart so the index can be made.
UPDATE matches SET fp = fp || ':97' WHERE id = 97;
CREATE UNIQUE INDEX IF NOT EXISTS matches_fp_once ON matches(fp);
