const express = require('express');
const multer  = require('multer');
const { WebSocketServer } = require('ws');
const { spawn } = require('child_process');
const http  = require('http');
const path  = require('path');
const fs    = require('fs');
const os    = require('os');
const crypto = require('crypto');

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocketServer({ server });

const ROOT    = path.resolve(__dirname, '..');   // /…/tmp  — where "idce" binary lives
const BINARY  = path.join(ROOT, 'idce');
const RUNS_DIR = path.join(__dirname, 'runs');   // temp per-run dirs

fs.mkdirSync(RUNS_DIR, { recursive: true });

// ── Upload config ──────────────────────────────────────────────────────────
const storage = multer.diskStorage({
  destination: (_req, _file, cb) => cb(null, os.tmpdir()),
  filename:    (_req,  file, cb) => cb(null, `${Date.now()}-${file.originalname}`),
});
const upload = multer({ storage });

// ── Static frontend ────────────────────────────────────────────────────────
app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json());

// ── Per-client WS registry (keyed by runId) ───────────────────────────────
const runClients = new Map(); // runId → Set<ws>

wss.on('connection', (ws, req) => {
  const runId = new URL(req.url, 'http://x').searchParams.get('runId');
  if (!runId) { ws.close(); return; }
  if (!runClients.has(runId)) runClients.set(runId, new Set());
  runClients.get(runId).add(ws);
  ws.on('close', () => runClients.get(runId)?.delete(ws));
});

function broadcast(runId, msg) {
  const clients = runClients.get(runId);
  if (!clients) return;
  const data = JSON.stringify(msg);
  clients.forEach(ws => { try { ws.send(data); } catch {} });
}

// ── Helper: parse summary.txt ──────────────────────────────────────────────
function parseSummary(txt) {
  const stats = { functions: 0, blocks: 0, liveStmts: 0 };
  const lines = txt.split('\n');
  lines.forEach(l => {
    const fm = l.match(/Final number of reachable functions:\s*(\d+)/);
    if (fm) stats.functions = parseInt(fm[1]);
    const bm = l.match(/Block\s+\d+\s+\((\d+) live stmts\)/);
    if (bm) { stats.blocks++; stats.liveStmts += parseInt(bm[1]); }
  });
  return stats;
}

// ── Helper: run IDCE binary ────────────────────────────────────────────────
async function runIDCE({ inputPath, mode, runId, outDir }) {
  return new Promise((resolve, reject) => {
    const args = [outDir];
    if (mode === 'ml') args.push('--ml-dce');

    broadcast(runId, { type: 'stage', stage: 0 }); // Parse

    const proc = spawn(BINARY, args, {
      cwd: ROOT,
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    const inputStream = fs.createReadStream(inputPath);
    inputStream.pipe(proc.stdin);

    let stdout = '';
    let stderr = '';

    // Stream stdout (DCE logs) to websocket clients
    proc.stdout.on('data', chunk => {
      const text = chunk.toString();
      stdout += text;
      text.split('\n').filter(Boolean).forEach(line => {
        let logType = 'info';
        if (line.includes('Removing')) logType = 'remove';
        else if (line.includes('Warning') || line.includes('Vetoed')) logType = 'warn';
        else if (line.includes('ML')) logType = 'ml';
        else if (line.includes('SCCP') || line.includes('Folding')) logType = 'fold';
        broadcast(runId, { type: 'log', text: line, logType, time: new Date().toLocaleTimeString('en-GB') });
      });
    });

    proc.stderr.on('data', chunk => {
      const text = chunk.toString();
      stderr += text;
      text.split('\n').filter(Boolean).forEach(line => {
        broadcast(runId, { type: 'log', text: line, logType: 'error', time: new Date().toLocaleTimeString('en-GB') });
      });
    });

    proc.on('close', code => {
      if (code !== 0) {
        broadcast(runId, { type: 'error', message: stderr || 'Analysis failed' });
        return reject(new Error(stderr || 'IDCE exited with error'));
      }

      // Read outputs
      const dotPath     = path.join(ROOT, outDir, 'graph.dot');
      const summaryPath = path.join(ROOT, outDir, 'summary.txt');

      const dot     = fs.existsSync(dotPath)     ? fs.readFileSync(dotPath, 'utf8')     : '';
      const summary = fs.existsSync(summaryPath) ? fs.readFileSync(summaryPath, 'utf8') : '';

      broadcast(runId, { type: 'stage', stage: 3 }); // Export done
      broadcast(runId, { type: 'done', dot, summary, stats: parseSummary(summary) });
      resolve({ dot, summary, stats: parseSummary(summary) });
    });
  });
}

// ── API: /api/analyze ──────────────────────────────────────────────────────
app.post('/api/analyze', upload.single('ssa'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'No SSA file uploaded' });

  const mode  = req.body.mode || 'classical'; // 'classical' | 'ml'
  const runId = crypto.randomBytes(6).toString('hex');
  const outDir = `gui_run_${runId}`;

  res.json({ runId });                         // respond immediately so client can connect WS

  // Small delay so the client has time to open WS
  await new Promise(r => setTimeout(r, 300));

  broadcast(runId, { type: 'log', text: `[IDCE] Starting ${mode === 'ml' ? 'ML-Guided' : 'Classical'} DCE...`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'log', text: `[IDCE] Input: ${req.file.originalname}`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'stage', stage: 1 }); // Feature extract

  try {
    await runIDCE({ inputPath: req.file.path, mode, runId, outDir });
    // Cleanup temp input file
    fs.unlink(req.file.path, () => {});
    // Cleanup run output dir
    setTimeout(() => {
      try { fs.rmSync(path.join(ROOT, outDir), { recursive: true, force: true }); } catch {}
    }, 60_000);
  } catch (err) {
    broadcast(runId, { type: 'error', message: err.message });
  }
});

// ── API: /api/sample — serve a built-in sample SSA file ───────────────────
app.get('/api/sample/:name', (req, res) => {
  const allowed = ['input.ssa', 'input2.ssa', 'input3.ssa', 'a-A.cpp.021t.ssa'];
  const name = req.params.name;
  if (!allowed.includes(name)) return res.status(404).json({ error: 'Not found' });
  const filePath = path.join(ROOT, 'inputs', name);
  if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'File not found' });
  res.download(filePath, name);
});

// ── API: /api/samples — list available built-in samples ───────────────────
app.get('/api/samples', (_req, res) => {
  const inputsDir = path.join(ROOT, 'inputs');
  try {
    const files = fs.readdirSync(inputsDir)
      .filter(f => f.endsWith('.ssa'))
      .map(f => ({ name: f, size: fs.statSync(path.join(inputsDir, f)).size }));
    res.json(files);
  } catch {
    res.json([]);
  }
});

// ── API: /api/analyze-sample — run a built-in sample ─────────────────────
app.post('/api/analyze-sample', express.json(), async (req, res) => {
  const { name, mode } = req.body;
  const allowed = ['input.ssa', 'input2.ssa', 'input3.ssa', 'a-A.cpp.021t.ssa'];
  if (!allowed.includes(name)) return res.status(400).json({ error: 'Invalid sample' });

  const inputPath = path.join(ROOT, 'inputs', name);
  if (!fs.existsSync(inputPath)) return res.status(404).json({ error: 'File not found' });

  const runId  = crypto.randomBytes(6).toString('hex');
  const outDir = `gui_run_${runId}`;

  res.json({ runId });

  await new Promise(r => setTimeout(r, 300));

  broadcast(runId, { type: 'log', text: `[IDCE] Starting ${mode === 'ml' ? 'ML-Guided' : 'Classical'} DCE...`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'log', text: `[IDCE] Sample: ${name}`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'stage', stage: 1 });

  try {
    await runIDCE({ inputPath, mode: mode || 'classical', runId, outDir });
    setTimeout(() => {
      try { fs.rmSync(path.join(ROOT, outDir), { recursive: true, force: true }); } catch {}
    }, 60_000);
  } catch (err) {
    broadcast(runId, { type: 'error', message: err.message });
  }
});

// ── Start ──────────────────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`IDCE GUI running at http://localhost:${PORT}`);
});
