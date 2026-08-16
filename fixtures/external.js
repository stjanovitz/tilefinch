document.getElementById('status').textContent = 'EXTERNAL SCRIPT';
globalThis.scriptOrder.push('external');
fetch('/api').then(response => response.json()).then(data => {
  globalThis.fetchTransport = data.value;
});
