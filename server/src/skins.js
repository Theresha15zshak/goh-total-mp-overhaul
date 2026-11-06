// Players' camouflages, for the others in their match to see. Only a player signed in with Steam sends; what is stored is what the
// mod packed (dll/skin_pack.h): small DXT pictures, never a PNG, and a text saying which vehicle wears what.
//   POST /skin/pic    x-steam, x-key; the package: answers its SHA-256
//   GET  /skin/pic?h= the package
//   POST /skin/set    x-steam, x-key; the player's list ("GOHSKIN1\n..."): answers its SHA-256
//   GET  /skin/set?id= the list
//   POST /skin/room   x-steam, x-key; "lobby\nplayer number": this player in that match
//   GET  /skin/room?room= "number\tsteam_id\tlist's hash" per player of the match who has a list
const STEAM_ID = /^7656119\d{10}$/, KEY = /^[0-9a-f]{64}$/, HASH = /^[0-9a-f]{64}$/, ROOM = /^\d{10,20}$/;
const MAX_PIC = 900 * 1024, MAX_SET = 64 * 1024, MAX_PICS = 300, MAX_BYTES = 40 * 1024 * 1024, ROOM_S = 6 * 3600;

const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');
const sha256 = async data => hex(await crypto.subtle.digest('SHA-256', data));
const plain = (body, status = 200) => new Response(body, { status, headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' } });

async function who(req, env) {
  const id = req.headers.get('x-steam') || '', key = req.headers.get('x-key') || '';
  if (!STEAM_ID.test(id) || !KEY.test(key)) return null;
  const p = await env.DB.prepare('SELECT key_hash, verified_at FROM players WHERE steam_id = ?1').bind(id).first();
  if (!p || !p.key_hash || !p.verified_at) return null;
  return p.key_hash === await sha256(new TextEncoder().encode(key)) ? id : null;
}

function goodPicture(b) {
  if (b.length <= 8 || b.length > MAX_PIC || b[0] !== 71 || b[1] !== 83 || b[2] !== 75 || b[3] !== 49) return false;
  const w = b[4] | b[5] << 8, h = b[6] | b[7] << 8;
  return w >= 4 && h >= 4 && w <= 1024 && h <= 1024 && w % 4 === 0 && h % 4 === 0;
}

export async function handle(req, env, url) {
  const post = req.method === 'POST';
  if (url.pathname === '/skin/pic' && post) {
    const id = await who(req, env);
    if (!id) return plain('not signed in', 403);
    const body = await req.arrayBuffer(), bytes = new Uint8Array(body);
    if (!goodPicture(bytes)) return plain('not a picture', 400);
    const hash = await sha256(body);
    if (await env.DB.prepare('SELECT 1 FROM skin_pics WHERE hash = ?1').bind(hash).first()) return plain(hash);
    const used = await env.DB.prepare('SELECT count(*) AS n, coalesce(sum(size), 0) AS bytes FROM skin_pics WHERE owner = ?1').bind(id).first();
    if (used.n >= MAX_PICS || used.bytes + bytes.length > MAX_BYTES) return plain('too many pictures', 413);
    await env.DB.prepare('INSERT OR IGNORE INTO skin_pics (hash, data, size, owner, at) VALUES (?1, ?2, ?3, ?4, ?5)')
      .bind(hash, body, bytes.length, id, Math.floor(Date.now() / 1000)).run();
    return plain(hash);
  }
  if (url.pathname === '/skin/pic' && req.method === 'GET') {
    const h = url.searchParams.get('h') || '';
    if (!HASH.test(h)) return plain('bad hash', 400);
    const r = await env.DB.prepare('SELECT data FROM skin_pics WHERE hash = ?1').bind(h).first();
    if (!r) return plain('none', 404);
    return new Response(new Uint8Array(r.data), { headers: { 'content-type': 'application/octet-stream', 'cache-control': 'public, max-age=31536000, immutable' } });
  }
  if (url.pathname === '/skin/set' && post) {
    const id = await who(req, env);
    if (!id) return plain('not signed in', 403);
    const body = await req.text();
    if (body.length > MAX_SET || !body.startsWith('GOHSKIN1\n')) return plain('not a list', 400);
    const hash = await sha256(new TextEncoder().encode(body));
    await env.DB.prepare(`INSERT INTO skin_sets (steam_id, list, hash, at) VALUES (?1, ?2, ?3, ?4)
      ON CONFLICT(steam_id) DO UPDATE SET list = ?2, hash = ?3, at = ?4`).bind(id, body, hash, Math.floor(Date.now() / 1000)).run();
    return plain(hash);
  }
  if (url.pathname === '/skin/set' && req.method === 'GET') {
    const id = url.searchParams.get('id') || '';
    if (!STEAM_ID.test(id)) return plain('bad id', 400);
    const r = await env.DB.prepare('SELECT list FROM skin_sets WHERE steam_id = ?1').bind(id).first();
    return r ? plain(r.list) : plain('none', 404);
  }
  if (url.pathname === '/skin/room' && post) {
    const id = await who(req, env);
    if (!id) return plain('not signed in', 403);
    const [room, number] = (await req.text()).split('\n');
    if (!ROOM.test(room || '') || !/^\d{1,2}$/.test(number || '')) return plain('bad room', 400);
    await env.DB.prepare(`INSERT INTO skin_rooms (room, number, steam_id, at) VALUES (?1, ?2, ?3, ?4)
      ON CONFLICT(room, number) DO UPDATE SET steam_id = ?3, at = ?4`).bind(room, Number(number), id, Math.floor(Date.now() / 1000)).run();
    return plain('ok');
  }
  if (url.pathname === '/skin/room' && req.method === 'GET') {
    const room = url.searchParams.get('room') || '';
    if (!ROOM.test(room)) return plain('bad room', 400);
    const { results } = await env.DB.prepare(
      `SELECT r.number, r.steam_id, s.hash FROM skin_rooms r JOIN skin_sets s ON s.steam_id = r.steam_id WHERE r.room = ?1 AND r.at >= ?2`
    ).bind(room, Math.floor(Date.now() / 1000) - ROOM_S).all();
    return plain(results.map(r => `${r.number}\t${r.steam_id}\t${r.hash}\n`).join(''));
  }
  return null;
}

export async function clean(env) {
  await env.DB.prepare('DELETE FROM skin_rooms WHERE at < ?1').bind(Math.floor(Date.now() / 1000) - ROOM_S).run();
}
