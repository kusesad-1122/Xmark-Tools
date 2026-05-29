#!/usr/bin/env node
/**
 * 苳季苦涩环境助手 - 安装统计服务器
 * 用法: node stats-server.js [端口]
 */
const http = require('http');
const fs   = require('fs');
const path = require('path');

const PORT       = process.argv[2] || 3000;
const DATA_FILE  = path.join(__dirname, 'stats.json');
const SECRET_KEY = '3d90947e62384678a8721014a8647316'; // 与模块内一致

function loadData() {
    try { return JSON.parse(fs.readFileSync(DATA_FILE, 'utf8')); }
    catch { return { total: 0, records: [] }; }
}
function saveData(d) { fs.writeFileSync(DATA_FILE, JSON.stringify(d, null, 2)); }

const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Content-Type', 'application/json');

    // GET / — 可视化查看页面
    if (req.method === 'GET' && url.pathname === '/') {
        const data = loadData();
        const byVersion = {};
        data.records.forEach(r => { byVersion[r.version] = (byVersion[r.version]||0)+1; });
        const rows = Object.entries(byVersion).sort((a,b)=>b[1]-a[1])
            .map(([v,c])=>`<tr><td>${v}</td><td>${c}</td><td>${(c/data.total*100).toFixed(1)}%</td></tr>`).join('');
        res.setHeader('Content-Type','text/html;charset=utf-8');
        res.end(`<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>安装统计 · 苳季苦涩</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:-apple-system,sans-serif;max-width:600px;margin:40px auto;padding:0 20px;background:#f2f2f7;color:#1c1c1e}
h2{margin-bottom:4px}
.sub{color:#8e8e93;font-size:14px;margin-bottom:24px}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.total{font-size:52px;font-weight:700;color:#0a84ff;line-height:1}
.label{font-size:15px;color:#8e8e93;margin-top:4px}
table{width:100%;border-collapse:collapse}
th,td{padding:10px 0;text-align:left;border-bottom:1px solid #f2f2f7;font-size:14px}
th{font-weight:600;color:#8e8e93}
</style></head><body>
<h2>苳季苦涩环境助手</h2>
<div class="sub">安装统计数据</div>
<div class="card">
  <div class="total">${data.total}</div>
  <div class="label">累计安装次数</div>
</div>
<div class="card">
  <table>
    <tr><th>版本</th><th>安装数</th><th>占比</th></tr>
    ${rows||'<tr><td colspan="3" style="color:#8e8e93">暂无数据</td></tr>'}
  </table>
</div>
<p style="color:#c7c7cc;font-size:12px;text-align:center">仅统计版本号，不记录设备信息</p>
</body></html>`);
        return;
    }

    // POST /api/install — 记录安装（需要密钥）
    if (req.method === 'POST' && url.pathname === '/api/install') {
        // 密钥验证：盗版/修改版没有密钥，直接拒绝
        const key = req.headers['x-module-key'];
        if (key !== SECRET_KEY) {
            res.statusCode = 403;
            res.end(JSON.stringify({ ok: false, msg: 'invalid key' }));
            return;
        }
        let body = '';
        req.on('data', c => body += c);
        req.on('end', () => {
            try {
                const info = JSON.parse(body || '{}');
                const data = loadData();
                data.total++;
                data.records.push({
                    time:    new Date().toISOString(),
                    version: info.version || 'unknown',
                });
                if (data.records.length > 5000) data.records = data.records.slice(-5000);
                saveData(data);
                res.end(JSON.stringify({ ok: true, total: data.total }));
            } catch {
                res.statusCode = 400;
                res.end(JSON.stringify({ ok: false }));
            }
        });
        return;
    }

    // GET /api/stats — 查询统计
    if (req.method === 'GET' && url.pathname === '/api/stats') {
        const data = loadData();
        const byVersion = {};
        data.records.forEach(r => { byVersion[r.version] = (byVersion[r.version]||0)+1; });
        res.end(JSON.stringify({ total: data.total, byVersion }));
        return;
    }

    res.statusCode = 404;
    res.end(JSON.stringify({ error: 'not found' }));
});

server.listen(PORT, () => {
    console.log(`统计服务器已启动: http://0.0.0.0:${PORT}`);
    console.log(`  查看统计: http://服务器IP:${PORT}/`);
    console.log(`  上报接口: POST /api/install (需密钥)`);
});
