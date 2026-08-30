# Prism Break 3D

Prism Break is a self-contained WebGL game and installable Tilefinch offline
app. It keeps the board in a bounded static mesh, uses the bounded instancing
extension for repeated dynamic effects, and retains an ordinary WebGL 1 mesh
fallback. It also uses the Gamepad API, page fullscreen,
visibility lifecycle events, and a small persistent Web Audio oscillator
graph. It has four levels, multiball, wide-paddle, slow-ball, shield, and laser
power-ups, plus trails, particles, and hit flashes.

Serve the repository over HTTPS, open `examples/prism-break-3d/index.html`,
then use **Page tools → Install offline app**. Tilefinch snapshots the same-origin
HTML, CSS, JavaScript, and manifest; no Service Worker or network access is
needed when the saved app is reopened.

On PSP, focus **Play** and press X. Its trusted click asks Tilefinch for Page
controls and shows the Start+Select escape hint. The nub or D-pad moves, X
launches the ball or fires the laser, and Triangle pauses. Hold Start+Select
to return controls to the browser; the same 0.7-second chord remains the
manual entry fallback.

On a computer, use Left/Right or A/D to move, Space or Enter to launch and
fire, and P or Escape to pause. A connected standard gamepad is also supported.
The game follows the last device that produces input, so an idle controller
does not block the keyboard and either device can take over without reloading.
