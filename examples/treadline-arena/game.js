(() => {
  "use strict";

  const canvas = document.getElementById("game");
  const gl = canvas && canvas.getContext("webgl", {
    alpha: false, depth: true, antialias: true,
  });
  const ui = {
    shell: document.getElementById("game-shell"),
    panel: document.getElementById("panel"),
    message: document.getElementById("message"),
    play: document.getElementById("play"),
    hud: document.getElementById("hud"),
    score: document.getElementById("score"),
    arena: document.getElementById("arena"),
    armor: document.getElementById("armor"),
    gadget: document.getElementById("gadget"),
    toast: document.getElementById("toast"),
    controls: document.getElementById("controls"),
    modeChoice: document.getElementById("mode-choice"),
    classChoice: document.getElementById("class-choice"),
    gadgetChoice: document.getElementById("gadget-choice"),
    difficultyChoice: document.getElementById("difficulty-choice"),
    modeValue: document.getElementById("mode-value"),
    classValue: document.getElementById("class-value"),
    gadgetValue: document.getElementById("gadget-value"),
    difficultyValue: document.getElementById("difficulty-value"),
    controlsChoice: document.getElementById("controls-choice"),
    controlsValue: document.getElementById("controls-value"),
    assistChoice: document.getElementById("assist-choice"),
    assistValue: document.getElementById("assist-value"),
    aimChoice: document.getElementById("aim-choice"),
    aimValue: document.getElementById("aim-value"),
    cameraChoice: document.getElementById("camera-choice"),
    cameraValue: document.getElementById("camera-value"),
    commandChoice: document.getElementById("command-choice"),
    commandValue: document.getElementById("command-value"),
    musicChoice: document.getElementById("music-choice"),
    effectsChoice: document.getElementById("effects-choice"),
    effectsValue: document.getElementById("effects-value"),
    shakeChoice: document.getElementById("shake-choice"),
    shakeValue: document.getElementById("shake-value"),
    objectiveChoice: document.getElementById("objective-choice"),
    objectiveValue: document.getElementById("objective-value"),
    reverseChoice: document.getElementById("reverse-choice"),
    reverseValue: document.getElementById("reverse-value"),
    paletteChoice: document.getElementById("palette-choice"),
    paletteValue: document.getElementById("palette-value"),
    paintChoice: document.getElementById("paint-choice"),
    paintValue: document.getElementById("paint-value"),
    replayLast: document.getElementById("replay-last"),
    replayShare: document.getElementById("replay-share"),
    replayLoad: document.getElementById("replay-load"),
    replayCode: document.getElementById("replay-code"),
    stats: document.getElementById("stats"),
    commandStatus: document.getElementById("command-status"),
    onlineActions: document.getElementById("online-actions"),
    onlineHost: document.getElementById("online-host"),
    onlineLan: document.getElementById("online-lan"),
    onlineCode: document.getElementById("online-code"),
    onlineStatus: document.getElementById("online-status"),
    onlineMessage: document.getElementById("online-message"),
    onlineInvite: document.getElementById("online-invite"),
    onlineResponse: document.getElementById("online-response"),
    onlinePeerActions: document.getElementById("online-peer-actions"),
    onlineAccept: document.getElementById("online-accept"),
    onlineReject: document.getElementById("online-reject"),
    onlineCancel: document.getElementById("online-cancel"),
    codeEntry: document.getElementById("code-entry"),
    codeValue: document.getElementById("code-value"),
    codeKeypad: document.getElementById("code-keypad"),
    codeCancel: document.getElementById("code-cancel"),
    webPairing: document.getElementById("web-pairing"),
    webStep: document.getElementById("web-step"),
    webHelp: document.getElementById("web-help"),
    webStatus: document.getElementById("web-status"),
    webShareLabel: document.getElementById("web-share-label"),
    webShare: document.getElementById("web-share"),
    webShareSize: document.getElementById("web-share-size"),
    webCopy: document.getElementById("web-copy"),
    webInputLabel: document.getElementById("web-input-label"),
    webInput: document.getElementById("web-input"),
    webApply: document.getElementById("web-apply"),
    webCancel: document.getElementById("web-cancel"),
  };
  if (!gl || !ui.panel || !ui.play) {
    globalThis.__treadlineBootReady = true;
    ui.message.textContent = "WebGL is unavailable.";
    globalThis.pocSummary = "TREADLINE-NO-WEBGL";
    return;
  }
  // The campaign (campaign.js): bounded hooks, see its header.
  const campaign = globalThis.__treadlineCampaign;
  // Control schemes and movement extras (controls.js): see its header.
  const controls = globalThis.__treadlineControls;
  // Practice Range (practice.js): bounded hooks, see its header.
  const practice = globalThis.__treadlinePractice;
  const webPairing = !!(navigator.tilefinchMultiplayer
    && navigator.tilefinchMultiplayer.pairingMode === "manual-offer-answer");
  if (webPairing) {
    ui.onlineHost.textContent = "Host web game";
    ui.onlineCode.textContent = "Join web game";
    ui.onlineLan.hidden = true;
  }
  const instancing = gl.getExtension("ANGLE_instanced_arrays");
  const vertexArrays = gl.getExtension("OES_vertex_array_object");
  const repeatedFrameCommands =
    gl.getExtension("TILEFINCH_repeated_frame_commands");
  let repeatedFrameList = null;
  const repeatedFrameCounts = new Uint16Array(6);
  const repeatedFrameLimits = new Uint16Array(6);
  let repeatedFrameCaptures = 0;
  /* The page URL's switches, parsed one way: urlSwitch(name) is the value
     of name=value in the query, else null. href can publish the committed
     query before search does on PSP, so its query wins. */
  const urlQuery = (() => {
    const href = String(location.href || ""), at = href.indexOf("?");
    return (at >= 0 ? href.slice(at) : location.search || "").split("#")[0];
  })();
  function urlSwitch(name) {
    const found = new RegExp(`[?&]${name}=([^&]*)`).exec(urlQuery);
    return found ? found[1] : null;
  }
  /* Qualification runs (?qualification=...; docs/DEVELOPMENT.md, "Canvas
     and WebGL game qualification") change the workload: soak and long-soak
     play a self-driving match, and every qualification run keeps music and
     killcam history off unless it asks for them. Their tooling (the debug
     API, phase profile and report, the soak's action cycle and the long
     soak's driver) is qualification.js: the game fetches it for a
     qualification URL, and a harness that evaluates the game scripts itself
     sets __treadlineEnableDebug before game.js and evaluates it after.
     Ordinary play does neither. */
  const qualificationKind = urlSwitch("qualification");
  const qualificationRun = qualificationKind !== null;
  const qualificationSoak = qualificationKind === "soak";
  const qualificationLongSoak = qualificationKind === "long-soak";
  // seed=N pins Onslaught's arena seed, in qualification runs only.
  const requestedSeed = qualificationRun ? urlSwitch("seed") : null;
  const requestedArenaSeed = requestedSeed !== null && /^\d{1,10}$/.test(requestedSeed)
    ? Number(requestedSeed) >>> 0 : 0;
  const qualificationAutoStart = qualificationSoak || qualificationLongSoak;
  const qualificationHarness = globalThis.__treadlineEnableDebug === true;
  const qualificationTooling = qualificationRun || qualificationHarness;
  /* Phase clocks. They stay null, and every clock behind
     qualificationAIActive or a measured frame stays off, until
     qualification.js installs them (qualificationBridge). */
  let qualificationAIActive = false;
  let validationInputProfile = false;
  let qualificationSampleLimit = 0, qualificationTiming = null;
  let qualificationAITimes = null, qualificationAICounts = null;
  let qualificationMoveTimes = null, qualificationSoundTimes = null;
  let qualificationActionTimes = null, qualificationActionCounts = null;
  let qualificationHudPhaseTotals = null, qualificationHudPhaseMaximums = null;
  let qualificationHudPhaseCounts = null, qualificationHudIndicator = null;
  /* qualification.js's per-frame hooks (with its switches: frozen bots, a
     scripted league player, the killcam) and the long soak's driver. */
  let qualificationHooks = null, longSoakDriver = null;
  /* Shot, hit, contact and stall counts for the bot league and the
     invariant sweep; qualification.js installs the object, and every count
     is behind its null check. */
  let qualificationStats = null;
  /* The modules' harness surfaces (campaign, practice, controls), which
     they lend at attach in qualification and test runs only (lendTooling
     is null otherwise); qualification.js publishes them. */
  const toolingSurfaces = qualificationTooling ? {} : null;
  const lendTooling = qualificationTooling
    ? (name, surface) => { toolingSurfaces[name] = surface; } : null;
  /* Bumped whenever scenery a shell can meet is replaced (aim guide cache). */
  let aimGeometryEpoch = 0;

  /* Arena bounds, in world units from the centre: the visible wall's
     inner face, the edge a shell meets, the limit for a hull's centre, the
     camera's look-target clamp and the crate placement limit. */
  const ARENA_WALL_FACE = 8.075, ARENA_EDGE = 7.85, HULL_EDGE = 7.65;
  const CAMERA_EDGE = 7.42, CRATE_PLACE_EDGE = 6.5;
  // Crates are .44-unit boxes; a shell spawned this close to one is in it.
  const CRATE_HALF = globalThis.__treadlineArenaData.spatial.CRATE_HALF;
  const CRATE_SHELL_HALF = .32;
  // Fixed pools (the hazard and crate grids keep one bit per slot).
  const MAX_HAZARDS = 8, MAX_CRATES = 4;
  /* Bot fire: the farthest (squared) a bot shoots, and the volley lane
     that spaces bots' shots at the player. */
  const BOT_FIRE_RANGE_SQUARED = 67.24, BOT_VOLLEY_GAP = .25;
  // Main cannon: a held shot charges at CHARGE_THRESHOLD seconds.
  const CHARGE_THRESHOLD = .4, MAIN_SHELL_LIFE = 3;

  const MAX_VERTICES = 4096;
  const MAX_INDICES = 6144;
  const positions = new Float32Array(MAX_VERTICES * 3);
  const colors = new Float32Array(MAX_VERTICES * 4);
  const indices = new Uint16Array(MAX_INDICES);
  let vertexCount = 0, indexCount = 0, meshDrops = 0;

  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
      throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, `
    attribute vec3 aPosition;
    attribute vec4 aColor;
    uniform mat4 uViewProjection;
    uniform vec4 uTint;
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uViewProjection * vec4(aPosition, 1.0);
      vColor = aColor * uTint;
    }
  `));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vColor;
    void main(void) { gl_FragColor = vColor; }
  `));
  gl.bindAttribLocation(program, 0, "aPosition");
  gl.bindAttribLocation(program, 1, "aColor");
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);

  const positionLocation = gl.getAttribLocation(program, "aPosition");
  const colorLocation = gl.getAttribLocation(program, "aColor");
  const viewProjectionLocation = gl.getUniformLocation(program, "uViewProjection");
  const sceneryTintLocation = gl.getUniformLocation(program, "uTint");
  gl.uniform4f(sceneryTintLocation, 1, 1, 1, 1);
  const dynamicPositionBuffer = gl.createBuffer();
  const dynamicColorBuffer = gl.createBuffer();
  const dynamicIndexBuffer = gl.createBuffer();
  const staticPositionBuffer = gl.createBuffer();
  const staticColorBuffer = gl.createBuffer();
  const staticIndexBuffer = gl.createBuffer();

  function createMeshVertexArray(position, color, index) {
    if (!vertexArrays) return null;
    const array = vertexArrays.createVertexArrayOES();
    vertexArrays.bindVertexArrayOES(array);
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(positionLocation);
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(colorLocation);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    return array;
  }

  function allocateMeshBuffers(position, color, index,
                               usage = gl.DYNAMIC_DRAW) {
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.bufferData(gl.ARRAY_BUFFER, positions.byteLength, usage);
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.bufferData(gl.ARRAY_BUFFER, colors.byteLength, usage);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices.byteLength, usage);
  }
  allocateMeshBuffers(dynamicPositionBuffer, dynamicColorBuffer, dynamicIndexBuffer);
  allocateMeshBuffers(staticPositionBuffer, staticColorBuffer,
    staticIndexBuffer, gl.STATIC_DRAW);
  const dynamicVertexArray = createMeshVertexArray(
    dynamicPositionBuffer, dynamicColorBuffer, dynamicIndexBuffer);
  const staticVertexArray = createMeshVertexArray(
    staticPositionBuffer, staticColorBuffer, staticIndexBuffer);
  if (vertexArrays) vertexArrays.bindVertexArrayOES(null);

  gl.enable(gl.DEPTH_TEST);
  gl.depthFunc(gl.LEQUAL);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  /* The horizon backdrop (addHorizonBackdrop) covers every pixel beyond the
     floor, so the clear colour shows only where the PSP GE dropped a
     backdrop triangle beside the camera: its upper half. Clear to that
     half's mid-gradient colour under the scenery tint, so such a hole reads
     as backdrop rather than near-black (applyPalette keeps it in step). */
  const BACKDROP_BASE = [-.002, .11, .13], BACKDROP_SKY = 1.3;
  const BACKDROP_HOLE_SHADE = 1 + (BACKDROP_SKY - 1) * .75;
  function clearToBackdrop(tint) {
    gl.clearColor(BACKDROP_BASE[0] * BACKDROP_HOLE_SHADE * tint[0],
      BACKDROP_BASE[1] * BACKDROP_HOLE_SHADE * tint[1],
      BACKDROP_BASE[2] * BACKDROP_HOLE_SHADE * tint[2], 1);
  }
  clearToBackdrop([1, 1, 1]);
  gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);

  const BOX_VERTICES = new Int8Array([
    -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,
     1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,
    -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,
    -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,
     1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,
    -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,
  ]);
  const BOX_INDICES = new Uint8Array([
     0, 1, 2,  0, 2, 3,  4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11, 12,13,14, 12,14,15,
    16,17,18, 16,18,19, 20,21,22, 20,22,23,
  ]);
  // Per box face: a strong key light on top, darker undersides and sides.
  const FACE_SHADE = new Float32Array([1.05, .5, 1.32, .45, .84, .62]);
  /* Warm light from above and cool fill in the shadows gives every existing
     box a readable silhouette without adding geometry or per-frame work. */
  const FACE_TEMPERATURE = new Float32Array([
    1, .98, .93,   .90, .95, 1.10,   1, .98, .92,
    .88, .94, 1.10, .95, .97, 1.03,  .92, .96, 1.07,
  ]);

  const MAX_INSTANCES_PER_DRAW = 64;
  const TRANSLATED_VERTEX_BATCH_LIMIT = 4096;
  const MAX_BOX_INSTANCES = MAX_INSTANCES_PER_DRAW;
  const boxInstanceMatrices = new Float32Array(MAX_BOX_INSTANCES * 16);
  for (let instance = 0; instance < MAX_BOX_INSTANCES; instance++)
    boxInstanceMatrices[instance * 16 + 15] = 1;
  const boxInstanceTints = new Float32Array(MAX_BOX_INSTANCES * 4);
  const boxInstanceSlotOwners = new Int16Array(MAX_BOX_INSTANCES);
  boxInstanceSlotOwners.fill(-1);
  let boxInstanceUploadCount = -1;
  let boxInstanceMatrixBytes = new Uint8Array(0);
  let boxInstanceTintBytes = new Uint8Array(0);
  let boxInstanceCount = 0, collectInstancedBoxes = false;
  let frameInstanceCeiling = MAX_BOX_INSTANCES, renderedBulletInstances = 0;
  let optionalInstanceCeiling = MAX_BOX_INSTANCES;
  let tankBarrelCount = 0;
  const RETAINED_SCENERY_INSTANCE_LIMIT = 16;
  let retainedSceneryCount = 0, retainedSceneryDirty = true;
  let retainedObjectiveFirst = -1;
  let boxInstanceProgram = null, boxInstancePosition = null;
  let boxInstanceShade = null, boxInstanceIndex = null;
  let boxInstanceMatrix = null, boxInstanceTint = null;
  let boxInstanceMatrixLocation = -1, boxInstanceTintLocation = -1;
  let boxInstanceViewProjectionLocation = null;
  let boxInstanceVertexArray = null;
  let lastBoxYaw = NaN, lastBoxCosine = 1, lastBoxSine = 0;
  /* Tank parts keep a stable logical cache id even though inactive optional
     parts compact the uploaded instance stream. When a part remains in the
     same physical slot, preserve its matrix basis. Publish translation and
     tint directly: moving tanks change these every frame, so checking and
     updating another copy of each field adds work rather than avoiding it. */
  const RETAINED_TANK_PARTS = 12;
  const retainedTankPartCount = 6 * RETAINED_TANK_PARTS;
  const retainedTankPartSlots = new Int8Array(retainedTankPartCount);
  retainedTankPartSlots.fill(-1);
  /* Keep signatures as ordinary JS numbers. Float32 signatures would compare
     unequal to the identical double-valued inputs after their first rounding,
     defeating the cache; Float64 typed-array traffic is costly on Allegrex. */
  const retainedTankPartState = new Array(retainedTankPartCount * 5);
  retainedTankPartState.fill(NaN);

  function resetMesh() {
    vertexCount = indexCount = 0;
    lastBoxYaw = NaN; lastBoxCosine = 1; lastBoxSine = 0;
  }

  function addBox(x, y, z, width, height, depth, yaw, color, alpha = 1,
                  knownSine = NaN, knownCosine = NaN) {
    if (collectInstancedBoxes && boxInstanceProgram) {
      if (boxInstanceCount >= frameInstanceCeiling) {
        meshDrops++;
        instanceCapHitThisFrame = true;
        return false;
      }
      const instance = boxInstanceCount++;
      boxInstanceSlotOwners[instance] = -1;
      const matrixAt = instance * 16;
      let cosine, sine;
      if (Number.isFinite(knownSine) && Number.isFinite(knownCosine)) {
        sine = knownSine; cosine = knownCosine;
      } else if (yaw === 0) { cosine = 1; sine = 0; }
      else if (yaw === lastBoxYaw) {
        cosine = lastBoxCosine; sine = lastBoxSine;
      } else {
        lastBoxYaw = yaw;
        lastBoxCosine = cosine = Math.cos(yaw);
        lastBoxSine = sine = Math.sin(yaw);
      }
      const halfWidth = width * .5, halfHeight = height * .5;
      const halfDepth = depth * .5;
      boxInstanceMatrices[matrixAt] = cosine * halfWidth;
      boxInstanceMatrices[matrixAt + 2] = -sine * halfWidth;
      boxInstanceMatrices[matrixAt + 5] = halfHeight;
      boxInstanceMatrices[matrixAt + 8] = sine * halfDepth;
      boxInstanceMatrices[matrixAt + 10] = cosine * halfDepth;
      boxInstanceMatrices[matrixAt + 12] = x;
      boxInstanceMatrices[matrixAt + 13] = y;
      boxInstanceMatrices[matrixAt + 14] = z;
      const tintAt = instance * 4;
      boxInstanceTints[tintAt] = color[0];
      boxInstanceTints[tintAt + 1] = color[1];
      boxInstanceTints[tintAt + 2] = color[2];
      boxInstanceTints[tintAt + 3] = alpha;
      return true;
    }
    if (vertexCount + 24 > MAX_VERTICES || indexCount + 36 > MAX_INDICES) {
      meshDrops++;
      return false;
    }
    const base = vertexCount;
    const cosine = Math.cos(yaw), sine = Math.sin(yaw);
    for (let at = 0; at < 24; at++) {
      const source = at * 3;
      const localX = BOX_VERTICES[source] * width * .5;
      const localY = BOX_VERTICES[source + 1] * height * .5;
      const localZ = BOX_VERTICES[source + 2] * depth * .5;
      const px = x + localX * cosine + localZ * sine;
      const pz = z - localX * sine + localZ * cosine;
      const p = vertexCount * 3, c = vertexCount * 4;
      const face = (at / 4) | 0;
      const shade = boxShade(at);
      const temperature = face * 3;
      positions[p] = px;
      positions[p + 1] = y + localY;
      positions[p + 2] = pz;
      colors[c] = Math.min(1,
        color[0] * shade * FACE_TEMPERATURE[temperature]);
      colors[c + 1] = Math.min(1,
        color[1] * shade * FACE_TEMPERATURE[temperature + 1]);
      colors[c + 2] = Math.min(1,
        color[2] * shade * FACE_TEMPERATURE[temperature + 2]);
      colors[c + 3] = alpha;
      vertexCount++;
    }
    for (let at = 0; at < 36; at++)
      indices[indexCount++] = base + BOX_INDICES[at];
    return true;
  }

  /* Side faces darken toward the ground for a deep contact. */
  function boxShade(vertex) {
    const face = (vertex / 4) | 0;
    let shade = FACE_SHADE[face];
    if (face !== 2 && face !== 3 && BOX_VERTICES[vertex * 3 + 1] < 0)
      shade *= .5;
    return shade;
  }

  function addOptionalBox(x, y, z, width, height, depth, yaw, color,
                          alpha = 1, knownSine = NaN, knownCosine = NaN) {
    if (collectInstancedBoxes && boxInstanceCount >= optionalInstanceCeiling) {
      instanceCapHitThisFrame = true;
      return false;
    }
    return addBox(x, y, z, width, height, depth, yaw, color, alpha,
      knownSine, knownCosine);
  }

  /* Effect records own immutable basis/RGB for one lifetime. Particles only
     change translation/alpha; decals only change alpha. Activation and decal
     recycling invalidate the logical slot. Generic emitters and killcam exit
     invalidate its physical owner, so compaction cannot retain another box.
     This is not a general cache for mutable geometry or scratch colors. */
  function addRetainedEffectBox(owner, stationary, x, y, z,
      width, height, depth, yaw, color, alpha, sine, cosine) {
    if (!collectInstancedBoxes || !boxInstanceProgram
        || owner < 0 || owner >= retainedEffectSlots.length)
      return addOptionalBox(x, y, z, width, height, depth, yaw,
        color, alpha, sine, cosine);
    if (boxInstanceCount >= optionalInstanceCeiling) {
      instanceCapHitThisFrame = true;
      return false;
    }
    if (boxInstanceCount >= frameInstanceCeiling) {
      meshDrops++;
      instanceCapHitThisFrame = true;
      return false;
    }
    const instance = boxInstanceCount, encoded = -3 - owner;
    if (retainedEffectSlots[owner] === instance
        && boxInstanceSlotOwners[instance] === encoded) {
      const tintAt = instance * 4;
      if (!stationary) {
        const at = instance * 16;
        boxInstanceMatrices[at + 12] = x;
        boxInstanceMatrices[at + 13] = y;
        boxInstanceMatrices[at + 14] = z;
        /* Moving owners are sparks, whose tint cools (SPARK_TINTS). Green
           rises through the whole ramp, so it tells the steps apart. */
        if (boxInstanceTints[tintAt + 1] !== color[1]) {
          boxInstanceTints[tintAt] = color[0];
          boxInstanceTints[tintAt + 1] = color[1];
          boxInstanceTints[tintAt + 2] = color[2];
        }
      }
      boxInstanceTints[tintAt + 3] = alpha;
      boxInstanceCount++;
      return true;
    }
    const admitted = addBox(x, y, z, width, height, depth, yaw,
      color, alpha, sine, cosine);
    // A cold invalid basis keeps addBox's fallback and is never retained.
    // The owned effect callers use finite quantized bases for their lifetime.
    if (admitted && Number.isFinite(sine) && Number.isFinite(cosine)) {
      retainedEffectSlots[owner] = instance;
      boxInstanceSlotOwners[instance] = encoded;
    }
    return admitted;
  }

  function addRetainedTankBox(tankId, part, optional, x, y, z,
                              width, height, depth, yaw, color, alpha,
                              sine, cosine) {
    if (!collectInstancedBoxes || !boxInstanceProgram)
      return optional
        ? addOptionalBox(x, y, z, width, height, depth, yaw, color, alpha,
          sine, cosine)
        : addBox(x, y, z, width, height, depth, yaw, color, alpha,
          sine, cosine);
    if (boxInstanceCount >= frameInstanceCeiling
        || (optional && boxInstanceCount >= optionalInstanceCeiling)) {
      if (boxInstanceCount >= frameInstanceCeiling) meshDrops++;
      instanceCapHitThisFrame = true;
      return false;
    }
    const cache = tankId * RETAINED_TANK_PARTS + part;
    const stateAt = cache * 5;
    const instance = boxInstanceCount;
    const matrixAt = instance * 16;
    const tintAt = instance * 4;
    const sameSlot = retainedTankPartSlots[cache] === instance
      && boxInstanceSlotOwners[instance] === cache;
    const sameBasis = sameSlot
      && retainedTankPartState[stateAt] === width
      && retainedTankPartState[stateAt + 1] === height
      && retainedTankPartState[stateAt + 2] === depth
      && retainedTankPartState[stateAt + 3] === sine
      && retainedTankPartState[stateAt + 4] === cosine;
    if (!sameBasis) {
      const halfWidth = width * .5, halfHeight = height * .5;
      const halfDepth = depth * .5;
      boxInstanceMatrices[matrixAt] = cosine * halfWidth;
      boxInstanceMatrices[matrixAt + 2] = -sine * halfWidth;
      boxInstanceMatrices[matrixAt + 5] = halfHeight;
      boxInstanceMatrices[matrixAt + 8] = sine * halfDepth;
      boxInstanceMatrices[matrixAt + 10] = cosine * halfDepth;
      retainedTankPartState[stateAt] = width;
      retainedTankPartState[stateAt + 1] = height;
      retainedTankPartState[stateAt + 2] = depth;
      retainedTankPartState[stateAt + 3] = sine;
      retainedTankPartState[stateAt + 4] = cosine;
    }
    boxInstanceMatrices[matrixAt + 12] = x;
    boxInstanceMatrices[matrixAt + 13] = y;
    boxInstanceMatrices[matrixAt + 14] = z;
    boxInstanceTints[tintAt] = color[0];
    boxInstanceTints[tintAt + 1] = color[1];
    boxInstanceTints[tintAt + 2] = color[2];
    boxInstanceTints[tintAt + 3] = alpha;
    retainedTankPartSlots[cache] = instance;
    boxInstanceSlotOwners[instance] = cache;
    boxInstanceCount++;
    return true;
  }

  function uploadBoxShades() {
    const shades = new Float32Array(24 * 4);
    for (let vertex = 0; vertex < 24; vertex++) {
      const shade = boxShade(vertex), temperature = ((vertex / 4) | 0) * 3;
      for (let at = 0; at < 3; at++)
        shades[vertex * 4 + at] = shade * FACE_TEMPERATURE[temperature + at];
      shades[vertex * 4 + 3] = 1;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
    gl.bufferData(gl.ARRAY_BUFFER, shades, gl.STATIC_DRAW);
  }

  function initializeBoxInstancing() {
    if (!instancing) return;
    const candidate = gl.createProgram();
    gl.attachShader(candidate, compile(gl.VERTEX_SHADER, `
      attribute vec3 aPosition;
      attribute vec4 aColor;
      attribute mat4 aInstanceModel;
      attribute vec4 aInstanceTint;
      uniform mat4 uViewProjection;
      varying lowp vec4 vColor;
      void main(void) {
        gl_Position = uViewProjection * aInstanceModel
          * vec4(aPosition, 1.0);
        vColor = aColor * aInstanceTint;
      }
    `));
    gl.attachShader(candidate, compile(gl.FRAGMENT_SHADER, `
      varying lowp vec4 vColor;
      void main(void) { gl_FragColor = vColor; }
    `));
    gl.bindAttribLocation(candidate, 0, "aPosition");
    gl.bindAttribLocation(candidate, 1, "aColor");
    gl.bindAttribLocation(candidate, 2, "aInstanceModel");
    gl.bindAttribLocation(candidate, 6, "aInstanceTint");
    gl.linkProgram(candidate);
    if (!gl.getProgramParameter(candidate, gl.LINK_STATUS)) return;
    boxInstanceProgram = candidate;
    boxInstanceMatrixLocation = gl.getAttribLocation(
      boxInstanceProgram, "aInstanceModel");
    boxInstanceTintLocation = gl.getAttribLocation(
      boxInstanceProgram, "aInstanceTint");
    boxInstanceViewProjectionLocation = gl.getUniformLocation(
      boxInstanceProgram, "uViewProjection");

    boxInstancePosition = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(BOX_VERTICES),
      gl.STATIC_DRAW);
    boxInstanceShade = gl.createBuffer();
    uploadBoxShades();
    boxInstanceIndex = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,
      new Uint16Array(BOX_INDICES), gl.STATIC_DRAW);
    boxInstanceMatrix = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
    gl.bufferData(gl.ARRAY_BUFFER, boxInstanceMatrices.byteLength,
      gl.DYNAMIC_DRAW);
    boxInstanceTint = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
    gl.bufferData(gl.ARRAY_BUFFER, boxInstanceTints.byteLength,
      gl.DYNAMIC_DRAW);
    if (vertexArrays) {
      boxInstanceVertexArray = vertexArrays.createVertexArrayOES();
      vertexArrays.bindVertexArrayOES(boxInstanceVertexArray);
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(0);
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(1);
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
      for (let column = 0; column < 4; column++) {
        const location = boxInstanceMatrixLocation + column;
        gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64,
          column * 16);
        gl.enableVertexAttribArray(location);
        instancing.vertexAttribDivisorANGLE(location, 1);
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
      gl.vertexAttribPointer(boxInstanceTintLocation, 4,
        gl.FLOAT, false, 16, 0);
      gl.enableVertexAttribArray(boxInstanceTintLocation);
      instancing.vertexAttribDivisorANGLE(boxInstanceTintLocation, 1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
      vertexArrays.bindVertexArrayOES(null);
    }
    gl.useProgram(program);
  }
  initializeBoxInstancing();

  /* Retain live chrome in WebGL; authored HTML is off the gameplay hot path. */
  /* 5x7 font as minimal ink-rectangle covers of the bitmaps in
     tests/fixtures/treadline-render-cache.js: x:3, y:3, w-1:3, h-1:3 bits.
     Runs are one pixel wider (bold), so every stem is three screen pixels
     at the 1.5x scale wherever it lands. */
  const HUD_CHARS = ' 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/.,:+!';
  const hudGlyphIndex = new Int8Array(128);
  hudGlyphIndex.fill(-1);
  for (let glyph = 0; glyph < HUD_CHARS.length; glyph++)
    hudGlyphIndex[HUD_CHARS.charCodeAt(glyph)] = glyph;
  const hudGlyphStarts = new Uint8Array([
    0,0,4,7,12,17,20,25,30,34,39,44,48,54,57,61,
    65,68,73,76,79,82,88,90,95,100,104,108,114,120,125,127,
    130,135,140,149,154,161,162,165,166,168,170,172,174,
  ]);
  const hudGlyphRuns = new Uint16Array([
    129,2056,2060,177,3074,73,177,192,524,153,1056,304,
    192,524,153,548,240,1536,3076,280,1024,256,208,1052,
    240,129,2056,216,548,177,256,516,19,1562,129,2056,
    2060,280,177,129,520,2060,217,177,129,2568,2572,280,
    3072,192,524,216,548,240,193,2056,241,3072,192,2060,
    240,3072,256,216,304,3072,256,216,193,2056,91,1052,
    177,3072,3076,280,129,3074,177,2564,544,177,3072,516,
    19,152,35,556,3072,304,3072,3076,72,75,530,3072,
    3076,80,26,99,129,2056,2060,177,3072,192,524,216,
    129,2056,1548,43,113,52,3072,192,524,216,539,556,
    193,520,153,548,240,256,3074,2560,2564,177,2048,2052,
    41,43,50,2560,2564,1050,49,51,512,516,17,19,
    26,33,35,552,556,512,516,17,19,1562,256,516,
    19,26,33,552,304,153,515,1042,553,50,42,49,
    18,42,1042,153,2050,50,
  ]);
  const HUD_GLYPH_LIMIT = 64;
  const HUD_OBJECTIVE_RADIUS_X = 116, HUD_OBJECTIVE_RADIUS_Y = 57;
  const HUD_PRIMITIVE_LIMIT = 150;
  const HUD_TRANSLATED_INDEX_RESERVE = HUD_PRIMITIVE_LIMIT * 6;
  const HUD_INDICATOR_PRIMITIVE_LIMIT = 12;
  /* Position and tint: the PSP never runs per-fragment logic. */
  const HUD_VERTEX_WORDS = 6;
  const hudVertices = new Float32Array(
    HUD_PRIMITIVE_LIMIT * 4 * HUD_VERTEX_WORDS);
  const hudIndices = new Uint16Array(HUD_PRIMITIVE_LIMIT * 6);
  for (let primitive = 0; primitive < HUD_PRIMITIVE_LIMIT; primitive++) {
    const vertex = primitive * 4, index = primitive * 6;
    // Pixel-space y is inverted (addHudQuad). Reverse the index order so
    // the quads and indicator triangles remain CCW in clip space.
    hudIndices[index] = vertex;
    hudIndices[index + 1] = vertex + 2;
    hudIndices[index + 2] = vertex + 1;
    hudIndices[index + 3] = vertex;
    hudIndices[index + 4] = vertex + 3;
    hudIndices[index + 5] = vertex + 2;
  }
  const hudIndicatorVertices = new Float32Array(
    HUD_INDICATOR_PRIMITIVE_LIMIT * 4 * HUD_VERTEX_WORDS);
  let hudVertexCount = 0, hudIndexCount = 0, hudCharacterCount = 0;
  let hudMeshDirty = true, hudScore = "", hudArena = "", hudArmor = "";
  let hudIndicatorVertexCount = 0, hudIndicatorIndexCount = 0;
  let hudPublishedIndicatorIndexCount = 0, hudIndicatorDirty = true;
  let hudTextUploads = 0, hudIndicatorUploads = 0;
  let hudDeferredFrames = 0;
  let hudGadget = "", hudCommand = "", hudToast = "";
  let hudObjectiveAngle = 0, hudDamageAngle = 0;
  let hudHitVisible = false, hudDamageVisible = false, hudObjectiveVisible = false;
  let hudGadgetRatio = 1, hudCommandRatio = 0, hudMultiplierRatio = 0;
  let hudVisualObjectiveStep = 0x7fffffff;
  let hudVisualDamageStep = 0x7fffffff;
  let hudVisualHit = false, hudVisualDamage = false, hudVisualObjective = false;
  let hudVisualGadgetStep = -1, hudVisualCommandStep = -1;
  let hudVisualMultiplierStep = -1;
  let hudVisualCommandEnabled = false, hudLungeCooling = false;
  let hudBuildPhase = -1;
  // A run started without a forge: setMode("playing") publishes its HUD.
  let hudRunStart = false;
  let hudBuildScore = "", hudBuildArena = "", hudBuildArmor = "";
  let hudBuildGadget = "", hudBuildCommand = "", hudBuildToast = "";
  let hudBuildGenerating = false;
  let hudBuildText = null, hudBuildTextAt = 0;
  let hudBuildTextX = 0, hudBuildTextY = 0, hudBuildTextScale = 1;
  let hudBuildTextTint = null;
  let hudPublishedIndexCount = 0, hudPublishedVertexCount = 0;
  /* The toast is the published text's last run. A gone or stale toast
     (an arena change, an expiry) is left out of the draw until the
     rebuilt HUD, which a busy frame may defer, replaces it. */
  let hudPublishedToastStart = 0, hudBuildToastIndex = -1, hudToastStale = false;
  /* Two glyphs fit beside a measured PSP gameplay frame. */
  const HUD_GLYPHS_PER_SLICE = 2;
  const HUD_TEXT = [.91, .98, 1, 1];
  const HUD_INK_BOLD = 1;
  const HUD_ADVANCE = 7;
  const HUD_ACCENT = [.56, 1, .83, 1];
  const HUD_WARNING = [1, .46, .34, 1];
  const HUD_GOLD = [1, .84, .44, 1];
  const HUD_GADGET_TRACK = [.12, .25, .23, .8];
  const HUD_LUNGE_TRACK = [.42, .3, .12, .8];
  const HUD_COMMAND_TRACK = [.28, .23, .1, .8];
  const HUD_MULTIPLIER_TRACK = [.22, .16, .08, .8];
  const HUD_PANEL = [.018, .05, .06, .94];
  const hudProgram = gl.createProgram();
  gl.attachShader(hudProgram, compile(gl.VERTEX_SHADER, `
    attribute vec2 aPosition;
    attribute vec4 aTint;
    varying lowp vec4 vTint;
    void main(void) {
      gl_Position = vec4(aPosition, 0.0, 1.0);
      vTint = aTint;
    }
  `));
  gl.attachShader(hudProgram, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vTint;
    void main(void) {
      gl_FragColor = vTint;
    }
  `));
  gl.bindAttribLocation(hudProgram, 0, "aPosition");
  gl.bindAttribLocation(hudProgram, 1, "aTint");
  gl.linkProgram(hudProgram);
  if (!gl.getProgramParameter(hudProgram, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(hudProgram));
  const hudVertexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, hudVertices.byteLength, gl.DYNAMIC_DRAW);
  const hudIndexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, hudIndices, gl.STATIC_DRAW);
  let hudVertexArray = null;
  if (vertexArrays) {
    hudVertexArray = vertexArrays.createVertexArrayOES();
    vertexArrays.bindVertexArrayOES(hudVertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
    gl.enableVertexAttribArray(0);
    gl.enableVertexAttribArray(1);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    vertexArrays.bindVertexArrayOES(null);
  }

  /* A glyph is several of these, so the four corners are written here in
     one call: a full HUD or forge panel can then be built in the frame
     that needs it. */
  function addHudQuad(x, y, width, height, tint) {
    if (width <= 0 || height <= 0
        || hudVertexCount + 4 > hudVertices.length / HUD_VERTEX_WORDS
        || hudIndexCount + 6 > hudIndices.length) return false;
    const left = x / 160 - 1, right = (x + width) / 160 - 1;
    const top = 1 - y / 90, bottom = 1 - (y + height) / 90;
    const r = tint[0], g = tint[1], b = tint[2], a = tint[3];
    const v = hudVertices;
    let at = hudVertexCount * HUD_VERTEX_WORDS;
    v[at] = left; v[at + 1] = top; v[at + 2] = r; v[at + 3] = g; v[at + 4] = b; v[at + 5] = a;
    at += 6;
    v[at] = right; v[at + 1] = top; v[at + 2] = r; v[at + 3] = g; v[at + 4] = b; v[at + 5] = a;
    at += 6;
    v[at] = right; v[at + 1] = bottom; v[at + 2] = r; v[at + 3] = g; v[at + 4] = b; v[at + 5] = a;
    at += 6;
    v[at] = left; v[at + 1] = bottom; v[at + 2] = r; v[at + 3] = g; v[at + 4] = b; v[at + 5] = a;
    hudVertexCount += 4;
    hudIndexCount += 6;
    return true;
  }

  function writeHudIndicatorVertex(pixelX, pixelY, tint) {
    const at = hudIndicatorVertexCount * HUD_VERTEX_WORDS;
    hudIndicatorVertices[at] = pixelX / 160 - 1;
    hudIndicatorVertices[at + 1] = 1 - pixelY / 90;
    hudIndicatorVertices[at + 2] = tint[0];
    hudIndicatorVertices[at + 3] = tint[1];
    hudIndicatorVertices[at + 4] = tint[2];
    hudIndicatorVertices[at + 5] = tint[3];
    hudIndicatorVertexCount++;
  }

  function addHudIndicatorQuad(x, y, width, height, tint) {
    if (width <= 0 || height <= 0
        || hudIndicatorVertexCount + 4
          > hudIndicatorVertices.length / HUD_VERTEX_WORDS
        || hudIndicatorIndexCount + 6
          > HUD_INDICATOR_PRIMITIVE_LIMIT * 6)
      return false;
    // The extent these have always drawn on the PSP.
    const outerWidth = width + 1, outerHeight = height + 1;
    writeHudIndicatorVertex(x, y, tint);
    writeHudIndicatorVertex(x + outerWidth, y, tint);
    writeHudIndicatorVertex(x + outerWidth, y + outerHeight, tint);
    writeHudIndicatorVertex(x, y + outerHeight, tint);
    hudIndicatorIndexCount += 6;
    return true;
  }

  function addHudIndicatorTriangle(cx, cy, radius, angle, tint) {
    if (hudIndicatorVertexCount + 4
        > hudIndicatorVertices.length / HUD_VERTEX_WORDS
        || hudIndicatorIndexCount + 6
          > HUD_INDICATOR_PRIMITIVE_LIMIT * 6)
      return false;
    let firstX = 0, firstY = 0;
    for (let point = 0; point < 3; point++) {
      const theta = angle + point * Math.PI * 2 / 3;
      const x = cx + Math.sin(theta) * radius;
      const y = cy - Math.cos(theta) * radius;
      if (point === 0) { firstX = x; firstY = y; }
      writeHudIndicatorVertex(x, y, tint);
    }
    writeHudIndicatorVertex(firstX, firstY, tint);
    hudIndicatorIndexCount += 6;
    return true;
  }

  function addHudGlyph(character, x, y, scale, tint) {
    if (hudCharacterCount >= HUD_GLYPH_LIMIT) return false;
    const code = character.charCodeAt(0);
    const glyph = code < hudGlyphIndex.length ? hudGlyphIndex[code] : -1;
    hudCharacterCount++;
    if (glyph <= 0) return true;
    for (let at = hudGlyphStarts[glyph]; at < hudGlyphStarts[glyph + 1]; at++) {
      const run = hudGlyphRuns[at];
      if (!addHudQuad(x + (run & 7) * scale, y + ((run >> 3) & 7) * scale,
          (((run >> 6) & 7) + 1) * scale + HUD_INK_BOLD,
          ((run >> 9) + 1) * scale, tint)) return false;
    }
    return true;
  }

  function hudTextWidth(text, scale) {
    return text.length
      ? text.length * HUD_ADVANCE * scale - 2 * scale + HUD_INK_BOLD : 0;
  }

  function hudCenteredX(text, scale) {
    return 160 - (hudTextWidth(text, scale) >> 1);
  }

  function prepareHudTextSlice(text, x, y, scale, tint) {
    hudBuildText = String(text).toUpperCase();
    hudBuildTextAt = 0;
    hudBuildTextX = x;
    hudBuildTextY = y;
    hudBuildTextScale = scale;
    hudBuildTextTint = tint;
  }

  function rebuildHudMeshSlice(glyphLimit = 0) {
    if (hudBuildPhase < 0) {
      hudMeshDirty = false;
      hudVertexCount = hudIndexCount = hudCharacterCount = 0;
      hudBuildGenerating = state.mode === "generating";
      if (hudBuildGenerating) addHudQuad(40, 46, 240, 82, HUD_PANEL);
      hudBuildScore = hudScore || "00000";
      hudBuildArena = hudArena;
      hudBuildArmor = hudArmor;
      hudBuildGadget = hudGadget;
      hudBuildCommand = hudCommand;
      hudBuildToast = hudToast.slice(0, 20);
      hudBuildText = null;
      hudBuildPhase = 0;
      hudBuildToastIndex = -1;
    }
    while (hudBuildPhase < 6) {
      if (hudBuildText === null) {
        // Rows start where 180 -> 272 doubles glyph rows 0, 2, 4 and 6.
        if (hudBuildGenerating && hudBuildPhase === 0)
          prepareHudTextSlice(hudBuildToast,
            hudCenteredX(hudBuildToast, 2), 72, 2, HUD_GOLD);
        else if (hudBuildGenerating && hudBuildPhase === 1)
          prepareHudTextSlice(hudBuildArena,
            hudCenteredX(hudBuildArena, 1), 93, 1, HUD_ACCENT);
        else if (hudBuildGenerating)
          prepareHudTextSlice("", 0, 0, 1, HUD_TEXT);
        else if (hudBuildPhase === 0)
          prepareHudTextSlice(hudBuildScore, 7, 5, 1, HUD_TEXT);
        else if (hudBuildPhase === 1)
          prepareHudTextSlice(hudBuildArena,
            hudCenteredX(hudBuildArena, 1), 5, 1, HUD_ACCENT);
        else if (hudBuildPhase === 2)
          prepareHudTextSlice(hudBuildArmor,
            313 - hudTextWidth(hudBuildArmor, 1), 5, 1, HUD_TEXT);
        else if (hudBuildPhase === 3)
          prepareHudTextSlice(hudBuildGadget, 7, 168, 1, HUD_ACCENT);
        else if (hudBuildPhase === 4)
          prepareHudTextSlice(hudBuildCommand,
            313 - hudTextWidth(hudBuildCommand, 1), 168, 1, HUD_GOLD);
        else {
          hudBuildToastIndex = hudIndexCount;
          prepareHudTextSlice(hudBuildToast,
            hudCenteredX(hudBuildToast, 2), 27, 2, HUD_GOLD);
        }
      }
      if (!hudBuildText.length) {
        hudBuildText = null;
        hudBuildPhase++;
        continue;
      }
      /* The first retained HUD is built while the game is entering play and
         should become complete promptly. Later live updates use the smaller
         measured slice so a score/power-up change cannot spike a frame. */
      const glyphsThisSlice = glyphLimit || (hudTextUploads === 0
        ? HUD_GLYPHS_PER_SLICE * 2 : HUD_GLYPHS_PER_SLICE);
      const end = Math.min(hudBuildText.length,
        hudBuildTextAt + glyphsThisSlice);
      for (; hudBuildTextAt < end; hudBuildTextAt++) {
        if (!addHudGlyph(hudBuildText[hudBuildTextAt],
            hudBuildTextX + hudBuildTextAt * HUD_ADVANCE * hudBuildTextScale,
            hudBuildTextY, hudBuildTextScale, hudBuildTextTint)) {
          hudBuildTextAt = hudBuildText.length;
          break;
        }
      }
      if (hudBuildTextAt >= hudBuildText.length) {
        hudBuildText = null;
        hudBuildPhase++;
      }
      return;
    }
    {
      gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0,
        hudVertices.subarray(0, hudVertexCount * HUD_VERTEX_WORDS));
      hudPublishedIndexCount = hudIndexCount;
      hudPublishedVertexCount = hudVertexCount;
      hudPublishedToastStart = hudBuildToastIndex < 0 ? hudIndexCount : hudBuildToastIndex;
      hudToastStale = false;
      /* Indicators share the retained HUD buffer. A text rebuild can move
         their suffix, so republish that small suffix before the next draw. */
      hudIndicatorDirty = true;
      hudTextUploads++;
      hudBuildPhase = -1;
      return;
    }
  }

  /* Build and publish the whole retained HUD now rather than in slices
     over the next frames. Used where nothing earlier may stay on screen:
     the forge panel as a run starts generating (the sliced build left the
     arena, the player's tank and the last HUD showing for about five
     frames at 111 MHz) and a run's first HUD (deferred as optional work,
     it took 30-180 frames to appear). Both happen in the frame that starts
     the run, which arena setup or generation already makes a long one. */
  function publishHudNow() {
    hudBuildPhase = -1;
    do rebuildHudMeshSlice(HUD_GLYPH_LIMIT); while (hudBuildPhase >= 0);
    rebuildHudIndicators();
  }

  function rebuildHudIndicators() {
    hudIndicatorDirty = false;
    hudIndicatorVertexCount = hudIndicatorIndexCount = 0;
    if (state.mode === "generating") {
      hudPublishedIndicatorIndexCount = 0;
      return;
    }
    // The gadget track turns amber while the lunge recharges.
    addHudIndicatorQuad(7, 176, 86, 2,
      hudLungeCooling ? HUD_LUNGE_TRACK : HUD_GADGET_TRACK);
    addHudIndicatorQuad(7, 176,
      Math.max(1, (86 * hudGadgetRatio) | 0), 2, HUD_ACCENT);
    if (isOnslaught()) {
      addHudIndicatorQuad(117, 176, 86, 2, HUD_MULTIPLIER_TRACK);
      addHudIndicatorQuad(117, 176,
        Math.max(1, (86 * hudMultiplierRatio) | 0), 2, HUD_GOLD);
    }
    if (hudCommand) {
      addHudIndicatorQuad(227, 176, 86, 2, HUD_COMMAND_TRACK);
      addHudIndicatorQuad(227, 176,
        Math.max(1, (86 * hudCommandRatio) | 0), 2, HUD_GOLD);
    }
    /* Keep the objective cue on a broad compass ellipse (radii
       HUD_OBJECTIVE_RADIUS_X and _Y around the screen centre). The old 38x30
       ellipse placed it directly over the player's tank and made an authored
       direction marker look like broken model geometry. */
    if (hudObjectiveVisible) {
      const objectiveX = 160 + Math.sin(hudObjectiveAngle) * HUD_OBJECTIVE_RADIUS_X;
      const objectiveY = 90 - Math.cos(hudObjectiveAngle) * HUD_OBJECTIVE_RADIUS_Y;
      addHudIndicatorTriangle(objectiveX, objectiveY, 3,
        hudObjectiveAngle, HUD_ACCENT);
    }
    if (hudDamageVisible) {
      const damageX = 160 + Math.sin(hudDamageAngle) * 49;
      const damageY = 90 - Math.cos(hudDamageAngle) * 39;
      addHudIndicatorTriangle(damageX, damageY, 5,
        hudDamageAngle, HUD_WARNING);
    }
    if (hudHitVisible) {
      addHudIndicatorQuad(154, 88, 4, 1, HUD_GOLD);
      addHudIndicatorQuad(162, 88, 4, 1, HUD_GOLD);
      addHudIndicatorQuad(159, 83, 1, 4, HUD_GOLD);
      addHudIndicatorQuad(159, 91, 1, 4, HUD_GOLD);
    }
    const availableVertices = Math.max(0,
      hudVertices.length / HUD_VERTEX_WORDS - hudPublishedVertexCount);
    if (hudIndicatorVertexCount > availableVertices) {
      const completePrimitives = Math.floor(availableVertices / 4);
      hudIndicatorVertexCount = completePrimitives * 4;
      hudIndicatorIndexCount = completePrimitives * 6;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
    gl.bufferSubData(gl.ARRAY_BUFFER,
      hudPublishedVertexCount * HUD_VERTEX_WORDS * 4,
      hudIndicatorVertices.subarray(
      0, hudIndicatorVertexCount * HUD_VERTEX_WORDS));
    hudPublishedIndicatorIndexCount = hudIndicatorIndexCount;
    hudIndicatorUploads++;
  }

  function bindHudGeometry() {
    if (vertexArrays) vertexArrays.bindVertexArrayOES(hudVertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
      gl.enableVertexAttribArray(0);
      gl.enableVertexAttribArray(1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    }
  }

  /* The arena-clear transition looks like play: score, armor and the
     ARENA CLEAR toast stay up until the next arena starts. */
  function hudModeVisible(mode) {
    return mode === "playing" || mode === "arena-clear" || mode === "killcam"
      || mode === "generating";
  }

  function drawHud() {
    if ((!hudPublishedIndexCount && !hudPublishedIndicatorIndexCount)
        || !hudModeVisible(state.mode)) return;
    gl.disable(gl.DEPTH_TEST);
    /* Only authored ink has geometry, so stale texture state cannot turn
       glyph cells into opaque blocks. Blending remains for translucent bars. */
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    /* HUD indices reverse the pixel-space winding, keeping ink CCW in clip
       space. Culling
       the back face also promises that these tiny pixel-aligned glyph quads
       do not need the scene's costly edge-AA expansion on PSP. */
    gl.enable(gl.CULL_FACE);
    gl.frontFace(gl.CCW);
    gl.cullFace(gl.BACK);
    gl.useProgram(hudProgram);
    bindHudGeometry();
    /* Text and changing indicators occupy one contiguous retained stream.
       One draw snapshots one invariant HUD command instead of rebuilding a
       second WebGL command every frame on the QuickJS hot path. */
    if (hudToastStale && hudPublishedToastStart < hudPublishedIndexCount) {
      if (hudPublishedToastStart)
        gl.drawElements(gl.TRIANGLES, hudPublishedToastStart, gl.UNSIGNED_SHORT, 0);
      if (hudPublishedIndicatorIndexCount) gl.drawElements(gl.TRIANGLES,
        hudPublishedIndicatorIndexCount, gl.UNSIGNED_SHORT, hudPublishedIndexCount * 2);
    } else gl.drawElements(gl.TRIANGLES,
      hudPublishedIndexCount + hudPublishedIndicatorIndexCount,
      gl.UNSIGNED_SHORT, 0);
    gl.disable(gl.CULL_FACE);
    gl.enable(gl.DEPTH_TEST);
    gl.useProgram(program);
  }

  function uploadBoxInstances() {
    if (!boxInstanceProgram || !boxInstanceCount) return;
    /* The visible count changes whenever a particle or pickup appears. Keep
       the largest admitted view rather than allocating two replacement
       typed-array views on every rise and fall. Stale suffix records are
       uploaded but never drawn, and the bounded high-water cost is only the
       already-owned 15 KiB instance store. */
    if (boxInstanceUploadCount < boxInstanceCount) {
      boxInstanceUploadCount = boxInstanceCount;
      boxInstanceMatrixBytes = new Uint8Array(
        boxInstanceMatrices.buffer, 0, boxInstanceCount * 16 * 4);
      boxInstanceTintBytes = new Uint8Array(
        boxInstanceTints.buffer, 0, boxInstanceCount * 4 * 4);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, boxInstanceMatrixBytes);
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, boxInstanceTintBytes);
  }

  function drawBoxInstances() {
    if (!boxInstanceProgram || !boxInstanceCount) return;
    gl.useProgram(boxInstanceProgram);
    gl.uniformMatrix4fv(boxInstanceViewProjectionLocation, false,
      viewProjection);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(boxInstanceVertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(0);
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    }
    // One draw: every add stops at MAX_BOX_INSTANCES, one draw's worth.
    if (!vertexArrays) {
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
      for (let column = 0; column < 4; column++) {
        const location = boxInstanceMatrixLocation + column;
        gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64, column * 16);
        gl.enableVertexAttribArray(location);
        instancing.vertexAttribDivisorANGLE(location, 1);
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
      gl.vertexAttribPointer(boxInstanceTintLocation, 4, gl.FLOAT, false, 16, 0);
      gl.enableVertexAttribArray(boxInstanceTintLocation);
      instancing.vertexAttribDivisorANGLE(boxInstanceTintLocation, 1);
    }
    instancing.drawElementsInstancedANGLE(gl.TRIANGLES, BOX_INDICES.length,
      gl.UNSIGNED_SHORT, 0, boxInstanceCount);
  }

  function addRamp(x, z, width, depth, height, direction) {
    const left = x - width * .5, right = x + width * .5;
    const near = z - depth * .5, far = z + depth * .5;
    const lowZ = direction > 0 ? near : far;
    const highZ = direction > 0 ? far : near;
    /* Match the continuous physics slope with an 18-index wedge. */
    addStaticQuad(left, 0, lowZ, right, 0, lowZ,
      right, height, highZ, left, height, highZ, COLORS.obstacle, 1.13);
    addStaticTriangle(left, 0, lowZ, left, height, highZ,
      left, 0, highZ, COLORS.obstacle, .78);
    addStaticTriangle(right, 0, highZ, right, height, highZ,
      right, 0, lowZ, COLORS.obstacle, .9);
    addStaticQuad(left, 0, highZ, left, height, highZ,
      right, height, highZ, right, 0, highZ, COLORS.obstacle, .72);
  }

  function uploadMesh(vertexArray, position, color, index,
                      firstVertex = 0, firstIndex = 0) {
    /* ELEMENT_ARRAY_BUFFER is VAO state. Bind the owner before updating it:
       otherwise an arena rebuild can silently replace the HUD or instance
       index binding and invalidate an otherwise reusable frame packet. */
    if (vertexArrays) vertexArrays.bindVertexArrayOES(vertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.bufferSubData(gl.ARRAY_BUFFER, firstVertex * 3 * 4,
      positions.subarray(firstVertex * 3, vertexCount * 3));
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.bufferSubData(gl.ARRAY_BUFFER, firstVertex * 4 * 4,
      colors.subarray(firstVertex * 4, vertexCount * 4));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, firstIndex * 2,
      indices.subarray(firstIndex, indexCount));
  }

  function drawMesh(vertexArray, position, color, index, count) {
    if (!count) return;
    if (vertexArrays) vertexArrays.bindVertexArrayOES(vertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, position);
      gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(positionLocation);
      gl.bindBuffer(gl.ARRAY_BUFFER, color);
      gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(colorLocation);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    }
    gl.drawElements(gl.TRIANGLES, count, gl.UNSIGNED_SHORT, 0);
  }

  const projection = new Float32Array(16);
  const view = new Float32Array(16);
  const viewProjection = new Float32Array(16);

  function perspective(out, fieldOfView, aspect, near, far) {
    const f = 1 / Math.tan(fieldOfView * .5);
    out.fill(0);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1;
    out[14] = 2 * far * near / (near - far);
  }

  function lookAt(out, ex, ey, ez, cx, cy, cz) {
    let zx = ex - cx, zy = ey - cy, zz = ez - cz;
    let length = Math.hypot(zx, zy, zz) || 1;
    zx /= length; zy /= length; zz /= length;
    let xx = zz, xy = 0, xz = -zx;
    length = Math.hypot(xx, xz) || 1;
    xx /= length; xz /= length;
    const yx = zy * xz, yy = zz * xx - zx * xz, yz = -zy * xx;
    out[0] = xx; out[1] = yx; out[2] = zx; out[3] = 0;
    out[4] = xy; out[5] = yy; out[6] = zy; out[7] = 0;
    out[8] = xz; out[9] = yz; out[10] = zz; out[11] = 0;
    out[12] = -(xx * ex + xy * ey + xz * ez);
    out[13] = -(yx * ex + yy * ey + yz * ez);
    out[14] = -(zx * ex + zy * ey + zz * ez);
    out[15] = 1;
  }

  function multiply(out, a, b) {
    for (let column = 0; column < 4; column++) {
      const at = column * 4;
      const b0 = b[at], b1 = b[at + 1], b2 = b[at + 2], b3 = b[at + 3];
      out[at] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
      out[at + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
      out[at + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
      out[at + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
    }
  }

  perspective(projection, 51 * Math.PI / 180, 320 / 180, .25, 45);

  const ARENAS = globalThis.__treadlineArenaData.arenas;
  const generatedArena = globalThis.__treadlineArenaData.generatedArena;
  const AUTHORED_ARENA_COUNT = 3;
  const GENERATED_ARENA_INDEX = AUTHORED_ARENA_COUNT;
  const TANK_COLORS = [
    [.1, .92, .67], [1, .23, .16], [1, .51, .08],
    [.73, .25, 1], [1, .76, .1], [.16, .57, 1],
  ];
  const arenaSpatial = globalThis.__treadlineArenaData.spatial;
  // Half-unit cells over [-8, 8): cell = floor((v + ORIGIN) * SCALE).
  const COLLISION_GRID_SIDE = 32, COLLISION_GRID_SCALE = 2;
  const COLLISION_GRID_ORIGIN = 8, COLLISION_GRID_LAST = COLLISION_GRID_SIDE - 1;
  const COLLISION_GRID_CELLS = 1024;
  const ARENA_OBSTACLE_BOUNDS = arenaSpatial.obstacleBounds;
  const ARENA_GATE_BOUNDS = arenaSpatial.gateBounds;
  const ARENA_OBSTACLE_COUNTS = arenaSpatial.obstacleCounts;
  const ARENA_GATE_COUNTS = arenaSpatial.gateCounts;
  const ARENA_RAMP_GRIDS = arenaSpatial.rampGrids;
  const ARENA_BULLET_OBSTACLE_GRIDS = arenaSpatial.bulletObstacleGrids;
  const ARENA_BULLET_GATE_GRIDS = arenaSpatial.bulletGateGrids;
  const ARENA_CIRCLE_OBSTACLE_GRIDS = arenaSpatial.circleObstacleGrids;
  const ARENA_CIRCLE_GATE_GRIDS = arenaSpatial.circleGateGrids;
  const arenaObstacleCount = (arena) => arenaSpatial.itemCount(arena, "obstacles");
  const arenaBarrierCount = (arena) => arenaSpatial.itemCount(arena, "barriers");
  const arenaRampCount = (arena) => arenaSpatial.itemCount(arena, "ramps");
  const arenaGateCount = (arena) => arenaSpatial.itemCount(arena, "gates");
  function fillArenaSpatialData(arena) {
    arenaSpatial.fillSpatialData(arena);
    aimGeometryEpoch++;
    if (state.arena === arena) {
      refreshSceneryHeights();
      fillTinyCircleOccupied();
      // The final crate/barrier placement publishes the new ray index once.
      // Queries during replacement use the exact fallback in the meantime.
      rayGeometryArena = -1;
    }
  }
  const COLORS = {
    ground: [.052, .115, .12], grid: [.075, .225, .215],
    wall: [.145, .315, .31], obstacle: [.175, .255, .265],
    tread: [.035, .045, .048],
    barrel: [.12, .14, .13], shadow: [.01, .015, .015],
    bullet: [1, .82, .12], health: [.2, 1, .4], healthLost: [.28, .1, .08],
    pickup: [.08, 1, .82], particle: [1, .38, .1], muzzle: [1, .96, .56],
  };
  const CONTROL_BLUE = [.12, .55, .48];
  const CONTROL_RED = [.5, .2, .18];
  const CONVOY_BODY = [.42, .58, .62];
  const CONVOY_TOP = [.2, .75, .72];
  const SMOKE_LOW = [.34, .38, .39];
  const BARRIER_INTACT = [.36, .29, .2];
  const BARRIER_BROKEN = [.2, .17, .13];

  const GAME_MODES = ["SURVIVAL", "TEAM CONTROL", "CONVOY ESCORT",
    "ONSLAUGHT", "DAILY ARENA", "BILLIARDS", "PASS THE PSP"];
  // Their indices (state.gameMode; saves and replays carry them).
  const MODE_SURVIVAL = 0, MODE_CONTROL = 1, MODE_CONVOY = 2, MODE_ONSLAUGHT = 3,
    MODE_DAILY = 4, MODE_BILLIARDS = 5, MODE_DUEL = 6;
  const MODE_DESCRIPTIONS = [
    "Clear three arenas before your last redeploy.",
    "Fight beside an ally and hold the central transmitter.",
    "Stay near the crawler and protect it through the arena.",
    "Survive escalating waves and protect your score chain.",
    "One UTC-day arena, fixed loadout, and a local daily best.",
    "Only bounced shells can finish an enemy. Clear three arenas.",
    "Two players, one PSP. Aim, fire, then hand over for the next turn.",
  ];
  const GADGETS = ["MINES", "SMOKE", "SHIELD", "REPAIR DRONE", "BOOST TREADS"];
  const DIFFICULTIES = ["CADET", "VETERAN", "ACE"];
  // Daily Run bots play at Veteran, whatever the player chose.
  const DAILY_DIFFICULTY = 1;
  const CAMERA_MODES = ["STABLE", "FOLLOW"];
  const OBJECTIVE_ARROW_NAMES = ["Auto", "On", "Off"];
  const AIM_ASSISTS = ["OFF", "SNAP", "LOCK"];
  const ASSIST_OFF = 0, ASSIST_SNAP = 1, ASSIST_LOCK = 2;
  const REVERSE_DEGREES = [90, 105, 120];
  const PALETTES = ["DAY", "DUSK", "NIGHT", "STORM"];
  const PALETTE_TINTS = [[1, 1, 1], [1.12, .78, .65], [.48, .66, .9], [.64, .82, .8]];
  const PAINT_NAMES = ["JADE", "COPPER", "ICE", "ROYAL", "SUNBURST", "PEARL"];
  const PAINT_COLORS = [[.1, .92, .67], [1, .47, .16], [.22, .82, 1],
    [.72, .4, 1], [1, .84, .16], [.92, .92, 1]];
  // Preserve the former Float32 intermediate in both instanced and fallback
  // rendering, without recomputing three unchanged channels per tank/frame.
  const TANK_BASE_TINTS = TANK_COLORS.map(color => new Float32Array(color));
  /* The campaign and the Practice Range repaint hulls for a while:
     setTankTint(id, rgb, flash) paints the body rgb and its hit and fire
     flashes flash (else rgb); restoreTankTints puts the originals back.
     Gold and cyan are the targets' shared signal colours. */
  const TANK_DEFAULT_COLORS = TANK_COLORS.map(color => color.slice());
  const GOLD_TINT = [1, .84, .3], CYAN_TINT = [.3, .9, 1];
  function setTankTint(id, rgb, flash = rgb) {
    const base = TANK_BASE_TINTS[id], color = TANK_COLORS[id];
    for (let c = 0; c < 3; c++) { base[c] = rgb[c]; color[c] = flash[c]; }
  }
  function restoreTankTints() {
    for (let id = 0; id < TANK_COLORS.length; id++)
      setTankTint(id, TANK_DEFAULT_COLORS[id]);
  }
  const PAINT_BASE_TINTS = PAINT_COLORS.map(color => new Float32Array(color));
  const HEALTH_BASE_TINT = new Float32Array(COLORS.health);
  const CLASSES = [
    {name: "SCOUT", secondary: "BURST", health: 70, speed: 3.15,
      reload: .31, scale: .86},
    {name: "STRIKER", secondary: "SABOT", health: 100, speed: 2.55,
      reload: .43, scale: 1},
    {name: "BULWARK", secondary: "CANISTER", health: 150, speed: 1.92,
      reload: .56, scale: 1.14},
  ];
  const CLASS_SCALE = new Float32Array(CLASSES.length);
  const CLASS_SPEED = new Float32Array(CLASSES.length);
  const CLASS_COLLISION_RADIUS = new Float32Array(CLASSES.length);
  const CLASS_COMBINED_RADIUS_SQUARED = new Float32Array(
    CLASSES.length * CLASSES.length);
  // A hull's collision radius per unit of its scale.
  const HULL_RADIUS_PER_SCALE = .52;
  for (let classId = 0; classId < CLASSES.length; classId++) {
    CLASS_SCALE[classId] = CLASSES[classId].scale;
    CLASS_SPEED[classId] = CLASSES[classId].speed;
    CLASS_COLLISION_RADIUS[classId] = HULL_RADIUS_PER_SCALE * CLASSES[classId].scale;
  }
  /* The boss (every fifth Onslaught wave; the campaign's SUNSET): a scaled
     Bulwark. Tread and turret armor by difficulty; Onslaught's is Ace's. */
  const BOSS = Object.freeze({scale: 1.45, collisionRadius: HULL_RADIUS_PER_SCALE * 1.45,
    driveSpeed: 1.15, health: 500, treadHealth: [70, 90, 110], turretHealth: [120, 150, 180]});
  function makeBoss(tank, difficulty) {
    tank.boss = true; tank.maxHealth = tank.health = BOSS.health;
    tank.leftTreadHealth = tank.rightTreadHealth = BOSS.treadHealth[difficulty];
    tank.turretHealth = BOSS.turretHealth[difficulty];
    tank.scale = BOSS.scale; tank.collisionRadius = BOSS.collisionRadius;
    tank.driveSpeed = BOSS.driveSpeed;
  }
  for (let left = 0; left < CLASSES.length; left++) {
    for (let right = 0; right < CLASSES.length; right++) {
      /* Use the same radii that admit scenery motion. The older .42-scale
         shortcut let two visible hulls overlap before tank contact was
         reported, then made the separation correction look like a jump. */
      const combined = CLASS_COLLISION_RADIUS[left]
        + CLASS_COLLISION_RADIUS[right];
      CLASS_COMBINED_RADIUS_SQUARED[left * CLASSES.length + right] =
        combined * combined;
    }
  }
  const STORAGE_KEY = "treadline-settings-v1";
  /* Eighteen authored projectiles leave room for all critical tank/scenery
     instances inside the single 64-instance repeated draw. */
  const MAX_TANKS = 6, MAX_BULLETS = 18, MAX_PARTICLES = 48, MAX_PICKUPS = 6;
  const MAX_MINES = 12, MAX_SMOKE = 6, MAX_BARRIERS = 6;
  // Barrier positions are fixed for an arena; destruction only changes
  // active flags. One startup-owned byte per cell skips unrelated boxes.
  const barrierCircleGrid = new Uint8Array(COLLISION_GRID_CELLS);
  const crateCircleGrid = new Uint8Array(COLLISION_GRID_CELLS);
  let crateGridReady = false;
  // The camera probe's tiny circle often occupies entirely open space. Keep
  // one conservative union byte per cell so those queries skip all category
  // walks. Active changes only add false candidates; exact tests remain live.
  const tinyCircleOccupied = new Uint8Array(COLLISION_GRID_CELLS);
  let tinyCircleArena = -1;

  // Rays share their direction and conservative axis candidates across all
  // exact box tests. Placement-only tables: destruction changes active bits,
  // and a gate toggle never rebuilds the 8 KiB interval index. Float64 bounds
  // retain the original arithmetic (dynamic rectangles are JS numbers).
  const rayBounds = new Float64Array(32 * 4);
  /* Boxes grown by the default .08 sight padding, with their sums and
     spans: what lineCrossesWalls derives per candidate, precomputed. */
  const RAY_AIM_PADDING = .08;
  const rayAimBoxes = new Float64Array(32 * 8);
  const rayAxisMasks = new Uint32Array(64);
  const rayIntervalMasks = new Uint32Array(2048);
  /* Per half-unit cell, the ray ids whose shape a shell can touch there
     (the rectangle grown by the shell radius): one step's segment spans at
     most four cells, so its exact candidates are a few ORs. */
  const shellCellMasks = new Uint32Array(COLLISION_GRID_CELLS);
  const rayBlockers = new Int16Array(MAX_TANKS * 12).fill(-1);
  const RAY_BARRIER_MASK = 0x03f00000;
  let rayGeometryArena = -1, rayActiveMask = 0, rayGateMask = 0;

  // How far the circle candidate grids reach (arena-generator.js).
  const CIRCLE_GRID_RADIUS = arenaSpatial.CIRCLE_GRID_RADIUS;

  /* bots.js's breach planning (planBreachShot) asks this; it stays here
     with the shell cast state it reads. Would a shell fired along this
     heading stop on the blocker (a bounce at most)? */
  function breachShotHits(tank, angle) {
    const direction = orientationIndex(angle);
    shellDirection(directionSines[direction], directionCosines[direction]);
    let dx = shellDirX, dz = shellDirZ;
    const overCover = elevatedFiringOrigin(tank);
    const reach = shellReach(tank.x, tank.z, dx, dz, overCover);
    let x = tank.x + dx * reach, z = tank.z + dz * reach, length = 21 - reach;
    const id = tank.breachKind === 1 ? tank.breachIndex : 20 + tank.breachIndex;
    for (let leg = 0; leg < 2; leg++) {
      castActive = shellActiveMask(overCover); castSkip = -1;
      castShell(x, z, dx, dz, length, overCover);
      if (castKind === SHELL_STOP) return castId === id;
      if (castKind !== SHELL_BOUNCE || castEmbedded) return false;
      x += dx * castLength; z += dz * castLength; length -= castLength;
      shellReflect(dx, dz);
      if (shellFlipX) dx = -dx;
      if (shellFlipZ) dz = -dz;
    }
    return false;
  }

  /* The bot league (qualification.js) puts bots on both sides; its
     scripted player, when it has one, is qualificationHooks.leaguePlayer. */
  let botLeagueActive = false;
  // Bump for any simulation/seed mapping change, not just codec changes.
  // 11: config slot 5 also carries the aim guide level (score multiplier).
  // 12: shells sweep their path (contacts, bounces, hulls) between steps.
  // 13: tanks are never placed sealed in, and bots breach out of pockets.
  // 14: turret headings are wrapped to [-pi, pi] (rounding changes).
  const REPLAY_VERSION = 14, REPLAY_CAPACITY = 8192, REPLAY_WORDS = 8;
  const replayLog = new Float64Array(REPLAY_CAPACITY * REPLAY_WORDS);
  const replayConfig = new Float64Array(16);
  const replayPrevious = new Float64Array(REPLAY_WORDS);
  const replayCodec = new Uint8Array(128 * 1024);
  const replayCodecView = new DataView(replayCodec.buffer);
  let replayCount = 0, replayCursor = 0, replayDigest = 0;
  let replayRecording = false, replayActive = false, replayPending = false;
  let replayTruncated = false, replayVerified = false;
  let replayLastSim = 0, replayLastWall = 0, replayLastFrame = 0;
  let replayFrameSim = 0, replayFrameWall = 0, replayPlaybackFrame = 0;
  /* Killcam copies the existing packed render streams, not entity objects.
     Fixed views are created once; capture does no allocation or new draws. */
  const KILLCAM_FRAMES = 48;
  const killcamMatrices = new Float32Array(KILLCAM_FRAMES * MAX_BOX_INSTANCES * 16);
  const killcamTints = new Float32Array(KILLCAM_FRAMES * MAX_BOX_INSTANCES * 4);
  const killcamCameras = new Float32Array(KILLCAM_FRAMES * 16);
  const killcamMatrixViews = Array.from({length: KILLCAM_FRAMES}, (_, at) =>
    new Float32Array(killcamMatrices.buffer, at * MAX_BOX_INSTANCES * 64, MAX_BOX_INSTANCES * 16));
  const killcamTintViews = Array.from({length: KILLCAM_FRAMES}, (_, at) =>
    new Float32Array(killcamTints.buffer, at * MAX_BOX_INSTANCES * 16, MAX_BOX_INSTANCES * 4));
  const killcamCameraViews = Array.from({length: KILLCAM_FRAMES}, (_, at) =>
    new Float32Array(killcamCameras.buffer, at * 64, 16));
  const killcamCounts = new Uint8Array(KILLCAM_FRAMES);
  const killcamStaticCounts = new Uint16Array(KILLCAM_FRAMES);
  const killcamTimes = new Float64Array(KILLCAM_FRAMES);
  let killcamCount = 0, killcamWrite = 0, killcamRead = 0;
  let killcamLastTime = -99, killcamElapsed = 0, killcamStartTime = 0;
  let killcamDeathStaticCount = 0;
  const tanks = Array.from({length: MAX_TANKS}, (_, id) => ({
    id, active: false, player: id === 0, x: 0, z: 0, surfaceY: 0,
    yaw: 0, turret: 0, yawSine: 0, yawCosine: 1,
    turretSine: 0, turretCosine: 1,
    team: id === 0 ? 0 : 1, health: 100, maxHealth: 100,
    classId: id % CLASSES.length, cooldown: 0, secondaryCooldown: 0,
    scale: CLASS_SCALE[id % CLASSES.length],
    driveSpeed: CLASS_SPEED[id % CLASSES.length],
    collisionRadius: CLASS_COLLISION_RADIUS[id % CLASSES.length],
    commandBuff: 0, shield: 0,
    fireHeld: false, fireCharge: 0,
    lunge: 0, lungeCooldown: 0, lungeSign: 1, drift: 0,
    repair: 0, boost: 0, recoil: 0, hitFlash: 0,
    fireWindup: 0, fireSecondaryArmed: false, fireTelegraphed: false,
    renderTint: new Float32Array(3), spawnGrace: 0, respawn: 0, inert: false,
    gadget: GADGETS[id % GADGETS.length], gadgetCooldown: 0,
    gadgetCooldownMax: 1,
    aiThink: 0, target: -1, role: "HUNTER", blockedTime: 0, guardIdle: 0,
    velocityX: 0, velocityZ: 0, difficulty: -1,
    boss: false, leftTreadHealth: 0, rightTreadHealth: 0, turretHealth: 0,
    slideX: 0, slideZ: 0,
    aggression: .8 + (id % 3) * .2, preferredRange: 3.1 + (id % 3) * .45,
    aimErrorX: 0, aimErrorZ: 0, aimRefresh: 0, bankAim: false,
    bankNext: 0, bankTarget: -1, bankRevision: -1,
    targetChoice: -1, targetNext: 0, targetEpoch: -1,
    bankMirrorX: 0, bankMirrorZ: 0, bankHitX: 0, bankHitZ: 0,
    bankTargetX: 0, bankTargetZ: 0,
    navGoal: -1, navRevision: -1, navFieldSlot: -1,
    navRawGoal: -1, navMappedGoal: -1, navMapRevision: -1, routeSteering: false,
    navWaypoint: -1, navWaypointFrom: -1,
    strategyNext: 0, strategyTarget: -1, strategyEpoch: -1, strategyMood: -1,
    strategyGoalX: 0, strategyGoalZ: 0, strategyConvoy: false, strategyHolding: false,
    strategySteerX: 0, strategySteerZ: 0,
    orbitRoute: false, orbitRouteX: 0, orbitRouteZ: 0,
    breachActive: false, breachKind: 0, breachIndex: -1, breachRevision: -1,
    breachStandX: 0, breachStandZ: 0, breachAngle: NaN, breachShotNext: 0,
    breachHold: false, breachShot: false, breachShots: 0, breaches: 0,
    breachX: 0, breachZ: 0, breachRetry: 0, breachQueued: false, breachQueuedSlot: 0,
    breachQueuedKey: -1, breachQueuedRevision: -1,
    avoidTime: 0, avoidTurn: 1, backingOff: false, standoff: false,
    orbitTurn: 1,
    lastAimX: NaN, lastAimZ: NaN, aimAngle: 0,
    treadDistance: 0,
    command: {left: 0, right: 0, reverse: false, aimX: 0, aimZ: 1,
      fire: false, secondary: false, gadget: false, ultimate: false, lunge: 0},
  }));
  const bullets = Array.from({length: MAX_BULLETS}, () => ({
    active: false, owner: 0, x: 0, z: 0, vx: 0, vz: 0, life: 0, speed: 0,
    heading: 0, headingSine: 0, headingCosine: 1,
    bounces: 0, bounceCount: 0, bounceFlash: 0,
    damage: 0, barrierDamage: 1, pierce: 0, pierceId: -1,
    bypassShield: false, overCover: false, turns: 0,
  }));
  /* Where each shell turned during its last step (bullet.turns points,
     x then z): qualification.js's (for the fixtures and the invariant
     sweep), installed with its statistics; never read back here. */
  let bulletTurns = null;
  const particles = Array.from({length: MAX_PARTICLES}, () => ({
    active: false, x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0, life: 0, maximum: 0,
    renderSine: 0, renderCosine: 1, renderStreak: .12,
  }));
  const MAX_DECALS = 12;
  /* Flat things on the floor all write depth (the browser has no depth
     mask), so two that overlap must never share a plane, or nearly: the
     16-bit depth buffer then picks the winner per pixel (per triangle on
     the PSP GE) and the overlap flickers as the camera moves. Each flat
     layer's top sits FLAT_STEP above the one below, two or more depth
     steps apart across the arena at the camera's angle
     (tests/fixtures/treadline-layers.js). From the floor up: grid lines
     (.014) and obstacle contact shadows (.03), both static; ground marks,
     decals and hazards, on MARK_LEVELS levels so marks that overlap stack
     (groundMarkLevel); the control zone; the objective plinth (.09); tank
     shadows; the aim guide's enemy plate; mines, three levels. */
  const FLAT_STEP = .012, MARK_TOP = .042, MARK_LEVELS = 3;
  const CONTROL_ZONE_TOP = MARK_TOP + MARK_LEVELS * FLAT_STEP;
  const TANK_SHADOW_TOP = .102, ENEMY_PLATE_TOP = .114, MINE_TOP = .132;
  const retainedEffectSlots = new Int8Array(MAX_PARTICLES + MAX_DECALS);
  retainedEffectSlots.fill(-1);
  const decals = Array.from({length: MAX_DECALS}, () => ({
    active: false, kind: 0, x: 0, y: 0, z: 0, yaw: 0,
    sine: 0, cosine: 1,
    width: 0, depth: 0, life: 0, maximum: 0, radius: 0, level: 0,
  }));
  let decalCursor = 0, decalRecycles = 0;
  /* A destroyed tank's scorch lands once its sparks have burned out. The
     kill frame is the busiest frame there is (sparks, sound, score, HUD), so
     the decal's spawn and first admission wait for an ordinary one. */
  const WRECK_DELAY = .8, MAX_WRECKS = 4;
  const wreckWaits = new Float32Array(MAX_WRECKS);
  const wreckPoses = new Float32Array(MAX_WRECKS * 3);
  let wreckCount = 0;
  let instanceCapHitFrames = 0, instanceCapHitThisFrame = false;
  let droppedDecalInstances = 0, droppedParticleInstances = 0;
  let bulletActiveMask = 0;
  let particleActiveLowMask = 0, particleActiveHighMask = 0;
  function setBulletActive(at, active) {
    const bit = 1 << at;
    bullets[at].active = active;
    bulletActiveMask = (active
      ? bulletActiveMask | bit : bulletActiveMask & ~bit) >>> 0;
  }
  // A new arena (or replay) starts every effect lifetime afresh.
  function clearEffects() {
    for (const particle of particles) particle.active = false;
    for (const decal of decals) decal.active = false;
    decalCursor = wreckCount = 0;
    particleActiveLowMask = particleActiveHighMask = 0;
  }

  function setParticleActive(at, active) {
    const bit = 1 << (at & 31);
    particles[at].active = active;
    if (active) retainedEffectSlots[at] = -1;
    if (at < 32)
      particleActiveLowMask = (active
        ? particleActiveLowMask | bit : particleActiveLowMask & ~bit) >>> 0;
    else
      particleActiveHighMask = (active
        ? particleActiveHighMask | bit : particleActiveHighMask & ~bit) >>> 0;
  }

  /* The lowest ground-mark level no active mark within reach of the
     circle (x, z, radius) uses. When all are taken, the level of the oldest
     overlapping decal is freed by retiring the decals there, unless a
     hazard holds it too; a hazard is never retired, and -1 means hazards
     hold every level. skip is a decal slot about to be recycled. Marks
     keep their level for life. */
  function groundMarkLevel(x, z, radius, skip = -1) {
    let used = 0, hazardLevels = 0;
    for (let at = 0; at < MAX_DECALS; at++) {
      const decal = decals[at];
      if (!decal.active || at === skip) continue;
      const dx = decal.x - x, dz = decal.z - z, reach = decal.radius + radius;
      if (dx * dx + dz * dz < reach * reach) used |= 1 << decal.level;
    }
    for (let at = 0; at < hazards.length; at++) {
      const hazard = hazards[at];
      if (!hazard.active) continue;
      // Hazards are axis-aligned squares: the circle's distance to one.
      const half = hazard.renderDiameter * .5;
      const dx = Math.max(0, Math.abs(hazard.x - x) - half);
      const dz = Math.max(0, Math.abs(hazard.z - z) - half);
      if (dx * dx + dz * dz < radius * radius) hazardLevels |= 1 << hazard.level;
    }
    used |= hazardLevels;
    for (let level = 0; level < MARK_LEVELS; level++)
      if (!(used & (1 << level))) return level;
    // Oldest first: the ring's next slot to recycle is its oldest decal.
    for (let age = 0; age < MAX_DECALS; age++) {
      const at = (decalCursor + age) % MAX_DECALS, decal = decals[at];
      if (!decal.active || at === skip || hazardLevels & (1 << decal.level)) continue;
      const dx = decal.x - x, dz = decal.z - z, reach = decal.radius + radius;
      if (dx * dx + dz * dz >= reach * reach) continue;
      const level = decal.level;
      for (let other = 0; other < MAX_DECALS; other++) {
        const old = decals[other];
        if (!old.active || other === skip || old.level !== level) continue;
        const ox = old.x - x, oz = old.z - z, near = old.radius + radius;
        if (ox * ox + oz * oz < near * near) old.active = false;
      }
      return level;
    }
    return -1;
  }

  function spawnDecal(kind, x, z, yaw, width = .42, depth = .18) {
    const decal = decals[decalCursor];
    const radius = Math.sqrt(width * width + depth * depth) * .5;
    const level = groundMarkLevel(x, z, radius, decalCursor);
    if (level < 0) return;
    retainedEffectSlots[MAX_PARTICLES + decalCursor] = -1;
    if (decal.active) decalRecycles++;
    decalCursor = (decalCursor + 1) % MAX_DECALS;
    decal.active = true;
    decal.kind = kind;
    decal.radius = radius; decal.level = level;
    decal.x = x; decal.z = z;
    // FLAT_STEP thick, so stacked decals' side faces never overlap.
    decal.y = surfaceHeightAt(x, z) + MARK_TOP + (level - .5) * FLAT_STEP;
    decal.yaw = yaw; decal.width = width; decal.depth = depth;
    const direction = orientationIndex(yaw);
    decal.sine = directionSines[direction];
    decal.cosine = directionCosines[direction];
    decal.life = decal.maximum = kind === 1 ? 6 : 9;
  }

  function queueWreckDecal(tank) {
    if (wreckCount >= MAX_WRECKS) return;
    const at = wreckCount++;
    wreckWaits[at] = WRECK_DELAY;
    wreckPoses[at * 3] = tank.x; wreckPoses[at * 3 + 1] = tank.z;
    wreckPoses[at * 3 + 2] = tank.yaw;
  }

  function updateDecals(dt) {
    for (let at = 0; at < wreckCount; at++) wreckWaits[at] -= dt;
    // Oldest first, at most one scorch a frame.
    if (wreckCount && wreckWaits[0] <= 0) {
      spawnDecal(2, wreckPoses[0], wreckPoses[1], wreckPoses[2], 1.3, 1.6);
      wreckCount--;
      wreckWaits.copyWithin(0, 1, wreckCount + 1);
      wreckPoses.copyWithin(0, 3, (wreckCount + 1) * 3);
    }
    for (let at = 0; at < MAX_DECALS; at++) {
      const decal = decals[at];
      if (!decal.active) continue;
      decal.life -= dt;
      if (decal.life <= 0) decal.active = false;
    }
  }
  function activeMaskIndex(mask) {
    return 31 - Math.clz32((mask & -mask) >>> 0);
  }
  function activeMaskCount(mask) {
    let count = 0;
    for (mask >>>= 0; mask; mask = (mask & (mask - 1)) >>> 0) count++;
    return count;
  }
  const pickups = Array.from({length: MAX_PICKUPS}, () => ({
    active: false, x: 0, z: 0, type: "", phase: 0,
  }));
  const mines = Array.from({length: MAX_MINES}, () => ({
    active: false, team: 0, owner: 0, x: 0, z: 0, arm: 0, life: 0, level: 0,
  }));
  const smokeClouds = Array.from({length: MAX_SMOKE}, () => ({
    active: false, x: 0, z: 0, life: 0,
  }));
  let activeSmokeCount = 0;
  let bulletSpawnCursor = 0, particleSpawnCursor = 0;
  const DIRECTION_COUNT = 512, PARTICLE_DIRECTION_COUNT = 32;
  const directionSines = new Float32Array(DIRECTION_COUNT);
  const directionCosines = new Float32Array(DIRECTION_COUNT);
  for (let direction = 0; direction < DIRECTION_COUNT; direction++) {
    const angle = direction * Math.PI * 2 / DIRECTION_COUNT;
    directionSines[direction] = Math.sin(angle);
    directionCosines[direction] = Math.cos(angle);
  }
  const PARTICLE_TEMPLATE_COUNT = 64;
  const particleTemplateDirection = new Uint16Array(PARTICLE_TEMPLATE_COUNT);
  const particleTemplateSpeed = new Float32Array(PARTICLE_TEMPLATE_COUNT);
  const particleTemplateVertical = new Float32Array(PARTICLE_TEMPLATE_COUNT);
  const particleTemplateLife = new Float32Array(PARTICLE_TEMPLATE_COUNT);
  let particleTemplateCursor = 0, particleTemplateSeed = 0x31d6a28b;
  function nextParticleTemplateUnit() {
    particleTemplateSeed = xorshift32(particleTemplateSeed);
    return (particleTemplateSeed >>> 0) / 4294967296;
  }
  for (let at = 0; at < PARTICLE_TEMPLATE_COUNT; at++) {
    particleTemplateDirection[at] =
      ((nextParticleTemplateUnit() * PARTICLE_DIRECTION_COUNT) | 0)
      * (DIRECTION_COUNT / PARTICLE_DIRECTION_COUNT);
    particleTemplateSpeed[at] = .35 + nextParticleTemplateUnit();
    particleTemplateVertical[at] = .5 + nextParticleTemplateUnit() * 1.8;
    particleTemplateLife[at] = .35 + nextParticleTemplateUnit() * .45;
  }
  function orientationIndex(angle) {
    const at = Math.round(angle * DIRECTION_COUNT / (Math.PI * 2));
    // Hull and turret headings are wrapped. Avoid two soft-float remainder
    // calls on their common range; offset angles (shell spread) may not be.
    if (at >= 0 && at < DIRECTION_COUNT) return at;
    if (at < 0 && at >= -DIRECTION_COUNT) return at + DIRECTION_COUNT;
    const wrapped = at % DIRECTION_COUNT;
    return wrapped < 0 ? wrapped + DIRECTION_COUNT : wrapped;
  }
  function updateTankYawCache(tank) {
    const at = orientationIndex(tank.yaw);
    tank.yawSine = directionSines[at];
    tank.yawCosine = directionCosines[at];
  }
  function updateTankTurretCache(tank) {
    const at = orientationIndex(tank.turret);
    tank.turretSine = directionSines[at];
    tank.turretCosine = directionCosines[at];
  }
  const barriers = Array.from({length: MAX_BARRIERS}, () => ({
    present: false, active: false, x: 0, z: 0,
    width: 0, depth: 0, health: 0,
    left: 0, right: 0, top: 0, bottom: 0,
  }));
  const hazards = Array.from({length: MAX_HAZARDS}, () => ({active: false, type: 0,
    x: 0, z: 0, radius: 1, life: 0, renderY: 0, renderDiameter: 2, level: 0}));
  // One bit per fixed hazard slot; empty cells avoid every terrain record read.
  const hazardGrid = new Uint8Array(COLLISION_GRID_CELLS);
  let hazardGridFallbackMask = 0;
  const crates = Array.from({length: MAX_CRATES}, () => ({active: false, x: 0, z: 0, type: 0, renderY: 0}));
  let hazardCursor = 0;
  const HAZARD_COLORS = [[.21, .47, .61], [.09, .12, .13], [.3, .22, .13]];
  const pickupTypes = ["COOLANT", "ARMOR"];
  const state = {
    mode: "title", arena: 0, score: 0, lives: 3, kills: 0, time: 0,
    wallTime: 0, wave: 1, multiplier: 1, bestWave: 0, bestScore: 0,
    dailyDay: 0, dailyBestDay: 0, dailyBest: 0, bankKills: 0, doubleBanks: 0,
    medals: 1, medalsDirty: false, mainShots: 0,
    duelTurn: 0, duelWinner: -1, duelTime: 12, duelResolving: false,
    killBeat: 0, pendingClear: false, heartbeatAt: 0,
    transition: 0, shake: 0, frames: 0, running: true,
    gameMode: 0, classChoice: 1, gadgetChoice: 0, difficultyChoice: 1,
    blueControl: 0, redControl: 0,
    gateOpen: false, ricochets: 0, barriersBroken: 0,
    shots: 0, hits: 0, damageTaken: 0, objectiveTicks: 0,
    hitConfirm: 0, damageIndicator: 0, damageAngle: 0,
    commandMeter: 0, armorZone: "FRONT", arenaSeed: 0,
  };
  const convoy = {active: false, x: -1.8, z: -4.8, health: 180, progress: 0};
  let staticIndexCount = 0, arenaPlaneMaxSpan = 0;
  let arenaCommonVertexCount = 0, arenaCommonIndexCount = 0;
  let arenaCommonPlaneMaxSpan = 0, arenaCommonReady = false;
  let arenaMaximumStaticIndexCount = 0;
  let arenaPackedVertexCount = 0;
  const arenaGeometryCache = new Array(ARENAS.length);
  let cameraYaw = 0, toastUntil = 0;
  let cameraSine = 0, cameraCosine = 1;
  let cameraSafetyUpdates = 0;
  let randomState = 0x7a2d31c5;
  let nextArenaSeed = requestedArenaSeed || 0x4d3c2b1a;
  let arenaGenerationAttempts = 0;
  let arenaGenerationFallback = false, arenaGenerationChecksum = 0;
  let generatedGeometryDirty = false;
  const ARENA_GENERATION_IDLE = 0, ARENA_GENERATION_ATTEMPTS = 1;
  const ARENA_GENERATION_SPATIAL = 2, ARENA_GENERATION_GEOMETRY = 3;
  const ARENA_GENERATION_UPLOAD = 4, ARENA_GENERATION_HOLD = 5;
  const ARENA_GENERATION_SETUP = 6, ARENA_GENERATION_PREPARE = 7;
  const ARENA_GENERATION_WARM = 8, ARENA_GENERATION_ACTIVATE = 9;
  const ARENA_GENERATION_MINIMUM_HOLD = .5;
  const forgingHeadings = ["FORGING ARENA", "FORGING ARENA.",
    "FORGING ARENA..", "FORGING ARENA..."];
  let arenaGenerationPhase = ARENA_GENERATION_IDLE;
  let arenaGenerationStartedAt = 0, arenaGenerationRunSeed = 0;
  let arenaGenerationTarget = 0, arenaGenerationProgress = -1;
  let arenaGenerationUploadGenerated = false;
  let arenaGenerationGeometryCursor = -1;
  let arenaGenerationTailVertices = 0, arenaGenerationTailIndices = 0;
  let arenaGenerationPlaneMaxSpan = 0;
  let arenaGenerationReuseSceneOnce = false;
  let onlineArenaGeometryPending = false;
  let lastBotPlayerShotAt = -99;
  /* aimGuide: 0 Off (the default everywhere), 1 Sight, 2 Full. The guide
     is an assist with a score cost (AIM_SCORE_MULTIPLIERS). */
  const preferences = {
    controls: 0, assist: ASSIST_SNAP,
    reverse: 2, aimGuide: 0,
    palette: 0, paint: 0,
    camera: 0, command: false, effects: true, shake: true,
    objectiveArrow: 0,
    /* 0 Off, 1 Menus, 2 Full. The campaign save (mu) owns Music, its
       label and its persistence; attach hands the saved value over. */
    music: 0,
  };
  /* Measurement and video runs pick the guide per run (no saved state):
     aim=off|sight|full overrides every context, tracers=off hides trails. */
  let aimEnemyPhase = 0, aimEnemyLevel = 0;
  const AIM_URL_LEVEL = ["off", "sight", "full"].indexOf(urlSwitch("aim"));
  const TRACERS_ENABLED = urlSwitch("tracers") !== "off";
  let cameraDistance = 6.4, cameraSafeDistance = 6.4;
  const online = {
    channel: null, role: "", active: false, pendingPeer: 0,
    inputSequence: 0, receivedInputSequence: 0, snapshotSequence: 0,
    receivedInputValid: false, snapshotValid: false,
    lastInputSend: 0, lastSnapshotSend: 0, pendingActions: 0,
    lastReceive: 0, code: "", localCode: "", entryMode: "join",
    disconnectReason: "", webPairingMode: "", discovered: [],
    arenaWire: -1, arenaSeed: 0, arenaChecksum: 0, arenaRejected: false,
  };

  // 5: signed treads (pivot) and the lunge bit in the flags' high byte.
  const ONLINE_PROTOCOL_VERSION = 5;
  const ONLINE_INPUT_BYTES = 12;
  const ONLINE_SNAPSHOT_HEADER_BYTES = 32;
  const ONLINE_GENERATED_ARENA = 255;
  const ONLINE_TANK_BYTES = 16;
  const ONLINE_BULLET_BYTES = 12;
  const ONLINE_BULLET_LIMIT = 16;
  const ONLINE_PEER_TIMEOUT = 15;

  function playerTank() {
    if (state.gameMode === MODE_DUEL) return tanks[state.duelTurn];
    return online.active && online.role === "guest" ? tanks[1] : tanks[0];
  }

  function loadPreferences() {
    try {
      const saved = JSON.parse(localStorage.getItem(STORAGE_KEY) || "null");
      if (!saved || typeof saved !== "object") return;
      if (Number.isInteger(saved.mode) && saved.mode >= 0
          && saved.mode < GAME_MODES.length) state.gameMode = saved.mode;
      if (Number.isInteger(saved.gadget) && saved.gadget >= 0
          && saved.gadget < GADGETS.length) state.gadgetChoice = saved.gadget;
      if (Number.isInteger(saved.classId) && saved.classId >= 0
          && saved.classId < CLASSES.length) state.classChoice = saved.classId;
      if (Number.isInteger(saved.difficulty) && saved.difficulty >= 0
          && saved.difficulty < DIFFICULTIES.length)
        state.difficultyChoice = saved.difficulty;
      preferences.effects = saved.effects !== false;
      preferences.shake = saved.shake !== false;
      preferences.camera = saved.camera === 1 ? 1 : 0;
      preferences.objectiveArrow = saved.objectiveArrow === 1
        || saved.objectiveArrow === 2 ? saved.objectiveArrow : 0;
      preferences.command = saved.command === true;
      preferences.controls = saved.controls === 1 || saved.controls === 2
        ? saved.controls : 0;
      preferences.assist = Number.isInteger(saved.assist)
        && saved.assist >= ASSIST_OFF && saved.assist <= ASSIST_LOCK
        ? saved.assist : ASSIST_SNAP;
      if (Number.isInteger(saved.bestWave) && saved.bestWave >= 0)
        state.bestWave = Math.min(999, saved.bestWave);
      if (Number.isInteger(saved.bestScore) && saved.bestScore >= 0)
        state.bestScore = Math.min(99999999, saved.bestScore);
      preferences.reverse = Number.isInteger(saved.reverse)
        && saved.reverse >= 0 && saved.reverse < 3 ? saved.reverse : 2;
      preferences.aimGuide = Number.isInteger(saved.aimGuide)
        && saved.aimGuide >= 0 && saved.aimGuide <= 2 ? saved.aimGuide : 0;
      if (Number.isInteger(saved.dailyBestDay)) state.dailyBestDay = saved.dailyBestDay;
      if (Number.isInteger(saved.dailyBest) && saved.dailyBest >= 0)
        state.dailyBest = Math.min(99999999, saved.dailyBest);
      // Medals unlock paints: one bit each, the first always earned.
      if (Number.isInteger(saved.medals))
        state.medals = (saved.medals & ((1 << PAINT_NAMES.length) - 1)) | 1;
      preferences.palette = Number.isInteger(saved.palette) && saved.palette >= 0
        && saved.palette < PALETTES.length ? saved.palette : 0;
      preferences.paint = Number.isInteger(saved.paint) && saved.paint >= 0
        && saved.paint < PAINT_NAMES.length
        && (state.medals & (1 << saved.paint)) ? saved.paint : 0;
    } catch (_) {}
  }

  /* A replay borrows its run's Quick Match loadout and the Command and
     Camera preferences. The viewer's own values wait here, under the saved
     settings' keys, until the replay ends; a save while it plays writes
     them, never the borrowed ones. */
  let replayViewerSettings = null;
  function borrowReplaySettings() {
    if (!replayViewerSettings) replayViewerSettings = {
      mode: state.gameMode, classId: state.classChoice,
      gadget: state.gadgetChoice, difficulty: state.difficultyChoice,
      command: preferences.command, camera: preferences.camera,
    };
    state.gameMode = replayConfig[1]; state.classChoice = replayConfig[2];
    state.gadgetChoice = replayConfig[3]; state.difficultyChoice = replayConfig[4];
    preferences.command = (replayConfig[5] & 1) === 1; preferences.camera = replayConfig[6];
  }
  // Every way out of a replay: its natural end, or a return to the title.
  function endReplay() {
    replayActive = false;
    const own = replayViewerSettings;
    if (!own) return;
    replayViewerSettings = null;
    state.gameMode = own.mode; state.classChoice = own.classId;
    state.gadgetChoice = own.gadget; state.difficultyChoice = own.difficulty;
    preferences.command = own.command; preferences.camera = own.camera;
    if (!preferences.camera) { cameraYaw = 0; cameraSine = 0; cameraCosine = 1; }
    refreshSetupLabels();
  }

  function savePreferences() {
    const own = replayViewerSettings;
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify({
        mode: own ? own.mode : state.gameMode,
        classId: own ? own.classId : state.classChoice,
        gadget: own ? own.gadget : state.gadgetChoice,
        difficulty: own ? own.difficulty : state.difficultyChoice,
        camera: own ? own.camera : preferences.camera,
        controls: preferences.controls, assist: preferences.assist,
        command: own ? own.command : preferences.command,
        effects: preferences.effects,
        shake: preferences.shake, objectiveArrow: preferences.objectiveArrow,
        bestWave: state.bestWave, bestScore: state.bestScore,
        reverse: preferences.reverse, aimGuide: preferences.aimGuide,
        dailyBestDay: state.dailyBestDay, dailyBest: state.dailyBest,
        medals: state.medals, palette: preferences.palette, paint: preferences.paint,
      }));
    } catch (_) {}
  }

  /* One xorshift32 step (an int32): particle templates, Daily seeds and
     Onslaught's next arena seed. random() inlines the same step, on the
     hot path; replays depend on all of these sequences. */
  function xorshift32(x) {
    x ^= x << 13; x ^= x >>> 17;
    return x ^ (x << 5);
  }

  function random() {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return (randomState >>> 0) / 4294967296;
  }

  const arenaGenerator = globalThis.__treadlineCreateArenaGenerator
    ? globalThis.__treadlineCreateArenaGenerator(ARENAS, generatedArena)
    : null;

  // A finished generation's result (Deploy's stepped one, or the harness's).
  function noteArenaGeneration(generated) {
    arenaGenerationAttempts = generated.attempts;
    arenaGenerationFallback = generated.fallback;
    arenaGenerationChecksum = generated.checksum;
    state.arenaSeed = generated.winningSeed;
  }

  function wrapAngle(angle) {
    while (angle > Math.PI) angle -= Math.PI * 2;
    while (angle < -Math.PI) angle += Math.PI * 2;
    return angle;
  }

  function approachAngle(current, target, amount) {
    const difference = wrapAngle(target - current);
    return current + (difference < -amount ? -amount
      : difference > amount ? amount : difference);
  }

  function activeEnemyCount() {
    let count = 0;
    for (let at = 1; at < MAX_TANKS; at++)
      if (tanks[at].active && tanks[at].team !== tanks[0].team) count++;
    return count;
  }

  function isOnslaught() { return state.gameMode === MODE_ONSLAUGHT || state.gameMode === MODE_DAILY; }

  function dailyDay() {
    const supplied = urlSwitch("daily");
    if (supplied !== null && /^\d{8}$/.test(supplied)) {
      const day = Number(supplied), year = (day / 10000) | 0;
      const month = ((day / 100) | 0) % 100, date = day % 100;
      const check = new Date(Date.UTC(year, month - 1, date));
      if (year >= 2000 && check.getUTCFullYear() === year
          && check.getUTCMonth() + 1 === month && check.getUTCDate() === date) return day;
    }
    const now = new Date();
    return now.getUTCFullYear() * 10000 + (now.getUTCMonth() + 1) * 100 + now.getUTCDate();
  }

  function dailySeed(day) {
    /* Versioned, integer-only mapping; independent of gameplay randomness. */
    return xorshift32(day ^ 0x54443101) >>> 0 || 1;
  }

  function showToast(text, seconds = .9) {
    hudToast = String(text || "").toUpperCase();
    hudMeshDirty = true;
    if (authoredSurfaceVisible(state.mode)) {
      ui.toast.textContent = text;
      ui.toast.classList.add("visible");
    }
    toastUntil = state.time + seconds;
  }

  function authoredSurfaceVisible(mode) {
    return mode !== "playing" && mode !== "generating"
      && mode !== "arena-clear" && mode !== "killcam" && mode !== "hidden";
  }

  function recordOnslaughtBest() {
    if (replayActive) return;
    if (state.gameMode === MODE_DAILY) {
      const score = finalScore();
      if (state.dailyBestDay !== state.dailyDay || score > state.dailyBest) {
        state.dailyBestDay = state.dailyDay; state.dailyBest = score;
        savePreferences();
      }
      return;
    }
    if (state.gameMode !== MODE_ONSLAUGHT) return;
    const wave = Math.max(1, state.wave | 0);
    const score = Math.max(0, finalScore() | 0);
    if (wave <= state.bestWave && score <= state.bestScore) return;
    state.bestWave = Math.max(state.bestWave, wave);
    state.bestScore = Math.max(state.bestScore, score);
    savePreferences();
  }

  /* The panel belongs to campaign.js's menu screens: each writes its own
     heading, message and groups (menu.onMode picks the screen for a mode
     on its next tick). The run keeps the stats line and Deploy's label.
     A menu mode reveals the panel just before that tick, so it never
     shows the previous screen's text for a frame. */
  const PLAY_LABELS = {paused: "Resume", "game-over": "Deploy again",
    victory: "Play again", "duel-pass": "Ready", "replay-done": "Deploy"};
  let panelReveal = false;
  function showRunPanel(mode) {
    const finished = mode === "game-over" || mode === "victory";
    hide(ui.stats, !finished);
    if (finished) put(ui.stats,
      `${state.hits}/${state.shots} hits · ${state.ricochets} ricochets · `
      + `${state.barriersBroken} barriers · ${state.objectiveTicks | 0}s objective`
      + (isOnslaught() && state.arenaSeed
        ? ` · seed ${state.arenaSeed}` : "")
      + (state.gameMode === MODE_DAILY ? ` · TD1-${state.dailyDay}` : "")
      + ` · ${state.bankKills} bank kills`
      + ` · paints ${PAINT_NAMES.filter((_, at) => state.medals & (1 << at)).join("/")}`);
    put(ui.play, PLAY_LABELS[mode]);
    panelReveal = true;
  }

  function setMode(mode) {
    if (mode === "paused" || mode === "duel-pass" || mode === "hidden")
      for (let at = 0; at < MAX_TANKS; at++) {
        tanks[at].fireHeld = false; tanks[at].fireCharge = 0;
      }
    if (mode === "duel-pass" || mode === "paused" || mode === "hidden")
      input.fireReleaseRequired = true;
    if (!replayActive && mode === "victory") {
      if (state.gameMode === MODE_CONVOY && !state.mainShots) unlockMedal(1);
      if (state.gameMode === MODE_CONTROL && state.lives === 3) unlockMedal(5);
    }
    if (!replayActive && (mode === "victory" || mode === "game-over") && state.medalsDirty) {
      savePreferences(); state.medalsDirty = false;
    }
    if (replayActive && mode === "title") endReplay();
    const authoredWasVisible = authoredSurfaceVisible(state.mode);
    if ((mode === "game-over" || mode === "victory")
        && isOnslaught()) recordOnslaughtBest();
    state.mode = mode;
    if (mode === "playing" && !qualificationAutoStart)
      globalThis.pocSummary = "TREADLINE-PLAYING";
    const authoredIsVisible = authoredSurfaceVisible(mode);
    hudMeshDirty = true;
    hudIndicatorDirty = true;
    if (mode === "playing" && hudRunStart) {
      hudRunStart = false;
      updateHud(true); updateOverlay(); publishHudNow();
    }
    /* arena-clear and hidden retain the same full-canvas presentation as
       playing. Crossing among those states must not rewrite the concealed
       HTML shell: doing so forces a full page relayout in the middle of an
       otherwise bounded gameplay frame. */
    if (!authoredWasVisible && !authoredIsVisible) return;
    const canvasMode = mode === "playing" || mode === "generating" || mode === "killcam";
    ui.shell.classList.toggle("game-running", canvasMode);
    const gameplayHud = [ui.hud, ui.gadget, ui.commandStatus, ui.toast];
    for (const element of gameplayHud)
      if (element) element.hidden = canvasMode;
    if (mode === "playing") {
      panelReveal = false;
      ui.panel.hidden = true;
    }
    else if (mode === "generating") {
      panelReveal = false;
      ui.panel.hidden = true;
      hudScore = hudArmor = hudGadget = hudCommand = "";
      hudArena = `RUN SEED ${arenaGenerationRunSeed}`;
      hudToast = forgingHeadings[0];
      hudMeshDirty = hudIndicatorDirty = true;
      publishHudNow();
    }
    else if (PLAY_LABELS[mode]) showRunPanel(mode);
    if (authoredIsVisible) updateHud(true);
  }

  /* Menu switches refresh these labels; an unchanged write is still a
     DOM mutation the browser relays out. */
  function put(el, text) { if (el && el.textContent !== text) el.textContent = text; }
  function off(el, on) { if (el && el.disabled !== on) el.disabled = on; }
  function hide(el, on) { if (el && el.hidden !== on) el.hidden = on; }
  function refreshSetupLabels() {
    put(ui.modeValue, GAME_MODES[state.gameMode]);
    put(ui.classValue, CLASSES[state.classChoice].name);
    put(ui.gadgetValue, GADGETS[state.gadgetChoice]);
    put(ui.difficultyValue, DIFFICULTIES[state.difficultyChoice]);
    put(ui.cameraValue, CAMERA_MODES[preferences.camera]);
    put(ui.controlsValue, controls.names[preferences.controls]);
    put(ui.assistValue, AIM_ASSISTS[preferences.assist]);
    put(ui.aimValue, AIM_LEVEL_NAMES[aimGuideLevel()]);
    put(ui.commandValue, preferences.command ? "On" : "Off");
    put(ui.effectsValue, preferences.effects ? "On" : "Off");
    put(ui.shakeValue, preferences.shake ? "On" : "Off");
    put(ui.objectiveValue, OBJECTIVE_ARROW_NAMES[preferences.objectiveArrow]);
    put(ui.reverseValue, `${REVERSE_DEGREES[preferences.reverse]} deg`);
    put(ui.paletteValue, PALETTES[preferences.palette]);
    put(ui.paintValue, PAINT_NAMES[preferences.paint]);
    off(ui.replayLast, !replayCount || replayRecording);
    off(ui.replayShare, !replayCount || replayRecording);
    const daily = state.gameMode === MODE_DAILY;
    if (daily && state.mode === "title") state.dailyDay = dailyDay();
    for (const button of [ui.classChoice, ui.gadgetChoice, ui.difficultyChoice])
      off(button, daily);
    if (daily) {
      const seed = dailySeed(state.dailyDay);
      put(ui.classValue, CLASSES[seed % 3].name);
      put(ui.gadgetValue, GADGETS[(seed >>> 8) % 5]);
      put(ui.difficultyValue, DIFFICULTIES[DAILY_DIFFICULTY]);
    }
    // Quick Match's message names the mode (campaign.js's quick screen).
    if (state.mode === "title") updateHud(true);
  }

  /* The tint is a live uniform. The clear colour follows it (see
     clearToBackdrop); a new colour re-captures the retained frame command
     list once, so this runs only when the look changes: arena start, the
     palette setting, a campaign theater, a Low Battery dim step. An
     unchanged colour is not a change. */
  function applyPalette() {
    const tint = PALETTE_TINTS[preferences.palette];
    gl.useProgram(program);
    gl.uniform4f(sceneryTintLocation, tint[0], tint[1], tint[2], 1);
    clearToBackdrop(tint);
  }

  function unlockMedal(index) {
    const bit = 1 << index;
    if (replayActive || (state.medals & bit)) return;
    state.medals |= bit; state.medalsDirty = true;
    showToast(`MEDAL ${PAINT_NAMES[index]}`, 1.4);
  }
  /* The Multiplayer screen's live group follows the online flow:
     "actions" (host, LAN, code), "code" or "response" (the keypad, joining
     or answering a host), "status" or "web" (manual pairing). campaign.js's
     screen shows the panel's groups and heading; it names the heading
     after this view (bridge onlineView). */
  let onlineView = "actions";
  function showOnlineView(view) {
    onlineView = view;
    hide(ui.onlineActions, view !== "actions");
    hide(ui.codeEntry, view !== "code" && view !== "response");
    hide(ui.onlineStatus, view !== "status");
    hide(ui.webPairing, view !== "web");
    ui.panel.classList.toggle("web-pairing-panel", view === "web");
    hide(ui.panel, false);
    campaign.showMenu("multiplayer");
  }

  function showOnlineSurface(message) {
    showOnlineView("status");
    ui.onlineMessage.textContent = message;
    ui.onlineInvite.textContent =
      !webPairing && online.role === "host" && online.localCode
        ? `CODE ${formatInviteCode(online.localCode)}` : "";
    ui.onlineResponse.hidden = webPairing
      || online.role !== "host" || !online.localCode;
    ui.onlinePeerActions.hidden = true;
  }

  // Cancel and Back leave online play for the Multiplayer screen.
  function leaveOnline() {
    if (online.channel) online.channel.close(1000, "cancelled");
    online.channel = null; online.active = false;
    if (state.mode !== "title") setMode("title");
    showOnlineView("actions");
  }

  function showWebPairing(mode) {
    online.webPairingMode = mode;
    showOnlineView("web");
    const host = mode === "host";
    const guestResponse = mode === "guest-response";
    const connecting = mode === "connecting";
    const guestOffer = !host && !guestResponse && !connecting;
    ui.webStep.textContent = host
      ? "STEP 1 · SEND OFFER / STEP 2 · PASTE RESPONSE"
      : guestResponse ? "STEP 2 · SEND RESPONSE"
        : connecting ? "CONNECTING" : "STEP 1 · PASTE OFFER";
    ui.webHelp.textContent = host
      ? "Send the offer privately. Paste the response from the guest below."
      : guestResponse ? "Send this response back to the host. Keep this page open."
        : connecting ? "Keep both game pages open while they connect directly."
          : "Paste the offer sent by the host, then create a response.";
    ui.webStatus.textContent = host ? "Preparing offer…"
      : guestResponse ? "Waiting for the host to connect…"
        : connecting ? "Connecting directly…" : "Ready for the host offer.";
    ui.webShareLabel.hidden = !host && !guestResponse;
    ui.webShare.hidden = !host && !guestResponse;
    ui.webShareSize.hidden = !host && !guestResponse;
    ui.webCopy.hidden = !host && !guestResponse;
    ui.webInputLabel.hidden = guestResponse || connecting;
    ui.webInputLabel.textContent = host
      ? "Paste the guest response" : "Paste the host offer";
    ui.webInput.hidden = guestResponse || connecting;
    if (guestOffer) ui.webInput.value = "";
    ui.webApply.hidden = guestResponse || connecting;
    ui.webApply.disabled = connecting;
    ui.webApply.textContent = host ? "Connect" : "Create response";
    ui.webShareLabel.textContent = host
      ? "Send this offer to the guest" : "Send this response to the host";
    ui.webCopy.textContent = host ? "Copy offer" : "Copy response";
    const focus = connecting ? ui.webCancel : guestResponse ? ui.webCopy : ui.webInput;
    if (focus) focus.focus();
  }

  function formatInviteCode(code) {
    code = String(code || "").replace(/\D/g, "");
    const groups = [];
    for (let at = 0; at < code.length; at += 4)
      groups.push(code.slice(at, at + 4));
    return groups.join(" ");
  }

  function onlineFail(message) {
    online.active = false;
    if (webPairing) showOnlineSurface(String(message));
    ui.onlineMessage.textContent = String(message);
    ui.onlineInvite.textContent = "";
    ui.onlinePeerActions.hidden = true;
  }

  function beginOnlineMatch() {
    online.active = true;
    online.inputSequence = online.receivedInputSequence = 0;
    online.snapshotSequence = 0;
    online.receivedInputValid = online.snapshotValid = false;
    online.arenaWire = -1; online.arenaSeed = online.arenaChecksum = 0;
    online.arenaRejected = false;
    online.disconnectReason = "";
    online.pendingActions = 0;
    online.lastInputSend = online.lastSnapshotSend = state.time;
    online.lastReceive = state.time;
    state.gameMode = MODE_CONTROL;
    tanks[0].player = online.role !== "guest";
    tanks[1].player = online.role === "guest";
    resetGame();
    setMode("playing");
    showToast(online.role === "host" ? "PLAYER CONNECTED" : "JOINED HOST", 1.2);
  }

  function configureOnlineChannel(channel, role) {
    online.channel = channel;
    online.role = role;
    online.pendingPeer = 0;
    online.discovered.length = 0;
    channel.onstatus = event => {
      const detail = event.detail || "Connecting…";
      if (webPairing) {
        if (event.phase === "retry-answer") showWebPairing("host");
        else if (event.phase === "connecting") showWebPairing("connecting");
        ui.webStatus.textContent = detail;
      } else ui.onlineMessage.textContent = detail;
    };
    channel.oninvitecode = event => {
      online.code = String(event.code || "");
      online.localCode = online.code;
      if (webPairing) {
        showWebPairing(role === "host" ? "host" : "guest-response");
        ui.webShare.value = online.code;
        ui.webShareSize.textContent = `${online.code.length} characters`;
        ui.webStatus.textContent = event.detail || "Pairing text ready.";
      } else ui.onlineInvite.textContent = `CODE ${formatInviteCode(online.code)}`;
      ui.onlineResponse.hidden = role !== "host";
      if (ui.onlineMessage && !webPairing)
        ui.onlineMessage.textContent = role === "host"
          ? "Share this code, or wait for a LAN player."
          : "If asked, share this response code with the host.";
    };
    channel.ondiscovered = event => {
      if (online.discovered.length >= 6
          || online.discovered.some(item => item.peerIdentity === event.peerIdentity))
        return;
      online.discovered.push({
        peerIdentity: event.peerIdentity, code: event.code, name: event.name,
      });
      online.pendingPeer = event.peerIdentity;
      online.code = event.code;
      ui.onlineMessage.textContent = `Found ${event.name || "LAN game"}`;
      ui.onlineAccept.textContent = "Join";
      ui.onlineReject.textContent = "Ignore";
      ui.onlinePeerActions.hidden = false;
    };
    channel.onpeerrequest = event => {
      online.pendingPeer = event.peerIdentity;
      ui.onlineMessage.textContent = `${event.name || "Player"} wants to join.`;
      ui.onlineAccept.textContent = "Accept";
      ui.onlineReject.textContent = "Reject";
      ui.onlinePeerActions.hidden = false;
    };
    channel.onopen = () => beginOnlineMatch();
    channel.onmessage = event => {
      if (!(event.data instanceof ArrayBuffer)) return;
      const bytes = new Uint8Array(event.data);
      if ((bytes[0] === 1 || bytes[0] === 2)
          && bytes[1] !== ONLINE_PROTOCOL_VERSION) {
        online.disconnectReason = "The other player runs a different Treadline version.";
        onlineFail(online.disconnectReason);
        try { channel.close(4001, "version"); } catch (_) {}
      } else if (bytes[0] === 1 && online.role === "host") applyOnlineInput(event.data);
      else if (bytes[0] === 2 && online.role === "guest")
        applyOnlineSnapshot(event.data);
    };
    channel.onerror = event => onlineFail(event.detail || "Could not connect.");
    channel.onclose = event => {
      online.active = false;
      online.channel = null;
      tanks[0].player = true; tanks[1].player = false;
      if (state.mode === "playing") {
        setMode("title");
        showOnlineSurface(online.disconnectReason
          || (event.wasClean ? "Player left." : "Connection lost."));
      }
      online.disconnectReason = "";
    };
  }

  function startOnline(role, code = "") {
    if (!navigator.tilefinchMultiplayer) {
      showOnlineSurface("Install this game in Tilefinch to play online.");
      return;
    }
    online.localCode = "";
    if (webPairing && role === "host" && ui.webInput) ui.webInput.value = "";
    showOnlineSurface(role === "host" ? "Opening game…"
      : role === "discover" ? "Looking on this Wi-Fi…" : "Connecting…");
    try {
      const options = {
        gameId: "treadline-arena-v2",
        name: webPairing ? "Web player" : "PSP Player",
      };
      if (webPairing && /^(localhost|127(?:\.\d+){3}|\[?::1\]?)$/i.test(
        location.hostname)) options.iceServers = [];
      const channel = role === "host"
        ? navigator.tilefinchMultiplayer.host(options)
        : role === "discover"
          ? navigator.tilefinchMultiplayer.discover(options)
          : navigator.tilefinchMultiplayer.join(code, options);
      configureOnlineChannel(channel, role);
    } catch (error) {
      onlineFail(error && error.message ? error.message : "Multiplayer unavailable.");
    }
  }

  ui.onlineHost.addEventListener("click", () => startOnline("host"));
  ui.onlineLan.addEventListener("click", () => startOnline("discover"));
  ui.onlineCode.addEventListener("click", () => {
    if (webPairing) {
      online.entryMode = "web-offer";
      showWebPairing("guest-offer");
      return;
    }
    showOnlineView("code");
    online.code = ""; online.entryMode = "join";
    ui.codeValue.textContent = "—";
    const first = ui.codeKeypad && ui.codeKeypad.querySelector("button");
    if (first) first.focus();
  });
  ui.codeKeypad.addEventListener("click", event => {
    const button = event.target.closest("button");
    if (!button) return;
    const key = button.dataset.key || button.textContent.trim();
    if (key === "delete") online.code = online.code.slice(0, -1);
    else if (key === "join") {
      if (online.code.length !== 12 && online.code.length !== 17) {
        ui.codeValue.textContent = "12 OR 17 DIGITS";
        return;
      }
      if (online.entryMode === "response" && online.channel) {
        const added = online.channel.addRemoteCode(online.code);
        showOnlineSurface(added ? "Punching through NAT…" : "Code was not accepted.");
      } else startOnline("guest", online.code);
      return;
    } else if (/^\d$/.test(key) && online.code.length < 17) online.code += key;
    ui.codeValue.textContent = online.code ? formatInviteCode(online.code) : "—";
  });
  ui.onlineResponse.addEventListener("click", () => {
    if (!online.channel || online.role !== "host") return;
    showOnlineView("response");
    online.code = ""; online.entryMode = "response";
    ui.codeValue.textContent = "—";
    const first = ui.codeKeypad && ui.codeKeypad.querySelector("button");
    if (first) first.focus();
  });
  ui.webCopy.addEventListener("click", async () => {
    const text = ui.webShare ? ui.webShare.value : "";
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      ui.webStatus.textContent = online.role === "host"
        ? "Offer copied. Paste the guest response below when it arrives."
        : "Response copied. Send it to the host and keep this page open.";
      showToast("PAIRING TEXT COPIED", 1.2);
    } catch (_) {
      ui.webShare.focus();
      ui.webShare.select();
      showToast("COPY THE SELECTED TEXT", 1.5);
    }
  });
  ui.webApply.addEventListener("click", () => {
    const text = ui.webInput ? ui.webInput.value.trim() : "";
    if (!text) {
      ui.webInput.focus();
      return;
    }
    if (online.role === "host" && online.channel) {
      const accepted = online.channel.addRemoteCode(text);
      if (accepted) {
        showWebPairing("connecting");
        ui.webStatus.textContent = "Checking the guest response…";
      }
      else showToast("RESPONSE WAS NOT ACCEPTED", 1.5);
    } else startOnline("guest", text);
  });
  ui.webCancel.addEventListener("click", leaveOnline);
  ui.codeCancel.addEventListener("click", () => {
    if (online.entryMode === "response" && online.channel)
      showOnlineSurface("Waiting for a player…");
    else showOnlineView("actions");
  });
  ui.onlineCancel.addEventListener("click", leaveOnline);
  ui.onlineAccept.addEventListener("click", () => {
    if (!online.channel) return;
    if (online.role === "host") online.channel.accept(online.pendingPeer, true);
    else if (online.role === "discover") {
      online.role = "guest";
      online.channel.addRemoteCode(online.code);
    }
    ui.onlinePeerActions.hidden = true;
    ui.onlineMessage.textContent = "Connecting…";
  });
  ui.onlineReject.addEventListener("click", () => {
    if (online.channel && online.role === "host")
      online.channel.accept(online.pendingPeer, false);
    ui.onlinePeerActions.hidden = true;
    ui.onlineMessage.textContent =
      online.role === "host" ? "Waiting for a player…" : "Looking on this Wi-Fi…";
  });

  class SoundBank {
    constructor() {
      this.context = null; this.voices = []; this.next = 0;
      this.engine = null; this.engineStep = -1; this.engineRole = "off";
      this.noise = null;
      // 1: prebuilt gain curves; 0: target envelopes (tests compare both).
      this.curveMode = 1; this.curves = new Map();
      /* Every effect's [frequency, duration, volume, end frequency, second
         frequency, second delay], once: the effect methods play these and
         their gain curves are prepared below, never rebuilt in play. A
         play() with other numbers keeps the target-envelope fallback;
         pitch is always ticked. */
      this.effects = {
        playerShot: [185, .07, .13, 142], botShot: [138, .07, .055, 112],
        hit: [610, .055, .06, 760], kill: [520, .24, .13, 560, 760, .11],
        damage: [104, .11, .11, 72], ricochet: [920, .055, .05, 1180],
        heartbeat: [68, .13, .075, 54], waveStart: [330, .22, .11, 520],
        waveClear: [560, .28, .13, 620, 820, .13], telegraph: [760, .045, .028, 920],
        pickup: [440, .11, .1, 620], gadget: [520, .13, .11, 690],
        command: [720, .18, .14, 920],
        // A Scout's or Striker's secondary (a Bulwark's canister is a blast).
        playerSecondary: [245, .09, .14, 190], botSecondary: [245, .09, .06, 190],
      };
      // The ricochet's noise burst: [duration, volume].
      this.ricochetNoise = [.045, .025];
      /* An explosion is a tone (112 Hz falling to 48, .22 s) and a noise
         burst (.18 s at .72 of its volume); its volume by cause. */
      this.blast = {crate: .08, playerCanister: .13, botCanister: .055,
        wreck: .14, barrier: .1, mine: .13};
      const presets = Object.values(this.effects);
      presets.push([0, this.ricochetNoise[0], this.ricochetNoise[1]]);
      for (const volume of Object.values(this.blast)) {
        presets.push([112,.22,volume,48],[0,.18,volume*.72,1]);
      }
      for (const p of presets) {
        const gain = new Float32Array(33);
        const release = Math.max(.018,p[1]*.35), tau = Math.max(.012,p[1]*.18);
        const peak = p[2]*(1-Math.exp(-release/.006));
        for (let at=0;at<33;at++) {
          const t=p[1]*at/32;
          gain[at]=t<release ? p[2]*(1-Math.exp(-t/.006))
            : peak*Math.exp(-(t-release)/tau);
        }
        gain[32]=0;
        this.curves.set(this.curveKey(p[0],p[1],p[2]),{gain});
      }
    }
    curveKey(frequency,duration,volume) {
      return frequency+Math.round(duration*100000)+Math.round(volume*10000000);
    }
    noiseWave() {
      const samples = 256, bytes = new Uint8Array(44 + samples);
      const view = new DataView(bytes.buffer);
      const word = (at, text) => {
        for (let index = 0; index < text.length; index++)
          bytes[at + index] = text.charCodeAt(index);
      };
      word(0, "RIFF"); view.setUint32(4, 36 + samples, true);
      word(8, "WAVE"); word(12, "fmt ");
      view.setUint32(16, 16, true); view.setUint16(20, 1, true);
      view.setUint16(22, 1, true); view.setUint32(24, 8000, true);
      view.setUint32(28, 8000, true); view.setUint16(32, 1, true);
      view.setUint16(34, 8, true); word(36, "data");
      view.setUint32(40, samples, true);
      let seed = 0x4f1bbcdc, prior = 0;
      for (let at = 0; at < samples; at++) {
        seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
        const value = ((seed >>> 24) - 128) | 0;
        bytes[44 + at] = Math.max(0, Math.min(255,
          128 + ((value - prior) * .62 | 0)));
        prior = value;
      }
      return bytes.buffer;
    }
    start() {
      if (this.context || typeof AudioContext !== "function") return;
      try {
        this.context = new AudioContext();
        this.context.resume().then(() => {
          for (let at = 0; at < 2; at++) {
            const gain = this.context.createGain();
            const oscillator = this.context.createOscillator();
            gain.gain.value = 0;
            oscillator.type = at === 0 ? "square" : "triangle";
            oscillator.connect(gain).connect(this.context.destination);
            oscillator.start();
            // AudioParams are stable objects on this game-owned graph. Retain
            // them once; keep methods and scheduling clocks live at each call.
            this.voices.push({oscillator, gain, gainParameter: gain.gain,
              frequencyParameter: oscillator.frequency, startAt: 0, stopAt: 0,
              attack: .012, volume: 0, startFrequency: 0, endFrequency: 0,
              secondFrequency: 0, secondAt: 0, scheduledEnvelope: 0,
              lastFrequency: 0});
          }
          const gain = this.context.createGain();
          const oscillator = this.context.createOscillator();
          oscillator.type = "triangle"; oscillator.frequency.value = 82;
          gain.gain.value = 0;
          oscillator.connect(gain).connect(this.context.destination);
          oscillator.start();
          this.engine = {oscillator, gain, gainParameter: gain.gain,
            frequencyParameter: oscillator.frequency, frequency: 82, level: 0};
          this.engineStep = -1;
          this.tick(state.wallTime, playerTank(), state.mode);
          return this.context.decodeAudioData(this.noiseWave());
        }).then((buffer) => {
          if (!buffer || !this.context) return;
          const gain = this.context.createGain();
          const source = this.context.createBufferSource();
          source.buffer = buffer; source.loop = true;
          gain.gain.value = 0;
          source.connect(gain).connect(this.context.destination);
          source.start();
          this.noise = {source, gain, gainParameter: gain.gain, startAt: 0, stopAt: 0,
            volume: 0, scheduledEnvelope: 0};
        }).catch(() => {});
      } catch (_) { this.context = null; }
    }
    /* Returns how the gain is driven: 0 ticked by tickVoice, 1 by
       setTargetAtTime events, 2 by a value curve that ends at zero and holds
       it. The curve owns the AudioParam until it ends on the audio clock,
       which lags the wall clock that expires the sound; a value write before
       then overlaps it (NotSupportedError in browsers), so expiry leaves a
       curve alone. */
    targetEnvelope(parameter, volume, duration, curve = null) {
      if (typeof parameter.setTargetAtTime !== "function"
          || !this.context) {
        parameter.value = 0;
        return 0;
      }
      const now = this.context.currentTime;
      const soundStarted = qualificationAIActive ? performance.now() : 0;
      let partStarted = soundStarted;
      parameter.cancelScheduledValues(now);
      if (qualificationAIActive) {
        const ended = performance.now(), elapsed = ended - partStarted;
        qualificationSoundTimes[2] += elapsed;
        qualificationSoundTimes[3] = Math.max(qualificationSoundTimes[3], elapsed);
        partStarted = ended;
      }
      if ((this.curveMode & 1) && curve
          && typeof parameter.setValueCurveAtTime === "function") {
        parameter.setValueCurveAtTime(curve,now,duration);
        if (qualificationAIActive) {
          const elapsed=performance.now()-soundStarted;
          qualificationSoundTimes[0]+=elapsed;
          qualificationSoundTimes[1]=Math.max(qualificationSoundTimes[1],elapsed);
          qualificationAITimes[25]+=elapsed; qualificationAITimes[30]++;
        }
        return 2;
      }
      parameter.setValueAtTime(0, now);
      if (qualificationAIActive) {
        const ended = performance.now(), elapsed = ended - partStarted;
        qualificationSoundTimes[4] += elapsed;
        qualificationSoundTimes[5] = Math.max(qualificationSoundTimes[5], elapsed);
        partStarted = ended;
      }
      parameter.setTargetAtTime(volume, now, .006);
      if (qualificationAIActive) {
        const ended = performance.now(), elapsed = ended - partStarted;
        qualificationSoundTimes[6] += elapsed;
        qualificationSoundTimes[7] = Math.max(qualificationSoundTimes[7], elapsed);
        partStarted = ended;
      }
      parameter.setTargetAtTime(0, now + Math.max(.018, duration * .35),
        Math.max(.012, duration * .18));
      if (qualificationAIActive) {
        const ended = performance.now(), releaseElapsed = ended - partStarted;
        qualificationSoundTimes[8] += releaseElapsed;
        qualificationSoundTimes[9] = Math.max(qualificationSoundTimes[9], releaseElapsed);
        const elapsed = ended - soundStarted;
        qualificationSoundTimes[0] += elapsed;
        qualificationSoundTimes[1] = Math.max(qualificationSoundTimes[1], elapsed);
        qualificationAITimes[25] += elapsed; qualificationAITimes[30]++;
      }
      return 1;
    }
    play(frequency, duration = .05, volume = .1,
         endFrequency = frequency, secondFrequency = 0,
         secondDelay = duration * .5) {
      if (!preferences.effects) return;
      if (!this.voices.length) return;
      const voice = this.music ? this.music.sfxVoice(this.voices, this.next++)
        : this.voices[this.next++ % this.voices.length];
      const curve=this.curves.get(this.curveKey(frequency,duration,volume));
      voice.frequencyParameter.value=frequency;
      voice.lastFrequency = frequency;
      voice.startAt = state.wallTime;
      voice.stopAt = state.wallTime + duration;
      voice.volume = volume;
      voice.startFrequency = frequency;
      voice.endFrequency = endFrequency;
      voice.secondFrequency = secondFrequency;
      voice.secondAt = secondDelay;
      voice.scheduledEnvelope = this.targetEnvelope(
        voice.gainParameter, volume, duration, curve?.gain);
    }
    gateNoise(duration, volume) {
      if (!preferences.effects || !this.noise) return;
      this.noise.startAt = state.wallTime;
      this.noise.stopAt = state.wallTime + duration;
      this.noise.volume = volume;
      this.noise.scheduledEnvelope = this.targetEnvelope(
        this.noise.gainParameter, volume, duration,
        this.curves.get(this.curveKey(0,duration,volume))?.gain);
    }
    // One of this.effects (missing trailing fields take play's defaults).
    effect(p) { this.play(p[0], p[1], p[2], p[3], p[4], p[5]); }
    shot(player) {
      this.effect(player ? this.effects.playerShot : this.effects.botShot);
    }
    secondary(player) {
      this.effect(player ? this.effects.playerSecondary : this.effects.botSecondary);
    }
    hit() { this.effect(this.effects.hit); }
    kill() { this.effect(this.effects.kill); }
    damage() { this.effect(this.effects.damage); }
    ricochet() {
      this.effect(this.effects.ricochet);
      this.gateNoise(this.ricochetNoise[0], this.ricochetNoise[1]);
    }
    explosion(volume = this.blast.wreck) {
      this.play(112, .22, volume, 48);
      this.gateNoise(.18, volume * .72);
    }
    heartbeat() { if (!this.music?.heartbeat()) this.effect(this.effects.heartbeat); }
    waveStart() { this.effect(this.effects.waveStart); }
    waveClear() { this.effect(this.effects.waveClear); }
    telegraph() { this.effect(this.effects.telegraph); }
    pickup() { this.effect(this.effects.pickup); }
    gadget() { this.effect(this.effects.gadget); }
    command() { this.effect(this.effects.command); }
    tickVoice(voice, time) {
      if (voice.stopAt <= 0) return;
      const elapsed = time - voice.startAt;
      const duration = voice.stopAt - voice.startAt;
      if (elapsed >= duration) {
        if (voice.scheduledEnvelope !== 2) voice.gainParameter.value = 0;
        voice.stopAt = 0;
        return;
      }
      if (!voice.scheduledEnvelope) {
        const attack = Math.min(voice.attack, duration * .25);
        const level = elapsed < attack ? Math.max(0, elapsed / attack)
          : Math.max(0, 1 - (elapsed - attack)
            / Math.max(.001, duration - attack));
        voice.gainParameter.value = voice.volume * level;
      }
      let frequency;
      if (voice.secondFrequency && elapsed >= voice.secondAt) {
        const phase = Math.min(1, (elapsed - voice.secondAt)
          / Math.max(.001, duration - voice.secondAt));
        frequency = voice.secondFrequency
          + (voice.endFrequency - voice.secondFrequency) * phase;
      } else {
        const phase = Math.min(1, elapsed / Math.max(.001, duration));
        frequency = voice.startFrequency
          + (voice.endFrequency - voice.startFrequency) * phase;
      }
      if (frequency !== voice.lastFrequency) {
        voice.frequencyParameter.value = frequency;
        voice.lastFrequency = frequency;
      }
    }
    setMusic(enabled) {
      this.engineStep = -1;
      this.tick(state.wallTime, playerTank(), state.mode);
    }
    tick(time, player, mode) {
      for (let at = 0; at < this.voices.length; at++) {
        const voice = this.voices[at];
        this.tickVoice(voice, time);
      }
      if (this.noise && this.noise.stopAt > 0) {
        const elapsed = time - this.noise.startAt;
        const duration = this.noise.stopAt - this.noise.startAt;
        if (elapsed >= duration) {
          if (this.noise.scheduledEnvelope !== 2) this.noise.gainParameter.value = 0;
          this.noise.stopAt = 0;
        } else if (!this.noise.scheduledEnvelope) {
          const attack = Math.min(.008, duration * .2);
          const level = elapsed < attack ? Math.max(0, elapsed / attack)
            : Math.max(0, 1 - (elapsed - attack)
              / Math.max(.001, duration - attack));
          this.noise.gainParameter.value = this.noise.volume * level;
        }
      }
      if (this.music?.tick(time, player, mode)) return;
      const active = mode === "playing" && player?.active;
      this.engineRole = active ? "hum" : "off";
      const step = (time * 10) | 0;
      if (step === this.engineStep || !this.engine) return;
      this.engineStep = step;
      const moving = active
        ? Math.min(1, (Math.abs(player.command.left)
          + Math.abs(player.command.right)) * .5) : 0;
      const boosted = active && player.boost > 0;
      let frequency = this.engine.frequency, level = 0;
      if (active) {
        frequency = 64 + moving * 34
          + (boosted ? 25 : 0);
        level = preferences.effects
          ? .009 + moving * .012 + (boosted ? .006 : 0) : 0;
      }
      // This private continuous voice has no scheduled automation. Repeating
      // an unchanged value only republishes the same native voice state.
      if (frequency !== this.engine.frequency) {
        this.engine.frequencyParameter.value = frequency;
        this.engine.frequency = frequency;
      }
      if (level !== this.engine.level) {
        this.engine.gainParameter.value = level;
        this.engine.level = level;
      }
    }
  }
  const sounds = new SoundBank();
  // music.js: timing runs keep the effects-only baseline unless music=on.
  if (!qualificationAutoStart || urlSwitch("music") === "on")
    sounds.music = globalThis.__treadlineMusic.create(sounds,
      {state, tanks, preferences, playerTank, campaign, practice});

  function requestPresentation() {
    /* The long soak keeps its audio-free workload although its Deploy is now
       a trusted activation; audio-inclusive timing is the input workloads'. */
    if (!qualificationLongSoak) sounds.start();
    const shell = document.getElementById("game-shell");
    if (!controls.claim(shell) && shell.requestFullscreen)
      shell.requestFullscreen().catch(() => {});
    canvas.focus();
  }

  const WARM_TONE = [1, .98, .92], COOL_TONE = [.9, .95, 1.1];
  const FLAT_TONE = [1, 1, 1];
  function addStaticVertex(x, y, z, color, shade, tone) {
    const p = vertexCount * 3, c = vertexCount++ * 4;
    positions[p] = x; positions[p + 1] = y; positions[p + 2] = z;
    for (let at = 0; at < 3; at++)
      colors[c + at] = Math.min(1, color[at] * shade * tone[at]);
    colors[c + 3] = 1;
  }

  function addStaticTriangle(ax, ay, az, bx, by, bz, cx, cy, cz,
                             color, shade = 1) {
    if (vertexCount + 3 > MAX_VERTICES || indexCount + 3 > MAX_INDICES) {
      meshDrops++;
      return false;
    }
    arenaPlaneMaxSpan = Math.max(arenaPlaneMaxSpan,
      Math.max(ax, bx, cx) - Math.min(ax, bx, cx),
      Math.max(az, bz, cz) - Math.min(az, bz, cz));
    const base = vertexCount, tone = shade >= 1 ? WARM_TONE : COOL_TONE;
    addStaticVertex(ax, ay, az, color, shade, tone);
    addStaticVertex(bx, by, bz, color, shade, tone);
    addStaticVertex(cx, cy, cz, color, shade, tone);
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    return true;
  }

  function addStaticQuad(ax, ay, az, bx, by, bz,
                         cx, cy, cz, dx, dy, dz, color, shade = 1) {
    return addStaticGradientQuad(ax, ay, az, bx, by, bz, cx, cy, cz,
      dx, dy, dz, color, shade, shade, shade, shade,
      shade >= 1 ? WARM_TONE : COOL_TONE);
  }

  function addStaticGradientQuad(ax, ay, az, bx, by, bz,
                                 cx, cy, cz, dx, dy, dz,
                                 color, shadeA, shadeB, shadeC, shadeD,
                                 tone = FLAT_TONE) {
    if (vertexCount + 4 > MAX_VERTICES || indexCount + 6 > MAX_INDICES) {
      meshDrops++;
      return false;
    }
    arenaPlaneMaxSpan = Math.max(arenaPlaneMaxSpan,
      Math.max(ax, bx, cx, dx) - Math.min(ax, bx, cx, dx),
      Math.max(az, bz, cz, dz) - Math.min(az, bz, cz, dz));
    const base = vertexCount;
    addStaticVertex(ax, ay, az, color, shadeA, tone);
    addStaticVertex(bx, by, bz, color, shadeB, tone);
    addStaticVertex(cx, cy, cz, color, shadeC, tone);
    addStaticVertex(dx, dy, dz, color, shadeD, tone);
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base + 3;
    return true;
  }

  function floorVignette(x, z) {
    const edge = Math.min(1, Math.max(Math.abs(x), Math.abs(z)) / 9);
    /* A brighter lit centre that falls off toward the walls. */
    return 1.42 - edge * edge * .78;
  }

  /* Floor grid lines. A band thinner than a canvas pixel falls between
     pixel centres and drops out as the camera moves a fraction of a pixel:
     the old 0.035-wide lines were 0.15-0.8 px at 320x180, and whole lines
     blinked. Each end of a line now gets the width that keeps it about
     1.1 px thick where the camera usually sees it. The Stable camera looks
     north from 6.4 behind the player, so northern lines are seen from
     farther, and lines across the view (constant z) are foreshortened most;
     the widths are fitted to a model of that camera (with some Follow
     yaws) over the whole arena. A wider band fades toward the floor colour
     (GRID_INK over its width), so near lines keep their weight and distant
     ones read as faint rather than flickering. Same quads, built once:
     nothing per frame. */
  const GRID_INK = .07;
  const gridColor = [0, 0, 0];
  function gridWidth(z, across) {
    return across ? .098 + z * (.009 + z * .0009) : .072 + z * (.003 + z * .0004);
  }
  function addGridVertex(x, z, width) {
    const ink = Math.min(1, GRID_INK / width);
    for (let at = 0; at < 3; at++) gridColor[at] = COLORS.ground[at]
      + (COLORS.grid[at] - COLORS.ground[at]) * ink;
    // Grid lines share the floor's light pool.
    addStaticVertex(x, .014, z, gridColor, floorVignette(x, z), FLAT_TONE);
  }
  // across: the line runs along x at z = line; else along z at x = line.
  function addGridLine(across, line, first, last) {
    if (vertexCount + 4 > MAX_VERTICES || indexCount + 6 > MAX_INDICES) {
      meshDrops++;
      return;
    }
    const near = gridWidth(across ? line : first, across);
    const far = gridWidth(across ? line : last, across);
    arenaPlaneMaxSpan = Math.max(arenaPlaneMaxSpan, last - first,
      Math.max(near, far));
    const base = vertexCount;
    if (across) {
      addGridVertex(first, line + near * .5, near);
      addGridVertex(last, line + far * .5, far);
      addGridVertex(last, line - far * .5, far);
      addGridVertex(first, line - near * .5, near);
    } else {
      addGridVertex(line - far * .5, last, far);
      addGridVertex(line + far * .5, last, far);
      addGridVertex(line + near * .5, first, near);
      addGridVertex(line - near * .5, first, near);
    }
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base + 3;
  }

  /* The backdrop leans in: its foot meets the floor's outer edge, so the
     ground the camera sees over the walls is backdrop rather than the clear
     colour, and the scenery tint dims it with everything else. Segments keep
     their 4.32 span along the wall; end segments overlap at the corners, where
     both faces share the same height gradient. Each segment is cut in two at
     mid-height: the PSP GE drops a whole triangle with a corner beside the
     camera, and an uncut panel then left a clear-colour hole as tall as the
     backdrop (seen on hardware). The cut is all the translated-vertex budget
     allows; finer cuts take instance capacity from shells and effects. The
     residual upper half-panel holes clear to the backdrop's own colour
     (clearToBackdrop). */
  function addHorizonBackdrop() {
    const base = BACKDROP_BASE, high = 5.6, edge = 10.8, foot = 9;
    const step = edge * 2 / 5, sky = BACKDROP_SKY;
    for (let segment = 0; segment < 5; segment++) {
      const first = -edge + segment * step, last = first + step;
      for (let row = 0; row < 2; row++) {
        const y0 = -.05 + (high + .05) * row / 2;
        const y1 = -.05 + (high + .05) * (row + 1) / 2;
        const d0 = foot + (edge - foot) * row / 2;
        const d1 = foot + (edge - foot) * (row + 1) / 2;
        const s0 = 1 + (sky - 1) * row / 2, s1 = 1 + (sky - 1) * (row + 1) / 2;
        addStaticGradientQuad(first, y0, d0, last, y0, d0,
          last, y1, d1, first, y1, d1, base, s0, s0, s1, s1);
        addStaticGradientQuad(last, y0, -d0, first, y0, -d0,
          first, y1, -d1, last, y1, -d1, base, s0, s0, s1, s1);
        addStaticGradientQuad(-d0, y0, first, -d0, y0, last,
          -d1, y1, last, -d1, y1, first, base, s0, s0, s1, s1);
        addStaticGradientQuad(d0, y0, last, d0, y0, first,
          d1, y1, first, d1, y1, last, base, s0, s0, s1, s1);
      }
    }
    // Skyline blocks stand just inside the leaning face.
    const skyline = [.8, 1.25, .65, 1.55, .9, 1.18];
    const lean = (y) => foot + (edge - foot) * (y + .05) / (high + .05) - .03;
    for (let at = 0; at < skyline.length; at++) {
      const center = -7.5 + at * 3;
      const height = skyline[at];
      const facing = at & 1 ? -1 : 1;
      const low = facing * lean(-.04), top = facing * lean(height);
      addStaticGradientQuad(center - .64, -.04, low,
        center + .64, -.04, low,
        center + .64, height, top,
        center - .64, height, top,
        [.035, .085, .09], .92, .92, .48, .48);
    }
  }

  function addArenaWallSegment(center, horizontal, edge, inside) {
    const half = 2.125, outer = edge - inside * .35;
    const inner = edge, low = -.005, high = .845;
    // Inner faces darken toward the floor contact.
    const foot = .5, lip = .98;
    if (horizontal) {
      addStaticQuad(center - half, high, outer,
        center + half, high, outer,
        center + half, high, inner,
        center - half, high, inner, COLORS.wall, 1.12);
      addStaticGradientQuad(center - half, low, inner,
        center + half, low, inner,
        center + half, high, inner,
        center - half, high, inner, COLORS.wall, foot, foot, lip, lip,
        COOL_TONE);
    } else {
      addStaticQuad(outer, high, center - half,
        inner, high, center - half,
        inner, high, center + half,
        outer, high, center + half, COLORS.wall, 1.12);
      addStaticGradientQuad(inner, low, center - half,
        inner, low, center + half,
        inner, high, center + half,
        inner, high, center - half, COLORS.wall, foot, foot, lip, lip,
        COOL_TONE);
    }
  }

  function addObstacle(o) {
    if (!addBox(o[0], .48, o[1], o[2], .96, o[3], 0, COLORS.obstacle))
      return;
    /* The hidden bottom face becomes a contact shadow cast away from the
       key light, fading with distance from the block. */
    for (let at = vertexCount - 12; at < vertexCount - 8; at++) {
      const lx = BOX_VERTICES[(at - vertexCount + 24) * 3];
      const lz = BOX_VERTICES[(at - vertexCount + 24) * 3 + 2];
      positions[at * 3] = o[0] + lx * (o[2] * .5 + .12) - .2;
      positions[at * 3 + 1] = .03;
      positions[at * 3 + 2] = o[1] + lz * (o[3] * .5 + .12) - .46;
      colors.fill(0, at * 4, at * 4 + 3);
      colors[at * 4 + 3] = lz > 0 ? .62 : .1;
    }
  }

  function appendArenaGeometry(index) {
    const arena = ARENAS[index];
    const obstacles = arena.obstacles;
    const obstacleCount = arenaObstacleCount(arena);
    for (let at = 0; at < obstacleCount; at++) {
      addObstacle(obstacles[at]);
    }
    const ramps = arena.ramps;
    const rampCount = arenaRampCount(arena);
    for (let at = 0; at < rampCount; at++) {
      const ramp = ramps[at];
      addRamp(ramp[0], ramp[1], ramp[2], ramp[3], ramp[4], ramp[5]);
    }
  }

  function cacheArenaGeometry(index) {
    const firstVertex = vertexCount;
    indexCount = arenaCommonIndexCount;
    arenaPlaneMaxSpan = arenaCommonPlaneMaxSpan;
    appendArenaGeometry(index);
    const tailVertexCount = vertexCount - firstVertex;
    const tailIndexCount = indexCount - arenaCommonIndexCount;
    const tailIndices = new Uint16Array(tailIndexCount);
    tailIndices.set(indices.subarray(arenaCommonIndexCount, indexCount));
    arenaGeometryCache[index] = {
      indices: tailIndices,
      firstVertex,
      vertexCount: tailVertexCount, indexCount: tailIndexCount,
      planeMaxSpan: arenaPlaneMaxSpan,
      positions: positions.subarray(firstVertex * 3, vertexCount * 3),
      colors: colors.subarray(firstVertex * 4, vertexCount * 4),
    };
    arenaMaximumStaticIndexCount = Math.max(arenaMaximumStaticIndexCount,
      arenaCommonIndexCount + tailIndexCount);
  }

  function refreshGeneratedArenaGeometryCache() {
    const cached = arenaGeometryCache[GENERATED_ARENA_INDEX];
    if (!cached) return false;
    vertexCount = cached.firstVertex;
    indexCount = arenaCommonIndexCount;
    arenaPlaneMaxSpan = arenaCommonPlaneMaxSpan;
    appendArenaGeometry(GENERATED_ARENA_INDEX);
    const nextVertexCount = vertexCount - cached.firstVertex;
    const nextIndexCount = indexCount - arenaCommonIndexCount;
    if (nextVertexCount !== cached.vertexCount
        || nextIndexCount !== cached.indexCount) return false;
    for (let at = 0; at < nextIndexCount; at++)
      cached.indices[at] = indices[arenaCommonIndexCount + at];
    cached.planeMaxSpan = arenaPlaneMaxSpan;
    return true;
  }

  function restoreArenaGeometry(index) {
    const cached = arenaGeometryCache[index];
    if (!cached) return false;
    indexCount = arenaCommonIndexCount + cached.indexCount;
    indices.set(cached.indices, arenaCommonIndexCount);
    arenaPlaneMaxSpan = cached.planeMaxSpan;
    return true;
  }

  function buildArena(initialArenaOnly = false) {
    const previousCollection = collectInstancedBoxes;
    const reusedCommonGeometry = arenaCommonReady;
    collectInstancedBoxes = false;
    resetMesh();
    arenaPlaneMaxSpan = 0;
    /* Segment arena planes so PSP near-plane clipping cannot drop a large half. */
    if (arenaCommonReady) {
      vertexCount = arenaCommonVertexCount;
      indexCount = arenaCommonIndexCount;
      arenaPlaneMaxSpan = arenaCommonPlaneMaxSpan;
    } else {
      const floorStep = 4.5;
      for (let row = 0; row < 4; row++) {
        for (let column = 0; column < 4; column++) {
          const left = -9 + column * floorStep;
          const top = -9 + row * floorStep;
          addStaticGradientQuad(left, -.025, top + floorStep,
            left + floorStep, -.025, top + floorStep,
            left + floorStep, -.025, top,
            left, -.025, top, COLORS.ground,
            floorVignette(left, top + floorStep),
            floorVignette(left + floorStep, top + floorStep),
            floorVignette(left + floorStep, top),
            floorVignette(left, top));
        }
      }
      for (let line = -8; line <= 8; line += 2) {
        for (let segment = 0; segment < 4; segment++) {
          const first = -8.1 + segment * 4.05;
          const last = first + 4.05;
          addGridLine(false, line, first, last);
          addGridLine(true, line, first, last);
        }
      }
      for (let segment = 0; segment < 4; segment++) {
        const center = -6.375 + segment * 4.25;
        addArenaWallSegment(center, true, -ARENA_WALL_FACE, 1);
        addArenaWallSegment(center, true, ARENA_WALL_FACE, -1);
        addArenaWallSegment(center, false, -ARENA_WALL_FACE, 1);
        addArenaWallSegment(center, false, ARENA_WALL_FACE, -1);
      }
      addHorizonBackdrop();
      arenaCommonVertexCount = vertexCount;
      arenaCommonIndexCount = indexCount;
      arenaCommonPlaneMaxSpan = arenaPlaneMaxSpan;
      arenaCommonReady = true;
    }
    let uploadGeneratedGeometry = false;
    let appendedArena = -1;
    if (!reusedCommonGeometry) {
      /* A normal title-screen boot can prepare every immutable arena while
         the player is choosing a loadout.  If Deploy arrived before the
         runtime, prepare only the arena needed to start (plus the generated
         shape when Onslaught is selected).  Building every future arena in
         that trusted click's script turn made the button appear stuck for
         several seconds on a real PSP. */
      arenaPackedVertexCount = arenaCommonVertexCount;
      const first = initialArenaOnly ? state.arena : 0;
      const last = initialArenaOnly ? state.arena + 1 : ARENAS.length;
      for (let at = first; at < last; at++) {
        vertexCount = arenaPackedVertexCount;
        cacheArenaGeometry(at);
        arenaPackedVertexCount = vertexCount;
      }
      if (initialArenaOnly && isOnslaught()
          && state.arena !== GENERATED_ARENA_INDEX) {
        vertexCount = arenaPackedVertexCount;
        cacheArenaGeometry(GENERATED_ARENA_INDEX);
        arenaPackedVertexCount = vertexCount;
      }
    } else if (!arenaGeometryCache[state.arena]) {
      /* A fast Deploy boot leaves future authored arenas cold.  Populate one
         only at the existing arena-transition boundary, never during play. */
      vertexCount = arenaPackedVertexCount;
      indexCount = arenaCommonIndexCount;
      arenaPlaneMaxSpan = arenaCommonPlaneMaxSpan;
      cacheArenaGeometry(state.arena);
      arenaPackedVertexCount = vertexCount;
      appendedArena = state.arena;
    } else if (generatedGeometryDirty) {
      uploadGeneratedGeometry = refreshGeneratedArenaGeometryCache();
    }
    restoreArenaGeometry(state.arena);
    staticIndexCount = indexCount;
    if (vertexArrays) vertexArrays.bindVertexArrayOES(staticVertexArray);
    if (!reusedCommonGeometry) {
      gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0,
        positions.subarray(0, arenaPackedVertexCount * 3));
      gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0,
        colors.subarray(0, arenaPackedVertexCount * 4));
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, staticIndexBuffer);
      gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0,
        indices.subarray(0, staticIndexCount));
    } else {
      if (appendedArena >= 0) {
        const appended = arenaGeometryCache[appendedArena];
        gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, appended.firstVertex * 3 * 4,
          appended.positions);
        gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, appended.firstVertex * 4 * 4,
          appended.colors);
      }
      if (uploadGeneratedGeometry) {
        const generatedCache = arenaGeometryCache[GENERATED_ARENA_INDEX];
        gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, generatedCache.firstVertex * 3 * 4,
          generatedCache.positions);
        gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, generatedCache.firstVertex * 4 * 4,
          generatedCache.colors);
      }
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, staticIndexBuffer);
      gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, arenaCommonIndexCount * 2,
        arenaGeometryCache[state.arena].indices);
    }
    if (!reusedCommonGeometry || uploadGeneratedGeometry
        || appendedArena === GENERATED_ARENA_INDEX)
      generatedGeometryDirty = false;
    vertexCount = arenaPackedVertexCount;
    collectInstancedBoxes = previousCollection;
  }

  function prepareForgedArenaGeometrySlice() {
    const previousCollection = collectInstancedBoxes;
    collectInstancedBoxes = false;
    if (state.arena !== GENERATED_ARENA_INDEX || !generatedGeometryDirty) {
      resetMesh();
      arenaPlaneMaxSpan = arenaCommonPlaneMaxSpan;
      vertexCount = arenaCommonVertexCount;
      indexCount = arenaCommonIndexCount;
      arenaGenerationUploadGenerated = false;
      restoreArenaGeometry(state.arena);
      staticIndexCount = indexCount;
      vertexCount = arenaPackedVertexCount;
      collectInstancedBoxes = previousCollection;
      return true;
    }
    const cached = arenaGeometryCache[GENERATED_ARENA_INDEX];
    const arena = ARENAS[GENERATED_ARENA_INDEX];
    const obstacleCount = arenaObstacleCount(arena);
    const rampCount = arenaRampCount(arena);
    if (arenaGenerationGeometryCursor < 0) {
      arenaGenerationGeometryCursor = 0;
      arenaGenerationTailVertices = arenaGenerationTailIndices = 0;
      arenaGenerationPlaneMaxSpan = arenaCommonPlaneMaxSpan;
    }
    vertexCount = cached.firstVertex + arenaGenerationTailVertices;
    indexCount = arenaCommonIndexCount + arenaGenerationTailIndices;
    arenaPlaneMaxSpan = arenaGenerationPlaneMaxSpan;
    if (arenaGenerationGeometryCursor < obstacleCount) {
      addObstacle(arena.obstacles[arenaGenerationGeometryCursor]);
    } else {
      const ramp = arena.ramps[arenaGenerationGeometryCursor - obstacleCount];
      addRamp(ramp[0], ramp[1], ramp[2], ramp[3], ramp[4], ramp[5]);
    }
    arenaGenerationGeometryCursor++;
    arenaGenerationTailVertices = vertexCount - cached.firstVertex;
    arenaGenerationTailIndices = indexCount - arenaCommonIndexCount;
    arenaGenerationPlaneMaxSpan = arenaPlaneMaxSpan;
    if (arenaGenerationGeometryCursor < obstacleCount + rampCount) {
      vertexCount = arenaPackedVertexCount;
      collectInstancedBoxes = previousCollection;
      return false;
    }
    arenaGenerationUploadGenerated = arenaGenerationTailVertices
      === cached.vertexCount && arenaGenerationTailIndices === cached.indexCount;
    if (arenaGenerationUploadGenerated) {
      for (let at = 0; at < arenaGenerationTailIndices; at++)
        cached.indices[at] = indices[arenaCommonIndexCount + at];
      cached.planeMaxSpan = arenaPlaneMaxSpan;
    }
    restoreArenaGeometry(state.arena);
    staticIndexCount = indexCount;
    vertexCount = arenaPackedVertexCount;
    collectInstancedBoxes = previousCollection;
    return true;
  }

  function publishForgedArenaGeometry() {
    if (vertexArrays) vertexArrays.bindVertexArrayOES(staticVertexArray);
    if (arenaGenerationUploadGenerated) {
      const generatedCache = arenaGeometryCache[GENERATED_ARENA_INDEX];
      gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, generatedCache.firstVertex * 3 * 4,
        generatedCache.positions);
      gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, generatedCache.firstVertex * 4 * 4,
        generatedCache.colors);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, staticIndexBuffer);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, arenaCommonIndexCount * 2,
      arenaGeometryCache[state.arena].indices);
    if (arenaGenerationUploadGenerated) generatedGeometryDirty = false;
    arenaGenerationUploadGenerated = false;
  }

  /* Quick Match foes never deploy within QUICK_SPAWN_MIN_DISTANCE of a
     player hull on another team: placeFoe arms this zone for the clear-spot
     search, so a nudged spot cannot slide back toward the player. */
  const QUICK_SPAWN_MIN_DISTANCE = 6;
  const QUICK_SPAWN_MIN_SQUARED = QUICK_SPAWN_MIN_DISTANCE * QUICK_SPAWN_MIN_DISTANCE;
  let spawnAvoidTeam = -1;
  function spawnNearPlayer(x, z) {
    for (let at = 0; at < MAX_TANKS; at++) {
      const other = tanks[at];
      if (!other.active || !other.player || other.team === spawnAvoidTeam) continue;
      const dx = other.x - x, dz = other.z - z;
      if (dx * dx + dz * dz < QUICK_SPAWN_MIN_SQUARED) return true;
    }
    return false;
  }

  function spawnBlocked(tank, x, z, radius) {
    if (spawnAvoidTeam >= 0 && spawnNearPlayer(x, z)) return true;
    return x < -HULL_EDGE + radius || x > HULL_EDGE - radius
      || z < -HULL_EDGE + radius || z > HULL_EDGE - radius
      || circleHitsObstacle(x, z, radius, true) || hullTouchesTank(tank, x, z, radius)
      || (convoy.active && convoyOverlap(x, z, radius) > 0);
  }

  /* The convoy crawler is a solid 1.5 x 1.85 box for hulls (shells keep
     their own hit test). Depth a circle reaches into it; 0 when clear. */
  const CONVOY_HALF_WIDTH = .75, CONVOY_HALF_DEPTH = .925;
  function convoyOverlap(x, z, radius) {
    const ax = (x < convoy.x ? convoy.x - x : x - convoy.x) - CONVOY_HALF_WIDTH;
    const az = (z < convoy.z ? convoy.z - z : z - convoy.z) - CONVOY_HALF_DEPTH;
    if (ax >= radius || az >= radius) return 0;
    if (ax <= 0 && az <= 0) return radius - (ax > az ? ax : az);
    const dx = ax > 0 ? ax : 0, dz = az > 0 ? az : 0;
    const squared = dx * dx + dz * dz;
    return squared < radius * radius ? radius - Math.sqrt(squared) : 0;
  }

  /* Move the crawler, nudging hulls out of its way like the barriers it
     rams into rubble. A hull it cannot nudge (pinned against scenery or
     another hull) stalls it instead: never crushed, never pushed through a
     wall. False when stalled; the convoy then stays where it was. */
  const convoyPush = new Float64Array(MAX_TANKS * 2);
  function moveConvoy(x, z) {
    const oldX = convoy.x, oldZ = convoy.z;
    // Mission setup can start the crawler part-way along: never stall that.
    const jump = Math.abs(z - oldZ) > .3;
    convoy.x = x; convoy.z = z;
    let pushed = 0;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (!tank.active) continue;
      const radius = tank.collisionRadius;
      const overlap = convoyOverlap(tank.x, tank.z, radius);
      if (overlap <= 0) continue;
      // Out along the nearest face, or away from the nearest box point.
      const rx = tank.x - x, rz = tank.z - z;
      const ax = (rx < 0 ? -rx : rx) - CONVOY_HALF_WIDTH;
      const az = (rz < 0 ? -rz : rz) - CONVOY_HALF_DEPTH;
      let nx = 0, nz = 0;
      if (ax > 0 && az > 0) {
        const length = Math.sqrt(ax * ax + az * az);
        nx = (rx < 0 ? -ax : ax) / length; nz = (rz < 0 ? -az : az) / length;
      } else if (ax > az) nx = rx < 0 ? -1 : 1;
      else nz = rz < 0 ? -1 : 1;
      const px = tank.x + nx * (overlap + .01), pz = tank.z + nz * (overlap + .01);
      if (px < -HULL_EDGE + radius || px > HULL_EDGE - radius || pz < -HULL_EDGE + radius
          || pz > HULL_EDGE - radius || circleHitsObstacle(px, pz, radius)
          || hullTouchesTank(tank, px, pz, radius)) {
        if (jump) continue;
        convoy.x = oldX; convoy.z = oldZ;
        return false;
      }
      convoyPush[at * 2] = px; convoyPush[at * 2 + 1] = pz;
      pushed |= 1 << at;
    }
    for (let at = 0; pushed; at++, pushed >>>= 1) {
      if (!(pushed & 1)) continue;
      const tank = tanks[at];
      tank.x = convoyPush[at * 2]; tank.z = convoyPush[at * 2 + 1];
      tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
    }
    return true;
  }

  function hullTouchesTank(tank, x, z, radius) {
    for (let at = 0; at < MAX_TANKS; at++) {
      const other = tanks[at];
      if (other === tank || !other.active) continue;
      const dx = other.x - x, dz = other.z - z;
      const reach = radius + other.collisionRadius;
      if (dx * dx + dz * dz < reach * reach) return true;
    }
    return false;
  }

  /* The placement search: (x, z) itself, else rings `spacing` apart, out
     to `rings`, each tried at `headings` evenly spaced headings; the first
     point blocked(x, z) refuses not lands in clearSpot. False, with
     clearSpot at (x, z), when every point is refused. Deterministic. */
  const clearSpot = {x: 0, z: 0};
  function searchClearSpot(x, z, rings, spacing, headings, blocked,
                           tank = null, radius = 0) {
    clearSpot.x = x; clearSpot.z = z;
    if (!blocked(x, z, tank, radius)) return true;
    for (let ring = 1; ring <= rings; ring++) {
      for (let step = 0; step < headings; step++) {
        const angle = step * Math.PI / (headings >> 1);
        clearSpot.x = x + Math.sin(angle) * ring * spacing;
        clearSpot.z = z + Math.cos(angle) * ring * spacing;
        if (!blocked(clearSpot.x, clearSpot.z, tank, radius)) return true;
      }
    }
    clearSpot.x = x; clearSpot.z = z;
    return false;
  }
  // A tank's spawn: the nearest spot within 6 units its hull fits.
  function tankSpotBlocked(x, z, tank, radius) {
    return spawnBlocked(tank, x, z, radius);
  }
  function findClearSpot(tank, x, z, radius) {
    return searchClearSpot(x, z, 24, .25, 16, tankSpotBlocked, tank, radius);
  }

  function placeTank(tank, x, z, yaw, team, gadget, role = "HUNTER",
                     classId = tank.classId) {
    classId = Math.max(0, Math.min(CLASSES.length - 1, classId | 0));
    /* Spawn points are fixed coordinates; generated arenas, crates, gates,
       campaign walls or a tank already standing there can cover them. Move
       to the nearest clear spot instead of starting inside an obstacle. */
    findClearSpot(tank, x, z, CLASS_COLLISION_RADIUS[classId] + .04);
    x = clearSpot.x; z = clearSpot.z;
    const profile = CLASSES[classId];
    tank.active = true;
    tank.x = x; tank.z = z; tank.yaw = yaw; tank.turret = yaw;
    updateTankYawCache(tank);
    updateTankTurretCache(tank);
    tank.team = team; tank.classId = classId;
    tank.scale = CLASS_SCALE[classId];
    tank.driveSpeed = CLASS_SPEED[classId];
    tank.collisionRadius = CLASS_COLLISION_RADIUS[classId];
    tank.maxHealth = profile.health; tank.health = profile.health;
    tank.cooldown = tank.secondaryCooldown = tank.commandBuff = 0;
    tank.fireHeld = false; tank.fireCharge = 0;
    tank.lunge = tank.lungeCooldown = tank.drift = tank.command.lunge = 0;
    tank.boss = false; tank.leftTreadHealth = tank.rightTreadHealth = tank.turretHealth = 0;
    tank.slideX = tank.slideZ = 0;
    tank.shield = 0;
    tank.repair = 0; tank.boost = 0; tank.gadgetCooldown = 0;
    tank.recoil = tank.hitFlash = tank.fireWindup = 0;
    tank.fireSecondaryArmed = tank.fireTelegraphed = false;
    tank.gadgetCooldownMax = 1;
    tank.spawnGrace = 1; tank.respawn = 0; tank.inert = false;
    tank.gadget = gadget; tank.aiThink = 0; tank.role = role;
    tank.guardIdle = 0;
    tank.velocityX = tank.velocityZ = 0;
    tank.target = -1; tank.aimRefresh = 0; tank.bankAim = false;
    tank.bankNext = 0; tank.bankTarget = tank.bankRevision = -1;
    tank.targetChoice = tank.targetEpoch = -1; tank.targetNext = 0;
    tank.aimErrorX = tank.aimErrorZ = 0;
    tank.lastAimX = tank.lastAimZ = NaN; tank.aimAngle = yaw;
    tank.navGoal = tank.navRevision = tank.navFieldSlot = -1;
    tank.navRawGoal = tank.navMappedGoal = tank.navMapRevision = -1;
    tank.navWaypoint = tank.navWaypointFrom = -1;
    tank.strategyNext = 0; tank.strategyTarget = tank.strategyEpoch = tank.strategyMood = -1;
    tank.strategyConvoy = false;
    tank.orbitRoute = false;
    tank.breachActive = tank.breachHold = tank.breachShot = tank.breachQueued = false;
    tank.breachKind = 0; tank.breachIndex = tank.breachRevision = -1;
    tank.breachAngle = NaN; tank.breachShotNext = tank.breachRetry = 0;
    tank.blockedTime = tank.avoidTime = 0;
    tank.avoidTurn = tank.id & 1 ? 1 : -1;
    tank.backingOff = false;
    tank.standoff = false;
    tank.orbitTurn = tank.id & 1 ? 1 : -1;
    tank.surfaceY = surfaceHeightAt(x, z);
    tank.treadDistance = 0;
    tank.command.left = tank.command.right = 0;
    // Staggered bots can move before their first planning turn. Never carry
    // an old match's aim into that first step (or a replay's initial state).
    tank.command.aimX = tank.yawSine; tank.command.aimZ = tank.yawCosine;
    tank.command.fire = tank.command.secondary = tank.command.gadget = false;
    tank.command.ultimate = false;
    tank.command.reverse = false;
  }

  /* A Quick Match foe: placeTank with the player keep-out armed. When no
     clear spot near (x, z) is far enough (a respawn base the player sits
     on), deploy on the far side of the arena from the player instead. */
  function placeFoe(tank, x, z, yaw, team, gadget, role, classId) {
    const radius = CLASS_COLLISION_RADIUS[Math.max(0,
      Math.min(CLASSES.length - 1, classId | 0))] + .04;
    spawnAvoidTeam = team;
    if (!findClearSpot(tank, x, z, radius)) {
      const player = playerTank();
      const length = Math.sqrt(player.x * player.x + player.z * player.z);
      x = length > .5 ? -player.x / length * 5.6 : 0;
      z = length > .5 ? -player.z / length * 5.6 : 5.6;
      yaw = Math.atan2(-x, -z);
      if (!findClearSpot(tank, x, z, radius)) spawnAvoidTeam = -1;
    }
    if (spawnAvoidTeam >= 0) { x = clearSpot.x; z = clearSpot.z; }
    placeTank(tank, x, z, yaw, team, gadget, role, classId);
    spawnAvoidTeam = -1;
  }
  // Quick Match foes deploy on a far arc like the campaign's: the player
  // starts at z -5.8, the arc spans +-1.5 rad around +z (a little wider
  // than the campaign's, so five foes do not jam each other leaving it).
  const farArcAngle = (at, count) => count > 1 ? -1.5 + 3 * at / (count - 1) : 0;

  // An arena starts without hazards or crates, and with an empty hazard grid.
  function resetHazards() {
    for (const hazard of hazards) hazard.active = false;
    hazardGrid.fill(0); hazardGridFallbackMask = 0;
    for (const crate of crates) crate.active = false;
    hazardCursor = 0;
  }

  function beginArena(index, geometryPrepared = false) {
    resetBotPlanning();
    state.arena = index;
    killcamCount = killcamWrite = 0; killcamLastTime = -99;
    for (const bullet of bullets) bullet.active = false;
    clearTracers();
    clearEffects();
    bulletActiveMask = 0;
    for (const pickup of pickups) pickup.active = false;
    for (const mine of mines) mine.active = false;
    for (const smoke of smokeClouds) smoke.active = false;
    activeSmokeCount = 0;
    for (const barrier of barriers) barrier.present = barrier.active = false;
    for (const tank of tanks) tank.active = false;
    resetHazards();
    const arena = ARENAS[index];
    const authoredBarriers = arena.barriers;
    const authoredBarrierCount = arenaBarrierCount(arena);
    for (let at = 0; at < authoredBarrierCount && at < MAX_BARRIERS; at++) {
      const source = authoredBarriers[at], barrier = barriers[at];
      barrier.present = barrier.active = true;
      barrier.x = source[0]; barrier.z = source[1];
      barrier.width = source[2]; barrier.depth = source[3]; barrier.health = 2;
      barrier.left = barrier.x - barrier.width * .5;
      barrier.right = barrier.x + barrier.width * .5;
      barrier.top = barrier.z - barrier.depth * .5;
      barrier.bottom = barrier.z + barrier.depth * .5;
    }
    fillBarrierCircleGrid();
    retainedSceneryDirty = true;
    state.blueControl = state.redControl = 0;
    state.gateOpen = false; state.ricochets = state.barriersBroken = 0;
    invalidateBotNavigation();
    convoy.active = state.gameMode === MODE_CONVOY;
    convoy.x = -1.8; convoy.z = -4.8;
    convoy.health = 180; convoy.progress = 0;
    const dailyLoadout = dailySeed(state.dailyDay);
    // Campaign missions re-place their foes; their crates were laid out
    // around (and tuned on) the original ring spawn.
    const quickSpawn = !campaign.owns(index);
    placeTank(tanks[0], state.gameMode === MODE_CONVOY ? 1.6 : 0, -5.8, 0, 0,
      GADGETS[state.gameMode === MODE_DAILY ? (dailyLoadout >>> 8) % 5 : state.gadgetChoice],
      "PLAYER", state.gameMode === MODE_DAILY ? dailyLoadout % 3 : state.classChoice);
    if (state.gameMode === MODE_DUEL) {
      placeTank(tanks[0], -4.5, -4.5, .7, 0, GADGETS[state.gadgetChoice], "PLAYER", state.classChoice);
      placeTank(tanks[1], 4.5, 4.5, -.7, 1, GADGETS[state.gadgetChoice], "PLAYER", state.classChoice);
      tanks[0].player = tanks[1].player = true;
      state.duelTurn = 0; state.duelTime = 12; state.duelResolving = false;
    } else if (state.gameMode === MODE_CONTROL) {
      placeTank(tanks[1], -2, -5.4, 0, 0, GADGETS[2], "CAPTURE", 1);
      for (let at = 2; at < 5; at++) {
        const angle = quickSpawn ? farArcAngle(at - 2, 3) : (at - 2) / 3 * Math.PI * 2 + .35;
        (quickSpawn ? placeFoe : placeTank)(tanks[at], Math.sin(angle) * 5.7,
          Math.cos(angle) * 5.7, wrapAngle(angle + Math.PI), 1,
          GADGETS[at % GADGETS.length],
          at === 2 ? "CAPTURE" : at === 3 ? "FLANK" : "GUARD", at % 3);
      }
    } else {
      const enemies = state.gameMode === MODE_CONVOY ? 4 : isOnslaught()
        ? Math.min(MAX_TANKS - 1, 2 + Math.ceil(state.wave * .5))
        : ARENAS[index].enemies;
      for (let at = 0; at < enemies; at++) {
        const angle = quickSpawn ? farArcAngle(at, enemies) : at / enemies * Math.PI * 2 + .35;
        (quickSpawn ? placeFoe : placeTank)(tanks[at + 1], Math.sin(angle) * 5.6,
          Math.cos(angle) * 5.6, wrapAngle(angle + Math.PI), 1,
          GADGETS[(at + 1) % GADGETS.length],
          state.gameMode === MODE_CONVOY
            ? (at < 2 ? "AMBUSH" : at === 2 ? "FLANK" : "GUARD")
            : (at % 3 === 1 ? "FLANK" : at % 3 === 2 ? "GUARD" : "HUNTER"),
          isOnslaught() ? (at + state.wave) % 3 : (at + 1) % 3);
      }
    }
    if (isOnslaught() && state.wave % 5 === 0) {
      const boss = tanks[MAX_TANKS - 1];
      placeFoe(boss, 0, 5, Math.PI, 1, GADGETS[2], "GUARD", 2);
      makeBoss(boss, 2);
      // Boss spawn uses its enlarged collision envelope, not Bulwark's, and
      // stays near its far-side spot (a grid scan began in the player's corner).
      spawnAvoidTeam = 1;
      if (findClearSpot(boss, 0, 5, boss.collisionRadius + .04)) {
        boss.x = clearSpot.x; boss.z = clearSpot.z;
        boss.surfaceY = surfaceHeightAt(boss.x, boss.z);
      }
      spawnAvoidTeam = -1;
    }
    initializeArenaHazards(); applyPalette();
    campaign.arena(index);
    validateSpawns();
    cameraYaw = playerTank().yaw;
    cameraSine = playerTank().yawSine;
    cameraCosine = playerTank().yawCosine;
    cameraTurn = 0; cameraArenaCut = true; hudToastStale = true;
    state.transition = 0;
    if (!geometryPrepared) buildArena();
    /* Retain transition chrome in the WebGL overlay. */
    hudToast = `ARENA ${index + 1}`;
    hudMeshDirty = true;
    toastUntil = state.time + 1.2;
  }

  function resetGame() {
    if (!replayActive) {
      replayRecording = false; replayCount = 0; replayTruncated = false;
      replayPending = !online.active && !qualificationLongSoak && !botLeagueActive;
    }
    state.score = 0; state.lives = 3; state.kills = 0;
    state.wave = 1; state.multiplier = 1;
    if (!replayActive) runAimLevel = 0;
    state.bankKills = state.doubleBanks = 0;
    state.mainShots = 0; state.duelWinner = -1;
    for (let at = 0; at < MAX_TANKS; at++) {
      tanks[at].player = at === 0; tanks[at].difficulty = -1;
    }
    state.killBeat = 0; state.pendingClear = false;
    state.heartbeatAt = 0;
    if (qualificationStats !== null) {
      qualificationStats.botTelegraphs = qualificationStats.botPlayerShots = 0;
      qualificationStats.botTelegraphViolations = 0;
      qualificationStats.botVolleyMinimum = 99;
    }
    lastBotPlayerShotAt = -99;
    state.shake = 0;
    state.shots = state.hits = state.damageTaken = state.objectiveTicks = 0;
    state.hitConfirm = state.damageIndicator = 0;
    state.commandMeter = 0; state.armorZone = "FRONT";
    randomState = 0x7a2d31c5;
    bulletSpawnCursor = particleSpawnCursor = particleTemplateCursor = 0;
    /* Every run (Quick Match, campaign, practice, online, replay) starts
       here, so no earlier Onslaught forge can outlive it: an unfinished
       one would keep advanceSimulation in its phase machine (nothing
       moves, nothing clears) and could later swap in its own arena. */
    arenaGenerationPhase = ARENA_GENERATION_IDLE;
    arenaGenerationReuseSceneOnce = false;
    hudRunStart = false;
    let firstArena = state.gameMode === MODE_CONTROL ? 1 : state.gameMode === MODE_CONVOY ? 2 : 0;
    if (isOnslaught() && !online.active) {
      if (state.gameMode === MODE_DAILY) state.dailyDay = dailyDay();
      const seed = state.gameMode === MODE_DAILY ? dailySeed(state.dailyDay)
        : requestedArenaSeed || nextArenaSeed;
      if (!requestedArenaSeed && state.gameMode !== MODE_DAILY) {
        nextArenaSeed = xorshift32(nextArenaSeed) >>> 0;
        if (!nextArenaSeed) nextArenaSeed = 0x4d3c2b1a;
      }
      arenaGenerationRunSeed = seed >>> 0 || 1;
      arenaGenerationStartedAt = state.wallTime;
      arenaGenerationTarget = 1;
      arenaGenerationProgress = -1;
      arenaGenerationPhase = ARENA_GENERATION_ATTEMPTS;
      arenaGenerator.beginGeneration(arenaGenerationRunSeed, 20);
      setMode("generating");
      return false;
    }
    beginArena(firstArena);
    if (isOnslaught()) sounds.waveStart();
    hudRunStart = true;
    return true;
  }

  function updateArenaGeneration() {
    const progress = Math.min(3,
      Math.max(0, ((state.wallTime - arenaGenerationStartedAt) * 4) | 0));
    if (progress !== arenaGenerationProgress) {
      arenaGenerationProgress = progress;
      hudToast = forgingHeadings[progress];
      hudMeshDirty = true;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_ATTEMPTS) {
      /* One bounded generation slice per frame is stricter than the original
         two-attempt ceiling: flood fills and sightline work can yield inside
         one candidate instead of relying on an attempt being uniformly cheap. */
      const generated = arenaGenerator.stepGeneration(1);
      if (!generated.done) return;
      noteArenaGeneration(generated);
      arenaGenerationTarget = generated.accepted
        ? GENERATED_ARENA_INDEX : 1;
      arenaGenerationPhase = ARENA_GENERATION_SPATIAL;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_SPATIAL) {
      if (arenaGenerationTarget === GENERATED_ARENA_INDEX) {
        fillArenaSpatialData(GENERATED_ARENA_INDEX);
        generatedGeometryDirty = true;
      }
      state.arena = arenaGenerationTarget;
      arenaGenerationGeometryCursor = -1;
      arenaGenerationPhase = ARENA_GENERATION_GEOMETRY;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_GEOMETRY) {
      /* CPU tail construction, native buffer upload, and simulation reset
         each own a frame. The ordinary buildArena() path remains authoritative
         for authored transitions; this is its bounded generated-tail split. */
      if (!prepareForgedArenaGeometrySlice()) return;
      arenaGenerationPhase = ARENA_GENERATION_UPLOAD;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_UPLOAD) {
      publishForgedArenaGeometry();
      arenaGenerationPhase = ARENA_GENERATION_HOLD;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_HOLD) {
      if (state.wallTime - arenaGenerationStartedAt
          < ARENA_GENERATION_MINIMUM_HOLD) return;
      arenaGenerationPhase = ARENA_GENERATION_SETUP;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_SETUP) {
      /* Keep the already-published forging scene for this frame. Simulation
         setup and the first dynamic-scene build must not share one callback. */
      beginArena(arenaGenerationTarget, true);
      arenaGenerationReuseSceneOnce = true;
      arenaGenerationPhase = ARENA_GENERATION_PREPARE;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_PREPARE) {
      /* This frame builds and uploads the first complete arena behind the
         forging HUD. The next phase can therefore change surfaces without a
         cold scene build on the same display deadline. */
      arenaGenerationPhase = ARENA_GENERATION_WARM;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_WARM) {
      if (state.mode !== "playing") {
        setMode("playing");
        updateHud(true);
      }
      arenaGenerationReuseSceneOnce = true;
      if (hudMeshDirty || hudBuildPhase >= 0 || hudIndicatorDirty) return;
      arenaGenerationPhase = ARENA_GENERATION_ACTIVATE;
      return;
    }
    if (arenaGenerationPhase === ARENA_GENERATION_ACTIVATE) {
      arenaGenerationReuseSceneOnce = true;
      arenaGenerationPhase = ARENA_GENERATION_IDLE;
      sounds.waveStart();
    }
  }

  function startOrResume() {
    requestPresentation();
    if (state.mode === "duel-pass") {
      playerTank().fireHeld = false; playerTank().fireCharge = 0;
      setMode("playing");
    } else if (state.mode === "paused") setMode("playing");
    else if (resetGame()) setMode("playing");
  }
  ui.play.addEventListener("click", startOrResume);
  ui.replayLast.addEventListener("click", () => {
    if (startReplay()) requestPresentation();
  });
  ui.replayShare.addEventListener("click", () => {
    const code = exportReplay();
    ui.replayCode.hidden = false;
    ui.replayCode.value = code;
    showToast(code ? "REPLAY CODE READY" : "REPLAY TOO LARGE TO SHARE", 1.2);
  });
  ui.replayLoad.addEventListener("click", () => {
    if (ui.replayCode && ui.replayCode.hidden) {
      ui.replayCode.hidden = false; ui.replayCode.focus(); return;
    }
    if (ui.replayCode && importReplay(ui.replayCode.value) && startReplay()) requestPresentation();
    else showToast("INVALID REPLAY CODE", 1.2);
  });

  const input = {
    keys: Object.create(null), pointerAimX: 0, pointerAimY: -1,
    pointerActive: false, source: "keyboard", gamepadConnected: false,
    aimAwayAge: 0, aimTowardAge: 0, aimLeftAge: 0, aimRightAge: 0,
    assistLockTarget: -1, assistLockBroken: false,
    lastFire: false, lastSecondary: false, lastGadget: false,
    lastUltimate: false, lastPause: false,
    fireReleaseRequired: false,
    fireQueued: false, secondaryQueued: false, gadgetQueued: false,
    ultimateQueued: false, pauseQueued: false,
  };

  /* The first connected gamepad. A simulation step reads it once (the
     menus in campaign.js and the controls both poll it, and every read
     builds a fresh array of native gamepad objects); outside a step every
     call reads. */
  let simulationStep = 0, stepping = false, gamepadStep = -1, stepGamepad = null;
  function connectedGamepad() {
    if (stepping && gamepadStep === simulationStep) return stepGamepad;
    let found = null;
    if (typeof navigator.getGamepads === "function") {
      const pads = navigator.getGamepads();
      for (let at = 0; pads && at < pads.length; at++)
        if (pads[at] && pads[at].connected) { found = pads[at]; break; }
    }
    if (stepping) { gamepadStep = simulationStep; stepGamepad = found; }
    return found;
  }

  function updateControlHint() { controls.hint(); }

  function resetAimAssist() {
    input.aimAwayAge = input.aimTowardAge = 0;
    input.aimLeftAge = input.aimRightAge = 0;
    input.assistLockTarget = -1;
    input.assistLockBroken = false;
    controls.reset();
  }

  function clearSchemeKeys() {
    input.keys.moveLeft = input.keys.moveRight = false;
    input.keys.moveUp = input.keys.moveDown = false;
    input.keys.aimLeft = input.keys.aimRight = false;
    input.keys.aimUp = input.keys.aimDown = false;
    input.keys.shift = false;
  }

  function setSource(source) {
    if (input.source === source) return;
    input.source = source;
    input.lastFire = input.lastSecondary = input.lastGadget = false;
    input.lastUltimate = input.lastPause = false;
    resetAimAssist();
  }

  function keyboardDown(name) { return !!input.keys[name]; }

  function applyScreenAim(command, aimX, aimY) {
    const forwardX = cameraSine, forwardZ = cameraCosine;
    const rightX = cameraCosine, rightZ = -cameraSine;
    command.aimX = rightX * aimX + forwardX * -aimY;
    command.aimZ = rightZ * aimX + forwardZ * -aimY;
  }

  function pollPlayerInput() {
    if (replayActive) applyReplayCommand();
    else if (!botLeagueActive) controls.poll();
  }

  function applyReplayCommand() {
    const base = replayCursor * REPLAY_WORDS, command = playerTank().command;
    const flags = replayLog[base + 7] % 128;
    command.left = replayLog[base + 1]; command.right = replayLog[base + 2];
    command.aimX = replayLog[base + 3]; command.aimZ = replayLog[base + 4];
    command.reverse = !!(flags & 1); command.fire = !!(flags & 2);
    command.lunge = flags & 64 ? flags & 1 ? -1 : 1 : 0;
    command.secondary = !!(flags & 4); command.gadget = !!(flags & 8);
    command.ultimate = !!(flags & 16);
  }

  function sequenceIsNewer(candidate, previous) {
    const difference = (candidate - previous) & 0xffff;
    return difference !== 0 && difference < 0x8000;
  }

  function encodeSignedUnit(value) {
    return Math.max(-32767, Math.min(32767,
      Math.round((Number(value) || 0) * 32767)));
  }

  function decodeSignedUnit(value) { return value / 32767; }

  function sendOnlinePacket(buffer) {
    const channel = online.channel;
    if (!channel || channel.readyState !== "open"
        || channel.bufferedAmount > 1024) return false;
    try { channel.send(buffer); return true; } catch (_) { return false; }
  }

  /* Fixed send buffers: Tilefinch's channel and the WebRTC adapter copy
     what they are given, so a packet never needs a fresh one. A snapshot
     is sent through the prepared view of its exact length. */
  const onlineInputView = new DataView(new ArrayBuffer(ONLINE_INPUT_BYTES));
  const onlineSnapshotView = new DataView(new ArrayBuffer(
    ONLINE_SNAPSHOT_HEADER_BYTES + MAX_TANKS * ONLINE_TANK_BYTES
      + ONLINE_BULLET_LIMIT * ONLINE_BULLET_BYTES));
  const onlineSnapshotPackets = Array.from({length: ONLINE_BULLET_LIMIT + 1},
    (_, bullets) => new Uint8Array(onlineSnapshotView.buffer, 0,
      ONLINE_SNAPSHOT_HEADER_BYTES + MAX_TANKS * ONLINE_TANK_BYTES
        + bullets * ONLINE_BULLET_BYTES));

  function sendOnlineInput() {
    const command = playerTank().command;
    const view = onlineInputView;
    view.setUint8(0, 1);
    view.setUint8(1, ONLINE_PROTOCOL_VERSION);
    online.inputSequence = (online.inputSequence + 1) & 0xffff;
    view.setUint16(2, online.inputSequence, true);
    let flags = command.left ? 1 : 0;
    if (command.right) flags |= 2;
    if (command.left < 0) flags |= 256;
    if (command.right < 0) flags |= 512;
    if (command.lunge) flags |= 1024;
    if (command.reverse) flags |= 4;
    if (command.fire) flags |= 8;
    if (command.secondary) flags |= 16;
    if (command.gadget) flags |= 32;
    if (command.ultimate) flags |= 64;
    flags |= online.pendingActions;
    view.setUint16(4, flags, true);
    view.setInt16(6, encodeSignedUnit(command.aimX), true);
    view.setInt16(8, encodeSignedUnit(command.aimZ), true);
    if (sendOnlinePacket(view.buffer)) online.pendingActions = 0;
  }

  function applyOnlineInput(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength !== ONLINE_INPUT_BYTES)
      return false;
    const view = new DataView(buffer);
    if (view.getUint8(0) !== 1 || view.getUint8(1) !== ONLINE_PROTOCOL_VERSION)
      return false;
    const sequence = view.getUint16(2, true);
    if (online.receivedInputValid
        && !sequenceIsNewer(sequence, online.receivedInputSequence)) return false;
    online.receivedInputSequence = sequence;
    online.receivedInputValid = true;
    online.lastReceive = state.time;
    const flags = view.getUint16(4, true), command = tanks[1].command;
    command.left = flags & 1 ? flags & 256 ? -1 : 1 : 0;
    command.right = flags & 2 ? flags & 512 ? -1 : 1 : 0;
    command.reverse = !!(flags & 4);
    // Set-only: a later packet before the next step must not drop it.
    if (flags & 1024) command.lunge = flags & 4 ? -1 : 1;
    command.fire = !!(flags & 8);
    command.secondary = !!(flags & 16);
    command.gadget = !!(flags & 32);
    command.ultimate = !!(flags & 64);
    command.aimX = decodeSignedUnit(view.getInt16(6, true));
    command.aimZ = decodeSignedUnit(view.getInt16(8, true));
    return true;
  }

  function onlineModeCode() {
    if (state.mode === "arena-clear") return 1;
    if (state.mode === "game-over") return 2;
    if (state.mode === "victory") return 3;
    return 0;
  }

  function activeBarrierMask() {
    let mask = 0;
    for (let at = 0; at < barriers.length && at < 16; at++)
      if (barriers[at].present && barriers[at].active) mask |= 1 << at;
    return mask;
  }

  function sendOnlineSnapshot() {
    let bulletCount = 0;
    for (let mask = bulletActiveMask; mask && bulletCount < ONLINE_BULLET_LIMIT;
         mask = (mask & (mask - 1)) >>> 0) bulletCount++;
    const view = onlineSnapshotView;
    view.setUint8(0, 2);
    view.setUint8(1, ONLINE_PROTOCOL_VERSION);
    online.snapshotSequence = (online.snapshotSequence + 1) & 0xffff;
    view.setUint16(2, online.snapshotSequence, true);
    const generated = state.arena === GENERATED_ARENA_INDEX;
    view.setUint8(4, generated ? ONLINE_GENERATED_ARENA : state.arena);
    view.setUint8(5, onlineModeCode());
    view.setUint8(6, MAX_TANKS);
    view.setUint8(7, bulletCount);
    view.setUint32(8, state.score >>> 0, true);
    view.setUint8(12, state.lives);
    view.setUint8(13, Math.round(state.blueControl));
    view.setUint8(14, Math.round(state.redControl));
    view.setUint8(15, state.gateOpen ? 1 : 0);
    view.setUint16(16, Math.max(0, Math.round(convoy.health)), true);
    view.setUint16(18, Math.round(convoy.progress * 65535), true);
    view.setUint32(20, state.frames >>> 0, true);
    view.setUint32(24, generated ? state.arenaSeed >>> 0 : 0, true);
    view.setUint16(28, generated ? arenaGenerationChecksum : 0, true);
    view.setUint16(30, activeBarrierMask(), true);
    let offset = ONLINE_SNAPSHOT_HEADER_BYTES;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      let flags = tank.active ? 1 : 0;
      if (tank.team) flags |= 2;
      flags |= (tank.classId & 3) << 2;
      view.setUint8(offset, flags);
      view.setUint8(offset + 1, Math.max(0, Math.min(255, Math.round(tank.health))));
      view.setInt16(offset + 2, Math.round(tank.x * 2048), true);
      view.setInt16(offset + 4, Math.round(tank.z * 2048), true);
      view.setInt16(offset + 6, Math.round(wrapAngle(tank.yaw) * 8192), true);
      view.setInt16(offset + 8, Math.round(wrapAngle(tank.turret) * 8192), true);
      view.setUint8(offset + 10,
        Math.max(0, Math.min(255, Math.round(tank.secondaryCooldown * 16))));
      view.setUint8(offset + 11,
        Math.max(0, Math.min(255, Math.round(tank.gadgetCooldown * 8))));
      view.setUint8(offset + 12,
        Math.max(0, Math.min(255, Math.round(tank.respawn * 16))));
      view.setUint8(offset + 13,
        Math.max(0, Math.min(GADGETS.length - 1, GADGETS.indexOf(tank.gadget))));
      view.setUint16(offset + 14, tank.maxHealth, true);
      offset += ONLINE_TANK_BYTES;
    }
    let written = 0, activeBullets = bulletActiveMask;
    while (activeBullets && written < bulletCount) {
      const bulletAt = activeMaskIndex(activeBullets);
      activeBullets = (activeBullets & (activeBullets - 1)) >>> 0;
      const bullet = bullets[bulletAt];
      view.setUint8(offset, bullet.owner);
      view.setUint8(offset + 1,
        (bullet.bounces & 0x7f) | (bullet.overCover ? 0x80 : 0));
      view.setInt16(offset + 2, Math.round(bullet.x * 2048), true);
      view.setInt16(offset + 4, Math.round(bullet.z * 2048), true);
      view.setInt16(offset + 6, Math.round(bullet.vx * 1024), true);
      view.setInt16(offset + 8, Math.round(bullet.vz * 1024), true);
      view.setUint16(offset + 10,
        Math.max(0, Math.min(65535, Math.round(bullet.life * 1024))), true);
      offset += ONLINE_BULLET_BYTES;
      written++;
    }
    sendOnlinePacket(onlineSnapshotPackets[bulletCount]);
  }

  function applyOnlineSnapshot(buffer) {
    if (!(buffer instanceof ArrayBuffer)
        || buffer.byteLength < ONLINE_SNAPSHOT_HEADER_BYTES) return false;
    const view = new DataView(buffer);
    if (view.getUint8(0) !== 2 || view.getUint8(1) !== ONLINE_PROTOCOL_VERSION
        || view.getUint8(6) !== MAX_TANKS
        || view.getUint8(7) > ONLINE_BULLET_LIMIT) return false;
    const expected = ONLINE_SNAPSHOT_HEADER_BYTES
      + MAX_TANKS * ONLINE_TANK_BYTES
      + view.getUint8(7) * ONLINE_BULLET_BYTES;
    if (buffer.byteLength !== expected) return false;
    const arena = view.getUint8(4);
    const arenaSeed = view.getUint32(24, true);
    const arenaChecksum = view.getUint16(28, true);
    if (arena !== ONLINE_GENERATED_ARENA && arena >= AUTHORED_ARENA_COUNT)
      return false;
    if (arena === ONLINE_GENERATED_ARENA && !arenaSeed) return false;
    const sequence = view.getUint16(2, true);
    if (online.snapshotValid
        && !sequenceIsNewer(sequence, online.snapshotSequence)) return false;
    online.snapshotSequence = sequence;
    online.snapshotValid = true;
    online.lastReceive = state.time;
    if (arena !== online.arenaWire || arenaSeed !== online.arenaSeed
        || arenaChecksum !== online.arenaChecksum) {
      online.arenaWire = arena; online.arenaSeed = arenaSeed;
      online.arenaChecksum = arenaChecksum;
      online.arenaRejected = false;
      if (arena === ONLINE_GENERATED_ARENA) {
        const localChecksum = arenaGenerator.materialize(arenaSeed);
        if (localChecksum !== arenaChecksum) {
          online.arenaRejected = true;
          onlineArenaGeometryPending = false;
          state.arenaSeed = 0; arenaGenerationChecksum = 0;
          beginArena(0);
          showToast("ARENA VERSION MISMATCH", 2);
        } else {
          state.arenaSeed = arenaSeed;
          arenaGenerationChecksum = localChecksum;
          arenaGenerationAttempts = 1; arenaGenerationFallback = false;
          fillArenaSpatialData(GENERATED_ARENA_INDEX);
          generatedGeometryDirty = true;
          /* Materialization is one bounded candidate pass. Defer the GL tail
             rebuild to the next update while snapshots continue to refresh
             the just-initialized simulation records underneath it. */
          beginArena(GENERATED_ARENA_INDEX, true);
          onlineArenaGeometryPending = true;
          showToast("SYNCING ARENA", .75);
        }
      } else if (arena !== state.arena) {
        onlineArenaGeometryPending = false;
        beginArena(arena);
      }
    }
    state.score = view.getUint32(8, true);
    state.lives = view.getUint8(12);
    state.blueControl = view.getUint8(13);
    state.redControl = view.getUint8(14);
    const barrierMask = view.getUint16(30, true);
    for (let at = 0; !online.arenaRejected
         && at < barriers.length && at < 16; at++) {
      const active = barriers[at].present && !!(barrierMask & (1 << at));
      if (barriers[at].active !== active) retainedSceneryDirty = true;
      barriers[at].active = active;
    }
    const snapshotGateOpen = !!view.getUint8(15);
    if (snapshotGateOpen !== state.gateOpen) retainedSceneryDirty = true;
    state.gateOpen = snapshotGateOpen;
    updateRayActiveMask();
    convoy.health = view.getUint16(16, true);
    convoy.progress = view.getUint16(18, true) / 65535;
    let offset = ONLINE_SNAPSHOT_HEADER_BYTES;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at], flags = view.getUint8(offset);
      tank.active = !!(flags & 1);
      tank.team = flags & 2 ? 1 : 0;
      tank.classId = (flags >> 2) & 3;
      tank.health = view.getUint8(offset + 1);
      const nextX = view.getInt16(offset + 2, true) / 2048;
      const nextZ = view.getInt16(offset + 4, true) / 2048;
      const nextYaw = view.getInt16(offset + 6, true) / 8192;
      const nextTurret = view.getInt16(offset + 8, true) / 8192;
      if (at === 1 && tank.active) {
        tank.x += (nextX - tank.x) * .55;
        tank.z += (nextZ - tank.z) * .55;
        tank.yaw = approachAngle(tank.yaw, nextYaw, .6);
        tank.turret = wrapAngle(approachAngle(tank.turret, nextTurret, .75));
      } else {
        tank.x = nextX; tank.z = nextZ;
        tank.yaw = nextYaw; tank.turret = nextTurret;
      }
      updateTankYawCache(tank);
      updateTankTurretCache(tank);
      tank.secondaryCooldown = view.getUint8(offset + 10) / 16;
      tank.gadgetCooldown = view.getUint8(offset + 11) / 8;
      tank.respawn = view.getUint8(offset + 12) / 16;
      tank.gadget = GADGETS[Math.min(
        GADGETS.length - 1, view.getUint8(offset + 13))];
      tank.maxHealth = view.getUint16(offset + 14, true) || 1;
      tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
      offset += ONLINE_TANK_BYTES;
    }
    for (const bullet of bullets) bullet.active = false;
    bulletActiveMask = 0;
    const bulletCount = view.getUint8(7);
    for (let at = 0; at < bulletCount; at++) {
      const bullet = bullets[at];
      setBulletActive(at, true);
      bullet.owner = view.getUint8(offset);
      const bulletFlags = view.getUint8(offset + 1);
      bullet.bounces = bulletFlags & 0x7f;
      bullet.overCover = !!(bulletFlags & 0x80);
      bullet.x = view.getInt16(offset + 2, true) / 2048;
      bullet.z = view.getInt16(offset + 4, true) / 2048;
      bullet.vx = view.getInt16(offset + 6, true) / 1024;
      bullet.vz = view.getInt16(offset + 8, true) / 1024;
      const headingLength = Math.hypot(bullet.vx, bullet.vz) || 1;
      bullet.speed = headingLength; bullet.pierceId = -1; bullet.turns = 0;
      bullet.headingSine = bullet.vx / headingLength;
      bullet.headingCosine = bullet.vz / headingLength;
      bullet.life = view.getUint16(offset + 10, true) / 1024;
      offset += ONLINE_BULLET_BYTES;
    }
    const mode = view.getUint8(5);
    const nextMode = mode === 1 ? "arena-clear"
      : mode === 2 ? "game-over" : mode === 3 ? "victory" : "playing";
    if (state.mode !== nextMode) setMode(nextMode);
    updateHud();
    return true;
  }

  function onlineTick() {
    if (!online.active || !online.channel
        || online.channel.readyState !== "open") return;
    if (state.time - online.lastReceive > ONLINE_PEER_TIMEOUT) {
      const channel = online.channel;
      online.disconnectReason = `No game packets received for ${ONLINE_PEER_TIMEOUT} seconds.`;
      onlineFail(online.disconnectReason);
      try { channel.close(4000, "peer timeout"); } catch (_) {}
      return;
    }
    if (online.role === "guest") {
      const interval = .05;
      if (state.time + 1e-6 - online.lastInputSend >= interval) {
        online.lastInputSend += interval;
        if (state.time - online.lastInputSend > interval)
          online.lastInputSend = state.time;
        sendOnlineInput();
      }
    } else if (online.role === "host") {
      const interval = 1 / 15;
      if (state.time + 1e-6 - online.lastSnapshotSend >= interval) {
        online.lastSnapshotSend += interval;
        if (state.time - online.lastSnapshotSend > interval)
          online.lastSnapshotSend = state.time;
        sendOnlineSnapshot();
      }
      if (state.time - online.lastReceive > .3) {
        const command = tanks[1].command;
        command.left = command.right = 0;
        command.reverse = command.fire = command.secondary = false;
        command.gadget = command.ultimate = false;
      }
    }
  }

  function keyAction(event) {
    const key = String(event.key || "");
    const code = String(event.code || "");
    const lower = key.length === 1 ? key.toLowerCase() : key;
    if (lower === "a" || code === "KeyA") return "a";
    if (lower === "d" || code === "KeyD") return "d";
    if (lower === "w" || code === "KeyW") return "w";
    if (lower === "s" || code === "KeyS") return "s";
    if (key === "Shift" || code === "ShiftLeft" || code === "ShiftRight") return "shift";
    if (key === "ArrowLeft") return preferences.controls !== 1
      ? "moveLeft" : "aimLeft";
    if (key === "ArrowRight") return preferences.controls !== 1
      ? "moveRight" : "aimRight";
    if (key === "ArrowUp") return preferences.controls !== 1
      ? "moveUp" : "aimUp";
    if (key === "ArrowDown") return preferences.controls !== 1
      ? "moveDown" : "aimDown";
    if (lower === "j" || code === "KeyJ" || code === "Numpad4") return "aimLeft";
    if (lower === "l" || code === "KeyL" || code === "Numpad6") return "aimRight";
    if (lower === "i" || code === "KeyI" || code === "Numpad8") return "aimUp";
    if (lower === "k" || code === "KeyK" || code === "Numpad2") return "aimDown";
    if (key === " " || key === "Spacebar" || code === "Space") return "fire";
    if (lower === "e" || code === "KeyE") return "secondary";
    if (lower === "f" || code === "KeyF") return "gadget";
    if (lower === "q" || code === "KeyQ") return "ultimate";
    if (lower === "z" || code === "KeyZ") return "pivotLeft";
    if (lower === "c" || code === "KeyC") return "pivotRight";
    if (lower === "p" || key === "Escape" || code === "KeyP") return "pause";
    return "";
  }

  addEventListener("keydown", (event) => {
    if (event.target && (event.target.tagName === "TEXTAREA" || event.target.tagName === "INPUT")) return;
    const action = keyAction(event);
    if (!action) return;
    input.keys[action] = true;
    if (!event.repeat) {
      if (action === "fire") input.fireQueued = true;
      else if (action === "secondary") input.secondaryQueued = true;
      else if (action === "gadget") input.gadgetQueued = true;
      else if (action === "ultimate") input.ultimateQueued = true;
      else if (action === "pause") input.pauseQueued = true;
    }
    setSource("keyboard");
    event.preventDefault();
  });
  addEventListener("keyup", (event) => {
    const action = keyAction(event);
    if (!action) return;
    input.keys[action] = false;
    event.preventDefault();
  });
  /* Tilefinch never blurs the window, and any focus-event listener makes it
     dispatch blur/focusout/focus/focusin through script on every D-pad
     focus move in the menus. */
  if (!navigator.tilefinch) addEventListener("blur", () => {
    input.keys = Object.create(null);
    input.lastFire = input.lastSecondary = input.lastGadget = false;
    input.lastUltimate = input.lastPause = false;
    input.fireQueued = input.secondaryQueued = input.gadgetQueued = false;
    input.ultimateQueued = input.pauseQueued = false;
    resetAimAssist();
  });
  addEventListener("gamepadconnected", updateControlHint);
  addEventListener("gamepaddisconnected", updateControlHint);
  canvas.addEventListener("pointermove", (event) => {
    const rectangle = canvas.getBoundingClientRect();
    if (!rectangle.width || !rectangle.height) return;
    input.pointerAimX = (event.clientX - rectangle.left) / rectangle.width * 2 - 1;
    input.pointerAimY = (event.clientY - rectangle.top) / rectangle.height * 2 - 1;
    input.pointerActive = true;
    setSource("pointer");
  });
  canvas.addEventListener("pointerdown", () => {
    setSource("pointer");
    input.keys.fire = true;
    input.fireQueued = true;
  });
  addEventListener("pointerup", () => { if (input.source === "pointer") input.keys.fire = false; });
  addEventListener("pointercancel", () => { if (input.source === "pointer") input.keys.fire = false; });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden && state.mode === "playing") setMode("hidden");
    else if (!document.hidden && state.mode === "hidden") {
      state.mode = "playing";
      lastTimestamp = 0;
    }
  });

  /* The first and last cell of a span on either axis, clamped to the grid.
     Build-time fills share these; per-frame casts inline the same math. */
  function gridFirst(low) {
    return Math.max(0, Math.floor((low + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE));
  }
  function gridLast(high) {
    return Math.min(COLLISION_GRID_LAST,
      Math.floor((high + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE));
  }

  function fillCircleCandidateRect(grid, at, left, right, top, bottom) {
    const firstX = gridFirst(left - CIRCLE_GRID_RADIUS);
    const lastX = gridLast(right + CIRCLE_GRID_RADIUS);
    const firstZ = gridFirst(top - CIRCLE_GRID_RADIUS);
    const lastZ = gridLast(bottom + CIRCLE_GRID_RADIUS);
    for (let z = firstZ; z <= lastZ; z++)
      for (let x = firstX; x <= lastX; x++)
        grid[z * COLLISION_GRID_SIDE + x] |= 1 << at;
  }

  function fillBarrierCircleGrid() {
    // Rebuilt after final crate placement; never expose a partially replaced
    // arena's union to queries during setup.
    tinyCircleArena = -1;
    aimGeometryEpoch++;
    rayGeometryArena = -1;
    barrierCircleGrid.fill(0);
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (!barrier.present) continue;
      fillCircleCandidateRect(barrierCircleGrid, at,
        barrier.left, barrier.right, barrier.top, barrier.bottom);
    }
  }

  /* The obstacle and gate shell grids grow rectangles by .1; dynamic ones
     (crates, barriers) round outward just past that, so a bound on a
     half-cell boundary still marks the cell beyond. */
  const SHELL_GRID_PADDING = .100000001;
  function fillTinyCircleOccupiedRect(left, right, top, bottom) {
    const firstX = gridFirst(left - SHELL_GRID_PADDING);
    const lastX = gridLast(right + SHELL_GRID_PADDING);
    const firstZ = gridFirst(top - SHELL_GRID_PADDING);
    const lastZ = gridLast(bottom + SHELL_GRID_PADDING);
    for (let z = firstZ; z <= lastZ; z++)
      for (let x = firstX; x <= lastX; x++)
        tinyCircleOccupied[z * COLLISION_GRID_SIDE + x] = 1;
  }

  function fillTinyCircleOccupied() {
    tinyCircleArena = -1;
    if (!crateGridReady) return;
    const obstacles = ARENA_BULLET_OBSTACLE_GRIDS[state.arena];
    const gates = ARENA_BULLET_GATE_GRIDS[state.arena];
    for (let cell = 0; cell < COLLISION_GRID_CELLS; cell++)
      tinyCircleOccupied[cell] = obstacles[cell] || gates[cell] ? 1 : 0;
    // Keep inactive positions too: reactivation is safe without refreshing
    // the union. The exact predicate checks current active flags and gate.
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      fillTinyCircleOccupiedRect(crate.x - CRATE_HALF, crate.x + CRATE_HALF,
        crate.z - CRATE_HALF, crate.z + CRATE_HALF);
    }
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      fillTinyCircleOccupiedRect(barrier.left, barrier.right,
        barrier.top, barrier.bottom);
    }
    tinyCircleArena = state.arena;
  }

  /* Does a circle overlap scenery? The candidate grids reach
     CIRCLE_GRID_RADIUS past each rectangle; a larger circle, or fullScan,
     tests every wall, gate, barrier and crate instead. */
  function circleHitsObstacle(x, z, radius, fullScan = false) {
    const radiusSquared = radius * radius;
    const scanAll = fullScan || radius > CIRCLE_GRID_RADIUS;
    const cellX = (x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    const cellZ = (z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    // Truncate only inside the grid, where coordinates are nonnegative.
    const cell = cellX >= 0 && cellX < COLLISION_GRID_SIDE
      && cellZ >= 0 && cellZ < COLLISION_GRID_SIDE
      ? (cellZ | 0) * COLLISION_GRID_SIDE + (cellX | 0) : -1;
    let mask = !crateGridReady || scanAll || cell < 0
      ? (1 << crates.length) - 1 : crateCircleGrid[cell];
    for (let at = 0; mask && at < crates.length; at++, mask >>>= 1) {
      if (!(mask & 1)) continue;
      const crate = crates[at];
      if (!crate.active) continue;
      const deltaX = x - crate.x;
      let dx = (deltaX < 0 ? -deltaX : deltaX) - CRATE_HALF;
      if (dx > radius) continue;
      const deltaZ = z - crate.z;
      let dz = (deltaZ < 0 ? -deltaZ : deltaZ) - CRATE_HALF;
      if (dz > radius) continue;
      if (dx < 0) dx = 0;
      if (dz < 0) dz = 0;
      if (dx * dx + dz * dz < radiusSquared) return true;
    }
    if (cell < 0) return false;
    const arena = state.arena;
    const obstacles = ARENA_OBSTACLE_BOUNDS[arena];
    mask = scanAll ? -1 : ARENA_CIRCLE_OBSTACLE_GRIDS[arena][cell];
    for (let at = 0;
         mask && at < ARENA_OBSTACLE_COUNTS[arena] * 4;
         at += 4, mask >>>= 1) {
      if (!(mask & 1)) continue;
      const left = obstacles[at], right = obstacles[at + 1];
      const top = obstacles[at + 2], bottom = obstacles[at + 3];
      const dx = x < left ? x - left : x > right ? x - right : 0;
      const dz = z < top ? z - top : z > bottom ? z - bottom : 0;
      if (dx * dx + dz * dz < radiusSquared) return true;
    }
    mask = scanAll ? (1 << MAX_BARRIERS) - 1 : barrierCircleGrid[cell];
    for (let barrierAt = 0; mask && barrierAt < MAX_BARRIERS;
         barrierAt++, mask >>>= 1) {
      if (!(mask & 1)) continue;
      const barrier = barriers[barrierAt];
      if (!barrier.active) continue;
      const dx = x < barrier.left ? x - barrier.left
        : x > barrier.right ? x - barrier.right : 0;
      if (dx < -radius || dx > radius) continue;
      const dz = z < barrier.top ? z - barrier.top
        : z > barrier.bottom ? z - barrier.bottom : 0;
      if (dz < -radius || dz > radius) continue;
      if (dx * dx + dz * dz < radiusSquared) return true;
    }
    if (!state.gateOpen) {
      const gates = ARENA_GATE_BOUNDS[arena];
      mask = scanAll ? -1 : ARENA_CIRCLE_GATE_GRIDS[arena][cell];
      for (let at = 0;
           mask && at < ARENA_GATE_COUNTS[arena] * 4;
           at += 4, mask >>>= 1) {
        if (!(mask & 1)) continue;
        const left = gates[at], right = gates[at + 1];
        const top = gates[at + 2], bottom = gates[at + 3];
        const dx = x < left ? x - left : x > right ? x - right : 0;
        const dz = z < top ? z - top : z > bottom ? z - bottom : 0;
        if (dx * dx + dz * dz < radiusSquared) return true;
      }
    }
    return false;
  }

  function bulletHitsRectangles(x, z, bounds, count, mask) {
    const radiusSquared = .01;
    for (let rectangle = 0; mask && rectangle < count;
         rectangle++, mask >>>= 1) {
      if (!(mask & 1)) continue;
      const at = rectangle * 4;
      const dx = x < bounds[at] ? x - bounds[at]
        : x > bounds[at + 1] ? x - bounds[at + 1] : 0;
      const dz = z < bounds[at + 2] ? z - bounds[at + 2]
        : z > bounds[at + 3] ? z - bounds[at + 3] : 0;
      if (dx * dx + dz * dz < radiusSquared) return true;
    }
    return false;
  }

  function bulletHitsObstacle(x, z) {
    const cellX = (x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    const cellZ = (z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    if (!(cellX >= 0 && cellX < COLLISION_GRID_SIDE
        && cellZ >= 0 && cellZ < COLLISION_GRID_SIDE)) return false;
    const arena = state.arena;
    const cell = (cellZ | 0) * COLLISION_GRID_SIDE + (cellX | 0);
    if (bulletHitsRectangles(x, z, ARENA_OBSTACLE_BOUNDS[arena],
        ARENA_OBSTACLE_COUNTS[arena],
        ARENA_BULLET_OBSTACLE_GRIDS[arena][cell])) return true;
    return !state.gateOpen && bulletHitsRectangles(
      x, z, ARENA_GATE_BOUNDS[arena], ARENA_GATE_COUNTS[arena],
      ARENA_BULLET_GATE_GRIDS[arena][cell]);
  }

  function barrierAt(x, z, radius) {
    for (let barrierIndex = 0; barrierIndex < barriers.length;
         barrierIndex++) {
      const barrier = barriers[barrierIndex];
      if (!barrier.active) continue;
      if (x + radius >= barrier.left && x - radius <= barrier.right
          && z + radius >= barrier.top && z - radius <= barrier.bottom)
        return barrier;
    }
    return null;
  }

  function surfaceHeightAt(x, z) {
    const cellX = Math.floor((x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE);
    if (cellX < 0 || cellX >= COLLISION_GRID_SIDE
        || cellZ < 0 || cellZ >= COLLISION_GRID_SIDE) return 0;
    const rampIndex = ARENA_RAMP_GRIDS[state.arena][
      cellZ * COLLISION_GRID_SIDE + cellX] - 1;
    if (rampIndex < 0) return 0;
    const ramp = ARENAS[state.arena].ramps[rampIndex];
    if (Math.abs(x - ramp[0]) > ramp[2] * .5
        || Math.abs(z - ramp[1]) > ramp[3] * .5) return 0;
    const local = (z - (ramp[1] - ramp[3] * .5)) / ramp[3];
    const progress = ramp[5] > 0 ? local : 1 - local;
    return Math.max(0, Math.min(ramp[4], progress * ramp[4]));
  }

  function elevatedFiringOrigin(tank) {
    // All authored/generated ramps have positive height. Ground-level
    // tanks cannot clear cover, and need no repeated ramp-grid lookup.
    if (tank.surfaceY <= 0) return false;
    const cellX = Math.floor((tank.x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((tank.z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE);
    if (cellX < 0 || cellX >= COLLISION_GRID_SIDE
        || cellZ < 0 || cellZ >= COLLISION_GRID_SIDE) return false;
    const rampIndex = ARENA_RAMP_GRIDS[state.arena][
      cellZ * COLLISION_GRID_SIDE + cellX] - 1;
    if (rampIndex < 0) return false;
    const ramp = ARENAS[state.arena].ramps[rampIndex];
    return tank.surfaceY >= ramp[4] * .6;
  }

  function lineCrossesSmoke(ax, az, bx, bz) {
    if (activeSmokeCount === 0) return false;
    const dx = bx - ax, dz = bz - az;
    const lengthSquared = dx * dx + dz * dz;
    for (let smokeAt = 0; smokeAt < MAX_SMOKE; smokeAt++) {
      const smoke = smokeClouds[smokeAt];
      if (!smoke.active) continue;
      let along = 0;
      if (lengthSquared > .0001) {
        along = ((smoke.x - ax) * dx + (smoke.z - az) * dz)
          / lengthSquared;
        along = Math.max(0, Math.min(1, along));
      }
      const nearestX = ax + dx * along, nearestZ = az + dz * along;
      const smokeDx = smoke.x - nearestX, smokeDz = smoke.z - nearestZ;
      if (smokeDx * smokeDx + smokeDz * smokeDz < 3.1) return true;
    }
    return false;
  }

  function segmentHitsBox(ax, az, bx, bz, left, right, top, bottom,
                          padding = .08) {
    left -= padding; right += padding; top -= padding; bottom += padding;
    // Most sight-line boxes are nowhere near the segment. Reject them before
    // testing the remaining separating axis.
    if ((ax < left && bx < left) || (ax > right && bx > right)
        || (az < top && bz < top) || (az > bottom && bz > bottom)) return false;
    const dx = bx - ax, dz = bz - az;
    const absDx = dx < 0 ? -dx : dx, absDz = dz < 0 ? -dz : dz;
    // Separating-axis test: the segment's x/z intervals overlap above; its
    // normal is the only remaining separating axis. Doubled center/extents
    // remove division entirely on Allegrex without approximating the ray.
    // Preserve the original near-axis slab convention for tiny deltas.
    if (absDx < .00001) return ax >= left && ax <= right
      && (absDz >= .00001 || (az >= top && az <= bottom));
    if (absDz < .00001) return az >= top && az <= bottom;
    const centerX = left + right - ax - bx;
    const centerZ = top + bottom - az - bz;
    const cross = dx * centerZ - dz * centerX;
    return (cross < 0 ? -cross : cross)
      <= absDz * (right - left) + absDx * (bottom - top);
  }

  function refreshSceneryHeights() {
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      crate.renderY = surfaceHeightAt(crate.x, crate.z) + .25;
    }
    for (let at = 0; at < hazards.length; at++) {
      const hazard = hazards[at];
      hazard.renderY = surfaceHeightAt(hazard.x, hazard.z)
        + MARK_TOP + hazard.level * FLAT_STEP - .01;
    }
  }

  function updateHazardGrid(index, present) {
    const hazard = hazards[index];
    if (index >= MAX_HAZARDS || !Number.isFinite(hazard.x) || !Number.isFinite(hazard.z)
        || !Number.isFinite(hazard.radius) || hazard.radius < 0 || hazard.radius > 2)
      return false;
    // Outward padding protects rounded half-cell boundaries; the circle test
    // below is still exact. Expired slots may remain conservative candidates.
    const radius = hazard.radius + .000000001;
    const firstX = gridFirst(hazard.x - radius);
    const lastX = gridLast(hazard.x + radius);
    const firstZ = gridFirst(hazard.z - radius);
    const lastZ = gridLast(hazard.z + radius);
    const bit = 1 << index;
    for (let z = firstZ; z <= lastZ; z++)
      for (let x = firstX; x <= lastX; x++) {
        const cell = z * COLLISION_GRID_SIDE + x;
        if (present) hazardGrid[cell] |= bit;
        else hazardGrid[cell] &= ~bit;
      }
    return true;
  }

  function spawnHazard(type, x, z, radius, life) {
    const index = hazardCursor, hazard = hazards[index];
    updateHazardGrid(index, false);
    hazardCursor = (hazardCursor + 1) % hazards.length;
    hazard.active = false;
    hazard.level = Math.max(0, groundMarkLevel(x, z, radius * 1.42));
    hazard.active = true; hazard.type = type;
    hazard.x = x; hazard.z = z; hazard.radius = radius; hazard.life = life;
    hazard.renderY = surfaceHeightAt(x, z) + MARK_TOP + hazard.level * FLAT_STEP - .01;
    hazard.renderDiameter = radius * 2;
    if (updateHazardGrid(index, true)) hazardGridFallbackMask &= ~(1 << index);
    else hazardGridFallbackMask |= 1 << index;
  }

  function initializeArenaHazards() {
    tinyCircleArena = -1;
    rayGeometryArena = -1;
    // New scenery is offline-only until its state has snapshot fields.
    crateCircleGrid.fill(0); crateGridReady = false;
    if (online.active) {
      crateGridReady = true;
      fillTinyCircleOccupied();
      fillRayGeometry();
      prepareNavigationGrids();
      return;
    }
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      for (let probe = 0; probe < 32; probe++) {
        const slot = probe + at * 7 + state.arena * 11;
        const x = ((slot * 5) & 15) - 7.5;
        const z = ((slot * 9 + 3) & 15) - 7.5;
        if (Math.abs(x) > CRATE_PLACE_EDGE || Math.abs(z) > CRATE_PLACE_EDGE
            || circleHitsObstacle(x, z, .7)) continue;
        let clear = true;
        for (let tankAt = 0; tankAt < MAX_TANKS; tankAt++) {
          const tank = tanks[tankAt], dx = tank.x - x, dz = tank.z - z;
          if (tank.active && dx * dx + dz * dz < 2.25) { clear = false; break; }
        }
        if (!clear) continue;
        crate.active = true; crate.x = x; crate.z = z; crate.type = at % 3;
        crate.renderY = surfaceHeightAt(x, z) + .25;
        if (at < 2) spawnHazard(2, x + (x < 0 ? .5 : -.5), z, .85, -1);
        break;
      }
    }
    for (let at = 0; at < crates.length; at++) {
      const c = crates[at];
      if (c.active) fillCircleCandidateRect(crateCircleGrid, at,
        c.x - CRATE_HALF, c.x + CRATE_HALF, c.z - CRATE_HALF, c.z + CRATE_HALF);
    }
    crateGridReady = true;
    fillTinyCircleOccupied();
    fillRayGeometry();
    prepareNavigationGrids();
    invalidateBotNavigation();
  }

  function hazardAt(x, z) {
    const cellX = (x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    const cellZ = (z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    if (hazardGridFallbackMask
        || !(cellX >= 0 && cellX < COLLISION_GRID_SIDE
          && cellZ >= 0 && cellZ < COLLISION_GRID_SIDE))
      return hazardAtFallback(x, z);
    let mask = hazardGrid[(cellZ | 0) * COLLISION_GRID_SIDE + (cellX | 0)];
    while (mask) {
      // Preserve highest-slot precedence for overlapping terrain types.
      const at = 31 - Math.clz32(mask);
      mask &= ~(1 << at);
      const hazard = hazards[at];
      if (!hazard.active) continue;
      const dx = x - hazard.x, dz = z - hazard.z;
      if (dx * dx + dz * dz < hazard.radius * hazard.radius) return hazard.type;
    }
    return -1;
  }

  function hazardAtFallback(x, z) {
    for (let at = hazards.length - 1; at >= 0; at--) {
      const hazard = hazards[at];
      if (!hazard.active) continue;
      const dx = x - hazard.x, dz = z - hazard.z;
      if (dx * dx + dz * dz < hazard.radius * hazard.radius) return hazard.type;
    }
    return -1;
  }

  function explodeCrate(index, owner, depth) {
    const crate = crates[index];
    if (!crate.active || depth >= crates.length) return;
    crate.active = false; invalidateBotNavigation();
    spawnHazard(crate.type === 2 ? 0 : 1, crate.x, crate.z, 1.25, 18);
    spawnParticles(crate.x, .35, crate.z, 4, 2.2);
    spawnDecal(2, crate.x, crate.z, 0, .8, .8);
    sounds.explosion(sounds.blast.crate);
    if (crate.type !== 1) return;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at], dx = tank.x - crate.x, dz = tank.z - crate.z;
      if (tank.active && dx * dx + dz * dz < 3.2) damageTank(tank, 32, owner, crate.x, crate.z);
    }
    for (let at = 0; at < crates.length; at++) {
      const other = crates[at], dx = other.x - crate.x, dz = other.z - crate.z;
      if (other.active && dx * dx + dz * dz < 4) explodeCrate(at, owner, depth + 1);
    }
  }

  function updateHazards(dt) {
    for (let at = 0; at < hazards.length; at++) {
      const hazard = hazards[at];
      if (!hazard.active || hazard.life < 0) continue;
      hazard.life -= dt; if (hazard.life <= 0) hazard.active = false;
    }
  }

  function updateDuel(dt) {
    if (state.mode !== "playing") return;
    if (!state.duelResolving) state.duelTime -= dt;
    if ((state.duelResolving && !bulletActiveMask) || state.duelTime <= 0) {
      state.duelTurn ^= 1; state.duelTime = 12; state.duelResolving = false;
      for (let at = 0; at < 2; at++) {
        const tank = tanks[at];
        tank.command.left = tank.command.right = 0;
        tank.command.fire = tank.command.secondary = tank.command.gadget = tank.command.ultimate = false;
        tank.fireHeld = false; tank.fireCharge = 0;
      }
      setMode("duel-pass");
    }
  }

  function lineCrossesWallsFallback(ax, az, bx, bz, overCover = false, padding = .08) {
    // Most boxes miss the ray's bounding interval. Reject them in this
    // loop before pushing the full segment-test argument frame; Allegrex
    // pays that interpreted call even when the helper immediately declines.
    const minX = ax < bx ? ax : bx, maxX = ax > bx ? ax : bx;
    const minZ = az < bz ? az : bz, maxZ = az > bz ? az : bz;
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      if (crate.active && maxX >= crate.x - CRATE_HALF - padding
          && minX <= crate.x + CRATE_HALF + padding
          && maxZ >= crate.z - CRATE_HALF - padding
          && minZ <= crate.z + CRATE_HALF + padding
          && segmentHitsBox(ax, az, bx, bz, crate.x - CRATE_HALF,
          crate.x + CRATE_HALF, crate.z - CRATE_HALF, crate.z + CRATE_HALF, padding)) return true;
    }
    const bounds = ARENA_OBSTACLE_BOUNDS[state.arena];
    for (let at = 0; at < ARENA_OBSTACLE_COUNTS[state.arena] * 4; at += 4)
      if (maxX >= bounds[at] - padding && minX <= bounds[at + 1] + padding
          && maxZ >= bounds[at + 2] - padding && minZ <= bounds[at + 3] + padding
          && segmentHitsBox(ax, az, bx, bz, bounds[at], bounds[at + 1],
          bounds[at + 2], bounds[at + 3], padding)) return true;
    if (!overCover) for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (barrier.active && maxX >= barrier.left - padding
          && minX <= barrier.right + padding && maxZ >= barrier.top - padding
          && minZ <= barrier.bottom + padding && segmentHitsBox(ax, az, bx, bz,
          barrier.left, barrier.right, barrier.top, barrier.bottom, padding))
        return true;
    }
    if (!state.gateOpen) {
      const gates = ARENA_GATE_BOUNDS[state.arena];
      for (let at = 0; at < ARENA_GATE_COUNTS[state.arena] * 4; at += 4)
        if (maxX >= gates[at] - padding && minX <= gates[at + 1] + padding
            && maxZ >= gates[at + 2] - padding && minZ <= gates[at + 3] + padding
            && segmentHitsBox(ax, az, bx, bz, gates[at], gates[at + 1],
            gates[at + 2], gates[at + 3], padding)) return true;
    }
    return false;
  }

  function setRayGeometryRect(id, left, right, top, bottom) {
    // Round outwards as the tiny-circle grid does; the cast decides contact.
    const firstX = gridFirst(left - SHELL_GRID_PADDING);
    const lastX = gridLast(right + SHELL_GRID_PADDING);
    const firstZ = gridFirst(top - SHELL_GRID_PADDING);
    const lastZ = gridLast(bottom + SHELL_GRID_PADDING);
    for (let z = firstZ; z <= lastZ; z++)
      for (let x = firstX; x <= lastX; x++)
        shellCellMasks[z * COLLISION_GRID_SIDE + x] |= 1 << id;
    const at = id * 4;
    rayBounds[at] = left; rayBounds[at + 1] = right;
    rayBounds[at + 2] = top; rayBounds[at + 3] = bottom;
    const box = id * 8;
    const grownLeft = rayBounds[at] - RAY_AIM_PADDING, grownRight = rayBounds[at + 1] + RAY_AIM_PADDING;
    const grownTop = rayBounds[at + 2] - RAY_AIM_PADDING, grownBottom = rayBounds[at + 3] + RAY_AIM_PADDING;
    rayAimBoxes[box] = grownLeft; rayAimBoxes[box + 1] = grownRight;
    rayAimBoxes[box + 2] = grownTop; rayAimBoxes[box + 3] = grownBottom;
    rayAimBoxes[box + 4] = grownLeft + grownRight; rayAimBoxes[box + 5] = grownTop + grownBottom;
    rayAimBoxes[box + 6] = grownRight - grownLeft; rayAimBoxes[box + 7] = grownBottom - grownTop;
    for (let axis = 0; axis < 2; axis++) {
      const low = axis ? top : left, high = axis ? bottom : right;
      const first = Math.max(0, gridLast(low));
      const last = Math.max(0, gridLast(high));
      for (let cell = first; cell <= last; cell++)
        rayAxisMasks[axis * 32 + cell] |= 1 << id;
    }
  }

  function updateRayActiveMask() {
    if (rayGeometryArena !== state.arena) return;
    let mask = ((1 << ARENA_OBSTACLE_COUNTS[state.arena]) - 1) << 4;
    for (let at = 0; at < crates.length; at++)
      if (crates[at].active) mask |= 1 << at;
    for (let at = 0; at < MAX_BARRIERS; at++)
      if (barriers[at].active) mask |= 1 << (20 + at);
    rayActiveMask = mask >>> 0;
  }

  function fillRayGeometry() {
    rayGeometryArena = -1;
    aimGeometryEpoch++;
    const obstacleCount = ARENA_OBSTACLE_COUNTS[state.arena];
    const gateCount = ARENA_GATE_COUNTS[state.arena];
    // Keep the general predicate available if an arena grows past this
    // allocation, or while final dynamic placements are still being chosen.
    if (!crateGridReady || obstacleCount > 16 || gateCount > 6) return;
    rayAxisMasks.fill(0); rayBlockers.fill(-1); shellCellMasks.fill(0);
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      setRayGeometryRect(at, crate.x - CRATE_HALF, crate.x + CRATE_HALF,
        crate.z - CRATE_HALF, crate.z + CRATE_HALF);
    }
    const bounds = ARENA_OBSTACLE_BOUNDS[state.arena];
    for (let at = 0; at < obstacleCount; at++)
      setRayGeometryRect(4 + at, bounds[at * 4], bounds[at * 4 + 1],
        bounds[at * 4 + 2], bounds[at * 4 + 3]);
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      setRayGeometryRect(20 + at, barrier.left, barrier.right,
        barrier.top, barrier.bottom);
    }
    const gates = ARENA_GATE_BOUNDS[state.arena];
    for (let at = 0; at < gateCount; at++)
      setRayGeometryRect(26 + at, gates[at * 4], gates[at * 4 + 1],
        gates[at * 4 + 2], gates[at * 4 + 3]);
    for (let axis = 0; axis < 2; axis++) for (let first = 0; first < 32; first++) {
      let mask = 0;
      for (let last = first; last < 32; last++) {
        mask |= rayAxisMasks[axis * 32 + last];
        rayIntervalMasks[axis * 1024 + first * 32 + last] = mask;
      }
    }
    rayGateMask = (((1 << gateCount) - 1) << 26) >>> 0;
    rayGeometryArena = state.arena;
    updateRayActiveMask();
  }

  function rayCandidateMask(minX, maxX, minZ, maxZ, active) {
    if (!(minX <= maxX && minZ <= maxZ) || minX < -8 || maxX >= 8
        || minZ < -8 || maxZ >= 8) return active;
    // An extra half-unit cell protects padded-edge rounding. This index can
    // admit extra boxes, but only the original predicate decides a collision.
    const origin = COLLISION_GRID_ORIGIN, scale = COLLISION_GRID_SCALE;
    const firstX = Math.max(0, (((minX + origin) * scale) | 0) - 1);
    const lastX = Math.min(COLLISION_GRID_LAST, (((maxX + origin) * scale) | 0) + 1);
    const firstZ = Math.max(0, (((minZ + origin) * scale) | 0) - 1);
    const lastZ = Math.min(COLLISION_GRID_LAST, (((maxZ + origin) * scale) | 0) + 1);
    return (rayIntervalMasks[firstX * 32 + lastX]
      & rayIntervalMasks[1024 + firstZ * 32 + lastZ] & active) >>> 0;
  }

  function lineCrossesWalls(ax, az, bx, bz, overCover = false, padding = .08,
                           blockerSlot = -1) {
    const dx = bx - ax, dz = bz - az;
    if (rayGeometryArena !== state.arena || dx !== dx || dz !== dz
        || padding !== padding)
      return lineCrossesWallsFallback(ax, az, bx, bz, overCover, padding);
    const minX = ax < bx ? ax : bx, maxX = ax > bx ? ax : bx;
    const minZ = az < bz ? az : bz, maxZ = az > bz ? az : bz;
    const absDx = dx < 0 ? -dx : dx, absDz = dz < 0 ? -dz : dz;
    let active = (rayActiveMask | (state.gateOpen ? 0 : rayGateMask)) >>> 0;
    if (overCover) active = (active & ~RAY_BARRIER_MASK) >>> 0;
    const retain = blockerSlot >= 0 && blockerSlot < rayBlockers.length;
    let prior = retain ? rayBlockers[blockerSlot] : -1;
    if (prior < 0 || !(active & (1 << prior))) prior = -1;
    let mask = 0, search = true, tested = -1;
    const aimPadding = padding === RAY_AIM_PADDING;
    while (prior >= 0 || search || mask) {
      let id;
      if (prior >= 0) {
        id = tested = prior; prior = -1;
      } else {
        if (search) {
          mask = rayCandidateMask(minX - padding, maxX + padding,
            minZ - padding, maxZ + padding, active);
          if (tested >= 0) mask = (mask & ~(1 << tested)) >>> 0;
          search = false;
        }
        if (!mask) break;
        id = 31 - Math.clz32(mask & -mask);
        mask = (mask & (mask - 1)) >>> 0;
      }
      let left, right, top, bottom, box = -1;
      if (aimPadding) {
        box = id * 8;
        left = rayAimBoxes[box]; right = rayAimBoxes[box + 1];
        top = rayAimBoxes[box + 2]; bottom = rayAimBoxes[box + 3];
      } else {
        const at = id * 4;
        left = rayBounds[at] - padding; right = rayBounds[at + 1] + padding;
        top = rayBounds[at + 2] - padding; bottom = rayBounds[at + 3] + padding;
      }
      if (!(maxX >= left && minX <= right && maxZ >= top && minZ <= bottom)) continue;
      // Keep the slab threshold, inclusive boundaries, and arithmetic order
      // identical to segmentHitsBox. Only direction preparation is shared.
      let hit;
      if (absDx < .00001) hit = ax >= left && ax <= right
        && (absDz >= .00001 || (az >= top && az <= bottom));
      else if (absDz < .00001) hit = az >= top && az <= bottom;
      else if (box >= 0) {
        const centerX = rayAimBoxes[box + 4] - ax - bx;
        const centerZ = rayAimBoxes[box + 5] - az - bz;
        const cross = dx * centerZ - dz * centerX;
        hit = (cross < 0 ? -cross : cross)
          <= absDz * rayAimBoxes[box + 6] + absDx * rayAimBoxes[box + 7];
      } else {
        const centerX = left + right - ax - bx;
        const centerZ = top + bottom - az - bz;
        const cross = dx * centerZ - dz * centerX;
        hit = (cross < 0 ? -cross : cross)
          <= absDz * (right - left) + absDx * (bottom - top);
      }
      if (hit) {
        // A retained ID only changes boolean-query order. Test it afresh
        // against this ray; never reuse a previous visibility answer.
        if (retain) rayBlockers[blockerSlot] = id;
        return true;
      }
    }
    if (retain) rayBlockers[blockerSlot] = -1;
    return false;
  }

  function invalidateBotNavigation() {
    updateRayActiveMask();
    aimGeometryEpoch++;
    restartBotNavigation();
  }

  /* (Kept here with the ray index it reads; bots.js routes with it.)
     Padding for a bot's route sight tests: its hull radius plus a margin,
     unless its centre already lies inside that padding around some box (a
     hull resting on a wall, or beside a corner, which the square padding
     reaches past the round hull). Every sight line would then fail, so
     routing gave up and the bot drove straight at its goal through the
     wall; test from where it stands with the padding that admits it. */
  function routePadding(tank) {
    const padding = tank.collisionRadius + .02, half = tank.collisionRadius * .5;
    const x = tank.x, z = tank.z;
    if (rayGeometryArena !== state.arena) {
      if (!lineCrossesWalls(x, z, x, z, false, padding)) return padding;
      return lineCrossesWalls(x, z, x, z, false, half) ? .05 : half;
    }
    // One pass over the ray index (lineCrossesWalls' point test, inclusive).
    let mask = rayCandidateMask(x - padding, x + padding, z - padding, z + padding,
      (rayActiveMask | (state.gateOpen ? 0 : rayGateMask)) >>> 0);
    let inside = false;
    while (mask) {
      const at = (31 - Math.clz32(mask & -mask)) * 4;
      mask = (mask & (mask - 1)) >>> 0;
      if (x < rayBounds[at] - padding || x > rayBounds[at + 1] + padding
          || z < rayBounds[at + 2] - padding || z > rayBounds[at + 3] + padding) continue;
      if (x >= rayBounds[at] - half && x <= rayBounds[at + 1] + half
          && z >= rayBounds[at + 2] - half && z <= rayBounds[at + 3] + half) return .05;
      inside = true;
    }
    return inside ? half : padding;
  }

  /* The objective (world mast and HUD arrow): the transmitter, the convoy,
     or the nearest of a set, live foes or a mission's own targets
     (campaign.objective offers barriers or memory cards, keys from 8).
     Nearest alone swapped between two foes at about the same distance
     every frame and flipped the arrow across the compass, so a target is
     kept until it is gone or another stays clearly nearer (within
     OBJECTIVE_SWITCH_RATIO of its squared distance, 80% of the distance)
     for OBJECTIVE_SWITCH_HOLD seconds. Presentation only: the simulation
     never reads it, and calls at one state.time agree. Returns
     OBJECTIVE_FIXED, OBJECTIVE_NEAREST or, with nothing to offer,
     OBJECTIVE_GATE (the arena's gate, which the Practice Range moves to
     its next slalom gate). */
  const OBJECTIVE_FIXED = 0, OBJECTIVE_NEAREST = 1, OBJECTIVE_GATE = 2;
  const OBJECTIVE_SWITCH_RATIO = .64, OBJECTIVE_SWITCH_HOLD = .5;
  let objectiveKey = -1, objectiveRival = -1, objectiveRivalSince = 0;
  // campaign.objective's last answer; -1 outside a mission.
  let objectiveMission = -1;
  let offerKey = -1, offerX = 0, offerZ = 0, offerDistance = Infinity;
  let offerHeldX = 0, offerHeldZ = 0, offerHeldDistance = Infinity;
  function offerObjective(key, x, z) {
    const player = playerTank(), dx = x - player.x, dz = z - player.z;
    const distance = dx * dx + dz * dz;
    if (key === objectiveKey) {
      offerHeldDistance = distance; offerHeldX = x; offerHeldZ = z;
    }
    if (distance < offerDistance) {
      offerDistance = distance; offerKey = key; offerX = x; offerZ = z;
    }
  }
  function objectivePosition(out) {
    offerKey = -1; offerDistance = offerHeldDistance = Infinity;
    objectiveMission = campaign.objective(offerObjective);
    if (objectiveMission !== 2) {
      if (state.gameMode === MODE_CONTROL) { out.x = 0; out.z = 0; return OBJECTIVE_FIXED; }
      if (state.gameMode === MODE_CONVOY && convoy.active) {
        out.x = convoy.x; out.z = convoy.z; return OBJECTIVE_FIXED;
      }
      const player = playerTank();
      for (let at = 0; at < MAX_TANKS; at++) {
        const tank = tanks[at];
        if (tank.active && tank.team !== player.team)
          offerObjective(at, tank.x, tank.z);
      }
    }
    if (offerKey < 0) {
      objectiveKey = objectiveRival = -1;
      if (practice.on) {
        // The range's marker: the next slalom gate or the lane.
        out.x = practice.markerX; out.z = practice.markerZ;
      } else {
        const gate = ARENAS[state.arena].gates[0];
        out.x = gate[0]; out.z = gate[1];
      }
      return OBJECTIVE_GATE;
    }
    if (offerHeldDistance === Infinity) {
      // Nothing held, or it is gone: the nearest at once.
      objectiveKey = offerKey; objectiveRival = -1;
    } else if (offerKey !== objectiveKey
        && offerDistance < offerHeldDistance * OBJECTIVE_SWITCH_RATIO) {
      if (objectiveRival !== offerKey || state.time < objectiveRivalSince) {
        objectiveRival = offerKey; objectiveRivalSince = state.time;
      }
      if (state.time - objectiveRivalSince >= OBJECTIVE_SWITCH_HOLD) {
        objectiveKey = offerKey; objectiveRival = -1;
      }
    } else objectiveRival = -1;
    if (objectiveKey === offerKey) { out.x = offerX; out.z = offerZ; }
    else { out.x = offerHeldX; out.z = offerHeldZ; }
    return OBJECTIVE_NEAREST;
  }

  /* Settings > Camera & Display > Objective arrow. Auto shows the HUD
     arrow only where the objective is not a foe that comes to you: Team
     Control's transmitter, the convoy, the other player in Pass the PSP,
     the Practice Range's next slalom gate and the missions that ask
     (campaign.objective). The world mast always shows. */
  function objectiveArrowShown(kind) {
    const level = preferences.objectiveArrow;
    if (level) return level === 1;
    if (objectiveMission >= 0) return objectiveMission > 0;
    if (practice.on) return kind === OBJECTIVE_GATE;
    return state.gameMode === MODE_CONTROL || state.gameMode === MODE_DUEL
      || (state.gameMode === MODE_CONVOY && kind === OBJECTIVE_FIXED);
  }

  const objectiveTarget = {x: 0, z: 0};

  /* -------------------------------------------------------- shell sweep --
     What a shell meets, in one implementation for updateBullets (every
     step of every shell) and the aim guide (the player's next shell): the
     arena edges at 7.85, walls and closed gates within .1 of their
     rectangles (a rounded box; the shell bounces), active barriers grown
     by .1 unless it flies over cover, and crates within .32 (both stop
     it). A step is a swept segment, not a sampled point: the first contact
     along it wins, a bounce turns the shell there and the rest of the step
     continues from the contact. No path can shave a convex corner between
     two samples, and where a shell goes does not depend on the frame rate.
     Allegrex does double arithmetic in software, so candidates are
     rejected by comparison before any slab math and slabs multiply rather
     than divide. */
  const SHELL_RANGE = 0, SHELL_BOUNCE = 1, SHELL_STOP = 2;
  const SHELL_RADIUS = .1;
  const SHELL_CHUNK = 2.5, SHELL_DIRECTION_EPSILON = 1e-9;
  /* A cast starting deeper than this inside a wall or gate is embedded (a
     gate closed over the shell), not resting on the face it just met. */
  const SHELL_EMBEDDED = 1e-9;
  /* Contacts one step resolves (bounces, a pierced barrier). A shell still
     turning after that many rests at its last contact until the next step. */
  const SHELL_MAX_CONTACTS = 4;
  let castLength = 0, castKind = SHELL_RANGE, castId = -1, castEdgeGap = Infinity;
  let castNormalX = 0, castNormalZ = 0, castArc = false, castEmbedded = false;
  let roundNormalX = 0, roundNormalZ = 0, roundEdgeGap = 0, roundEmbedded = false;
  /* Per cast: reciprocal direction (Infinity along an axis the ray does
     not move on), the live candidate set (fillRayGeometry's ids), one
     barrier id a piercing shell is passing through, and the bounds of the
     segment still in play, which shrink as nearer contacts are found. */
  let castInverseX = 0, castInverseZ = 0, castActive = 0, castSkip = -1;
  let castMinX = 0, castMaxX = 0, castMinZ = 0, castMaxZ = 0;
  function setCastBounds(ox, oz, dx, dz) {
    const ex = ox + dx * castLength, ez = oz + dz * castLength;
    castMinX = ox < ex ? ox : ex; castMaxX = ox > ex ? ox : ex;
    castMinZ = oz < ez ? oz : ez; castMaxZ = oz > ez ? oz : ez;
  }

  /* A shell flies along the 512-step heading table's direction made unit
     in double precision: the sweep's corner and hull math assume a unit
     ray, and the table's single-precision pair is only unit to 1e-7,
     enough to move a glancing corner contact by 1e-5 over a long leg.
     spawnShell and the aim guide both start from here. */
  let shellDirX = 0, shellDirZ = 1;
  function shellDirection(sine, cosine) {
    const inverse = 1 / Math.sqrt(sine * sine + cosine * cosine);
    shellDirX = sine * inverse; shellDirZ = cosine * inverse;
  }

  /* The live candidate set: walls, active crates and barriers (none over
     cover) and, while it is closed, the gate. With the ray index ready its
     active mask is kept current wherever a barrier or crate changes
     (invalidateBotNavigation, snapshots), as the bots' sight lines rely
     on. Without it the cast walks every record's live flags instead and
     this mask serves only as the aim guide's cache key. */
  function shellActiveMask(overCover) {
    const arena = state.arena;
    let active;
    if (rayGeometryArena === arena) {
      active = (rayActiveMask | (state.gateOpen ? 0 : rayGateMask)) >>> 0;
      if (overCover) active = (active & ~RAY_BARRIER_MASK) >>> 0;
      return active;
    }
    active = ((1 << ARENA_OBSTACLE_COUNTS[arena]) - 1) << 4;
    for (let at = 0; at < crates.length; at++)
      if (crates[at].active) active |= 1 << at;
    if (!overCover) for (let at = 0; at < MAX_BARRIERS; at++)
      if (barriers[at].active) active |= 1 << (20 + at);
    if (!state.gateOpen) active |= ((1 << ARENA_GATE_COUNTS[arena]) - 1) << 26;
    return active >>> 0;
  }

  /* Entry distance of the ray into a box already grown by the shell
     radius, Infinity on a miss; an origin inside the box meets it at 0. */
  function castBox(ox, oz, dx, dz, left, right, top, bottom, best) {
    let enter = 0, exit = best;
    if (dx > SHELL_DIRECTION_EPSILON || dx < -SHELL_DIRECTION_EPSILON) {
      let near = (left - ox) * castInverseX, far = (right - ox) * castInverseX;
      if (near > far) { const swap = near; near = far; far = swap; }
      if (near > enter) enter = near;
      if (far < exit) exit = far;
    } else if (ox < left || ox > right) return Infinity;
    if (dz > SHELL_DIRECTION_EPSILON || dz < -SHELL_DIRECTION_EPSILON) {
      let near = (top - oz) * castInverseZ, far = (bottom - oz) * castInverseZ;
      if (near > far) { const swap = near; near = far; far = swap; }
      if (near > enter) enter = near;
      if (far < exit) exit = far;
    } else if (oz < top || oz > bottom) return Infinity;
    return enter <= exit ? enter : Infinity;
  }

  /* Walls and gates: within .1 of the rectangle, a rounded box. The ray
     enters the grown box either on a flat face or in a corner square; a
     corner square is part of the shape only inside its quarter circle, and
     a ray that misses that circle leaves the grown box without touching
     the shape (it is convex). A ray starting on the surface it is leaving
     (the contact a bounce just turned at) meets nothing. Sets the outward
     normal at the contact. */
  function castRounded(ox, oz, dx, dz, left, right, top, bottom, best) {
    const radius = SHELL_RADIUS;
    let enter = -Infinity, exit = Infinity, enterX = true;
    if (dx > SHELL_DIRECTION_EPSILON || dx < -SHELL_DIRECTION_EPSILON) {
      let near = (left - radius - ox) * castInverseX, far = (right + radius - ox) * castInverseX;
      if (near > far) { const swap = near; near = far; far = swap; }
      enter = near; exit = far;
    } else if (ox <= left - radius + SHELL_EMBEDDED
        || ox >= right + radius - SHELL_EMBEDDED) return Infinity;
    if (dz > SHELL_DIRECTION_EPSILON || dz < -SHELL_DIRECTION_EPSILON) {
      let near = (top - radius - oz) * castInverseZ, far = (bottom + radius - oz) * castInverseZ;
      if (near > far) { const swap = near; near = far; far = swap; }
      if (near > enter) { enter = near; enterX = false; }
      if (far < exit) exit = far;
    } else if (oz <= top - radius + SHELL_EMBEDDED
        || oz >= bottom + radius - SHELL_EMBEDDED) return Infinity;
    if (enter > exit || exit <= 1e-6 || enter >= best) return Infinity;
    const embedded = enter < -SHELL_EMBEDDED;
    if (enter < 0) enter = 0;
    const px = ox + dx * enter, pz = oz + dz * enter;
    if (enterX ? pz >= top && pz <= bottom : px >= left && px <= right) {
      roundNormalX = enterX ? (dx > 0 ? -1 : 1) : 0;
      roundNormalZ = enterX ? 0 : (dz > 0 ? -1 : 1);
      // How far along the face its nearer end lies.
      roundEdgeGap = enterX ? Math.min(pz - top, bottom - pz)
        : Math.min(px - left, right - px);
      roundEmbedded = embedded;
      return enter;
    }
    roundEdgeGap = 0;
    const cornerX = px < left ? left : px > right ? right : px;
    const cornerZ = pz < top ? top : pz > bottom ? bottom : pz;
    const fromX = ox - cornerX, fromZ = oz - cornerZ;
    const along = fromX * dx + fromZ * dz;
    const outside = fromX * fromX + fromZ * fromZ - radius * radius;
    if (outside <= 0) {
      // On the arc (or embedded in it): leaving it is no contact.
      if (along >= 0 && outside > -SHELL_EMBEDDED) return Infinity;
      const length = Math.sqrt(fromX * fromX + fromZ * fromZ) || 1;
      roundNormalX = fromX / length; roundNormalZ = fromZ / length;
      roundEmbedded = outside <= -SHELL_EMBEDDED;
      return 0;
    }
    if (along >= 0) return Infinity;
    const discriminant = along * along - outside;
    if (discriminant < 0) return Infinity;
    const contact = -along - Math.sqrt(discriminant);
    if (contact >= best) return Infinity;
    roundNormalX = (ox + dx * contact - cornerX) / radius;
    roundNormalZ = (oz + dz * contact - cornerZ) / radius;
    roundEmbedded = false;
    return contact;
  }

  /* One candidate from the ray-geometry id space: 0-3 crates, 4-19 walls,
     20-25 barriers, 26-31 gates (see fillRayGeometry). */
  function castCandidate(id, ox, oz, dx, dz, left, right, top, bottom) {
    // A little slack keeps a stop that ties the current contact (it wins).
    if (left - SHELL_RADIUS > castMaxX + 1e-6 || right + SHELL_RADIUS < castMinX - 1e-6
        || top - SHELL_RADIUS > castMaxZ + 1e-6 || bottom + SHELL_RADIUS < castMinZ - 1e-6) return;
    if (id < 4 || (id >= 20 && id < 26)) {
      if (id === castSkip) return;
      const contact = castBox(ox, oz, dx, dz, left - SHELL_RADIUS,
        right + SHELL_RADIUS, top - SHELL_RADIUS, bottom + SHELL_RADIUS,
        castLength);
      // A stop that ties a bounce wins: the shell stops at the barrier.
      if (contact < castLength
          || (contact <= castLength + 1e-9 && castKind !== SHELL_STOP)) {
        castLength = contact; castKind = SHELL_STOP; castId = id;
        castArc = castEmbedded = false;
        setCastBounds(ox, oz, dx, dz);
      }
      return;
    }
    const contact = castRounded(ox, oz, dx, dz, left, right, top, bottom,
      castLength);
    if (!(contact < castLength)) return;
    // A tangent touch, or a shell already leaving the face, is no contact.
    if (dx * roundNormalX + dz * roundNormalZ > -SHELL_DIRECTION_EPSILON) return;
    castLength = contact; castKind = SHELL_BOUNCE; castId = id;
    castNormalX = roundNormalX; castNormalZ = roundNormalZ; castEdgeGap = roundEdgeGap;
    castArc = roundNormalX !== 0 && roundNormalZ !== 0;
    castEmbedded = roundEmbedded;
    setCastBounds(ox, oz, dx, dz);
  }

  /* First contact along a unit ray within maxLength. Edges bound the
     segment, so the walk asks the ray-geometry index for candidates one
     chunk at a time and stops once the nearest contact lies in a chunk
     already searched. Without a ready index it walks every record's live
     flags. castActive (shellActiveMask) holds the live candidate set and
     castSkip a barrier to pass through. */
  /* Start a cast: reciprocals, the arena edge (a bounce), the range. */
  function castBegin(ox, oz, dx, dz, maxLength) {
    const movesX = dx > SHELL_DIRECTION_EPSILON || dx < -SHELL_DIRECTION_EPSILON;
    const movesZ = dz > SHELL_DIRECTION_EPSILON || dz < -SHELL_DIRECTION_EPSILON;
    castInverseX = movesX ? 1 / dx : 0;
    castInverseZ = movesZ ? 1 / dz : 0;
    let edge = Infinity, edgeX = false, edgeZ = false;
    if (movesX) { edge = ((dx > 0 ? ARENA_EDGE : -ARENA_EDGE) - ox) * castInverseX; edgeX = true; }
    if (movesZ) {
      const along = ((dz > 0 ? ARENA_EDGE : -ARENA_EDGE) - oz) * castInverseZ;
      if (along < edge - 1e-9) { edge = along; edgeX = false; edgeZ = true; }
      else if (along <= edge + 1e-9) edgeZ = true;
    }
    if (edge < 0) edge = 0;
    castId = -1; castEdgeGap = Infinity; castArc = castEmbedded = false;
    if (edge <= maxLength) {
      castLength = edge; castKind = SHELL_BOUNCE;
      castNormalX = edgeX ? (dx > 0 ? -1 : 1) : 0;
      castNormalZ = edgeZ ? (dz > 0 ? -1 : 1) : 0;
    } else {
      castLength = maxLength; castKind = SHELL_RANGE;
    }
    setCastBounds(ox, oz, dx, dz);
  }
  /* Test the candidates of a ray-id mask against the cast in progress. */
  function castCandidates(mask, ox, oz, dx, dz) {
    while (mask) {
      const id = 31 - Math.clz32(mask & -mask);
      mask = (mask & (mask - 1)) >>> 0;
      const at = id * 4;
      castCandidate(id, ox, oz, dx, dz, rayBounds[at], rayBounds[at + 1],
        rayBounds[at + 2], rayBounds[at + 3]);
    }
  }
  function castShell(ox, oz, dx, dz, maxLength, overCover) {
    castBegin(ox, oz, dx, dz, maxLength);
    const arena = state.arena, active = castActive;
    if (rayGeometryArena === arena) {
      // Chunks double in length: near contacts end the walk early, and a
      // long clear leg costs a few index queries rather than one per 2.5.
      // A zero-length cast (a resting shell) still tests where it is.
      let tested = 0, start = 0, chunk = SHELL_CHUNK;
      for (;;) {
        const end = start + chunk < castLength ? start + chunk : castLength;
        chunk += chunk;
        const ax = ox + dx * start, az = oz + dz * start;
        const bx = ox + dx * end, bz = oz + dz * end;
        let mask = (rayCandidateMask((ax < bx ? ax : bx) - SHELL_RADIUS,
          (ax > bx ? ax : bx) + SHELL_RADIUS, (az < bz ? az : bz) - SHELL_RADIUS,
          (az > bz ? az : bz) + SHELL_RADIUS, active) & ~tested) >>> 0;
        tested = (tested | mask) >>> 0;
        castCandidates(mask, ox, oz, dx, dz);
        if (castLength <= end) break;
        start = end;
      }
      return;
    }
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      if (crate.active) castCandidate(at, ox, oz, dx, dz, crate.x - CRATE_HALF,
        crate.x + CRATE_HALF, crate.z - CRATE_HALF, crate.z + CRATE_HALF);
    }
    const bounds = ARENA_OBSTACLE_BOUNDS[arena];
    for (let at = 0; at < ARENA_OBSTACLE_COUNTS[arena] * 4; at += 4)
      castCandidate(4, ox, oz, dx, dz, bounds[at], bounds[at + 1],
        bounds[at + 2], bounds[at + 3]);
    if (!overCover) for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (barrier.active) castCandidate(20 + at, ox, oz, dx, dz, barrier.left,
        barrier.right, barrier.top, barrier.bottom);
    }
    if (!state.gateOpen) {
      const gates = ARENA_GATE_BOUNDS[arena];
      for (let at = 0; at < ARENA_GATE_COUNTS[arena] * 4; at += 4)
        castCandidate(26, ox, oz, dx, dz, gates[at], gates[at + 1],
          gates[at + 2], gates[at + 3]);
    }
  }

  /* How a shell of direction (dx, dz) turns at the bounce just cast.
     Shells keep to axis flips, as they always have. A face or the arena
     edge reverses the component along its normal (an arena corner both).
     A rounded corner takes the flip (x, z or both) nearest the mirror
     reflection about its normal among those that leave the corner, so a
     glancing hit turns aside and a near head-on one comes back; ties
     prefer one flip, x first. */
  let shellFlipX = false, shellFlipZ = false;
  function shellReflect(dx, dz) {
    const nx = castNormalX, nz = castNormalZ;
    if (!castArc) {
      shellFlipX = dx * nx < -SHELL_DIRECTION_EPSILON;
      shellFlipZ = dz * nz < -SHELL_DIRECTION_EPSILON;
      return;
    }
    const twice = 2 * (dx * nx + dz * nz);
    const mirrorX = dx - twice * nx, mirrorZ = dz - twice * nz;
    // Candidates: flip x, flip z, flip both (always leaves: -d.n > 0).
    let best = -dx * mirrorX - dz * mirrorZ;
    shellFlipX = shellFlipZ = true;
    if (dx * nx - dz * nz >= 0) {
      const score = dx * mirrorX - dz * mirrorZ;
      if (score >= best) { best = score; shellFlipX = false; shellFlipZ = true; }
    }
    if (-dx * nx + dz * nz >= 0) {
      const score = -dx * mirrorX + dz * mirrorZ;
      if (score >= best) { shellFlipX = true; shellFlipZ = false; }
    }
  }

  /* The hulls a shell of this owner can hit, as a tank bit mask: active
     tanks of another team. */
  function shellTargetMask(owner) {
    let mask = 0;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active && tank.id !== owner.id && tank.team !== owner.team) mask |= 1 << at;
    }
    return mask;
  }
  /* Nearest hull of the mask the leg would meet, within .34 (a boss .8)
     squared of the path; -1 when none. Its distance along the leg is
     shellScanAt (0 when the leg starts inside the hull). */
  let shellScanAt = 0;
  function shellScanTanks(mask, ax, az, dx, dz, length) {
    let best = -1;
    shellScanAt = length;
    // No hull (radius under 1) reaches a leg from beyond its length plus 1.
    const reach = length + 1;
    for (; mask; mask &= mask - 1) {
      const at = 31 - Math.clz32(mask & -mask);
      const tank = tanks[at];
      const fromX = tank.x - ax, fromZ = tank.z - az;
      if (fromX > reach || fromX < -reach || fromZ > reach || fromZ < -reach) continue;
      const radiusSquared = tank.boss ? .8 : .34;
      const along = fromX * dx + fromZ * dz;
      if (along < -1 || along > shellScanAt + 1) continue;
      const distanceSquared = fromX * fromX + fromZ * fromZ;
      const offSquared = distanceSquared - along * along;
      if (offSquared > radiusSquared) continue;
      let contact = along - Math.sqrt(radiusSquared - offSquared);
      if (contact < 0) {
        if (distanceSquared > radiusSquared) continue;
        contact = 0;
      }
      if (contact <= shellScanAt) { shellScanAt = contact; best = at; }
    }
    return best;
  }

  /* ---------------------------------------------------------- aim guide --
     The player's next shell, cast with the shell sweep above. Leg 1 runs
     from spawnShell's real spawn point to the first contact; leg 2 starts
     there in the direction the shell turns to (shellReflect) and runs to
     the next contact or the end of range, exactly where updateBullets
     takes the shell while nothing moves into its path. The walls-only
     cast is cached on everything it reads, so a still tank costs one key
     comparison a frame; the Full level's enemy test reads moving tanks and
     runs every frame. Visual only: no simulation, bot, control or replay
     state reads it. */
  const AIM_OFF = 0, AIM_FULL = 2;
  const AIM_LEVEL_NAMES = ["Off", "Sight", "Full"];
  const aimGuide = {
    keyX: NaN, keyZ: NaN, keySine: NaN, keyCosine: NaN, keyBounces: false,
    keyRange: -1, keyArena: -1, keyEpoch: -1, keyActive: -1, keyIndexed: -2,
    keyFull: false,
    reach: 0, originX: 0, originZ: 0, dirX: 0, dirZ: 1,
    length1: 0, kind1: SHELL_RANGE, hitX: 0, hitZ: 0, normalX: 0, normalZ: 0,
    edgeGap: 0, arc: false,
    bounceX: 0, bounceZ: 0, length2: 0, kind2: SHELL_RANGE, endX: 0, endZ: 0,
    enemy: -1, enemyLeg: 0, enemyAt: 0, hidden: false,
    casts: 0, updates: 0, boxes: 0, drops: 0,
  };

  /* Recast only when something the cast reads has changed. Returns true
     when it recast. Charging past 0.4 s makes a shell that cannot bounce. */
  function updateAimGuide(tank, level = AIM_FULL) {
    const guide = aimGuide;
    // Sight draws only the bounce stub, so its leg 2 is cast that far.
    const full = level === AIM_FULL;
    guide.updates++;
    // The turret cache holds the 512-step heading spawnShell will use.
    shellDirection(tank.turretSine, tank.turretCosine);
    const sine = shellDirX, cosine = shellDirZ;
    const charged = tank.fireCharge >= CHARGE_THRESHOLD;
    const range = mainShellSpeed(tank, charged) * MAIN_SHELL_LIFE;
    const bounces = !charged;
    const overCover = elevatedFiringOrigin(tank);
    // The live candidate set is also the key for barriers, crates and the gate.
    const arena = state.arena;
    const active = shellActiveMask(overCover);
    if (guide.keyX === tank.x && guide.keyZ === tank.z
        && guide.keySine === sine && guide.keyCosine === cosine
        && guide.keyBounces === bounces && guide.keyRange === range
        && guide.keyArena === arena && guide.keyEpoch === aimGeometryEpoch
        && guide.keyActive === active && guide.keyIndexed === rayGeometryArena
        && guide.keyFull === full) return false;
    guide.keyX = tank.x; guide.keyZ = tank.z; guide.keySine = sine;
    guide.keyCosine = cosine; guide.keyBounces = bounces; guide.keyRange = range;
    guide.keyArena = arena; guide.keyEpoch = aimGeometryEpoch;
    guide.keyActive = active; guide.keyIndexed = rayGeometryArena;
    guide.keyFull = full;
    castActive = active; castSkip = -1;
    // spawnShell's muzzle rule (shellReach).
    const reach = shellReach(tank.x, tank.z, sine, cosine, overCover);
    const ox = tank.x + sine * reach, oz = tank.z + cosine * reach;
    guide.reach = reach; guide.originX = ox; guide.originZ = oz;
    guide.dirX = sine; guide.dirZ = cosine;
    guide.casts++;
    castShell(ox, oz, sine, cosine, range, overCover);
    const hitX = ox + sine * castLength, hitZ = oz + cosine * castLength;
    guide.length1 = castLength; guide.kind1 = castKind;
    guide.hitX = hitX; guide.hitZ = hitZ;
    guide.normalX = castNormalX; guide.normalZ = castNormalZ;
    guide.edgeGap = castEdgeGap; guide.arc = castArc;
    guide.length2 = 0; guide.kind2 = SHELL_RANGE;
    guide.endX = hitX; guide.endZ = hitZ;
    if (castKind !== SHELL_BOUNCE) return true;
    if (!bounces) { guide.kind1 = SHELL_STOP; return true; }
    shellReflect(sine, cosine);
    const bounceX = shellFlipX ? -sine : sine, bounceZ = shellFlipZ ? -cosine : cosine;
    guide.bounceX = bounceX; guide.bounceZ = bounceZ;
    const left = range - guide.length1;
    if (left <= 0) return true;
    /* From the contact itself, as updateBullets continues the step. Sight
       draws only a direction stub, so it casts the same sweep (walls, the
       gate, barriers, crates and the arena edge) no further than that. */
    guide.casts++;
    castShell(hitX, hitZ, bounceX, bounceZ,
      full || left < AIM_STUB ? left : AIM_STUB, overCover);
    guide.length2 = castLength; guide.kind2 = castKind;
    guide.endX = hitX + bounceX * castLength;
    guide.endZ = hitZ + bounceZ * castLength;
    return true;
  }

  /* Full only: confirm a hit on an enemy the player can see. Concealment
     follows the player's own rules: smoke along the shell's path (as bots
     check their bank shots), a campaign foe still hidden in its camouflage,
     and no hit indicators at all under the Fog of Memory fault. A hidden
     hull is drawn as if absent and confirms nothing, so the guide never
     reveals it by stopping short or turning red. */
  function updateAimGuideEnemy(player) {
    const guide = aimGuide;
    guide.enemy = -1; guide.enemyLeg = 0; guide.hidden = false;
    if (campaign.hidesHits()) return;
    const targets = shellTargetMask(player);
    let id = shellScanTanks(targets, guide.originX, guide.originZ,
      guide.dirX, guide.dirZ, guide.length1);
    if (id >= 0) {
      const at = shellScanAt;
      if (campaign.concealed(id)
          || lineCrossesSmoke(guide.originX, guide.originZ,
            guide.originX + guide.dirX * at, guide.originZ + guide.dirZ * at)) {
        guide.hidden = true; return;
      }
      guide.enemy = id; guide.enemyLeg = 1; guide.enemyAt = at;
      return;
    }
    if (guide.length2 <= 0) return;
    id = shellScanTanks(targets, guide.hitX, guide.hitZ,
      guide.bounceX, guide.bounceZ, guide.length2);
    if (id < 0) return;
    const at = shellScanAt;
    if (campaign.concealed(id)
        || lineCrossesSmoke(guide.originX, guide.originZ, guide.hitX, guide.hitZ)
        || lineCrossesSmoke(guide.hitX, guide.hitZ,
          guide.hitX + guide.bounceX * at, guide.hitZ + guide.bounceZ * at)) {
      guide.hidden = true; return;
    }
    guide.enemy = id; guide.enemyLeg = 2; guide.enemyAt = at;
  }

  /* The level in force: the URL switch, then the Practice Range's own
     toggle, then the player's choice (Off unless they turn it on). */
  function aimGuideLevel() {
    if (AIM_URL_LEVEL >= 0) return AIM_URL_LEVEL;
    if (practice.on) return practice.aimLevel;
    return preferences.aimGuide;
  }
  /* The Controls choice changes the level it shows: the range's own
     toggle while the range runs, otherwise the saved preference. */
  function cycleAimGuide(delta = 1) {
    if (practice.on) practice.cycleAim(delta);
    else {
      preferences.aimGuide = (preferences.aimGuide + 3 + (delta < 0 ? -1 : 1)) % 3;
      savePreferences();
    }
    refreshSetupLabels();
  }

  /* The guide is an assist with a score cost: a run scores at the multiplier
     of the highest level it used while playing (switching it off in the
     pause menu before the end does not undo it). Practice has no score. The
     simulation's score is untouched; final and best scores, and campaign
     results through the Range Faults' multiplier, apply it. A replay
     records the level its run used and scores with that, never the
     viewer's. Each player's own setting prices only their own result. */
  const AIM_SCORE_MULTIPLIERS = [1, .9, .75];
  let runAimLevel = 0;
  function noteAimLevel() {
    if (replayActive || practice.on) return;
    const level = aimGuideLevel();
    if (level > runAimLevel) runAimLevel = level;
  }
  function aimScoreMultiplier() { return AIM_SCORE_MULTIPLIERS[runAimLevel]; }
  function finalScore() { return Math.round(state.score * AIM_SCORE_MULTIPLIERS[runAimLevel]); }
  function aimScoreNote() {
    return runAimLevel ? ` Aim guide ${AIM_LEVEL_NAMES[runAimLevel]} x${AIM_SCORE_MULTIPLIERS[runAimLevel]}.` : "";
  }

  /* Guide geometry: flat ribbons at shell height, one box per leg. Each
     extra instance costs about 0.2 ms a frame on a PSP at 111 MHz (upload
     and draw, measured in PPSSPP), so a dashed leg (six boxes) cost more
     than the whole old reticle; a leg is one thin ribbon instead. Sight:
     leg 1, the impact mark and a short bounce stub (direction only), three
     boxes like the old reticle. Full adds the dimmer bounce leg, red legs
     and a plate under a visible enemy the shell would hit: five. */
  const AIM_LINE_COLOR = [1, .93, .55], AIM_BOUNCE_COLOR = [.45, .92, 1];
  const AIM_STOP_COLOR = [1, .5, .16], AIM_ENEMY_COLOR = [1, .27, .16];
  const AIM_Y = .44, AIM_STUB = .6, AIM_BARREL_CLEAR = 1.12;
  function aimBox(x, z, width, height, depth, color, alpha, sine, cosine, y = AIM_Y) {
    if (addOptionalBox(x, y, z, width, height, depth, 0, color, alpha, sine, cosine))
      aimGuide.boxes++;
  }
  /* The impact mark: a bar along the face for a bounce, a block where the
     shell stops (barrier, crate, or a charged shell at a wall), a dot at
     the end of range. A confirmed leg-1 enemy is marked by its plate. */
  function addAimMark() {
    const guide = aimGuide;
    if (guide.enemy >= 0 && guide.enemyLeg === 1) return;
    if (guide.kind1 === SHELL_BOUNCE)
      aimBox(guide.hitX, guide.hitZ, .5, .2, .06, AIM_LINE_COLOR, .95,
        guide.normalX, guide.normalZ);
    else if (guide.kind1 === SHELL_STOP)
      aimBox(guide.hitX, guide.hitZ, .24, .24, .24, AIM_STOP_COLOR, .95,
        guide.dirX, guide.dirZ);
    else aimBox(guide.hitX, guide.hitZ, .14, .07, .14, AIM_LINE_COLOR, .55,
      guide.dirX, guide.dirZ);
  }
  /* Legs meet (and a shell bounced straight back retraces leg 1), so each
     leg rides one FLAT_STEP above the one before it: equal tops flickered
     where they overlapped. */
  function addAimLeg(ax, az, dx, dz, from, to, width, color, alpha, step) {
    const center = (from + to) * .5;
    aimBox(ax + dx * center, az + dz * center, width, .03, to - from, color, alpha, dx, dz,
      AIM_Y + step * FLAT_STEP);
  }
  function addAimLines(level) {
    const guide = aimGuide;
    const enemy1 = guide.enemy >= 0 && guide.enemyLeg === 1;
    const enemy2 = guide.enemy >= 0 && guide.enemyLeg === 2;
    const start = guide.reach < AIM_BARREL_CLEAR ? AIM_BARREL_CLEAR - guide.reach : 0;
    // An enemy found last frame never extends a leg past this frame's wall.
    const end1 = enemy1 && guide.enemyAt < guide.length1 ? guide.enemyAt : guide.length1;
    const leg1 = end1 - start > .1;
    const stub = !enemy1 && guide.length2 > 0;
    let stubLength = stub ? Math.min(AIM_STUB, guide.length2) : 0;
    if (enemy2 && guide.enemyAt < stubLength) stubLength = guide.enemyAt;
    const end2 = enemy2 && guide.enemyAt < guide.length2 ? guide.enemyAt : guide.length2;
    const leg2 = level === AIM_FULL && stub && end2 - stubLength > .35;
    const need = (leg1 ? 1 : 0) + (stubLength > .05 ? 1 : 0) + (leg2 ? 1 : 0)
      + (guide.enemy >= 0 ? 1 : 0);
    if (!need) return;
    if (collectInstancedBoxes && boxInstanceCount + need > optionalInstanceCeiling) {
      instanceCapHitThisFrame = true; guide.drops++;
      return;
    }
    if (leg1) addAimLeg(guide.originX, guide.originZ, guide.dirX, guide.dirZ, start, end1,
      .07, enemy1 ? AIM_ENEMY_COLOR : AIM_LINE_COLOR, enemy1 ? .85 : .7, 0);
    if (stubLength > .05)
      addAimLeg(guide.hitX, guide.hitZ, guide.bounceX, guide.bounceZ, 0, stubLength,
        .12, AIM_BOUNCE_COLOR, .9, 1);
    if (leg2) addAimLeg(guide.hitX, guide.hitZ, guide.bounceX, guide.bounceZ,
      stubLength + .15, end2, .06, enemy2 ? AIM_ENEMY_COLOR : AIM_BOUNCE_COLOR,
      enemy2 ? .8 : .42, 2);
    if (guide.enemy >= 0) {
      const tank = tanks[guide.enemy];
      aimBox(tank.x, tank.z, 1.8, .02, 1.8, AIM_ENEMY_COLOR, .45,
        tank.yawSine, tank.yawCosine, tank.surfaceY + ENEMY_PLATE_TOP - .01);
    }
  }

  /* ------------------------------------------------------------- tracers --
     The player's own shells leave a short fading trail of the path they
     actually took, bounces included: feedback, not a preview. spawnShell
     and updateBullets' bounce record vertices into these fixed arrays
     (visual-only writes; nothing reads them back into the simulation). The
     trail is the last TRACER_LENGTH units behind the shell, one box per
     straight piece (two around a ricochet), drawn for the newest shell
     only; it fades for TRACER_FADE s of game time after the
     shell ends. Each trail is admitted whole or not at all. */
  const TRACER_SLOTS = 4, TRACER_VERTICES = 4, TRACER_PIECES = 2;
  const TRACER_LENGTH = 1.5, TRACER_PIECE = 1.5, TRACER_FADE = .25;
  const TRACER_BOX_LIMIT = 2, TRACER_COLOR = [1, .86, .45];
  const TRACER_ALPHAS = [.5, .3];
  const tracerBullet = new Int8Array(TRACER_SLOTS).fill(-1);
  const bulletTracer = new Int8Array(MAX_BULLETS).fill(-1);
  const tracerVertices = new Float32Array(TRACER_SLOTS * TRACER_VERTICES * 2);
  const tracerVertexCount = new Uint8Array(TRACER_SLOTS);
  const tracerHead = new Float32Array(TRACER_SLOTS * 2);
  const tracerEnded = new Float64Array(TRACER_SLOTS);
  const tracerPieces = new Float32Array(TRACER_PIECES * 5);
  let tracerCursor = 0, tracerBoxes = 0;
  function tracerStart(bulletAt, x, z) {
    const slot = tracerCursor;
    tracerCursor = (tracerCursor + 1) % TRACER_SLOTS;
    const previous = tracerBullet[slot];
    if (previous >= 0 && bulletTracer[previous] === slot) bulletTracer[previous] = -1;
    tracerBullet[slot] = bulletAt; bulletTracer[bulletAt] = slot;
    tracerVertices[slot * TRACER_VERTICES * 2] = x;
    tracerVertices[slot * TRACER_VERTICES * 2 + 1] = z;
    tracerVertexCount[slot] = 1;
    tracerHead[slot * 2] = x; tracerHead[slot * 2 + 1] = z;
    tracerEnded[slot] = NaN;
  }
  function tracerVertex(slot, x, z) {
    const first = slot * TRACER_VERTICES * 2;
    let count = tracerVertexCount[slot];
    if (count === TRACER_VERTICES) {
      tracerVertices.copyWithin(first, first + 2, first + TRACER_VERTICES * 2);
      count--;
    }
    tracerVertices[first + count * 2] = x;
    tracerVertices[first + count * 2 + 1] = z;
    tracerVertexCount[slot] = count + 1;
  }
  function clearTracers() {
    tracerBullet.fill(-1); bulletTracer.fill(-1); tracerVertexCount.fill(0);
  }
  function addTracer(slot, fade, shell) {
    let x = tracerHead[slot * 2], z = tracerHead[slot * 2 + 1];
    let remaining = TRACER_LENGTH, pieces = 0;
    let vertex = tracerVertexCount[slot] - 1;
    const first = slot * TRACER_VERTICES * 2;
    while (remaining > .01 && vertex >= 0 && pieces < TRACER_PIECES) {
      const dx = tracerVertices[first + vertex * 2] - x;
      const dz = tracerVertices[first + vertex * 2 + 1] - z;
      let length, ux, uz;
      if (shell && !pieces) {
        // A live shell's newest piece runs back along its own heading.
        ux = -shell.headingSine; uz = -shell.headingCosine;
        length = dx * ux + dz * uz;
      } else {
        length = Math.sqrt(dx * dx + dz * dz);
        ux = dx / length; uz = dz / length;
      }
      if (!(length >= .01)) { vertex--; continue; }
      let take = length < remaining ? length : remaining;
      if (take > TRACER_PIECE) take = TRACER_PIECE;
      const at = pieces * 5;
      tracerPieces[at] = x + ux * take * .5; tracerPieces[at + 1] = z + uz * take * .5;
      tracerPieces[at + 2] = take; tracerPieces[at + 3] = ux; tracerPieces[at + 4] = uz;
      x += ux * take; z += uz * take; remaining -= take; pieces++;
      if (take >= length - 1e-4) vertex--;
    }
    if (!pieces || tracerBoxes + pieces > TRACER_BOX_LIMIT) return;
    if (collectInstancedBoxes && boxInstanceCount + pieces > optionalInstanceCeiling) {
      instanceCapHitThisFrame = true;
      return;
    }
    for (let piece = 0; piece < pieces; piece++) {
      const at = piece * 5;
      if (addOptionalBox(tracerPieces[at], .48, tracerPieces[at + 1], .07, .07,
          tracerPieces[at + 2], 0, TRACER_COLOR, TRACER_ALPHAS[piece] * fade,
          tracerPieces[at + 3], tracerPieces[at + 4])) tracerBoxes++;
    }
  }
  function addTracers() {
    tracerBoxes = 0;
    for (let step = 0; step < TRACER_SLOTS; step++) {
      // Newest first, so a crowded frame keeps the latest trail.
      const slot = (tracerCursor + TRACER_SLOTS - 1 - step) % TRACER_SLOTS;
      const bulletAt = tracerBullet[slot];
      if (bulletAt < 0) continue;
      let fade = 1, shell = null;
      if (bulletTracer[bulletAt] === slot && bullets[bulletAt].active) {
        shell = bullets[bulletAt];
        tracerHead[slot * 2] = shell.x;
        tracerHead[slot * 2 + 1] = shell.z;
      } else {
        if (tracerEnded[slot] !== tracerEnded[slot]) tracerEnded[slot] = state.time;
        const elapsed = state.time - tracerEnded[slot];
        if (elapsed < 0 || elapsed >= TRACER_FADE) {
          if (bulletTracer[bulletAt] === slot) bulletTracer[bulletAt] = -1;
          tracerBullet[slot] = -1;
          continue;
        }
        fade = 1 - elapsed / TRACER_FADE;
      }
      addTracer(slot, fade, shell);
      // One trail a frame (each box costs about 0.2 ms on a PSP): the
      // newest shell's, with older slots still expiring above.
      if (tracerBoxes) break;
    }
  }

  function updateOverlay() {
    const player = playerTank();
    const objectiveVisible = objectiveArrowShown(objectivePosition(objectiveTarget));
    const objectiveAngle = Math.atan2(objectiveTarget.x - player.x,
      objectiveTarget.z - player.z) - cameraYaw;
    const objectiveStep = Math.round(wrapAngle(objectiveAngle) * 16 / Math.PI);
    const damageStep = Math.round(
      wrapAngle(state.damageAngle - cameraYaw) * 16 / Math.PI);
    const cooldown = Math.max(0, player.gadgetCooldown);
    const gadgetStep = Math.round(43 * (1 - Math.min(1,
      cooldown / Math.max(1, player.gadgetCooldownMax))));
    const commandStep = Math.round(43 * state.commandMeter / 100);
    const multiplierStep = isOnslaught()
      ? Math.max(1, Math.min(5, state.multiplier | 0)) : 0;
    const hitVisible = state.hitConfirm > 0;
    const damageVisible = state.damageIndicator > 0;
    const commandEnabled = preferences.command;
    const lungeCooling = player.lungeCooldown > 0;
    if (lungeCooling === hudLungeCooling
        && objectiveStep === hudVisualObjectiveStep
        && objectiveVisible === hudVisualObjective
        && damageStep === hudVisualDamageStep
        && hitVisible === hudVisualHit
        && damageVisible === hudVisualDamage
        && gadgetStep === hudVisualGadgetStep
        && commandStep === hudVisualCommandStep
        && multiplierStep === hudVisualMultiplierStep
        && commandEnabled === hudVisualCommandEnabled) return;
    hudVisualObjectiveStep = objectiveStep;
    hudVisualObjective = hudObjectiveVisible = objectiveVisible;
    hudVisualDamageStep = damageStep;
    hudVisualHit = hitVisible;
    hudVisualDamage = damageVisible;
    hudVisualGadgetStep = gadgetStep;
    hudVisualCommandStep = commandStep;
    hudVisualMultiplierStep = multiplierStep;
    hudVisualCommandEnabled = commandEnabled;
    hudLungeCooling = lungeCooling;
    hudObjectiveAngle = objectiveStep * Math.PI / 16;
    hudDamageAngle = damageStep * Math.PI / 16;
    hudHitVisible = hitVisible;
    hudDamageVisible = damageVisible;
    hudGadgetRatio = gadgetStep / 43;
    hudCommandRatio = commandStep / 43;
    hudMultiplierRatio = multiplierStep / 5;
    hudIndicatorDirty = true;
  }

  function spawnParticles(x, y, z, count, speed = 2) {
    /* The renderer admits six particles. Initializing additional invisible
       particles only moves action work into the collision frame. */
    count = Math.min(6, count);
    for (let made = 0; made < count; made++) {
      let particle = null, particleAt = -1;
      for (let probe = 0; probe < MAX_PARTICLES; probe++) {
        const at = (particleSpawnCursor + probe) % MAX_PARTICLES;
        if (!particles[at].active) {
          particle = particles[at]; particleAt = at;
          particleSpawnCursor = (at + 1) % MAX_PARTICLES;
          break;
        }
      }
      if (!particle) return;
      const template = particleTemplateCursor++ & (PARTICLE_TEMPLATE_COUNT - 1);
      const direction = particleTemplateDirection[template];
      const velocity = speed * particleTemplateSpeed[template];
      setParticleActive(particleAt, true);
      particle.x = x; particle.y = y; particle.z = z;
      particle.vx = directionSines[direction] * velocity;
      particle.vz = directionCosines[direction] * velocity;
      particle.renderSine = directionSines[direction];
      particle.renderCosine = directionCosines[direction];
      particle.renderStreak = Math.max(.12, Math.min(.46,
        .1 + velocity * .075));
      particle.vy = particleTemplateVertical[template];
      particle.life = particle.maximum = particleTemplateLife[template];
    }
  }

  function spawnShell(tank, spread, speed, life, damage, bounces,
                      barrierDamage = 1, pierce = 0, bypassShield = false) {
    let bullet = null, bulletAt = -1;
    for (let probe = 0; probe < MAX_BULLETS; probe++) {
      const at = (bulletSpawnCursor + probe) % MAX_BULLETS;
      if (!bullets[at].active) {
        bullet = bullets[at]; bulletAt = at;
        bulletSpawnCursor = (at + 1) % MAX_BULLETS;
        break;
      }
    }
    if (!bullet) return false;
    const angle = tank.turret + spread;
    const direction = orientationIndex(angle);
    shellDirection(directionSines[direction], directionCosines[direction]);
    const sine = shellDirX, cosine = shellDirZ;
    const overCover = elevatedFiringOrigin(tank);
    setBulletActive(bulletAt, true); bullet.owner = tank.id;
    if (qualificationStats !== null) qualificationStats.shots[tank.id]++;
    // A hull resting against scenery has its muzzle inside it; start that
    // shell at the hull centre so it meets the face instead of emerging
    // beyond a thin wall, gate, barrier or crate.
    const reach = shellReach(tank.x, tank.z, sine, cosine, overCover);
    bullet.x = tank.x + sine * reach;
    bullet.z = tank.z + cosine * reach;
    bullet.vx = sine * speed;
    bullet.vz = cosine * speed;
    bullet.speed = speed; bullet.pierceId = -1; bullet.turns = 0;
    bullet.headingSine = sine;
    bullet.headingCosine = cosine;
    bullet.heading = angle;
    bullet.life = life; bullet.bounces = bounces;
    bullet.bounceCount = 0; bullet.bounceFlash = 0;
    bullet.damage = damage; bullet.barrierDamage = barrierDamage;
    bullet.pierce = pierce; bullet.bypassShield = bypassShield;
    bullet.overCover = overCover;
    // Visual only: a reused shell slot ends any trail it carried.
    bulletTracer[bulletAt] = -1;
    if (tank === playerTank()) tracerStart(bulletAt, bullet.x, bullet.z);
    return true;
  }

  /* Where a shell starts along its barrel: the muzzle .78 ahead, or the
     hull centre when the barrel meets scenery (a hull resting against a
     thin wall has its muzzle inside or beyond it, and one beside a corner
     has its barrel across the corner's tip): that shell sweeps from the
     centre and meets the face. The tiny-circle occupancy grid covers every
     shape a shell meets, so empty cells under the barrel answer without a
     cast. Shared by spawnShell and the aim guide. */
  function shellReach(x, z, dx, dz, overCover) {
    const muzzleX = x + dx * .78, muzzleZ = z + dz * .78;
    if (!(muzzleX >= -ARENA_EDGE && muzzleX <= ARENA_EDGE
        && muzzleZ >= -ARENA_EDGE && muzzleZ <= ARENA_EDGE)) return 0;
    if (crateGridReady && tinyCircleArena === state.arena) {
      const firstX = ((x < muzzleX ? x : muzzleX) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
      const lastX = ((x < muzzleX ? muzzleX : x) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
      const firstZ = ((z < muzzleZ ? z : muzzleZ) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
      const lastZ = ((z < muzzleZ ? muzzleZ : z) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
      let open = true;
      for (let cellZ = firstZ; open && cellZ <= lastZ; cellZ++)
        for (let cellX = firstX; cellX <= lastX; cellX++)
          if (tinyCircleOccupied[cellZ * COLLISION_GRID_SIDE + cellX]) { open = false; break; }
      if (open) return .78;
    }
    if (muzzleBlocked(muzzleX, muzzleZ, overCover)) return 0;
    castActive = shellActiveMask(overCover); castSkip = -1;
    castShell(x, z, dx, dz, .78, overCover);
    return castKind === SHELL_RANGE ? .78 : 0;
  }

  // Point tests for a shell spawned at (x, z): the arena edge, walls and
  // the closed gate, barriers (unless fired over them) and crates.
  function muzzleBlocked(x, z, overCover) {
    if (x < -ARENA_EDGE || x > ARENA_EDGE || z < -ARENA_EDGE || z > ARENA_EDGE
        || bulletHitsObstacle(x, z) || (!overCover && barrierAt(x, z, .1))) return true;
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      if (crate.active && Math.abs(x - crate.x) < CRATE_SHELL_HALF
          && Math.abs(z - crate.z) < CRATE_SHELL_HALF) return true;
    }
    return false;
  }

  /* How long a bot whose shot at the player waits for the volley lane
     (gap: time since the last bot shot at the player); the id offset keeps
     two ready bots from waking together. bots.js waits at the end of its
     telegraph; deferCrowdedBotVolley is the fire-time backstop for a shot
     that skipped the telegraph. */
  function botVolleyDelay(tank, gap) {
    return Math.max(.02, BOT_VOLLEY_GAP - gap + (tank.id % 3) * .015);
  }

  function deferCrowdedBotVolley(tank, secondary) {
    const gap = state.time - lastBotPlayerShotAt;
    if (tank.player || tank.target !== playerTank().id
        || gap >= BOT_VOLLEY_GAP) return false;
    tank.fireWindup = botVolleyDelay(tank, gap);
    tank.fireSecondaryArmed = !!secondary;
    tank.fireTelegraphed = true;
    return true;
  }

  // The main shell's speed; the aim guide prices its range from the same.
  function mainShellSpeed(tank, charged) {
    return charged ? 11 : tank.classId === 1 && tank.commandBuff > 0 ? 8.4 : 7.2;
  }

  function fireTank(tank, spread = 0, charged = false) {
    if (state.gameMode === MODE_DUEL && state.duelResolving) return false;
    if (tank.cooldown > 0 || !tank.active || (tank.boss && tank.turretHealth <= 0)) return false;
    if (deferCrowdedBotVolley(tank, false)) return false;
    const actionStarted = qualificationAIActive ? performance.now() : 0;
    const profile = CLASSES[tank.classId];
    const strikerCommand = tank.classId === 1 && tank.commandBuff > 0;
    if (!spawnShell(tank, spread, mainShellSpeed(tank, charged), MAIN_SHELL_LIFE,
        charged ? 38 : strikerCommand ? 34 : 28,
        charged ? 0 : strikerCommand || tank.player ? 2 : 1,
        strikerCommand ? 2 : 1, strikerCommand ? 1 : 0, strikerCommand)) {
      if (qualificationAIActive) recordQualificationAction(0, actionStarted);
      return false;
    }
    tank.cooldown = profile.reload * (tank.classId === 0 && tank.commandBuff > 0
      ? .55 : 1);
    tank.recoil = .11;
    if (tank.player) { state.shots++; state.mainShots++; }
    if (tank.player || (online.active && tank.id < 2)) controls.dodge(tank);
    if (state.gameMode === MODE_DUEL) state.duelResolving = true;
    else noteBotPlayerShot(tank);
    spawnParticles(tank.x + tank.turretSine * .78, .42,
      tank.z + tank.turretCosine * .78, 3, .8);
    sounds.shot(tank.player);
    if (qualificationAIActive) recordQualificationAction(0, actionStarted);
    return true;
  }

  function recordQualificationAction(kind, started) {
    const elapsed = performance.now() - started;
    qualificationActionTimes[kind] += elapsed;
    qualificationActionTimes[kind + 4] = Math.max(
      qualificationActionTimes[kind + 4], elapsed);
    qualificationActionCounts[kind]++;
    qualificationAITimes[21 + kind] += elapsed;
    qualificationAITimes[26 + kind]++;
  }

  /* A bot's cannon or secondary shot at the player: the volley lane that
     spaces such shots, and its diagnostics. A breach shot aims at
     scenery, not the player, so it counts for neither. */
  function noteBotPlayerShot(tank) {
    if (tank.player || tank.target !== playerTank().id || tank.breachShot) return;
    const stats = qualificationStats;
    if (stats !== null) {
      stats.botPlayerShots++;
      if (!tank.fireTelegraphed) stats.botTelegraphViolations++;
      if (lastBotPlayerShotAt > -90) stats.botVolleyMinimum = Math.min(
        stats.botVolleyMinimum, state.time - lastBotPlayerShotAt);
    }
    lastBotPlayerShotAt = state.time;
    tank.fireTelegraphed = false;
  }

  function fireSecondary(tank) {
    if (state.gameMode === MODE_DUEL && state.duelResolving) return false;
    if (tank.boss && tank.turretHealth <= 0) return false;
    if (!tank.active || tank.secondaryCooldown > 0) return false;
    if (deferCrowdedBotVolley(tank, true)) return false;
    const actionStarted = qualificationAIActive ? performance.now() : 0;
    let admitted = 0;
    if (tank.classId === 0) {
      for (let shot = 0; shot < 3; shot++) {
        const spread = (shot - 1) * .075;
        if (spawnShell(tank, spread, 8.1, 2.2, 11, 1)) admitted++;
      }
    } else if (tank.classId === 1) {
      if (spawnShell(tank, 0, 9, 3, 46, 0, 2, 1, true)) admitted++;
    } else {
      for (let shot = 0; shot < 5; shot++) {
        const spread = (shot - 2) * .12;
        if (spawnShell(tank, spread, 6.6, .72, 10, 0)) admitted++;
      }
    }
    /* Only a full shell pool refuses every shell. The main cannon says
       nothing then either; the cooldown is kept for the next press. */
    if (!admitted) {
      if (qualificationAIActive) recordQualificationAction(1, actionStarted);
      return false;
    }
    if (state.gameMode === MODE_DUEL) state.duelResolving = true;
    tank.secondaryCooldown = tank.classId === 0 ? 3.8 : tank.classId === 1 ? 5.5 : 4.8;
    tank.recoil = .14;
    if (tank.player) state.shots += admitted;
    else noteBotPlayerShot(tank);
    spawnParticles(tank.x + tank.turretSine * .85, .42,
      tank.z + tank.turretCosine * .85, 5, 1.15);
    if (tank.classId === 2) sounds.explosion(tank.player
      ? sounds.blast.playerCanister : sounds.blast.botCanister);
    else sounds.secondary(tank.player);
    if (qualificationAIActive) recordQualificationAction(1, actionStarted);
    return true;
  }

  function awardCommand(amount) {
    if (!preferences.command || !(amount > 0)) return;
    state.commandMeter = Math.min(100, state.commandMeter + amount);
  }

  function activateCommand(tank) {
    if (!tank.player || !preferences.command || state.commandMeter < 100)
      return false;
    const actionStarted = qualificationAIActive ? performance.now() : 0;
    state.commandMeter = 0;
    if (tank.classId === 0) {
      tank.commandBuff = 6;
      tank.cooldown = tank.secondaryCooldown = 0;
      showToast("SCOUT OVERDRIVE", 1.1);
    } else if (tank.classId === 1) {
      tank.commandBuff = 8;
      tank.cooldown = 0;
      showToast("STRIKER SABOT", 1.1);
    } else {
      tank.commandBuff = 5;
      tank.health = Math.min(tank.maxHealth, tank.health + 35);
      for (let at = 0; at < MAX_TANKS; at++) {
        const ally = tanks[at];
        if (ally.active && ally.team === tank.team)
          ally.shield = Math.max(ally.shield, 5);
      }
      showToast("BULWARK AEGIS", 1.1);
    }
    sounds.command();
    if (qualificationAIActive) recordQualificationAction(3, actionStarted);
    return true;
  }

  function useGadget(tank) {
    if (!tank.gadget || tank.gadgetCooldown > 0) return;
    const actionStarted = qualificationAIActive ? performance.now() : 0;
    const type = tank.gadget;
    let admitted = true;
    if (type === "MINES") {
      let mine = null;
      for (let at = 0; at < MAX_MINES; at++) {
        const candidate = mines[at];
        if (!candidate.active) { mine = candidate; break; }
      }
      if (mine) {
        mine.active = true; mine.team = tank.team; mine.owner = tank.id;
        mine.x = tank.x - tank.yawSine * .72;
        mine.z = tank.z - tank.yawCosine * .72;
        // Backed against scenery: drop it under the hull, not in the wall.
        if (Math.abs(mine.x) > HULL_EDGE || Math.abs(mine.z) > HULL_EDGE
            || circleHitsObstacle(mine.x, mine.z, .12, true)) {
          mine.x = tank.x; mine.z = tank.z;
        }
        // A mine dropped onto others sits a flat step above them.
        let used = 0;
        for (let at = 0; at < MAX_MINES; at++) {
          const other = mines[at];
          if (other !== mine && other.active && Math.abs(other.x - mine.x) < .42
              && Math.abs(other.z - mine.z) < .42) used |= 1 << other.level;
        }
        mine.level = used & 1 ? used & 2 ? used & 4 ? 0 : 2 : 1 : 0;
        mine.arm = .55; mine.life = 16;
      } else admitted = false;
    } else if (type === "SMOKE") {
      let smoke = null;
      for (let at = 0; at < smokeClouds.length; at++) {
        const candidate = smokeClouds[at];
        if (!candidate.active) { smoke = candidate; break; }
      }
      if (smoke) {
        smoke.active = true; smoke.x = tank.x; smoke.z = tank.z; smoke.life = 7;
        activeSmokeCount++;
      } else admitted = false;
    } else if (type === "SHIELD") tank.shield = 7;
    else if (type === "REPAIR DRONE") tank.repair = 5;
    else if (type === "BOOST TREADS") tank.boost = 5;
    if (!admitted) {
      if (tank.player) showToast("GADGET BUSY", .7);
      if (qualificationAIActive) recordQualificationAction(2, actionStarted);
      return;
    }
    tank.gadgetCooldown = type === "MINES" ? 5 : type === "SMOKE" ? 8 : 10;
    tank.gadgetCooldownMax = tank.gadgetCooldown;
    if (tank.player) showToast(type, 1);
    sounds.gadget();
    if (qualificationAIActive) recordQualificationAction(2, actionStarted);
  }

  /* Bot AI (bots.js), created once the state it reads exists. Its
     functions land in constants here, so every call stays direct. */
  const bots = globalThis.__treadlineCreateBots({
    campaign, qualificationLongSoak, MODE_SURVIVAL, MODE_CONTROL, MODE_CONVOY,
    MODE_ONSLAUGHT, MODE_DAILY, MODE_BILLIARDS, MODE_DUEL,
    ARENAS, arenaSpatial, CLASS_COLLISION_RADIUS, BOSS, DAILY_DIFFICULTY, MAX_TANKS, MAX_PICKUPS,
    MAX_BARRIERS, ARENA_EDGE, HULL_EDGE, CRATE_HALF, BOT_FIRE_RANGE_SQUARED,
    BOT_VOLLEY_GAP, botVolleyDelay, QUICK_SPAWN_MIN_DISTANCE, tanks, pickups, barriers,
    crates, state, convoy, online, sounds, playerTank, random, wrapAngle,
    isOnslaught, orientationIndex, spawnBlocked, circleHitsObstacle,
    surfaceHeightAt, elevatedFiringOrigin, lineCrossesSmoke, lineCrossesWalls,
    routePadding, breachShotHits, invalidateBotNavigation,
    setQualificationAIActive,
    // Values game.js reassigns, read on rare paths only.
    playerIsBot, qualificationStats: () => qualificationStats,
    activeSmokeCount: () => activeSmokeCount, randomState: () => randomState,
    lastBotPlayerShotAt: () => lastBotPlayerShotAt,
  });
  // var: closure reads skip the TDZ check, like the function declarations
  // these were.
  var {updateBotCommand, updateBotNavigation, restartBotNavigation,
    resetBotPlanning, prepareNavigationGrids, validateSpawns, validateRespawn,
    settleSpawnChecks, cancelSpawnChecks, steerBotRoute, bankAim, setBotProfiling,
    botDifficulty, botDifficultyValue, botDifficultyTables, beginBotFireWindup, pocketKept,
    navigationCell, debug: botDebug} = bots;
  // bots.js keeps its own copy of the per-step profiling flag.
  function setQualificationAIActive(on) {
    qualificationAIActive = on; setBotProfiling(on);
  }

  /* The deepest contact of a hull at (x, z): the outward unit normal from
     the nearest point of whatever it overlaps most (walls, the closed
     gate, active barriers and crates, the edge limit for hull centres).
     Only a blocked bot move asks, so this walks every record. */
  let contactNormalX = 0, contactNormalZ = 0, contactDepth = 0;
  function considerContact(x, z, radius, left, right, top, bottom) {
    const nearX = x < left ? left : x > right ? right : x;
    const nearZ = z < top ? top : z > bottom ? bottom : z;
    const dx = x - nearX, dz = z - nearZ, squared = dx * dx + dz * dz;
    if (squared >= radius * radius || squared === 0) return;
    const distance = Math.sqrt(squared);
    if (radius - distance <= contactDepth) return;
    contactDepth = radius - distance;
    contactNormalX = dx / distance; contactNormalZ = dz / distance;
  }
  function hullContact(x, z, radius) {
    contactDepth = contactNormalX = contactNormalZ = 0;
    // The candidate grids circleHitsObstacle reads (built for hulls up to
    // CIRCLE_GRID_RADIUS); past them, or off the grid, every record.
    const cellX = (x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE, cellZ = (z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    const cell = radius <= CIRCLE_GRID_RADIUS && cellX >= 0 && cellX < COLLISION_GRID_SIDE
      && cellZ >= 0 && cellZ < COLLISION_GRID_SIDE
      ? (cellZ | 0) * COLLISION_GRID_SIDE + (cellX | 0) : -1;
    const arena = state.arena, bounds = ARENA_OBSTACLE_BOUNDS[arena];
    let mask = cell >= 0 ? ARENA_CIRCLE_OBSTACLE_GRIDS[arena][cell] : -1;
    for (let at = 0; mask && at < ARENA_OBSTACLE_COUNTS[arena] * 4; at += 4, mask >>>= 1)
      if (mask & 1) considerContact(x, z, radius, bounds[at], bounds[at + 1],
        bounds[at + 2], bounds[at + 3]);
    if (!state.gateOpen) {
      const gates = ARENA_GATE_BOUNDS[arena];
      mask = cell >= 0 ? ARENA_CIRCLE_GATE_GRIDS[arena][cell] : -1;
      for (let at = 0; mask && at < ARENA_GATE_COUNTS[arena] * 4; at += 4, mask >>>= 1)
        if (mask & 1) considerContact(x, z, radius, gates[at], gates[at + 1],
          gates[at + 2], gates[at + 3]);
    }
    mask = cell >= 0 ? barrierCircleGrid[cell] : -1;
    for (let at = 0; mask && at < MAX_BARRIERS; at++, mask >>>= 1) {
      const barrier = barriers[at];
      if ((mask & 1) && barrier.active) considerContact(x, z, radius, barrier.left,
        barrier.right, barrier.top, barrier.bottom);
    }
    mask = cell >= 0 && crateGridReady ? crateCircleGrid[cell] : -1;
    for (let at = 0; mask && at < crates.length; at++, mask >>>= 1) {
      const crate = crates[at];
      if ((mask & 1) && crate.active) considerContact(x, z, radius, crate.x - CRATE_HALF,
        crate.x + CRATE_HALF, crate.z - CRATE_HALF, crate.z + CRATE_HALF);
    }
    // The edge limit for hull centres.
    if (x - HULL_EDGE > contactDepth) { contactDepth = x - HULL_EDGE; contactNormalX = -1; contactNormalZ = 0; }
    if (-HULL_EDGE - x > contactDepth) { contactDepth = -HULL_EDGE - x; contactNormalX = 1; contactNormalZ = 0; }
    if (z - HULL_EDGE > contactDepth) { contactDepth = z - HULL_EDGE; contactNormalX = 0; contactNormalZ = -1; }
    if (-HULL_EDGE - z > contactDepth) { contactDepth = -HULL_EDGE - z; contactNormalX = 0; contactNormalZ = 1; }
    return contactDepth > 0;
  }

  // An avoidance arc's look-ahead: scenery or past the hull's edge limit.
  function arcBlocked(x, z, radius) {
    return x < -HULL_EDGE || x > HULL_EDGE || z < -HULL_EDGE || z > HULL_EDGE
      || circleHitsObstacle(x, z, radius);
  }

  function moveTank(tank, dt) {
    let movePhaseAt = qualificationAIActive ? performance.now() : 0;
    const command = tank.command;
    const sign = command.reverse ? -1 : 1;
    let left = command.left * sign, right = command.right * sign;
    if (state.gameMode === MODE_DUEL && state.duelResolving) left = right = 0;
    if (tank.boss) {
      if (tank.leftTreadHealth <= 0) left = 0;
      if (tank.rightTreadHealth <= 0) right = 0;
    }
    const movement = (left + right) * .5;
    const priorYaw = tank.yaw;
    tank.yaw = wrapAngle(priorYaw + (right - left) * 2.05 * dt);
    if (tank.yaw !== priorYaw) updateTankYawCache(tank);
    const oldX = tank.x, oldZ = tank.z;
    let driveSpeed = tank.driveSpeed;
    if (tank.boost > 0) driveSpeed *= 1.46;
    if (tank.classId === 0 && tank.commandBuff > 0) driveSpeed *= 1.38;
    const terrain = hazardAt(tank.x, tank.z);
    if (terrain === 2) driveSpeed *= .58;
    const intendedTravel = controls.slide(tank, dt, terrain, movement,
      driveSpeed, right - left);
    if (tank.boss && tank.leftTreadHealth <= 0 && tank.rightTreadHealth <= 0)
      tank.slideX = tank.slideZ = 0;
    tank.x += tank.slideX * dt; tank.z += tank.slideZ * dt;
    if (qualificationAIActive) {
      const now = performance.now(), elapsed = now - movePhaseAt;
      qualificationMoveTimes[0] += elapsed;
      qualificationAITimes[11] += elapsed;
      qualificationMoveTimes[4] = Math.max(qualificationMoveTimes[4], elapsed);
      movePhaseAt = now;
    }
    let blocked = false, collidedTank = null, collisionDistanceSquared = 0;
    if (tank.slideX || tank.slideZ) blocked = tank.x < -HULL_EDGE || tank.x > HULL_EDGE
      || tank.z < -HULL_EDGE || tank.z > HULL_EDGE
      || circleHitsObstacle(tank.x, tank.z, tank.collisionRadius);
    if (blocked && !tank.player) {
      /* A bot meeting scenery or the arena edge at an angle slides along
         it, as a hull would, instead of stopping dead: stopping made it back
         off and arc round every corner it clipped at a gap's mouth, then
         drive into the same corner again. The move loses its component into
         the deepest contact (a face, a corner's rounded tip, an edge), which
         also eases a hull round a gap's corner into the gap; failing that,
         the larger axis component alone, then the other. Meeting a face
         nearly head-on (under a third of the move left) still counts as
         blocked, so the unstick manoeuvre runs rather than a grind. */
      const moveX = tank.slideX * dt, moveZ = tank.slideZ * dt;
      const absX = moveX < 0 ? -moveX : moveX, absZ = moveZ < 0 ? -moveZ : moveZ;
      const xFirst = absX >= absZ;
      if (hullContact(tank.x, tank.z, tank.collisionRadius)) {
        const into = moveX * contactNormalX + moveZ * contactNormalZ;
        if (into < 0) {
          const slideX = moveX - into * contactNormalX, slideZ = moveZ - into * contactNormalZ;
          // A hair outwards: sliding round a corner's tip curves into it.
          const x = oldX + slideX + contactNormalX * .002;
          const z = oldZ + slideZ + contactNormalZ * .002;
          if ((slideX * slideX + slideZ * slideZ) * 9 >= moveX * moveX + moveZ * moveZ
              && x >= -HULL_EDGE && x <= HULL_EDGE && z >= -HULL_EDGE && z <= HULL_EDGE
              && !circleHitsObstacle(x, z, tank.collisionRadius)) {
            tank.x = x; tank.z = z;
            tank.slideX = slideX / dt; tank.slideZ = slideZ / dt;
            blocked = false;
          }
        }
      }
      for (let axis = 0; blocked && axis < 2; axis++) {
        const alongX = (axis === 0) === xFirst;
        const x = alongX ? oldX + moveX : oldX, z = alongX ? oldZ : oldZ + moveZ;
        if ((alongX ? absX : absZ) * 3 < absX + absZ
            || x < -HULL_EDGE || x > HULL_EDGE || z < -HULL_EDGE || z > HULL_EDGE
            || circleHitsObstacle(x, z, tank.collisionRadius)) continue;
        tank.x = x; tank.z = z;
        if (alongX) tank.slideZ = 0; else tank.slideX = 0;
        blocked = false;
      }
    }
    // The convoy blocks hulls; one already touching it may only back out.
    if ((tank.slideX || tank.slideZ) && !blocked && convoy.active) {
      const overlap = convoyOverlap(tank.x, tank.z, tank.collisionRadius);
      blocked = overlap > 0
        && overlap >= convoyOverlap(oldX, oldZ, tank.collisionRadius);
    }
    if ((tank.slideX || tank.slideZ) && !blocked) {
      for (let at = 0; at < MAX_TANKS; at++) {
        const other = tanks[at];
        if (!other.active || other.id === tank.id) continue;
        const dx = tank.x - other.x, dxSquared = dx * dx;
        let combinedSquared;
        if (tank.boss || other.boss) {
          // QuickJS's exponent opcode calls soft-float libm pow even for 2.
          // This finite, nonnegative radius only needs one multiplication.
          const combined = tank.collisionRadius + other.collisionRadius;
          combinedSquared = combined * combined;
        } else combinedSquared = CLASS_COMBINED_RADIUS_SQUARED[
          tank.classId * CLASSES.length + other.classId];
        if (dxSquared >= combinedSquared) continue;
        const dz = tank.z - other.z;
        const distanceSquared = dxSquared + dz * dz;
        if (distanceSquared < combinedSquared) {
          blocked = true;
          collidedTank = other;
          collisionDistanceSquared = distanceSquared;
          break;
        }
      }
    }
    if (blocked) {
      tank.slideX = tank.slideZ = 0;
      tank.x = oldX; tank.z = oldZ;
      let separatedFromTank = false;
      // Unstick existing overlaps without displacing the player or crossing
      // scenery. Rolling back alone preserves the trapping overlap.
      if (!tank.player && collidedTank) {
        let dx = oldX - collidedTank.x, dz = oldZ - collidedTank.z;
        let distance = Math.sqrt(dx * dx + dz * dz);
        if (distance < .001) {
          dx = tank.yawSine || (tank.id & 1 ? 1 : -1);
          dz = tank.yawCosine;
          distance = Math.sqrt(dx * dx + dz * dz) || 1;
        }
        const combined = (tank.boss || collidedTank.boss
          ? tank.collisionRadius + collidedTank.collisionRadius
          : Math.sqrt(CLASS_COMBINED_RADIUS_SQUARED[
            tank.classId * CLASSES.length + collidedTank.classId])) + .06;
        if (collisionDistanceSquared < combined * combined) {
          const separatedX = collidedTank.x + dx / distance * combined;
          const separatedZ = collidedTank.z + dz / distance * combined;
          if (Math.abs(separatedX) <= HULL_EDGE && Math.abs(separatedZ) <= HULL_EDGE
              && !circleHitsObstacle(separatedX, separatedZ,
                tank.collisionRadius) && !hullTouchesTank(tank, separatedX,
                separatedZ, tank.collisionRadius)) {
            tank.x = separatedX; tank.z = separatedZ;
            separatedFromTank = true;
          }
        }
      }
      tank.blockedTime = Math.min(1.2, tank.blockedTime + dt);
      if (separatedFromTank && collidedTank.team === tank.team) {
        /* A teammate in the way. Backing off is a stand-off from the
           target, so it ended at once and two teammates heading into each
           other (or one parked in a gap) rammed and separated every frame.
           Give way round it instead: a short reverse, then an arc turning
           away from the side it is on (the same rule for both hulls, so a
           head-on pair passes), kept to the end before steering resumes. */
        if (tank.avoidTime <= 0) {
          tank.avoidTime = 1.2;
          tank.avoidTurn = wrapAngle(Math.atan2(collidedTank.x - tank.x,
            collidedTank.z - tank.z) - tank.yaw) > 0 ? -1 : 1;
        }
        command.reverse = true;
        command.left = command.right = 1;
      } else if (separatedFromTank) {
        tank.backingOff = true;
        if (collidedTank.id === tank.target) tank.standoff = true;
        tank.avoidTime = 0;
        command.reverse = true;
        command.left = command.right = 1;
      } else if ((!tank.player || qualificationLongSoak)
          && tank.avoidTime <= 0 && (tank.player || tank.blockedTime >= .2)) {
        /* A bot gives sliding a moment (.2 s blocked) before the unstick
           manoeuvre: a hull easing round a gap's corner is blocked for a
           frame or two, and backing off then threw it out of line again. */
        /* Wedged: the last manoeuvre was blocked throughout. Routing's padded
           sight tests start inside any wall the hull touches and so fail; shove
           the hull to the nearest spot with a little clearance first. */
        if (!tank.player && tank.blockedTime >= 1.2
            && circleHitsObstacle(oldX, oldZ, tank.collisionRadius + .06, true)
            && findClearSpot(tank, oldX, oldZ, tank.collisionRadius + .06)) {
          tank.x = clearSpot.x; tank.z = clearSpot.z;
        }
        tank.avoidTime = 1.35;
        /* Back clear, then hold one forward arc around the obstruction. Pick
           the side with a clear look-ahead when only one is available; keep
           a deterministic choice when both are equivalent. */
        const leftYaw = tank.yaw + .85, rightYaw = tank.yaw - .85;
        const probeRadius = tank.collisionRadius;
        const leftBlocked = arcBlocked(oldX + Math.sin(leftYaw) * .9,
          oldZ + Math.cos(leftYaw) * .9, probeRadius);
        const rightBlocked = arcBlocked(oldX + Math.sin(rightYaw) * .9,
          oldZ + Math.cos(rightYaw) * .9, probeRadius);
        /* With both sides alike, arc towards where it is steering rather
           than alternating sides on every attempt (which dithered in
           place); the player in the long soak has no steering goal. */
        tank.avoidTurn = leftBlocked !== rightBlocked ? (leftBlocked ? -1 : 1)
          : tank.player ? -tank.avoidTurn
            : wrapAngle(Math.atan2(tank.strategySteerX - oldX,
              tank.strategySteerZ - oldZ) - tank.yaw) >= 0 ? 1 : -1;
        command.reverse = true;
        command.left = command.right = 1;
      }
    } else if (tank.blockedTime > 0) {
      tank.blockedTime -= dt * 2;
      if (tank.blockedTime < 0) tank.blockedTime = 0;
    }
    if (tank.avoidTime > 0) {
      tank.avoidTime -= dt;
      if (tank.avoidTime < 0) tank.avoidTime = 0;
    }
    if (qualificationAIActive) {
      const now = performance.now(), elapsed = now - movePhaseAt;
      qualificationMoveTimes[1] += elapsed;
      qualificationAITimes[12] += elapsed;
      qualificationMoveTimes[5] = Math.max(qualificationMoveTimes[5], elapsed);
      movePhaseAt = now;
    }
    const treadDx = tank.x - oldX, treadDz = tank.z - oldZ;
    tank.velocityX = dt > 0 ? treadDx / dt : 0;
    tank.velocityZ = dt > 0 ? treadDz / dt : 0;
    if (qualificationStats !== null && blocked && Math.abs(intendedTravel) > .001)
      qualificationStats.stuck[tank.id] += dt;
    // Corrections need their actual travel norm.
    const treadTravel = !blocked && terrain < 0 ? Math.abs(intendedTravel)
      : (treadDx || treadDz
        ? Math.sqrt(treadDx * treadDx + treadDz * treadDz) : 0);
    if (treadTravel > .001) {
      tank.treadDistance += treadTravel;
      if (tank.treadDistance >= .72) {
        tank.treadDistance -= .72;
        spawnDecal(1,
          tank.x - tank.yawSine * .38 * tank.scale,
          tank.z - tank.yawCosine * .38 * tank.scale,
          tank.yaw, .38 * tank.scale, .23 * tank.scale);
      }
    }
    // Placement initializes height; stationary tanks retain it.
    if (treadDx || treadDz) tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
    const aimLengthSquared = command.aimX * command.aimX
      + command.aimZ * command.aimZ;
    if (aimLengthSquared > .0025) {
      if (command.aimX !== tank.lastAimX || command.aimZ !== tank.lastAimZ) {
        tank.lastAimX = command.aimX;
        tank.lastAimZ = command.aimZ;
        tank.aimAngle = Math.atan2(command.aimX, command.aimZ);
      }
      const priorTurret = tank.turret;
      /* Kept in [-pi, pi] like the hull: an unwrapped turret made every
         wrapAngle of it loop once per accumulated turn, and overflowed
         the snapshot's 16-bit turret field past +-4 rad. */
      tank.turret = wrapAngle(approachAngle(priorTurret, tank.aimAngle, 3.8 * dt));
      if (tank.turret !== priorTurret) updateTankTurretCache(tank);
    }
    if (qualificationAIActive) {
      const now = performance.now(), elapsed = now - movePhaseAt;
      qualificationMoveTimes[2] += elapsed;
      qualificationAITimes[13] += elapsed;
      qualificationMoveTimes[6] = Math.max(qualificationMoveTimes[6], elapsed);
      movePhaseAt = now;
    }
    // Expired timers already satisfy the zero invariant.
    if (tank.cooldown > 0) {
      tank.cooldown -= dt; if (tank.cooldown < 0) tank.cooldown = 0;
    }
    if (tank.secondaryCooldown > 0) {
      tank.secondaryCooldown -= dt;
      if (tank.secondaryCooldown < 0) tank.secondaryCooldown = 0;
    }
    if (tank.commandBuff > 0) {
      tank.commandBuff -= dt; if (tank.commandBuff < 0) tank.commandBuff = 0;
    }
    if (tank.recoil > 0) {
      tank.recoil -= dt; if (tank.recoil < 0) tank.recoil = 0;
    }
    if (tank.hitFlash > 0) {
      tank.hitFlash -= dt; if (tank.hitFlash < 0) tank.hitFlash = 0;
    }
    if (tank.shield > 0) {
      tank.shield -= dt; if (tank.shield < 0) tank.shield = 0;
    }
    if (tank.boost > 0) {
      tank.boost -= dt; if (tank.boost < 0) tank.boost = 0;
    }
    if (tank.repair > 0) {
      tank.repair -= dt; if (tank.repair < 0) tank.repair = 0;
    }
    if (tank.gadgetCooldown > 0) {
      tank.gadgetCooldown -= dt;
      if (tank.gadgetCooldown < 0) tank.gadgetCooldown = 0;
    }
    if (tank.repair > 0)
      tank.health = Math.min(tank.maxHealth, tank.health + 8 * dt);
    if (tank.spawnGrace > 0) {
      tank.spawnGrace -= dt; if (tank.spawnGrace < 0) tank.spawnGrace = 0;
    }
    const humanFire = !botLeagueActive && !qualificationLongSoak
      && (tank.player || (online.active && tank.id < 2));
    if (humanFire) {
      if (command.fire) tank.fireCharge = Math.min(.8, tank.fireCharge + dt);
      else if (tank.fireHeld) {
        fireTank(tank, 0, tank.fireCharge >= CHARGE_THRESHOLD);
        tank.fireCharge = 0;
      }
      tank.fireHeld = !!command.fire;
    } else if (command.fire) fireTank(tank);
    if (command.secondary) fireSecondary(tank);
    if (command.gadget) useGadget(tank);
    if (command.ultimate) activateCommand(tank);
    /* Bot planning is staggered across frames. Its fire/gadget outputs are
       edge-triggered, so consume them here instead of repeating the action
       on the intervening movement-only frame. */
    if (!tank.player) {
      if (!humanFire) command.fire = false;
      command.secondary = command.gadget = command.ultimate = false;
    }
    if (qualificationAIActive) {
      const elapsed = performance.now() - movePhaseAt;
      qualificationMoveTimes[3] += elapsed;
      qualificationAITimes[14] += elapsed;
      qualificationMoveTimes[7] = Math.max(qualificationMoveTimes[7], elapsed);
    }
  }

  function spawnPickup(x, z) {
    for (let at = 0; at < MAX_PICKUPS; at++) {
      const pickup = pickups[at];
      if (pickup.active) continue;
      pickup.active = true; pickup.x = x; pickup.z = z; pickup.phase = 0;
      pickup.type = pickupTypes[state.kills % pickupTypes.length];
      return;
    }
  }

  function beginFinalKillBeat() {
    if (state.pendingClear) return;
    state.killBeat = .35;
    state.pendingClear = true;
    if (isOnslaught() && state.wave >= 5) unlockMedal(4);
    showToast(isOnslaught()
      ? `WAVE ${state.wave} CLEAR` : "ARENA CLEAR", 1.25);
  }

  function damageTank(tank, damage, attacker, originX = tank.x,
                      originZ = tank.z, bypassShield = false, bounceCount = 0) {
    if (!tank.active || tank.spawnGrace > 0) return false;
    /* Device qualification must remain a gameplay soak rather than becoming
       a measurement of the title panel after an unattended bot wins. The
       browser-visible mode is unchanged outside the explicit fixture query. */
    if (qualificationLongSoak && tank.player) return false;
    const incoming = Math.atan2(originX - tank.x, originZ - tank.z);
    const facing = Math.abs(wrapAngle(incoming - tank.yaw));
    const armorZone = facing < .8 ? "FRONT" : facing > 2.35 ? "REAR" : "SIDE";
    if (armorZone === "FRONT") damage *= .65;
    else if (armorZone === "REAR") damage *= 1.35;
    damage = Math.max(1, Math.round(damage));
    if (tank.shield > 0 && !bypassShield) {
      damage = Math.max(4, (damage * .25) | 0);
      tank.shield = Math.max(0, tank.shield - 1.2);
    }
    damage = campaign.damage(tank, damage, attacker, bounceCount);
    if (state.gameMode === MODE_BILLIARDS && !tank.player && tank.team !== tanks[0].team
        && !bounceCount && damage >= tank.health) damage = Math.max(0, tank.health - 1);
    tank.health -= damage;
    // A damage exchange re-arms both sides' guard patience.
    tank.guardIdle = 0;
    if (tanks[attacker]) tanks[attacker].guardIdle = 0;
    if (qualificationStats !== null) {
      if (armorZone === "FRONT") qualificationStats.frontDamage[tank.id] += damage;
      else if (armorZone === "REAR") qualificationStats.rearDamage[tank.id] += damage;
      if (tanks[attacker]) qualificationStats.damageContacts[attacker]++;
    }
    tank.hitFlash = .12;
    if (attacker === 0 && !tank.player) {
      state.hits++;
      state.hitConfirm = .18;
      sounds.hit();
    }
    if (tank.player) {
      state.damageTaken += damage;
      state.damageIndicator = .55;
      state.armorZone = armorZone;
      state.damageAngle = incoming;
      if (isOnslaught() && state.multiplier !== 1) {
        state.multiplier = 1;
        hudIndicatorDirty = true;
      }
    }
    if (tank.boss) {
      if (armorZone === "SIDE") {
        const side = (originX - tank.x) * tank.yawCosine
          - (originZ - tank.z) * tank.yawSine;
        if (side > 0) tank.rightTreadHealth = Math.max(0, tank.rightTreadHealth - damage);
        else tank.leftTreadHealth = Math.max(0, tank.leftTreadHealth - damage);
      } else tank.turretHealth = Math.max(0, tank.turretHealth - damage);
      if (!tank.leftTreadHealth && !tank.rightTreadHealth && !tank.turretHealth)
        tank.health = 0;
    }
    state.shake = Math.min(.22, state.shake + .07);
    if (tank.health > 0) {
      spawnParticles(tank.x, .35, tank.z, 6, 2.3);
      if (tank.player) sounds.damage();
      return true;
    }
    tank.active = false;
    spawnParticles(tank.x, .5, tank.z, 6, 3.2);
    queueWreckDecal(tank);
    sounds.explosion();
    if (state.gameMode === MODE_DUEL) {
      state.duelWinner = tank.id === 0 ? 1 : 0;
      setMode("victory"); return true;
    }
    if (tank.player) {
      state.lives--;
      if (!beginKillcam()) finishPlayerDeath();
    } else if (tank.team === 0) {
      tank.respawn = 3;
    } else {
      state.kills++;
      const killScore = 500 + (isOnslaught()
        ? Math.max(0, state.wave - 1) * 75 : state.arena * 150);
      state.score += killScore * (isOnslaught()
        ? state.multiplier : 1);
      if (attacker === 0 && bounceCount > 0) {
        state.bankKills++;
        const double = bounceCount > 1;
        if (double) state.doubleBanks++;
        state.score += double ? 500 : 250;
        showToast(double ? "DOUBLE BANK +500" : "BANK SHOT +250", 1.1);
        if (state.bankKills >= 3) unlockMedal(3);
      }
      if (tank.boss) state.score += 1500;
      if (isOnslaught())
        state.multiplier = Math.min(5, state.multiplier + 1);
      if (attacker === 0) awardCommand(15);
      if (attacker === 0) sounds.kill();
      if (attacker === 0 && state.kills % 2 === 0) spawnPickup(tank.x, tank.z);
      if (state.gameMode === MODE_CONTROL || state.gameMode === MODE_CONVOY) tank.respawn = 3.5;
      else if (!activeEnemyCount()) beginFinalKillBeat();
    }
    return true;
  }

  function finishPlayerDeath() {
    if (state.lives <= 0) { setMode("game-over"); return; }
    const tank = playerTank(), gadget = tank.gadget, classId = tank.classId;
    placeTank(tank, state.gameMode === MODE_CONVOY ? 1.6 : 0, -5.8,
      0, 0, gadget, "PLAYER", classId);
    tank.spawnGrace = 2.5;
    setMode("playing"); showToast(`REDEPLOY - ${state.lives} LEFT`, 1.2);
  }

  function captureKillcam() {
    if (state.mode !== "playing" || online.active || replayActive || botLeagueActive
        || qualificationTooling && !(qualificationHooks !== null && qualificationHooks.killcam)
        || state.wallTime - killcamLastTime < 1 / 15 || !boxInstanceProgram) return;
    killcamMatrixViews[killcamWrite].set(boxInstanceMatrices);
    killcamTintViews[killcamWrite].set(boxInstanceTints);
    killcamCameraViews[killcamWrite].set(viewProjection);
    killcamCounts[killcamWrite] = boxInstanceCount;
    killcamStaticCounts[killcamWrite] = staticIndexCount;
    killcamTimes[killcamWrite] = state.wallTime;
    killcamLastTime = state.wallTime;
    killcamWrite = (killcamWrite + 1) % KILLCAM_FRAMES;
    killcamCount = Math.min(KILLCAM_FRAMES, killcamCount + 1);
  }

  function beginKillcam() {
    if (replayActive || online.active || botLeagueActive || killcamCount < 2) return false;
    killcamRead = (killcamWrite - killcamCount + KILLCAM_FRAMES) % KILLCAM_FRAMES;
    killcamStartTime = killcamTimes[killcamRead]; killcamElapsed = 0;
    killcamDeathStaticCount = staticIndexCount;
    setMode("killcam"); showToast("KILLCAM", 3.2);
    return true;
  }

  function advanceKillcam(dt) {
    state.wallTime += dt; killcamElapsed += dt;
    /* The render ring is chronological, with a hard 48-record search bound. */
    for (let at = 0; at < KILLCAM_FRAMES; at++) {
      const next = (killcamRead + 1) % KILLCAM_FRAMES;
      if (next === killcamWrite || killcamTimes[next] > killcamStartTime + killcamElapsed) break;
      killcamRead = next;
    }
    sounds.tick(state.wallTime, playerTank(), "killcam");
    if (killcamElapsed >= 3) {
      staticIndexCount = killcamDeathStaticCount;
      killcamCount = 0; boxInstanceSlotOwners.fill(-1); retainedSceneryDirty = true;
      finishPlayerDeath();
    }
  }

  /* Each step flies every shell along its swept path (see the shell sweep):
     the nearest of a hull, the convoy (red shells), a stop and a bounce
     along the travelled segment wins, and a bounce continues the rest of
     the step from its contact. A shell covers its whole range (speed times
     its 3 s life, or less) whatever the frame rate. */
  /* Per owner team, the hulls its shells can hit (refreshed after a hit). */
  const shellTeamTargets = new Int32Array(2);
  function refreshShellTargets() {
    shellTeamTargets[0] = shellTeamTargets[1] = 0;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active) shellTeamTargets[tank.team ? 0 : 1] |= 1 << at;
    }
  }
  function updateBullets(dt) {
    let activeBullets = bulletActiveMask;
    if (activeBullets) refreshShellTargets();
    while (activeBullets) {
      const at = activeMaskIndex(activeBullets);
      activeBullets = (activeBullets & (activeBullets - 1)) >>> 0;
      const bullet = bullets[at];
      bullet.bounceFlash = Math.max(0, bullet.bounceFlash - dt);
      const flight = bullet.life < dt ? bullet.life : dt;
      bullet.life -= dt;
      if (flight > 0) sweepShell(bullet, at, bullet.speed * flight);
      if (bullet.active && bullet.life <= 0) setBulletActive(at, false);
    }
  }

  function sweepShell(bullet, at, length) {
    const owner = tanks[bullet.owner];
    let x = bullet.x, z = bullet.z;
    let dx = bullet.headingSine, dz = bullet.headingCosine;
    let prepared = false, active = 0;
    bullet.turns = 0;
    for (let contact = 0; contact < SHELL_MAX_CONTACTS; contact++) {
      /* With the ray index ready, the cells the segment's bounding box
         spans name its exact candidates; most steps cross open floor and
         cast nothing. Otherwise (an arena still being placed, a campaign
         wall moving) the cast walks every record. */
      if (rayGeometryArena === state.arena) {
        if (!prepared) {
          prepared = true;
          active = (rayActiveMask | (state.gateOpen ? 0 : rayGateMask)) >>> 0;
          if (bullet.overCover) active = (active & ~RAY_BARRIER_MASK) >>> 0;
          castSkip = bullet.pierceId;
          if (castSkip >= 0) active = (active & ~(1 << castSkip)) >>> 0;
        }
        const endX = x + dx * length, endZ = z + dz * length;
        let firstX = ((x < endX ? x : endX) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
        let lastX = ((x < endX ? endX : x) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
        let firstZ = ((z < endZ ? z : endZ) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
        let lastZ = ((z < endZ ? endZ : z) + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE | 0;
        const inside = endX > -ARENA_EDGE && endX < ARENA_EDGE
          && endZ > -ARENA_EDGE && endZ < ARENA_EDGE;
        if (!inside) {
          // Only an edge bounce reaches past the walls' cells.
          if (firstX < 0) firstX = 0;
          if (lastX > COLLISION_GRID_LAST) lastX = COLLISION_GRID_LAST;
          if (firstZ < 0) firstZ = 0;
          if (lastZ > COLLISION_GRID_LAST) lastZ = COLLISION_GRID_LAST;
        }
        let mask = 0;
        for (let cellZ = firstZ; cellZ <= lastZ; cellZ++)
          for (let cellX = firstX; cellX <= lastX; cellX++)
            mask |= shellCellMasks[cellZ * COLLISION_GRID_SIDE + cellX];
        mask = (mask & active) >>> 0;
        if (inside && !mask) {
          castLength = length; castKind = SHELL_RANGE;
        } else {
          castBegin(x, z, dx, dz, length);
          if (mask) castCandidates(mask, x, z, dx, dz);
        }
      } else {
        if (!prepared) {
          prepared = true;
          castSkip = bullet.pierceId;
          castActive = shellActiveMask(bullet.overCover);
          if (castSkip >= 0) castActive = (castActive & ~(1 << castSkip)) >>> 0;
        }
        castShell(x, z, dx, dz, length, bullet.overCover);
      }
      const reach = castLength;
      const targets = shellTeamTargets[owner.team ? 1 : 0] & ~(1 << owner.id);
      const hit = targets ? shellScanTanks(targets, x, z, dx, dz, reach) : -1;
      let hitAt = targets ? shellScanAt : reach, convoyHit = false;
      if (convoy.active && owner.team === 1) {
        // The crawler: strictly within .48 squared of its centre.
        const fromX = convoy.x - x, fromZ = convoy.z - z;
        const along = fromX * dx + fromZ * dz;
        const distanceSquared = fromX * fromX + fromZ * fromZ;
        const offSquared = distanceSquared - along * along;
        if (along > -1 && along < hitAt + 1 && offSquared < .48) {
          let touch = along - Math.sqrt(.48 - offSquared);
          if (touch < 0) touch = distanceSquared < .48 ? 0 : Infinity;
          if (touch < hitAt || (hit < 0 && touch <= hitAt)) {
            hitAt = touch; convoyHit = true;
          }
        }
      }
      if (hit >= 0 || convoyHit) {
        bullet.x = x + dx * hitAt; bullet.z = z + dz * hitAt;
        setBulletActive(at, false);
        if (convoyHit) {
          convoy.health -= Math.max(6, bullet.damage * .45);
          spawnParticles(convoy.x, .4, convoy.z, 5, 1.6);
          if (convoy.health <= 0) {
            convoy.active = false;
            setMode("game-over");
          }
          return;
        }
        const damaged = damageTank(tanks[hit], bullet.damage, bullet.owner,
          bullet.x - bullet.vx * .04, bullet.z - bullet.vz * .04,
          bullet.bypassShield, bullet.bounceCount);
        refreshShellTargets();
        if (damaged && qualificationStats !== null) qualificationStats.hits[owner.id]++;
        spawnDecal(2, bullet.x, bullet.z, bullet.heading, .42, .34);
        if (bullet.owner === 0 && damaged) awardCommand(5);
        return;
      }
      x += dx * reach; z += dz * reach;
      if (castKind === SHELL_RANGE) break;
      length -= reach;
      if (castKind === SHELL_STOP) {
        bullet.x = x; bullet.z = z;
        if (castId < 4) {
          explodeCrate(castId, bullet.owner, 0);
          setBulletActive(at, false);
          return;
        }
        const barrier = barriers[castId - 20];
        barrier.health -= Math.max(1, bullet.barrierDamage | 0);
        spawnParticles(x, .35, z, 6, 1.7);
        spawnDecal(2, x, z, bullet.heading, .48, .38);
        if (barrier.health <= 0) {
          barrier.active = false;
          invalidateBotNavigation();
          retainedSceneryDirty = true;
          state.barriersBroken++;
          state.score += bullet.owner === 0 ? 125 : 0;
          if (bullet.owner === 0) awardCommand(10);
          showToast("BARRIER BREACHED", .8);
          sounds.explosion(sounds.blast.barrier);
        }
        if (bullet.pierce > 0) {
          // Through it: this barrier is passed over until the shell is clear.
          bullet.pierce--;
          bullet.pierceId = castSkip = castId;
          active = (active & ~(1 << castId)) >>> 0;
          castActive = (castActive & ~(1 << castId)) >>> 0;
          continue;
        }
        if (qualificationStats !== null) qualificationStats.wallShots[owner.id]++;
        setBulletActive(at, false);
        return;
      }
      if (castEmbedded || bullet.bounces <= 0) {
        // A charged shell, a spent one, or one a gate closed over.
        if (qualificationStats !== null) qualificationStats.wallShots[owner.id]++;
        bullet.x = x; bullet.z = z;
        spawnDecal(2, x, z, bullet.heading, .44, .34);
        setBulletActive(at, false);
        return;
      }
      shellReflect(dx, dz);
      bullet.bounces--;
      bullet.bounceCount++;
      if (bulletTurns !== null) {
        bulletTurns[at * 8 + bullet.turns * 2] = x;
        bulletTurns[at * 8 + bullet.turns * 2 + 1] = z;
      }
      bullet.turns++;
      // Visual only: the trail's vertex is the contact.
      if (bulletTracer[at] >= 0) tracerVertex(bulletTracer[at], x, z);
      if (bullet.owner === playerTank().id) bullet.bounceFlash = .12;
      state.ricochets++;
      if (bullet.owner === 0) awardCommand(6);
      if (shellFlipX) {
        dx = -dx;
        bullet.vx = -bullet.vx;
        bullet.headingSine = -bullet.headingSine;
        bullet.heading = -bullet.heading;
      }
      if (shellFlipZ) {
        dz = -dz;
        bullet.vz = -bullet.vz;
        bullet.headingCosine = -bullet.headingCosine;
        bullet.heading = wrapAngle(Math.PI - bullet.heading);
      }
      sounds.ricochet();
    }
    bullet.x = x; bullet.z = z;
    if (bullet.pierceId >= 0) {
      const barrier = barriers[bullet.pierceId - 20];
      if (x < barrier.left - SHELL_RADIUS || x > barrier.right + SHELL_RADIUS
          || z < barrier.top - SHELL_RADIUS || z > barrier.bottom + SHELL_RADIUS)
        bullet.pierceId = -1;
    }
  }

  function updatePickups(dt) {
    const player = playerTank();
    for (let at = 0; at < MAX_PICKUPS; at++) {
      const pickup = pickups[at];
      if (!pickup.active) continue;
      pickup.phase += dt * 2.7;
      if (!player.active) continue;
      const dx = player.x - pickup.x, dz = player.z - pickup.z;
      if (dx * dx + dz * dz > .65) continue;
      pickup.active = false;
      if (pickup.type === "COOLANT") {
        player.gadgetCooldown = Math.max(0, player.gadgetCooldown - 6);
        showToast("GADGET COOLED", 1);
      } else {
        player.health = Math.min(player.maxHealth, player.health + 30);
        showToast("ARMOR PATCHED", 1);
      }
      sounds.pickup();
    }
  }

  function updateMinesAndSmoke(dt) {
    for (let smokeAt = 0; smokeAt < smokeClouds.length; smokeAt++) {
      const smoke = smokeClouds[smokeAt];
      if (!smoke.active) continue;
      smoke.life -= dt;
      if (smoke.life <= 0) {
        smoke.active = false;
        activeSmokeCount--;
      }
    }
    for (let at = 0; at < MAX_MINES; at++) {
      const mine = mines[at];
      if (!mine.active) continue;
      mine.arm = Math.max(0, mine.arm - dt);
      mine.life -= dt;
      if (mine.life <= 0) { mine.active = false; continue; }
      if (mine.arm > 0) continue;
      let triggered = false;
      for (let tankAt = 0; tankAt < MAX_TANKS; tankAt++) {
        const tank = tanks[tankAt];
        if (!tank.active || tank.team === mine.team) continue;
        const dx = tank.x - mine.x, dz = tank.z - mine.z;
        if (dx * dx + dz * dz < 1.15) { triggered = true; break; }
      }
      if (!triggered) continue;
      mine.active = false;
      spawnParticles(mine.x, .15, mine.z, 14, 3);
      const killsBefore = state.kills;
      for (let tankAt = 0; tankAt < MAX_TANKS; tankAt++) {
        const tank = tanks[tankAt];
        if (!tank.active || tank.team === mine.team) continue;
        const dx = tank.x - mine.x, dz = tank.z - mine.z;
        if (dx * dx + dz * dz < 2.2)
          damageTank(tank, 38, mine.owner, mine.x, mine.z);
      }
      if (state.kills - killsBefore >= 3 && mine.owner === 0) unlockMedal(2);
      state.shake = Math.max(state.shake, .18);
      sounds.explosion(sounds.blast.mine);
    }
  }

  function updateRespawns(dt) {
    if (state.gameMode === MODE_SURVIVAL || state.gameMode === MODE_BILLIARDS
        || state.gameMode === MODE_DUEL || isOnslaught()) return;
    for (let at = 1; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active || tank.respawn <= 0) continue;
      tank.respawn -= dt;
      if (tank.respawn > 0) continue;
      const team = tank.team;
      const x = team === 0 ? -2.2 : 2.2;
      const z = team === 0 ? -5.7 : 5.7;
      (team === 0 ? placeTank : placeFoe)(tank, x + (at & 1 ? .8 : -.8), z,
        team === 0 ? 0 : Math.PI, team, tank.gadget, tank.role, tank.classId);
      validateRespawn(tank);
      tank.spawnGrace = 2;
    }
  }

  function updateParticles(dt) {
    let low = particleActiveLowMask, high = particleActiveHighMask;
    while (low) {
      const at = activeMaskIndex(low);
      low = (low & (low - 1)) >>> 0;
      const particle = particles[at];
      particle.life -= dt;
      if (particle.life <= 0) { setParticleActive(at, false); continue; }
      particle.x += particle.vx * dt;
      particle.y += particle.vy * dt;
      particle.z += particle.vz * dt;
      particle.vy -= 3.6 * dt;
    }
    while (high) {
      const at = 32 + activeMaskIndex(high);
      high = (high & (high - 1)) >>> 0;
      const particle = particles[at];
      particle.life -= dt;
      if (particle.life <= 0) { setParticleActive(at, false); continue; }
      particle.x += particle.vx * dt;
      particle.y += particle.vy * dt;
      particle.z += particle.vz * dt;
      particle.vy -= 3.6 * dt;
    }
  }

  function updateObjective(dt) {
    if (state.gameMode === MODE_DUEL) return;
    const wasOpen = state.gateOpen;
    if (state.gameMode === MODE_SURVIVAL || state.gameMode === MODE_BILLIARDS || isOnslaught()) {
      // Campaign reinforcements can re-seal the gate: never on a hull.
      state.gateOpen = activeEnemyCount()
        <= Math.max(1, (ARENAS[state.arena].enemies / 2) | 0)
        || (wasOpen && gateOccupied());
    } else if (state.gameMode === MODE_CONTROL) {
      let blue = 0, red = 0;
      for (let at = 0; at < MAX_TANKS; at++) {
        const tank = tanks[at];
        if (!tank.active) continue;
        const distance = tank.x * tank.x + tank.z * tank.z;
        if (distance > 2.9) continue;
        if (tank.team === 0) blue++; else red++;
      }
      if (blue > 0 && red === 0)
        state.blueControl = Math.min(100, state.blueControl + dt * 11);
      else if (red > 0 && blue === 0)
        state.redControl = Math.min(100, state.redControl + dt * 9);
      if (botLeagueActive) {
        if (blue > 0 && red === 0) qualificationStats.control[0] += dt;
        if (red > 0 && blue === 0) qualificationStats.control[1] += dt;
      }
      state.gateOpen = state.blueControl >= 18 || state.redControl >= 18;
      const player = playerTank();
      if (player.active && blue > 0 && red === 0
          && player.x * player.x + player.z * player.z <= 2.9) {
        state.objectiveTicks += dt;
        awardCommand(dt * 5);
      }
      if (state.blueControl >= 100) setMode("victory");
      else if (state.redControl >= 100) setMode("game-over");
    } else if (convoy.active) {
      const player = playerTank();
      const playerDx = player.x - convoy.x, playerDz = player.z - convoy.z;
      const playerDistance = playerDx * playerDx + playerDz * playerDz;
      let contested = false;
      for (let at = 0; at < MAX_TANKS; at++) {
        const tank = tanks[at];
        if (!tank.active || tank.team === 0) continue;
        const dx = tank.x - convoy.x, dz = tank.z - convoy.z;
        if (dx * dx + dz * dz < 5.3) { contested = true; break; }
      }
      if (player.active && playerDistance < 9 && !contested) {
        convoy.progress = Math.min(1, convoy.progress + dt * .045);
        state.objectiveTicks += dt;
        awardCommand(dt * 5);
      }
      // Stalled by a hull it cannot nudge: progress stays with the position.
      if (!moveConvoy(-1.8 + Math.sin(convoy.progress * Math.PI * 2) * 1.15,
          -4.8 + convoy.progress * 10.6)) convoy.progress = (convoy.z + 4.8) / 10.6;
      // The crawler rams through a barrier on its route, never through it.
      for (let at = 0; at < MAX_BARRIERS; at++) {
        const barrier = barriers[at];
        if (!barrier.active || barrier.right <= convoy.x - CONVOY_HALF_WIDTH
            || barrier.left >= convoy.x + CONVOY_HALF_WIDTH
            || barrier.bottom <= convoy.z - CONVOY_HALF_DEPTH
            || barrier.top >= convoy.z + CONVOY_HALF_DEPTH) continue;
        barrier.active = false; state.barriersBroken++;
        invalidateBotNavigation(); retainedSceneryDirty = true;
        spawnParticles(barrier.x, .35, barrier.z, 6, 1.7);
        showToast("BARRIER BREACHED", .8); sounds.explosion(sounds.blast.barrier);
      }
      // ...and crushes a crate on its route rather than passing through it.
      for (let at = 0; at < crates.length; at++) {
        const crate = crates[at];
        if (!crate.active || convoyOverlap(crate.x, crate.z, .31) <= 0) continue;
        crate.active = false; invalidateBotNavigation();
        spawnParticles(crate.x, .35, crate.z, 4, 1.7);
      }
      state.gateOpen = convoy.progress >= .22;
      if (convoy.progress >= 1) {
        convoy.active = false;
        state.score += 2500;
        setMode("victory");
      }
    }
    if (wasOpen !== state.gateOpen) {
      retainedSceneryDirty = true;
      // The last kill's ARENA CLEAR outranks a gate opening the same frame.
      if (!state.pendingClear)
        showToast(state.gateOpen ? "GATE OPEN" : "GATE SEALED", .75);
    }
  }

  function gateOccupied() {
    const gates = ARENA_GATE_BOUNDS[state.arena];
    for (let at = 0; at < ARENA_GATE_COUNTS[state.arena] * 4; at += 4) {
      for (let id = 0; id < MAX_TANKS; id++) {
        const tank = tanks[id], x = tank.x, z = tank.z;
        if (!tank.active) continue;
        const dx = x < gates[at] ? x - gates[at] : x > gates[at + 1] ? x - gates[at + 1] : 0;
        const dz = z < gates[at + 2] ? z - gates[at + 2] : z > gates[at + 3] ? z - gates[at + 3] : 0;
        if (dx * dx + dz * dz < tank.collisionRadius * tank.collisionRadius) return true;
      }
    }
    return false;
  }

  const hudCache = {
    score: -1, arena: -1, health: -1, maximum: -1, classId: -1,
    secondary: -1, gadget: "", gadgetCooldown: -1, lives: -1,
    blue: -1, red: -1, convoyProgress: -1, convoyHealth: -1,
    gameMode: -1, command: -1, commandEnabled: false, enemyCount: -1,
    wave: -1, multiplier: -1,
  };
  let authoredHudWrites = 0;
  function updateHud(force = false) {
    const player = playerTank();
    const displayClass = state.mode === "title"
      ? state.gameMode === MODE_DAILY ? dailySeed(state.dailyDay) % 3 : state.classChoice : player.classId;
    const displayProfile = CLASSES[displayClass];
    const displayHealth = state.mode === "title" ? displayProfile.health : player.health;
    const displayMaximum = state.mode === "title" ? displayProfile.health : player.maxHealth;
    const secondaryStep = Math.ceil(Math.max(0, player.secondaryCooldown));
    const gadgetStep = Math.ceil(Math.max(0, player.gadgetCooldown));
    const blueStep = state.blueControl | 0;
    const redStep = state.redControl | 0;
    const convoyStep = (convoy.progress * 100) | 0;
    /* Quantize: raw fractional health misses this cache every frame. */
    const convoyHealthStep = Math.max(0, Math.round(convoy.health));
    const commandStep = state.commandMeter | 0;
    const enemyCount = state.gameMode === MODE_SURVIVAL || state.gameMode === MODE_BILLIARDS || isOnslaught()
      ? activeEnemyCount() : 0;
    if (!force && hudCache.score === state.score
        && hudCache.arena === state.arena
        && hudCache.health === displayHealth
        && hudCache.maximum === displayMaximum
        && hudCache.classId === displayClass
        && hudCache.secondary === secondaryStep
        && hudCache.gadget === player.gadget
        && hudCache.gadgetCooldown === gadgetStep
        && hudCache.lives === state.lives
        && hudCache.blue === blueStep && hudCache.red === redStep
        && hudCache.convoyProgress === convoyStep
        && hudCache.convoyHealth === convoyHealthStep
        && hudCache.gameMode === state.gameMode
        && hudCache.command === commandStep
        && hudCache.commandEnabled === preferences.command
        && hudCache.enemyCount === enemyCount
        && hudCache.wave === state.wave
        && hudCache.multiplier === state.multiplier) return;
    hudCache.score = state.score;
    hudCache.arena = state.arena;
    hudCache.health = displayHealth;
    hudCache.maximum = displayMaximum;
    hudCache.classId = displayClass;
    hudCache.secondary = secondaryStep;
    hudCache.gadget = player.gadget;
    hudCache.gadgetCooldown = gadgetStep;
    hudCache.lives = state.lives;
    hudCache.blue = blueStep;
    hudCache.red = redStep;
    hudCache.convoyProgress = convoyStep;
    hudCache.convoyHealth = convoyHealthStep;
    hudCache.gameMode = state.gameMode;
    hudCache.command = commandStep;
    hudCache.commandEnabled = preferences.command;
    hudCache.enemyCount = enemyCount;
    hudCache.wave = state.wave;
    hudCache.multiplier = state.multiplier;
    const scoreText = String(state.score).padStart(5, "0");
    const arenaText = state.gameMode === MODE_DUEL ? `P${state.duelTurn + 1} TURN`
      : state.gameMode === MODE_SURVIVAL || state.gameMode === MODE_BILLIARDS
      ? `A${state.arena + 1} F${enemyCount}`
      : state.gameMode === MODE_CONTROL
        ? `C${state.blueControl | 0}-${state.redControl | 0}`
        : state.gameMode === MODE_CONVOY
          ? `V${(convoy.progress * 100) | 0}`
          : `W${state.wave} X${state.multiplier} F${enemyCount}`;
    const armorText = `HP${Math.max(0, displayHealth)} L${state.lives}`;
    const secondary = secondaryStep > 0 ? `${secondaryStep}` : "R";
    const displayGadget = state.mode === "title"
      ? GADGETS[state.gameMode === MODE_DAILY ? (dailySeed(state.dailyDay) >>> 8) % 5 : state.gadgetChoice]
      : player.gadget;
    const gadget = state.mode !== "title" && gadgetStep > 0
      ? `${gadgetStep}` : "R";
    const gadgetText = `${displayProfile.secondary.slice(0, 3)} ${secondary}`
      + `/${displayGadget.slice(0, 3)} ${gadget}`;
    const commandText = preferences.command
      ? state.commandMeter >= 100 ? "CMD R" : `CMD ${commandStep}`
      : "";
    if (scoreText !== hudScore || arenaText !== hudArena
        || armorText !== hudArmor || gadgetText !== hudGadget
        || commandText !== hudCommand) {
      hudScore = scoreText;
      hudArena = arenaText;
      hudArmor = armorText;
      hudGadget = gadgetText;
      hudCommand = commandText;
      hudMeshDirty = true;
    }
    /* The authored HUD remains useful on the title and pause surfaces. During
       play, its retained WebGL counterpart is authoritative and these DOM
       writes are deliberately absent from the frame loop. */
    if (authoredSurfaceVisible(state.mode)) {
      // put()/hide(): an unchanged write is still a relayout on the PSP.
      authoredHudWrites++;
      put(ui.score, scoreText);
      put(ui.arena, state.gameMode === MODE_SURVIVAL || state.gameMode === MODE_BILLIARDS
        ? `ARENA ${state.arena + 1} - ${enemyCount} FOES`
        : state.gameMode === MODE_CONTROL
          ? `CONTROL ${state.blueControl | 0}-${state.redControl | 0}`
          : state.gameMode === MODE_CONVOY
            ? `CONVOY ${(convoy.progress * 100) | 0}% - ${convoyHealthStep}`
            : `WAVE ${state.wave} - X${state.multiplier} - ${enemyCount} FOES`);
      put(ui.armor, `${displayProfile.name} `
        + `${Math.max(0, displayHealth)}/${displayMaximum} - ${state.lives}`);
      put(ui.gadget, `${displayProfile.secondary} ${secondary}`
        + ` - ${displayGadget} ${gadget}`);
      if (ui.commandStatus) {
        hide(ui.commandStatus, !preferences.command);
        if (preferences.command) put(ui.commandStatus, commandText);
      }
    }
  }

  function simulationDigest() {
    let hash = 2166136261;
    function mix(value) { hash = Math.imul(hash ^ (Math.round(value * 1000000) | 0), 16777619) >>> 0; }
    mix(randomState); mix(state.score); mix(state.lives); mix(state.wave);
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      mix(tank.active); if (!tank.active) continue;
      mix(tank.x); mix(tank.z); mix(tank.health);
      mix(tank.yaw); mix(tank.turret); mix(tank.cooldown); mix(tank.fireCharge);
    }
    for (let at = 0; at < MAX_BULLETS; at++) {
      const bullet = bullets[at];
      mix(bullet.active); if (!bullet.active) continue;
      mix(bullet.x); mix(bullet.z);
      mix(bullet.vx); mix(bullet.vz); mix(bullet.life); mix(bullet.bounceCount);
    }
    return hash;
  }

  function beginReplayRecording() {
    replayPending = false; replayRecording = true; replayVerified = false;
    replayCount = 0; replayLastSim = state.time; replayLastWall = state.wallTime;
    replayLastFrame = state.frames;
    replayConfig.set([REPLAY_VERSION, state.gameMode, state.classChoice, state.gadgetChoice,
      state.difficultyChoice, preferences.command ? 1 : 0, preferences.camera,
      state.arena, state.arenaSeed, state.dailyDay, state.time, state.wallTime,
      state.frames, randomState >>> 0, 0, 0]);
  }

  function recordReplayInput(dt) {
    if (!replayRecording || replayActive || replayCount >= REPLAY_CAPACITY) return;
    const command = playerTank().command, base = replayCount++ * REPLAY_WORDS;
    replayLog[base] = dt;
    replayLog[base + 1] = command.left; replayLog[base + 2] = command.right;
    replayLog[base + 3] = command.aimX; replayLog[base + 4] = command.aimZ;
    replayLog[base + 5] = replayFrameSim === replayLastSim ? NaN : replayFrameSim;
    replayLog[base + 6] = replayFrameWall === replayLastWall ? NaN : replayFrameWall;
    let flags = command.reverse ? 1 : 0;
    if (command.fire) flags |= 2;
    if (command.secondary) flags |= 4;
    if (command.gadget) flags |= 8;
    if (command.ultimate) flags |= 16;
    if (state.mode === "paused" || state.mode === "duel-pass") flags |= 32;
    if (command.lunge) flags |= 64;
    replayLog[base + 7] = flags + (state.frames - replayLastFrame) * 128;
    replayLastFrame = state.frames;
  }

  function finishReplayRecording() {
    if (!replayRecording) return;
    replayRecording = false; replayDigest = simulationDigest();
    replayConfig[14] = replayDigest; replayConfig[15] = replayCount;
    // Slot 5: Command (bit 0) and the aim guide level the run used (x2).
    replayConfig[5] = (preferences.command ? 1 : 0) + runAimLevel * 2;
    ui.replayLast.disabled = false;
    ui.replayShare.disabled = false;
  }

  function startReplay() {
    if (!replayCount || replayRecording || replayActive || online.active) return false;
    replayActive = true; replayPending = false; replayCursor = 0;
    borrowReplaySettings();
    runAimLevel = replayConfig[5] >> 1;
    resetGame();
    arenaGenerationPhase = ARENA_GENERATION_IDLE;
    state.arenaSeed = replayConfig[8]; state.dailyDay = replayConfig[9];
    if (replayConfig[7] === GENERATED_ARENA_INDEX) {
      arenaGenerator.materialize(state.arenaSeed);
      fillArenaSpatialData(GENERATED_ARENA_INDEX); generatedGeometryDirty = true;
    }
    beginArena(replayConfig[7]);
    state.time = replayConfig[10]; state.wallTime = replayConfig[11];
    state.frames = replayPlaybackFrame = replayConfig[12];
    randomState = replayConfig[13] >>> 0;
    bulletSpawnCursor = particleSpawnCursor = particleTemplateCursor = 0;
    if (qualificationHooks !== null) qualificationHooks.botsFrozen = false;
    setMode("playing"); showToast("REPLAY", 1);
    return true;
  }

  function replayChecksum(length) {
    let hash = 2166136261;
    for (let at = 0; at < length; at++) hash = Math.imul(hash ^ replayCodec[at], 16777619) >>> 0;
    return hash;
  }

  function exportReplay() {
    if (!replayCount || replayRecording) return "";
    let write = 0;
    for (let at = 0; at < 16; at++, write += 8) replayCodecView.setFloat64(write, replayConfig[at], true);
    replayPrevious.fill(0);
    for (let frame = 0; frame < replayCount; frame++) {
      const base = frame * REPLAY_WORDS;
      let mask = 0;
      for (let word = 0; word < REPLAY_WORDS; word++)
        if (!Object.is(replayLog[base + word], replayPrevious[word])) mask |= 1 << word;
      if (write > replayCodec.length - 70) return "";
      replayCodec[write++] = mask;
      if (!mask) {
        let run = 1;
        while (frame + run < replayCount && run < 65535) {
          let equal = true;
          for (let word = 0; word < REPLAY_WORDS; word++)
            if (!Object.is(replayLog[(frame + run) * REPLAY_WORDS + word], replayPrevious[word])) { equal = false; break; }
          if (!equal) break;
          run++;
        }
        replayCodecView.setUint16(write, run, true); write += 2; frame += run - 1;
      } else for (let word = 0; word < REPLAY_WORDS; word++) if (mask & (1 << word)) {
        const value = replayLog[base + word]; replayPrevious[word] = value;
        replayCodecView.setFloat64(write, value, true); write += 8;
      }
    }
    const checksum = replayChecksum(write);
    /* Up to 128 KiB: convert in 2 KiB chunks rather than appending one
       character at a time. */
    const parts = [];
    for (let at = 0; at < write; at += 2048)
      parts.push(String.fromCharCode.apply(null,
        replayCodec.subarray(at, Math.min(write, at + 2048))));
    return `TR1.${checksum.toString(16)}.${btoa(parts.join(""))}`;
  }

  /* The largest value each small config slot may hold: mode, class,
     gadget, difficulty, the Command bit plus twice the aim level, camera,
     arena (slot 0 is the version). */
  const REPLAY_CONFIG_LIMITS = [0, GAME_MODES.length - 1, CLASSES.length - 1,
    GADGETS.length - 1, DIFFICULTIES.length - 1, 1 + 2 * (AIM_LEVEL_NAMES.length - 1),
    CAMERA_MODES.length - 1, ARENAS.length - 1];
  function importReplay(code) {
    if (typeof code !== "string" || code.length > 180000 || replayRecording || replayActive) return false;
    code = code.trim();
    const match = /^TR1\.([0-9a-f]{1,8})\./.exec(code);
    if (!match) return false;
    const payload = code.slice(match[0].length);
    if (/[^A-Za-z0-9+/=]/.test(payload)) return false;
    let binary;
    try { binary = atob(payload); } catch (_) { return false; }
    if (binary.length < 129 || binary.length > replayCodec.length) return false;
    for (let at = 0; at < binary.length; at++) replayCodec[at] = binary.charCodeAt(at);
    if (replayChecksum(binary.length) !== parseInt(match[1], 16)) return false;
    const count = replayCodecView.getFloat64(120, true);
    if (!Number.isInteger(count) || count < 1 || count > REPLAY_CAPACITY
        || replayCodecView.getFloat64(0, true) !== REPLAY_VERSION) return false;
    for (let at = 1; at < 16; at++)
      if (!Number.isFinite(replayCodecView.getFloat64(at * 8, true))) return false;
    for (let at = 1; at <= 7; at++) {
      const value = replayCodecView.getFloat64(at * 8, true);
      if (!Number.isInteger(value) || value < 0 || value > REPLAY_CONFIG_LIMITS[at])
        return false;
    }
    for (const at of [8, 9, 12, 13, 14]) {
      const value = replayCodecView.getFloat64(at * 8, true);
      if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) return false;
    }
    for (let at = 10; at <= 11; at++) {
      const value = replayCodecView.getFloat64(at * 8, true);
      if (value < 0 || value > 1e9) return false;
    }
    /* Validate before replacing records; both passes reuse fixed scratch. */
    for (let pass = 0; pass < 2; pass++) {
      let read = 128, frame = 0;
      replayPrevious.fill(0);
      while (frame < count) {
        if (read >= binary.length) return false;
        const mask = replayCodec[read++]; let run = 1;
        if (!mask) {
          if (read > binary.length - 2) return false;
          run = replayCodecView.getUint16(read, true); read += 2;
          if (!run || run > count - frame) return false;
        } else for (let word = 0; word < REPLAY_WORDS; word++) if (mask & (1 << word)) {
          if (read > binary.length - 8) return false;
          replayPrevious[word] = replayCodecView.getFloat64(read, true); read += 8;
        }
        if (!Number.isFinite(replayPrevious[0]) || replayPrevious[0] < 0 || replayPrevious[0] > 1 / 30
            || !Number.isFinite(replayPrevious[7]) || !Number.isInteger(replayPrevious[7])
            || replayPrevious[7] < 0 || replayPrevious[7] > 128000000) return false;
        for (let word = 1; word <= 4; word++)
          if (!Number.isFinite(replayPrevious[word]) || Math.abs(replayPrevious[word]) > 1.01) return false;
        for (let word = 5; word <= 6; word++)
          if (!Number.isNaN(replayPrevious[word]) && (!Number.isFinite(replayPrevious[word])
              || replayPrevious[word] < 0 || replayPrevious[word] > 1e9)) return false;
        if (pass) for (let repeat = 0; repeat < run; repeat++)
          replayLog.set(replayPrevious, (frame + repeat) * REPLAY_WORDS);
        frame += run;
      }
      if (read !== binary.length) return false;
    }
    for (let at = 0; at < 16; at++) replayConfig[at] = replayCodecView.getFloat64(at * 8, true);
    replayCount = count; replayDigest = replayConfig[14] >>> 0;
    replayPending = false; return true;
  }

  function update(dt, profileFrame = false) {
    if (state.mode === "playing") noteAimLevel();
    if (state.mode === "killcam") { advanceKillcam(dt); return; }
    if (replayActive) {
      // The recorded command is applied where live input is polled
      // (applyReplayCommand); a campaign stage change resets it before then.
      const base = replayCursor * REPLAY_WORDS;
      dt = replayLog[base];
      if (!Number.isNaN(replayLog[base + 5])) state.time = replayLog[base + 5];
      if (!Number.isNaN(replayLog[base + 6])) state.wallTime = replayLog[base + 6];
      const encoded = replayLog[base + 7], flags = encoded % 128;
      replayPlaybackFrame += Math.floor(encoded / 128); state.frames = replayPlaybackFrame;
      if (flags & 32) state.mode = state.gameMode === MODE_DUEL ? "duel-pass" : "paused";
      else if (state.mode === "paused" || state.mode === "duel-pass") {
        playerTank().fireHeld = false; playerTank().fireCharge = 0;
        state.mode = "playing";
      }
    } else if (replayPending && state.mode === "playing"
        && arenaGenerationPhase === ARENA_GENERATION_IDLE && !botLeagueActive)
      beginReplayRecording();
    replayFrameSim = state.time; replayFrameWall = state.wallTime;
    stepping = true;
    advanceSimulation(dt, profileFrame);
    stepping = false;
    if (replayRecording) {
      replayLastSim = state.time; replayLastWall = state.wallTime;
      if (replayCount >= REPLAY_CAPACITY) replayTruncated = true;
      if (replayTruncated || state.mode === "game-over" || state.mode === "victory"
          || (state.mode === "killcam" && state.lives <= 0)) finishReplayRecording();
    }
    if (replayActive && ++replayCursor >= replayCount) {
      replayVerified = simulationDigest() === replayDigest;
      endReplay(); setMode("replay-done");
    }
  }

  function advanceSimulation(dt, profileFrame = false) {
    let profileAt = profileFrame ? performance.now() : 0;
    simulationStep++;
    const wallDt = dt;
    state.wallTime += wallDt;
    if (state.killBeat > 0) {
      state.killBeat = Math.max(0, state.killBeat - wallDt);
      dt *= .28;
      if (state.killBeat === 0 && state.pendingClear) {
        state.pendingClear = false;
        state.transition = isOnslaught()
          ? Math.max(.72, 1.35 - state.wave * .055) : 1.35;
        /* Keep the clear toast for the whole transition; the next arena's
           (or wave's) own toast replaces it. */
        if (toastUntil)
          toastUntil = Math.max(toastUntil, state.time + state.transition + .25);
        if (isOnslaught()) recordOnslaughtBest();
        sounds.waveClear();
        setMode("arena-clear");
      }
    }
    state.time += dt;
    // A menu mode's panel shows as campaign.js fills it (showRunPanel).
    if (panelReveal) { panelReveal = false; ui.panel.hidden = false; }
    campaign.tick(dt);
    const audioPlayer = playerTank();
    sounds.tick(state.wallTime, audioPlayer, state.mode);
    if (arenaGenerationPhase !== ARENA_GENERATION_IDLE) {
      updateArenaGeneration();
      return;
    }
    if (state.mode === "playing" && audioPlayer.active
        && audioPlayer.health / audioPlayer.maxHealth < .3) {
      if (state.wallTime >= state.heartbeatAt) {
        sounds.heartbeat();
        state.heartbeatAt = state.wallTime + .82;
      }
    } else state.heartbeatAt = state.wallTime;
    if (profileFrame) {
      const inputStarted = performance.now();
      qualificationTiming.updatePrelude += inputStarted - profileAt;
      pollPlayerInput();
      if (longSoakDriver !== null) longSoakDriver(state.frames, dt);
      const inputFinished = performance.now();
      const inputElapsed = inputFinished - inputStarted;
      qualificationTiming.updateInput += inputElapsed;
      qualificationTiming.maxUpdateInput = Math.max(
        qualificationTiming.maxUpdateInput, inputElapsed);
      profileAt = inputFinished;
    } else {
      pollPlayerInput();
      if (longSoakDriver !== null) longSoakDriver(state.frames, dt);
    }
    recordReplayInput(wallDt);
    if (toastUntil && state.time >= toastUntil) {
      if (authoredSurfaceVisible(state.mode))
        ui.toast.classList.remove("visible");
      hudToast = "";
      hudMeshDirty = hudToastStale = true;
      toastUntil = 0;
    }
    updateParticles(dt);
    updateDecals(dt);
    state.shake = Math.max(0, state.shake - dt * .55);
    state.hitConfirm = Math.max(0, state.hitConfirm - dt);
    state.damageIndicator = Math.max(0, state.damageIndicator - dt);
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updatePrelude += elapsed;
      qualificationTiming.maxUpdatePrelude = Math.max(
        qualificationTiming.maxUpdatePrelude, elapsed);
      profileAt = now;
    }
    if (onlineArenaGeometryPending) {
      buildArena();
      onlineArenaGeometryPending = false;
    }
    if (online.active && online.role === "guest") {
      if (state.mode === "playing" && playerTank().active)
        moveTank(playerTank(), dt);
      onlineTick();
      if ((state.frames & 1) === 0) updateHud();
      updateOverlay();
      return;
    }
    if (state.mode === "arena-clear") {
      state.transition -= dt;
      if (state.transition <= 0) {
        if (isOnslaught()) {
          state.wave++;
          recordOnslaughtBest();
          beginArena(state.arena);
          showToast(`WAVE ${state.wave}`, .9);
          sounds.waveStart();
          setMode("playing");
        } else if (state.arena + 1 >= AUTHORED_ARENA_COUNT) {
          if (qualificationLongSoak) {
            beginArena(0);
            setMode("playing");
          } else setMode("victory");
        }
        else { beginArena(state.arena + 1); setMode("playing"); }
      }
      return;
    }
    if (state.mode !== "playing") return;
    settleSpawnChecks(false);
    updateBotNavigation();
    if (botLeagueActive && qualificationHooks.leaguePlayer !== null)
      qualificationHooks.leaguePlayer(dt);
    const firstBot = state.gameMode === MODE_DUEL
      || qualificationHooks !== null && qualificationHooks.botsFrozen ? MAX_TANKS
      : playerIsBot() ? 0
      : online.active && online.role === "host" ? 2 : 1;
    for (let at = firstBot; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active && !tank.inert && ((state.frames + at) & 1) === 0)
        updateBotCommand(tank, dt * 2);
    }
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updateBots += elapsed;
      qualificationTiming.maxUpdateBots = Math.max(
        qualificationTiming.maxUpdateBots, elapsed);
      profileAt = now;
    }
    // Stagger planning, not transforms: 15Hz movement cohorts visibly judder.
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active && (state.gameMode !== MODE_DUEL || at === state.duelTurn)) moveTank(tank, dt);
    }
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updateMove += elapsed;
      qualificationTiming.maxUpdateMove = Math.max(
        qualificationTiming.maxUpdateMove, elapsed);
      profileAt = now;
    }
    updateBullets(dt);
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updateProjectiles += elapsed;
      qualificationTiming.maxUpdateProjectiles = Math.max(
        qualificationTiming.maxUpdateProjectiles, elapsed);
      profileAt = now;
    }
    updatePickups(dt);
    updateHazards(dt);
    updateMinesAndSmoke(dt);
    updateRespawns(dt);
    updateObjective(dt);
    if (state.gameMode === MODE_DUEL) updateDuel(wallDt);
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updateWorld += elapsed;
      qualificationTiming.maxUpdateWorld = Math.max(
        qualificationTiming.maxUpdateWorld, elapsed);
      profileAt = now;
    }
    // Quantized HUD: text at 15Hz, indicators at 7.5Hz; no simulation skipping.
    if ((state.frames & 1) === 0) updateHud();
    if ((state.frames & 3) === 0) updateOverlay();
    onlineTick();
    if (profileFrame) {
      const elapsed = performance.now() - profileAt;
      qualificationTiming.updateUi += elapsed;
      qualificationTiming.maxUpdateUi = Math.max(
        qualificationTiming.maxUpdateUi, elapsed);
    }
  }

  /* The bot league drives the player tank with the bot AI unless it
     installed a scripted league player. */
  function playerIsBot() {
    return botLeagueActive && qualificationHooks.leaguePlayer === null;
  }

  function addTank(tank) {
    const baseColor = tank.id === 0 ? PAINT_COLORS[preferences.paint]
      : tank.boss ? PAINT_COLORS[3] : TANK_COLORS[tank.id];
    let color;
    if (tank.hitFlash > 0 || tank.fireWindup > 0) {
      color = tank.renderTint;
      const flash = Math.max(0, Math.min(1, tank.hitFlash / .12));
      const windup = tank.fireWindup > 0
        ? .28 + (((state.wallTime * 8 + tank.id) | 0) & 1) * .18 : 0;
      for (let channel = 0; channel < 3; channel++) {
        const warmed = baseColor[channel]
          + (COLORS.muzzle[channel] - baseColor[channel]) * windup;
        color[channel] = warmed + (1 - warmed) * flash;
      }
    } else {
      color = tank.id === 0 ? PAINT_BASE_TINTS[preferences.paint]
        : tank.boss ? PAINT_BASE_TINTS[3] : TANK_BASE_TINTS[tank.id];
    }
    const baseY = tank.surfaceY;
    const scale = tank.scale;
    const detailed = tank.id === 0 || tank.boss || state.gameMode === MODE_DUEL;
    if (detailed) addRetainedTankBox(tank.id, 0, false,
      tank.x, baseY + TANK_SHADOW_TOP - .0125, tank.z,
      1.32 * scale, .025, 1.7 * scale, tank.yaw, COLORS.shadow, .42,
      tank.yawSine, tank.yawCosine);
    const tankCosine = tank.yawCosine, tankSine = tank.yawSine;
    if (detailed) {
      const treadOffsetX = tankCosine * .49 * scale;
      const treadOffsetZ = -tankSine * .49 * scale;
      addRetainedTankBox(tank.id, 1, false,
        tank.x + treadOffsetX, baseY + .25, tank.z + treadOffsetZ,
        .32 * scale, .36 * scale, 1.42 * scale, tank.yaw,
        tank.boss && tank.rightTreadHealth <= 0 ? COLORS.shadow : COLORS.tread, 1,
        tankSine, tankCosine);
      addRetainedTankBox(tank.id, 2, false,
        tank.x - treadOffsetX, baseY + .25, tank.z - treadOffsetZ,
        .32 * scale, .36 * scale, 1.42 * scale, tank.yaw,
        tank.boss && tank.leftTreadHealth <= 0 ? COLORS.shadow : COLORS.tread, 1,
        tankSine, tankCosine);
    }
    addRetainedTankBox(tank.id, 3, false,
      tank.x, baseY + (detailed ? .42 : .35) * scale, tank.z,
      (detailed ? 1.02 : .96) * scale,
      (detailed ? .48 : .58) * scale,
      (detailed ? 1.25 : 1.36) * scale, tank.yaw, color, 1,
      tankSine, tankCosine);
    addRetainedTankBox(tank.id, 4, false,
      tank.x, baseY + .72 * scale, tank.z,
      (tank.classId === 0 ? .62 : .76) * scale,
      (tank.classId === 2 ? .34 : .28) * scale,
      (tank.classId === 2 ? .88 : .76) * scale, tank.turret,
      tank.boss && tank.turretHealth <= 0 ? COLORS.tread : color, 1,
      tank.turretSine, tank.turretCosine);
    if (detailed && tank.classId === 2)
      addRetainedTankBox(tank.id, 5, true,
        tank.x, baseY + .54 * scale, tank.z,
        1.24 * scale, .15 * scale, .42 * scale, tank.yaw, COLORS.wall, .9,
        tankSine, tankCosine);
    else if (detailed && tank.classId === 0)
      addRetainedTankBox(tank.id, 5, true,
        tank.x - tankSine * .38 * scale,
        baseY + .47 * scale, tank.z - tankCosine * .38 * scale,
        .56 * scale, .12 * scale, .42 * scale, tank.yaw, COLORS.pickup, .7,
        tankSine, tankCosine);
    const recoilOffset = tank.recoil * 1.5;
    const turretSine = tank.turretSine;
    const turretCosine = tank.turretCosine;
    const barrelX = tank.x + turretSine * (.56 * scale - recoilOffset);
    const barrelZ = tank.z + turretCosine * (.56 * scale - recoilOffset);
    addRetainedTankBox(tank.id, 6, false,
      barrelX, baseY + .75 * scale, barrelZ,
      (tank.classId === 2 ? .17 : .13) * scale,
      (tank.classId === 2 ? .17 : .13) * scale,
      (detailed ? 1.05 : .92) * scale,
      tank.turret, COLORS.barrel, 1, turretSine, turretCosine);
    tankBarrelCount++;
    // Once the whole optional tail is refused, avoid constructing its box
    // arguments. Keep rejection counters and the persistent scratch tint
    // exactly as the individual calls below would leave them.
    // OPTIONAL_TANK_TAIL_REJECTION_BEGIN
    if (collectInstancedBoxes && boxInstanceProgram
        && (boxInstanceCount >= frameInstanceCeiling
          || boxInstanceCount >= optionalInstanceCeiling)) {
      let rejected = (tank.recoil > .055 ? 1 : 0)
        + (tank.shield > 0 ? 1 : 0) + (tank.repair > 0 ? 1 : 0);
      if (detailed) {
        const ratio = Math.max(0, tank.health) / tank.maxHealth;
        rejected += 1 + (ratio > 0 ? 1 : 0);
        const pulse = tank.player && ratio < .3
          ? .55 + .45 * Math.sin(state.wallTime * Math.PI * 2.4) : 0;
        if (pulse > 0) {
          tank.renderTint[0] = COLORS.health[0]
            + (COLORS.bullet[0] - COLORS.health[0]) * pulse;
          tank.renderTint[1] = COLORS.health[1]
            + (COLORS.bullet[1] - COLORS.health[1]) * pulse;
          tank.renderTint[2] = COLORS.health[2]
            + (COLORS.bullet[2] - COLORS.health[2]) * pulse;
        }
      }
      if (rejected) {
        if (boxInstanceCount >= frameInstanceCeiling) meshDrops += rejected;
        instanceCapHitThisFrame = true;
      }
      return;
    }
    // OPTIONAL_TANK_TAIL_REJECTION_END
    if (tank.recoil > .055)
      addRetainedTankBox(tank.id, 7, true,
        tank.x + turretSine * 1.12, baseY + .73,
        tank.z + turretCosine * 1.12, .18, .18, .18,
        tank.turret, COLORS.muzzle, .95, turretSine, turretCosine);
    if (tank.shield > 0)
      addRetainedTankBox(tank.id, 8, true,
        tank.x, baseY + .58, tank.z, 1.55, .065, 1.8,
        tank.yaw, COLORS.pickup, .7, tankSine, tankCosine);
    if (tank.repair > 0) {
      const phase = state.time * 3 + tank.id;
      const repairSine = Math.sin(phase), repairCosine = Math.cos(phase);
      addRetainedTankBox(tank.id, 9, true,
        tank.x + repairSine * .72, baseY + 1.02,
        tank.z + repairCosine * .72, .24, .18, .32,
        phase, COLORS.pickup, .9, repairSine, repairCosine);
    }
    // Compact non-player tanks have no world health bar. Nothing below this
    // point contributes to their stream, so avoid the unused tint writes.
    if (!detailed) return;
    const ratio = Math.max(0, tank.health) / tank.maxHealth;
    const lowHealthPulse = tank.player && ratio < .3
      ? .55 + .45 * Math.sin(state.wallTime * Math.PI * 2.4) : 0;
    if (lowHealthPulse > 0) {
      color = tank.renderTint;
      color[0] = COLORS.health[0]
        + (COLORS.bullet[0] - COLORS.health[0]) * lowHealthPulse;
      color[1] = COLORS.health[1]
        + (COLORS.bullet[1] - COLORS.health[1]) * lowHealthPulse;
      color[2] = COLORS.health[2]
        + (COLORS.bullet[2] - COLORS.health[2]) * lowHealthPulse;
    } else {
      color = HEALTH_BASE_TINT;
    }
    /* The lost-health bar sits inside the fill bar with FLAT_STEP-sized
       margins on the faces the camera sees: with the old .0025 the two
       front faces were under one 16-bit depth step apart and the bar
       flickered between the two colours. */
    if (detailed) addRetainedTankBox(tank.id, 10, true,
      tank.x, baseY + 1.08, tank.z,
      1.06, .06, .06, cameraYaw, COLORS.healthLost, .85,
      cameraSine, cameraCosine);
    if (detailed && ratio > 0)
      addRetainedTankBox(tank.id, 11, true,
        tank.x - cameraCosine * (1 - ratio) * .54,
        baseY + 1.085, tank.z + cameraSine * (1 - ratio) * .54,
        1.08 * ratio, .075, .085, cameraYaw, color, .9,
        cameraSine, cameraCosine);
  }

  function addRetainedArenaScenery() {
    const arena = ARENAS[state.arena];
    const gateCount = arenaGateCount(arena);
    for (let gateAt = 0; gateAt < gateCount; gateAt++) {
      const gate = arena.gates[gateAt];
      if (state.gateOpen) {
        const horizontal = gate[2] > gate[3];
        const shiftX = horizontal ? gate[2] * .42 : 0;
        const shiftZ = horizontal ? 0 : gate[3] * .42;
        addBox(gate[0] - shiftX, .52, gate[1] - shiftZ,
          gate[2] * (horizontal ? .16 : 1), 1.04,
          gate[3] * (horizontal ? 1 : .16), 0, COLORS.wall);
        addBox(gate[0] + shiftX, .52, gate[1] + shiftZ,
          gate[2] * (horizontal ? .16 : 1), 1.04,
          gate[3] * (horizontal ? 1 : .16), 0, COLORS.wall);
      } else addBox(gate[0], .52, gate[1], gate[2], 1.04, gate[3],
        0, COLORS.wall);
      const horizontal = gate[2] > gate[3];
      const lightColor = state.gateOpen ? COLORS.health : COLORS.particle;
      addBox(gate[0] + (horizontal ? gate[2] * .46 : 0), 1.08,
        gate[1] + (horizontal ? 0 : gate[3] * .46), .12, .12, .12,
        0, lightColor, .95);
      addBox(gate[0] - (horizontal ? gate[2] * .46 : 0), 1.08,
        gate[1] - (horizontal ? 0 : gate[3] * .46), .12, .12, .12,
        0, lightColor, .95);
    }
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (!barrier.present) continue;
      if (barrier.active) addBox(barrier.x, .39, barrier.z,
        barrier.width, .78, barrier.depth, 0, BARRIER_INTACT);
      else addBox(barrier.x, .07, barrier.z,
        barrier.width * .82, .14, barrier.depth * .78,
        .12, BARRIER_BROKEN);
    }
  }

  function restoreRetainedArenaScenery() {
    if (!collectInstancedBoxes) {
      addRetainedArenaScenery();
      return;
    }
    if (retainedSceneryDirty) {
      const first = boxInstanceCount;
      addRetainedArenaScenery();
      retainedSceneryCount = Math.min(
        RETAINED_SCENERY_INSTANCE_LIMIT, boxInstanceCount - first);
      retainedSceneryDirty = false;
      return;
    }
    /* Dynamic instances never overwrite this retained scenery prefix. */
    boxInstanceCount = retainedSceneryCount;
  }

  function addObjectiveMarker(x, z) {
    const first = boxInstanceCount;
    /* This three-part marker follows the retained scenery prefix. Only its
       shared X/Z position moves; a distinct owner id proves that its fixed
       basis, heights and tints survived any intervening scene/killcam. */
    if (collectInstancedBoxes && boxInstanceProgram
        && retainedObjectiveFirst === first
        && first + 3 <= frameInstanceCeiling
        && boxInstanceSlotOwners[first] === -2
        && boxInstanceSlotOwners[first + 1] === -2
        && boxInstanceSlotOwners[first + 2] === -2) {
      const at = first * 16;
      boxInstanceMatrices[at + 12] = x;
      boxInstanceMatrices[at + 14] = z;
      boxInstanceMatrices[at + 28] = x;
      boxInstanceMatrices[at + 30] = z;
      boxInstanceMatrices[at + 44] = x;
      boxInstanceMatrices[at + 46] = z;
      boxInstanceCount += 3;
      return;
    }
    /* A solid plinth and mast read as a world objective at PSP resolution. */
    addBox(x, .045, z, .46, .09, .46, 0, COLORS.pickup, .72);
    addBox(x, .57, z, .12, 1.05, .12, 0, COLORS.pickup, 1);
    addBox(x, 1.12, z, .28, .12, .28, 0, COLORS.bullet, 1);
    if (collectInstancedBoxes && boxInstanceCount === first + 3) {
      retainedObjectiveFirst = first;
      boxInstanceSlotOwners[first] = -2;
      boxInstanceSlotOwners[first + 1] = -2;
      boxInstanceSlotOwners[first + 2] = -2;
    }
  }

  /* Sparks cool from white-hot through orange to smoke. Only the tint
     changes, so sparks keep the retained effect-box fast path; the ramp is
     baked into SPARK_STEPS tints at load (one more for a full-life spark),
     so a live spark costs a table lookup a frame and a tint upload only when
     its step changes. */
  const SPARK_STEPS = 8;
  const SPARK_TINTS = (() => {
    const ramp = [.2, .18, .17, 1, .38, .1, 1, .94, .6], tints = [];
    for (let step = 0; step <= SPARK_STEPS; step++) {
      const heat = Math.min(1, (step + .5) / SPARK_STEPS);
      const at = Math.min(1.999, heat * heat * 2.4), from = (at | 0) * 3;
      const tint = new Float32Array(3);
      for (let channel = 0; channel < 3; channel++)
        tint[channel] = ramp[from + channel] + (ramp[from + channel + 3]
          - ramp[from + channel]) * (at - (at | 0));
      tints.push(tint);
    }
    return tints;
  })();

  function buildDynamicScene(profileFrame = false) {
    let profileAt = profileFrame ? performance.now() : 0;
    resetMesh();
    boxInstanceCount = 0;
    frameInstanceCeiling = MAX_BOX_INSTANCES;
    renderedBulletInstances = 0;
    instanceCapHitThisFrame = false;
    const activeBulletInstances = activeMaskCount(bulletActiveMask);
    let essentialTankInstances = 0;
    for (let at = 0; at < MAX_TANKS; at++) if (tanks[at].active)
      essentialTankInstances += tanks[at].id === 0 || tanks[at].boss || state.gameMode === MODE_DUEL ? 6 : 3;
    optionalInstanceCeiling = MAX_BOX_INSTANCES - activeBulletInstances;
    const retainedHudIndices = Math.max(HUD_TRANSLATED_INDEX_RESERVE,
      hudPublishedIndexCount + hudPublishedIndicatorIndexCount);
    const translatedInstanceCeiling = Math.max(0, Math.floor(
      (TRANSLATED_VERTEX_BATCH_LIMIT - staticIndexCount - retainedHudIndices)
        / BOX_INDICES.length));
    frameInstanceCeiling = Math.min(MAX_BOX_INSTANCES,
      translatedInstanceCeiling);
    optionalInstanceCeiling = Math.min(optionalInstanceCeiling,
      Math.max(0, translatedInstanceCeiling
        - activeBulletInstances - essentialTankInstances));
    tankBarrelCount = 0;
    collectInstancedBoxes = boxInstanceProgram !== null;
    restoreRetainedArenaScenery();
    objectivePosition(objectiveTarget);
    addObjectiveMarker(objectiveTarget.x, objectiveTarget.z);
    if (state.gameMode === MODE_CONTROL) {
      const blueLeads = state.blueControl >= state.redControl;
      addBox(0, CONTROL_ZONE_TOP * .5, 0, 3.1, CONTROL_ZONE_TOP, 3.1, 0,
        blueLeads ? CONTROL_BLUE : CONTROL_RED, .62);
      addBox(0, .07, 0, .18, .14, 3.5, state.time * .25,
        COLORS.pickup, .75);
    }
    if (convoy.active) {
      addBox(convoy.x, .22, convoy.z, CONVOY_HALF_WIDTH * 2, .38,
        CONVOY_HALF_DEPTH * 2, 0, CONVOY_BODY);
      addBox(convoy.x, .54, convoy.z, .78, .3, 1.02, 0, CONVOY_TOP);
      addBox(convoy.x - .62, .12, convoy.z, .2, .2, 1.5, 0, COLORS.tread);
      addBox(convoy.x + .62, .12, convoy.z, .2, .2, 1.5, 0, COLORS.tread);
    }
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.buildScenery += now - profileAt;
      profileAt = now;
    }
    /* Reserve core tank and projectile geometry before optional effects. */
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active) addTank(tank);
    }
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.buildTanks += now - profileAt;
      profileAt = now;
    }
    const player = playerTank();
    const aimLevel = player.active ? aimGuideLevel() : AIM_OFF;
    aimGuide.boxes = 0;
    if (aimLevel !== AIM_OFF) {
      const at = profileFrame ? performance.now() : 0;
      if (updateAimGuide(player, aimLevel) && profileFrame) qualificationAITimes[20]++;
      // Full's enemy confirmation runs every other frame (the cast itself
      // runs every frame); a fresh switch to Full confirms at once.
      if (aimLevel === AIM_FULL) {
        if ((aimEnemyPhase ^= 1) === 1 || aimEnemyLevel !== AIM_FULL)
          updateAimGuideEnemy(player);
      } else { aimGuide.enemy = -1; aimGuide.enemyLeg = 0; aimGuide.hidden = false; }
      aimEnemyLevel = aimLevel;
      if (profileFrame) qualificationAITimes[19] += performance.now() - at;
      // The guide's marks sit ahead of the shells in the instance stream;
      // the guide still updates when its geometry cannot be published.
      if (collectInstancedBoxes && boxInstanceCount >= optionalInstanceCeiling) {
        instanceCapHitThisFrame = true;
      } else addAimMark();
    }
    let activeBullets = bulletActiveMask;
    while (activeBullets) {
      const at = activeMaskIndex(activeBullets);
      activeBullets = (activeBullets & (activeBullets - 1)) >>> 0;
      const bullet = bullets[at];
      if (addBox(bullet.x, .48, bullet.z, .13, .13,
          bullet.bounceFlash > 0 ? .8 : .28,
          0, bullet.bounceFlash > 0 ? COLORS.muzzle : COLORS.bullet,
          1, bullet.headingSine, bullet.headingCosine))
        renderedBulletInstances++;
    }
    /* Tanks/shells are published; retain only the frame and HUD caps. */
    optionalInstanceCeiling = frameInstanceCeiling;
    for (let at = 0; at < crates.length; at++) {
      const crate = crates[at];
      if (crate.active) addOptionalBox(crate.x, crate.renderY,
        crate.z, CRATE_HALF * 2, .5, CRATE_HALF * 2, 0, crate.type === 1 ? COLORS.particle : COLORS.wall, 1);
    }
    for (let at = 0; at < hazards.length; at++) {
      const hazard = hazards[at];
      if (hazard.active) addOptionalBox(hazard.x, hazard.renderY,
        hazard.z, hazard.renderDiameter, .02, hazard.renderDiameter, 0,
        HAZARD_COLORS[hazard.type], hazard.life < 0 ? .5 : Math.min(.5, hazard.life * .08));
    }
    for (let at = 0; at < MAX_PICKUPS; at++) {
      const pickup = pickups[at];
      if (!pickup.active) continue;
      const bob = .24 + Math.sin(pickup.phase * 2) * .08;
      addOptionalBox(pickup.x, bob, pickup.z, .42, .42, .42,
        0, COLORS.pickup, .9);
    }
    for (let at = 0; at < MAX_MINES; at++) {
      const mine = mines[at];
      if (mine.active) addOptionalBox(mine.x,
        MINE_TOP * .5 + mine.level * FLAT_STEP, mine.z, .42, MINE_TOP, .42,
        0, mine.arm > 0 ? COLORS.wall : COLORS.bullet, .9);
    }
    for (let at = 0; at < MAX_SMOKE; at++) {
      const smoke = smokeClouds[at];
      if (!smoke.active) continue;
      const pulse = .9 + Math.sin(state.time * 2) * .08;
      /* Clouds breathe in step at one height, so two that overlap shared
         their top face (and a side face when aligned). Clouds write depth,
         so the later cloud's face was tested against the earlier one's on
         the same plane: rounding picked the winner per pixel, and on the
         PSP GE per triangle, and the overlap flickered as the camera moved.
         Each slot insets its top and sides by a step that stays a few
         16-bit depth steps apart across the arena; slot 0 keeps the shape.
         Bottoms stay on the ground: a bottom face is never the nearest
         depth a cloud writes, so a shared one never competes. */
      const inset = at * .014;
      addOptionalBox(smoke.x, .42 - inset * .5, smoke.z,
        1.8 * pulse - inset * 2, .7 - inset, 1.8 * pulse - inset * 2,
        0, SMOKE_LOW, .42);
    }
    /* The guide's lines and the shell tracers follow the scenery props and
       precede decals and sparks; each set is admitted whole or not at all,
       so a crowded frame drops them before anything that can be shot. */
    if (aimLevel !== AIM_OFF) addAimLines(aimLevel);
    if (TRACERS_ENABLED) addTracers();
    /* Pressure discards oldest decals before transient sparks, for good:
       the room left for decals rises and falls with the sparks and shells
       from frame to frame, and an oldest decal that was only skipped came
       back next frame and blinked. */
    const particleReserve = Math.min(6,
      activeMaskCount(particleActiveLowMask)
        + activeMaskCount(particleActiveHighMask));
    const effectsInstanceCeiling = optionalInstanceCeiling;
    optionalInstanceCeiling = Math.max(boxInstanceCount,
      optionalInstanceCeiling - particleReserve);
    for (let age = 0; age < MAX_DECALS; age++) {
      const at = (decalCursor + MAX_DECALS - 1 - age) % MAX_DECALS;
      const decal = decals[at];
      if (!decal.active) continue;
      if (collectInstancedBoxes && boxInstanceCount >= optionalInstanceCeiling) {
        instanceCapHitThisFrame = true;
        droppedDecalInstances++;
        decal.active = false;
        continue;
      }
      const alpha = Math.max(0, Math.min(.58,
        decal.life / decal.maximum * .58));
      if (!addRetainedEffectBox(MAX_PARTICLES + at, true, decal.x, decal.y, decal.z,
          decal.width, FLAT_STEP, decal.depth, decal.yaw,
          decal.kind === 1 ? COLORS.tread : COLORS.shadow, alpha,
          decal.sine, decal.cosine)) {
        droppedDecalInstances++;
        decal.active = false;
      }
    }
    optionalInstanceCeiling = effectsInstanceCeiling;
    let renderedParticles = 0;
    let lowParticles = particleActiveLowMask;
    let highParticles = particleActiveHighMask;
    while (lowParticles && renderedParticles < 6) {
      const at = activeMaskIndex(lowParticles);
      lowParticles = (lowParticles & (lowParticles - 1)) >>> 0;
      const particle = particles[at];
      renderedParticles++;
      // Life left (0..1) picks the cooling tint; brighter, with no calls.
      const heat = Math.max(0, particle.life / particle.maximum);
      if (!addRetainedEffectBox(at, false, particle.x, Math.max(.04, particle.y), particle.z,
          .045, .045, particle.renderStreak, 0,
          SPARK_TINTS[(heat * SPARK_STEPS) | 0], heat < .625 ? heat * 1.6 : 1,
          particle.renderSine, particle.renderCosine))
        droppedParticleInstances++;
    }
    while (highParticles && renderedParticles < 6) {
      const at = 32 + activeMaskIndex(highParticles);
      highParticles = (highParticles & (highParticles - 1)) >>> 0;
      const particle = particles[at];
      renderedParticles++;
      // Life left (0..1) picks the cooling tint; brighter, with no calls.
      const heat = Math.max(0, particle.life / particle.maximum);
      if (!addRetainedEffectBox(at, false, particle.x, Math.max(.04, particle.y), particle.z,
          .045, .045, particle.renderStreak, 0,
          SPARK_TINTS[(heat * SPARK_STEPS) | 0], heat < .625 ? heat * 1.6 : 1,
          particle.renderSine, particle.renderCosine))
        droppedParticleInstances++;
    }
    if (instanceCapHitThisFrame) instanceCapHitFrames++;
    if (profileFrame) {
      qualificationTiming.buildEffects += performance.now() - profileAt;
      qualificationTiming.instances += boxInstanceCount;
      qualificationTiming.maxInstances = Math.max(
        qualificationTiming.maxInstances, boxInstanceCount);
    }
  }

  /* The eye-safety probe. The eye, d behind the player at 5.7, looks at a
     target lead(d) = clamp(.32d, .35, 1.25) ahead at .28; only the sight
     line's low (lead + d) * ratio can meet scenery. The target is walked
     back to the clear line ahead (cameraClearAhead), so only the part
     behind the player, g(d) = d * ratio - lead * (1 - ratio), can block; g
     is piecewise linear, so the farthest safe d is exact. It never reads
     the live distance: that fed back (a wall ahead pumped the camera). */
  const CAMERA_LOW_RAY_RATIO = (1.08 - .28) / (5.7 - .28);
  const CAMERA_EXPAND_HOLD = .3, CAMERA_EXPAND_MARGIN = .1, CAMERA_HISTORY = 32;
  const cameraSafeHistory = new Float64Array(CAMERA_HISTORY).fill(6.4);
  const cameraSafeTimes = new Float64Array(CAMERA_HISTORY).fill(-1);
  let cameraHistoryCursor = 0, cameraHistoryTime = 0;
  let cameraClearAhead = 1.25, cameraRenderLead = 1.25;
  // The follow camera's turn direction (-1, 0, 1), held near +-pi.
  let cameraTurn = 0;
  const CAMERA_TURN_MARGIN = .35;
  // Set by an arena change: frame the new arena at once (see updateCamera).
  let cameraArenaCut = false;
  // circleHitsObstacle(x, z, .07), answered from tinyCircleOccupied when
  // the cell is empty and the table is current (grid).
  function cameraPointBlocked(x, z, grid) {
    const cellX = (x + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE, cellZ = (z + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
    return !(grid && cellX >= 0 && cellX < COLLISION_GRID_SIDE
        && cellZ >= 0 && cellZ < COLLISION_GRID_SIDE
        && !tinyCircleOccupied[(cellZ | 0) * COLLISION_GRID_SIDE + (cellX | 0)])
      && circleHitsObstacle(x, z, .07);
  }
  // How far along (dx, dz), up to limit, a .07 circle stays clear (to .005).
  function cameraClearAlong(px, pz, dx, dz, limit, grid) {
    if (!(limit > 0)) return 0;
    const ex = px + dx * limit, ez = pz + dz * limit;
    if (grid) {
      const firstX = (px + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE, lastX = (ex + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
      const firstZ = (pz + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE, lastZ = (ez + COLLISION_GRID_ORIGIN) * COLLISION_GRID_SCALE;
      if (firstX >= 0 && firstX < COLLISION_GRID_SIDE && lastX >= 0
          && lastX < COLLISION_GRID_SIDE && firstZ >= 0 && firstZ < COLLISION_GRID_SIDE
          && lastZ >= 0 && lastZ < COLLISION_GRID_SIDE) {
        const left = (firstX < lastX ? firstX : lastX) | 0;
        const right = (firstX < lastX ? lastX : firstX) | 0;
        const top = (firstZ < lastZ ? firstZ : lastZ) | 0;
        const bottom = (firstZ < lastZ ? lastZ : firstZ) | 0;
        let occupied = false;
        for (let z = top; z <= bottom && !occupied; z++)
          for (let x = left; x <= right; x++)
            if (tinyCircleOccupied[z * COLLISION_GRID_SIDE + x]) { occupied = true; break; }
        if (!occupied) return limit;
      }
    }
    let clear = 0;
    for (let step = 1; step <= 16; step++) {
      const along = step * .16 < limit ? step * .16 : limit;
      if (cameraPointBlocked(px + dx * along, pz + dz * along, grid)) {
        let blocked = along;
        for (let refine = 0; refine < 5; refine++) {
          const middle = (clear + blocked) * .5;
          if (cameraPointBlocked(px + dx * middle, pz + dz * middle, grid)) blocked = middle;
          else clear = middle;
        }
        return clear;
      }
      clear = along;
      if (along >= limit) break;
    }
    return clear;
  }
  function cameraBehind(d, ahead) {
    const lead = Math.min(ahead, Math.min(1.25, Math.max(.35, d * .32)));
    return d * CAMERA_LOW_RAY_RATIO - lead * (1 - CAMERA_LOW_RAY_RATIO);
  }
  const cameraBreaks = new Float64Array(5);
  function cameraProbe(px, pz, sine, cosine) {
    const grid = crateGridReady && tinyCircleArena === state.arena;
    let limit = 1.25;
    if (sine > 1e-9) limit = Math.min(limit, (CAMERA_EDGE - px) / sine);
    else if (sine < -1e-9) limit = Math.min(limit, (-CAMERA_EDGE - px) / sine);
    if (cosine > 1e-9) limit = Math.min(limit, (CAMERA_EDGE - pz) / cosine);
    else if (cosine < -1e-9) limit = Math.min(limit, (-CAMERA_EDGE - pz) / cosine);
    const ahead = cameraClearAhead = cameraClearAlong(px, pz, sine, cosine, limit, grid);
    let far = 6.4;
    if (sine > 1e-9) far = Math.min(far, (px + HULL_EDGE) / sine);
    else if (sine < -1e-9) far = Math.min(far, (px - HULL_EDGE) / sine);
    if (cosine > 1e-9) far = Math.min(far, (pz + HULL_EDGE) / cosine);
    else if (cosine < -1e-9) far = Math.min(far, (pz - HULL_EDGE) / cosine);
    if (!(far > 1.2)) return 1.2;
    let breaks = 0;
    cameraBreaks[breaks++] = 1.2;
    const grown = ahead < 1.25 ? ahead / .32 : 1.25 / .32;
    if (grown > 1.2 && grown < far) cameraBreaks[breaks++] = grown;
    cameraBreaks[breaks++] = far;
    let longest = 0;
    for (let at = 0; at < breaks; at++) {
      const behind = cameraBehind(cameraBreaks[at], ahead);
      if (behind > longest) longest = behind;
    }
    if (longest <= 0) return far;
    const clearBehind = cameraClearAlong(px, pz, -sine, -cosine, longest, grid);
    if (clearBehind >= longest) return far;
    for (let at = breaks - 1; at > 0; at--) {
      const near = cameraBreaks[at - 1], distant = cameraBreaks[at];
      const nearBehind = cameraBehind(near, ahead), distantBehind = cameraBehind(distant, ahead);
      if (distantBehind < clearBehind) return distant;
      if (nearBehind < clearBehind)
        return near + (distant - near) * (clearBehind - nearBehind)
          / (distantBehind - nearBehind) * .999;
    }
    return 1.2;
  }

  function updateCamera(dt) {
    if (state.mode === "killcam") {
      viewProjection.set(killcamCameraViews[killcamRead]);
      publishCamera(); return;
    }
    const player = playerTank();
    if (preferences.camera === 0) { cameraYaw = 0; cameraTurn = 0; }
    else if (player.active) {
      let difference = wrapAngle(player.yaw - cameraYaw);
      const deadZone = .32;
      /* A hull facing about away from the camera and wiggling flips the
         wrapped difference's sign at +-pi: keep turning the way it was
         until the difference is clearly short of pi. */
      if (cameraTurn && Math.sign(difference) !== cameraTurn
          && Math.abs(difference) > Math.PI - CAMERA_TURN_MARGIN)
        difference += cameraTurn * Math.PI * 2;
      if (Math.abs(difference) > deadZone) {
        const follow = difference - Math.sign(difference) * deadZone;
        cameraTurn = Math.sign(follow);
        cameraYaw = wrapAngle(cameraYaw
          + Math.max(-dt * .72, Math.min(dt * .72, follow)));
      } else cameraTurn = 0;
    }
    // Presentation shake never consumes simulation randomness.
    const visibleShake = preferences.shake ? state.shake : 0;
    const shakeX = visibleShake ? Math.sin(state.frames * 2.73) * visibleShake : 0;
    const shakeZ = visibleShake ? Math.cos(state.frames * 1.91) * visibleShake : 0;
    cameraSine = Math.sin(cameraYaw);
    cameraCosine = Math.cos(cameraYaw);
    // Every frame: amortized, walls looked screen-fixed before a retraction.
    cameraSafetyUpdates++;
    cameraSafeDistance = cameraProbe(player.x, player.z, cameraSine, cameraCosine);
    // Retract at once; expand to what stayed clear for CAMERA_EXPAND_HOLD s.
    if (cameraArenaCut) {
      /* A new arena is a cut: its first frame takes its own distance and
         lead rather than easing from the last arena's. */
      cameraArenaCut = false;
      cameraDistance = cameraSafeDistance;
      cameraSafeHistory.fill(cameraSafeDistance);
      cameraSafeTimes.fill(cameraHistoryTime);
      cameraRenderLead = Math.min(cameraClearAhead,
        Math.min(1.25, Math.max(.35, cameraDistance * .32)));
    }
    cameraHistoryTime += dt > 0 ? dt : 0;
    cameraSafeHistory[cameraHistoryCursor] = cameraSafeDistance;
    cameraSafeTimes[cameraHistoryCursor] = cameraHistoryTime;
    cameraHistoryCursor = (cameraHistoryCursor + 1) % CAMERA_HISTORY;
    if (cameraSafeDistance < cameraDistance) {
      cameraDistance = cameraSafeDistance;
    } else if (cameraSafeDistance > cameraDistance) {
      let held = cameraSafeDistance, covered = false;
      for (let at = 0; at < CAMERA_HISTORY; at++) {
        if (cameraHistoryTime - cameraSafeTimes[at] > CAMERA_EXPAND_HOLD) { covered = true; continue; }
        if (cameraSafeHistory[at] < held) held = cameraSafeHistory[at];
      }
      // Short of 6.4, settle inside the limit so rocking does not pump it.
      const goal = held >= 6.4 ? held : held - CAMERA_EXPAND_MARGIN;
      if (covered && goal > cameraDistance + (held >= 6.4 ? 0 : .02)) {
        cameraDistance += (goal - cameraDistance) * Math.min(1, dt * 7);
        if (goal - cameraDistance < .001) cameraDistance = goal;
      }
    }
    // The target, walked back out of scenery ahead, and eased.
    const leadGoal = Math.min(cameraClearAhead,
      Math.min(1.25, Math.max(.35, cameraDistance * .32)));
    // Quick to draw in, slow to reach out again: a hull nosing along a wall
    // must not nod the view.
    cameraRenderLead += (leadGoal - cameraRenderLead)
      * Math.min(1, dt * (leadGoal < cameraRenderLead ? 8 : 2.5));
    const targetX = Math.max(-CAMERA_EDGE, Math.min(CAMERA_EDGE, player.x + cameraSine * cameraRenderLead));
    const targetZ = Math.max(-CAMERA_EDGE, Math.min(CAMERA_EDGE, player.z + cameraCosine * cameraRenderLead));
    const eyeX = player.x - cameraSine * cameraDistance + shakeX;
    const eyeZ = player.z - cameraCosine * cameraDistance + shakeZ;
    lookAt(view, eyeX, 5.7, eyeZ, targetX, .28, targetZ);
    multiply(viewProjection, projection, view);
    publishCamera();
  }

  function publishCamera() {
    /* Repeated draws still need the instanced program's current camera. */
    gl.useProgram(program);
    gl.uniformMatrix4fv(viewProjectionLocation, false, viewProjection);
    if (boxInstanceProgram) {
      gl.useProgram(boxInstanceProgram);
      gl.uniformMatrix4fv(boxInstanceViewProjectionLocation, false,
        viewProjection);
      gl.useProgram(program);
    }
  }

  function prepareFrame(profileFrame = false) {
    if (state.mode === "killcam") {
      boxInstanceMatrices.set(killcamMatrixViews[killcamRead]);
      boxInstanceTints.set(killcamTintViews[killcamRead]);
      boxInstanceCount = killcamCounts[killcamRead];
      staticIndexCount = killcamStaticCounts[killcamRead]; indexCount = 0;
      return;
    }
    const phaseStarted = profileFrame ? performance.now() : 0;
    buildDynamicScene(profileFrame);
    if (profileFrame) {
      const elapsed = performance.now() - phaseStarted;
      qualificationTiming.build += elapsed;
      qualificationTiming.maxBuild = Math.max(
        qualificationTiming.maxBuild, elapsed);
    }
  }

  const OPTIONAL_HUD_START_BUDGET_MS = 10;
  const HUD_MAX_DEFERRED_FRAMES = 8;
  function presentFrame(dt = 1 / 60, profileFrame = false,
                        deferOptionalWork = false,
                        frameWorkStarted = 0,
                        reuseUploadedScene = false) {
    let phaseStarted = profileFrame ? performance.now() : 0;
    updateCamera(dt);
    captureKillcam();
    if (profileFrame) {
      const elapsed = performance.now() - phaseStarted;
      qualificationTiming.camera += elapsed;
      qualificationTiming.maxCamera = Math.max(
        qualificationTiming.maxCamera, elapsed);
      phaseStarted = performance.now();
    }
    const dynamicCount = indexCount;
    if (!reuseUploadedScene) {
      if (dynamicCount)
        uploadMesh(dynamicVertexArray, dynamicPositionBuffer,
          dynamicColorBuffer, dynamicIndexBuffer);
      uploadBoxInstances();
    }
    if (profileFrame) {
      const elapsed = performance.now() - phaseStarted;
      qualificationTiming.upload += elapsed;
      qualificationTiming.maxUpload = Math.max(
        qualificationTiming.maxUpload, elapsed);
      phaseStarted = performance.now();
    }
    let hudElapsed = 0;
    // Account for geometry before admitting optional retained-HUD work.
    const pendingHud = hudMeshDirty || hudBuildPhase >= 0 || hudIndicatorDirty;
    if (!deferOptionalWork && frameWorkStarted > 0
        && pendingHud
        && performance.now() - frameWorkStarted
          > OPTIONAL_HUD_START_BUDGET_MS) {
      deferOptionalWork = true;
      deadlineHudDeferrals++;
    }
    /* Deferral may stretch a score update, not permanently erase the HUD.
       Under sustained load make one glyph of progress every eight frames.
       Keep the previous complete mesh visible until the new one commits. */
    let tinyHudSlice = false;
    if (pendingHud && deferOptionalWork) {
      if (++hudDeferredFrames >= HUD_MAX_DEFERRED_FRAMES) {
        tinyHudSlice = true;
        deferOptionalWork = false;
        hudDeferredFrames = 0;
      }
    } else hudDeferredFrames = 0;
    if (!deferOptionalWork && (hudMeshDirty || hudBuildPhase >= 0)) {
      const hudPhase = hudBuildPhase < 0 ? 0 : Math.min(6, hudBuildPhase);
      const hudStarted = profileFrame ? performance.now() : 0;
      rebuildHudMeshSlice(tinyHudSlice ? 1 : 0);
      if (profileFrame) {
        const elapsed = performance.now() - hudStarted;
        hudElapsed += elapsed;
        qualificationHudPhaseTotals[hudPhase] += elapsed;
        qualificationHudPhaseMaximums[hudPhase] = Math.max(
          qualificationHudPhaseMaximums[hudPhase], elapsed);
        qualificationHudPhaseCounts[hudPhase]++;
      }
    }
    if (!deferOptionalWork && hudIndicatorDirty
        && (!tinyHudSlice || hudBuildPhase < 0)) {
      const indicatorStarted = profileFrame ? performance.now() : 0;
      rebuildHudIndicators();
      if (profileFrame) {
        const elapsed = performance.now() - indicatorStarted;
        hudElapsed += elapsed;
        qualificationHudIndicator[0] += elapsed;
        qualificationHudIndicator[1] = Math.max(
          qualificationHudIndicator[1], elapsed);
        qualificationHudIndicator[2]++;
      }
    }
    if (profileFrame) {
      qualificationTiming.hud += hudElapsed;
      qualificationTiming.maxHud = Math.max(
        qualificationTiming.maxHud, hudElapsed);
      phaseStarted = performance.now();
    }
    const hudCount = hudPublishedIndexCount
      + hudPublishedIndicatorIndexCount;
    let commandsIssued = false;
    /* The game profile exposes a bounded retained command list for an
       invariant frame shape. Buffer contents, uniform matrices, generations,
       counts, and instance counts remain live; only repeated WebGL admission
       and wire-template construction are skipped. Fall back to ordinary
       WebGL whenever the frame shape changes or the list cannot be replayed. */
    const repeatable = repeatedFrameCommands && !dynamicCount
      && boxInstanceCount > 0 && hudCount > 0;
    if (repeatable && repeatedFrameList) {
      repeatedFrameCounts[0] = staticIndexCount;
      repeatedFrameCounts[1] = 1;
      repeatedFrameCounts[2] = BOX_INDICES.length;
      repeatedFrameCounts[3] = boxInstanceCount;
      repeatedFrameCounts[4] = hudCount;
      repeatedFrameCounts[5] = 1;
      commandsIssued = repeatedFrameCommands.execute(
        repeatedFrameList, repeatedFrameCounts);
    }
    if (!commandsIssued) {
      repeatedFrameList = null;
      const capturing = repeatable && repeatedFrameCommands.begin();
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      drawMesh(staticVertexArray, staticPositionBuffer, staticColorBuffer,
        staticIndexBuffer, staticIndexCount);
      if (dynamicCount)
        drawMesh(dynamicVertexArray, dynamicPositionBuffer, dynamicColorBuffer,
          dynamicIndexBuffer, dynamicCount);
      drawBoxInstances();
      drawHud();
      if (capturing) {
        /* Every authored arena was cached before gameplay, so the retained
           draw can admit later levels without rebuilding its command packet. */
        repeatedFrameLimits[0] = arenaMaximumStaticIndexCount;
        repeatedFrameLimits[1] = 1;
        repeatedFrameLimits[2] = BOX_INDICES.length;
        repeatedFrameLimits[3] = MAX_INSTANCES_PER_DRAW;
        repeatedFrameLimits[4] = hudIndices.length;
        repeatedFrameLimits[5] = 1;
        const captured = repeatedFrameCommands.end(repeatedFrameLimits);
        if (captured?.drawCount === 3) {
          repeatedFrameList = captured;
          repeatedFrameCaptures++;
        }
      }
    }
    if (profileFrame) {
      const elapsed = performance.now() - phaseStarted;
      qualificationTiming.commands += elapsed;
      qualificationTiming.maxCommands = Math.max(
        qualificationTiming.maxCommands, elapsed);
    }
    state.frames++;
  }

  function render(dt = 1 / 60, profileFrame = false,
                  deferOptionalWork = false, frameWorkStarted = 0,
                  reuseUploadedScene = false) {
    /* A late simulation retains one coherent scene until the next frame. */
    if (!reuseUploadedScene) prepareFrame(profileFrame);
    presentFrame(dt, profileFrame, deferOptionalWork, frameWorkStarted,
      reuseUploadedScene);
  }

  function advanceElapsed(elapsed) {
    elapsed = Math.min(1 / 30, Math.max(0, Number(elapsed) || 0));
    if (elapsed > 0) update(elapsed);
    render(elapsed || 1 / 60);
  }

  let lastTimestamp = 0;
  const UPDATE_SCENE_BUDGET_MS = 5.0;
  let deadlineSceneReuses = 0;
  let deadlineHudDeferrals = 0;
  function frame(timestamp) {
    if (!state.running) return;
    /* Do not replay simulation debt on an already late PSP frame. At the
       authored speed, one 1/30-second step stays below the collision sweep's
       smallest obstacle thickness; a stall therefore slows game time rather
       than creating a self-sustaining four-update catch-up spike. */
    const elapsed = lastTimestamp
      ? Math.min(1 / 30, Math.max(0, (timestamp - lastTimestamp) / 1000)) : 0;
    lastTimestamp = timestamp;
    // Qualification runs only (the soak's action cycle).
    if (qualificationHooks !== null) qualificationHooks.frame(state.frames,
      arenaGenerationPhase === ARENA_GENERATION_IDLE);
    // A measured frame of a phase profile (qualification.js).
    const profileFrame = validationInputProfile
      && qualificationTiming.samples < qualificationSampleLimit
      && state.frames >= 20;
    if (profileFrame) qualificationHooks.measureStart();
    const updateStarted = performance.now();
    // bots.js's copy of the flag changes only on measured frames.
    if (profileFrame) { qualificationAIActive = true; setBotProfiling(true); }
    if (elapsed > 0) update(elapsed, profileFrame);
    if (profileFrame) { setBotProfiling(false); qualificationAIActive = false; }
    const updateElapsed = performance.now() - updateStarted;
    /* Shed optional HUD rebuilding, never live world transforms. A persistent
       slow simulation otherwise reuses the old scene on every frame and
       makes moving actors look frozen while their physics keeps advancing. */
    /* Qualification must measure the product behavior, not disable its
       deadline guard and manufacture HUD work that a release frame defers. */
    const deferOptionalWork = updateElapsed > UPDATE_SCENE_BUDGET_MS;
    const reuseUploadedScene = arenaGenerationReuseSceneOnce;
    arenaGenerationReuseSceneOnce = false;
    if (reuseUploadedScene) deadlineSceneReuses++;
    render(elapsed || 1 / 60, profileFrame, deferOptionalWork,
      updateStarted, reuseUploadedScene);
    if (profileFrame) qualificationHooks.measureEnd(updateElapsed);
    requestAnimationFrame(frame);
  }

  /* qualification.js's view of the game, built only when it attaches:
     the bindings its debug API, profile and drivers use, accessors for the
     values the game reassigns, and installers for its clocks and hooks. */
  function qualificationBridge() {
    return {
      AIM_FULL, AIM_SCORE_MULTIPLIERS, ARENAS, ASSIST_LOCK, ASSIST_OFF,
      ASSIST_SNAP, BOSS, BOT_FIRE_RANGE_SQUARED, CLASSES, CLASS_COLLISION_RADIUS,
      CLASS_COMBINED_RADIUS_SQUARED, COLLISION_GRID_CELLS, COLLISION_GRID_SIDE,
      CRATE_HALF, DIFFICULTIES, GADGETS, GAME_MODES, GENERATED_ARENA_INDEX,
      HUD_GLYPH_LIMIT, MODE_CONTROL, HUD_OBJECTIVE_RADIUS_X, HUD_OBJECTIVE_RADIUS_Y, HUD_PRIMITIVE_LIMIT,
      HUD_TEXT, HUD_VERTEX_WORDS, HULL_EDGE, MAX_BARRIERS, MAX_BOX_INSTANCES,
      MAX_BULLETS, MAX_DECALS, MAX_TANKS, PAINT_NAMES, REPLAY_CAPACITY,
      RETAINED_TANK_PARTS, REVERSE_DEGREES, TRACER_SLOTS, TRACER_VERTICES,
      activateCommand, activeBarrierMask, activeEnemyCount, addHudGlyph,
      advanceElapsed, aimGuide, aimGuideLevel, applyOnlineSnapshot,
      applyPalette, applyScreenAim, arenaGenerator, arenaGeometryCache,
      barrierCircleGrid, barriers, beginArena, beginBotFireWindup,
      botDebug, botDifficultyTables, botDifficultyValue, boxInstanceSlotOwners,
      boxInstanceTints, bulletHitsObstacle, bulletTracer, bullets,
      cancelSpawnChecks, circleHitsObstacle, clearSchemeKeys, convoy,
      crateCircleGrid, crates, dailySeed, damageTank, decals,
      elevatedFiringOrigin, explodeCrate,
      exportReplay, fillArenaSpatialData, fillBarrierCircleGrid, fillRayGeometry,
      fillTinyCircleOccupied, finalScore, finishReplayRecording, fireSecondary,
      generatedArena, gl, hazards, hudGlyphRuns, hudGlyphStarts,
      hudModeVisible, hudVertices, importReplay, input,
      invalidateBotNavigation, lineCrossesWalls, noteArenaGeneration, loadPreferences, mines,
      navigationCell, objectiveArrowShown, objectivePosition, online,
      orientationIndex, particles, pickups, placeTank, playerTank, pocketKept,
      pollPlayerInput, preferences, prepareNavigationGrids, program,
      qualificationAutoStart, qualificationKind, qualificationLongSoak, urlSwitch,
      refreshSetupLabels, render, resetAimAssist, resetGame,
      retainedTankPartSlots, savePreferences, sceneryTintLocation,
      segmentHitsBox, sendOnlineSnapshot, steerBotRoute, bankAim, setBotProfiling, setBulletActive,
      setMode, setParticleActive, setQualificationAIActive, setSource,
      smokeClouds, sounds, spawnDecal, spawnHazard, spawnParticles,
      startOrResume, startReplay, state, surfaceHeightAt, tanks,
      toolingSurfaces, tracerBullet,
      tracerHead, tracerVertexCount, tracerVertices, unlockMedal, update,
      updateAimGuide, updateAimGuideEnemy, updateBotCommand, updateBullets,
      updateCamera, updateControlHint, updateHud, updateOverlay,
      updateRayActiveMask, updateTankTurretCache, updateTankYawCache,
      useGadget, view, viewProjection, wrapAngle,
      get activeSmokeCount() { return activeSmokeCount; }, set activeSmokeCount(value) { activeSmokeCount = value; },
      get arenaCommonIndexCount() { return arenaCommonIndexCount; },
      get arenaGenerationAttempts() { return arenaGenerationAttempts; },
      get arenaGenerationChecksum() { return arenaGenerationChecksum; },
      get arenaGenerationFallback() { return arenaGenerationFallback; },
      get arenaGenerationPhase() { return arenaGenerationPhase; },
      get arenaGenerationStartedAt() { return arenaGenerationStartedAt; },
      get arenaMaximumStaticIndexCount() { return arenaMaximumStaticIndexCount; },
      get arenaPlaneMaxSpan() { return arenaPlaneMaxSpan; },
      get authoredHudWrites() { return authoredHudWrites; },
      get botLeagueActive() { return botLeagueActive; }, set botLeagueActive(value) { botLeagueActive = value; },
      get boxInstanceCount() { return boxInstanceCount; },
      get boxInstanceProgram() { return boxInstanceProgram; },
      get boxInstanceViewProjectionLocation() { return boxInstanceViewProjectionLocation; },
      get bulletActiveMask() { return bulletActiveMask; }, set bulletActiveMask(value) { bulletActiveMask = value; },
      get bulletSpawnCursor() { return bulletSpawnCursor; }, set bulletSpawnCursor(value) { bulletSpawnCursor = value; },
      get cameraCosine() { return cameraCosine; }, set cameraCosine(value) { cameraCosine = value; },
      get cameraDistance() { return cameraDistance; },
      get cameraSafeDistance() { return cameraSafeDistance; },
      get cameraSafetyUpdates() { return cameraSafetyUpdates; },
      get cameraSine() { return cameraSine; }, set cameraSine(value) { cameraSine = value; },
      get cameraYaw() { return cameraYaw; }, set cameraYaw(value) { cameraYaw = value; },
      get deadlineHudDeferrals() { return deadlineHudDeferrals; },
      get deadlineSceneReuses() { return deadlineSceneReuses; },
      get decalRecycles() { return decalRecycles; },
      get droppedDecalInstances() { return droppedDecalInstances; },
      get droppedParticleInstances() { return droppedParticleInstances; },
      get frameInstanceCeiling() { return frameInstanceCeiling; },
      set generatedGeometryDirty(value) { generatedGeometryDirty = value; },
      get hudCharacterCount() { return hudCharacterCount; }, set hudCharacterCount(value) { hudCharacterCount = value; },
      get hudIndexCount() { return hudIndexCount; }, set hudIndexCount(value) { hudIndexCount = value; },
      get hudIndicatorUploads() { return hudIndicatorUploads; },
      get hudObjectiveAngle() { return hudObjectiveAngle; },
      get hudObjectiveVisible() { return hudObjectiveVisible; },
      get hudPublishedIndexCount() { return hudPublishedIndexCount; },
      get hudPublishedIndicatorIndexCount() { return hudPublishedIndicatorIndexCount; },
      get hudTextUploads() { return hudTextUploads; },
      get hudToast() { return hudToast; },
      get hudVertexCount() { return hudVertexCount; }, set hudVertexCount(value) { hudVertexCount = value; },
      get indexCount() { return indexCount; },
      get instanceCapHitFrames() { return instanceCapHitFrames; },
      get killcamCount() { return killcamCount; },
      get killcamElapsed() { return killcamElapsed; },
      get meshDrops() { return meshDrops; },
      get objectiveKey() { return objectiveKey; },
      get objectiveMission() { return objectiveMission; },
      get onlineArenaGeometryPending() { return onlineArenaGeometryPending; },
      get particleSpawnCursor() { return particleSpawnCursor; }, set particleSpawnCursor(value) { particleSpawnCursor = value; },
      get particleTemplateCursor() { return particleTemplateCursor; }, set particleTemplateCursor(value) { particleTemplateCursor = value; },
      get qualificationAIActive() { return qualificationAIActive; },
      get randomState() { return randomState; }, set randomState(value) { randomState = value; },
      get renderedBulletInstances() { return renderedBulletInstances; },
      get repeatedFrameCaptures() { return repeatedFrameCaptures; },
      get replayActive() { return replayActive; },
      get replayCount() { return replayCount; },
      get replayDigest() { return replayDigest; },
      get replayPending() { return replayPending; }, set replayPending(value) { replayPending = value; },
      get replayRecording() { return replayRecording; }, set replayRecording(value) { replayRecording = value; },
      get replayTruncated() { return replayTruncated; },
      get replayVerified() { return replayVerified; },
      get retainedSceneryDirty() { return retainedSceneryDirty; }, set retainedSceneryDirty(value) { retainedSceneryDirty = value; },
      get runAimLevel() { return runAimLevel; },
      get staticIndexCount() { return staticIndexCount; },
      get tankBarrelCount() { return tankBarrelCount; },
      get tracerBoxes() { return tracerBoxes; },
      get validationInputProfile() { return validationInputProfile; }, set validationInputProfile(value) { validationInputProfile = value; },
      get vertexCount() { return vertexCount; },
      installProfile(profile) {
        qualificationSampleLimit = profile.sampleLimit;
        qualificationTiming = profile.timing;
        qualificationAITimes = profile.aiTimes;
        qualificationAICounts = profile.aiCounts;
        qualificationMoveTimes = profile.moveTimes;
        qualificationSoundTimes = profile.soundTimes;
        qualificationActionTimes = profile.actionTimes;
        qualificationActionCounts = profile.actionCounts;
        qualificationHudPhaseTotals = profile.hudPhaseTotals;
        qualificationHudPhaseMaximums = profile.hudPhaseMaximums;
        qualificationHudPhaseCounts = profile.hudPhaseCounts;
        qualificationHudIndicator = profile.hudIndicator;
        bots.setProfileArrays(profile.aiTimes, profile.aiCounts);
      },
      /* One real shell cast from shellActiveMask, through the ray index
         or (indexed false) the record walk: what it stops on, and where. */
      castShellOnce(x, z, dx, dz, length, overCover, indexed) {
        if (indexed) fillRayGeometry(); else rayGeometryArena = -1;
        castActive = shellActiveMask(overCover); castSkip = -1;
        castShell(x, z, dx, dz, length, overCover);
        return {id: castKind === SHELL_STOP ? castId : -1, length: castLength,
          indexed: rayGeometryArena === state.arena};
      },
      setHooks(hooks) { qualificationHooks = hooks; },
      installStats(stats, turns) { qualificationStats = stats; bulletTurns = turns; },
      setLongSoakDriver(driver) { longSoakDriver = driver; },
    };
  }
  if (qualificationTooling) {
    globalThis.__treadlineQualify = (attach) => attach(qualificationBridge());
    // A harness evaluates qualification.js itself; a qualification URL loads it.
    if (!qualificationHarness) {
      const script = document.createElement("script");
      script.src = "qualification.js";
      document.head.appendChild(script);
    }
  }
  globalThis.pocSummary = "TREADLINE-READY";
  globalThis.__treadlineBootReady = true;
  const queuedDeploy = !!globalThis.__treadlineDeployQueued;
  globalThis.__treadlineDeployQueued = false;
  ui.play.removeAttribute("aria-busy");
  ui.play.textContent = "Deploy";
  controls.attach({input, preferences, state, online, tanks, ui, playerTank,
    setSource, keyboardDown, connectedGamepad, applyScreenAim,
    lineCrossesSmoke, setMode, random, botDifficulty, ASSIST_OFF, ASSIST_LOCK,
    lendTooling});
  loadPreferences();
  applyPalette();
  // The long soak's mode: mode=control, convoy or onslaught, else Survival.
  if (qualificationLongSoak) state.gameMode = {control: MODE_CONTROL,
    convoy: MODE_CONVOY, onslaught: MODE_ONSLAUGHT}[urlSwitch("mode")] ?? MODE_SURVIVAL;
  /* Populate only the fixed geometry shape before immutable arena tails are
     packed. Validating this throwaway placeholder used to duplicate the full
     Deploy-time generator inside initial script evaluation; the real run is
     validated cooperatively before this slot can ever become playable. */
  arenaGenerator.materialize(nextArenaSeed);
  buildArena(queuedDeploy);
  refreshSetupLabels();
  updateControlHint();
  updateHud(true);
  updateOverlay();
  campaign.attach({state, tanks, barriers, crates, hazards, pickups, mines,
    convoy, convoyOverlap, preferences, ui, sounds, placeTank, findClearSpot,
    connectedGamepad,
    searchClearSpot, clearSpot,
    beginArena, setMode, showToast, makeBoss,
    startOrResume, resetGame, applyPalette, spawnHazard, spawnParticles,
    circleHitsObstacle, fillBarrierCircleGrid, invalidateBotNavigation,
    updateTankYawCache, updateTankTurretCache, surfaceHeightAt,
    savePreferences, refreshSetupLabels, updateHud, updateControlHint,
    clearSchemeKeys, resetAimAssist, exportReplay, importReplay, startReplay,
    requestPresentation, arenaGenerator, fillArenaSpatialData, PALETTE_TINTS,
    aimGuideLevel, cycleAimGuide, AIM_LEVEL_NAMES, AIM_ASSISTS, AIM_SCORE_MULTIPLIERS,
    aimScoreMultiplier, runAimLevel: () => runAimLevel,
    finalScore, aimScoreNote, MODE_DESCRIPTIONS,
    replayVerified: () => replayVerified, onlineView: () => onlineView,
    // Practice runs never record (see README): no replay to keep in step.
    noReplay() { replayPending = replayRecording = false; replayCount = 0; },
    setTankTint, restoreTankTints, GOLD_TINT, CYAN_TINT,
    PAINT_COLORS, CRATE_HALF, GAME_MODES, GADGETS,
    MODE_SURVIVAL, MODE_CONTROL, MODE_CONVOY, MODE_ONSLAUGHT, MODE_DAILY,
    MODE_BILLIARDS, MODE_DUEL,
    CLASSES, PALETTES,
    PAINT_NAMES, replaying: () => replayActive,
    dirty(generated) {
      retainedSceneryDirty = true; if (generated) generatedGeometryDirty = true;
    },
    cameraReset() { cameraYaw = 0; cameraSine = 0; cameraCosine = 1; },
    /* Scenery the campaign placed or switched: rebuild everything derived
       from it, including the navigation occupancy (pocket checks and
       routes read it; it used to keep the arena-start barriers). */
    refreshScenery() {
      fillBarrierCircleGrid();
      if (crateGridReady) { fillTinyCircleOccupied(); fillRayGeometry(); prepareNavigationGrids(); }
      invalidateBotNavigation();
    },
    validateSpawn: validateRespawn, lendTooling,
  });
  /* The long soak enters play the way a person does: Quick Match's Deploy is
     a trusted activation that claims Page controls, so the measured match
     runs full screen without browser chrome over the canvas. Its input
     script presses Cross on the focused Deploy once the runtime is ready. */
  const longSoakDeploy = qualificationLongSoak && !!toolingSurfaces.campaign;
  if (qualificationAutoStart && !longSoakDeploy) {
    if (resetGame()) setMode("playing");
    /* Attribute reflection is the standards path. The direct style is a
       validation-only belt-and-suspenders guard against measuring a stale
       pre-layout setup panel on an early deferred-script mutation. */
    if (qualificationLongSoak) ui.panel.style.display = "none";
  } else if (queuedDeploy) startOrResume();
  else {
    render();
    if (longSoakDeploy) toolingSurfaces.campaign.menu.show("quick", false);
  }
  if (qualificationLongSoak)
    globalThis.pocSummary = "TREADLINE-LONG-SOAK-ACTIVE";
  requestAnimationFrame(frame);
})();
