// The server admin's page: everyone who ever registered with the mod (the players table), behind a token.
//   GET  /admin   a form asking for the token
//   POST /admin   form field "token": with the right one, the list
// The token is a Worker secret (wrangler secret put ADMIN_TOKEN): not in the code, not in the database, and the
// page never puts it in an address (no ?token=..., which would stay in the browser's history and the logs).
// Nothing is remembered between requests: each look at the list asks for the token again.
import { sha256 } from './verify.js';

const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const html = (body, status = 200) => new Response(`<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<meta name="robots" content="noindex"><title>GoH TOTAL MP OVERHAUL: players</title>
<style>body{font:14px system-ui;margin:24px;background:#16181c;color:#ddd}a{color:#9cf}td,th{padding:4px 10px;text-align:left;border-bottom:1px solid #2a2d33}
th{position:sticky;top:0;background:#16181c;cursor:pointer}input{font:inherit;padding:6px;background:#0d0e10;color:#ddd;border:1px solid #444}
button{font:inherit;padding:6px 14px}.v{color:#7c6}.a{color:#999}code{font-size:12px}</style>${body}`,
  { status, headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store', 'x-robots-tag': 'noindex' } });

const form = (msg = '') => html(`<h1>Players / Игроки</h1>${msg ? `<p style="color:#e66">${esc(msg)}</p>` : ''}
<form method="post"><input type="password" name="token" placeholder="admin token" autofocus size="40"> <button>Open / Открыть</button></form>`);

async function tokenOk(env, token) {
  if (!env.ADMIN_TOKEN || !token) return false;
  return (await sha256(token)) === (await sha256(env.ADMIN_TOKEN));   // same length, compared as hashes
}

export async function page(req, env) {
  if (req.method !== 'POST') return form();
  const data = await req.formData().catch(() => null);
  if (!(await tokenOk(env, data && data.get('token')))) return form('Wrong token / Неверный токен');
  const now = Math.floor(Date.now() / 1000);
  const { results } = await env.DB.prepare(
    `SELECT p.steam_id, p.name, p.version, p.last_seen, p.verified_at, p.key_hash IS NOT NULL AS has_key,
            (SELECT count(*) FROM match_players mp WHERE mp.steam_id = p.steam_id) AS matches,
            (SELECT max(mmr.mu) FROM ratings mmr WHERE mmr.steam_id = p.steam_id AND mmr.nation = 'all') AS mu
     FROM players p ORDER BY p.last_seen DESC`
  ).all();
  const when = t => (t ? new Date(t * 1000).toISOString().replace('T', ' ').slice(0, 16) : '');
  const ago = t => {
    if (!t) return '';
    const s = now - t;
    return s < 300 ? 'online' : s < 86400 ? `${Math.round(s / 3600)} h` : `${Math.round(s / 86400)} d`;
  };
  const rows = results.map(p => `<tr><td><a href="https://steamcommunity.com/profiles/${esc(p.steam_id)}" target="_blank">${esc(p.name)}</a></td>
<td><code>${esc(p.steam_id)}</code></td><td class="${p.verified_at ? 'v' : 'a'}">${p.verified_at ? 'Steam ✔' : 'anonymous'}</td>
<td>${esc(p.version)}</td><td data-t="${p.last_seen}">${when(p.last_seen)} <span class="a">${ago(p.last_seen)}</span></td>
<td data-t="${p.matches}">${p.matches}</td><td data-t="${p.mu ?? -999}">${p.mu == null ? '' : Math.round(1500 + 40 * (p.mu - 25))}</td></tr>`).join('');
  const verified = results.filter(p => p.verified_at).length, online = results.filter(p => now - p.last_seen < 300).length;
  return html(`<h1>Players / Игроки: ${results.length}</h1>
<p>Signed in with Steam / вошли через Steam: ${verified} · online now / сейчас онлайн: ${online}</p>
<p><input id="q" placeholder="search name or SteamID / поиск" size="40" oninput="f()"></p>
<table id="t"><thead><tr><th>Name / Ник</th><th>SteamID</th><th>Sign-in / Вход</th><th>Version / Версия</th>
<th>Last seen / Был</th><th>Rated matches / Матчей</th><th>MMR (best)</th></tr></thead><tbody>${rows}</tbody></table>
<script>
function f(){const q=document.getElementById('q').value.toLowerCase();for(const r of document.querySelectorAll('#t tbody tr'))r.style.display=r.textContent.toLowerCase().includes(q)?'':'none'}
document.querySelectorAll('#t th').forEach((th,i)=>th.onclick=()=>{const b=document.querySelector('#t tbody'),rs=[...b.rows],d=th.dataset.d=th.dataset.d==='1'?'-1':'1';
rs.sort((x,y)=>{const a=x.cells[i],c=y.cells[i],ta=a.dataset.t,tc=c.dataset.t;const r=ta!==undefined?(+ta)-(+tc):a.textContent.localeCompare(c.textContent);return r*d});rs.forEach(r=>b.appendChild(r))})
</script>`);
}
