/**
 * 苳季苦涩环境助手 - 安装统计 (Cloudflare Worker 版 · v2)
 *
 * 部署步骤：
 * 1. cloudflare.com 注册账号（免费）
 * 2. Workers & Pages → Create Worker → 把这段代码贴入 → Save and Deploy
 * 3. 创建 KV Namespace（名字随意，例如 stats-kv-namespace）
 * 4. Worker → Settings → Variables and Secrets → KV Namespace Bindings → Add
 *    Variable name 一定要填 STATS_KV（区分大小写）
 *    KV namespace 选刚才创建的那个
 * 5. 把 Worker 的 *.workers.dev 域名填到 service.sh 的 STATS_URL
 *
 * v2 改动：
 * - 所有 KV 访问 try/catch，把 TypeError/绑定缺失等问题转成 JSON 错误而不是 500
 * - 加 GET /api/health 健康检查，可直接浏览器打开诊断
 * - GET / 永远先尝试展示页面，KV 出错时显示具体原因而不是白屏
 */

const SECRET_KEY = '3d90947e62384678a8721014a8647316'; // 与 service.sh 内一致

function jsonResponse(obj, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(obj, null, 2), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, X-Module-Key',
      ...extraHeaders,
    },
  });
}

function checkKv(env) {
  if (!env) return { ok: false, msg: 'env is undefined' };
  if (!env.STATS_KV) return { ok: false, msg: 'env.STATS_KV is undefined — KV binding 缺失或变量名拼错（必须为 STATS_KV）' };
  if (typeof env.STATS_KV.get !== 'function') return { ok: false, msg: 'env.STATS_KV.get 不是函数，绑定可能损坏' };
  return { ok: true };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const method = request.method;

    if (method === 'OPTIONS') {
      return new Response(null, {
        headers: {
          'Access-Control-Allow-Origin': '*',
          'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
          'Access-Control-Allow-Headers': 'Content-Type, X-Module-Key',
        },
      });
    }

    // ── GET /api/health 健康检查（无需密钥） ──
    if (method === 'GET' && url.pathname === '/api/health') {
      const kvStatus = checkKv(env);
      let kvTest = null;
      if (kvStatus.ok) {
        try {
          const v = await env.STATS_KV.get('total');
          kvTest = { ok: true, total: v || '0' };
        } catch (e) {
          kvTest = { ok: false, msg: String(e && e.message || e) };
        }
      }
      return jsonResponse({
        worker: 'ok',
        kv_binding: kvStatus,
        kv_readwrite: kvTest,
        time: new Date().toISOString(),
      });
    }

    // ── POST /api/install 记录安装（需密钥） ──
    if (method === 'POST' && url.pathname === '/api/install') {
      if (request.headers.get('X-Module-Key') !== SECRET_KEY) {
        return jsonResponse({ ok: false, msg: 'invalid key' }, 403);
      }
      const kvStatus = checkKv(env);
      if (!kvStatus.ok) {
        return jsonResponse({ ok: false, msg: kvStatus.msg }, 500);
      }
      let info = {};
      try {
        info = await request.json();
      } catch (e) {
        // body 解析失败仍允许继续，version 默认为 unknown
      }
      const version = String(info.version || 'unknown').slice(0, 50);

      try {
        const total = parseInt((await env.STATS_KV.get('total')) || '0', 10) + 1;
        await env.STATS_KV.put('total', String(total));
        const vKey = 'ver:' + version;
        const vCount = parseInt((await env.STATS_KV.get(vKey)) || '0', 10) + 1;
        await env.STATS_KV.put(vKey, String(vCount));
        return jsonResponse({ ok: true, total, version });
      } catch (e) {
        return jsonResponse({ ok: false, msg: 'kv_error: ' + String(e && e.message || e) }, 500);
      }
    }

    // ── GET / 可视化统计页面 ──
    if (method === 'GET' && url.pathname === '/') {
      const kvStatus = checkKv(env);
      let total = 0;
      let versions = [];
      let errorMsg = null;

      if (!kvStatus.ok) {
        errorMsg = kvStatus.msg;
      } else {
        try {
          total = parseInt((await env.STATS_KV.get('total')) || '0', 10);
          const list = await env.STATS_KV.list({ prefix: 'ver:' });
          for (const k of list.keys) {
            versions.push({
              v: k.name.slice(4),
              c: parseInt((await env.STATS_KV.get(k.name)) || '0', 10),
            });
          }
          versions.sort((a, b) => b.c - a.c);
        } catch (e) {
          errorMsg = String(e && e.message || e);
        }
      }

      const rows = versions.map(x =>
        `<tr><td>${x.v}</td><td>${x.c}</td><td>${total ? (x.c / total * 100).toFixed(1) : 0}%</td></tr>`
      ).join('');

      const errorBanner = errorMsg
        ? `<div class="err">⚠️ KV 访问异常：${errorMsg}<br><br>请到 Cloudflare Dashboard → Worker → Settings → Variables → KV Namespace Bindings 检查变量名是否为 <code>STATS_KV</code>，并已绑定到一个 KV 命名空间。<br><br>诊断接口：<a href="/api/health">/api/health</a></div>`
        : '';

      const html = `<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>安装统计 · 苳季苦涩</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:-apple-system,sans-serif;max-width:600px;margin:40px auto;padding:0 20px;background:#f2f2f7;color:#1c1c1e}
h2{margin-bottom:4px}.sub{color:#8e8e93;font-size:14px;margin-bottom:24px}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.total{font-size:52px;font-weight:700;color:#0a84ff;line-height:1}
.label{font-size:15px;color:#8e8e93;margin-top:4px}
table{width:100%;border-collapse:collapse}
th,td{padding:10px 0;text-align:left;border-bottom:1px solid #f2f2f7;font-size:14px}
th{font-weight:600;color:#8e8e93}
.err{background:#fff3cd;border:1px solid #ffeaa7;color:#664d03;padding:14px;border-radius:10px;margin-bottom:16px;font-size:13px;line-height:1.5}
.err code{background:#fff;padding:1px 6px;border-radius:4px;font-family:monospace}
.err a{color:#0a84ff}
</style></head><body>
<h2>苳季苦涩环境助手</h2>
<div class="sub">安装统计数据</div>
${errorBanner}
<div class="card"><div class="total">${total}</div><div class="label">累计安装次数</div></div>
<div class="card"><table>
  <tr><th>版本</th><th>安装数</th><th>占比</th></tr>
  ${rows || '<tr><td colspan="3" style="color:#8e8e93">暂无数据</td></tr>'}
</table></div>
<p style="color:#c7c7cc;font-size:12px;text-align:center">仅统计版本号，不记录设备信息 · <a href="/api/health" style="color:#c7c7cc">健康检查</a></p>
</body></html>`;

      return new Response(html, {
        headers: {
          'Content-Type': 'text/html;charset=utf-8',
          'Access-Control-Allow-Origin': '*',
        },
      });
    }

    return jsonResponse({ error: 'not found', path: url.pathname }, 404);
  },
};
