// Rated matches. A player with the mod, signed in with Steam, reports each match they finish (the DLL reads it out of the replay the
// game writes, dll/report.c); everyone in it is rated, with the mod or without. Rated: Axis vs Allies, Battle
// Zones ("battle_zones") or Domination ("ammunition", «Превосходство»), equal sides of 1..4, at least 3 minutes.
//
//   POST /report   text, one field per line:
//                    reporter SteamID / key / "v2" / mode / sides / map / start / end (unix, from the game) /
//                    winner ("axis"|"allies"), then a line per player: "steam_id\tnation\tname"
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
const MIN_DURATION_S = 180, MAX_AGE_S = 14 * 86400, REPORTS_PER_HOUR = 30, HISTORY = 30;

const clean = (s, n) => Array.from(s.replace(/[\u0000-\u001f\u007f]/g, '').trim()).slice(0, n).join('') || '?';
const ladderOf = size => (size === 1 ? '1v1' : 'team');

async function sha256(s) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function parse(body, now) {
  const lines = body.split('\n');
  const [reporter, key, ver, mode, sides, map, startS, endS, winner] = lines;
  if (ver !== 'v2') return { err: 'unknown report version' };
  if (!STEAM_ID.test(reporter || '') || !KEY.test(key || '')) return { err: 'bad reporter' };
  if (!MODES.has(mode)) return { err: 'mode not rated' };
  if (sides !== 'axis_allies') return { err: 'sides not rated' };
  const start = Number(startS), end = Number(endS);
  if (!Number.isInteger(start) || !Number.isInteger(end) || end > now + 600 || end < now - MAX_AGE_S) return { err: 'bad time' };
  const duration = end - start;
  if (duration < MIN_DURATION_S) return { err: 'too short' };
  if (winner !== 'axis' && winner !== 'allies') return { err: 'bad winner' };
  const players = [], seen = new Set();
  for (const l of lines.slice(9)) {
    if (!l) continue;
    const [id, nation, name = ''] = l.split('\t');
    if (!STEAM_ID.test(id || '') || !SIDE[nation] || seen.has(id)) return { err: 'bad player' };
    seen.add(id);
    players.push({ id, nation, side: SIDE[nation], name: clean(name, 32) });
  }
  const axis = players.filter(p => p.side === 'axis'), allies = players.filter(p => p.side === 'allies');
  if (!axis.length || axis.length !== allies.length || axis.length > 4) return { err: 'sides not equal' };
  if (!seen.has(reporter)) return { err: 'reporter not in the match' };
  return { reporter, key, mode, map: clean(map || '', 64), start, end, duration, winner, players, size: axis.length };
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
  const held = await env.DB.prepare('SELECT key_hash, verified_at FROM players WHERE steam_id = ?1').bind(r.reporter).first();
  if (!held || !held.key_hash || held.key_hash !== (await sha256(r.key))) return [403, 'unknown key'];
  if (!held.verified_at) return [403, 'not verified'];     // only players signed in with Steam report (verify.js)
  const recent = await env.DB.prepare('SELECT count(*) AS n FROM reports WHERE reporter = ?1 AND at > ?2').bind(r.reporter, now - 3600).first();
  if (recent.n >= REPORTS_PER_HOUR) return [429, 'too many reports'];

  const fp = await sha256([r.mode, r.map, r.start, ...r.players.map(p => p.id + ':' + p.nation).sort()].join('|'));
  const old = await env.DB.prepare('SELECT * FROM matches WHERE fp = ?1 ORDER BY id DESC LIMIT 1').bind(fp).first();
  if (old) return [200, await again(env, old, r, now)];

  // a new match: rate everyone on both tracks
  const ladder = ladderOf(r.size);
  const t = await tracks(env, ladder, r.players);
  const sideIdx = s => r.players.map((p, i) => (p.side === s ? i : -1)).filter(i => i >= 0);
  const ax = sideIdx('axis'), al = sideIdx('allies'), axisWon = r.winner === 'axis';
  const pkey = r.players.map(p => p.id).sort().join(',');
  const rep = await env.DB.prepare(
    "SELECT count(*) AS n FROM matches WHERE pkey = ?1 AND status = 'rated' AND ended_at >= ?2 AND ended_at < ?3"
  ).bind(pkey, r.end - 86400, r.end).first();
  const f = repeatFactor(rep.n);
  const after = { all: [], nat: [] };
  for (const k of ['all', 'nat']) {
    const before = t.map(x => drift(x[k], r.end));
    const [a, b] = rate(ax.map(i => before[i]), al.map(i => before[i]), axisWon);
    ax.forEach((i, j) => (after[k][i] = a[j]));
    al.forEach((i, j) => (after[k][i] = b[j]));
    after[k] = after[k].map((x, i) => ({ mu: t[i][k].mu + f * (x.mu - t[i][k].mu), sigma: x.sigma }));
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
    const won = p.side === r.winner ? 1 : 0;
    st.push(env.DB.prepare(
      `INSERT INTO profiles (steam_id, name, updated) VALUES (?1, ?2, ?3) ON CONFLICT(steam_id) DO UPDATE SET name = ?2, updated = ?3`
    ).bind(p.id, p.name, now));
    st.push(put(p.id, 'all', after.all[i], won), put(p.id, p.nation, after.nat[i], won));
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
  const { results: rs } = await env.DB.prepare('SELECT ladder, nation, mu, games, wins FROM ratings WHERE steam_id = ?1 AND games > 0').bind(id).all();
  const { results: ms } = await env.DB.prepare(
    `SELECT m.id, m.ended_at, m.mode, m.map, m.size, m.status, mp.nation, mp.side, mp.won, mp.mu_a0, mp.mu_a1
     FROM match_players mp JOIN matches m ON m.id = mp.match_id WHERE mp.steam_id = ?1 AND m.status != 'removed' ORDER BY m.id DESC LIMIT ${HISTORY}`
  ).bind(id).all();
  let out = tsv('P', prof ? prof.name : '');
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
        names(m.side), names(m.side === 'axis' ? 'allies' : 'axis'));
    }
  }
  return out;
}

export async function leaderboard(url, env) {
  const ladder = url.searchParams.get('ladder') === '1v1' ? '1v1' : 'team';
  const { results } = await env.DB.prepare(
    `SELECT r.steam_id, coalesce(p.name, '?') AS name, r.mu, r.games, r.wins FROM ratings r LEFT JOIN profiles p ON p.steam_id = r.steam_id
     WHERE r.ladder = ?1 AND r.nation = 'all' AND r.games > 0 ORDER BY r.mu DESC`
  ).bind(ladder).all();
  return results.map((r, i) => tsv(i + 1, r.steam_id, r.name, mmr(r.mu), r.games, r.wins)).join('');
}
