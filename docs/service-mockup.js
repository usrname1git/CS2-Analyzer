'use strict';
const byId = id => document.getElementById(id);
let timer;
let stage = 'idle';
const phases = {
  idle: ['Ready to preview', 0, -1],
  intake: ['Validating sample', 15, 0],
  decode: ['Decoding sample', 40, 1],
  player: ['Choose a player', 65, 2],
  report: ['Building sample report', 85, 3],
  complete: ['Sample report ready', 100, 4],
  failed: ['Sample decode failed', 40, 1],
};

function state(next, detail) {
  stage = next;
  const [label, percent, active] = phases[next];
  byId('state').textContent = label;
  byId('state').dataset.state = next;
  byId('progress').value = percent;
  byId('progress-label').textContent = `${percent}%`;
  byId('status-detail').textContent = detail;
  ['intake', 'decode', 'player', 'report'].forEach((name, index) => {
    byId(`step-${name}`).className = index < active ? 'done' : index === active ? 'active' : '';
  });
  const busy = ['intake', 'decode', 'report'].includes(next);
  byId('start').disabled = busy;
  byId('fail').disabled = busy;
  byId('build').disabled = next !== 'player';
}

function clearPreview() {
  clearTimeout(timer);
  byId('player-panel').hidden = true;
  byId('report').hidden = true;
  byId('player').value = 'one';
  byId('demo-file').value = '';
  byId('file-name').textContent = 'synthetic_mirage.dem';
  byId('file-detail').textContent = 'Synthetic sample - no file attached';
  byId('file-help').textContent = 'Choose a file to inspect its name and size only. Preview runs always use synthetic sample data.';
  state('idle', 'Preview cleared. No file was uploaded or stored by this page.');
}

function chooseFile(file) {
  clearPreview();
  if (!file) return;
  if (!file.name.toLowerCase().endsWith('.dem') || file.size === 0 || file.size > 256 * 1024 * 1024) {
    byId('file-help').textContent = 'Intake preview rejected: choose a nonempty .dem file no larger than 256 MiB. This metadata check is not format validation.';
    return;
  }
  byId('file-name').textContent = file.name;
  byId('file-detail').textContent = `${(file.size / (1024 * 1024)).toFixed(1)} MiB - metadata only`;
  byId('file-help').textContent = 'File selected for UI preview only. It is not uploaded, read or analyzed; the sample buttons use synthetic_mirage.dem.';
}

function preview(fail) {
  clearTimeout(timer);
  byId('player-panel').hidden = true;
  byId('report').hidden = true;
  state('intake', 'Simulating synthetic_mirage.dem only. Your selected file is not being processed.');
  timer = setTimeout(() => {
    state('decode', 'Synthetic stage preview. A live worker would decode the sealed replay with pinned CSDA.');
    timer = setTimeout(() => {
      if (fail) {
        state('failed', 'Example failure: unsupported demo protocol. No completed report is published. Retry would require an updated, qualified decoder.');
      } else {
        byId('player-panel').hidden = false;
        state('player', 'Synthetic roster ready. Select a sample player before building the report.');
      }
    }, 650);
  }, 650);
}

function buildReport() {
  if (stage !== 'player') return;
  state('report', 'Simulating artifact verification. These numbers come from a fixed synthetic fixture.');
  timer = setTimeout(() => {
    const first = byId('player').value === 'one';
    byId('report-title').textContent = `Sample Player ${first ? '1' : '2'} - Mirage`;
    byId('kda').textContent = first ? '24 / 18 / 6' : '18 / 24 / 4';
    byId('kd').textContent = first ? '1.333' : '0.750';
    byId('adr').textContent = first ? '92.4' : '68.7';
    byId('hs').textContent = first ? '58.3%' : '44.4%';
    byId('hs').nextElementSibling.textContent = first ? '14 of 24 enemy kills' : '8 of 18 enemy kills';
    ['ak', 'awp', 'other'].forEach((weapon, index) => {
      const count = (first ? [16, 5, 3] : [9, 6, 3])[index];
      byId(`weapon-${weapon}`).textContent = count;
      byId(`bar-${weapon}`).style.width = `${100 * count / (first ? 24 : 18)}%`;
    });
    byId('report').hidden = false;
    byId('player-panel').hidden = true;
    state('complete', 'Synthetic sample report complete. Nothing was uploaded; no expiry or deletion has run on a server.');
  }, 650);
}

byId('start').addEventListener('click', () => preview(false));
byId('fail').addEventListener('click', () => preview(true));
byId('clear').addEventListener('click', clearPreview);
byId('build').addEventListener('click', buildReport);
byId('demo-file').addEventListener('change', event => chooseFile(event.target.files[0]));
byId('dropzone').addEventListener('dragover', event => {
  event.preventDefault();
  byId('dropzone').classList.add('dragging');
});
byId('dropzone').addEventListener('dragleave', () => byId('dropzone').classList.remove('dragging'));
byId('dropzone').addEventListener('drop', event => {
  event.preventDefault();
  byId('dropzone').classList.remove('dragging');
  if (event.dataTransfer.files.length !== 1) {
    clearPreview();
    byId('file-help').textContent = 'Intake preview accepts one file at a time.';
    return;
  }
  chooseFile(event.dataTransfer.files[0]);
});
byId('download').addEventListener('click', () => {
  if (stage !== 'complete') return;
  const text = [
    'SYNTHETIC UI SAMPLE - NOT AN ANALYSIS OF YOUR UPLOAD',
    byId('report-title').textContent,
    `K/D/A: ${byId('kda').textContent}`,
    `K/D: ${byId('kd').textContent}`,
    `ADR: ${byId('adr').textContent}`,
    `Headshot kills: ${byId('hs').textContent}`,
    'Cheating: not assessed. Damage-to-kill timing is not reaction time.',
    'No upload, server-side storage, processing or retention occurred.',
  ].join('\n') + '\n';
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = 'CS2-sample-report.txt';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
