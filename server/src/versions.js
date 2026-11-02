// The mod's official builds: the SHA-256 of each iphlpapi.dll released (tools/release.py puts them here).
//   GET /versions        for the DLL: "sha256\tversion\tkind\treleased_at\tcurrent" per build, newest first
//                        (kind: full / competitive; current 1 = the one to have)
//   GET /versions.html   the same as a page, with how to check a file by hand: a changed DLL can say anything
//                        about itself, the hash taken outside the game cannot lie
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

async function list(env) {
  const { results } = await env.DB.prepare(
    'SELECT sha256, version, kind, released_at, current FROM mod_versions ORDER BY released_at DESC, kind LIMIT 200').all();
  return results;
}

export async function text(env) {
  return (await list(env)).map(v => [v.sha256, v.version, v.kind, v.released_at, v.current].join('\t') + '\n').join('');
}

export async function page(env) {
  const rows = (await list(env)).map(v => `<tr${v.current ? ' class="cur"' : ''}><td>${esc(v.version)}</td><td>${esc(v.kind)}</td>
<td>${new Date(v.released_at * 1000).toISOString().slice(0, 10)}</td><td>${v.current ? 'текущая' : 'старая'}</td><td><code>${esc(v.sha256)}</code></td></tr>`).join('');
  return new Response(`<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>GoH mod: официальные сборки</title>
<style>body{font:15px system-ui;margin:24px;background:#16181c;color:#ddd}td,th{padding:4px 10px;text-align:left}
code{font-size:12px;word-break:break-all}.cur td{color:#d9a441}pre{background:#0d0e10;padding:10px;overflow:auto}</style>
<h1>Официальные сборки мода</h1>
<p>Проверить свой файл (PowerShell или командная строка):</p>
<pre>certutil -hashfile "...\\Call to Arms - Gates of Hell\\binaries\\x64\\iphlpapi.dll" SHA256</pre>
<p>Хеш должен совпасть с одной из строк ниже. Если нет, файл не официальный.</p>
<table><tr><th>Версия</th><th>Сборка</th><th>Дата</th><th></th><th>SHA-256</th></tr>${rows}</table>`,
    { headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' } });
}
