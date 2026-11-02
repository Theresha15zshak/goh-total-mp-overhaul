// Global chat, channels "ru" and "en". Nothing is kept for long: every message goes at the even hours (UTC),
// by the cron trigger (wrangler.jsonc). Writing takes the player's key, as everything else here does.
//
//   GET  /chat?ch=ru&after=N&epoch=E&id=ME   text, a line per field set:
//          "E\tepoch"  messages were taken away since (a wipe or a moderator): with another E the client
//                      drops what it has and gets the whole channel again (after is then ignored)
//          "S\tmute\tuntil" / "S\tban"   ME may not write (a ban also hides the channel)
//          "A"                           ME is a moderator, then "B\tsteam_id\tname\tkind\tuntil" per
//                                        punishment in force
//          "M\tid\tat_ms\tsteam_id\tname\ttext"   oldest first, at most 100
//   POST /chat      "steam_id\nkey\nchannel\ntext"            -> "ok" / "muted <until>" / "banned" / "slow" ...
//   POST /chatmod   "steam_id\nkey\naction\ntarget\ndays"     moderators only (env.MODERATORS, comma separated):
//                   mute (days) / ban / unban / del (the target's messages in every channel)
const STEAM_ID = /^7656119\d{10}$/;
const KEY = /^[0-9a-f]{64}$/;
const CHANNELS = new Set(['ru', 'en']);
const MAX_TEXT = 300, SLOW_MS = 2000, PAGE = 100;

async function sha256(s) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

const tsv = (...f) => f.map(x => String(x).replace(/[\t\n\r]/g, ' ')).join('\t') + '\n';
const isMod = (env, id) => (env.MODERATORS || '').split(',').map(s => s.trim()).includes(id);

async function epoch(env) {
  const r = await env.DB.prepare("SELECT v FROM chat_meta WHERE k = 'epoch'").first();
  return r ? r.v : 0;
}
const bump = env => env.DB.prepare(
  "INSERT INTO chat_meta (k, v) VALUES ('epoch', 1) ON CONFLICT(k) DO UPDATE SET v = v + 1");

// the punishment in force for a player, if any: { kind, until } (until 0 = for good)
async function punished(env, id, now) {
  const r = await env.DB.prepare('SELECT kind, until FROM chat_bans WHERE steam_id = ?1').bind(id).first();
  return r && (r.until === 0 || r.until > now) ? r : null;
}

// the reporting player with their key, or null
async function player(env, id, key) {
  if (!STEAM_ID.test(id || '') || !KEY.test(key || '')) return null;
  const p = await env.DB.prepare('SELECT name, key_hash FROM players WHERE steam_id = ?1').bind(id).first();
  return p && p.key_hash && p.key_hash === (await sha256(key)) ? p : null;
}

export async function read(url, env) {
  const ch = url.searchParams.get('ch');
  if (!CHANNELS.has(ch)) return null;
  const me = url.searchParams.get('id') || '';
  const now = Math.floor(Date.now() / 1000);
  const e = await epoch(env);
  let out = tsv('E', e);
  const p = STEAM_ID.test(me) ? await punished(env, me, now) : null;
  if (p) out += p.kind === 'ban' ? tsv('S', 'ban') : tsv('S', 'mute', p.until);
  if (STEAM_ID.test(me) && isMod(env, me)) {
    out += 'A\n';
    const { results } = await env.DB.prepare(
      `SELECT b.steam_id, coalesce(pl.name, '?') AS name, b.kind, b.until FROM chat_bans b
       LEFT JOIN players pl ON pl.steam_id = b.steam_id WHERE b.until = 0 OR b.until > ?1 ORDER BY b.at DESC LIMIT 50`
    ).bind(now).all();
    for (const b of results) out += tsv('B', b.steam_id, b.name, b.kind, b.until);
  }
  if (p && p.kind === 'ban') return out;
  const after = String(e) === url.searchParams.get('epoch') ? Math.max(0, Number(url.searchParams.get('after')) || 0) : 0;
  const { results } = await env.DB.prepare(
    'SELECT id, at, steam_id, name, text FROM chat_messages WHERE channel = ?1 AND id > ?2 ORDER BY id DESC LIMIT ' + PAGE
  ).bind(ch, after).all();
  for (const m of results.reverse()) out += tsv('M', m.id, m.at, m.steam_id, m.name, m.text);
  return out;
}

export async function write(req, env) {
  const body = await req.text();
  if (body.length > 2048) return [413, 'too big'];
  const [id, key, ch, ...rest] = body.split('\n');
  const text = Array.from(rest.join(' ').replace(/[\u0000-\u001f\u007f]/g, ' ').trim()).slice(0, MAX_TEXT).join('');
  if (!CHANNELS.has(ch) || !text) return [400, 'bad'];
  const p = await player(env, id, key);
  if (!p) return [403, 'unknown key'];
  const now = Date.now();
  const pun = await punished(env, id, Math.floor(now / 1000));
  if (pun) return [403, pun.kind === 'ban' ? 'banned' : 'muted ' + pun.until];
  const last = await env.DB.prepare('SELECT max(at) AS at FROM chat_messages WHERE steam_id = ?1').bind(id).first();
  if (last && last.at && now - last.at < SLOW_MS) return [429, 'slow'];
  await env.DB.prepare('INSERT INTO chat_messages (channel, steam_id, name, text, at) VALUES (?1, ?2, ?3, ?4, ?5)')
    .bind(ch, id, p.name, text, now).run();
  return [200, 'ok'];
}

export async function moderate(req, env) {
  const body = await req.text();
  if (body.length > 512) return [413, 'too big'];
  const [id, key, action, target, daysS] = body.split('\n');
  if (!isMod(env, id) || !(await player(env, id, key))) return [403, 'not a moderator'];
  if (!STEAM_ID.test(target || '')) return [400, 'bad target'];
  const now = Math.floor(Date.now() / 1000);
  const put = (kind, until) => env.DB.prepare(
    `INSERT INTO chat_bans (steam_id, kind, until, by, at) VALUES (?1, ?2, ?3, ?4, ?5)
     ON CONFLICT(steam_id) DO UPDATE SET kind = ?2, until = ?3, by = ?4, at = ?5`
  ).bind(target, kind, until, id, now).run();
  if (action === 'mute') {
    const days = Number(daysS);
    if (!(days > 0 && days <= 3650)) return [400, 'bad days'];
    await put('mute', now + Math.round(days * 86400));
  } else if (action === 'ban') await put('ban', 0);
  else if (action === 'unban') await env.DB.prepare('DELETE FROM chat_bans WHERE steam_id = ?1').bind(target).run();
  else if (action === 'del') {
    await env.DB.batch([env.DB.prepare('DELETE FROM chat_messages WHERE steam_id = ?1').bind(target), bump(env)]);
  } else return [400, 'bad action'];
  return [200, 'ok'];
}

// the cron trigger: everything goes
export async function wipe(env) {
  await env.DB.batch([env.DB.prepare('DELETE FROM chat_messages'), bump(env)]);
}
