#!/usr/bin/env node
// Bitaxe Deck – lokaler Server
// - liefert das Dashboard (./public) aus
// - Proxy für die AxeOS-API des Bitaxe (umgeht CORS), nur zu privaten IPs im LAN
// - Proxy + Cache für CoinGecko / mempool.space (schont Rate-Limits)
// Keine npm-Abhängigkeiten nötig.

import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import dns from 'node:dns/promises';
import net from 'node:net';

const PORT = Number(process.env.PORT || 8420);
const HOST = process.env.HOST || '127.0.0.1';
const PUBLIC_DIR = join(fileURLToPath(new URL('.', import.meta.url)), 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.json': 'application/json; charset=utf-8',
  '.ico': 'image/x-icon',
};

// Externe Quellen: Prefix -> Basis-URL und Cache-Dauer (ms)
const UPSTREAMS = {
  coingecko: { base: 'https://api.coingecko.com/api/v3', ttl: 60_000 },
  mempool: { base: 'https://mempool.space/api', ttl: 20_000 },
};
const cache = new Map();

function send(res, status, body, headers = {}) {
  res.writeHead(status, { 'Cache-Control': 'no-store', ...headers });
  res.end(body);
}
const sendJson = (res, status, obj) =>
  send(res, status, JSON.stringify(obj), { 'Content-Type': MIME['.json'] });

// --- Bitaxe-Proxy -----------------------------------------------------------

function isPrivateIp(ip) {
  if (net.isIPv4(ip)) {
    const [a, b] = ip.split('.').map(Number);
    return a === 10 || a === 127 || (a === 172 && b >= 16 && b <= 31) ||
      (a === 192 && b === 168) || (a === 169 && b === 254);
  }
  if (net.isIPv6(ip)) {
    const l = ip.toLowerCase();
    return l === '::1' || l.startsWith('fc') || l.startsWith('fd') || l.startsWith('fe80');
  }
  return false;
}

async function resolveDevice(host) {
  // erlaubt "192.168.1.50", "192.168.1.50:80" oder "bitaxe.local"
  const clean = String(host || '').trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
  if (!/^[a-z0-9.\-:\[\]]+$/i.test(clean)) throw new Error('Ungültige Adresse');
  const m = clean.match(/^\[?([^\]]+?)\]?(?::(\d+))?$/);
  let hostname = m[1], port = m[2] || '80';
  if (net.isIP(clean)) { hostname = clean; port = '80'; }
  const ip = net.isIP(hostname) ? hostname : (await dns.lookup(hostname)).address;
  if (!isPrivateIp(ip)) throw new Error('Nur Geräte im lokalen Netz (private IP) erlaubt');
  return { ip, port };
}

const BITAXE_PATHS = new Set(['system/info', 'system/asic', 'system/statistics', 'system/statistics/dashboard']);

async function proxyBitaxe(req, res, url) {
  const path = url.pathname.replace(/^\/bitaxe\//, '');
  if (!BITAXE_PATHS.has(path)) return sendJson(res, 404, { error: 'Unbekannter Endpunkt' });
  const target = url.searchParams.get('host') || process.env.BITAXE_IP;
  if (!target) return sendJson(res, 400, { error: 'Keine Bitaxe-Adresse angegeben' });
  try {
    const { ip, port } = await resolveDevice(target);
    const hostPart = net.isIPv6(ip) ? `[${ip}]` : ip;
    const r = await fetch(`http://${hostPart}:${port}/api/${path}`, { signal: AbortSignal.timeout(4000) });
    const body = await r.text();
    send(res, r.status, body, { 'Content-Type': MIME['.json'] });
  } catch (e) {
    const code = e.cause?.code || e.code;
    const reason = e.name === 'TimeoutError' ? 'Zeitüberschreitung – Gerät antwortet nicht'
      : ({ ECONNREFUSED: 'Verbindung abgelehnt', EHOSTUNREACH: 'Host nicht erreichbar', ENETUNREACH: 'Netz nicht erreichbar',
           ENOTFOUND: 'Hostname nicht gefunden', ECONNRESET: 'Verbindung abgebrochen' }[code] || e.message);
    sendJson(res, 502, { error: reason });
  }
}

// --- Externe APIs mit Cache --------------------------------------------------

async function proxyExternal(res, name, rest, search) {
  const up = UPSTREAMS[name];
  const key = name + rest + search;
  const hit = cache.get(key);
  if (hit && Date.now() - hit.t < up.ttl) {
    return send(res, 200, hit.body, { 'Content-Type': MIME['.json'], 'X-Cache': 'HIT' });
  }
  try {
    const r = await fetch(up.base + rest + search, {
      headers: { accept: 'application/json', 'user-agent': 'bitaxe-deck/1.0' },
      signal: AbortSignal.timeout(10_000),
    });
    const body = await r.text();
    if (r.ok) {
      cache.set(key, { t: Date.now(), body });
      return send(res, 200, body, { 'Content-Type': MIME['.json'], 'X-Cache': 'MISS' });
    }
    if (hit) return send(res, 200, hit.body, { 'Content-Type': MIME['.json'], 'X-Cache': 'STALE' });
    send(res, r.status, body, { 'Content-Type': MIME['.json'] });
  } catch (e) {
    if (hit) return send(res, 200, hit.body, { 'Content-Type': MIME['.json'], 'X-Cache': 'STALE' });
    sendJson(res, 502, { error: e.message });
  }
}

// --- Statische Dateien -------------------------------------------------------

async function serveStatic(res, pathname) {
  const rel = normalize(pathname === '/' ? '/index.html' : pathname).replace(/^(\.\.[/\\])+/, '');
  const file = join(PUBLIC_DIR, rel);
  if (!file.startsWith(PUBLIC_DIR)) return send(res, 403, 'Forbidden');
  try {
    const data = await readFile(file);
    send(res, 200, data, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
  } catch {
    send(res, 404, 'Not found');
  }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  if (req.method !== 'GET') return send(res, 405, 'Method not allowed');
  if (url.pathname.startsWith('/bitaxe/')) return proxyBitaxe(req, res, url);
  const ext = url.pathname.match(/^\/ext\/(coingecko|mempool)(\/.*)$/);
  if (ext) return proxyExternal(res, ext[1], ext[2], url.search);
  if (url.pathname === '/config') return sendJson(res, 200, { defaultHost: process.env.BITAXE_IP || '' });
  serveStatic(res, url.pathname);
});

server.listen(PORT, HOST, () => {
  console.log(`\n  ⛏  Bitaxe Deck läuft:  http://${HOST === '0.0.0.0' ? 'localhost' : HOST}:${PORT}\n`);
  if (process.env.BITAXE_IP) console.log(`  Standard-Bitaxe: ${process.env.BITAXE_IP}`);
});
