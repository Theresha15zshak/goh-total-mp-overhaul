// Global chat, channels "ru" and "en". Nothing is kept for long: every message goes at the even hours (UTC),
// by the cron trigger (wrangler.jsonc). Writing takes the player's key, as everything else here does.
//
//   GET  /chat?ch=ru&after=N&epoch=E&id=ME   text, a line per field set:
//          "E\tepoch"  messages were taken away since (a wipe or a moderator): with another E the client
//                      drops what it has and gets the whole channel again (after is then ignored)
//          "S\tmute\tuntil" / "S\tban"   ME may not write (a ban also hides the channel)
//          "A"                           ME is a moderator, then "B\tsteam_id\tname\tkind\tuntil" per
//                                        punishment in force
//          "M\tid\tat_ms\tsteam_id\tname\ttext\tverified"   oldest first, at most 100; players not signed in
//                                        with Steam (verify.js) write as Anonymous####, verified 0
//   POST /chat      "steam_id\nkey\nchannel\ntext"            -> "ok" / "muted <until>" / "banned" / "slow" / "flood" /
//                   "repeat" ...; links to sites not in ALLOWED_LINKS are cut out of the text
//   POST /chatmod   "steam_id\nkey\naction\ntarget\ndays"     moderators only (env.MODERATORS, comma separated):
//                   mute (days) / ban / unban / del (the target's messages in every channel)
import { anonName } from './verify.js';

const STEAM_ID = /^7656119\d{10}$/;
const KEY = /^[0-9a-f]{64}$/;
const CHANNELS = new Set(['ru', 'en']);
const MAX_TEXT = 300, SLOW_MS = 2000, PAGE = 100;
const BURST_MS = 30000, BURST = 5, REPEAT_MS = 60000;     // at most 5 messages in 30 s; the same text again after a minute

// Links: only to these sites (and their subdomains); any other address in a message becomes "[ссылка удалена]".
// An address is anything with "://", "www." or a name.tld (Latin letters), with or without a path.
const ALLOWED_LINKS = ['discord.gg', 'youtube.com', 'youtu.be'];
const LINK = /\b(?:[a-z][a-z0-9+.-]*:\/\/\S+|www\.\S+|(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,}(?::\d+)?(?:[\/?#]\S*)?)/gi;
export function cleanLinks(text) {
  return text.replace(LINK, m => {
    const host = m.replace(/^[a-z][a-z0-9+.-]*:\/\//i, '').split(/[\/?#:]/)[0].toLowerCase();
    return ALLOWED_LINKS.some(d => host === d || host.endsWith('.' + d)) ? m : '[ссылка удалена]';
  });
}

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

// the punishment in force for a player (or for their key, when it is known), if any: { kind, until } (until
// 0 = for good); a ban wins over a mute
async function punished(env, id, now, keyHash = null) {
  const { results } = await env.DB.prepare(
    'SELECT kind, until FROM chat_bans WHERE (steam_id = ?1 OR (?2 IS NOT NULL AND key_hash = ?2)) AND (until = 0 OR until > ?3)'
  ).bind(id, keyHash, now).all();
  return results.find(r => r.kind === 'ban') || results[0] || null;
}

// the reporting player with their key, or null
async function player(env, id, key) {
  if (!STEAM_ID.test(id || '') || !KEY.test(key || '')) return null;
  const p = await env.DB.prepare('SELECT name, key_hash, verified_at FROM players WHERE steam_id = ?1').bind(id).first();
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
    'SELECT id, at, steam_id, name, text, verified FROM chat_messages WHERE channel = ?1 AND id > ?2 ORDER BY id DESC LIMIT ' + PAGE
  ).bind(ch, after).all();
  for (const m of results.reverse()) out += tsv('M', m.id, m.at, m.steam_id, m.name, m.text, m.verified);
  return out;
}

export async function write(req, env) {
  const body = await req.text();
  if (body.length > 2048) return [413, 'too big'];
  const [id, key, ch, ...rest] = body.split('\n');
  const text = cleanLinks(Array.from(rest.join(' ').replace(/[\u0000-\u001f\u007f]/g, ' ').trim()).slice(0, MAX_TEXT).join(''));
  if (!CHANNELS.has(ch) || !text) return [400, 'bad'];
  const p = await player(env, id, key);
  if (!p) return [403, 'unknown key'];
  const now = Date.now();
  const pun = await punished(env, id, Math.floor(now / 1000), p.key_hash);
  if (pun) return [403, pun.kind === 'ban' ? 'banned' : 'muted ' + pun.until];
  const { results: mine } = await env.DB.prepare(
    'SELECT at, text FROM chat_messages WHERE steam_id = ?1 AND at > ?2 ORDER BY at DESC LIMIT ' + BURST
  ).bind(id, now - Math.max(BURST_MS, REPEAT_MS)).all();
  if (mine.length && now - mine[0].at < SLOW_MS) return [429, 'slow'];
  if (mine.filter(m => now - m.at < BURST_MS).length >= BURST) return [429, 'flood'];
  if (mine.some(m => m.text === text)) return [429, 'repeat'];
  const verified = p.verified_at ? 1 : 0;
  await env.DB.prepare('INSERT INTO chat_messages (channel, steam_id, name, text, at, verified) VALUES (?1, ?2, ?3, ?4, ?5, ?6)')
    .bind(ch, id, verified ? p.name : anonName(p.key_hash), text, now, verified).run();
  return [200, 'ok'];
}

export async function moderate(req, env) {
  const body = await req.text();
  if (body.length > 512) return [413, 'too big'];
  const [id, key, action, target, daysS] = body.split('\n');
  if (!isMod(env, id) || !(await player(env, id, key))) return [403, 'not a moderator'];
  if (!STEAM_ID.test(target || '')) return [400, 'bad target'];
  const now = Math.floor(Date.now() / 1000);
  const held = await env.DB.prepare('SELECT key_hash FROM players WHERE steam_id = ?1').bind(target).first();
  const put = (kind, until) => env.DB.prepare(
    `INSERT INTO chat_bans (steam_id, kind, until, by, at, key_hash) VALUES (?1, ?2, ?3, ?4, ?5, ?6)
     ON CONFLICT(steam_id) DO UPDATE SET kind = ?2, until = ?3, by = ?4, at = ?5, key_hash = ?6`
  ).bind(target, kind, until, id, now, held ? held.key_hash : null).run();
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
