// Rated matches. A player with the mod, signed in with Steam, reports each match they finish (the DLL reads it out of the replay the
// game writes, dll/report.c); everyone in it is rated, with the mod or without. Rated: Axis vs Allies, Battle
// Zones ("battle_zones") or Domination ("ammunition", «Превосходство»), equal sides of 1..4, at least 3 minutes.
//
//   POST /report   text, one field per line:
//                    reporter SteamID / key / "v3" / mode / sides / map / start / end (unix, from the game) /
//                    winner ("axis"|"allies"), then a line per player: "steam_id\tnation\tname"
//                    two teams with nations of their own choice: "teams" in place of axis_allies, the winner is a team's letter and
//                    a player's line goes on with "\tteam"; side in the tables is then that letter
//                    the line's last field is the player's outcome (report.c): w win, l loss, q loss for leaving or closing the match,
//                    s small loss (the host's team when he closes it, losers to a team short of a man), n nothing
//                    only the reporter q and all others n: his own word that he left. It touches nobody else and is kept apart from
//                    the match, whose report comes later and would otherwise find it taken; that report then does not charge him again
//                    (nation: ger fin | eng usa rus)
//                  answers "rated", "confirmed", "disputed", "duplicate" or why not (4xx)
//   GET /ratings?ids=a,b,c     for the lobby: "steam_id\tladder\tmmr\tgames\twins" per known track
//   GET /player?id=X           "P\tname", then "L\tladder\tmmr\tgames\twins", "N\tladder\tnation\tmmr\tgames\twins",
//                              "M\tid\tended_at\tmode\tmap\tsize\tnation\twon\tdelta\tstatus\tallies\topponents" (newest first)
//   GET /leaderboard?ladder=1v1|team   "rank\tsteam_id\tname\tmmr\tgames\twins"
//
// The same match comes from several players: it is found again by its mode, map, start time and players.
// Reports that disagree on the winner make it disputed, and its rating changes are taken back out.
// A match an admin took out has status 'removed': out of the ratings and the histories, and kept, so that a late report of it is not rated anew.
// Rating: rating.js; uncertainty grows with time away, and the same players again within a day count for less.
import { rate, drift, repeatFactor, mmr, MU, SIGMA } from './rating.js';

const STEAM_ID = /^7656119\d{10}$/;
const KEY = /^[0-9a-f]{64}$/;
const MODES = new Set(['battle_zones', 'ammunition']);
const SIDE = { ger: 'axis', fin: 'axis', eng: 'allies', usa: 'allies', rus: 'allies' };
const MIN_DURATION_S = 120, MAX_AGE_S = 14 * 86400, REPORTS_PER_HOUR = 30, HISTORY = 30, PAGE = 15, MAX_PAGES = 999, BOARD_PAGE = 100;
// Balance tags of the builds we take reports from. An older build also sends matches its host closed early (116, 2026-11-07, from F919).
// The tag comes with the heartbeat; a release with a new tag goes in here.
const REPORT_TAGS = new Set(['4D2C']);
const SMALL_LOSS = 0.25, QUIT_MU = 1.5;   // QUIT_MU: leaving costs at least this, 60 MMR, or leaving would pay against a stronger team

const clean = (s, n) => Array.from(s.replace(/[\u0000-\u001f\u007f]/g, '').trim()).slice(0, n).join('') || '?';
const ladderOf = size => (size === 1 ? '1v1' : 'team');

async function sha256(s) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function parse(body, now) {
  const lines = body.split('\n');
  const [reporter, key, ver, mode, sides, map, startS, endS, winner] = lines;
  if (ver !== 'v3') return { err: 'unknown report version' };   // v2 also sent matches their host closed (119): v3 waits for the game's own GameOver
  if (!STEAM_ID.test(reporter || '') || !KEY.test(key || '')) return { err: 'bad reporter' };
  if (!MODES.has(mode)) return { err: 'mode not rated' };
  if (sides !== 'axis_allies' && sides !== 'teams') return { err: 'sides not rated' };
  const free = sides === 'teams';
  const start = Number(startS), end = Number(endS);
  if (!Number.isInteger(start) || !Number.isInteger(end) || end > now + 600 || end < now - MAX_AGE_S) return { err: 'bad time' };
  const duration = end - start;
  if (duration < MIN_DURATION_S) return { err: 'too short' };
  const players = [], seen = new Set();
  for (const l of lines.slice(9)) {
    if (!l) continue;
    const [id, nation, name = '', team = '', out = ''] = l.split('\t');
    if (!STEAM_ID.test(id || '') || !SIDE[nation] || seen.has(id) || (free && !/^[a-h]$/.test(team)) || !/^[wlqsn]$/.test(out)) return { err: 'bad player' };
    seen.add(id);
    players.push({ id, nation, side: free ? team : SIDE[nation], name: clean(name, 32), out });
  }
  const pair = free ? [...new Set(players.map(p => p.side))].sort() : ['axis', 'allies'];
  if (pair.length !== 2) return { err: 'not two teams' };
  if (!pair.includes(winner) || players.some(p => p.out === 'w' && p.side !== winner)) return { err: 'bad winner' };
  const one = players.filter(p => p.side === pair[0]), two = players.filter(p => p.side === pair[1]);
  if (!one.length || one.length !== two.length || one.length > 4) return { err: 'sides not equal' };
  if (!seen.has(reporter)) return { err: 'reporter not in the match' };
  return { reporter, key, mode, map: clean(map || '', 64), start, end, duration, winner, players, pair, size: one.length };
}

async function tracks(env, ladder, players) {
  // each player's 'all' and nation tracks, new ones at the start values
  const ids = players.map(p => p.id);
  const { results } = await env.DB.prepare(
    `SELECT steam_id, nation, mu, sigma, games, wins, last_at FROM ratings WHERE ladder = ?1 AND steam_id IN (${ids.map((_, i) => '?' + (i + 2)).join(',')})`
  ).bind(ladder, ...ids).all();
  const got = new Map(results.map(r => [r.steam_id + '/' + r.nation, r]));
  const one = (id, nation) => got.get(id + '/' + nation) || { mu: MU, sigma: SIGMA, games: 0, wins: 0 };
  return players.map(p => ({ all: one(p.id, 'all'), nat: one(p.id, p.nation) }));
}

export async function report(req, env) {
  const body = await req.text();
  if (body.length > 4096) return [413, 'too big'];
  const now = Math.floor(Date.now() / 1000);
  const r = parse(body, now);
  if (r.err) return [400, r.err];
  const held = await env.DB.prepare('SELECT key_hash, verified_at, version FROM players WHERE steam_id = ?1').bind(r.reporter).first();
  if (!held || !held.key_hash || held.key_hash !== (await sha256(r.key))) return [403, 'unknown key'];
  if (!held.verified_at) return [403, 'not verified'];     // only players signed in with Steam report (verify.js)
  if (!REPORT_TAGS.has(held.version)) return [200, 'old build'];   // 200: the client lets this match go and does not ask again
  const recent = await env.DB.prepare('SELECT count(*) AS n FROM reports WHERE reporter = ?1 AND at > ?2').bind(r.reporter, now - 3600).first();
  if (recent.n >= REPORTS_PER_HOUR) return [429, 'too many reports'];

  const base = await sha256([r.mode, r.map, r.start, ...r.players.map(p => p.id + ':' + p.nation).sort()].join('|'));
  const selfFp = id => sha256(base + ':self:' + id);
  const self = r.players.every(p => p.out === 'n' || (p.out === 'q' && p.id === r.reporter)) && r.players.some(p => p.out === 'q');
  const fp = self ? await selfFp(r.reporter) : base;
  const old = await env.DB.prepare('SELECT * FROM matches WHERE fp = ?1 ORDER BY id DESC LIMIT 1').bind(fp).first();
  if (old) return [200, await again(env, old, r, now)];
  if (!self) for (const p of r.players) {
    if (p.out !== 'q') continue;
    if (await env.DB.prepare('SELECT id FROM matches WHERE fp = ?1').bind(await selfFp(p.id)).first()) { p.out = 'n'; p.paid = true; }
  }

  // a new match: rate everyone on both tracks
  const ladder = ladderOf(r.size);
  const t = await tracks(env, ladder, r.players);
  const sideIdx = s => r.players.map((p, i) => (p.side === s ? i : -1)).filter(i => i >= 0);
  const ax = sideIdx(r.pair[0]), al = sideIdx(r.pair[1]), axisWon = r.winner === r.pair[0];
  const pkey = r.players.map(p => p.id).sort().join(',');
  const rep = await env.DB.prepare(
    "SELECT count(*) AS n FROM matches WHERE pkey = ?1 AND status = 'rated' AND ended_at >= ?2 AND ended_at < ?3"
  ).bind(pkey, r.end - 86400, r.end).first();
  const f = repeatFactor(rep.n);
  const after = { all: [], nat: [] };
  for (const k of ['all', 'nat']) {
    const before = t.map(x => drift(x[k], r.end));
    const [aWin, bLose] = rate(ax.map(i => before[i]), al.map(i => before[i]), true);
    const [aLose, bWin] = rate(ax.map(i => before[i]), al.map(i => before[i]), false);
    const up = [], down = [];
    ax.forEach((i, j) => { up[i] = aWin[j]; down[i] = aLose[j]; });
    al.forEach((i, j) => { up[i] = bWin[j]; down[i] = bLose[j]; });
    after[k] = r.players.map((p, i) => {
      const mu = t[i][k].mu, move = x => mu + f * (x.mu - mu);
      if (p.out === 'n') return { mu, sigma: t[i][k].sigma };
      if (p.out === 'w') return { mu: move(up[i]), sigma: up[i].sigma };
      if (p.out === 's') return { mu: mu + SMALL_LOSS * (move(down[i]) - mu), sigma: down[i].sigma };
      if (p.out === 'q') return { mu: Math.min(move(down[i]), mu - QUIT_MU), sigma: down[i].sigma };
      return { mu: move(down[i]), sigma: down[i].sigma };
    });
  }
  // One batch is one transaction, and fp is unique: of two reports of a match at the same moment one gets in whole, the other is
  // turned back here and goes on as a second report. Apart, both were rated as new matches (97 and 98).
  const matchId = '(SELECT id FROM matches WHERE fp = ?1)';
  const put = (pid, nation, x, won) => env.DB.prepare(
    `INSERT INTO ratings (steam_id, ladder, nation, mu, sigma, games, wins, last_at) VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7)
     ON CONFLICT(steam_id, ladder, nation) DO UPDATE SET mu = ?4, sigma = ?5, games = games + 1, wins = wins + ?6,
       last_at = max(coalesce(last_at, 0), ?7)`
  ).bind(pid, ladder, nation, x.mu, x.sigma, won, r.end);
  const st = [
    env.DB.prepare(
      `INSERT INTO matches (fp, mode, map, size, ladder, duration_s, winner, ended_at, status, reports, pkey)
       VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'rated', 1, ?9)`
    ).bind(fp, r.mode, r.map, r.size, ladder, r.duration, r.winner, r.end, pkey),
    env.DB.prepare(`INSERT INTO reports (match_id, reporter, winner, at) VALUES (${matchId}, ?2, ?3, ?4)`).bind(fp, r.reporter, r.winner, now),
  ];
  r.players.forEach((p, i) => {
    if (p.paid || (self && p.id !== r.reporter)) return;
    const won = p.out === 'w' ? 1 : p.out === 'n' ? 2 : 0;      // 2: this match changed nothing for this player
    st.push(env.DB.prepare(
      `INSERT INTO profiles (steam_id, name, updated) VALUES (?1, ?2, ?3) ON CONFLICT(steam_id) DO UPDATE SET name = ?2, updated = ?3`
    ).bind(p.id, p.name, now));
    if (won !== 2) st.push(put(p.id, 'all', after.all[i], won), put(p.id, p.nation, after.nat[i], won));
    st.push(env.DB.prepare(
      `INSERT INTO match_players (match_id, steam_id, name, nation, side, won, mu_a0, sg_a0, mu_a1, sg_a1, mu_n0, sg_n0, mu_n1, sg_n1)
       VALUES (${matchId}, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)`
    ).bind(fp, p.id, p.name, p.nation, p.side, won, t[i].all.mu, t[i].all.sigma, after.all[i].mu, after.all[i].sigma,
      t[i].nat.mu, t[i].nat.sigma, after.nat[i].mu, after.nat[i].sigma));
  });
  try {
    await env.DB.batch(st);
  } catch (e) {
    const first = /UNIQUE/i.test(String(e)) && await env.DB.prepare('SELECT * FROM matches WHERE fp = ?1 ORDER BY id DESC LIMIT 1').bind(fp).first();
    if (!first) throw e;
    return [200, await again(env, first, r, now)];
  }
  return [200, 'rated'];
}

// another player's report of a match we have
async function again(env, m, r, now) {
  const dup = await env.DB.prepare('SELECT 1 FROM reports WHERE match_id = ?1 AND reporter = ?2').bind(m.id, r.reporter).first();
  if (dup) return 'duplicate';
  const st = [
    env.DB.prepare('INSERT INTO reports (match_id, reporter, winner, at) VALUES (?1, ?2, ?3, ?4)').bind(m.id, r.reporter, r.winner, now),
    env.DB.prepare('UPDATE matches SET reports = reports + 1 WHERE id = ?1').bind(m.id),
  ];
  if (r.winner === m.winner || m.status !== 'rated') {
    await env.DB.batch(st);
    return m.status === 'rated' ? 'confirmed' : m.status;
  }
  // the reports disagree: the match comes back out of everyone's ratings
  const { results } = await env.DB.prepare('SELECT * FROM match_players WHERE match_id = ?1').bind(m.id).all();
  const back = (pid, nation, mu0, sg0, mu1, sg1, won) => env.DB.prepare(
    `UPDATE ratings SET mu = mu - ?4, sigma = sigma * ?5, games = games - 1, wins = wins - ?6
     WHERE steam_id = ?1 AND ladder = ?2 AND nation = ?3`
  ).bind(pid, m.ladder, nation, mu1 - mu0, sg0 / sg1, won);
  for (const p of results) {
    if (p.won === 2) continue;                  // nothing to take back
    st.push(back(p.steam_id, 'all', p.mu_a0, p.sg_a0, p.mu_a1, p.sg_a1, p.won));
    st.push(back(p.steam_id, p.nation, p.mu_n0, p.sg_n0, p.mu_n1, p.sg_n1, p.won));
  }
  st.push(env.DB.prepare("UPDATE matches SET status = 'disputed' WHERE id = ?1").bind(m.id));
  await env.DB.batch(st);
  return 'disputed';
}

const tsv = (...f) => f.map(x => String(x).replace(/[\t\n\r]/g, ' ')).join('\t') + '\n';

export async function ratings(url, env) {
  const ids = (url.searchParams.get('ids') || '').split(',').filter(x => STEAM_ID.test(x)).slice(0, 16);
  if (!ids.length) return '';
  const { results } = await env.DB.prepare(
    `SELECT steam_id, ladder, mu, games, wins FROM ratings WHERE nation = 'all' AND games > 0 AND steam_id IN (${ids.map((_, i) => '?' + (i + 1)).join(',')})`
  ).bind(...ids).all();
  return results.map(r => tsv(r.steam_id, r.ladder, mmr(r.mu), r.games, r.wins)).join('');
}

export async function player(url, env) {
  const id = url.searchParams.get('id') || '';
  if (!STEAM_ID.test(id)) return null;
  const prof = await env.DB.prepare('SELECT name FROM profiles WHERE steam_id = ?1').bind(id).first();
  const { results: rs } = await env.DB.prepare(
    'SELECT ladder, nation, mu, games, wins FROM ratings WHERE steam_id = ?1 AND games > 0 ORDER BY ladder, nation').bind(id).all();
  // ?page=N (from 1): PAGE matches of the history and an "H page pages" line; without it the newest HISTORY, as older clients expect.
  // With a page also "V version": the client keeps the text, sends &have=version next time and gets "SAME" while nothing changed.
  // Every match that counts or is taken back moves the player's ratings or his count of matches, and so the version.
  const paged = url.searchParams.has('page'), size = paged ? PAGE : HISTORY;
  const page = paged ? Math.min(Math.max(parseInt(url.searchParams.get('page'), 10) || 1, 1), MAX_PAGES) : 1;
  let count = 0, version = '';
  if (paged) {
    count = (await env.DB.prepare(
      "SELECT count(*) AS n FROM match_players mp JOIN matches m ON m.id = mp.match_id WHERE mp.steam_id = ?1 AND m.status != 'removed'"
    ).bind(id).first()).n;
    version = (await sha256(JSON.stringify([prof ? prof.name : '', rs, count, page, PAGE]))).slice(0, 16);
    if (url.searchParams.get('have') === version) return 'SAME\n';
  }
  const { results: ms } = await env.DB.prepare(
    `SELECT m.id, m.ended_at, m.mode, m.map, m.size, m.status, mp.nation, mp.side, mp.won, mp.mu_a0, mp.mu_a1
     FROM match_players mp JOIN matches m ON m.id = mp.match_id WHERE mp.steam_id = ?1 AND m.status != 'removed' ORDER BY m.id DESC
     LIMIT ${size} OFFSET ${(page - 1) * size}`
  ).bind(id).all();
  let out = tsv('P', prof ? prof.name : '');
  if (paged) out += tsv('V', version) + tsv('H', page, Math.min(MAX_PAGES, Math.max(1, Math.ceil(count / PAGE))));
  for (const r of rs) out += r.nation === 'all' ? tsv('L', r.ladder, mmr(r.mu), r.games, r.wins) : tsv('N', r.ladder, r.nation, mmr(r.mu), r.games, r.wins);
  if (ms.length) {
    const { results: all } = await env.DB.prepare(
      `SELECT match_id, steam_id, name, side FROM match_players WHERE match_id IN (${ms.map((_, i) => '?' + (i + 1)).join(',')})`
    ).bind(...ms.map(m => m.id)).all();
    for (const m of ms) {
      const others = all.filter(p => p.match_id === m.id && p.steam_id !== id);
      const names = side => others.filter(p => p.side === side).map(p => p.name).join(', ');
      const delta = m.status === 'rated' ? mmr(m.mu_a1) - mmr(m.mu_a0) : 0;
      out += tsv('M', m.id, m.ended_at, m.mode, m.map, m.size, m.nation, m.won, delta, m.status,
        names(m.side), others.filter(p => p.side !== m.side).map(p => p.name).join(', '));
    }
  }
  return out;
}

// &page=N: BOARD_PAGE players and an "H page pages" line; without it the whole ladder, as older clients expect.
// &cache=1: a "V version" line first, from a cheap sum over the ladder; &have=version then gets "SAME" while it holds.
// A name changed in profiles does not move it: the board shows the old one until a rating moves.
export async function leaderboard(url, env) {
  const ladder = url.searchParams.get('ladder') === '1v1' ? '1v1' : 'team';
  const paged = url.searchParams.has('page');
  const page = paged ? Math.min(Math.max(parseInt(url.searchParams.get('page'), 10) || 1, 1), MAX_PAGES) : 1;
  let head = '';
  if (paged || url.searchParams.get('cache') === '1') {
    const s = await env.DB.prepare(
      "SELECT count(*) AS n, total(mu) AS mu, total(sigma) AS sg, total(games) AS g, total(wins) AS w FROM ratings WHERE ladder = ?1 AND nation = 'all' AND games > 0"
    ).bind(ladder).first();
    if (url.searchParams.get('cache') === '1') {
      const version = (await sha256(JSON.stringify([ladder, s.n, s.mu, s.sg, s.g, s.w, paged ? page : 0]))).slice(0, 16);
      if (url.searchParams.get('have') === version) return 'SAME\n';
      head = tsv('V', version);
    }
    if (paged) head += tsv('H', page, Math.min(MAX_PAGES, Math.max(1, Math.ceil(s.n / BOARD_PAGE))));
  }
  const skip = paged ? (page - 1) * BOARD_PAGE : 0;
  const { results } = await env.DB.prepare(
    `SELECT r.steam_id, coalesce(p.name, '?') AS name, r.mu, r.games, r.wins FROM ratings r LEFT JOIN profiles p ON p.steam_id = r.steam_id
     WHERE r.ladder = ?1 AND r.nation = 'all' AND r.games > 0 ORDER BY r.mu DESC, r.steam_id${paged ? ` LIMIT ${BOARD_PAGE} OFFSET ${skip}` : ''}`
  ).bind(ladder).all();
  return head + results.map((r, i) => tsv(skip + i + 1, r.steam_id, r.name, mmr(r.mu), r.games, r.wins)).join('');
}
