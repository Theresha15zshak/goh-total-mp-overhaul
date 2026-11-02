// Steam sign-in (Steam OpenID 2.0): proves a SteamID belongs to the key that asks. The player signs in on
// steamcommunity.com itself (the game's Steam overlay or a browser); we get only the SteamID, signed by Steam,
// and check that signature with Steam. No password, session or token of theirs ever reaches us.
//
//   POST /verify/start   "steam_id\nkey"   -> the address to open: /verify?t=TOKEN (one use, 15 minutes)
//   GET  /verify?t=      on to Steam's sign-in page
//   GET  /verify/back    Steam's answer: checked with Steam, then the SteamID is this key's, verified (taken
//                        from whichever key held it before)
//   POST /verify/status  "steam_id\nkey"   -> "verified" / "anon" (not verified with this key) / "held" (another
//                        key has it)
// Players who have not signed in are "anonymous": their name in the chat is Anonymous#### and they report no
// matches (matches.js, chat.js).
const STEAM_ID = /^7656119\d{10}$/;
const KEY = /^[0-9a-f]{64}$/;
const STEAM_LOGIN = 'https://steamcommunity.com/openid/login';
const CLAIMED = /^https:\/\/steamcommunity\.com\/openid\/id\/(7656119\d{10})$/;
const TOKEN_S = 900, STARTS_PER_HOUR = 10;

export async function sha256(s) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

// the chat name of a player not signed in: the same for the same key
export const anonName = keyHash => 'Anonymous' + (1000 + (parseInt((keyHash || '0').slice(0, 8), 16) % 9000));

const page = (title, body, status = 200) => new Response(
  `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>${title}</title>
<style>body{font:16px system-ui;margin:0;min-height:100vh;display:grid;place-items:center;background:#16181c;color:#ddd}
main{max-width:520px;padding:24px}h1{font-size:22px;color:#d9a441}</style><main><h1>${title}</h1>${body}</main>`,
  { status, headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' } });

export async function start(req, env, origin) {
  const body = await req.text();
  if (body.length > 256) return [413, 'too big'];
  const [id, key] = body.split('\n');
  if (!STEAM_ID.test(id || '') || !KEY.test(key || '')) return [400, 'bad'];
  const hash = await sha256(key), now = Math.floor(Date.now() / 1000);
  const n = await env.DB.prepare('SELECT count(*) AS n FROM verify_tokens WHERE key_hash = ?1 AND at > ?2').bind(hash, now - 3600).first();
  if (n.n >= STARTS_PER_HOUR) return [429, 'too many'];
  const r = new Uint8Array(24);
  crypto.getRandomValues(r);
  const token = [...r].map(b => b.toString(16).padStart(2, '0')).join('');
  await env.DB.batch([
    env.DB.prepare('DELETE FROM verify_tokens WHERE at < ?1').bind(now - 3600),
    env.DB.prepare('INSERT INTO verify_tokens (token, key_hash, at) VALUES (?1, ?2, ?3)').bind(token, hash, now),
  ]);
  return [200, `${origin}/verify?t=${token}`];
}

export function toSteam(url) {
  const t = url.searchParams.get('t') || '';
  if (!/^[0-9a-f]{48}$/.test(t)) return page('Ссылка неверна / Wrong link', '<p>Откройте вход заново из игры.<br>Start the sign-in again from the game.</p>', 400);
  const q = new URLSearchParams({
    'openid.ns': 'http://specs.openid.net/auth/2.0',
    'openid.mode': 'checkid_setup',
    'openid.return_to': `${url.origin}/verify/back?t=${t}`,
    'openid.realm': `${url.origin}/`,
    'openid.identity': 'http://specs.openid.net/auth/2.0/identifier_select',
    'openid.claimed_id': 'http://specs.openid.net/auth/2.0/identifier_select',
  });
  return Response.redirect(`${STEAM_LOGIN}?${q}`, 302);
}

export async function back(url, env) {
  const t = url.searchParams.get('t') || '';
  const p = url.searchParams;
  const fail = (why, en) => page('Не получилось / It did not work', `<p>${why}<br>${en}</p><p>Откройте вход заново из игры.<br>Start the sign-in again from the game.</p>`, 400);
  if (!/^[0-9a-f]{48}$/.test(t) || p.get('openid.mode') !== 'id_res' || p.get('openid.op_endpoint') !== STEAM_LOGIN ||
      p.get('openid.return_to') !== `${url.origin}/verify/back?t=${t}`)
    return fail('Steam ответил не то, что ждали.', 'Steam answered something unexpected.');
  const m = CLAIMED.exec(p.get('openid.claimed_id') || '');
  if (!m || p.get('openid.identity') !== p.get('openid.claimed_id')) return fail('Steam не назвал аккаунт.', 'Steam named no account.');
  const now = Math.floor(Date.now() / 1000);
  const tok = await env.DB.prepare('SELECT key_hash, at FROM verify_tokens WHERE token = ?1').bind(t).first();
  if (!tok || tok.at < now - TOKEN_S) return fail('Ссылка устарела.', 'The link has expired.');
  // Steam itself says whether it signed this answer
  const check = new URLSearchParams();
  for (const [k, v] of p) if (k.startsWith('openid.')) check.set(k, v);
  check.set('openid.mode', 'check_authentication');
  const r = await fetch(STEAM_LOGIN, { method: 'POST', body: check, headers: { 'content-type': 'application/x-www-form-urlencoded' } });
  if (!r.ok || !/^is_valid\s*:\s*true\s*$/m.test(await r.text())) return fail('Steam не подтвердил вход.', 'Steam did not confirm the sign-in.');
  const id = m[1];
  await env.DB.batch([
    env.DB.prepare('DELETE FROM verify_tokens WHERE token = ?1').bind(t),
    env.DB.prepare(
      `INSERT INTO players (steam_id, name, version, last_seen, key_hash, verified_at) VALUES (?1, '?', '-', 0, ?2, ?3)
       ON CONFLICT(steam_id) DO UPDATE SET key_hash = ?2, verified_at = ?3`
    ).bind(id, tok.key_hash, now),
  ]);
  return page('Готово / Done', `<p>Аккаунт Steam <b>${id}</b> подтверждён. Можно закрыть эту страницу и вернуться в игру.</p>
<p>Steam account <b>${id}</b> is confirmed. You can close this page and go back to the game.</p>
<p style="color:#999">Если в игре вы под другим аккаунтом Steam, войдите здесь под ним и повторите.<br>
If the game runs under another Steam account, sign in with that one here and try again.</p>`);
}

export async function status(req, env) {
  const body = await req.text();
  if (body.length > 256) return [413, 'too big'];
  const [id, key] = body.split('\n');
  if (!STEAM_ID.test(id || '') || !KEY.test(key || '')) return [400, 'bad'];
  const row = await env.DB.prepare('SELECT key_hash, verified_at FROM players WHERE steam_id = ?1').bind(id).first();
  const hash = await sha256(key);
  if (row && row.key_hash && row.key_hash !== hash) return [200, 'held'];
  return [200, row && row.verified_at ? 'verified' : 'anon'];
}
