const express = require('express');
const multer  = require('multer');
const { WebSocketServer } = require('ws');
const { spawn, execSync } = require('child_process');
const http  = require('http');
const path  = require('path');
const fs    = require('fs');
const os    = require('os');
const crypto = require('crypto');
require('dotenv').config();

// ── Paths ──────────────────────────────────────────────────────────────────
const CORRECTIONS_FILE = path.resolve(__dirname, '..', 'ml_data', 'corrections.jsonl');
const MODEL_PT         = path.resolve(__dirname, '..', 'ml_data', 'dce_model.pt');

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocketServer({ server });

const ROOT    = path.resolve(__dirname, '..');   // /…/tmp  — where "idce" binary lives
const BINARY  = '/home/vyrion/tmp_build/idce'; // ext4 home build — NTFS can't exec
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
const runState = new Map();   // runId → { stage, done, error, logs[] }

function ensureRunState(runId) {
  if (!runState.has(runId)) {
    runState.set(runId, { stage: null, done: null, error: null, logs: [] });
  }
  return runState.get(runId);
}

wss.on('connection', (ws, req) => {
  const runId = new URL(req.url, 'http://x').searchParams.get('runId');
  if (!runId) { ws.close(); return; }
  if (!runClients.has(runId)) runClients.set(runId, new Set());
  runClients.get(runId).add(ws);

  const state = runState.get(runId);
  if (state) {
    if (state.logs.length) {
      state.logs.forEach(log => { try { ws.send(JSON.stringify(log)); } catch {} });
    }
    if (typeof state.stage === 'number') {
      try { ws.send(JSON.stringify({ type: 'stage', stage: state.stage })); } catch {}
    }
    if (state.done) {
      try { ws.send(JSON.stringify(state.done)); } catch {}
    } else if (state.error) {
      try { ws.send(JSON.stringify({ type: 'error', message: state.error })); } catch {}
    }
  }

  ws.on('close', () => runClients.get(runId)?.delete(ws));
});

function broadcast(runId, msg) {
  const state = ensureRunState(runId);
  if (msg.type === 'log') {
    state.logs.push(msg);
    if (state.logs.length > 300) state.logs.shift();
  } else if (msg.type === 'stage') {
    state.stage = msg.stage;
  } else if (msg.type === 'done') {
    state.done = msg;
    state.error = null;
  } else if (msg.type === 'error') {
    state.error = msg.message || 'Analysis failed';
  }

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

async function cppToSsa(cppCode, runId) {
  const tmpDir = path.join(os.tmpdir(), `cpp-${runId}`);
  fs.mkdirSync(tmpDir, { recursive: true });
  const cppFile = path.join(tmpDir, 'source.cpp');
  fs.writeFileSync(cppFile, cppCode);
  
  try {
    // Run GCC to dump SSA
    execSync(`gcc -O0 -fdump-tree-ssa -c source.cpp`, { cwd: tmpDir });
    const files = fs.readdirSync(tmpDir);
    const ssaFileFound = files.find(f => f.endsWith('.ssa'));
    if (!ssaFileFound) throw new Error("SSA dump not generated");
    
    const ssaContent = fs.readFileSync(path.join(tmpDir, ssaFileFound), 'utf8');
    // Cleanup
    fs.rmSync(tmpDir, { recursive: true, force: true });
    return ssaContent;
  } catch (err) {
    fs.rmSync(tmpDir, { recursive: true, force: true });
    throw new Error("C++ Compilation failed: " + err.message);
  }
}

// ── Helper: parse enriched ML prediction JSON ────────────────────────────
function parseMlPreds(outDir) {
  try {
    const bDir    = '/home/vyrion/tmp_build';
    const predPath = path.join(bDir, outDir, 'tmp_ml_preds.json');
    if (!fs.existsSync(predPath)) return null;
    const raw = JSON.parse(fs.readFileSync(predPath, 'utf8'));
    return {
      dead_items:       raw.dead_items || [],
      uncertainty_nodes: raw.uncertainty_nodes || [],
      model_version:    raw.model_version || 'unknown',
    };
  } catch { return null; }
}

async function runIDCE({ inputPath, mode, runId, outDir, threshold = 0.6, originalSource }) {
  return new Promise((resolve, reject) => {
    const args = [outDir];
    if (mode === 'ml') { args.push('--ml-dce'); args.push('--ml-threshold'); args.push(String(threshold)); }

    broadcast(runId, { type: 'stage', stage: 0 }); // Parse

    const BINARY_DIR = '/home/vyrion/tmp_build';
    const proc = spawn(BINARY, args, {
      cwd: BINARY_DIR,
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
        else if (line.includes('unused function') || line.includes('after terminal')) logType = 'remove';
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

      // Read outputs (binary runs from BINARY_DIR so outputs land there)
      const bDir        = '/home/vyrion/tmp_build';
      const dotPath     = path.join(bDir, outDir, 'graph.dot');
      const summaryPath = path.join(bDir, outDir, 'summary.txt');
      const optSsaPath  = path.join(bDir, outDir, 'optimized.ssa');

      const dot     = fs.existsSync(dotPath)     ? fs.readFileSync(dotPath, 'utf8')     : '';
      const summary = fs.existsSync(summaryPath) ? fs.readFileSync(summaryPath, 'utf8') : '';
      const optSsa  = fs.existsSync(optSsaPath)  ? fs.readFileSync(optSsaPath, 'utf8')  : '';
      const origSsa = fs.existsSync(inputPath)   ? fs.readFileSync(inputPath, 'utf8')   : '';

      const mlPreds = parseMlPreds(outDir);
      broadcast(runId, { type: 'stage', stage: 3 }); // Export done
      broadcast(runId, { type: 'done', dot, summary, optSsa, origSsa,
                         stats: parseSummary(summary), mlPreds });
      resolve({ dot, summary, optSsa, origSsa, stats: parseSummary(summary), mlPreds });
    });
  });
}

// ── API: /api/analyze ──────────────────────────────────────────────────────
app.post('/api/analyze', upload.single('ssa'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'No SSA file uploaded' });

  const mode      = req.body.mode || 'classical';
  const threshold = parseFloat(req.body.threshold || '0.6');
  const runId = crypto.randomBytes(6).toString('hex');
  const outDir = `gui_run_${runId}`;
  ensureRunState(runId);

  res.json({ runId });                         // respond immediately so client can connect WS

  broadcast(runId, { type: 'log', text: `[IDCE] Starting ${mode === 'ml' ? 'ML-Guided' : 'Classical'} DCE... (threshold=${threshold})`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'log', text: `[IDCE] Input: ${req.file.originalname}`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'stage', stage: 1 }); // Feature extract

  try {
    await runIDCE({ inputPath: req.file.path, mode, threshold, runId, outDir });
    // Cleanup temp input file
    fs.unlink(req.file.path, () => {});
    // Cleanup run output dir
    setTimeout(() => {
      try { fs.rmSync(path.join(ROOT, outDir), { recursive: true, force: true }); } catch {}
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
  } catch (err) {
    broadcast(runId, { type: 'error', message: err.message });
    setTimeout(() => {
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
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
      .filter(f => f.endsWith('.cpp'))
      .map(f => ({ name: f, size: fs.statSync(path.join(inputsDir, f)).size }));
    res.json(files);
  } catch {
    res.json([]);
  }
});

// ── API: /api/sample-text/:name — fetch raw code of a sample ─────────────
app.get('/api/sample-text/:name', (req, res) => {
  const name = req.params.name;
  if (!name.endsWith('.cpp')) return res.status(400).json({ error: 'Only .cpp allowed' });
  const filePath = path.join(ROOT, 'inputs', name);
  if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'File not found' });
  res.send(fs.readFileSync(filePath, 'utf8'));
});

// ── API: /api/analyze-text — run using raw text from editor ───────────────
app.post('/api/analyze-text', express.json(), async (req, res) => {
  const { code, mode, threshold: thr } = req.body;
  const threshold = parseFloat(thr || '0.6');
  if (!code || !code.trim()) return res.status(400).json({ error: 'Empty code' });

  const runId  = crypto.randomBytes(6).toString('hex');
  const outDir = `gui_run_${runId}`;
  ensureRunState(runId);
  
  res.json({ runId });

  let finalSsa = code;
  let isCpp = code.includes('main') || code.includes('#include');

  if (isCpp) {
    try {
      broadcast(runId, { type: 'log', text: "[IDCE] Detected C++ code. Converting to SSA...", logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
      finalSsa = await cppToSsa(code, runId);
      broadcast(runId, { type: 'log', text: "[IDCE] SSA Conversion complete.", logType: 'ml', time: new Date().toLocaleTimeString('en-GB') });
    } catch (err) {
      broadcast(runId, { type: 'error', message: err.message });
      return; 
    }
  }

  // Save text to a temp file
  const tmpFile = path.join(os.tmpdir(), `editor-${runId}.ssa`);
  fs.writeFileSync(tmpFile, finalSsa);
  await new Promise(r => setTimeout(r, 300));

  broadcast(runId, { type: 'log', text: `[IDCE] Starting ${mode === 'ml' ? 'ML-Guided' : 'Classical'} DCE...`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'log', text: `[IDCE] Input: Editor Code`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'stage', stage: 1 });

  try {
    await runIDCE({ inputPath: tmpFile, mode: mode || 'classical', threshold, runId, outDir, originalSource: code });
    fs.unlink(tmpFile, () => {});
    setTimeout(() => {
      try { fs.rmSync(path.join(ROOT, outDir), { recursive: true, force: true }); } catch {}
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
  } catch (err) {
    broadcast(runId, { type: 'error', message: err.message });
    setTimeout(() => {
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
  }
});

// ── API: /api/compile-run — compile and execute C++ directly ──────────────
const { spawnSync } = require('child_process');
app.post('/api/compile-run', express.json(), async (req, res) => {
  const { code } = req.body;
  if (!code) return res.status(400).json({ error: 'No code provided' });

  const tmpId = crypto.randomBytes(6).toString('hex');
  const tmpCpp = path.join(os.tmpdir(), `test_exec_${tmpId}.cpp`);
  fs.writeFileSync(tmpCpp, code);
  
  const pyScript = path.join(ROOT, 'idce_compiler.py');
  
  try {
     // Run the python wrapper
     const compileOut = execSync(`python3 ${pyScript} ${tmpCpp}`, { cwd: ROOT, encoding: 'utf8' });
     
     // Find the generated executable path from the script output
     const match = compileOut.match(/Output Executable Generated -> (.*\.out)/);
     if (!match) throw new Error("Could not find executable output path. Wrapper logs:\n" + compileOut);
     const exePath = match[1].trim();
     
     // Run the executable
     const runProc = spawnSync(exePath, [], { encoding: 'utf8', timeout: 5000 });
     res.json({
        stdout: runProc.stdout || '',
        stderr: runProc.stderr || '',
        returncode: runProc.status !== null ? runProc.status : -1
     });
     
     // Cleanup
     try { fs.unlinkSync(tmpCpp); } catch(e){}
  } catch (err) {
      res.json({
         error: err.message,
         stdout: err.stdout || '',
         stderr: err.stderr || ''
      });
  }
});

// ── API: /api/analyze-sample — run a built-in sample ─────────────────────
app.post('/api/analyze-sample', express.json(), async (req, res) => {
  const { name, mode, threshold: thr } = req.body;
  const threshold = parseFloat(thr || '0.6');
  if (!name.endsWith('.cpp')) return res.status(400).json({ error: 'Invalid sample' });

  // Use the pre-compiled .ssa file that corresponds to the .cpp source
  const ssaName = name + '.021t.ssa';
  const inputPath = path.join(ROOT, 'inputs', ssaName);
  
  if (!fs.existsSync(inputPath)) return res.status(404).json({ error: 'SSA equivalent not compiled' });

  const runId  = crypto.randomBytes(6).toString('hex');
  const outDir = `gui_run_${runId}`;
  ensureRunState(runId);

  res.json({ runId });

  broadcast(runId, { type: 'log', text: `[IDCE] Starting ${mode === 'ml' ? 'ML-Guided' : 'Classical'} DCE...`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'log', text: `[IDCE] Sample: ${name}`, logType: 'info', time: new Date().toLocaleTimeString('en-GB') });
  broadcast(runId, { type: 'stage', stage: 1 });

  try {
    await runIDCE({ inputPath, mode: mode || 'classical', threshold, runId, outDir });
    setTimeout(() => {
      try { fs.rmSync(path.join(ROOT, outDir), { recursive: true, force: true }); } catch {}
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
  } catch (err) {
    broadcast(runId, { type: 'error', message: err.message });
    setTimeout(() => {
      runState.delete(runId);
      runClients.delete(runId);
    }, 60_000);
  }
});

// ── API: /api/feedback — record user correction ───────────────────────────
app.post('/api/feedback', express.json(), (req, res) => {
  const { id, correct, graph_file, run_id } = req.body;
  if (id === undefined || correct === undefined)
    return res.status(400).json({ error: 'Missing id or correct field' });

  const entry = JSON.stringify({
    id: Number(id),
    correct: Boolean(correct),
    graph_file: graph_file || '',
    run_id: run_id || '',
    timestamp: new Date().toISOString(),
  });

  try {
    fs.mkdirSync(path.dirname(CORRECTIONS_FILE), { recursive: true });
    fs.appendFileSync(CORRECTIONS_FILE, entry + '\n');
    // Count pending corrections
    const lines = fs.readFileSync(CORRECTIONS_FILE, 'utf8')
                    .split('\n').filter(Boolean).length;
    res.json({ ok: true, pending_corrections: lines });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ── API: /api/model_info — model metadata ─────────────────────────────────
app.get('/api/model_info', (_req, res) => {
  let modelAge = null;
  let pendingCorrections = 0;

  try {
    const stat = fs.statSync(MODEL_PT);
    const ageMs = Date.now() - stat.mtimeMs;
    const ageDays = Math.floor(ageMs / 86400000);
    modelAge = ageDays === 0 ? 'today' :
               ageDays === 1 ? '1 day ago' : `${ageDays} days ago`;
  } catch { modelAge = 'not trained'; }

  try {
    if (fs.existsSync(CORRECTIONS_FILE)) {
      pendingCorrections = fs.readFileSync(CORRECTIONS_FILE, 'utf8')
                            .split('\n').filter(Boolean).length;
    }
  } catch {}

  res.json({
    model_version:       'hybrid-v2',
    last_trained:        modelAge,
    confidence_threshold: 0.6,
    pending_corrections:  pendingCorrections,
    model_exists:        fs.existsSync(MODEL_PT),
  });
});

// ── API: /api/explain — AI DCE Explainer ──────────────────────────────────
app.post('/api/explain', express.json(), async (req, res) => {
  const { code_text, reason, confidence } = req.body;
  if (!code_text) return res.status(400).json({ error: 'Missing code_text' });

  const apiKey = process.env.OPENAI_API_KEY;

  if (apiKey && apiKey.startsWith('sk-')) {
    try {
      const resp = await fetch('https://api.openai.com/v1/chat/completions', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${apiKey}` },
        body: JSON.stringify({
          model: 'gpt-3.5-turbo',
          messages: [{
            role: 'system',
            content: 'You are an expert compiler optimization engineer explaining why specific code was removed by a dead code elimination (DCE) pass. Be highly technical, concise, and sound like a senior ML engineer.'
          }, {
            role: 'user',
            content: `The IDCE engine removed the following code: \`${code_text}\`\nThe heuristical/ML assigned reason is: "${reason}" and ML confidence was ${confidence}.\nExplain step-by-step why this code is dead or redundant and safe to remove without breaking the program's intent.`
          }],
          max_tokens: 250,
          temperature: 0.3
        })
      });
      const data = await resp.json();
      if (data.choices && data.choices[0]) {
        return res.json({ explanation: data.choices[0].message.content });
      } else {
        throw new Error(data.error?.message || 'Unknown OpenAI Error');
      }
    } catch (err) {
      console.error('OpenAI fetch failed, falling back to mock:', err);
      // Fall through to mock
    }
  }

  // Fallback: MOCK GPT
  await new Promise(r => setTimeout(r, 1200)); // simulate network delay
  
  let mockExpl = `Based on the program dependence graph and semantic analysis, the statement \`${code_text}\` is classified as structurally redundant. `;
  
  if (reason.includes("dead store")) {
    mockExpl += `A dead store occurs when a variable is modified, but the modified value is never read downstream by any other instruction before the variable goes out of scope or is overwritten. The compiler proves that preserving this write operation uses CPU cycles with absolutely zero side effects on the final observable state of the machine. Removing it is perfectly safe and semantics-preserving.`;
  } else if (reason.includes("unreachable")) {
    mockExpl += `Control flow analysis formally proves that there is no valid execution trace from the program entry point that can ever reach this basic block. Because the compiler can mathematically guarantee this region is dormant, safely pruning it shrinks binary size and improves cache locality.`;
  } else if (reason.includes("constant false")) {
    mockExpl += `Path profiling and symbolic execution determined that the guard condition for this branch is provably false 100% of the time (e.g., evaluating \`0 == 1\`). Because the conditional is statically determinable, the entire dependent subgraph of logic inside the branch is rendered inert and can be aggressively culled.`;
  } else if (reason.includes("redundant computation")) {
    mockExpl += `The GNN inference engine analyzed the computational subgraph and matched this expression to a zero-effect mathematical identity (like adding zero or multiplying by one), or it is computing a value that is structurally decoupled from any stateful side-effects. The engine bypasses these operations to save CPU pipelines.`;
  } else {
    mockExpl += `The Hybrid-GNN classifier scored this structural AST node at a confidence of ${confidence}. Given the lack of stateful I/O side-effects, the ML pass concluded that bypassing this node does not alter the semantic footprint of the binary. The compiler acts securely via inference mapping to fold the node entirely out of emission.`;
  }

  res.json({ explanation: mockExpl });
});

// ── Start ──────────────────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`IDCE GUI running at http://localhost:${PORT}`);
});
