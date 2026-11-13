'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '..', 'web', 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'index.html'), 'utf8');
const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function element() {
  return {
    hidden: false, disabled: false, value: '', textContent: '', children: [], dataset: {}, handlers: {},
    files: [], href: '/',
    addEventListener(name, handler) { this.handlers[name] = handler; },
    replaceChildren(...children) { this.children = children; },
    append(child) { this.children.push(child); },
    removeAttribute(name) { delete this[name]; }
  };
}
async function harness(saved = '') {
  const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(match => [match[1], element()]));
  for (const id of ['player-panel', 'report', 'retry', 'intake']) elements.get(id).hidden = true;
  const storage = new Map(saved ? [['csda-job', saved]] : []);
  const requests = [], timers = new Map(), uploads = [];
  let timerId = 0;
  class Xhr {
    constructor() { this.upload = {}; uploads.push(this); }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader() {}
    send(file) { this.file = file; }
    abort() { this.aborted = true; if (this.onabort) this.onabort(); }
  }
  const context = vm.createContext({
    document: { getElementById: id => elements.get(id), createElement: element },
    sessionStorage: {
      getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value),
      removeItem: key => storage.delete(key)
    },
    Option: function (text, value) { this.textContent = text; this.value = value; },
    XMLHttpRequest: Xhr, AbortController, console,
    setTimeout: (callback, delay) => { const id = ++timerId; timers.set(id, { callback, delay }); return id; },
    clearTimeout: id => timers.delete(id),
    fetch(url, options) {
      if (url === '/api/session' && options.method === 'GET')
        return Promise.resolve({ ok: true, status: 200, json: async () => ({ csrf: 'fixture', upload_max_bytes: 1048576 }) });
      const pending = deferred();
      requests.push({
        url, method: options.method, signal: options.signal, pending,
        respond(data, status = 200) {
          pending.resolve({ ok: status >= 200 && status < 300, status, json: async () => data });
        },
        reject(error = new TypeError('Synthetic connection failure')) { pending.reject(error); }
      });
      // Deliberately allow late replies despite abort: generation checks must protect the UI.
      return pending.promise;
    }
  });
  vm.runInContext(source, context, { filename: 'web/app.js' });
  await flush();
  return {
    context, requests, timers, uploads, storage, $: id => elements.get(id),
    run: expression => vm.runInContext(expression, context),
    activate(id = 'job-a') { vm.runInContext(`jobId = ${JSON.stringify(id)}; sessionStorage.setItem('csda-job', jobId);`, context); },
    poll() { vm.runInContext('void poll()', context); },
    fire(id, type = 'click') { return elements.get(id).handlers[type]({ preventDefault() {} }); },
    async tick() {
      assert.equal(timers.size, 1, 'exactly one recovery/progress timer');
      const [id, timer] = timers.entries().next().value; timers.delete(id);
      timer.callback(); await flush(); return timer;
    }
  };
}
function job(id = 'job-a', state = 'select') {
  return {
    id, state, expires: 1000, server_time: 100, source_sha256: 'synthetic-source-hash',
    players: [{ name: `Player ${id}`, steam_id: '76561198000000001' }],
    phases: [{ cpu_seconds: 1, elapsed_ms: 1000, peak_memory_bytes: 1048576, peak_working_set_bytes: 1048576 }],
    upload_ms: 1000, upload_bytes: 1024, artifacts: { 'report.html': { bytes: 2000 } }
  };
}
function result() {
  return {
    metadata: { name: 'Fixture report' }, summary: { rounds: 3, kills: 2, deaths: 1, assists: 0, kd: 2, adr: 50, headshot_percent: 50 },
    weapons: [{ weapon: '<untrusted>', kills: 2, headshots: 1 }], warnings: []
  };
}
async function clear(h) {
  const clearing = h.fire('clear'); await flush();
  const request = h.requests.at(-1); assert.equal(request.method, 'DELETE');
  request.respond({ state: 'cancelling' }, 202);
  await clearing; await flush();
}
function cleared(h) {
  assert.equal(h.run('jobId'), '');
  assert.equal(h.storage.has('csda-job'), false);
  assert.equal(h.$('player-panel').hidden, true);
  assert.equal(h.$('report').hidden, true);
  assert.equal(h.$('player').children.length, 0);
  assert.equal(h.$('download').href, undefined);
  assert.equal(h.$('receipt').href, undefined);
  assert.equal(h.timers.size, 0);
  assert.equal(h.run('uploading'), false);
}
test('late status JSON cannot restore a deleted roster or links', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  const old = h.requests[0], json = deferred();
  old.pending.resolve({ ok: true, status: 200, json: () => json.promise }); await flush();
  await clear(h);
  json.resolve(job()); await flush();
  assert.equal(old.signal.aborted, true); cleared(h);
  assert.equal(h.$('state').textContent, 'Cleared');
});
test('late result cannot restore a cancelled report', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  h.requests[0].respond(job('job-a', 'complete')); await flush();
  const oldResult = h.requests.at(-1); assert.equal(oldResult.url, '/api/jobs/job-a/result');
  await clear(h); oldResult.respond(result()); await flush();
  cleared(h); assert.equal(h.run('reportId'), '');
});
test('obsolete status does not disturb the replacement job', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  const old = h.requests[0];
  await clear(h); h.activate('job-b'); h.poll(); await flush();
  h.requests.at(-1).respond(job('job-b')); await flush();
  old.respond(job()); await flush();
  assert.equal(h.run('jobId'), 'job-b');
  assert.equal(h.$('player').dataset.job, 'job-b');
  assert.equal(h.$('player').children[0].textContent, 'Player job-b / 76561198000000001');
  assert.equal(h.$('player-panel').hidden, false);
  assert.equal(h.timers.size, 1);
});
test('delete aborts upload and stale callbacks cannot leave controls busy', async () => {
  const h = await harness();
  h.$('demo-file').files = [{ name: 'fixture.dem', size: 100 }];
  const starting = h.fire('start'); await flush();
  h.requests[0].respond({ id: 'job-a' }, 201); await flush();
  assert.equal(h.uploads.length, 1);
  const xhr = h.uploads[0];
  const clearing = h.fire('clear'); await flush();
  assert.equal(xhr.aborted, true);
  const count = h.requests.length; await h.fire('build'); assert.equal(h.requests.length, count);
  h.requests.at(-1).respond({ state: 'cancelling' }, 202); await clearing; await starting;
  xhr.upload.onprogress({ lengthComputable: true, loaded: 100, total: 100 }); await flush();
  cleared(h); assert.equal(h.$('build').disabled, false); assert.equal(h.$('start').disabled, false);
  assert.equal(h.$('progress-label').textContent, 'No active job');
});
test('late selection response does not poll or re-enable an obsolete job', async () => {
  const h = await harness(); h.activate();
  h.$('player').value = '76561198000000001';
  const building = h.fire('build'); await flush();
  const selection = h.requests[0]; assert.equal(selection.method, 'POST');
  await clear(h); const count = h.requests.length;
  selection.respond(job('job-a', 'analysis_queued'), 202); await building; await flush();
  cleared(h); assert.equal(h.requests.length, count); assert.equal(h.$('build').disabled, false);
});
test('failed deletion retains the job and resumes only its status', async () => {
  const h = await harness(); h.activate();
  const clearing = h.fire('clear'); await flush();
  h.requests[0].respond({ error: 'Deletion temporarily unavailable' }, 503); await clearing; await flush();
  assert.equal(h.run('jobId'), 'job-a'); assert.equal(h.storage.get('csda-job'), 'job-a');
  assert.equal(h.$('clear').disabled, false); assert.equal(h.$('build').disabled, false);
  assert.equal(h.requests.at(-1).url, '/api/jobs/job-a'); assert.equal(h.requests.at(-1).method, 'GET');
  h.requests.at(-1).respond(job('job-a', 'queued')); await flush(); assert.equal(h.timers.size, 1);
});
test('404 deletion uses status, not English error substring matching', async () => {
  const h = await harness(); h.activate();
  const clearing = h.fire('clear'); await flush();
  h.requests[0].respond({ error: 'Gone' }, 404); await clearing; await flush(); cleared(h);
});
test('terminal deletion failures retain ownership without automatic requests', async () => {
  for (const status of [401, 403]) {
    const h = await harness(); h.activate();
    const clearing = h.fire('clear'); await flush();
    h.requests[0].respond({ error: 'Authorization denied' }, status); await clearing; await flush();
    assert.equal(h.run('jobId'), 'job-a'); assert.equal(h.storage.get('csda-job'), 'job-a');
    assert.equal(h.requests.length, 1); assert.equal(h.timers.size, 0);
    assert.equal(h.$('retry').hidden, true); assert.equal(h.$('clear').disabled, false);
  }
});
test('transient poll rejection recovers and resets backoff without resubmission', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  h.requests[0].reject(); await flush();
  assert.equal(h.timers.values().next().value.delay, 1200);
  await h.tick(); h.requests.at(-1).respond(job('job-a', 'queued')); await flush();
  await h.tick(); h.requests.at(-1).reject(); await flush();
  assert.equal(h.timers.values().next().value.delay, 1200);
  assert.ok(h.requests.every(request => request.method === 'GET' && request.url === '/api/jobs/job-a'));
});
test('failed result cycles exhaust bounded retries; visible Retry recovers the same job', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  for (let attempt = 0; attempt < 6; attempt++) {
    h.requests.at(-1).respond(job('job-a', 'complete')); await flush();
    assert.equal(h.requests.at(-1).url, '/api/jobs/job-a/result');
    h.requests.at(-1).reject(); await flush();
    if (attempt < 5) {
      assert.equal(h.timers.values().next().value.delay, [1200, 2400, 4800, 9600, 15000][attempt]);
      await h.tick();
    }
  }
  assert.equal(h.timers.size, 0); assert.equal(h.$('retry').hidden, false);
  assert.equal(h.$('clear').disabled, false);
  await h.fire('retry'); await flush();
  h.requests.at(-1).respond(job('job-a', 'complete')); await flush();
  h.requests.at(-1).respond(result()); await flush();
  assert.equal(h.$('report').hidden, false); assert.equal(h.$('retry').hidden, true);
  assert.equal(h.$('download').href, '/api/jobs/job-a/report');
  assert.ok(h.requests.every(request => request.method === 'GET'));
});
test('401, 403, 404 and confirmed expiry are terminal, without retry timers', async () => {
  for (const status of [401, 403, 404]) {
    const h = await harness(); h.activate(); h.poll(); await flush();
    h.requests[0].respond({ error: `Synthetic HTTP ${status}` }, status); await flush();
    assert.equal(h.timers.size, 0); assert.equal(h.$('retry').hidden, true);
    assert.equal(h.requests.length, 1); assert.equal(h.$('report').hidden, true);
  }
  const h = await harness(); h.activate(); h.poll(); await flush();
  h.requests[0].respond({ ...job(), expires: 100, server_time: 100 }); await flush();
  assert.equal(h.timers.size, 0); assert.equal(h.$('retry').hidden, true);
  assert.match(h.$('status-detail').textContent, /expired/);
});
test('cancel invalidates even an already-dispatched recovery timer', async () => {
  const h = await harness(); h.activate(); h.poll(); await flush();
  h.requests[0].reject(); await flush();
  const callback = h.timers.values().next().value.callback;
  await clear(h); const count = h.requests.length; callback(); await flush();
  cleared(h); assert.equal(h.requests.length, count);
});
test('restored session still polls its valid active job', async () => {
  const h = await harness('job-a');
  assert.equal(h.requests[0].url, '/api/jobs/job-a');
  h.requests[0].respond(job()); await flush();
  assert.equal(h.$('player-panel').hidden, false);
  assert.equal(h.timers.size, 1); assert.equal(h.storage.get('csda-job'), 'job-a');
});
