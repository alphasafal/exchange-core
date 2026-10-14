// Drives the WebAssembly engine from a requestAnimationFrame loop.
//
// The engine runs synchronously on the main thread on purpose: it is fast
// enough that a batch per frame keeps the page responsive, and moving it to a
// worker would add a message hop to every interaction for no visible gain at
// this scale. The batch size is what bounds frame time, not a timer.

let demo = null;
let running = true;
let batch = 4000;

// Exponential moving average of the per-batch cost, not a running average over
// the whole session. A cumulative figure only ever drifts: it mixes the first
// frame, when the book is empty and every command is cheap, with the steady
// state, and it cannot show the cost changing when you move a slider. The
// smoothing is heavy because a single frame that lands on a GC pause is not
// news.
let nsPerCommand = 0;
const SMOOTHING = 0.08;

const $ = (id) => document.getElementById(id);
const fmt = new Intl.NumberFormat('en-US');
const fmt1 = new Intl.NumberFormat('en-US', { maximumFractionDigits: 1 });

function renderLevels(el, levels, side, maxQty) {
  if (!levels.length) {
    el.innerHTML = `<tr><td colspan="3" style="color:var(--dim);text-align:center">empty</td></tr>`;
    return;
  }
  const rows = levels.map((l) => {
    const pct = maxQty > 0 ? (l.quantity / maxQty) * 100 : 0;
    return `<tr class="${side}row">
      <td><span class="bar" style="width:${pct.toFixed(1)}%"></span><span class="cellv">${fmt.format(l.price)}</span></td>
      <td><span class="cellv">${fmt.format(l.quantity)}</span></td>
      <td><span class="cellv">${fmt.format(l.orders)}</span></td>
    </tr>`;
  });
  el.innerHTML = rows.join('');
}

function render() {
  const d = demo.depth(10);
  const s = demo.stats();

  const all = [...d.bids, ...d.asks].map((l) => l.quantity);
  const maxQty = all.length ? Math.max(...all) : 0;

  // Asks are rendered worst-price-first so the book reads outward from the
  // spread in the middle, the way a trader's ladder does.
  renderLevels($('asks'), d.asks.slice().reverse(), 'ask', maxQty);
  renderLevels($('bids'), d.bids, 'bid', maxQty);

  const bid = s.bestBid, ask = s.bestAsk;
  $('spread').textContent =
    bid && ask ? `spread ${fmt.format(ask - bid)}  ·  mid ${fmt1.format((ask + bid) / 2)}`
               : 'one side empty';

  $('s-commands').textContent = fmt.format(s.commands);
  $('s-resting').textContent = fmt.format(s.resting);
  $('s-trades').textContent = fmt.format(s.trades);
  $('s-ns').textContent = nsPerCommand ? fmt1.format(nsPerCommand) : '—';
  $('s-rejects').textContent = fmt.format(s.rejects);
  $('s-lastreject').textContent =
    s.rejects > 0 && s.lastReject !== 'None' ? ` · last: ${s.lastReject}` : '';

  const badge = $('haltbadge');
  badge.textContent = s.halted ? 'halted' : 'trading';
  badge.className = s.halted ? 'badge on' : 'badge';

  const tape = demo.tape();
  $('tape').innerHTML = tape.length
    ? tape.map((t) =>
        `<div><span class="${t.buy ? 'b' : 's'}">${t.buy ? 'BUY ' : 'SELL'}</span>
         <span>${fmt.format(t.quantity)} @ ${fmt.format(t.price)}</span></div>`).join('')
    : '<div style="color:var(--dim);border:0">no trades yet</div>';
}

function tick() {
  if (running && demo) {
    const ns = demo.step(batch);
    const perCommand = ns / batch;
    nsPerCommand = nsPerCommand === 0 ? perCommand
                                      : nsPerCommand + SMOOTHING * (perCommand - nsPerCommand);
  }
  if (demo) render();
}

// requestAnimationFrame is the right pacing for this: it matches the display
// and stops entirely in a background tab, so a page left open overnight is not
// burning a core on a demo nobody is watching.
//
// It does not fire everywhere, though. Some embedded webviews and automation
// panes never run a frame even with document.visibilityState === "visible" --
// which is how this was found. A page whose whole content depends on rAF shows
// an empty shell there and gives no clue why, so a watchdog falls back to a
// timer if the first frame has not arrived shortly after start.
function startLoop() {
  let usingRaf = false;

  const rafLoop = () => {
    usingRaf = true;
    tick();
    requestAnimationFrame(rafLoop);
  };
  requestAnimationFrame(rafLoop);

  setTimeout(() => {
    if (usingRaf) return;
    console.info('exchange-core: requestAnimationFrame is not firing here; using a timer.');
    setInterval(() => {
      if (document.hidden) return;   // match rAF: do no work while nobody can see it
      tick();
    }, 33);
  }, 600);
}

function wire() {
  $('run').onclick = () => {
    running = !running;
    $('run').textContent = running ? 'Pause' : 'Run';
  };
  $('burst').onclick = () => demo.step(50000);
  $('reset').onclick = () => { demo.reset(0); render(); };
  $('halt').onclick = () => {
    const halted = demo.stats().halted;
    if (halted) { demo.resume(); $('halt').textContent = 'Trip kill switch'; }
    else { demo.halt(); $('halt').textContent = 'Resume trading'; }
  };
  $('policy').onchange = (e) => { demo.setPolicy(Number(e.target.value)); render(); };
  $('maxqty').oninput = (e) => {
    $('maxqtyv').textContent = e.target.value;
    demo.setMaxOrderQuantity(Number(e.target.value));
  };
  $('collar').oninput = (e) => {
    const v = Number(e.target.value);
    $('collarv').textContent = v === 0 ? 'off' : `${v} bps`;
    demo.setCollarBps(v);
  };
}

Module.onRuntimeInitialized = () => {
  demo = new Module.Demo();
  $('build').textContent = Module.Demo.build();
  wire();
  startLoop();
};
