'use strict';
const $ = id => document.getElementById(id);
let csrf = '', jobId = sessionStorage.getItem('csda-job') || '', pollTimer, maximum = 512 * 1024 * 1024;
let uploading = false, reportId = '';
function message(state, detail) { $('state').textContent = state; $('status-detail').textContent = detail; }
function failure(error) { message('Request failed', error.message); }
async function api(path, method = 'GET', body) {
  const response = await fetch(path, { method, credentials: 'same-origin',
    headers: method === 'GET' ? {} : { 'Content-Type': 'application/json', 'X-CSDA-CSRF': csrf },
    body: body === undefined ? undefined : JSON.stringify(body), cache: 'no-store' });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}
function unlocked(session) {
  csrf = session.csrf; maximum = session.upload_max_bytes;
  $('access').hidden = true; $('intake').hidden = false;
  $('limit').textContent = `${(maximum / 1048576).toFixed(0)} MiB maximum`;
  message('Ready', 'Choose a replay. Processing uses one serial CPU worker.');
}
function fileChanged() {
  const file = $('demo-file').files[0];
  $('start').disabled = uploading || Boolean(jobId) || !file || file.size > maximum || !/\.dem$/i.test(file.name);
  $('file-detail').textContent = file ? `${file.name} / ${(file.size / 1048576).toFixed(1)} MiB` : 'Choose a replay.';
}
function upload(file) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open('PUT', `/api/jobs/${jobId}/demo`);
    request.setRequestHeader('Content-Type', 'application/octet-stream');
    request.setRequestHeader('X-CSDA-CSRF', csrf);
    request.timeout = 185000;
    request.upload.onprogress = event => {
      if (event.lengthComputable) {
        const percent = Math.floor(event.loaded * 100 / event.total);
        $('progress').value = percent; $('progress-label').textContent = `Uploading payload: ${percent}%`;
      }
    };
    request.onload = () => {
      try {
        const data = JSON.parse(request.responseText);
        if (request.status < 200 || request.status >= 300) throw new Error(data.error || 'Upload rejected');
        resolve(data);
      } catch (error) { reject(error); }
    };
    request.onerror = () => reject(new Error('Upload connection failed; cancel this job before retrying.'));
    request.ontimeout = () => reject(new Error('Upload timed out; cancel this job before retrying.'));
    request.send(file);
  });
}
function number(value, digits = 1) { return value === null || value === undefined ? 'N/A' : Number(value).toFixed(digits); }
async function showReport(job) {
  if (reportId === job.id) return;
  const result = await api(`/api/jobs/${job.id}/result`);
  reportId = job.id;
  const s = result.summary;
  $('report-title').textContent = result.metadata.name;
  $('report-subtitle').textContent = `${s.rounds} observed rounds / ${job.source_sha256}`;
  $('kda').textContent = `${s.kills} / ${s.deaths} / ${s.assists}`;
  $('kd').textContent = number(s.kd, 3); $('adr').textContent = number(s.adr);
  $('hs').textContent = s.headshot_percent == null ? 'N/A' : `${number(s.headshot_percent)}%`;
  $('weapons').replaceChildren();
  for (const weapon of result.weapons) {
    const row = document.createElement('tr');
    for (const text of [weapon.weapon, weapon.kills, weapon.headshots]) {
      const cell = document.createElement('td'); cell.textContent = String(text); row.append(cell);
    }
    $('weapons').append(row);
  }
  const cpu = job.phases.reduce((sum, p) => sum + p.cpu_seconds, 0);
  const elapsed = job.phases.reduce((sum, p) => sum + p.elapsed_ms, 0) / 1000;
  const peak = Math.max(...job.phases.map(p => p.peak_memory_bytes)) / 1048576;
  const workingSet = Math.max(...job.phases.map(p => p.peak_working_set_bytes)) / 1048576;
  $('resources').textContent = `${number(elapsed)} s worker wall time / ${number(cpu)} CPU-seconds / ${number(workingSet)} MiB peak worker working set / ${number(peak)} MiB peak committed job memory. Limits are provisional, not a measured optimum.`;
  const uploadSeconds = job.upload_ms / 1000;
  $('transfer').textContent = `${number(job.upload_bytes / 1048576)} MiB input / ${number(uploadSeconds)} s server-observed upload / ${number(job.upload_bytes * 8 / Math.max(uploadSeconds, .001) / 1000000)} Mbit/s payload. Report ${(job.artifacts['report.html'].bytes / 1024).toFixed(1)} KiB.`;
  $('warnings').textContent = `${result.warnings.length} analyzer warnings. Results expire at ${new Date(job.expires * 1000).toLocaleTimeString()}.`;
  $('download').href = `/api/jobs/${job.id}/report`; $('receipt').href = `/api/jobs/${job.id}/receipt`;
  $('report').hidden = false;
}
async function poll() {
  clearTimeout(pollTimer);
  if (!jobId) return;
  try {
    const job = await api(`/api/jobs/${jobId}`);
    const states = {
      reserved: ['Waiting for replay', 'Upload this job or cancel it.'],
      uploading: ['Uploading', 'Receiving the complete replay.'],
      queued: ['Queued', 'Waiting for the one CPU worker.'],
      decoding: ['Decoding replay', 'Reading real replay events; no invented progress percentage.'],
      select: ['Choose a player', 'The roster below was read from your replay.'],
      analysis_queued: ['Queued for analysis', 'Waiting for the one CPU worker.'],
      analyzing: ['Building report', 'Computing event metrics and hash-confirming output.'],
      complete: ['Complete', 'Raw replay/export removed. Download results before they expire.'],
      failed: ['Processing failed', job.error || 'No completed result was published.'],
      cleanup_pending: ['Cleaning up', job.error || 'The exact worker must exit before files are removed.']
    };
    message(...(states[job.state] || [job.state, job.error || 'Check job status.']));
    $('clear').disabled = false;
    if (!uploading) {
      $('progress').removeAttribute('value');
      $('progress-label').textContent = 'Stage status / no estimated completion time';
    }
    $('player-panel').hidden = job.state !== 'select';
    if (job.state === 'select' && $('player').dataset.job !== job.id) {
      $('player').replaceChildren(...job.players.map(p => new Option(`${p.name} / ${p.steam_id}`, p.steam_id)));
      $('player').dataset.job = job.id;
    }
    const stage = ['reserved', 'uploading'].includes(job.state) ? 0 :
      ['queued', 'decoding'].includes(job.state) ? 1 : job.state === 'select' ? 2 : 3;
    ['intake', 'decode', 'player', 'report'].forEach((name, i) => {
      $(`step-${name}`).className = i < stage || job.state === 'complete' ? 'done' : i === stage ? 'active' : '';
    });
    if (job.state === 'complete') await showReport(job);
    else if (job.state !== 'failed') pollTimer = setTimeout(poll, 1200);
  } catch (error) { failure(error); $('clear').disabled = !jobId; }
}
$('access').addEventListener('submit', async event => {
  event.preventDefault();
  try { unlocked(await api('/api/session', 'POST', { invite: $('invite').value })); $('invite').value = ''; if (jobId) poll(); }
  catch (error) { failure(error); }
});
$('demo-file').addEventListener('change', fileChanged);
$('start').addEventListener('click', async () => {
  uploading = true; fileChanged();
  try {
    const job = await api('/api/jobs', 'POST', {});
    jobId = job.id; sessionStorage.setItem('csda-job', jobId); $('clear').disabled = false;
    message('Uploading replay', 'Sending bytes to temporary private storage.');
    await upload($('demo-file').files[0]);
    uploading = false; poll();
  } catch (error) { uploading = false; failure(error); fileChanged(); }
});
$('build').addEventListener('click', async () => {
  $('build').disabled = true;
  try { await api(`/api/jobs/${jobId}/analysis`, 'POST', { steam_id: $('player').value }); await poll(); }
  catch (error) { failure(error); }
  finally { $('build').disabled = false; }
});
$('clear').addEventListener('click', async () => {
  try {
    if (jobId) { try { await api(`/api/jobs/${jobId}`, 'DELETE'); } catch (error) { if (!error.message.includes('not found')) throw error; } }
    clearTimeout(pollTimer); jobId = ''; reportId = ''; sessionStorage.removeItem('csda-job');
    $('player-panel').hidden = true; $('report').hidden = true; $('clear').disabled = true;
    $('progress').value = 0; $('progress-label').textContent = 'No active job'; fileChanged();
    message('Cleared', 'Deletion requested. Any active worker exits before its files are removed.');
  } catch (error) { failure(error); }
});
api('/api/session').then(session => { unlocked(session); if (jobId) poll(); })
  .catch(error => { if (error.message.includes('access') || error.message.includes('expired') || error.message.includes('session')) message('Beta access required', 'Enter your access code to upload a replay.'); else failure(error); });
