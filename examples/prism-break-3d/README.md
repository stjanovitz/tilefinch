# Prism Break 3D

Prism Break is a self-contained WebGL game and installable Tilefinch offline
app. It keeps the board in a bounded static mesh and uploads one smaller
dynamic mesh per frame. It also uses the Gamepad API, page fullscreen,
visibility lifecycle events, and a small persistent Web Audio oscillator
graph. It has four levels, multiball, wide-paddle, slow-ball, shield, and laser
power-ups, plus trails, particles, hit flashes, and bounded screen shake.

Serve the repository over HTTPS, open `examples/prism-break-3d/index.html`,
then use **Page tools → Install offline app**. Tilefinch snapshots the same-origin
HTML, CSS, JavaScript, and manifest; no Service Worker or network access is
needed when the saved app is reopened.

On PSP, focus **Play** and press X. Hold Start+Select for 0.7 seconds to give
the page the controls. The nub or D-pad moves, X launches the ball or fires the
laser, and Triangle pauses. Hold Start+Select again to return controls to the
browser.
