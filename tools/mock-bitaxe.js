// Simulierter Bitaxe (AxeOS-API) zum Testen ohne Gerät:  node tools/mock-bitaxe.js  → Adresse 127.0.0.1:8081
import http from 'node:http';
const start = Date.now();
http.createServer((req, res) => {
  if (!req.url.startsWith('/api/system/info')) { res.writeHead(404); return res.end(); }
  const s = (Date.now() - start) / 1000;
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({
    power: 17.6 + Math.random(), voltage: 5100, current: 3450, temp: 60 + Math.random() * 2, vrTemp: 67,
    hashRate: 1180 + Math.random() * 60, expectedHashrate: 1210, bestDiff: '1.24G', bestSessionDiff: '86.1M',
    sharesAccepted: 5000 + Math.floor(s / 5), sharesRejected: 3, uptimeSeconds: 7200 + Math.floor(s),
    hostname: 'bitaxe-mock', ASICModel: 'BM1370', boardVersion: '601', version: 'v2.5.0-mock',
    fanspeed: 42, fanrpm: 3900, frequency: 525, coreVoltage: 1150, coreVoltageActual: 1148, wifiRSSI: -48,
    stratumURL: 'public-pool.io', stratumPort: 21496, stratumUser: 'bc1qexample.bitaxe', poolDifficulty: 1000,
    isUsingFallbackStratum: 0,
  }));
}).listen(8081, '127.0.0.1', () => console.log('Mock-Bitaxe auf http://127.0.0.1:8081'));
