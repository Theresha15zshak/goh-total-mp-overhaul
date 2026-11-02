// goh-online: who is playing Gates of Hell with the mod right now.
//   POST /hb      body "steam_id\nname\nversion\nkey" (UTF-8): the DLL says it is online, every 2 minutes
//                 200 ok, 403 the SteamID is held by another key
//   GET  /online  plain text for the DLL: "count\n" then "steam_id\tname\tversion\tseconds_ago\n" per player
//   GET  /        the same list as a page
// The SteamID comes from the game and cannot be verified here (that takes Steam). The key can: the DLL makes
// a random one at its first start, the first heartbeat with it claims the SteamID, and from then on nothing
// is taken for that SteamID without it. Only its SHA-256 is kept. DLLs from before keys send none: taken
// for a SteamID no key holds yet, refused once one does.

const ONLINE_S = 300;          // seen within the last 5 minutes = online
const MAX_LIST = 200;

const STEAM_ID = /^7656119\d{10}$/;
const VERSION = /^[0-9A-Za-z._-]{1,16}$/;
const KEY = /^[0-9a-f]{64}$/;

async function sha256(s) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function clean_name(s) {
  // no control characters (the list is tab/newline separated), at most 32 characters
  s = s.replace(/[\u0000-\u001f\u007f]/g, '').trim();
  return Array.from(s).slice(0, 32).join('') || '?';
}

function text(body, status = 200) {
  return new Response(body, { status, headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' } });
}

async function heartbeat(req, env) {
  const len = Number(req.headers.get('content-length') || 0);
  if (len > 512) return text('too big', 413);
  const body = await req.text();
  if (body.length > 512) return text('too big', 413);
  const [id = '', name = '', version = '', key = ''] = body.split('\n');
  if (!STEAM_ID.test(id) || !VERSION.test(version) || (key && !KEY.test(key))) return text('bad', 400);
  const hash = key ? await sha256(key) : null;
  const held = await env.DB.prepare('SELECT key_hash FROM players WHERE steam_id = ?1').bind(id).first();
  if (held && held.key_hash && held.key_hash !== hash) return text('held by another key', 403);
  const now = Math.floor(Date.now() / 1000);
  // the WHERE keeps a key that got in between the two statements; the first key in claims the row
  const r = await env.DB.prepare(
    `INSERT INTO players (steam_id, name, version, last_seen, key_hash) VALUES (?1, ?2, ?3, ?4, ?5)
     ON CONFLICT(steam_id) DO UPDATE SET name = ?2, version = ?3, last_seen = ?4, key_hash = coalesce(players.key_hash, ?5)
     WHERE players.key_hash IS NULL OR players.key_hash = ?5`
  ).bind(id, clean_name(name), version, now, hash).run();
  if (!r.meta.changes) return text('held by another key', 403);
  return text('ok');
}

async function online(env) {
  const now = Math.floor(Date.now() / 1000);
  const { results } = await env.DB.prepare(
    `SELECT steam_id, name, version, last_seen FROM players WHERE last_seen >= ?1 ORDER BY last_seen DESC LIMIT ${MAX_LIST}`
  ).bind(now - ONLINE_S).all();
  return results.map(r => ({ ...r, ago: Math.max(0, now - r.last_seen) }));
}

function esc(s) {
  return s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function page(list) {
  const rows = list.map(p =>
    `<tr><td><a href="https://steamcommunity.com/profiles/${p.steam_id}">${esc(p.name)}</a></td><td>${esc(p.version)}</td><td>${p.ago} с назад</td></tr>`
  ).join('');
  return `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>GoH онлайн</title>
<style>body{font:15px system-ui;margin:24px;background:#16181c;color:#ddd}a{color:#9cf}td,th{padding:4px 12px;text-align:left}</style>
<h1>Сейчас в игре с модом: ${list.length}</h1>
<table><tr><th>Игрок</th><th>Версия</th><th>Был</th></tr>${rows}</table>`;
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/hb' && req.method === 'POST') return heartbeat(req, env);
    if (url.pathname === '/online' && req.method === 'GET') {
      const list = await online(env);
      return text(list.length + '\n' + list.map(p => `${p.steam_id}\t${p.name}\t${p.version}\t${p.ago}\n`).join(''));
    }
    if (url.pathname === '/' && req.method === 'GET') {
      return new Response(page(await online(env)), { headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' } });
    }
    return text('not found', 404);
  },
};
