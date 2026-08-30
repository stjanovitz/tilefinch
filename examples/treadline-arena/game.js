(() => {
  "use strict";

  const canvas = document.getElementById("game");
  const gl = canvas && canvas.getContext("webgl", {
    alpha: false, depth: true, antialias: true,
  });
  const ui = {
    shell: document.getElementById("game-shell"),
    panel: document.getElementById("panel"),
    heading: document.querySelector("#panel h1"),
    message: document.getElementById("message"),
    play: document.getElementById("play"),
    hud: document.getElementById("hud"),
    score: document.getElementById("score"),
    arena: document.getElementById("arena"),
    armor: document.getElementById("armor"),
    gadget: document.getElementById("gadget"),
    toast: document.getElementById("toast"),
    controls: document.getElementById("controls"),
    loadout: document.getElementById("loadout"),
    preferences: document.getElementById("preferences"),
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
    cameraChoice: document.getElementById("camera-choice"),
    cameraValue: document.getElementById("camera-value"),
    commandChoice: document.getElementById("command-choice"),
    commandValue: document.getElementById("command-value"),
    musicChoice: document.getElementById("music-choice"),
    musicValue: document.getElementById("music-value"),
    effectsChoice: document.getElementById("effects-choice"),
    effectsValue: document.getElementById("effects-value"),
    shakeChoice: document.getElementById("shake-choice"),
    shakeValue: document.getElementById("shake-value"),
    stats: document.getElementById("stats"),
    gadgetMeter: document.querySelector("#gadget-meter i"),
    gadgetMeterShell: document.getElementById("gadget-meter"),
    commandStatus: document.getElementById("command-status"),
    commandMeter: document.querySelector("#command-meter i"),
    commandMeterShell: document.getElementById("command-meter"),
    objectiveArrow: document.getElementById("objective-arrow"),
    hitMarker: document.getElementById("hit-marker"),
    damageArrow: document.getElementById("damage-arrow"),
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
    if (ui.message) ui.message.textContent = "WebGL is unavailable.";
    globalThis.pocSummary = "TREADLINE-NO-WEBGL";
    return;
  }
  const webPairing = !!(navigator.tilefinchMultiplayer
    && navigator.tilefinchMultiplayer.pairingMode === "manual-offer-answer");
  if (webPairing) {
    if (ui.onlineHost) ui.onlineHost.textContent = "Host web game";
    if (ui.onlineCode) ui.onlineCode.textContent = "Join web game";
    if (ui.onlineLan) ui.onlineLan.hidden = true;
  }
  const instancing = gl.getExtension("ANGLE_instanced_arrays");
  const vertexArrays = gl.getExtension("OES_vertex_array_object");
  /* The query switch is validation-only: it provides an exact same-build
     A/B seam for distinguishing retained-command defects from simulation or
     presentation defects on real PSP hardware. */
  const repeatedFrameCommands =
    !String(location.href).includes("repeat-commands=off")
      ? gl.getExtension("TILEFINCH_repeated_frame_commands") : null;
  let repeatedFrameList = null;
  const repeatedFrameCounts = new Uint16Array(6);
  const repeatedFrameLimits = new Uint16Array(6);
  let repeatedFrameCaptures = 0;
  /* The PSP URL bridge has historically exposed the committed query through
     href slightly earlier than through search. Device qualification must not
     silently become a title-panel benchmark during that window. */
  const qualificationURL = `${location.search || ""} ${location.href || ""}`;
  const requestedSeedMatch = qualificationURL.match(/(?:seed=)(\d{1,10})/);
  const requestedArenaSeed = requestedSeedMatch
    ? Number(requestedSeedMatch[1]) >>> 0 : 0;
  const qualificationHint =
    document.body?.getAttribute("data-tilefinch-qualification") || "";
  const qualificationProfile = qualificationURL.includes("qualification=profile");
  let validationInputProfile = false;
  const qualificationBridgeProfile =
    !qualificationURL.includes("bridge-profile=off");
  const qualificationSoak = qualificationURL.includes("qualification=soak");
  const qualificationLongSoak =
    qualificationURL.includes("qualification=long-soak")
    || qualificationHint === "long-soak";
  const qualificationGameMode = !qualificationLongSoak ? 0
    : qualificationURL.includes("mode=onslaught") ? 3
      : qualificationURL.includes("mode=convoy") ? 2
        : qualificationURL.includes("mode=control") ? 1 : 0;
  const qualificationOrdinary =
    qualificationURL.includes("qualification=ordinary");
  const qualificationActions = qualificationProfile || qualificationSoak
    || qualificationLongSoak;
  const qualificationAutoStart = qualificationActions || qualificationOrdinary;
  const qualificationTiming = {
    samples: 0, update: 0, camera: 0, build: 0, upload: 0, hud: 0,
    commands: 0, frame: 0, maxUpdate: 0, maxCamera: 0, maxBuild: 0,
    maxUpload: 0, maxHud: 0, maxCommands: 0, maxFrame: 0,
    updatePrelude: 0, updateInput: 0, updateBots: 0, updateMove: 0,
    updateProjectiles: 0, updateWorld: 0, updateUi: 0,
    maxUpdatePrelude: 0, maxUpdateInput: 0, maxUpdateBots: 0, maxUpdateMove: 0,
    maxUpdateProjectiles: 0, maxUpdateWorld: 0, maxUpdateUi: 0,
    buildScenery: 0, buildTanks: 0, buildEffects: 0,
    instances: 0, maxInstances: 0,
  };
  const QUALIFICATION_PROFILE_SAMPLE_LIMIT = 180;
  const qualificationFrameSamples = new Float32Array(
    QUALIFICATION_PROFILE_SAMPLE_LIMIT * 5);
  const qualificationHudPhaseTotals = new Float32Array(7);
  const qualificationHudPhaseMaximums = new Float32Array(7);
  const qualificationHudPhaseCounts = new Uint16Array(7);
  let qualificationHudIndicatorTotal = 0;
  let qualificationHudIndicatorMaximum = 0;
  let qualificationHudIndicatorCount = 0;
  const QUALIFICATION_SLOW_FRAME_LIMIT = 16;
  const QUALIFICATION_SLOW_FRAME_WORDS = 10;
  const qualificationSlowFrames = new Float32Array(
    QUALIFICATION_SLOW_FRAME_LIMIT * QUALIFICATION_SLOW_FRAME_WORDS);
  const qualificationSlowFrameEvents = new Uint8Array(
    QUALIFICATION_SLOW_FRAME_LIMIT);
  let qualificationSlowFrameCount = 0;
  let qualificationProfileStarted = false;
  let qualificationActionFrames = 0;
  let qualificationActionKind = 0;
  let qualificationActionSamples = 0;
  let qualificationActionMax = 0;
  let qualificationLateActionMax = 0;
  let qualificationLateActionUpdate = 0;
  let qualificationLateActionBuild = 0;
  let qualificationLateActionHud = 0;
  let qualificationLateActionCommands = 0;
  if (qualificationProfile) {
    const bridge = globalThis.__tilefinchWebGLDiagnostics;
    if (bridge) {
      bridge.profileDrawPhases = false;
      for (const key of ["profileDraws", "profileBasicMs",
        "profileInstancesMs", "profileRangesMs", "profilePrepareMs",
        "profileEnqueueMs", "profileFinishMs", "profileQueueAdmissionMs",
        "profileWirePackMs", "profileWireSourcesMs", "profileWireStateMs",
        "profileWireInstancesMs", "profileWireRetainMs", "drawPlanHits",
        "drawPlanMisses", "commandTemplateHits",
        "commandTemplateMisses", "commandSlotTemplateHits",
        "commandSlotTemplateMisses", "sourcePacketHits",
        "sourcePacketMisses", "sourcePacketIndexHits",
        "sourcePacketIndexMisses"]) bridge[key] = 0;
    }
  }

  function startQualificationProfile() {
    if (qualificationProfileStarted) return;
    qualificationProfileStarted = true;
    const bridge = globalThis.__tilefinchWebGLDiagnostics;
    if (!bridge) return;
    for (const key of ["profileDraws", "profileBasicMs",
      "profileInstancesMs", "profileRangesMs", "profilePrepareMs",
      "profileEnqueueMs", "profileFinishMs", "profileQueueAdmissionMs",
      "profileWirePackMs", "profileWireSourcesMs", "profileWireStateMs",
      "profileWireInstancesMs", "profileWireRetainMs", "drawPlanHits",
      "drawPlanMisses", "commandTemplateHits",
      "commandTemplateMisses", "commandSlotTemplateHits",
      "commandSlotTemplateMisses", "sourcePacketHits",
      "sourcePacketMisses", "sourcePacketIndexHits",
      "sourcePacketIndexMisses"]) bridge[key] = 0;
    bridge.profileDrawPhases = qualificationBridgeProfile;
  }

  /* The PSP validation input script calls this at its measurement marker.
     Unlike ?qualification=profile it does not auto-start or synthesize game
     commands: profiling therefore covers the real Offline Library -> Page
     controls -> Gamepad API path and the physical button combinations that a
     player uses. Shipping builds never call this validation hook. */
  globalThis.__tilefinchStartInputProfile = () => {
    for (const key of Object.keys(qualificationTiming))
      qualificationTiming[key] = 0;
    qualificationProfileStarted = false;
    validationInputProfile = true;
    startQualificationProfile();
  };

  function qualificationPercentile(slot, count, percentile) {
    const values = new Float32Array(count);
    for (let at = 0; at < count; at++)
      values[at] = qualificationFrameSamples[at * 5 + slot];
    for (let at = 1; at < count; at++) {
      const value = values[at];
      let before = at;
      while (before && values[before - 1] > value) {
        values[before] = values[before - 1];
        before--;
      }
      values[before] = value;
    }
    return values[Math.min(count - 1,
      Math.max(0, Math.ceil(count * percentile / 100) - 1))];
  }

  function recordQualificationSlowFrame(event, update, prelude, bots, move,
                                        projectiles, world, build, hud,
                                        commands, total) {
    let insertion = qualificationSlowFrameCount;
    if (insertion === QUALIFICATION_SLOW_FRAME_LIMIT) {
      const last = (QUALIFICATION_SLOW_FRAME_LIMIT - 1)
        * QUALIFICATION_SLOW_FRAME_WORDS;
      if (total <= qualificationSlowFrames[last + 9]) return;
      insertion--;
    } else qualificationSlowFrameCount++;
    while (insertion > 0) {
      const before = (insertion - 1) * QUALIFICATION_SLOW_FRAME_WORDS;
      if (qualificationSlowFrames[before + 9] >= total) break;
      const destination = insertion * QUALIFICATION_SLOW_FRAME_WORDS;
      for (let at = 0; at < QUALIFICATION_SLOW_FRAME_WORDS; at++)
        qualificationSlowFrames[destination + at]
          = qualificationSlowFrames[before + at];
      qualificationSlowFrameEvents[insertion]
        = qualificationSlowFrameEvents[insertion - 1];
      insertion--;
    }
    const at = insertion * QUALIFICATION_SLOW_FRAME_WORDS;
    qualificationSlowFrames[at] = update;
    qualificationSlowFrames[at + 1] = prelude;
    qualificationSlowFrames[at + 2] = bots;
    qualificationSlowFrames[at + 3] = move;
    qualificationSlowFrames[at + 4] = projectiles;
    qualificationSlowFrames[at + 5] = world;
    qualificationSlowFrames[at + 6] = build;
    qualificationSlowFrames[at + 7] = hud;
    qualificationSlowFrames[at + 8] = commands;
    qualificationSlowFrames[at + 9] = total;
    qualificationSlowFrameEvents[insertion] = event;
  }

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
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uViewProjection * vec4(aPosition, 1.0);
      vColor = aColor;
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
  gl.clearColor(.012, .031, .037, 1);
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
  const FACE_SHADE = new Float32Array([1, .55, 1.14, .45, .82, .68]);
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
  let boxInstanceProgram = null, boxInstancePosition = null;
  let boxInstanceShade = null, boxInstanceIndex = null;
  let boxInstanceMatrix = null, boxInstanceTint = null;
  let boxInstanceMatrixLocation = -1, boxInstanceTintLocation = -1;
  let boxInstanceViewProjectionLocation = null;
  let boxInstanceVertexArray = null;
  let lastBoxYaw = NaN, lastBoxCosine = 1, lastBoxSine = 0;
  /* Tank parts keep a stable logical cache id even though inactive optional
     parts compact the uploaded instance stream. When a part remains in the
     same physical slot, preserve its matrix basis and patch only translation
     or tint fields that changed. This is cheaper than copying a full retained
     mat4 and keeps the sparse scale/translate representation allocation-free. */
  const RETAINED_TANK_PARTS = 12;
  const retainedTankPartCount = 6 * RETAINED_TANK_PARTS;
  const retainedTankPartSlots = new Int8Array(retainedTankPartCount);
  retainedTankPartSlots.fill(-1);
  /* Keep signatures as ordinary JS numbers. Float32 signatures would compare
     unequal to the identical double-valued inputs after their first rounding,
     defeating the cache; Float64 typed-array traffic is costly on Allegrex. */
  const retainedTankPartState = new Array(retainedTankPartCount * 12);
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
      let shade = FACE_SHADE[face];
      if (face !== 2 && face !== 3 && BOX_VERTICES[source + 1] < 0)
        shade *= .72;
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

  function addOptionalBox(x, y, z, width, height, depth, yaw, color,
                          alpha = 1, knownSine = NaN, knownCosine = NaN) {
    if (collectInstancedBoxes && boxInstanceCount >= optionalInstanceCeiling) {
      instanceCapHitThisFrame = true;
      return false;
    }
    return addBox(x, y, z, width, height, depth, yaw, color, alpha,
      knownSine, knownCosine);
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
    const stateAt = cache * 12;
    const instance = boxInstanceCount;
    const matrixAt = instance * 16;
    const tintAt = instance * 4;
    const sameSlot = retainedTankPartSlots[cache] === instance
      && boxInstanceSlotOwners[instance] === cache;
    const sameBasis = sameSlot
      && retainedTankPartState[stateAt + 3] === width
      && retainedTankPartState[stateAt + 4] === height
      && retainedTankPartState[stateAt + 5] === depth
      && retainedTankPartState[stateAt + 6] === sine
      && retainedTankPartState[stateAt + 7] === cosine;
    if (!sameBasis) {
      const halfWidth = width * .5, halfHeight = height * .5;
      const halfDepth = depth * .5;
      boxInstanceMatrices[matrixAt] = cosine * halfWidth;
      boxInstanceMatrices[matrixAt + 2] = -sine * halfWidth;
      boxInstanceMatrices[matrixAt + 5] = halfHeight;
      boxInstanceMatrices[matrixAt + 8] = sine * halfDepth;
      boxInstanceMatrices[matrixAt + 10] = cosine * halfDepth;
      retainedTankPartState[stateAt + 3] = width;
      retainedTankPartState[stateAt + 4] = height;
      retainedTankPartState[stateAt + 5] = depth;
      retainedTankPartState[stateAt + 6] = sine;
      retainedTankPartState[stateAt + 7] = cosine;
    }
    if (!sameSlot || retainedTankPartState[stateAt] !== x
        || retainedTankPartState[stateAt + 1] !== y
        || retainedTankPartState[stateAt + 2] !== z) {
      boxInstanceMatrices[matrixAt + 12] = x;
      boxInstanceMatrices[matrixAt + 13] = y;
      boxInstanceMatrices[matrixAt + 14] = z;
      retainedTankPartState[stateAt] = x;
      retainedTankPartState[stateAt + 1] = y;
      retainedTankPartState[stateAt + 2] = z;
    }
    if (!sameSlot || retainedTankPartState[stateAt + 8] !== color[0]
        || retainedTankPartState[stateAt + 9] !== color[1]
        || retainedTankPartState[stateAt + 10] !== color[2]
        || retainedTankPartState[stateAt + 11] !== alpha) {
      boxInstanceTints[tintAt] = color[0];
      boxInstanceTints[tintAt + 1] = color[1];
      boxInstanceTints[tintAt + 2] = color[2];
      boxInstanceTints[tintAt + 3] = alpha;
      retainedTankPartState[stateAt + 8] = color[0];
      retainedTankPartState[stateAt + 9] = color[1];
      retainedTankPartState[stateAt + 10] = color[2];
      retainedTankPartState[stateAt + 11] = alpha;
    }
    retainedTankPartSlots[cache] = instance;
    boxInstanceSlotOwners[instance] = cache;
    boxInstanceCount++;
    return true;
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
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
    const shades = new Float32Array(24 * 4);
    for (let vertex = 0; vertex < 24; vertex++) {
      const face = (vertex / 4) | 0;
      let shade = FACE_SHADE[face];
      if (face !== 2 && face !== 3 && BOX_VERTICES[vertex * 3 + 1] < 0)
        shade *= .72;
      const temperature = face * 3;
      shades.set([
        shade * FACE_TEMPERATURE[temperature],
        shade * FACE_TEMPERATURE[temperature + 1],
        shade * FACE_TEMPERATURE[temperature + 2], 1,
      ], vertex * 4);
    }
    gl.bufferData(gl.ARRAY_BUFFER, shades, gl.STATIC_DRAW);
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
  const HUD_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/";
  const hudGlyphIndex = new Int8Array(128);
  hudGlyphIndex.fill(-1);
  for (let glyph = 0; glyph < HUD_CHARS.length; glyph++)
    hudGlyphIndex[HUD_CHARS.charCodeAt(glyph)] = glyph;
  /* Row-major 3x5 glyph masks. Each glyph chooses horizontal or vertical ink
     runs, whichever emits fewer quads, to preserve the 4096-vertex ceiling. */
  const hudGlyphMasks = new Uint16Array([
    0,31599,29850,29671,31207,18925,31183,31695,18727,31727,31215,
    23530,15083,29263,15211,29391,4815,31567,23533,29847,31524,23277,
    29257,23549,24573,31599,5103,20335,22511,31183,9367,31597,11117,
    24557,23213,9389,29351,448,4772,
  ]);
  const HUD_GLYPH_LIMIT = 64;
  const HUD_PRIMITIVE_LIMIT = 150;
  const HUD_TRANSLATED_INDEX_RESERVE = HUD_PRIMITIVE_LIMIT * 6;
  const HUD_INDICATOR_PRIMITIVE_LIMIT = 12;
  /* Local coordinates paint a shadow without another draw. */
  const HUD_VERTEX_WORDS = 10;
  const hudVertices = new Float32Array(
    HUD_PRIMITIVE_LIMIT * 4 * HUD_VERTEX_WORDS);
  const hudIndices = new Uint16Array(HUD_PRIMITIVE_LIMIT * 6);
  for (let primitive = 0; primitive < HUD_PRIMITIVE_LIMIT; primitive++) {
    const vertex = primitive * 4, index = primitive * 6;
    hudIndices[index] = vertex;
    hudIndices[index + 1] = vertex + 1;
    hudIndices[index + 2] = vertex + 2;
    hudIndices[index + 3] = vertex;
    hudIndices[index + 4] = vertex + 2;
    hudIndices[index + 5] = vertex + 3;
  }
  const hudIndicatorVertices = new Float32Array(
    HUD_INDICATOR_PRIMITIVE_LIMIT * 4 * HUD_VERTEX_WORDS);
  let hudVertexCount = 0, hudIndexCount = 0, hudCharacterCount = 0;
  let hudMeshDirty = true, hudScore = "", hudArena = "", hudArmor = "";
  let hudIndicatorVertexCount = 0, hudIndicatorIndexCount = 0;
  let hudPublishedIndicatorIndexCount = 0, hudIndicatorDirty = true;
  let hudTextUploads = 0, hudIndicatorUploads = 0;
  let hudGadget = "", hudCommand = "", hudToast = "";
  let hudObjectiveAngle = 0, hudDamageAngle = 0;
  let hudHitVisible = false, hudDamageVisible = false;
  let hudGadgetRatio = 1, hudCommandRatio = 0, hudMultiplierRatio = 0;
  let hudVisualObjectiveStep = 0x7fffffff;
  let hudVisualDamageStep = 0x7fffffff;
  let hudVisualHit = false, hudVisualDamage = false;
  let hudVisualGadgetStep = -1, hudVisualCommandStep = -1;
  let hudVisualMultiplierStep = -1;
  let hudVisualCommandEnabled = false;
  let hudBuildPhase = -1;
  let hudBuildScore = "", hudBuildArena = "", hudBuildArmor = "";
  let hudBuildGadget = "", hudBuildCommand = "", hudBuildToast = "";
  let hudBuildGenerating = false;
  let hudBuildText = null, hudBuildTextAt = 0;
  let hudBuildTextX = 0, hudBuildTextY = 0, hudBuildTextScale = 1;
  let hudBuildTextTint = null;
  let hudPublishedIndexCount = 0, hudPublishedVertexCount = 0;
  /* Two glyphs fit beside a measured PSP gameplay frame. */
  const HUD_GLYPHS_PER_SLICE = 2;
  const HUD_TEXT = [.91, .98, 1, 1];
  const HUD_ACCENT = [.56, 1, .83, 1];
  const HUD_WARNING = [1, .46, .34, 1];
  const HUD_GOLD = [1, .84, .44, 1];
  const HUD_GADGET_TRACK = [.12, .25, .23, .8];
  const HUD_COMMAND_TRACK = [.28, .23, .1, .8];
  const HUD_MULTIPLIER_TRACK = [.22, .16, .08, .8];
  const HUD_PANEL = [.018, .05, .06, .94];
  const hudProgram = gl.createProgram();
  gl.attachShader(hudProgram, compile(gl.VERTEX_SHADER, `
    attribute vec2 aPosition;
    attribute vec4 aTint;
    attribute vec4 aShadowRect;
    varying lowp vec4 vTint;
    varying lowp vec4 vShadowRect;
    void main(void) {
      gl_Position = vec4(aPosition, 0.0, 1.0);
      vTint = aTint;
      vShadowRect = aShadowRect;
    }
  `));
  gl.attachShader(hudProgram, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vTint;
    varying lowp vec4 vShadowRect;
    void main(void) {
      if (vShadowRect.z >= 0.0
          && (vShadowRect.x > vShadowRect.z
            || vShadowRect.y > vShadowRect.w))
        gl_FragColor = vec4(0.005, 0.012, 0.014, vTint.a * 0.78);
      else
        gl_FragColor = vTint;
    }
  `));
  gl.bindAttribLocation(hudProgram, 0, "aPosition");
  gl.bindAttribLocation(hudProgram, 1, "aTint");
  gl.bindAttribLocation(hudProgram, 2, "aShadowRect");
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
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 40, 0);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 40, 8);
    gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 40, 24);
    gl.enableVertexAttribArray(0);
    gl.enableVertexAttribArray(1);
    gl.enableVertexAttribArray(2);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    vertexArrays.bindVertexArrayOES(null);
  }

  function writeHudVertex(pixelX, pixelY, tint,
                          localX, localY, width, height) {
    const at = hudVertexCount * HUD_VERTEX_WORDS;
    hudVertices[at] = pixelX / 160 - 1;
    hudVertices[at + 1] = 1 - pixelY / 90;
    hudVertices[at + 2] = tint[0];
    hudVertices[at + 3] = tint[1];
    hudVertices[at + 4] = tint[2];
    hudVertices[at + 5] = tint[3];
    hudVertices[at + 6] = localX;
    hudVertices[at + 7] = localY;
    hudVertices[at + 8] = width;
    hudVertices[at + 9] = height;
    hudVertexCount++;
  }

  function addHudQuad(x, y, width, height, tint) {
    if (width <= 0 || height <= 0
        || hudVertexCount + 4 > hudVertices.length / HUD_VERTEX_WORDS
        || hudIndexCount + 6 > hudIndices.length) return false;
    const outerWidth = width + 1, outerHeight = height + 1;
    writeHudVertex(x, y, tint, 0, 0, width, height);
    writeHudVertex(x + outerWidth, y, tint,
      outerWidth, 0, width, height);
    writeHudVertex(x + outerWidth, y + outerHeight, tint,
      outerWidth, outerHeight, width, height);
    writeHudVertex(x, y + outerHeight, tint,
      0, outerHeight, width, height);
    hudIndexCount += 6;
    return true;
  }

  function addHudRect(x, y, width, height, tint) {
    return addHudQuad(x, y, width, height, tint);
  }

  function writeHudIndicatorVertex(pixelX, pixelY, tint,
                                   localX, localY, width, height) {
    const at = hudIndicatorVertexCount * HUD_VERTEX_WORDS;
    hudIndicatorVertices[at] = pixelX / 160 - 1;
    hudIndicatorVertices[at + 1] = 1 - pixelY / 90;
    hudIndicatorVertices[at + 2] = tint[0];
    hudIndicatorVertices[at + 3] = tint[1];
    hudIndicatorVertices[at + 4] = tint[2];
    hudIndicatorVertices[at + 5] = tint[3];
    hudIndicatorVertices[at + 6] = localX;
    hudIndicatorVertices[at + 7] = localY;
    hudIndicatorVertices[at + 8] = width;
    hudIndicatorVertices[at + 9] = height;
    hudIndicatorVertexCount++;
  }

  function addHudIndicatorQuad(x, y, width, height, tint) {
    if (width <= 0 || height <= 0
        || hudIndicatorVertexCount + 4
          > hudIndicatorVertices.length / HUD_VERTEX_WORDS
        || hudIndicatorIndexCount + 6
          > HUD_INDICATOR_PRIMITIVE_LIMIT * 6)
      return false;
    const outerWidth = width + 1, outerHeight = height + 1;
    writeHudIndicatorVertex(x, y, tint, 0, 0, width, height);
    writeHudIndicatorVertex(x + outerWidth, y, tint,
      outerWidth, 0, width, height);
    writeHudIndicatorVertex(x + outerWidth, y + outerHeight, tint,
      outerWidth, outerHeight, width, height);
    writeHudIndicatorVertex(x, y + outerHeight, tint,
      0, outerHeight, width, height);
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
      writeHudIndicatorVertex(x, y, tint, 0, 0, -1, -1);
    }
    writeHudIndicatorVertex(firstX, firstY, tint, 0, 0, -1, -1);
    hudIndicatorIndexCount += 6;
    return true;
  }

  function addHudGlyph(character, x, y, scale, tint) {
    if (hudCharacterCount >= HUD_GLYPH_LIMIT) return false;
    const code = character.charCodeAt(0);
    const glyph = code < hudGlyphIndex.length ? hudGlyphIndex[code] : -1;
    hudCharacterCount++;
    if (glyph <= 0) return true;
    const mask = hudGlyphMasks[glyph];
    let horizontal = 0, vertical = 0;
    for (let row = 0; row < 5; row++) for (let column = 0; column < 3; column++)
      if ((mask & (1 << (row * 3 + column)))
          && (!column || !(mask & (1 << (row * 3 + column - 1)))))
        horizontal++;
    for (let column = 0; column < 3; column++) for (let row = 0; row < 5; row++)
      if ((mask & (1 << (row * 3 + column)))
          && (!row || !(mask & (1 << ((row - 1) * 3 + column)))))
        vertical++;
    if (vertical < horizontal) {
      for (let column = 0; column < 3; column++) for (let row = 0; row < 5;) {
        while (row < 5 && !(mask & (1 << (row * 3 + column)))) row++;
        const first = row;
        while (row < 5 && (mask & (1 << (row * 3 + column)))) row++;
        if (row > first && !addHudRect(x + column * scale, y + first * scale,
            scale, (row - first) * scale, tint)) return false;
      }
    } else {
      for (let row = 0; row < 5; row++) for (let column = 0; column < 3;) {
        while (column < 3 && !(mask & (1 << (row * 3 + column)))) column++;
        const first = column;
        while (column < 3 && (mask & (1 << (row * 3 + column)))) column++;
        if (column > first && !addHudRect(x + first * scale, y + row * scale,
            (column - first) * scale, scale, tint)) return false;
      }
    }
    return true;
  }

  function prepareHudTextSlice(text, x, y, scale, tint) {
    hudBuildText = String(text).toUpperCase();
    hudBuildTextAt = 0;
    hudBuildTextX = x;
    hudBuildTextY = y;
    hudBuildTextScale = scale;
    hudBuildTextTint = tint;
  }

  function rebuildHudMeshSlice() {
    if (hudBuildPhase < 0) {
      hudMeshDirty = false;
      hudVertexCount = hudIndexCount = hudCharacterCount = 0;
      hudBuildGenerating = state.mode === "generating";
      if (hudBuildGenerating) addHudRect(54, 46, 212, 82, HUD_PANEL);
      hudBuildScore = hudScore || "00000";
      hudBuildArena = hudArena;
      hudBuildArmor = hudArmor;
      hudBuildGadget = hudGadget;
      hudBuildCommand = hudCommand;
      hudBuildToast = hudToast.slice(0, 20);
      hudBuildText = null;
      hudBuildPhase = 0;
    }
    while (hudBuildPhase < 6) {
      if (hudBuildText === null) {
        if (hudBuildGenerating && hudBuildPhase === 0)
          prepareHudTextSlice(hudBuildToast,
            Math.max(7, 160 - hudBuildToast.length * 4), 68, 2, HUD_GOLD);
        else if (hudBuildGenerating && hudBuildPhase === 1)
          prepareHudTextSlice(hudBuildArena,
            Math.max(7, 160 - hudBuildArena.length * 4), 91, 2, HUD_ACCENT);
        else if (hudBuildGenerating)
          prepareHudTextSlice("", 0, 0, 1, HUD_TEXT);
        else if (hudBuildPhase === 0)
          prepareHudTextSlice(hudBuildScore, 7, 5, 2, HUD_TEXT);
        else if (hudBuildPhase === 1)
          prepareHudTextSlice(hudBuildArena, 116, 5, 2, HUD_ACCENT);
        else if (hudBuildPhase === 2)
          prepareHudTextSlice(hudBuildArmor,
            313 - hudBuildArmor.length * 8, 5, 2, HUD_TEXT);
        else if (hudBuildPhase === 3)
          prepareHudTextSlice(hudBuildGadget, 7, 165, 2, HUD_ACCENT);
        else if (hudBuildPhase === 4)
          prepareHudTextSlice(hudBuildCommand,
            313 - hudBuildCommand.length * 8, 165, 2, HUD_GOLD);
        else
          prepareHudTextSlice(hudBuildToast,
            Math.max(7, 160 - hudBuildToast.length * 4), 27, 2, HUD_GOLD);
      }
      if (!hudBuildText.length) {
        hudBuildText = null;
        hudBuildPhase++;
        continue;
      }
      /* The first retained HUD is built while the game is entering play and
         should become complete promptly. Later live updates use the smaller
         measured slice so a score/power-up change cannot spike a frame. */
      const glyphsThisSlice = hudTextUploads === 0
        ? HUD_GLYPHS_PER_SLICE * 2 : HUD_GLYPHS_PER_SLICE;
      const end = Math.min(hudBuildText.length,
        hudBuildTextAt + glyphsThisSlice);
      for (; hudBuildTextAt < end; hudBuildTextAt++) {
        if (!addHudGlyph(hudBuildText[hudBuildTextAt],
            hudBuildTextX + hudBuildTextAt * 4 * hudBuildTextScale,
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
      /* Indicators share the retained HUD buffer. A text rebuild can move
         their suffix, so republish that small suffix before the next draw. */
      hudIndicatorDirty = true;
      hudTextUploads++;
      hudBuildPhase = -1;
      return;
    }
  }

  function rebuildHudIndicators() {
    hudIndicatorDirty = false;
    hudIndicatorVertexCount = hudIndicatorIndexCount = 0;
    if (state.mode === "generating") {
      hudPublishedIndicatorIndexCount = 0;
      return;
    }
    addHudIndicatorQuad(7, 176, 86, 2, HUD_GADGET_TRACK);
    addHudIndicatorQuad(7, 176,
      Math.max(1, (86 * hudGadgetRatio) | 0), 2, HUD_ACCENT);
    if (state.gameMode === 3) {
      addHudIndicatorQuad(117, 176, 86, 2, HUD_MULTIPLIER_TRACK);
      addHudIndicatorQuad(117, 176,
        Math.max(1, (86 * hudMultiplierRatio) | 0), 2, HUD_GOLD);
    }
    if (hudCommand) {
      addHudIndicatorQuad(227, 176, 86, 2, HUD_COMMAND_TRACK);
      addHudIndicatorQuad(227, 176,
        Math.max(1, (86 * hudCommandRatio) | 0), 2, HUD_GOLD);
    }
    /* Keep the objective cue on a broad compass ellipse. The old 38x30
       ellipse placed it directly over the player's tank and made an authored
       direction marker look like broken model geometry. */
    const objectiveX = 160 + Math.sin(hudObjectiveAngle) * 116;
    const objectiveY = 90 - Math.cos(hudObjectiveAngle) * 57;
    addHudIndicatorTriangle(objectiveX, objectiveY, 3,
      hudObjectiveAngle, HUD_ACCENT);
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
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 40, 0);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 40, 8);
      gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 40, 24);
      gl.enableVertexAttribArray(0);
      gl.enableVertexAttribArray(1);
      gl.enableVertexAttribArray(2);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    }
  }

  function drawHud() {
    if ((!hudPublishedIndexCount && !hudPublishedIndicatorIndexCount)
        || state.mode !== "playing") return;
    gl.disable(gl.DEPTH_TEST);
    /* Only authored ink has geometry, so stale texture state cannot turn
       glyph cells into opaque blocks. Blending remains for translucent bars. */
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    /* HUD ink is authored counter-clockwise for the PSP viewport. Culling
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
    gl.drawElements(gl.TRIANGLES,
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
    for (let first = 0; first < boxInstanceCount;
         first += MAX_INSTANCES_PER_DRAW) {
      if (!vertexArrays || first !== 0) {
        gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
        for (let column = 0; column < 4; column++) {
          const location = boxInstanceMatrixLocation + column;
          gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64,
            first * 64 + column * 16);
          gl.enableVertexAttribArray(location);
          instancing.vertexAttribDivisorANGLE(location, 1);
        }
        gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
        gl.vertexAttribPointer(boxInstanceTintLocation, 4, gl.FLOAT, false, 16,
          first * 16);
        gl.enableVertexAttribArray(boxInstanceTintLocation);
        instancing.vertexAttribDivisorANGLE(boxInstanceTintLocation, 1);
      }
      instancing.drawElementsInstancedANGLE(gl.TRIANGLES, BOX_INDICES.length,
        gl.UNSIGNED_SHORT, 0,
        Math.min(MAX_INSTANCES_PER_DRAW, boxInstanceCount - first));
    }
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
  const GENERATED_OBSTACLE_LIMIT = 16;
  const GENERATED_BARRIER_LIMIT = 6;
  const GENERATED_RAMP_LIMIT = 2;
  const GENERATED_STATIC_TAIL_INDEX_LIMIT = 8 * 36 + 2 * 18;
  const TANK_COLORS = [
    [.1, .92, .67], [1, .23, .16], [1, .51, .08],
    [.73, .25, 1], [1, .76, .1], [.16, .57, 1],
  ];
  const arenaSpatial = globalThis.__treadlineArenaData.spatial;
  const COLLISION_GRID_SIDE = 32, COLLISION_GRID_SCALE = 2;
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
  const fillArenaSpatialData = arenaSpatial.fillSpatialData;
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
    "ONSLAUGHT"];
  const MODE_DESCRIPTIONS = [
    "Clear three arenas before your last redeploy.",
    "Fight beside an ally and hold the central transmitter.",
    "Stay near the crawler and protect it through the arena.",
    "Survive escalating waves and protect your score chain.",
  ];
  const GADGETS = ["MINES", "SMOKE", "SHIELD", "REPAIR DRONE", "BOOST TREADS"];
  const DIFFICULTIES = ["CADET", "VETERAN", "ACE"];
  const CAMERA_MODES = ["STABLE", "FOLLOW"];
  const CONTROL_SCHEMES = ["ARCADE", "CLASSIC"];
  const AIM_ASSISTS = ["OFF", "SNAP", "LOCK"];
  const CONTROL_ARCADE = 0, CONTROL_CLASSIC = 1;
  const ASSIST_OFF = 0, ASSIST_SNAP = 1, ASSIST_LOCK = 2;
  const ARCADE_DEAD_ZONE = .22;
  const ARCADE_AIM_ROLL_FRAMES = 3;
  const ASSIST_CONE_COSINE_SQUARED = .75;
  const INVERSE_SQRT_TWO = .7071067811865476;
  const ASSIST_SNAP_BLEND = .6;
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
  for (let classId = 0; classId < CLASSES.length; classId++) {
    CLASS_SCALE[classId] = CLASSES[classId].scale;
    CLASS_SPEED[classId] = CLASSES[classId].speed;
    CLASS_COLLISION_RADIUS[classId] = .52 * CLASSES[classId].scale;
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
  const BOT_REACTION = [.32, .2, .12];
  const BOT_ACCURACY = [.28, .2, .13];
  const BOT_FIRE_CHANCE = [.24, .36, .46];
  const BOT_FIRE_WINDUP = [.5, .42, .35];
  const BOT_BACKOFF_ENTER_SQUARED = 4.41;
  const BOT_BACKOFF_EXIT_SQUARED = 7.29;
  const BOT_STANDOFF_RELEASE_SQUARED = 13.69;
  const BOT_STANDOFF_RADIUS = 3.05;
  const STORAGE_KEY = "treadline-settings-v1";
  /* Eighteen authored projectiles leave room for all critical tank/scenery
     instances inside the single 64-instance repeated draw. */
  const MAX_TANKS = 6, MAX_BULLETS = 18, MAX_PARTICLES = 48, MAX_PICKUPS = 6;
  const MAX_MINES = 12, MAX_SMOKE = 6, MAX_BARRIERS = 6;
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
    repair: 0, boost: 0, recoil: 0, hitFlash: 0,
    fireWindup: 0, fireSecondaryArmed: false, fireTelegraphed: false,
    renderTint: new Float32Array(3), spawnGrace: 0, respawn: 0,
    gadget: GADGETS[id % GADGETS.length], gadgetCooldown: 0,
    gadgetCooldownMax: 1,
    aiThink: 0, target: 0, role: "HUNTER", blockedTime: 0,
    avoidTime: 0, avoidTurn: 1, backingOff: false, standoff: false,
    orbitTurn: 1,
    lastAimX: NaN, lastAimZ: NaN, aimAngle: 0,
    lastTreadX: 0, lastTreadZ: 0, treadDistance: 0,
    command: {left: 0, right: 0, reverse: false, aimX: 0, aimZ: 1,
      fire: false, secondary: false, gadget: false, ultimate: false},
  }));
  const bullets = Array.from({length: MAX_BULLETS}, () => ({
    active: false, owner: 0, x: 0, z: 0, vx: 0, vz: 0, life: 0,
    heading: 0, headingSine: 0, headingCosine: 1,
    bounces: 0, damage: 0, barrierDamage: 1, pierce: 0,
    bypassShield: false, overCover: false,
  }));
  const particles = Array.from({length: MAX_PARTICLES}, () => ({
    active: false, x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0, life: 0, maximum: 0,
    renderSine: 0, renderCosine: 1, renderStreak: .12,
  }));
  const MAX_DECALS = 12;
  const decals = Array.from({length: MAX_DECALS}, () => ({
    active: false, kind: 0, x: 0, y: 0, z: 0, yaw: 0,
    sine: 0, cosine: 1,
    width: 0, depth: 0, life: 0, maximum: 0,
  }));
  let decalCursor = 0, decalRecycles = 0;
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
  function setParticleActive(at, active) {
    const bit = 1 << (at & 31);
    particles[at].active = active;
    if (at < 32)
      particleActiveLowMask = (active
        ? particleActiveLowMask | bit : particleActiveLowMask & ~bit) >>> 0;
    else
      particleActiveHighMask = (active
        ? particleActiveHighMask | bit : particleActiveHighMask & ~bit) >>> 0;
  }

  function spawnDecal(kind, x, z, yaw, width = .42, depth = .18) {
    const decal = decals[decalCursor];
    if (decal.active) decalRecycles++;
    decalCursor = (decalCursor + 1) % MAX_DECALS;
    decal.active = true;
    decal.kind = kind;
    decal.x = x; decal.z = z; decal.y = surfaceHeightAt(x, z) + .018;
    decal.yaw = yaw; decal.width = width; decal.depth = depth;
    const direction = orientationIndex(yaw);
    decal.sine = directionSines[direction];
    decal.cosine = directionCosines[direction];
    decal.life = decal.maximum = kind === 1 ? 6 : 9;
  }

  function updateDecals(dt) {
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
    active: false, team: 0, x: 0, z: 0, arm: 0, life: 0,
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
    particleTemplateSeed ^= particleTemplateSeed << 13;
    particleTemplateSeed ^= particleTemplateSeed >>> 17;
    particleTemplateSeed ^= particleTemplateSeed << 5;
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
    return (Math.round(angle * DIRECTION_COUNT / (Math.PI * 2))
      % DIRECTION_COUNT + DIRECTION_COUNT) % DIRECTION_COUNT;
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
  const pickupTypes = ["COOLANT", "ARMOR"];
  const state = {
    mode: "title", arena: 0, score: 0, lives: 3, kills: 0, time: 0,
    wallTime: 0, wave: 1, multiplier: 1, bestWave: 0, bestScore: 0,
    killBeat: 0, pendingClear: false, heartbeatAt: 0,
    transition: 0, shake: 0, flash: 0, frames: 0, running: true,
    gameMode: 0, classChoice: 1, gadgetChoice: 0, difficultyChoice: 1,
    blueControl: 0, redControl: 0,
    gateOpen: false, ricochets: 0, barriersBroken: 0,
    shots: 0, hits: 0, damageTaken: 0, objectiveTicks: 0,
    hitConfirm: 0, damageIndicator: 0, damageAngle: 0,
    botTelegraphs: 0, botPlayerShots: 0, botTelegraphViolations: 0,
    botVolleyMinimum: 99,
    commandMeter: 0, armorZone: "FRONT", arenaSeed: 0,
  };
  const convoy = {active: false, x: -1.8, z: -4.8, health: 180, progress: 0};
  let staticIndexCount = 0, arenaPlaneMaxSpan = 0;
  let arenaCommonVertexCount = 0, arenaCommonIndexCount = 0;
  let arenaCommonPlaneMaxSpan = 0, arenaCommonReady = false;
  let arenaBuildGeometryMs = 0, arenaBuildUploadMs = 0;
  let arenaMaximumStaticIndexCount = 0;
  let arenaPackedVertexCount = 0;
  const arenaGeometryCache = new Array(ARENAS.length);
  let cameraYaw = 0, toastUntil = 0;
  let cameraSine = 0, cameraCosine = 1;
  let cameraSafetyUpdates = 0;
  let qualificationCameraSwitches = 0;
  let debugBotsFrozen = false;
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
  const arenaGenerationFrameMaximums = qualificationLongSoak
    ? new Float32Array(10) : null;
  let arenaGenerationReportFrames = -1;
  let arenaGenerationReport = "";
  let lastBotPlayerShotAt = -99;
  const preferences = {
    controls: CONTROL_ARCADE, assist: ASSIST_SNAP,
    camera: 0, command: false, music: false, effects: true, shake: true,
  };
  const aimProbe = {x: 0, z: 0, bounceX: 0, bounceZ: 0, hit: false};
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

  const ONLINE_PROTOCOL_VERSION = 3;
  const ONLINE_INPUT_BYTES = 12;
  const ONLINE_SNAPSHOT_HEADER_BYTES = 32;
  const ONLINE_GENERATED_ARENA = 255;
  const ONLINE_TANK_BYTES = 16;
  const ONLINE_BULLET_BYTES = 12;
  const ONLINE_BULLET_LIMIT = 16;
  const ONLINE_PEER_TIMEOUT = 15;

  function playerTank() {
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
      preferences.music = saved.music === true;
      preferences.effects = saved.effects !== false;
      preferences.shake = saved.shake !== false;
      preferences.camera = saved.camera === 1 ? 1 : 0;
      preferences.command = saved.command === true;
      preferences.controls = saved.controls === CONTROL_CLASSIC
        ? CONTROL_CLASSIC : CONTROL_ARCADE;
      preferences.assist = Number.isInteger(saved.assist)
        && saved.assist >= ASSIST_OFF && saved.assist <= ASSIST_LOCK
        ? saved.assist : ASSIST_SNAP;
      if (Number.isInteger(saved.bestWave) && saved.bestWave >= 0)
        state.bestWave = Math.min(999, saved.bestWave);
      if (Number.isInteger(saved.bestScore) && saved.bestScore >= 0)
        state.bestScore = Math.min(99999999, saved.bestScore);
    } catch (_) {}
  }

  function savePreferences() {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify({
        mode: state.gameMode, classId: state.classChoice,
        gadget: state.gadgetChoice,
        difficulty: state.difficultyChoice,
        camera: preferences.camera,
        controls: preferences.controls, assist: preferences.assist,
        command: preferences.command,
        music: preferences.music, effects: preferences.effects,
        shake: preferences.shake,
        bestWave: state.bestWave, bestScore: state.bestScore,
      }));
    } catch (_) {}
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

  function validateArenaLayout(index, mode) {
    return arenaGenerator.validate(index, mode);
  }

  function generateArena(seed) {
    seed = seed >>> 0 || 1;
    const generated = arenaGenerator.generate(seed);
    arenaGenerationAttempts = generated.attempts;
    arenaGenerationFallback = generated.fallback;
    arenaGenerationChecksum = generated.checksum;
    state.arenaSeed = generated.winningSeed;
    fillArenaSpatialData(GENERATED_ARENA_INDEX);
    generatedGeometryDirty = true;
    return generated.accepted;
  }

  function wrapAngle(angle) {
    while (angle > Math.PI) angle -= Math.PI * 2;
    while (angle < -Math.PI) angle += Math.PI * 2;
    return angle;
  }

  function approachAngle(current, target, amount) {
    const difference = wrapAngle(target - current);
    return current + Math.max(-amount, Math.min(amount, difference));
  }

  function activeEnemyCount() {
    let count = 0;
    for (let at = 1; at < MAX_TANKS; at++)
      if (tanks[at].active && tanks[at].team !== tanks[0].team) count++;
    return count;
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
      && mode !== "arena-clear" && mode !== "hidden";
  }

  function recordOnslaughtBest() {
    if (state.gameMode !== 3) return;
    const wave = Math.max(1, state.wave | 0);
    const score = Math.max(0, state.score | 0);
    if (wave <= state.bestWave && score <= state.bestScore) return;
    state.bestWave = Math.max(state.bestWave, wave);
    state.bestScore = Math.max(state.bestScore, score);
    savePreferences();
  }

  function showPanel(title, message, label) {
    ui.heading.textContent = title;
    ui.message.textContent = message;
    ui.play.textContent = label;
    const forging = title.slice(0, 13) === "FORGING ARENA";
    if (ui.loadout) ui.loadout.hidden = title === "PAUSED" || forging;
    if (ui.stats) {
      const finished = title === "TANK LOST" || title === "MISSION COMPLETE";
      ui.stats.hidden = !finished;
      if (finished) ui.stats.textContent =
        `${state.hits}/${state.shots} hits · ${state.ricochets} ricochets · `
        + `${state.barriersBroken} barriers · ${state.objectiveTicks | 0}s objective`
        + (state.gameMode === 3 && state.arenaSeed
          ? ` · seed ${state.arenaSeed}` : "");
    }
    ui.play.hidden = forging;
    ui.panel.hidden = false;
  }

  function setMode(mode) {
    const authoredWasVisible = authoredSurfaceVisible(state.mode);
    if ((mode === "game-over" || mode === "victory")
        && state.gameMode === 3) recordOnslaughtBest();
    state.mode = mode;
    if (mode === "playing" && !qualificationActions)
      globalThis.pocSummary = "TREADLINE-PLAYING";
    const authoredIsVisible = authoredSurfaceVisible(mode);
    hudMeshDirty = true;
    hudIndicatorDirty = true;
    /* arena-clear and hidden retain the same full-canvas presentation as
       playing. Crossing among those states must not rewrite the concealed
       HTML shell: doing so forces a full page relayout in the middle of an
       otherwise bounded gameplay frame. */
    if (!authoredWasVisible && !authoredIsVisible) return;
    const canvasMode = mode === "playing" || mode === "generating";
    if (ui.shell) ui.shell.classList.toggle("game-running", canvasMode);
    const gameplayHud = [ui.hud, ui.gadget, ui.gadgetMeterShell,
      ui.commandStatus, ui.commandMeterShell, ui.objectiveArrow,
      ui.hitMarker, ui.damageArrow, ui.toast];
    for (const element of gameplayHud)
      if (element) element.hidden = canvasMode;
    if (mode === "playing") {
      ui.play.hidden = false;
      ui.panel.hidden = true;
    }
    else if (mode === "generating") {
      ui.panel.hidden = true;
      hudScore = hudArmor = hudGadget = hudCommand = "";
      hudArena = `RUN SEED ${arenaGenerationRunSeed}`;
      hudToast = forgingHeadings[0];
      hudMeshDirty = hudIndicatorDirty = true;
    }
    else if (mode === "paused")
      showPanel("PAUSED", `${GAME_MODES[state.gameMode]} · simulation frozen.`, "Resume");
    else if (mode === "game-over")
      showPanel("TANK LOST", `Final score ${state.score}. The arena held.`
        + (state.gameMode === 3 ? ` Seed ${state.arenaSeed}.` : ""), "Deploy again");
    else if (mode === "victory")
      showPanel("MISSION COMPLETE", `Final score ${state.score}. Objective secured.`
        + (state.gameMode === 3 ? ` Seed ${state.arenaSeed}.` : ""), "Play again");
    if (authoredIsVisible) updateHud(true);
  }

  function refreshSetupLabels() {
    if (ui.modeValue) ui.modeValue.textContent = GAME_MODES[state.gameMode];
    if (ui.classValue) ui.classValue.textContent = CLASSES[state.classChoice].name;
    if (ui.gadgetValue) ui.gadgetValue.textContent = GADGETS[state.gadgetChoice];
    if (ui.difficultyValue)
      ui.difficultyValue.textContent = DIFFICULTIES[state.difficultyChoice];
    if (ui.cameraValue) ui.cameraValue.textContent = CAMERA_MODES[preferences.camera];
    if (ui.controlsValue)
      ui.controlsValue.textContent = CONTROL_SCHEMES[preferences.controls];
    if (ui.assistValue)
      ui.assistValue.textContent = AIM_ASSISTS[preferences.assist];
    if (ui.commandValue) ui.commandValue.textContent = preferences.command ? "On" : "Off";
    if (ui.musicValue) ui.musicValue.textContent = preferences.music ? "On" : "Off";
    if (ui.effectsValue) ui.effectsValue.textContent = preferences.effects ? "On" : "Off";
    if (ui.shakeValue) ui.shakeValue.textContent = preferences.shake ? "On" : "Off";
    if (state.mode === "title" && ui.message) {
      ui.message.textContent = MODE_DESCRIPTIONS[state.gameMode]
        + (state.gameMode === 3
          ? ` Best W${state.bestWave} · ${state.bestScore}.` : "");
      if (ui.gadget) ui.gadget.textContent = `${GADGETS[state.gadgetChoice]} · READY`;
      updateHud(true);
    }
  }

  if (ui.modeChoice) ui.modeChoice.addEventListener("click", () => {
    state.gameMode = (state.gameMode + 1) % GAME_MODES.length;
    refreshSetupLabels(); savePreferences();
  });
  if (ui.classChoice) ui.classChoice.addEventListener("click", () => {
    state.classChoice = (state.classChoice + 1) % CLASSES.length;
    refreshSetupLabels(); savePreferences();
  });
  if (ui.gadgetChoice) ui.gadgetChoice.addEventListener("click", () => {
    state.gadgetChoice = (state.gadgetChoice + 1) % GADGETS.length;
    refreshSetupLabels(); savePreferences();
  });
  if (ui.difficultyChoice) ui.difficultyChoice.addEventListener("click", () => {
    state.difficultyChoice = (state.difficultyChoice + 1) % DIFFICULTIES.length;
    refreshSetupLabels(); savePreferences();
  });
  if (ui.cameraChoice) ui.cameraChoice.addEventListener("click", () => {
    preferences.camera = preferences.camera ? 0 : 1;
    if (!preferences.camera) {
      cameraYaw = 0; cameraSine = 0; cameraCosine = 1;
    }
    refreshSetupLabels(); savePreferences();
  });
  if (ui.controlsChoice) ui.controlsChoice.addEventListener("click", () => {
    preferences.controls = preferences.controls === CONTROL_ARCADE
      ? CONTROL_CLASSIC : CONTROL_ARCADE;
    clearSchemeKeys();
    resetAimAssist();
    refreshSetupLabels(); savePreferences(); updateControlHint();
  });
  if (ui.assistChoice) ui.assistChoice.addEventListener("click", () => {
    preferences.assist = (preferences.assist + 1) % AIM_ASSISTS.length;
    resetAimAssist();
    refreshSetupLabels(); savePreferences(); updateControlHint();
  });
  if (ui.commandChoice) ui.commandChoice.addEventListener("click", () => {
    preferences.command = !preferences.command;
    refreshSetupLabels(); savePreferences(); updateHud(true);
  });
  if (ui.musicChoice) ui.musicChoice.addEventListener("click", () => {
    preferences.music = !preferences.music;
    refreshSetupLabels(); savePreferences(); sounds.start();
    sounds.setMusic(preferences.music);
  });
  if (ui.effectsChoice) ui.effectsChoice.addEventListener("click", () => {
    preferences.effects = !preferences.effects;
    refreshSetupLabels(); savePreferences();
  });
  if (ui.shakeChoice) ui.shakeChoice.addEventListener("click", () => {
    preferences.shake = !preferences.shake;
    if (!preferences.shake) state.shake = 0;
    refreshSetupLabels(); savePreferences();
  });

  function showOnlineSurface(message) {
    if (ui.panel) ui.panel.hidden = false;
    if (ui.panel) ui.panel.classList.remove("web-pairing-panel");
    if (ui.heading) ui.heading.textContent = "ONLINE TEAM CONTROL";
    if (ui.loadout) ui.loadout.hidden = true;
    if (ui.preferences) ui.preferences.hidden = true;
    if (ui.play) ui.play.hidden = true;
    if (ui.onlineActions) ui.onlineActions.hidden = true;
    if (ui.codeEntry) ui.codeEntry.hidden = true;
    if (ui.webPairing) ui.webPairing.hidden = true;
    if (ui.onlineStatus) ui.onlineStatus.hidden = false;
    if (ui.onlineMessage) ui.onlineMessage.textContent = message;
    if (ui.onlineInvite) ui.onlineInvite.textContent =
      !webPairing && online.role === "host" && online.localCode
        ? `CODE ${formatInviteCode(online.localCode)}` : "";
    if (ui.onlineResponse) ui.onlineResponse.hidden = webPairing
      || online.role !== "host" || !online.localCode;
    if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = true;
  }

  function restoreTitleSurface() {
    state.mode = "title";
    if (ui.panel) ui.panel.hidden = false;
    if (ui.panel) ui.panel.classList.remove("web-pairing-panel");
    if (ui.heading) ui.heading.textContent = "TREADLINE ARENA";
    if (ui.message) ui.message.hidden = false;
    if (ui.controls) ui.controls.hidden = false;
    if (ui.loadout) ui.loadout.hidden = false;
    if (ui.preferences) ui.preferences.hidden = false;
    if (ui.play) ui.play.hidden = false;
    if (ui.onlineActions) ui.onlineActions.hidden = false;
    if (ui.onlineStatus) ui.onlineStatus.hidden = true;
    if (ui.codeEntry) ui.codeEntry.hidden = true;
    if (ui.webPairing) ui.webPairing.hidden = true;
    refreshSetupLabels();
  }

  function showWebPairing(mode) {
    online.webPairingMode = mode;
    if (ui.panel) ui.panel.hidden = false;
    if (ui.panel) ui.panel.classList.add("web-pairing-panel");
    if (ui.heading) ui.heading.textContent = "WEB TEAM CONTROL";
    if (ui.message) ui.message.hidden = true;
    if (ui.controls) ui.controls.hidden = true;
    if (ui.loadout) ui.loadout.hidden = true;
    if (ui.preferences) ui.preferences.hidden = true;
    if (ui.play) ui.play.hidden = true;
    if (ui.onlineActions) ui.onlineActions.hidden = true;
    if (ui.onlineStatus) ui.onlineStatus.hidden = true;
    if (ui.codeEntry) ui.codeEntry.hidden = true;
    if (ui.webPairing) ui.webPairing.hidden = false;
    const host = mode === "host";
    const guestResponse = mode === "guest-response";
    const connecting = mode === "connecting";
    const guestOffer = !host && !guestResponse && !connecting;
    if (ui.webStep) ui.webStep.textContent = host
      ? "STEP 1 · SEND OFFER / STEP 2 · PASTE RESPONSE"
      : guestResponse ? "STEP 2 · SEND RESPONSE"
        : connecting ? "CONNECTING" : "STEP 1 · PASTE OFFER";
    if (ui.webHelp) ui.webHelp.textContent = host
      ? "Send the offer privately. Paste the response from the guest below."
      : guestResponse ? "Send this response back to the host. Keep this page open."
        : connecting ? "Keep both game pages open while they connect directly."
          : "Paste the offer sent by the host, then create a response.";
    if (ui.webStatus)
      ui.webStatus.textContent = host ? "Preparing offer…"
        : guestResponse ? "Waiting for the host to connect…"
          : connecting ? "Connecting directly…" : "Ready for the host offer.";
    if (ui.webShareLabel) ui.webShareLabel.hidden = !host && !guestResponse;
    if (ui.webShare) ui.webShare.hidden = !host && !guestResponse;
    if (ui.webShareSize) ui.webShareSize.hidden = !host && !guestResponse;
    if (ui.webCopy) ui.webCopy.hidden = !host && !guestResponse;
    if (ui.webInputLabel) {
      ui.webInputLabel.hidden = guestResponse || connecting;
      ui.webInputLabel.textContent = host
        ? "Paste the guest response" : "Paste the host offer";
    }
    if (ui.webInput) {
      ui.webInput.hidden = guestResponse || connecting;
      if (guestOffer) ui.webInput.value = "";
    }
    if (ui.webApply) {
      ui.webApply.hidden = guestResponse || connecting;
      ui.webApply.disabled = connecting;
      ui.webApply.textContent = host ? "Connect" : "Create response";
    }
    if (ui.webShareLabel) ui.webShareLabel.textContent = host
      ? "Send this offer to the guest" : "Send this response to the host";
    if (ui.webCopy) ui.webCopy.textContent = host ? "Copy offer" : "Copy response";
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
    if (ui.onlineMessage) ui.onlineMessage.textContent = String(message);
    if (ui.onlineInvite) ui.onlineInvite.textContent = "";
    if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = true;
  }

  function beginOnlineMatch() {
    online.active = true;
    online.inputSequence = online.receivedInputSequence = 0;
    online.snapshotSequence = 0;
    online.receivedInputValid = online.snapshotValid = false;
    online.arenaWire = -1; online.arenaSeed = online.arenaChecksum = 0;
    online.arenaRejected = false;
    online.disconnectReason = "";
    online.lastInputSend = online.lastSnapshotSend = state.time;
    online.lastReceive = state.time;
    state.gameMode = 1;
    tanks[0].player = online.role !== "guest";
    tanks[1].player = online.role === "guest";
    resetGame();
    if (ui.controls) ui.controls.hidden = false;
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
        if (ui.webStatus) ui.webStatus.textContent = detail;
      } else if (ui.onlineMessage) ui.onlineMessage.textContent = detail;
    };
    channel.oninvitecode = event => {
      online.code = String(event.code || "");
      online.localCode = online.code;
      if (webPairing) {
        showWebPairing(role === "host" ? "host" : "guest-response");
        if (ui.webShare) ui.webShare.value = online.code;
        if (ui.webShareSize)
          ui.webShareSize.textContent = `${online.code.length} characters`;
        if (ui.webStatus) ui.webStatus.textContent = event.detail || "Pairing text ready.";
      } else if (ui.onlineInvite) {
        ui.onlineInvite.textContent = `CODE ${formatInviteCode(online.code)}`;
      }
      if (ui.onlineResponse) ui.onlineResponse.hidden = role !== "host";
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
      if (ui.onlineMessage)
        ui.onlineMessage.textContent = `Found ${event.name || "LAN game"}`;
      if (ui.onlineAccept) ui.onlineAccept.textContent = "Join";
      if (ui.onlineReject) ui.onlineReject.textContent = "Ignore";
      if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = false;
    };
    channel.onpeerrequest = event => {
      online.pendingPeer = event.peerIdentity;
      if (ui.onlineMessage)
        ui.onlineMessage.textContent = `${event.name || "Player"} wants to join.`;
      if (ui.onlineAccept) ui.onlineAccept.textContent = "Accept";
      if (ui.onlineReject) ui.onlineReject.textContent = "Reject";
      if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = false;
    };
    channel.onopen = () => beginOnlineMatch();
    channel.onmessage = event => {
      if (!(event.data instanceof ArrayBuffer)) return;
      const bytes = new Uint8Array(event.data);
      if (bytes[0] === 1 && online.role === "host") applyOnlineInput(event.data);
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

  if (ui.onlineHost) ui.onlineHost.addEventListener("click", () => startOnline("host"));
  if (ui.onlineLan) ui.onlineLan.addEventListener("click", () => startOnline("discover"));
  if (ui.onlineCode) ui.onlineCode.addEventListener("click", () => {
    if (webPairing) {
      online.entryMode = "web-offer";
      showWebPairing("guest-offer");
      return;
    }
    if (ui.loadout) ui.loadout.hidden = true;
    if (ui.preferences) ui.preferences.hidden = true;
    if (ui.play) ui.play.hidden = true;
    if (ui.onlineActions) ui.onlineActions.hidden = true;
    if (ui.onlineStatus) ui.onlineStatus.hidden = true;
    if (ui.codeEntry) ui.codeEntry.hidden = false;
    online.code = ""; online.entryMode = "join";
    if (ui.codeValue) ui.codeValue.textContent = "—";
    const first = ui.codeKeypad && ui.codeKeypad.querySelector("button");
    if (first) first.focus();
  });
  if (ui.codeKeypad) ui.codeKeypad.addEventListener("click", event => {
    const button = event.target.closest("button");
    if (!button) return;
    const key = button.dataset.key || button.textContent.trim();
    if (key === "delete") online.code = online.code.slice(0, -1);
    else if (key === "join") {
      if (online.code.length !== 12 && online.code.length !== 17) {
        if (ui.codeValue) ui.codeValue.textContent = "12 OR 17 DIGITS";
        return;
      }
      if (online.entryMode === "response" && online.channel) {
        const added = online.channel.addRemoteCode(online.code);
        showOnlineSurface(added ? "Punching through NAT…" : "Code was not accepted.");
      } else startOnline("guest", online.code);
      return;
    } else if (/^\d$/.test(key) && online.code.length < 17) online.code += key;
    if (ui.codeValue)
      ui.codeValue.textContent = online.code ? formatInviteCode(online.code) : "—";
  });
  if (ui.onlineResponse) ui.onlineResponse.addEventListener("click", () => {
    if (!online.channel || online.role !== "host") return;
    if (ui.onlineStatus) ui.onlineStatus.hidden = true;
    if (ui.codeEntry) ui.codeEntry.hidden = false;
    online.code = ""; online.entryMode = "response";
    if (ui.codeValue) ui.codeValue.textContent = "—";
    const first = ui.codeKeypad && ui.codeKeypad.querySelector("button");
    if (first) first.focus();
  });
  if (ui.webCopy) ui.webCopy.addEventListener("click", async () => {
    const text = ui.webShare ? ui.webShare.value : "";
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      if (ui.webStatus) ui.webStatus.textContent = online.role === "host"
        ? "Offer copied. Paste the guest response below when it arrives."
        : "Response copied. Send it to the host and keep this page open.";
      showToast("PAIRING TEXT COPIED", 1.2);
    } catch (_) {
      if (ui.webShare) { ui.webShare.focus(); ui.webShare.select(); }
      showToast("COPY THE SELECTED TEXT", 1.5);
    }
  });
  if (ui.webApply) ui.webApply.addEventListener("click", () => {
    const text = ui.webInput ? ui.webInput.value.trim() : "";
    if (!text) {
      if (ui.webInput) ui.webInput.focus();
      return;
    }
    if (online.role === "host" && online.channel) {
      const accepted = online.channel.addRemoteCode(text);
      if (accepted) {
        showWebPairing("connecting");
        if (ui.webStatus) ui.webStatus.textContent = "Checking the guest response…";
      }
      else showToast("RESPONSE WAS NOT ACCEPTED", 1.5);
    } else startOnline("guest", text);
  });
  if (ui.webCancel) ui.webCancel.addEventListener("click", () => {
    if (online.channel) online.channel.close(1000, "cancelled");
    online.channel = null;
    online.active = false;
    restoreTitleSurface();
  });
  if (ui.codeCancel) ui.codeCancel.addEventListener("click", () => {
    if (online.entryMode === "response" && online.channel)
      showOnlineSurface("Waiting for a player…");
    else restoreTitleSurface();
  });
  if (ui.onlineCancel) ui.onlineCancel.addEventListener("click", () => {
    if (online.channel) online.channel.close(1000, "cancelled");
    online.channel = null; online.active = false;
    restoreTitleSurface();
  });
  if (ui.onlineAccept) ui.onlineAccept.addEventListener("click", () => {
    if (!online.channel) return;
    if (online.role === "host") online.channel.accept(online.pendingPeer, true);
    else if (online.role === "discover") {
      online.role = "guest";
      online.channel.addRemoteCode(online.code);
    }
    if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = true;
    if (ui.onlineMessage) ui.onlineMessage.textContent = "Connecting…";
  });
  if (ui.onlineReject) ui.onlineReject.addEventListener("click", () => {
    if (online.channel && online.role === "host")
      online.channel.accept(online.pendingPeer, false);
    if (ui.onlinePeerActions) ui.onlinePeerActions.hidden = true;
    if (ui.onlineMessage) ui.onlineMessage.textContent =
      online.role === "host" ? "Waiting for a player…" : "Looking on this Wi-Fi…";
  });

  class SoundBank {
    constructor() {
      this.context = null; this.voices = []; this.next = 0;
      this.engine = null; this.engineStep = -1; this.engineRole = "off";
      this.noise = null;
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
            this.voices.push({oscillator, gain, startAt: 0, stopAt: 0,
              attack: .012, volume: 0, startFrequency: 0, endFrequency: 0,
              secondFrequency: 0, secondAt: 0, scheduledEnvelope: false,
              lastFrequency: 0});
          }
          const gain = this.context.createGain();
          const oscillator = this.context.createOscillator();
          oscillator.type = "triangle"; oscillator.frequency.value = 82;
          gain.gain.value = 0;
          oscillator.connect(gain).connect(this.context.destination);
          oscillator.start();
          this.engine = {oscillator, gain};
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
          this.noise = {source, gain, startAt: 0, stopAt: 0,
            volume: 0, scheduledEnvelope: false};
        }).catch(() => {});
      } catch (_) { this.context = null; }
    }
    targetEnvelope(gain, volume, duration) {
      const parameter = gain.gain;
      if (typeof parameter.setTargetAtTime !== "function"
          || !this.context) {
        parameter.value = 0;
        return false;
      }
      const now = this.context.currentTime;
      parameter.cancelScheduledValues(now);
      parameter.setValueAtTime(0, now);
      parameter.setTargetAtTime(volume, now, .006);
      parameter.setTargetAtTime(0, now + Math.max(.018, duration * .35),
        Math.max(.012, duration * .18));
      return true;
    }
    play(frequency, duration = .05, volume = .1,
         endFrequency = frequency, secondFrequency = 0,
         secondDelay = duration * .5) {
      if (!preferences.effects) return;
      if (!this.voices.length) return;
      const voice = this.voices[this.next++ % this.voices.length];
      voice.oscillator.frequency.value = frequency;
      voice.lastFrequency = frequency;
      voice.startAt = state.wallTime;
      voice.stopAt = state.wallTime + duration;
      voice.volume = volume;
      voice.startFrequency = frequency;
      voice.endFrequency = endFrequency;
      voice.secondFrequency = secondFrequency;
      voice.secondAt = secondDelay;
      voice.scheduledEnvelope = this.targetEnvelope(
        voice.gain, volume, duration);
    }
    gateNoise(duration, volume) {
      if (!preferences.effects || !this.noise) return;
      this.noise.startAt = state.wallTime;
      this.noise.stopAt = state.wallTime + duration;
      this.noise.volume = volume;
      this.noise.scheduledEnvelope = this.targetEnvelope(
        this.noise.gain, volume, duration);
    }
    shot(player) {
      this.play(player ? 185 : 138, .07, player ? .13 : .055,
        player ? 142 : 112);
    }
    hit() { this.play(610, .055, .06, 760); }
    kill() { this.play(520, .24, .13, 560, 760, .11); }
    damage() { this.play(104, .11, .11, 72); }
    ricochet() {
      this.play(920, .055, .05, 1180);
      this.gateNoise(.045, .025);
    }
    explosion(volume = .14) {
      this.play(112, .22, volume, 48);
      this.gateNoise(.18, volume * .72);
    }
    heartbeat() { this.play(68, .13, .075, 54); }
    waveStart() { this.play(330, .22, .11, 520); }
    waveClear() { this.play(560, .28, .13, 620, 820, .13); }
    telegraph() { this.play(760, .045, .028, 920); }
    pickup() { this.play(440, .11, .1, 620); }
    gadget() { this.play(520, .13, .11, 690); }
    command() { this.play(720, .18, .14, 920); }
    tickVoice(voice, time) {
      if (voice.stopAt <= 0) return;
      const elapsed = time - voice.startAt;
      const duration = voice.stopAt - voice.startAt;
      if (elapsed >= duration) {
        voice.gain.gain.value = 0; voice.stopAt = 0;
        return;
      }
      if (!voice.scheduledEnvelope) {
        const attack = Math.min(voice.attack, duration * .25);
        const level = elapsed < attack ? Math.max(0, elapsed / attack)
          : Math.max(0, 1 - (elapsed - attack)
            / Math.max(.001, duration - attack));
        voice.gain.gain.value = voice.volume * level;
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
        voice.oscillator.frequency.value = frequency;
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
          this.noise.gain.gain.value = 0;
          this.noise.stopAt = 0;
        } else if (!this.noise.scheduledEnvelope) {
          const attack = Math.min(.008, duration * .2);
          const level = elapsed < attack ? Math.max(0, elapsed / attack)
            : Math.max(0, 1 - (elapsed - attack)
              / Math.max(.001, duration - attack));
          this.noise.gain.gain.value = this.noise.volume * level;
        }
      }
      const active = mode === "playing" && player?.active;
      const menuMusic = preferences.music && (mode === "title"
        || mode === "paused" || mode === "game-over" || mode === "victory");
      this.engineRole = menuMusic ? "melody" : active ? "hum" : "off";
      const step = (time * 10) | 0;
      if (step === this.engineStep || !this.engine) return;
      this.engineStep = step;
      const moving = active
        ? Math.min(1, (Math.abs(player.command.left)
          + Math.abs(player.command.right)) * .5) : 0;
      const boosted = active && player.boost > 0;
      const notes = [82, 98, 110, 98, 73, 82, 123, 98];
      if (menuMusic) {
        this.engine.oscillator.frequency.value = notes[((time * 1.6) | 0) & 7];
        this.engine.gain.gain.value = .018;
      } else if (active) {
        this.engine.oscillator.frequency.value = 64 + moving * 34
          + (boosted ? 25 : 0);
        this.engine.gain.gain.value = preferences.effects
          ? .009 + moving * .012 + (boosted ? .006 : 0) : 0;
      } else this.engine.gain.gain.value = 0;
    }
  }
  const sounds = new SoundBank();

  function requestPresentation() {
    sounds.start();
    const shell = document.getElementById("game-shell");
    if (shell && navigator.tilefinch?.requestPageControls)
      navigator.tilefinch.requestPageControls(shell).catch(() => {});
    else if (shell && shell.requestFullscreen)
      shell.requestFullscreen().catch(() => {});
    canvas.focus();
  }

  function addStaticVertex(x, y, z, red, green, blue) {
    const p = vertexCount * 3, c = vertexCount * 4;
    positions[p] = x; positions[p + 1] = y; positions[p + 2] = z;
    colors[c] = red; colors[c + 1] = green;
    colors[c + 2] = blue; colors[c + 3] = 1;
    vertexCount++;
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
    const base = vertexCount, warm = shade >= 1;
    const red = Math.min(1, color[0] * shade * (warm ? 1 : .9));
    const green = Math.min(1, color[1] * shade * (warm ? .98 : .95));
    const blue = Math.min(1, color[2] * shade * (warm ? .92 : 1.1));
    addStaticVertex(ax, ay, az, red, green, blue);
    addStaticVertex(bx, by, bz, red, green, blue);
    addStaticVertex(cx, cy, cz, red, green, blue);
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    return true;
  }

  function addStaticQuad(ax, ay, az, bx, by, bz,
                         cx, cy, cz, dx, dy, dz, color, shade = 1) {
    if (vertexCount + 4 > MAX_VERTICES || indexCount + 6 > MAX_INDICES) {
      meshDrops++;
      return false;
    }
    arenaPlaneMaxSpan = Math.max(arenaPlaneMaxSpan,
      Math.max(ax, bx, cx, dx) - Math.min(ax, bx, cx, dx),
      Math.max(az, bz, cz, dz) - Math.min(az, bz, cz, dz));
    const base = vertexCount;
    const warm = shade >= 1;
    const red = Math.min(1, color[0] * shade * (warm ? 1 : .9));
    const green = Math.min(1, color[1] * shade * (warm ? .98 : .95));
    const blue = Math.min(1, color[2] * shade * (warm ? .92 : 1.1));
    addStaticVertex(ax, ay, az, red, green, blue);
    addStaticVertex(bx, by, bz, red, green, blue);
    addStaticVertex(cx, cy, cz, red, green, blue);
    addStaticVertex(dx, dy, dz, red, green, blue);
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base + 3;
    return true;
  }

  function addStaticGradientQuad(ax, ay, az, bx, by, bz,
                                 cx, cy, cz, dx, dy, dz,
                                 color, shadeA, shadeB, shadeC, shadeD) {
    if (vertexCount + 4 > MAX_VERTICES || indexCount + 6 > MAX_INDICES) {
      meshDrops++;
      return false;
    }
    arenaPlaneMaxSpan = Math.max(arenaPlaneMaxSpan,
      Math.max(ax, bx, cx, dx) - Math.min(ax, bx, cx, dx),
      Math.max(az, bz, cz, dz) - Math.min(az, bz, cz, dz));
    const base = vertexCount;
    addStaticVertex(ax, ay, az,
      Math.min(1, color[0] * shadeA), Math.min(1, color[1] * shadeA),
      Math.min(1, color[2] * shadeA));
    addStaticVertex(bx, by, bz,
      Math.min(1, color[0] * shadeB), Math.min(1, color[1] * shadeB),
      Math.min(1, color[2] * shadeB));
    addStaticVertex(cx, cy, cz,
      Math.min(1, color[0] * shadeC), Math.min(1, color[1] * shadeC),
      Math.min(1, color[2] * shadeC));
    addStaticVertex(dx, dy, dz,
      Math.min(1, color[0] * shadeD), Math.min(1, color[1] * shadeD),
      Math.min(1, color[2] * shadeD));
    indices[indexCount++] = base;
    indices[indexCount++] = base + 1;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base;
    indices[indexCount++] = base + 2;
    indices[indexCount++] = base + 3;
    return true;
  }

  function floorVignette(x, z) {
    const edge = Math.max(Math.abs(x), Math.abs(z)) / 9;
    return 1.16 - Math.min(1, edge) * .34;
  }

  function addHorizonBackdrop() {
    const base = [-.002, .11, .13], high = 5.6, edge = 10.8;
    const step = edge * 2 / 5;
    for (let segment = 0; segment < 5; segment++) {
      const first = -edge + segment * step, last = first + step;
      addStaticGradientQuad(first, -.05, edge, last, -.05, edge,
        last, high, edge, first, high, edge, base, 1, 1, .05, .05);
      addStaticGradientQuad(last, -.05, -edge, first, -.05, -edge,
        first, high, -edge, last, high, -edge, base, 1, 1, .05, .05);
      addStaticGradientQuad(-edge, -.05, first, -edge, -.05, last,
        -edge, high, last, -edge, high, first, base, 1, 1, .05, .05);
      addStaticGradientQuad(edge, -.05, last, edge, -.05, first,
        edge, high, first, edge, high, last, base, 1, 1, .05, .05);
    }
    const skyline = [.8, 1.25, .65, 1.55, .9, 1.18];
    for (let at = 0; at < skyline.length; at++) {
      const center = -7.5 + at * 3;
      const height = skyline[at];
      const far = at & 1 ? -10.72 : 10.72;
      const facing = far < 0 ? -1 : 1;
      addStaticGradientQuad(center - .64, -.04, far,
        center + .64, -.04, far,
        center + .64, height, far - facing * .01,
        center - .64, height, far - facing * .01,
        [.035, .085, .09], .92, .92, .48, .48);
    }
  }

  function addArenaWallSegment(center, horizontal, edge, inside) {
    const half = 2.125, outer = edge - inside * .35;
    const inner = edge, low = -.005, high = .845;
    if (horizontal) {
      addStaticQuad(center - half, high, outer,
        center + half, high, outer,
        center + half, high, inner,
        center - half, high, inner, COLORS.wall, 1.12);
      addStaticQuad(center - half, low, inner,
        center + half, low, inner,
        center + half, high, inner,
        center - half, high, inner, COLORS.wall, .82);
    } else {
      addStaticQuad(outer, high, center - half,
        inner, high, center - half,
        inner, high, center + half,
        outer, high, center + half, COLORS.wall, 1.12);
      addStaticQuad(inner, low, center - half,
        inner, low, center + half,
        inner, high, center + half,
        inner, high, center - half, COLORS.wall, .82);
    }
  }

  function appendArenaGeometry(index) {
    const arena = ARENAS[index];
    const obstacles = arena.obstacles;
    const obstacleCount = arenaObstacleCount(arena);
    for (let at = 0; at < obstacleCount; at++) {
      const obstacle = obstacles[at];
      addBox(obstacle[0], .48, obstacle[1], obstacle[2], .96, obstacle[3],
        0, COLORS.obstacle);
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
    const buildStarted = qualificationLongSoak ? performance.now() : 0;
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
          addStaticQuad(line - .0175, .014, last,
            line + .0175, .014, last,
            line + .0175, .014, first,
            line - .0175, .014, first, COLORS.grid);
          addStaticQuad(first, .014, line + .0175,
            last, .014, line + .0175,
            last, .014, line - .0175,
            first, .014, line - .0175, COLORS.grid);
        }
      }
      for (let segment = 0; segment < 4; segment++) {
        const center = -6.375 + segment * 4.25;
        addArenaWallSegment(center, true, -8.075, 1);
        addArenaWallSegment(center, true, 8.075, -1);
        addArenaWallSegment(center, false, -8.075, 1);
        addArenaWallSegment(center, false, 8.075, -1);
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
      if (initialArenaOnly && state.gameMode === 3
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
    const geometryFinished = qualificationLongSoak ? performance.now() : 0;
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
    if (qualificationLongSoak) {
      const uploadFinished = performance.now();
      arenaBuildGeometryMs = geometryFinished - buildStarted;
      arenaBuildUploadMs = uploadFinished - geometryFinished;
    }
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
      const obstacle = arena.obstacles[arenaGenerationGeometryCursor];
      addBox(obstacle[0], .48, obstacle[1], obstacle[2], .96, obstacle[3],
        0, COLORS.obstacle);
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

  function placeTank(tank, x, z, yaw, team, gadget, role = "HUNTER",
                     classId = tank.classId) {
    classId = Math.max(0, Math.min(CLASSES.length - 1, classId | 0));
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
    tank.shield = 0;
    tank.repair = 0; tank.boost = 0; tank.gadgetCooldown = 0;
    tank.recoil = tank.hitFlash = tank.fireWindup = 0;
    tank.fireSecondaryArmed = tank.fireTelegraphed = false;
    tank.gadgetCooldownMax = 1;
    tank.spawnGrace = 1; tank.respawn = 0;
    tank.gadget = gadget; tank.aiThink = 0; tank.role = role;
    tank.blockedTime = tank.avoidTime = 0;
    tank.avoidTurn = tank.id & 1 ? 1 : -1;
    tank.backingOff = false;
    tank.standoff = false;
    tank.orbitTurn = tank.id & 1 ? 1 : -1;
    tank.surfaceY = surfaceHeightAt(x, z);
    tank.lastTreadX = x; tank.lastTreadZ = z; tank.treadDistance = 0;
    tank.command.left = tank.command.right = 0;
    tank.command.fire = tank.command.secondary = tank.command.gadget = false;
    tank.command.ultimate = false;
    tank.command.reverse = false;
  }

  function beginArena(index, geometryPrepared = false) {
    const transitionStarted = qualificationLongSoak ? performance.now() : 0;
    state.arena = index;
    for (const bullet of bullets) bullet.active = false;
    for (const particle of particles) particle.active = false;
    for (const decal of decals) decal.active = false;
    decalCursor = 0;
    bulletActiveMask = particleActiveLowMask = particleActiveHighMask = 0;
    for (const pickup of pickups) pickup.active = false;
    for (const mine of mines) mine.active = false;
    for (const smoke of smokeClouds) smoke.active = false;
    activeSmokeCount = 0;
    for (const barrier of barriers) barrier.present = barrier.active = false;
    for (const tank of tanks) tank.active = false;
    const resetFinished = qualificationLongSoak ? performance.now() : 0;
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
    retainedSceneryDirty = true;
    state.blueControl = state.redControl = 0;
    state.gateOpen = false; state.ricochets = state.barriersBroken = 0;
    convoy.active = state.gameMode === 2;
    convoy.x = -1.8; convoy.z = -4.8;
    convoy.health = 180; convoy.progress = 0;
    placeTank(tanks[0], state.gameMode === 2 ? 1.6 : 0, -5.8, 0, 0,
      GADGETS[state.gadgetChoice], "PLAYER", state.classChoice);
    if (state.gameMode === 1) {
      placeTank(tanks[1], -2, -5.4, 0, 0, GADGETS[2], "CAPTURE", 1);
      for (let at = 2; at < 5; at++) {
        const angle = (at - 2) / 3 * Math.PI * 2 + .35;
        placeTank(tanks[at], Math.sin(angle) * 5.7,
          Math.cos(angle) * 5.7, wrapAngle(angle + Math.PI), 1,
          GADGETS[at % GADGETS.length],
          at === 2 ? "CAPTURE" : at === 3 ? "FLANK" : "GUARD", at % 3);
      }
    } else {
      const enemies = state.gameMode === 2 ? 4 : state.gameMode === 3
        ? Math.min(MAX_TANKS - 1, 2 + Math.ceil(state.wave * .5))
        : ARENAS[index].enemies;
      for (let at = 0; at < enemies; at++) {
        const angle = at / enemies * Math.PI * 2 + .35;
        placeTank(tanks[at + 1], Math.sin(angle) * 5.6,
          Math.cos(angle) * 5.6, wrapAngle(angle + Math.PI), 1,
          GADGETS[(at + 1) % GADGETS.length],
          state.gameMode === 2
            ? (at < 2 ? "AMBUSH" : at === 2 ? "FLANK" : "GUARD")
            : (at % 3 === 1 ? "FLANK" : at % 3 === 2 ? "GUARD" : "HUNTER"),
          state.gameMode === 3 ? (at + state.wave) % 3 : (at + 1) % 3);
      }
    }
    cameraYaw = playerTank().yaw;
    cameraSine = playerTank().yawSine;
    cameraCosine = playerTank().yawCosine;
    state.transition = 0;
    const setupFinished = qualificationLongSoak ? performance.now() : 0;
    if (!geometryPrepared) buildArena();
    const buildFinished = qualificationLongSoak ? performance.now() : 0;
    /* Retain transition chrome in the WebGL overlay. */
    hudToast = `ARENA ${index + 1}`;
    hudMeshDirty = true;
    toastUntil = state.time + 1.2;
    if (qualificationLongSoak) {
      const finished = performance.now();
      const transitionSummary = ["TREADLINE-ARENA-TRANSITION",
        `arena=${index + 1}`,
        `total=${(finished - transitionStarted).toFixed(3)}ms`,
        `reset=${(resetFinished - transitionStarted).toFixed(3)}ms`,
        `setup=${(setupFinished - resetFinished).toFixed(3)}ms`,
        `build=${(buildFinished - setupFinished).toFixed(3)}ms`,
        `geometry=${arenaBuildGeometryMs.toFixed(3)}ms`,
        `upload=${arenaBuildUploadMs.toFixed(3)}ms`,
        `hud=${(finished - buildFinished).toFixed(3)}ms`,
        `late=${qualificationLateActionMax.toFixed(3)}ms`,
        `late-update=${qualificationLateActionUpdate.toFixed(3)}ms`,
        `late-build=${qualificationLateActionBuild.toFixed(3)}ms`,
        `late-hud=${qualificationLateActionHud.toFixed(3)}ms`,
        `late-commands=${qualificationLateActionCommands.toFixed(3)}ms`,
        `repeatcap=${repeatedFrameCaptures}`].join(" ");
      console.log(transitionSummary);
      /* Keep the phase profiler's terminal sample authoritative. Arena
         transitions still expose their timing in ordinary/soak modes, but a
         later transition must not erase the per-phase profile requested by
         the validation URL before the device log is harvested. */
      if (!qualificationProfile) globalThis.pocSummary = transitionSummary;
    }
  }

  function resetGame() {
    state.score = 0; state.lives = 3; state.kills = 0;
    state.wave = 1; state.multiplier = 1;
    state.killBeat = 0; state.pendingClear = false;
    state.heartbeatAt = 0;
    state.botTelegraphs = state.botPlayerShots = 0;
    state.botTelegraphViolations = 0; state.botVolleyMinimum = 99;
    lastBotPlayerShotAt = -99;
    state.shake = 0; state.flash = 0;
    state.shots = state.hits = state.damageTaken = state.objectiveTicks = 0;
    state.hitConfirm = state.damageIndicator = 0;
    state.commandMeter = 0; state.armorZone = "FRONT";
    randomState = 0x7a2d31c5;
    let firstArena = state.gameMode === 1 ? 1 : state.gameMode === 2 ? 2 : 0;
    if (state.gameMode === 3 && !online.active) {
      const seed = requestedArenaSeed || nextArenaSeed;
      if (!requestedArenaSeed) {
        nextArenaSeed ^= nextArenaSeed << 13;
        nextArenaSeed ^= nextArenaSeed >>> 17;
        nextArenaSeed ^= nextArenaSeed << 5;
        nextArenaSeed >>>= 0;
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
    if (state.gameMode === 3) sounds.waveStart();
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
      arenaGenerationAttempts = generated.attempts;
      arenaGenerationFallback = generated.fallback;
      arenaGenerationChecksum = generated.checksum;
      state.arenaSeed = generated.winningSeed;
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
    if (state.mode === "paused") setMode("playing");
    else if (resetGame()) setMode("playing");
  }
  ui.play.addEventListener("click", startOrResume);

  const input = {
    keys: Object.create(null), pointerAimX: 0, pointerAimY: -1,
    pointerActive: false, source: "keyboard", gamepadConnected: false,
    aimAwayAge: 0, aimTowardAge: 0, aimLeftAge: 0, aimRightAge: 0,
    assistLockTarget: -1, assistLockBroken: false,
    lastFire: false, lastSecondary: false, lastGadget: false,
    lastUltimate: false, lastPause: false,
    fireQueued: false, secondaryQueued: false, gadgetQueued: false,
    ultimateQueued: false, pauseQueued: false,
  };

  function connectedGamepad() {
    if (typeof navigator.getGamepads !== "function") return null;
    const pads = navigator.getGamepads();
    if (!pads) return null;
    for (let at = 0; at < pads.length; at++)
      if (pads[at] && pads[at].connected) return pads[at];
    return null;
  }

  function updateControlHint() {
    const pad = connectedGamepad();
    input.gamepadConnected = !!pad;
    if (!ui.controls) return;
    if (preferences.controls === CONTROL_ARCADE) {
      if (pad || navigator.platform === "PSP") ui.controls.textContent =
        "Arcade: nub moves | face buttons aim | R fire | L secondary | Up gadget | Down Command";
      else ui.controls.textContent =
        "Arcade: WASD/arrows move | IJKL/numpad aim | Space fire | Shift secondary";
    } else if (pad || navigator.platform === "PSP") ui.controls.textContent =
      "Classic: L/R drive | nub aims | X fire | Square secondary | Up gadget | Triangle Command";
    else ui.controls.textContent =
      "Classic: WASD treads | arrows/IJKL aim | Space fire | E secondary | F gadget | Q Command";
  }

  function resetAimAssist() {
    input.aimAwayAge = input.aimTowardAge = 0;
    input.aimLeftAge = input.aimRightAge = 0;
    input.assistLockTarget = -1;
    input.assistLockBroken = false;
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

  function applyArcadeMovement(command, moveX, moveY) {
    const magnitudeSquared = moveX * moveX + moveY * moveY;
    if (!(magnitudeSquared > ARCADE_DEAD_ZONE * ARCADE_DEAD_ZONE))
      return false;
    const magnitude = Math.sqrt(magnitudeSquared);
    moveX /= magnitude; moveY /= magnitude;
    const desiredX = cameraCosine * moveX + cameraSine * -moveY;
    const desiredZ = -cameraSine * moveX + cameraCosine * -moveY;
    const tank = playerTank();
    /* The old mapping converted both vectors back to angles with atan2 on
       every nub frame. Allegrex implements that in software. Dot/cross keep
       the same 120-degree reverse decision and provide a smooth normalized
       steering approximation without a transcendental call. */
    let turn = desiredX * tank.yawCosine - desiredZ * tank.yawSine;
    let alignment = desiredX * tank.yawSine + desiredZ * tank.yawCosine;
    command.reverse = alignment < -.5;
    if (command.reverse) {
      turn = -turn;
      alignment = -alignment;
    }
    let steering = Math.max(-1, Math.min(1,
      turn / Math.max(.5, 1 + alignment * .5)));
    /* moveTank applies the reverse sign before its tread differential. Mirror
       the steering command so the rear of a reversing tank still converges
       toward the requested travel heading. */
    if (command.reverse) steering = -steering;
    if (steering >= 0) {
      command.left = 1 - steering;
      command.right = 1;
    } else {
      command.left = 1;
      command.right = 1 + steering;
    }
    return true;
  }

  function aimEnemyInCone(player, aimX, aimZ) {
    let chosen = -1, nearest = Infinity;
    for (let at = 0; at < MAX_TANKS; at++) {
      const enemy = tanks[at];
      if (!enemy.active || enemy.id === player.id || enemy.team === player.team)
        continue;
      const dx = enemy.x - player.x, dz = enemy.z - player.z;
      const distanceSquared = dx * dx + dz * dz;
      if (!(distanceSquared > .0001)) continue;
      const alignment = aimX * dx + aimZ * dz;
      if (!(alignment > 0)
          || alignment * alignment
              < distanceSquared * ASSIST_CONE_COSINE_SQUARED)
        continue;
      if (distanceSquared < nearest) {
        nearest = distanceSquared;
        chosen = at;
      }
    }
    return chosen;
  }

  function applyAimAssist(command, active) {
    if (!active) {
      input.assistLockTarget = -1;
      input.assistLockBroken = false;
      return;
    }
    let length = Math.sqrt(command.aimX * command.aimX
      + command.aimZ * command.aimZ);
    if (!(length > .0001)) return;
    command.aimX /= length; command.aimZ /= length;
    if (preferences.assist === ASSIST_OFF) return;
    const player = playerTank();
    let targetIndex = -1;
    if (preferences.assist === ASSIST_LOCK) {
      if (input.assistLockTarget >= 0) {
        const locked = tanks[input.assistLockTarget];
        if (!locked || !locked.active || locked.team === player.team
            || lineCrossesSmoke(player.x, player.z, locked.x, locked.z)) {
          input.assistLockTarget = -1;
          input.assistLockBroken = true;
        }
      }
      if (input.assistLockTarget < 0 && !input.assistLockBroken)
        input.assistLockTarget = aimEnemyInCone(
          player, command.aimX, command.aimZ);
      if (input.assistLockTarget >= 0) {
        const selected = tanks[input.assistLockTarget];
        if (lineCrossesSmoke(player.x, player.z, selected.x, selected.z)) {
          input.assistLockTarget = -1;
          input.assistLockBroken = true;
        }
      }
      targetIndex = input.assistLockTarget;
    } else targetIndex = aimEnemyInCone(player, command.aimX, command.aimZ);
    if (targetIndex < 0) return;
    const target = tanks[targetIndex];
    const targetX = target.x - player.x, targetZ = target.z - player.z;
    const inverseDistance = 1 / Math.sqrt(targetX * targetX + targetZ * targetZ);
    const normalizedX = targetX * inverseDistance;
    const normalizedZ = targetZ * inverseDistance;
    if (preferences.assist === ASSIST_LOCK) {
      command.aimX = normalizedX; command.aimZ = normalizedZ;
      return;
    }
    command.aimX = command.aimX * (1 - ASSIST_SNAP_BLEND)
      + normalizedX * ASSIST_SNAP_BLEND;
    command.aimZ = command.aimZ * (1 - ASSIST_SNAP_BLEND)
      + normalizedZ * ASSIST_SNAP_BLEND;
    length = Math.sqrt(command.aimX * command.aimX
      + command.aimZ * command.aimZ) || 1;
    command.aimX /= length; command.aimZ /= length;
  }

  function applyArcadeFaceAim(command, pad) {
    const away = !!pad.buttons[3]?.pressed;
    const toward = !!pad.buttons[0]?.pressed;
    const left = !!pad.buttons[2]?.pressed;
    const right = !!pad.buttons[1]?.pressed;
    const held = away || toward || left || right;
    if (!held) {
      input.aimAwayAge = input.aimTowardAge = 0;
      input.aimLeftAge = input.aimRightAge = 0;
      return false;
    }
    input.aimAwayAge = away ? ARCADE_AIM_ROLL_FRAMES
      : Math.max(0, input.aimAwayAge - 1);
    input.aimTowardAge = toward ? ARCADE_AIM_ROLL_FRAMES
      : Math.max(0, input.aimTowardAge - 1);
    input.aimLeftAge = left ? ARCADE_AIM_ROLL_FRAMES
      : Math.max(0, input.aimLeftAge - 1);
    input.aimRightAge = right ? ARCADE_AIM_ROLL_FRAMES
      : Math.max(0, input.aimRightAge - 1);
    let aimX = (input.aimRightAge ? 1 : 0) - (input.aimLeftAge ? 1 : 0);
    let aimY = (input.aimTowardAge ? 1 : 0) - (input.aimAwayAge ? 1 : 0);
    if (!aimX && !aimY) return false;
    if (aimX && aimY) {
      aimX *= INVERSE_SQRT_TWO;
      aimY *= INVERSE_SQRT_TWO;
    }
    applyScreenAim(command, aimX, aimY);
    return true;
  }

  function pollPlayerInput() {
    const command = playerTank().command;
    command.left = command.right = 0;
    command.reverse = false; command.fire = command.secondary = false;
    command.gadget = command.ultimate = false;
    const pad = connectedGamepad();
    if (!!pad !== input.gamepadConnected) updateControlHint();
    let padActive = false, fire = false, secondary = false;
    let gadget = false, ultimate = false, pause = false;
    let aimActive = false;
    if (pad) {
      const buttons = pad.buttons;
      const nubX = Number(pad.axes[0]) || 0;
      const nubY = Number(pad.axes[1]) || 0;
      const leftShoulder = !!buttons[4]?.pressed;
      const rightShoulder = !!buttons[5]?.pressed;
      gadget = !!buttons[12]?.pressed;
      pause = !!buttons[9]?.pressed;
      if (preferences.controls === CONTROL_ARCADE) {
        fire = rightShoulder;
        secondary = leftShoulder;
        ultimate = !!buttons[13]?.pressed;
      } else {
        fire = !!buttons[0]?.pressed;
        secondary = !!buttons[2]?.pressed;
        ultimate = !!buttons[3]?.pressed;
      }
      const anyFace = !!buttons[0]?.pressed || !!buttons[1]?.pressed
        || !!buttons[2]?.pressed || !!buttons[3]?.pressed;
      padActive = leftShoulder || rightShoulder || anyFace || gadget
        || ultimate || pause || Math.abs(nubX) > .16 || Math.abs(nubY) > .16;
      if (padActive) {
        setSource("gamepad");
        if (preferences.controls === CONTROL_ARCADE) {
          applyArcadeMovement(command, nubX, nubY);
          aimActive = applyArcadeFaceAim(command, pad);
        } else {
          command.left = leftShoulder ? 1 : 0;
          command.right = rightShoulder ? 1 : 0;
          command.reverse = !!pad.buttons[1]?.pressed;
          if (Math.sqrt(nubX * nubX + nubY * nubY) > ARCADE_DEAD_ZONE) {
            applyScreenAim(command, nubX, nubY);
            aimActive = true;
          }
        }
      }
    }
    if (!pad && input.source === "gamepad") setSource("keyboard");
    if (input.source !== "gamepad") {
      const arcade = preferences.controls === CONTROL_ARCADE;
      const moveX = (keyboardDown("d") || keyboardDown("moveRight") ? 1 : 0)
        - (keyboardDown("a") || keyboardDown("moveLeft") ? 1 : 0);
      const moveY = (keyboardDown("s") || keyboardDown("moveDown") ? 1 : 0)
        - (keyboardDown("w") || keyboardDown("moveUp") ? 1 : 0);
      if (arcade) applyArcadeMovement(command, moveX, moveY);
      else {
        const straightForward = keyboardDown("w");
        const straightReverse = keyboardDown("s");
        command.left = keyboardDown("a") || straightForward || straightReverse ? 1 : 0;
        command.right = keyboardDown("d") || straightForward || straightReverse ? 1 : 0;
        command.reverse = straightReverse || keyboardDown("shift");
      }
      fire = keyboardDown("fire");
      secondary = keyboardDown("secondary") || (arcade && keyboardDown("shift"));
      gadget = keyboardDown("gadget");
      ultimate = keyboardDown("ultimate");
      pause = keyboardDown("pause");
      let aimX = (keyboardDown("aimRight") ? 1 : 0)
        - (keyboardDown("aimLeft") ? 1 : 0);
      let aimY = (keyboardDown("aimDown") ? 1 : 0)
        - (keyboardDown("aimUp") ? 1 : 0);
      if (!aimX && !aimY && input.pointerActive) {
        aimX = input.pointerAimX; aimY = input.pointerAimY;
      }
      if (aimX || aimY) {
        if (aimX && aimY) {
          aimX *= INVERSE_SQRT_TWO;
          aimY *= INVERSE_SQRT_TWO;
        }
        applyScreenAim(command, aimX, aimY);
        aimActive = true;
      }
    }
    applyAimAssist(command, aimActive);
    command.fire = input.fireQueued || fire;
    command.secondary = input.secondaryQueued
      || (secondary && !input.lastSecondary);
    command.gadget = input.gadgetQueued || (gadget && !input.lastGadget);
    command.ultimate = input.ultimateQueued || (ultimate && !input.lastUltimate);
    if (input.pauseQueued || (pause && !input.lastPause)) {
      if (state.mode === "playing") setMode("paused");
      else if (state.mode === "paused") setMode("playing");
    }
    input.lastFire = fire; input.lastSecondary = secondary;
    input.lastGadget = gadget; input.lastUltimate = ultimate;
    input.lastPause = pause;
    input.fireQueued = input.secondaryQueued = input.gadgetQueued = false;
    input.ultimateQueued = input.pauseQueued = false;
  }

  function setLongSoakMovement(command, frameNumber) {
    const phase = frameNumber % 360;
    /* Exercise translation, turning, obstacle contact, and backing away in a
       repeatable 360-frame path. Qualification remains hands-free, but its
       frame tails now include the same moving-camera and collision work as a
       person driving around the arena. */
    if (phase < 90 || (phase >= 150 && phase < 240)) {
      command.left = command.right = 1;
      command.reverse = false;
    } else if (phase < 150) {
      command.left = .28;
      command.right = 1;
      command.reverse = false;
    } else if (phase < 300) {
      command.left = 1;
      command.right = .28;
      command.reverse = false;
    } else {
      command.left = command.right = 1;
      command.reverse = true;
    }
  }

  function applyLongSoakMovement(frameNumber, dt) {
    if (!qualificationLongSoak || state.mode !== "playing") return;
    /* Exercise both authored camera paths for thirty seconds each in the
       device soak. This changes only the in-memory qualification preference;
       it never writes the user's saved camera choice. */
    const qualificationCamera = ((frameNumber / 900) | 0) & 1;
    if (preferences.camera !== qualificationCamera) {
      preferences.camera = qualificationCamera;
      qualificationCameraSwitches++;
      if (!qualificationCamera) {
        cameraYaw = 0; cameraSine = 0; cameraCosine = 1;
      }
    }
    const player = playerTank();
    /* Drive the qualification player with the same bounded combat planner
       used by arena opponents. A blind authored loop repeatedly parked on
       scenery and exercised neither aiming nor ordinary projectile kills. */
    updateBotCommand(player, dt);
    const target = tanks[player.target];
    if (!target || !target.active || target.team === player.team) {
      setLongSoakMovement(player.command, frameNumber);
      return;
    }
    const dx = target.x - player.x, dz = target.z - player.z;
    const distanceSquared = dx * dx + dz * dz;
    const aimAngle = Math.atan2(dx, dz);
    const aligned = Math.abs(wrapAngle(aimAngle - player.turret)) < .18;
    /* Qualification attacks through the real bounded projectile path. */
    player.command.fire = aligned && distanceSquared < 67.24
      && !pointInSmoke(player.x, player.z)
      && !pointInSmoke(target.x, target.z);
    if (aligned && player.secondaryCooldown <= 0
        && (frameNumber % 300) === 120)
      player.command.secondary = true;
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

  function sendOnlineInput() {
    const command = playerTank().command;
    const buffer = new ArrayBuffer(ONLINE_INPUT_BYTES);
    const view = new DataView(buffer);
    view.setUint8(0, 1);
    view.setUint8(1, ONLINE_PROTOCOL_VERSION);
    online.inputSequence = (online.inputSequence + 1) & 0xffff;
    view.setUint16(2, online.inputSequence, true);
    let flags = command.left ? 1 : 0;
    if (command.right) flags |= 2;
    if (command.reverse) flags |= 4;
    if (command.fire) flags |= 8;
    if (command.secondary) flags |= 16;
    if (command.gadget) flags |= 32;
    if (command.ultimate) flags |= 64;
    view.setUint8(4, flags);
    view.setInt16(6, encodeSignedUnit(command.aimX), true);
    view.setInt16(8, encodeSignedUnit(command.aimZ), true);
    sendOnlinePacket(buffer);
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
    const flags = view.getUint8(4), command = tanks[1].command;
    command.left = flags & 1 ? 1 : 0;
    command.right = flags & 2 ? 1 : 0;
    command.reverse = !!(flags & 4);
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
    const length = ONLINE_SNAPSHOT_HEADER_BYTES
      + MAX_TANKS * ONLINE_TANK_BYTES + bulletCount * ONLINE_BULLET_BYTES;
    const buffer = new ArrayBuffer(length), view = new DataView(buffer);
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
    sendOnlinePacket(buffer);
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
        tank.turret = approachAngle(tank.turret, nextTurret, .75);
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
      bullet.headingSine = bullet.vx / headingLength;
      bullet.headingCosine = bullet.vz / headingLength;
      bullet.life = view.getUint16(offset + 10, true) / 1024;
      offset += ONLINE_BULLET_BYTES;
    }
    const mode = view.getUint8(5);
    const nextMode = mode === 1 ? "arena-clear"
      : mode === 2 ? "game-over" : mode === 3 ? "victory" : "playing";
    if (state.mode !== nextMode) setMode(nextMode);
    updateHud(true);
    return true;
  }

  function onlineTick() {
    if (!online.active || !online.channel
        || online.channel.readyState !== "open") return;
    if (state.time - online.lastReceive > ONLINE_PEER_TIMEOUT) {
      const channel = online.channel;
      online.disconnectReason = "No game packets received for 15 seconds.";
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
    if (key === "ArrowLeft") return preferences.controls === CONTROL_ARCADE
      ? "moveLeft" : "aimLeft";
    if (key === "ArrowRight") return preferences.controls === CONTROL_ARCADE
      ? "moveRight" : "aimRight";
    if (key === "ArrowUp") return preferences.controls === CONTROL_ARCADE
      ? "moveUp" : "aimUp";
    if (key === "ArrowDown") return preferences.controls === CONTROL_ARCADE
      ? "moveDown" : "aimDown";
    if (lower === "j" || code === "KeyJ" || code === "Numpad4") return "aimLeft";
    if (lower === "l" || code === "KeyL" || code === "Numpad6") return "aimRight";
    if (lower === "i" || code === "KeyI" || code === "Numpad8") return "aimUp";
    if (lower === "k" || code === "KeyK" || code === "Numpad2") return "aimDown";
    if (key === " " || key === "Spacebar" || code === "Space") return "fire";
    if (lower === "e" || code === "KeyE") return "secondary";
    if (lower === "f" || code === "KeyF") return "gadget";
    if (lower === "q" || code === "KeyQ") return "ultimate";
    if (lower === "p" || key === "Escape" || code === "KeyP") return "pause";
    return "";
  }

  addEventListener("keydown", (event) => {
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
  addEventListener("blur", () => {
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
    input.fireQueued = true;
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden && state.mode === "playing") state.mode = "hidden";
    else if (!document.hidden && state.mode === "hidden") {
      state.mode = "playing";
      lastTimestamp = 0;
    }
  });

  function circleHitsObstacle(x, z, radius) {
    const radiusSquared = radius * radius;
    const cellX = Math.floor((x + 8) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((z + 8) * COLLISION_GRID_SCALE);
    if (cellX < 0 || cellX >= COLLISION_GRID_SIDE
        || cellZ < 0 || cellZ >= COLLISION_GRID_SIDE) return false;
    const arena = state.arena;
    const cell = cellZ * COLLISION_GRID_SIDE + cellX;
    const obstacles = ARENA_OBSTACLE_BOUNDS[arena];
    let mask = ARENA_CIRCLE_OBSTACLE_GRIDS[arena][cell];
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
    for (let barrierAt = 0; barrierAt < barriers.length; barrierAt++) {
      const barrier = barriers[barrierAt];
      if (!barrier.active) continue;
      const dx = x < barrier.left ? x - barrier.left
        : x > barrier.right ? x - barrier.right : 0;
      const dz = z < barrier.top ? z - barrier.top
        : z > barrier.bottom ? z - barrier.bottom : 0;
      if (dx * dx + dz * dz < radiusSquared) return true;
    }
    if (!state.gateOpen) {
      const gates = ARENA_GATE_BOUNDS[arena];
      mask = ARENA_CIRCLE_GATE_GRIDS[arena][cell];
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
    const cellX = Math.floor((x + 8) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((z + 8) * COLLISION_GRID_SCALE);
    if (cellX < 0 || cellX >= COLLISION_GRID_SIDE
        || cellZ < 0 || cellZ >= COLLISION_GRID_SIDE) return false;
    const arena = state.arena;
    const cell = cellZ * COLLISION_GRID_SIDE + cellX;
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
    const cellX = Math.floor((x + 8) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((z + 8) * COLLISION_GRID_SCALE);
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
    const cellX = Math.floor((tank.x + 8) * COLLISION_GRID_SCALE);
    const cellZ = Math.floor((tank.z + 8) * COLLISION_GRID_SCALE);
    if (cellX < 0 || cellX >= COLLISION_GRID_SIDE
        || cellZ < 0 || cellZ >= COLLISION_GRID_SIDE) return false;
    const rampIndex = ARENA_RAMP_GRIDS[state.arena][
      cellZ * COLLISION_GRID_SIDE + cellX] - 1;
    if (rampIndex < 0) return false;
    const ramp = ARENAS[state.arena].ramps[rampIndex];
    return tank.surfaceY >= ramp[4] * .6;
  }

  function pointInSmoke(x, z) {
    for (let smokeAt = 0; smokeAt < smokeClouds.length; smokeAt++) {
      const smoke = smokeClouds[smokeAt];
      if (!smoke.active) continue;
      const dx = x - smoke.x, dz = z - smoke.z;
      if (dx * dx + dz * dz < 3.1) return true;
    }
    return false;
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

  function objectivePosition(out) {
    if (state.gameMode === 1) { out.x = 0; out.z = 0; return; }
    if (state.gameMode === 2 && convoy.active) {
      out.x = convoy.x; out.z = convoy.z; return;
    }
    const player = playerTank();
    let best = Infinity; out.x = 0; out.z = 0;
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (!tank.active || tank.team === player.team) continue;
      const dx = tank.x - player.x, dz = tank.z - player.z;
      const distance = dx * dx + dz * dz;
      if (distance < best) { best = distance; out.x = tank.x; out.z = tank.z; }
    }
    if (best === Infinity) {
      const gate = ARENAS[state.arena].gates[0];
      out.x = gate[0]; out.z = gate[1];
    }
  }

  const objectiveTarget = {x: 0, z: 0};

  function updateAimProbe(tank) {
    let x = tank.x + tank.turretSine * .8;
    let z = tank.z + tank.turretCosine * .8;
    const dx = tank.turretSine * .28;
    const dz = tank.turretCosine * .28;
    aimProbe.hit = false;
    for (let step = 0; step < 30; step++) {
      const nextX = x + dx, nextZ = z + dz;
      const boundaryX = Math.abs(nextX) > 7.85;
      const boundaryZ = Math.abs(nextZ) > 7.85;
      if (boundaryX || boundaryZ || circleHitsObstacle(nextX, nextZ, .08)) {
        aimProbe.x = x; aimProbe.z = z; aimProbe.hit = true;
        const openX = !circleHitsObstacle(nextX, z, .08) && !boundaryX;
        const openZ = !circleHitsObstacle(x, nextZ, .08) && !boundaryZ;
        aimProbe.bounceX = openX ? dx : -dx;
        aimProbe.bounceZ = openZ ? dz : -dz;
        return;
      }
      x = nextX; z = nextZ;
    }
    aimProbe.x = x; aimProbe.z = z;
    aimProbe.bounceX = dx; aimProbe.bounceZ = dz;
  }

  function updateOverlay() {
    const player = playerTank();
    objectivePosition(objectiveTarget);
    const objectiveAngle = Math.atan2(objectiveTarget.x - player.x,
      objectiveTarget.z - player.z) - cameraYaw;
    const objectiveStep = Math.round(wrapAngle(objectiveAngle) * 16 / Math.PI);
    const damageStep = Math.round(
      wrapAngle(state.damageAngle - cameraYaw) * 16 / Math.PI);
    const cooldown = Math.max(0, player.gadgetCooldown);
    const gadgetStep = Math.round(43 * (1 - Math.min(1,
      cooldown / Math.max(1, player.gadgetCooldownMax))));
    const commandStep = Math.round(43 * state.commandMeter / 100);
    const multiplierStep = state.gameMode === 3
      ? Math.max(1, Math.min(5, state.multiplier | 0)) : 0;
    const hitVisible = state.hitConfirm > 0;
    const damageVisible = state.damageIndicator > 0;
    const commandEnabled = preferences.command;
    if (objectiveStep === hudVisualObjectiveStep
        && damageStep === hudVisualDamageStep
        && hitVisible === hudVisualHit
        && damageVisible === hudVisualDamage
        && gadgetStep === hudVisualGadgetStep
        && commandStep === hudVisualCommandStep
        && multiplierStep === hudVisualMultiplierStep
        && commandEnabled === hudVisualCommandEnabled) return;
    hudVisualObjectiveStep = objectiveStep;
    hudVisualDamageStep = damageStep;
    hudVisualHit = hitVisible;
    hudVisualDamage = damageVisible;
    hudVisualGadgetStep = gadgetStep;
    hudVisualCommandStep = commandStep;
    hudVisualMultiplierStep = multiplierStep;
    hudVisualCommandEnabled = commandEnabled;
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
    const sine = directionSines[direction];
    const cosine = directionCosines[direction];
    setBulletActive(bulletAt, true); bullet.owner = tank.id;
    bullet.x = tank.x + sine * .78;
    bullet.z = tank.z + cosine * .78;
    bullet.vx = sine * speed;
    bullet.vz = cosine * speed;
    bullet.headingSine = sine;
    bullet.headingCosine = cosine;
    bullet.heading = angle;
    bullet.life = life; bullet.bounces = bounces;
    bullet.damage = damage; bullet.barrierDamage = barrierDamage;
    bullet.pierce = pierce; bullet.bypassShield = bypassShield;
    bullet.overCover = elevatedFiringOrigin(tank);
    return true;
  }

  function deferCrowdedBotVolley(tank, secondary) {
    if (tank.player || tank.target !== playerTank().id
        || state.time - lastBotPlayerShotAt >= .25) return false;
    tank.fireWindup = Math.max(.02,
      .25 - (state.time - lastBotPlayerShotAt) + (tank.id % 3) * .015);
    tank.fireSecondaryArmed = !!secondary;
    tank.fireTelegraphed = true;
    return true;
  }

  function fireTank(tank, spread = 0) {
    if (tank.cooldown > 0 || !tank.active) return false;
    if (deferCrowdedBotVolley(tank, false)) return false;
    const profile = CLASSES[tank.classId];
    const strikerCommand = tank.classId === 1 && tank.commandBuff > 0;
    if (!spawnShell(tank, spread, strikerCommand ? 8.4 : 7.2, 3,
        strikerCommand ? 34 : 28, strikerCommand ? 2 : 1,
        strikerCommand ? 2 : 1, strikerCommand ? 1 : 0, strikerCommand))
      return false;
    tank.cooldown = profile.reload * (tank.classId === 0 && tank.commandBuff > 0
      ? .55 : 1);
    tank.recoil = .11;
    if (tank.player) state.shots++;
    else if (tank.target === playerTank().id) {
      state.botPlayerShots++;
      if (!tank.fireTelegraphed) state.botTelegraphViolations++;
      const gap = state.time - lastBotPlayerShotAt;
      if (lastBotPlayerShotAt > -90)
        state.botVolleyMinimum = Math.min(state.botVolleyMinimum, gap);
      lastBotPlayerShotAt = state.time;
      tank.fireTelegraphed = false;
    }
    spawnParticles(tank.x + tank.turretSine * .78, .42,
      tank.z + tank.turretCosine * .78, 3, .8);
    sounds.shot(tank.player);
    return true;
  }

  function fireSecondary(tank) {
    if (!tank.active || tank.secondaryCooldown > 0) return false;
    if (deferCrowdedBotVolley(tank, true)) return false;
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
    if (!admitted) {
      if (tank.player) showToast("CANNON BUSY", .65);
      return false;
    }
    tank.secondaryCooldown = tank.classId === 0 ? 3.8 : tank.classId === 1 ? 5.5 : 4.8;
    tank.recoil = .14;
    if (tank.player) state.shots += admitted;
    else if (tank.target === playerTank().id) {
      state.botPlayerShots++;
      if (!tank.fireTelegraphed) state.botTelegraphViolations++;
      const gap = state.time - lastBotPlayerShotAt;
      if (lastBotPlayerShotAt > -90)
        state.botVolleyMinimum = Math.min(state.botVolleyMinimum, gap);
      lastBotPlayerShotAt = state.time;
      tank.fireTelegraphed = false;
    }
    spawnParticles(tank.x + tank.turretSine * .85, .42,
      tank.z + tank.turretCosine * .85, 5, 1.15);
    if (tank.classId === 2) sounds.explosion(tank.player ? .13 : .055);
    else sounds.play(245, .09, tank.player ? .14 : .06, 190);
    return true;
  }

  function awardCommand(amount) {
    if (!preferences.command || !(amount > 0)) return;
    state.commandMeter = Math.min(100, state.commandMeter + amount);
  }

  function activateCommand(tank) {
    if (!tank.player || !preferences.command || state.commandMeter < 100)
      return false;
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
    return true;
  }

  function useGadget(tank) {
    if (!tank.gadget || tank.gadgetCooldown > 0) return;
    const type = tank.gadget;
    let admitted = true;
    if (type === "MINES") {
      let mine = null;
      for (let at = 0; at < MAX_MINES; at++) {
        const candidate = mines[at];
        if (!candidate.active) { mine = candidate; break; }
      }
      if (mine) {
        mine.active = true; mine.team = tank.team;
        mine.x = tank.x - tank.yawSine * .72;
        mine.z = tank.z - tank.yawCosine * .72;
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
      return;
    }
    tank.gadgetCooldown = type === "MINES" ? 5 : type === "SMOKE" ? 8 : 10;
    tank.gadgetCooldownMax = tank.gadgetCooldown;
    if (tank.player) showToast(type, 1);
    sounds.gadget();
  }

  function botDifficultyValue(values) {
    if (state.gameMode !== 3) return values[state.difficultyChoice];
    const level = Math.min(2,
      state.difficultyChoice + Math.max(0, state.wave - 1) * .16);
    const lower = Math.floor(level), upper = Math.min(2, lower + 1);
    const blend = level - lower;
    return values[lower] + (values[upper] - values[lower]) * blend;
  }

  function beginBotFireWindup(tank, secondary) {
    if (tank.fireWindup > 0) return false;
    tank.fireWindup = botDifficultyValue(BOT_FIRE_WINDUP)
      + (tank.id % 3) * .025;
    tank.fireSecondaryArmed = !!secondary;
    tank.fireTelegraphed = false;
    state.botTelegraphs++;
    sounds.telegraph();
    return true;
  }

  function updateBotCommand(tank, dt) {
    const command = tank.command;
    let targetX = tanks[0].x, targetZ = tanks[0].z, targetId = 0;
    let targetDistance = Infinity;
    for (let at = 0; at < MAX_TANKS; at++) {
      const candidate = tanks[at];
      if (!candidate.active || candidate.team === tank.team) continue;
      const dx = candidate.x - tank.x, dz = candidate.z - tank.z;
      const distance = dx * dx + dz * dz;
      if (distance < targetDistance) {
        targetDistance = distance; targetX = candidate.x; targetZ = candidate.z;
        targetId = candidate.id;
      }
    }
    tank.target = targetId;

    let goalX = targetX, goalZ = targetZ;
    if (tank.health < 32) {
      let bestPickup = null, bestDistance = Infinity;
      for (let at = 0; at < MAX_PICKUPS; at++) {
        const pickup = pickups[at];
        if (!pickup.active || pickup.type !== "ARMOR") continue;
        const px = pickup.x - tank.x, pz = pickup.z - tank.z;
        const distance = px * px + pz * pz;
        if (distance < bestDistance) {
          bestDistance = distance; bestPickup = pickup;
        }
      }
      if (bestPickup) { goalX = bestPickup.x; goalZ = bestPickup.z; }
      else {
        goalX = tank.team === 0 ? -5.8 : 5.8;
        goalZ = tank.team === 0 ? -5.8 : 5.8;
      }
    } else if (state.gameMode === 1) {
      if (tank.role === "CAPTURE"
          || (tank.role === "GUARD" && (tank.team === 0
            ? state.blueControl >= state.redControl : state.redControl >= state.blueControl))) {
        const angle = tank.id * 2.1;
        goalX = Math.sin(angle) * .7; goalZ = Math.cos(angle) * .7;
      } else if (tank.role === "FLANK") {
        const side = tank.id & 1 ? 1 : -1;
        goalX = targetX + (targetZ - tank.z) * .34 * side;
        goalZ = targetZ - (targetX - tank.x) * .34 * side;
      }
    } else if (state.gameMode === 2 && tank.team === 1 && convoy.active) {
      const dx = convoy.x - tank.x, dz = convoy.z - tank.z;
      const distance = dx * dx + dz * dz;
      if (tank.role === "AMBUSH") {
        goalX = convoy.x + (tank.id & 1 ? 2.1 : -2.1);
        goalZ = Math.min(6.5, convoy.z + 2.2);
      } else if (tank.role === "GUARD") {
        goalX = convoy.x; goalZ = Math.min(6.3, convoy.z + 1.4);
      } else {
        const side = tank.id & 1 ? 1 : -1;
        goalX = tanks[0].x + (tanks[0].z - tank.z) * .3 * side;
        goalZ = tanks[0].z - (tanks[0].x - tank.x) * .3 * side;
      }
      if (distance < targetDistance * 1.5 || tank.role !== "FLANK") {
        targetX = convoy.x; targetZ = convoy.z; targetDistance = distance;
      }
    } else if (tank.role === "FLANK") {
      const side = tank.id & 1 ? 1 : -1;
      goalX = targetX + (targetZ - tank.z) * .38 * side;
      goalZ = targetZ - (targetX - tank.x) * .38 * side;
    } else if (tank.role === "GUARD" && state.gateOpen) {
      const gate = ARENAS[state.arena].gates[0];
      goalX = gate[0] + (tank.id & 1 ? 1.5 : -1.5);
      goalZ = gate[1] + (tank.team ? 1.3 : -1.3);
    }

    const goalDx = goalX - tank.x, goalDz = goalZ - tank.z;
    const targetDx = targetX - tank.x, targetDz = targetZ - tank.z;
    const goalDistanceSquared = goalDx * goalDx + goalDz * goalDz;
    const targetDistanceSquared = targetDx * targetDx + targetDz * targetDz;
    /* Backing directly away and then driving directly back toward a still
       target produces an unnatural distance oscillation. Once a combatant
       has established clear space, retain that state and travel tangentially
       around the target. A small radial correction holds the useful firing
       distance; pursuit resumes only after a substantially wider gap. */
    if (tank.backingOff) {
      if (targetDistanceSquared >= BOT_BACKOFF_EXIT_SQUARED) {
        tank.backingOff = false;
        tank.standoff = true;
      }
    } else if (targetDistanceSquared < BOT_BACKOFF_ENTER_SQUARED) {
      tank.backingOff = true;
      tank.standoff = true;
    }
    if (tank.standoff
        && targetDistanceSquared > BOT_STANDOFF_RELEASE_SQUARED)
      tank.standoff = false;
    let steeringX = goalDx, steeringZ = goalDz;
    if (tank.backingOff) {
      steeringX = targetDx;
      steeringZ = targetDz;
    } else if (tank.standoff && targetDistanceSquared > .01) {
      const targetDistance = Math.sqrt(targetDistanceSquared);
      const towardX = targetDx / targetDistance;
      const towardZ = targetDz / targetDistance;
      const radial = Math.max(-.62, Math.min(.62,
        (targetDistance - BOT_STANDOFF_RADIUS) * .72));
      steeringX = targetDz / targetDistance * tank.orbitTurn
        + towardX * radial;
      steeringZ = -targetDx / targetDistance * tank.orbitTurn
        + towardZ * radial;
    }
    const desired = Math.atan2(steeringX, steeringZ);
    const turn = wrapAngle(desired - tank.yaw);
    command.left = turn > .22 ? 0 : 1;
    command.right = turn < -.22 ? 0 : 1;
    command.reverse = tank.backingOff;
    if (!tank.backingOff && goalDistanceSquared < 2.25
        && tank.role !== "CAPTURE") {
      command.reverse = true;
      command.left = command.right = 1;
    }
    if (tank.avoidTime > 0) {
      if (tank.avoidTime > 1) {
        command.reverse = true;
        command.left = command.right = 1;
      } else {
        command.reverse = false;
        command.left = tank.avoidTurn > 0 ? 0 : 1;
        command.right = command.left ? 0 : 1;
      }
    }
    if (command.reverse && command.left !== command.right) {
      /* Reversing both tread signs also reverses steering. Swap the authored
         tread command so the hull still converges on its desired heading
         instead of orbiting the object it is trying to escape. */
      const left = command.left;
      command.left = command.right;
      command.right = left;
    }
    const accuracy = botDifficultyValue(BOT_ACCURACY);
    command.aimX = targetDx + Math.sin(state.time * .8 + tank.id * 2.3) * accuracy;
    command.aimZ = targetDz + Math.cos(state.time * .7 + tank.id * 1.7) * accuracy;
    command.fire = command.secondary = command.gadget = command.ultimate = false;
    const fireAngle = Math.atan2(targetDx, targetDz);
    const aimedAtTarget = Math.abs(wrapAngle(fireAngle - tank.turret))
      < .16 + accuracy * .2;
    const playerTarget = targetId === playerTank().id;
    const targetVisible = !lineCrossesSmoke(tank.x, tank.z, targetX, targetZ);
    let completedWindup = false;
    if (tank.fireWindup > 0) {
      if (!playerTarget || !targetVisible) {
        tank.fireWindup = 0;
        tank.fireSecondaryArmed = false;
      } else {
        tank.fireWindup -= dt;
        if (tank.fireWindup <= 0) {
          const gap = state.time - lastBotPlayerShotAt;
          if (aimedAtTarget && targetDistanceSquared < 67.24
              && gap >= .25) {
            command.secondary = tank.fireSecondaryArmed;
            command.fire = !tank.fireSecondaryArmed;
            tank.fireTelegraphed = true;
            completedWindup = true;
          } else if (aimedAtTarget && targetDistanceSquared < 67.24
              && targetVisible && gap < .25) {
            /* Preserve the telegraph while the global volley lane drains;
               the id offset prevents two ready bots from waking together. */
            tank.fireWindup = Math.max(.02,
              .25 - gap + (tank.id % 3) * .015);
          } else {
            tank.fireWindup = 0;
            tank.fireSecondaryArmed = false;
          }
        }
      }
    }
    tank.aiThink -= dt;
    if (tank.aiThink <= 0 && tank.fireWindup <= 0 && !completedWindup) {
      tank.aiThink = botDifficultyValue(BOT_REACTION) + random() * .16;
      const wantsFire = random() < botDifficultyValue(BOT_FIRE_CHANCE)
        && aimedAtTarget
        && targetDistanceSquared < 67.24
        && targetVisible;
      const wantsSecondary = tank.secondaryCooldown <= 0
        && targetDistanceSquared < (tank.classId === 2 ? 64 : 900)
        && Math.abs(wrapAngle(fireAngle - tank.turret)) < .21
        && random() < .24 + state.difficultyChoice * .08;
      if (playerTarget && (wantsFire || wantsSecondary))
        beginBotFireWindup(tank, wantsSecondary);
      else {
        command.fire = wantsFire;
        command.secondary = wantsSecondary;
      }
      const nearTarget = targetDistanceSquared < 5.76;
      command.gadget = tank.gadgetCooldown <= 0 && (
        (tank.gadget === "REPAIR DRONE" && tank.health < 70)
        || (tank.gadget === "SHIELD" && targetDistance < 12)
        || (tank.gadget === "BOOST TREADS" && goalDistanceSquared > 16)
        || (tank.gadget === "SMOKE" && tank.health < 58)
        || (tank.gadget === "MINES" && nearTarget));
    }
  }

  function moveTank(tank, dt) {
    const command = tank.command;
    const sign = command.reverse ? -1 : 1;
    const left = command.left * sign, right = command.right * sign;
    const movement = (left + right) * .5;
    const priorYaw = tank.yaw;
    tank.yaw = wrapAngle(priorYaw + (right - left) * 2.05 * dt);
    if (tank.yaw !== priorYaw) updateTankYawCache(tank);
    const oldX = tank.x, oldZ = tank.z;
    let driveSpeed = tank.driveSpeed;
    if (tank.boost > 0) driveSpeed *= 1.46;
    if (tank.classId === 0 && tank.commandBuff > 0) driveSpeed *= 1.38;
    const intendedTravel = movement * driveSpeed * dt;
    tank.x += tank.yawSine * intendedTravel;
    tank.z += tank.yawCosine * intendedTravel;
    let blocked = false, collidedTank = null, collisionDistanceSquared = 0;
    if (movement !== 0) blocked = Math.abs(tank.x) > 7.65
      || Math.abs(tank.z) > 7.65
      || circleHitsObstacle(tank.x, tank.z, tank.collisionRadius);
    if (movement !== 0 && !blocked) {
      for (let at = 0; at < MAX_TANKS; at++) {
        const other = tanks[at];
        if (!other.active || other.id === tank.id) continue;
        const dx = tank.x - other.x, dz = tank.z - other.z;
        const distanceSquared = dx * dx + dz * dz;
        if (distanceSquared < CLASS_COMBINED_RADIUS_SQUARED[
            tank.classId * CLASSES.length + other.classId]) {
          blocked = true;
          collidedTank = other;
          collisionDistanceSquared = distanceSquared;
          break;
        }
      }
    }
    if (blocked) {
      tank.x = oldX; tank.z = oldZ;
      let separatedFromTank = false;
      /* A tank can already overlap after a spawn, network correction, or
         older save. Merely rolling the attempted move back traps it inside
         the collision forever. Move AI tanks to the nearest separated point
         when that point remains inside the arena and clear of scenery. The
         player is never displaced by an AI correction. */
      if (!tank.player && collidedTank) {
        let dx = oldX - collidedTank.x, dz = oldZ - collidedTank.z;
        let distance = Math.sqrt(dx * dx + dz * dz);
        if (distance < .001) {
          dx = tank.yawSine || (tank.id & 1 ? 1 : -1);
          dz = tank.yawCosine;
          distance = Math.sqrt(dx * dx + dz * dz) || 1;
        }
        const combined = Math.sqrt(CLASS_COMBINED_RADIUS_SQUARED[
          tank.classId * CLASSES.length + collidedTank.classId]) + .06;
        if (collisionDistanceSquared < combined * combined) {
          const separatedX = collidedTank.x + dx / distance * combined;
          const separatedZ = collidedTank.z + dz / distance * combined;
          if (Math.abs(separatedX) <= 7.65 && Math.abs(separatedZ) <= 7.65
              && !circleHitsObstacle(separatedX, separatedZ,
                tank.collisionRadius)) {
            tank.x = separatedX; tank.z = separatedZ;
            separatedFromTank = true;
          }
        }
      }
      tank.blockedTime = Math.min(1.2, tank.blockedTime + dt);
      if (separatedFromTank) {
        tank.backingOff = true;
        if (collidedTank.id === tank.target) tank.standoff = true;
        tank.avoidTime = 0;
        command.reverse = true;
        command.left = command.right = 1;
      } else if ((!tank.player || qualificationLongSoak)
          && tank.avoidTime <= 0) {
        tank.avoidTime = 1.35;
        /* Back clear, then hold one forward arc around the obstruction. Pick
           the side with a clear look-ahead when only one is available; keep
           a deterministic choice when both are equivalent. */
        const leftYaw = tank.yaw + .85, rightYaw = tank.yaw - .85;
        const probeRadius = tank.collisionRadius;
        const leftBlocked = circleHitsObstacle(
          oldX + Math.sin(leftYaw) * .9, oldZ + Math.cos(leftYaw) * .9,
          probeRadius);
        const rightBlocked = circleHitsObstacle(
          oldX + Math.sin(rightYaw) * .9, oldZ + Math.cos(rightYaw) * .9,
          probeRadius);
        tank.avoidTurn = leftBlocked !== rightBlocked
          ? (leftBlocked ? -1 : 1)
          : (((tank.id + ((state.time * .5) | 0)) & 1) ? 1 : -1);
        command.reverse = true;
        command.left = command.right = 1;
      }
    } else tank.blockedTime = Math.max(0, tank.blockedTime - dt * 2);
    tank.avoidTime = Math.max(0, tank.avoidTime - dt);
    const treadDx = tank.x - oldX, treadDz = tank.z - oldZ;
    /* An unobstructed tread step already has a known length because the
       cached yaw basis is unit length. Only the rare separation correction
       moves by an independently-computed vector and needs a square root. */
    const treadTravel = !blocked ? Math.abs(intendedTravel)
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
        tank.lastTreadX = tank.x; tank.lastTreadZ = tank.z;
      }
    }
    tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
    const aimLengthSquared = command.aimX * command.aimX
      + command.aimZ * command.aimZ;
    if (aimLengthSquared > .0025) {
      if (command.aimX !== tank.lastAimX || command.aimZ !== tank.lastAimZ) {
        tank.lastAimX = command.aimX;
        tank.lastAimZ = command.aimZ;
        tank.aimAngle = Math.atan2(command.aimX, command.aimZ);
      }
      const priorTurret = tank.turret;
      tank.turret = approachAngle(priorTurret, tank.aimAngle, 3.8 * dt);
      if (tank.turret !== priorTurret) updateTankTurretCache(tank);
    }
    tank.cooldown -= dt; if (tank.cooldown < 0) tank.cooldown = 0;
    tank.secondaryCooldown -= dt;
    if (tank.secondaryCooldown < 0) tank.secondaryCooldown = 0;
    tank.commandBuff -= dt; if (tank.commandBuff < 0) tank.commandBuff = 0;
    tank.recoil -= dt; if (tank.recoil < 0) tank.recoil = 0;
    tank.hitFlash -= dt; if (tank.hitFlash < 0) tank.hitFlash = 0;
    tank.shield -= dt; if (tank.shield < 0) tank.shield = 0;
    tank.boost -= dt; if (tank.boost < 0) tank.boost = 0;
    tank.repair -= dt; if (tank.repair < 0) tank.repair = 0;
    tank.gadgetCooldown -= dt;
    if (tank.gadgetCooldown < 0) tank.gadgetCooldown = 0;
    if (tank.repair > 0)
      tank.health = Math.min(tank.maxHealth, tank.health + 8 * dt);
    tank.spawnGrace -= dt; if (tank.spawnGrace < 0) tank.spawnGrace = 0;
    if (command.fire) fireTank(tank);
    if (command.secondary) fireSecondary(tank);
    if (command.gadget) useGadget(tank);
    if (command.ultimate) activateCommand(tank);
    /* Bot planning is staggered across frames. Its fire/gadget outputs are
       edge-triggered, so consume them here instead of repeating the action
       on the intervening movement-only frame. */
    if (!tank.player)
      command.fire = command.secondary = command.gadget = command.ultimate = false;
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
    showToast(state.gameMode === 3
      ? `WAVE ${state.wave} CLEAR` : "ARENA CLEAR", 1.25);
  }

  function damageTank(tank, damage, attacker, originX = tank.x,
                      originZ = tank.z, bypassShield = false) {
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
    tank.health -= damage;
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
      if (state.gameMode === 3 && state.multiplier !== 1) {
        state.multiplier = 1;
        hudIndicatorDirty = true;
      }
    }
    state.shake = Math.min(.22, state.shake + .07);
    if (tank.health > 0) {
      spawnParticles(tank.x, .35, tank.z, 6, 2.3);
      if (tank.player) sounds.damage();
      return true;
    }
    tank.active = false;
    spawnParticles(tank.x, .5, tank.z, 6, 3.2);
    sounds.explosion();
    if (tank.player) {
      state.lives--;
      if (state.lives <= 0) setMode("game-over");
      else {
        const gadget = tank.gadget;
        const classId = tank.classId;
        placeTank(tank, state.gameMode === 2 ? 1.6 : 0, -5.8,
          0, 0, gadget, "PLAYER", classId);
        tank.spawnGrace = 2.5;
        showToast(`REDEPLOY · ${state.lives} LEFT`, 1.2);
      }
    } else if (tank.team === 0) {
      tank.respawn = 3;
    } else {
      state.kills++;
      const killScore = 500 + (state.gameMode === 3
        ? Math.max(0, state.wave - 1) * 75 : state.arena * 150);
      state.score += killScore * (state.gameMode === 3
        ? state.multiplier : 1);
      if (state.gameMode === 3)
        state.multiplier = Math.min(5, state.multiplier + 1);
      if (attacker === 0) awardCommand(15);
      if (attacker === 0) sounds.kill();
      if (attacker === 0 && state.kills % 2 === 0) spawnPickup(tank.x, tank.z);
      if (state.gameMode === 1 || state.gameMode === 2) tank.respawn = 3.5;
      else if (!activeEnemyCount()) beginFinalKillBeat();
    }
    return true;
  }

  function updateBullets(dt) {
    let activeBullets = bulletActiveMask;
    while (activeBullets) {
      const at = activeMaskIndex(activeBullets);
      activeBullets = (activeBullets & (activeBullets - 1)) >>> 0;
      const bullet = bullets[at];
      bullet.life -= dt;
      if (bullet.life <= 0) { setBulletActive(at, false); continue; }
      let nextX = bullet.x + bullet.vx * dt;
      let nextZ = bullet.z + bullet.vz * dt;
      const barrier = bullet.overCover ? null : barrierAt(nextX, nextZ, .1);
      if (barrier) {
        barrier.health -= Math.max(1, bullet.barrierDamage | 0);
        spawnParticles(nextX, .35, nextZ, 6, 1.7);
        spawnDecal(2, nextX, nextZ, bullet.heading, .48, .38);
        if (barrier.health <= 0) {
          barrier.active = false;
          retainedSceneryDirty = true;
          state.barriersBroken++;
          state.score += bullet.owner === 0 ? 125 : 0;
          if (bullet.owner === 0) awardCommand(10);
          showToast("BARRIER BREACHED", .8);
          sounds.explosion(.1);
        }
        if (bullet.pierce > 0) {
          bullet.pierce--;
          nextX += bullet.vx * .055; nextZ += bullet.vz * .055;
        } else {
          setBulletActive(at, false);
          continue;
        }
      }
      let bounceX = Math.abs(nextX) > 7.85;
      let bounceZ = Math.abs(nextZ) > 7.85;
      if (bulletHitsObstacle(nextX, nextZ)) {
        const xOnly = !bulletHitsObstacle(nextX, bullet.z);
        const zOnly = !bulletHitsObstacle(bullet.x, nextZ);
        bounceZ = xOnly; bounceX = zOnly || !xOnly;
      }
      if (bounceX || bounceZ) {
        if (bullet.bounces <= 0) {
          spawnDecal(2, nextX, nextZ, bullet.heading, .44, .34);
          setBulletActive(at, false);
          continue;
        }
        bullet.bounces--;
        state.ricochets++;
        if (bullet.owner === 0) awardCommand(6);
        if (bounceX) {
          bullet.vx = -bullet.vx;
          bullet.headingSine = -bullet.headingSine;
          bullet.heading = -bullet.heading;
        }
        if (bounceZ) {
          bullet.vz = -bullet.vz;
          bullet.headingCosine = -bullet.headingCosine;
          bullet.heading = wrapAngle(Math.PI - bullet.heading);
        }
        nextX = bullet.x + bullet.vx * dt;
        nextZ = bullet.z + bullet.vz * dt;
        sounds.ricochet();
      }
      bullet.x = nextX; bullet.z = nextZ;
      const owner = tanks[bullet.owner];
      for (let tankAt = 0; tankAt < MAX_TANKS; tankAt++) {
        const tank = tanks[tankAt];
        if (!tank.active || tank.id === bullet.owner) continue;
        if (owner && tank.team === owner.team) continue;
        const dx = tank.x - bullet.x, dz = tank.z - bullet.z;
        if (dx * dx + dz * dz > .34) continue;
        setBulletActive(at, false);
        const damaged = damageTank(tank, bullet.damage, bullet.owner,
          bullet.x - bullet.vx * .04, bullet.z - bullet.vz * .04,
          bullet.bypassShield);
        spawnDecal(2, bullet.x, bullet.z, bullet.heading, .42, .34);
        if (bullet.owner === 0 && damaged) awardCommand(5);
        break;
      }
      if (bullet.active && convoy.active && owner && owner.team === 1) {
        const dx = convoy.x - bullet.x, dz = convoy.z - bullet.z;
        if (dx * dx + dz * dz < .48) {
          setBulletActive(at, false);
          convoy.health -= Math.max(6, bullet.damage * .45);
          spawnParticles(convoy.x, .4, convoy.z, 5, 1.6);
          if (convoy.health <= 0) {
            convoy.active = false;
            setMode("game-over");
          }
        }
      }
    }
  }

  function updatePickups(dt) {
    const player = tanks[0];
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
      for (let tankAt = 0; tankAt < MAX_TANKS; tankAt++) {
        const tank = tanks[tankAt];
        if (!tank.active || tank.team === mine.team) continue;
        const dx = tank.x - mine.x, dz = tank.z - mine.z;
        if (dx * dx + dz * dz < 2.2)
          damageTank(tank, 38, 0, mine.x, mine.z);
      }
      state.shake = Math.max(state.shake, .18);
      sounds.explosion(.13);
    }
  }

  function updateRespawns(dt) {
    if (state.gameMode === 0 || state.gameMode === 3) return;
    for (let at = 1; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active || tank.respawn <= 0) continue;
      tank.respawn -= dt;
      if (tank.respawn > 0) continue;
      const team = tank.team;
      const x = team === 0 ? -2.2 : 2.2;
      const z = team === 0 ? -5.7 : 5.7;
      placeTank(tank, x + (at & 1 ? .8 : -.8), z,
        team === 0 ? 0 : Math.PI, team, tank.gadget, tank.role, tank.classId);
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
    const wasOpen = state.gateOpen;
    if (state.gameMode === 0 || state.gameMode === 3) {
      state.gateOpen = activeEnemyCount()
        <= Math.max(1, (ARENAS[state.arena].enemies / 2) | 0);
    } else if (state.gameMode === 1) {
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
      const playerDistance = (player.x - convoy.x) ** 2 + (player.z - convoy.z) ** 2;
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
      convoy.z = -4.8 + convoy.progress * 10.6;
      convoy.x = -1.8 + Math.sin(convoy.progress * Math.PI * 2) * 1.15;
      state.gateOpen = convoy.progress >= .22;
      if (convoy.progress >= 1) {
        convoy.active = false;
        state.score += 2500;
        setMode("victory");
      }
    }
    if (wasOpen !== state.gateOpen) {
      retainedSceneryDirty = true;
      showToast(state.gateOpen ? "GATE OPEN" : "GATE SEALED", .75);
    }
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
    const displayClass = state.mode === "title" ? state.classChoice : player.classId;
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
    const enemyCount = state.gameMode === 0 || state.gameMode === 3
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
    const arenaText = state.gameMode === 0
      ? `A${state.arena + 1} F${enemyCount}`
      : state.gameMode === 1
        ? `C${state.blueControl | 0}-${state.redControl | 0}`
        : state.gameMode === 2
          ? `V${(convoy.progress * 100) | 0}`
          : `W${state.wave} X${state.multiplier} F${enemyCount}`;
    const armorText = `HP${Math.max(0, displayHealth)} L${state.lives}`;
    const secondary = secondaryStep > 0 ? `${secondaryStep}` : "R";
    const displayGadget = state.mode === "title"
      ? GADGETS[state.gadgetChoice] : player.gadget;
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
      authoredHudWrites++;
      ui.score.textContent = scoreText;
      if (state.gameMode === 0)
        ui.arena.textContent = `ARENA ${state.arena + 1} - ${enemyCount} FOES`;
      else if (state.gameMode === 1)
        ui.arena.textContent = `CONTROL ${state.blueControl | 0}-${state.redControl | 0}`;
      else if (state.gameMode === 2) ui.arena.textContent =
        `CONVOY ${(convoy.progress * 100) | 0}% - ${convoyHealthStep}`;
      else ui.arena.textContent =
        `WAVE ${state.wave} - X${state.multiplier} - ${enemyCount} FOES`;
      ui.armor.textContent = `${displayProfile.name} `
        + `${Math.max(0, displayHealth)}/${displayMaximum} - ${state.lives}`;
      ui.gadget.textContent = `${displayProfile.secondary} ${secondary}`
        + ` - ${displayGadget} ${gadget}`;
      if (ui.commandStatus && ui.commandMeterShell) {
        ui.commandStatus.hidden = !preferences.command;
        ui.commandMeterShell.hidden = !preferences.command;
        if (preferences.command) ui.commandStatus.textContent = commandText;
      }
    }
  }

  function update(dt, profileFrame = false) {
    let profileAt = profileFrame ? performance.now() : 0;
    const wallDt = dt;
    state.wallTime += wallDt;
    if (state.killBeat > 0) {
      state.killBeat = Math.max(0, state.killBeat - wallDt);
      dt *= .28;
      if (state.killBeat === 0 && state.pendingClear) {
        state.pendingClear = false;
        state.transition = state.gameMode === 3
          ? Math.max(.72, 1.35 - state.wave * .055) : 1.35;
        if (state.gameMode === 3) recordOnslaughtBest();
        sounds.waveClear();
        setMode("arena-clear");
      }
    }
    state.time += dt;
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
      applyLongSoakMovement(state.frames, dt);
      const inputFinished = performance.now();
      const inputElapsed = inputFinished - inputStarted;
      qualificationTiming.updateInput += inputElapsed;
      qualificationTiming.maxUpdateInput = Math.max(
        qualificationTiming.maxUpdateInput, inputElapsed);
      profileAt = inputFinished;
    } else {
      pollPlayerInput();
      applyLongSoakMovement(state.frames, dt);
    }
    if (toastUntil && state.time >= toastUntil) {
      if (authoredSurfaceVisible(state.mode))
        ui.toast.classList.remove("visible");
      hudToast = "";
      hudMeshDirty = true;
      toastUntil = 0;
    }
    updateParticles(dt);
    updateDecals(dt);
    state.shake = Math.max(0, state.shake - dt * .55);
    state.flash = Math.max(0, state.flash - dt);
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
        if (state.gameMode === 3) {
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
    const firstBot = online.active && online.role === "host" ? 2 : 1;
    for (let at = firstBot; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active && !debugBotsFrozen && ((state.frames + at) & 1) === 0)
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
    /* Planning remains staggered, but movement follows the display cadence.
       Advancing AI transforms in 15 Hz cohorts was measurable as alternating
       stationary/moving frames even when the renderer held 30 fps. */
    for (let at = 0; at < MAX_TANKS; at++) {
      const tank = tanks[at];
      if (tank.active) moveTank(tank, dt);
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
    updateMinesAndSmoke(dt);
    updateRespawns(dt);
    updateObjective(dt);
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - profileAt;
      qualificationTiming.updateWorld += elapsed;
      qualificationTiming.maxUpdateWorld = Math.max(
        qualificationTiming.maxUpdateWorld, elapsed);
      profileAt = now;
    }
    /* Text is retained at 15 Hz. The tiny arrows/meters are already
       quantized to coarse pixel steps, so refreshing them at 7.5 Hz avoids
       invalidating the much larger retained HUD vertex prefix on every
       other frame without changing input, simulation, or scene cadence.
       Forced/debug state changes retain their direct update paths. */
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

  function addTank(tank) {
    const baseColor = TANK_COLORS[tank.id];
    const color = tank.renderTint;
    const flash = Math.max(0, Math.min(1, tank.hitFlash / .12));
    const windup = tank.fireWindup > 0
      ? .28 + (((state.wallTime * 8 + tank.id) | 0) & 1) * .18 : 0;
    for (let channel = 0; channel < 3; channel++) {
      const warmed = baseColor[channel]
        + (COLORS.muzzle[channel] - baseColor[channel]) * windup;
      color[channel] = warmed + (1 - warmed) * flash;
    }
    const baseY = tank.surfaceY;
    const scale = tank.scale;
    const detailed = tank.id === 0;
    if (detailed) addRetainedTankBox(tank.id, 0, false,
      tank.x, baseY + .018, tank.z,
      1.32 * scale, .025, 1.7 * scale, tank.yaw, COLORS.shadow, .42,
      tank.yawSine, tank.yawCosine);
    const tankCosine = tank.yawCosine, tankSine = tank.yawSine;
    if (detailed) {
      const treadOffsetX = tankCosine * .49 * scale;
      const treadOffsetZ = -tankSine * .49 * scale;
      addRetainedTankBox(tank.id, 1, false,
        tank.x + treadOffsetX, baseY + .25, tank.z + treadOffsetZ,
        .32 * scale, .36 * scale, 1.42 * scale, tank.yaw, COLORS.tread, 1,
        tankSine, tankCosine);
      addRetainedTankBox(tank.id, 2, false,
        tank.x - treadOffsetX, baseY + .25, tank.z - treadOffsetZ,
        .32 * scale, .36 * scale, 1.42 * scale, tank.yaw, COLORS.tread, 1,
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
      (tank.classId === 2 ? .88 : .76) * scale, tank.turret, color, 1,
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
    const ratio = Math.max(0, tank.health) / tank.maxHealth;
    const lowHealthPulse = tank.player && ratio < .3
      ? .55 + .45 * Math.sin(state.wallTime * Math.PI * 2.4) : 0;
    if (lowHealthPulse > 0) {
      color[0] = COLORS.health[0]
        + (COLORS.bullet[0] - COLORS.health[0]) * lowHealthPulse;
      color[1] = COLORS.health[1]
        + (COLORS.bullet[1] - COLORS.health[1]) * lowHealthPulse;
      color[2] = COLORS.health[2]
        + (COLORS.bullet[2] - COLORS.health[2]) * lowHealthPulse;
    } else {
      color[0] = COLORS.health[0];
      color[1] = COLORS.health[1];
      color[2] = COLORS.health[2];
    }
    if (detailed) addRetainedTankBox(tank.id, 10, true,
      tank.x, baseY + 1.08, tank.z,
      1.08, .07, .08, cameraYaw, COLORS.healthLost, .85,
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
    /* Dynamic instances are appended after this prefix and never overwrite
       it. The retained scenery already remains in the upload arrays, so a
       per-frame copy only burned QuickJS time and memory bandwidth. */
    boxInstanceCount = retainedSceneryCount;
  }

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
      essentialTankInstances += tanks[at].id === 0 ? 6 : 3;
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
    /* A solid plinth and mast read as a world objective at PSP resolution.
       The former one-pixel translucent pulsing mast aliased into a flashing
       cylinder whenever it lined up with tanks behind it. */
    addBox(objectiveTarget.x, .045, objectiveTarget.z, .46, .09,
      .46, 0, COLORS.pickup, .72);
    addBox(objectiveTarget.x, .57, objectiveTarget.z, .12, 1.05,
      .12, 0, COLORS.pickup, 1);
    addBox(objectiveTarget.x, 1.12, objectiveTarget.z, .28, .12,
      .28, 0, COLORS.bullet, 1);
    if (state.gameMode === 1) {
      const blueLeads = state.blueControl >= state.redControl;
      addBox(0, .025, 0, 3.1, .05, 3.1, 0,
        blueLeads ? CONTROL_BLUE : CONTROL_RED, .62);
      addBox(0, .07, 0, .18, .14, 3.5, state.time * .25,
        COLORS.pickup, .75);
    }
    if (convoy.active) {
      addBox(convoy.x, .22, convoy.z, 1.5, .38, 1.85, 0, CONVOY_BODY);
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
    if (player.active) {
      if ((state.frames & 3) === 0) updateAimProbe(player);
      const reticleColor = aimProbe.hit ? COLORS.bullet : COLORS.pickup;
      addOptionalBox(aimProbe.x, .06, aimProbe.z, .5, .035, .055,
        player.turret, reticleColor, .9,
        player.turretSine, player.turretCosine);
      addOptionalBox(aimProbe.x, .06, aimProbe.z, .055, .035, .5,
        player.turret, reticleColor, .9,
        player.turretSine, player.turretCosine);
      if (aimProbe.hit)
        addOptionalBox(aimProbe.x + aimProbe.bounceX * .8, .07,
          aimProbe.z + aimProbe.bounceZ * .8, .06, .04, .75,
          0, COLORS.bullet, .8,
          aimProbe.bounceX / .28, aimProbe.bounceZ / .28);
    }
    let activeBullets = bulletActiveMask;
    while (activeBullets) {
      const at = activeMaskIndex(activeBullets);
      activeBullets = (activeBullets & (activeBullets - 1)) >>> 0;
      const bullet = bullets[at];
      if (addBox(bullet.x, .48, bullet.z, .13, .13, .28,
          0, COLORS.bullet, 1, bullet.headingSine, bullet.headingCosine))
        renderedBulletInstances++;
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
      if (mine.active) addOptionalBox(mine.x, .06, mine.z, .42, .12, .42,
        0, mine.arm > 0 ? COLORS.wall : COLORS.bullet, .9);
    }
    for (let at = 0; at < MAX_SMOKE; at++) {
      const smoke = smokeClouds[at];
      if (!smoke.active) continue;
      const pulse = .9 + Math.sin(state.time * 2) * .08;
      addOptionalBox(smoke.x, .42, smoke.z, 1.8 * pulse, .7, 1.8 * pulse,
        0, SMOKE_LOW, .42);
    }
    /* Pressure discards oldest decals before transient sparks. */
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
      const alpha = Math.max(0, Math.min(.58,
        decal.life / decal.maximum * .58));
      if (!addOptionalBox(decal.x, decal.y, decal.z,
          decal.width, .018, decal.depth, decal.yaw,
          decal.kind === 1 ? COLORS.tread : COLORS.shadow, alpha,
          decal.sine, decal.cosine))
        droppedDecalInstances++;
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
      const alpha = Math.max(0, particle.life / particle.maximum);
      if (!addOptionalBox(particle.x, Math.max(.04, particle.y), particle.z,
          .045, .045, particle.renderStreak, 0,
          COLORS.particle, alpha, particle.renderSine,
          particle.renderCosine))
        droppedParticleInstances++;
    }
    while (highParticles && renderedParticles < 6) {
      const at = 32 + activeMaskIndex(highParticles);
      highParticles = (highParticles & (highParticles - 1)) >>> 0;
      const particle = particles[at];
      renderedParticles++;
      const alpha = Math.max(0, particle.life / particle.maximum);
      if (!addOptionalBox(particle.x, Math.max(.04, particle.y), particle.z,
          .045, .045, particle.renderStreak, 0,
          COLORS.particle, alpha, particle.renderSine,
          particle.renderCosine))
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

  function updateCamera(dt) {
    const player = playerTank();
    if (preferences.camera === 0) cameraYaw = 0;
    else if (player.active) {
      const difference = wrapAngle(player.yaw - cameraYaw);
      const deadZone = .32;
      if (Math.abs(difference) > deadZone) {
        const follow = difference - Math.sign(difference) * deadZone;
        cameraYaw = wrapAngle(cameraYaw
          + Math.max(-dt * .72, Math.min(dt * .72, follow)));
      }
    }
    /* Presentation shake must not consume simulation randomness: future
       network peers can step the same input stream at a different render
       cadence without changing drops or bot decisions. */
    const visibleShake = preferences.shake ? state.shake : 0;
    const shakeX = visibleShake ? Math.sin(state.frames * 2.73) * visibleShake : 0;
    const shakeZ = visibleShake ? Math.cos(state.frames * 1.91) * visibleShake : 0;
    cameraSine = Math.sin(cameraYaw);
    cameraCosine = Math.cos(cameraYaw);
    const cameraLead = Math.min(1.25, Math.max(.35, cameraDistance * .32));
    /* A tank may face out of the arena while touching the perimeter. Looking
       beyond the inside face then puts the wall between the eye and target,
       even though the eye itself remains in bounds. Keep the focal point on
       the playable side of the wall. */
    const intendedTargetX = player.x + cameraSine * cameraLead;
    const intendedTargetZ = player.z + cameraCosine * cameraLead;
    const targetX = Math.max(-7.42, Math.min(7.42, intendedTargetX));
    const targetZ = Math.max(-7.42, Math.min(7.42, intendedTargetZ));
    const sharedCameraRay = targetX === intendedTargetX
      && targetZ === intendedTargetZ;
    /* Probe the low target-to-eye prefix every frame; amortizing it made walls
       appear screen-fixed before a hard camera retraction. */
    cameraSafetyUpdates++;
    cameraSafeDistance = 1.2;
    /* Candidate eyes share one ray; do not rescan its prefix per distance. */
    let cameraProbeAlong = 0;
    let cameraProbeBlocked = false;
    const lowRayRatio = (1.08 - .28) / (5.7 - .28);
    for (let sample = 3; sample <= 16; sample++) {
      const distance = sample * .4;
      const eyeX = player.x - cameraSine * distance;
      const eyeZ = player.z - cameraCosine * distance;
      if (Math.abs(eyeX) > 7.65 || Math.abs(eyeZ) > 7.65) break;
      if (sharedCameraRay) {
        /* The unclamped target and every candidate eye lie on the same unit
           camera vector. Walk that prefix directly: deriving its length with
           sqrt and then dividing every sample only reconstructs the same
           target - direction * along point. */
        const lowRayLength = (cameraLead + distance) * lowRayRatio;
        let along = cameraProbeAlong + .16;
        while (along <= lowRayLength) {
          cameraProbeAlong = along;
          if (circleHitsObstacle(targetX - cameraSine * along,
                targetZ - cameraCosine * along, .07)) {
            cameraProbeBlocked = true;
            break;
          }
          along += .16;
        }
      } else {
        /* At the perimeter the clamped target is no longer collinear with
           the camera. Retain the full bounded ray for that uncommon case. */
        const rayX = eyeX - targetX, rayZ = eyeZ - targetZ;
        const rayLength = Math.sqrt(rayX * rayX + rayZ * rayZ);
        if (rayLength > .001) {
          const lowRayLength = rayLength * lowRayRatio;
          let along = .16;
          while (along <= lowRayLength) {
            const fraction = along / rayLength;
            if (circleHitsObstacle(targetX + rayX * fraction,
                  targetZ + rayZ * fraction, .07)) {
              cameraProbeBlocked = true;
              break;
            }
            along += .16;
          }
        }
      }
      if (cameraProbeBlocked) break;
      cameraSafeDistance = distance;
    }
    /* Retract before composing this frame so the camera never eases through
       a solid wall. Expanding back to the normal distance remains smooth. */
    if (cameraSafeDistance < cameraDistance)
      cameraDistance = cameraSafeDistance;
    else
      cameraDistance += (cameraSafeDistance - cameraDistance)
        * Math.min(1, dt * 7);
    const eyeX = player.x - cameraSine * cameraDistance + shakeX;
    const eyeZ = player.z - cameraCosine * cameraDistance + shakeZ;
    lookAt(view, eyeX, 5.7, eyeZ, targetX, .28, targetZ);
    multiply(viewProjection, projection, view);
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
  function presentFrame(dt = 1 / 60, profileFrame = false,
                        deferOptionalWork = false,
                        frameWorkStarted = 0,
                        reuseUploadedScene = false) {
    let phaseStarted = profileFrame ? performance.now() : 0;
    updateCamera(dt);
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
    /* Simulation alone is not the whole callback budget: a busy effect
       frame can spend several more milliseconds rebuilding geometry. Check
       at the actual optional-work boundary and carry only retained HUD work
       forward when the frame has already consumed its share. */
    if (!deferOptionalWork && frameWorkStarted > 0
        && (hudMeshDirty || hudBuildPhase >= 0 || hudIndicatorDirty)
        && performance.now() - frameWorkStarted
          > OPTIONAL_HUD_START_BUDGET_MS) {
      deferOptionalWork = true;
      deadlineSceneReuses++;
    }
    if (!deferOptionalWork && (hudMeshDirty || hudBuildPhase >= 0)) {
      const hudPhase = hudBuildPhase < 0 ? 0 : Math.min(6, hudBuildPhase);
      const hudStarted = profileFrame ? performance.now() : 0;
      rebuildHudMeshSlice();
      if (profileFrame) {
        const elapsed = performance.now() - hudStarted;
        hudElapsed += elapsed;
        qualificationHudPhaseTotals[hudPhase] += elapsed;
        qualificationHudPhaseMaximums[hudPhase] = Math.max(
          qualificationHudPhaseMaximums[hudPhase], elapsed);
        qualificationHudPhaseCounts[hudPhase]++;
      }
    }
    if (!deferOptionalWork && hudIndicatorDirty) {
      const indicatorStarted = profileFrame ? performance.now() : 0;
      rebuildHudIndicators();
      if (profileFrame) {
        const elapsed = performance.now() - indicatorStarted;
        hudElapsed += elapsed;
        qualificationHudIndicatorTotal += elapsed;
        qualificationHudIndicatorMaximum = Math.max(
          qualificationHudIndicatorMaximum, elapsed);
        qualificationHudIndicatorCount++;
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
      && boxInstanceCount > 0 && hudCount > 0
      && boxInstanceCount <= MAX_INSTANCES_PER_DRAW;
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
  function runQualificationAction(frameNumber) {
    if (!qualificationActions || state.mode !== "playing") return;
    /* A long device soak repeats the four expensive interactions without
       growing its script or entity pools. The quiet part of each eight-second
       cycle still exercises ordinary play, AI, collision, and reclamation. */
    if (qualificationLongSoak) frameNumber %= 240;
    const player = tanks[0];
    if (frameNumber === 80) {
      qualificationActionKind = 1;
      spawnParticles(player.x, .45, player.z, 18, 3.2);
    } else if (frameNumber === 110) {
      qualificationActionKind = 2;
      player.secondaryCooldown = 0;
      fireSecondary(player);
    } else if (frameNumber === 140) {
      qualificationActionKind = 3;
      player.gadget = "SMOKE";
      player.gadgetCooldown = 0;
      useGadget(player);
    } else if (frameNumber === 170) {
      qualificationActionKind = 4;
      const target = tanks[1];
      if (target && target.active) {
        target.spawnGrace = 0;
        damageTank(target, target.maxHealth + 1, 0,
          player.x, player.z, true);
      }
    } else return;
    /* Include the triggering frame and the following two frames, where the
       new particles/projectiles and HUD state first reach the renderer. */
    qualificationActionFrames = 3;
  }

  function frame(timestamp) {
    if (!state.running) return;
    /* Do not replay simulation debt on an already late PSP frame. At the
       authored speed, one 1/30-second step stays below the collision sweep's
       smallest obstacle thickness; a stall therefore slows game time rather
       than creating a self-sustaining four-update catch-up spike. */
    const generationPhaseBefore = arenaGenerationPhase;
    const generationFrameStarted = generationPhaseBefore
      && arenaGenerationFrameMaximums ? performance.now() : 0;
    const elapsed = lastTimestamp
      ? Math.min(1 / 30, Math.max(0, (timestamp - lastTimestamp) / 1000)) : 0;
    lastTimestamp = timestamp;
    if (arenaGenerationPhase === ARENA_GENERATION_IDLE)
      runQualificationAction(state.frames);
    const profileFrame = (qualificationProfile || validationInputProfile)
      && qualificationTiming.samples < QUALIFICATION_PROFILE_SAMPLE_LIMIT
      && state.frames >= 20;
    const lateActionFrame = qualificationLongSoak && !profileFrame
      && qualificationActionFrames > 0;
    const measuredFrame = profileFrame || lateActionFrame;
    if (profileFrame && !qualificationProfileStarted)
      startQualificationProfile();
    const frameStarted = measuredFrame ? performance.now() : 0;
    const preludeBefore = qualificationTiming.updatePrelude;
    const botsBefore = qualificationTiming.updateBots;
    const moveBefore = qualificationTiming.updateMove;
    const projectilesBefore = qualificationTiming.updateProjectiles;
    const worldBefore = qualificationTiming.updateWorld;
    const updateStarted = performance.now();
    const arenaBeforeUpdate = state.arena;
    let updateElapsed = 0;
    if (elapsed > 0) update(elapsed, measuredFrame);
    updateElapsed = performance.now() - updateStarted;
    if (measuredFrame) {
      qualificationTiming.update += updateElapsed;
      qualificationTiming.maxUpdate = Math.max(
        qualificationTiming.maxUpdate, updateElapsed);
    }
    const buildBefore = qualificationTiming.build;
    const hudBefore = qualificationTiming.hud;
    const commandsBefore = qualificationTiming.commands;
    /* The frame still has command packing, native GE work, compositing, and
       publication ahead of it. If simulation consumed its measured share,
       publish the already-uploaded scene rather than beginning an unbounded
       geometry rebuild. This is a one-frame decision: ordinary frames keep
       full-rate transforms, while a collision/AI/GC tail cannot force a
       missed 30 Hz present. */
    /* Qualification must measure the product behavior, not disable its
       deadline guard and manufacture HUD work that a release frame defers. */
    const deferOptionalWork = updateElapsed > UPDATE_SCENE_BUDGET_MS;
    const generationSceneReuse = arenaGenerationReuseSceneOnce;
    arenaGenerationReuseSceneOnce = false;
    const reuseUploadedScene = generationSceneReuse || (deferOptionalWork
      && state.mode === "playing" && state.arena === arenaBeforeUpdate
      && boxInstanceCount > 0 && repeatedFrameList !== null);
    if (reuseUploadedScene) deadlineSceneReuses++;
    render(elapsed || 1 / 60, measuredFrame, deferOptionalWork,
      updateStarted, reuseUploadedScene);
    if (generationPhaseBefore && arenaGenerationFrameMaximums) {
      arenaGenerationFrameMaximums[generationPhaseBefore] = Math.max(
        arenaGenerationFrameMaximums[generationPhaseBefore],
        performance.now() - generationFrameStarted);
      if (arenaGenerationPhase === ARENA_GENERATION_IDLE)
        arenaGenerationReportFrames = 30;
    } else if (arenaGenerationReportFrames > 0
        && --arenaGenerationReportFrames === 0) {
      arenaGenerationReport = ["TREADLINE-FORGE-PHASES",
        `attempt=${arenaGenerationFrameMaximums[ARENA_GENERATION_ATTEMPTS].toFixed(3)}ms`,
        `spatial=${arenaGenerationFrameMaximums[ARENA_GENERATION_SPATIAL].toFixed(3)}ms`,
        `geometry=${arenaGenerationFrameMaximums[ARENA_GENERATION_GEOMETRY].toFixed(3)}ms`,
        `upload=${arenaGenerationFrameMaximums[ARENA_GENERATION_UPLOAD].toFixed(3)}ms`,
        `hold=${arenaGenerationFrameMaximums[ARENA_GENERATION_HOLD].toFixed(3)}ms`,
        `setup=${arenaGenerationFrameMaximums[ARENA_GENERATION_SETUP].toFixed(3)}ms`,
        `prepare=${arenaGenerationFrameMaximums[ARENA_GENERATION_PREPARE].toFixed(3)}ms`,
        `warm=${arenaGenerationFrameMaximums[ARENA_GENERATION_WARM].toFixed(3)}ms`,
        `activate=${arenaGenerationFrameMaximums[ARENA_GENERATION_ACTIVATE].toFixed(3)}ms`,
      ].join(" ");
      console.log(arenaGenerationReport);
      globalThis.pocSummary = arenaGenerationReport;
    }
    if (arenaGenerationReport && state.frames >= 90 && state.frames < 120)
      globalThis.pocSummary = arenaGenerationReport;
    if (lateActionFrame) {
      const frameElapsed = performance.now() - frameStarted;
      if (frameElapsed > qualificationLateActionMax) {
        qualificationLateActionMax = frameElapsed;
        qualificationLateActionUpdate = updateElapsed;
        qualificationLateActionBuild = qualificationTiming.build - buildBefore;
        qualificationLateActionHud = qualificationTiming.hud - hudBefore;
        qualificationLateActionCommands = qualificationTiming.commands
          - commandsBefore;
        globalThis.pocSummary = ["TREADLINE-LATE-ACTION",
          `kind=${qualificationActionKind}`,
          `total=${frameElapsed.toFixed(3)}ms`,
          `update=${updateElapsed.toFixed(3)}ms`,
          `build=${(qualificationTiming.build - buildBefore).toFixed(3)}ms`,
          `hud=${(qualificationTiming.hud - hudBefore).toFixed(3)}ms`,
          `commands=${(qualificationTiming.commands
            - commandsBefore).toFixed(3)}ms`].join(" ");
      }
    }
    if (profileFrame) {
      const frameElapsed = performance.now() - frameStarted;
      const sample = qualificationTiming.samples * 5;
      qualificationFrameSamples[sample] = updateElapsed;
      qualificationFrameSamples[sample + 1] =
        qualificationTiming.build - buildBefore;
      qualificationFrameSamples[sample + 2] =
        qualificationTiming.hud - hudBefore;
      qualificationFrameSamples[sample + 3] =
        qualificationTiming.commands - commandsBefore;
      qualificationFrameSamples[sample + 4] = frameElapsed;
      qualificationTiming.frame += frameElapsed;
      qualificationTiming.maxFrame = Math.max(
        qualificationTiming.maxFrame, frameElapsed);
      recordQualificationSlowFrame(qualificationActionKind,
        updateElapsed,
        qualificationTiming.updatePrelude - preludeBefore,
        qualificationTiming.updateBots - botsBefore,
        qualificationTiming.updateMove - moveBefore,
        qualificationTiming.updateProjectiles - projectilesBefore,
        qualificationTiming.updateWorld - worldBefore,
        qualificationTiming.build - buildBefore,
        qualificationTiming.hud - hudBefore,
        qualificationTiming.commands - commandsBefore,
        frameElapsed);
      if (qualificationActionFrames > 0) {
        qualificationActionSamples++;
        qualificationActionMax = Math.max(qualificationActionMax, frameElapsed);
      }
      qualificationTiming.samples++;
    }
    if (qualificationActionFrames > 0) {
      qualificationActionFrames--;
      if (qualificationActionFrames === 0) qualificationActionKind = 0;
    }
    if (profileFrame
        && qualificationTiming.samples === QUALIFICATION_PROFILE_SAMPLE_LIMIT) {
        const timing = qualificationTiming, count = timing.samples;
        const bridge = globalThis.__tilefinchWebGLDiagnostics;
        const draws = Math.max(1, Number(bridge?.profileDraws) || 0);
        const summary = ["TREADLINE-JS-PROFILE", `samples=${count}`,
          `bridge-profile=${qualificationBridgeProfile ? "on" : "off"}`,
          `update=${(timing.update / count).toFixed(3)}ms`,
          `update-prelude=${(timing.updatePrelude / count).toFixed(3)}ms`,
          `update-input=${(timing.updateInput / count).toFixed(3)}ms`,
          `update-bots=${(timing.updateBots / count).toFixed(3)}ms`,
          `update-move=${(timing.updateMove / count).toFixed(3)}ms`,
          `update-projectiles=${(timing.updateProjectiles / count).toFixed(3)}ms`,
          `update-world=${(timing.updateWorld / count).toFixed(3)}ms`,
          `update-ui=${(timing.updateUi / count).toFixed(3)}ms`,
          `camera=${(timing.camera / count).toFixed(3)}ms`,
          `build=${(timing.build / count).toFixed(3)}ms`,
          `build-scenery=${(timing.buildScenery / count).toFixed(3)}ms`,
          `build-tanks=${(timing.buildTanks / count).toFixed(3)}ms`,
          `build-effects=${(timing.buildEffects / count).toFixed(3)}ms`,
          `instances=${(timing.instances / count).toFixed(1)}`,
          `max-instances=${timing.maxInstances}`,
          `kills=${state.kills}`,
          `shots=${state.shots}`,
          `player-blocked=${playerTank().blockedTime.toFixed(3)}`,
          `upload=${(timing.upload / count).toFixed(3)}ms`,
          `hud=${(timing.hud / count).toFixed(3)}ms`,
          `commands=${(timing.commands / count).toFixed(3)}ms`,
          `frame=${(timing.frame / count).toFixed(3)}ms`,
          `max-update=${timing.maxUpdate.toFixed(3)}ms`,
          `max-camera=${timing.maxCamera.toFixed(3)}ms`,
          `max-build=${timing.maxBuild.toFixed(3)}ms`,
          `max-upload=${timing.maxUpload.toFixed(3)}ms`,
          `max-hud=${timing.maxHud.toFixed(3)}ms`,
          `max-commands=${timing.maxCommands.toFixed(3)}ms`,
          `max-frame=${timing.maxFrame.toFixed(3)}ms`,
          `bridge-draws=${draws}`,
          `bridge-plan=${Number(bridge?.drawPlanHits || 0)}`
            + `/${Number(bridge?.drawPlanMisses || 0)}`,
          `bridge-template=${Number(bridge?.commandTemplateHits || 0)}`
            + `/${Number(bridge?.commandTemplateMisses || 0)}`,
          `bridge-slot=${Number(bridge?.commandSlotTemplateHits || 0)}`
            + `/${Number(bridge?.commandSlotTemplateMisses || 0)}`,
          `bridge-packet=${Number(bridge?.sourcePacketHits || 0)}`
            + `/${Number(bridge?.sourcePacketMisses || 0)}`,
          `bridge-index=${Number(bridge?.sourcePacketIndexHits || 0)}`
            + `/${Number(bridge?.sourcePacketIndexMisses || 0)}`,
          `bridge-basic=${(Number(bridge?.profileBasicMs || 0) / draws).toFixed(3)}ms`,
          `bridge-instance=${(Number(bridge?.profileInstancesMs || 0) / draws).toFixed(3)}ms`,
          `bridge-range=${(Number(bridge?.profileRangesMs || 0) / draws).toFixed(3)}ms`,
          `bridge-prepare=${(Number(bridge?.profilePrepareMs || 0) / draws).toFixed(3)}ms`,
          `bridge-enqueue=${(Number(bridge?.profileEnqueueMs || 0) / draws).toFixed(3)}ms`,
          `bridge-admit=${(Number(bridge?.profileQueueAdmissionMs || 0) / draws).toFixed(3)}ms`,
          `bridge-pack=${(Number(bridge?.profileWirePackMs || 0) / draws).toFixed(3)}ms`,
        ].join(" ");
        globalThis.pocSummary = summary;
        console.log(summary);
        const tailSummary = ["TREADLINE-JS-TAIL", `samples=${count}`,
          `update-p95=${qualificationPercentile(0, count, 95).toFixed(3)}ms`,
          `build-p95=${qualificationPercentile(1, count, 95).toFixed(3)}ms`,
          `hud-p95=${qualificationPercentile(2, count, 95).toFixed(3)}ms`,
          `commands-p95=${qualificationPercentile(3, count, 95).toFixed(3)}ms`,
          `frame-p50=${qualificationPercentile(4, count, 50).toFixed(3)}ms`,
          `frame-p95=${qualificationPercentile(4, count, 95).toFixed(3)}ms`,
          `frame-p99=${qualificationPercentile(4, count, 99).toFixed(3)}ms`,
          `action-samples=${qualificationActionSamples}`,
          `action-max=${qualificationActionMax.toFixed(3)}ms`,
          `bridge-draws=${draws}`,
          `bridge-admit=${(Number(bridge?.profileQueueAdmissionMs || 0) / draws).toFixed(3)}ms`,
          `bridge-pack=${(Number(bridge?.profileWirePackMs || 0) / draws).toFixed(3)}ms`,
        ].join(" ");
        console.log(tailSummary);
        const hudPhaseSummary = [];
        for (let phase = 0; phase < qualificationHudPhaseCounts.length;
             phase++) {
          const phaseCount = qualificationHudPhaseCounts[phase];
          hudPhaseSummary.push(`${phase}:`
            + `${(qualificationHudPhaseTotals[phase]
              / Math.max(1, phaseCount)).toFixed(2)}`
            + `/${qualificationHudPhaseMaximums[phase].toFixed(2)}`
            + `/${phaseCount}`);
        }
        const phaseSummary = ["TREADLINE-JS-PHASES", `samples=${count}`,
          `slot=${Number(bridge?.commandSlotTemplateHits || 0)}`
            + `/${Number(bridge?.commandSlotTemplateMisses || 0)}`,
          `packet=${Number(bridge?.sourcePacketHits || 0)}`
            + `/${Number(bridge?.sourcePacketMisses || 0)}`,
          `pre=${(timing.updatePrelude / count).toFixed(3)}`,
          `input=${(timing.updateInput / count).toFixed(3)}`,
          `bots=${(timing.updateBots / count).toFixed(3)}`,
          `move=${(timing.updateMove / count).toFixed(3)}`,
          `projectiles=${(timing.updateProjectiles / count).toFixed(3)}`,
          `world=${(timing.updateWorld / count).toFixed(3)}`,
          `ui=${(timing.updateUi / count).toFixed(3)}`,
          `scenery=${(timing.buildScenery / count).toFixed(3)}`,
          `tanks=${(timing.buildTanks / count).toFixed(3)}`,
          `effects=${(timing.buildEffects / count).toFixed(3)}`,
          `instances=${(timing.instances / count).toFixed(1)}`,
          `maxinst=${timing.maxInstances}`,
          `kills=${state.kills}`,
          `shots=${state.shots}`,
          `blocked=${playerTank().blockedTime.toFixed(3)}`,
          `camera=${(timing.camera / count).toFixed(3)}`,
          `upload=${(timing.upload / count).toFixed(3)}`,
          `hud=${(timing.hud / count).toFixed(3)}`,
          `commands=${(timing.commands / count).toFixed(3)}`,
          `frame=${(timing.frame / count).toFixed(3)}`,
          `action=${qualificationActionMax.toFixed(3)}`,
          `maxu=${timing.maxUpdate.toFixed(3)}`,
          `maxpre=${timing.maxUpdatePrelude.toFixed(3)}`,
          `maxinput=${timing.maxUpdateInput.toFixed(3)}`,
          `maxbot=${timing.maxUpdateBots.toFixed(3)}`,
          `maxmove=${timing.maxUpdateMove.toFixed(3)}`,
          `maxproj=${timing.maxUpdateProjectiles.toFixed(3)}`,
          `maxworld=${timing.maxUpdateWorld.toFixed(3)}`,
          `maxui=${timing.maxUpdateUi.toFixed(3)}`,
          `maxb=${timing.maxBuild.toFixed(3)}`,
          `maxup=${timing.maxUpload.toFixed(3)}`,
          `maxhud=${timing.maxHud.toFixed(3)}`,
          `hudph=${hudPhaseSummary.join(",")}`,
          `hudi=${(qualificationHudIndicatorTotal
            / Math.max(1, qualificationHudIndicatorCount)).toFixed(2)}`
            + `/${qualificationHudIndicatorMaximum.toFixed(2)}`
            + `/${qualificationHudIndicatorCount}`,
          `maxcmd=${timing.maxCommands.toFixed(3)}`,
          `maxframe=${timing.maxFrame.toFixed(3)}`,
          `repeatcap=${repeatedFrameCaptures}`,
          `idx=${Number(bridge?.sourcePacketIndexHits || 0)}`
            + `/${Number(bridge?.sourcePacketIndexMisses || 0)}`,
          `b-basic=${(Number(bridge?.profileBasicMs || 0) / draws).toFixed(3)}`,
          `b-inst=${(Number(bridge?.profileInstancesMs || 0) / draws).toFixed(3)}`,
          `b-range=${(Number(bridge?.profileRangesMs || 0) / draws).toFixed(3)}`,
          `b-prep=${(Number(bridge?.profilePrepareMs || 0) / draws).toFixed(3)}`,
          `b-enq=${(Number(bridge?.profileEnqueueMs || 0) / draws).toFixed(3)}`,
          `b-finish=${(Number(bridge?.profileFinishMs || 0) / draws).toFixed(3)}`,
          `b-admit=${(Number(bridge?.profileQueueAdmissionMs || 0) / draws).toFixed(3)}`,
          `b-pack=${(Number(bridge?.profileWirePackMs || 0) / draws).toFixed(3)}`,
        ].join(" ");
        console.log(phaseSummary);
        globalThis.pocSummary = phaseSummary;
        const slowEventNames = ["steady", "particles", "secondary",
          "smoke", "destroy"];
        const slowSummaries = [
          "SLOW-COLUMNS rank event total update pre bots move projectile world build hud command",
        ];
        for (let rank = 0; rank < qualificationSlowFrameCount; rank++) {
          const at = rank * QUALIFICATION_SLOW_FRAME_WORDS;
          console.log(["TREADLINE-SLOW", `rank=${rank + 1}`,
            `event=${slowEventNames[qualificationSlowFrameEvents[rank]]}`,
            `total=${qualificationSlowFrames[at + 9].toFixed(3)}`,
            `update=${qualificationSlowFrames[at].toFixed(3)}`,
            `pre=${qualificationSlowFrames[at + 1].toFixed(3)}`,
            `bots=${qualificationSlowFrames[at + 2].toFixed(3)}`,
            `move=${qualificationSlowFrames[at + 3].toFixed(3)}`,
            `projectiles=${qualificationSlowFrames[at + 4].toFixed(3)}`,
            `world=${qualificationSlowFrames[at + 5].toFixed(3)}`,
            `build=${qualificationSlowFrames[at + 6].toFixed(3)}`,
            `hud=${qualificationSlowFrames[at + 7].toFixed(3)}`,
            `commands=${qualificationSlowFrames[at + 8].toFixed(3)}`,
          ].join(" "));
          slowSummaries.push([
            "SLOW", rank + 1,
            slowEventNames[qualificationSlowFrameEvents[rank]],
            qualificationSlowFrames[at + 9].toFixed(3),
            qualificationSlowFrames[at].toFixed(3),
            qualificationSlowFrames[at + 1].toFixed(3),
            qualificationSlowFrames[at + 2].toFixed(3),
            qualificationSlowFrames[at + 3].toFixed(3),
            qualificationSlowFrames[at + 4].toFixed(3),
            qualificationSlowFrames[at + 5].toFixed(3),
            qualificationSlowFrames[at + 6].toFixed(3),
            qualificationSlowFrames[at + 7].toFixed(3),
            qualificationSlowFrames[at + 8].toFixed(3),
          ].join(" "));
        }
        globalThis.pocSummary = `${phaseSummary}\n${slowSummaries.join("\n")}`;
        console.log(["TREADLINE-BRIDGE-PROFILE", `draws=${draws}`,
          `basic=${(Number(bridge?.profileBasicMs || 0) / draws).toFixed(3)}ms`,
          `instance=${(Number(bridge?.profileInstancesMs || 0) / draws).toFixed(3)}ms`,
          `range=${(Number(bridge?.profileRangesMs || 0) / draws).toFixed(3)}ms`,
          `prepare=${(Number(bridge?.profilePrepareMs || 0) / draws).toFixed(3)}ms`,
          `enqueue=${(Number(bridge?.profileEnqueueMs || 0) / draws).toFixed(3)}ms`,
          `admit=${(Number(bridge?.profileQueueAdmissionMs || 0) / draws).toFixed(3)}ms`,
          `pack=${(Number(bridge?.profileWirePackMs || 0) / draws).toFixed(3)}ms`,
        ].join(" "));
        if (bridge) bridge.profileDrawPhases = false;
    }
    requestAnimationFrame(frame);
  }

  /* The large qualification surface is useful to host conformance and
     explicitly requested PSP probes, but constructing all of its closures
     on every ordinary device launch only delays the first playable frame.
     Keep shipping PSP startup on the game path; qualification URLs retain
     the complete seam. */
  const exposeDebug = navigator.platform !== "PSP"
    || qualificationURL.includes("qualification=")
    || globalThis.__treadlineEnableDebug === true;
  if (exposeDebug) {
  const debug = Object.freeze({
    start() { if (resetGame()) setMode("playing"); },
    setPaused(paused) { setMode(paused ? "paused" : "playing"); },
    selectMode(index) {
      state.gameMode = Math.max(0, Math.min(GAME_MODES.length - 1, index | 0));
      refreshSetupLabels();
    },
    selectGadget(index) {
      state.gadgetChoice = Math.max(0, Math.min(GADGETS.length - 1, index | 0));
      refreshSetupLabels();
    },
    selectClass(index) {
      state.classChoice = Math.max(0, Math.min(CLASSES.length - 1, index | 0));
      refreshSetupLabels();
    },
    selectDifficulty(index) {
      state.difficultyChoice = Math.max(0,
        Math.min(DIFFICULTIES.length - 1, index | 0));
      refreshSetupLabels();
    },
    selectCamera(index) {
      preferences.camera = index ? 1 : 0;
      if (!preferences.camera) {
        cameraYaw = 0; cameraSine = 0; cameraCosine = 1;
      }
      refreshSetupLabels();
    },
    selectControls(index) {
      preferences.controls = index === CONTROL_CLASSIC
        ? CONTROL_CLASSIC : CONTROL_ARCADE;
      clearSchemeKeys();
      resetAimAssist();
      refreshSetupLabels(); updateControlHint();
    },
    selectAimAssist(index) {
      index = Number(index) | 0;
      preferences.assist = index >= ASSIST_OFF && index <= ASSIST_LOCK
        ? index : ASSIST_SNAP;
      resetAimAssist();
      refreshSetupLabels(); updateControlHint();
    },
    savePreferences,
    reloadPreferences() {
      loadPreferences(); resetAimAssist(); refreshSetupLabels();
      updateControlHint();
    },
    setCommandEnabled(enabled) {
      preferences.command = !!enabled;
      if (!preferences.command) state.commandMeter = 0;
      refreshSetupLabels(); updateHud(true);
    },
    setMusicEnabled(enabled) {
      preferences.music = !!enabled; sounds.setMusic(preferences.music);
      refreshSetupLabels();
    },
    setCommandMeter(value) {
      state.commandMeter = Math.max(0, Math.min(100, Number(value) || 0));
      updateOverlay();
    },
    mapScreenAim(x, y) {
      const command = {aimX: 0, aimZ: 0};
      applyScreenAim(command, Number(x) || 0, Number(y) || 0);
      return {x: command.aimX, z: command.aimZ};
    },
    pollInput() {
      pollPlayerInput();
      const command = playerTank().command;
      return {left: command.left, right: command.right,
        reverse: command.reverse, aimX: command.aimX, aimZ: command.aimZ,
        fire: command.fire, secondary: command.secondary,
        gadget: command.gadget, ultimate: command.ultimate,
        lockTarget: input.assistLockTarget};
    },
    resetInputProbe() {
      input.keys = Object.create(null);
      input.source = "keyboard";
      input.lastFire = input.lastSecondary = input.lastGadget = false;
      input.lastUltimate = input.lastPause = false;
      input.fireQueued = input.secondaryQueued = input.gadgetQueued = false;
      input.ultimateQueued = input.pauseQueued = false;
      resetAimAssist();
      const command = playerTank().command;
      command.left = command.right = 0; command.reverse = false;
      command.fire = command.secondary = command.gadget = command.ultimate = false;
    },
    cameraProgramsAligned() {
      if (!boxInstanceProgram) return true;
      const boxCamera = gl.getUniform(
        boxInstanceProgram, boxInstanceViewProjectionLocation);
      if (!boxCamera || boxCamera.length !== viewProjection.length)
        return false;
      for (let at = 0; at < viewProjection.length; at++)
        if (Math.abs(boxCamera[at] - viewProjection[at]) > .00001)
          return false;
      return true;
    },
    freezeBots(frozen) { debugBotsFrozen = !!frozen; },
    setTankActive(index, active) {
      const tank = tanks[index | 0];
      if (tank && !tank.player) tank.active = !!active;
    },
    setTankPosition(index, x, z) {
      const tank = tanks[index | 0];
      if (!tank || !tank.active) return;
      tank.x = Number(x) || 0; tank.z = Number(z) || 0;
      tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
    },
    setTankHeading(index, yaw) {
      const tank = tanks[index | 0];
      if (!tank || !tank.active || !Number.isFinite(Number(yaw))) return;
      tank.yaw = tank.turret = wrapAngle(Number(yaw));
      updateTankYawCache(tank);
      updateTankTurretCache(tank);
    },
    armBotShot(index, secondary = false) {
      const tank = tanks[index | 0];
      const player = tanks[0];
      if (!tank || tank.player || !tank.active || !player.active) return false;
      tank.target = player.id;
      tank.aiThink = 999;
      const angle = Math.atan2(player.x - tank.x, player.z - tank.z);
      tank.yaw = tank.turret = angle;
      updateTankYawCache(tank);
      updateTankTurretCache(tank);
      return beginBotFireWindup(tank, !!secondary);
    },
    destroyTank(index, attacker = 0) {
      const tank = tanks[index | 0];
      if (!tank || tank.player || !tank.active) return false;
      tank.spawnGrace = 0;
      tank.health = 1;
      return damageTank(tank, 1000, attacker | 0,
        tank.x, tank.z - 1, true);
    },
    completeArenaTransition() {
      const before = state.arena;
      const writesBefore = authoredHudWrites;
      for (let at = 1; at < MAX_TANKS; at++) tanks[at].active = false;
      state.mode = "arena-clear";
      state.transition = 0;
      updateHud(true);
      update(1 / 30);
      return {transitioned: state.mode === "playing" && state.arena !== before,
        authoredWrites: authoredHudWrites - writesBefore};
    },
    tankState(index) {
      const tank = tanks[index | 0];
      if (!tank) return null;
      return {active: tank.active, x: tank.x, z: tank.z, team: tank.team,
        role: tank.role, health: tank.health, maxHealth: tank.maxHealth,
        classId: tank.classId, target: tank.target,
        blockedTime: tank.blockedTime, avoidTime: tank.avoidTime,
        hitFlash: tank.hitFlash, fireWindup: tank.fireWindup,
        fireTelegraphed: tank.fireTelegraphed,
        backingOff: tank.backingOff, standoff: tank.standoff,
        reversing: tank.command.reverse,
        placementValid: !tank.active || (Math.abs(tank.x) <= 7.65
          && Math.abs(tank.z) <= 7.65
          && !circleHitsObstacle(tank.x, tank.z, tank.collisionRadius))};
    },
    tankPairClearance() {
      let minimum = 99;
      for (let left = 0; left < MAX_TANKS; left++) {
        const first = tanks[left];
        if (!first.active) continue;
        for (let right = left + 1; right < MAX_TANKS; right++) {
          const second = tanks[right];
          if (!second.active) continue;
          const dx = first.x - second.x, dz = first.z - second.z;
          const distance = Math.sqrt(dx * dx + dz * dz);
          const contact = Math.sqrt(CLASS_COMBINED_RADIUS_SQUARED[
            first.classId * CLASSES.length + second.classId]);
          minimum = Math.min(minimum, distance - contact);
        }
      }
      return minimum;
    },
    fireSecondary() {
      const player = tanks[0];
      player.secondaryCooldown = 0;
      fireSecondary(player); render(1 / 60);
    },
    activateCommand() {
      activateCommand(tanks[0]); render(1 / 60);
    },
    damagePlayerFrom(zone, damage = 40, bypassShield = false) {
      const player = tanks[0];
      player.spawnGrace = 0;
      let angle = player.yaw;
      if (zone === "SIDE") angle += Math.PI * .5;
      else if (zone === "REAR") angle += Math.PI;
      damageTank(player, Number(damage) || 0, 1,
        player.x + Math.sin(angle) * 2,
        player.z + Math.cos(angle) * 2, !!bypassShield);
      render(1 / 60);
    },
    activateGadget(name) {
      if (!GADGETS.includes(name)) return;
      const player = tanks[0];
      player.gadget = name; player.gadgetCooldown = 0;
      useGadget(player); render(1 / 60);
    },
    clearGadgetEffects() {
      const player = tanks[0];
      player.shield = player.repair = player.boost = player.gadgetCooldown = 0;
      for (const mine of mines) mine.active = false;
      for (const smoke of smokeClouds) smoke.active = false;
      activeSmokeCount = 0;
    },
    probeRicochet() {
      for (const bullet of bullets) bullet.active = false;
      bulletActiveMask = 0;
      state.ricochets = 0;
      const bullet = bullets[0];
      setBulletActive(0, true); bullet.owner = 0;
      bullet.x = 7.82; bullet.z = 6.5; bullet.vx = 6; bullet.vz = 0;
      bullet.headingSine = 1; bullet.headingCosine = 0;
      bullet.life = 3; bullet.bounces = 1;
      updateBullets(1 / 60); render(1 / 60);
    },
    probeSecondImpact() {
      const bullet = bullets[0];
      setBulletActive(0, true); bullet.owner = 0;
      bullet.x = -7.82; bullet.z = 6.5; bullet.vx = -6; bullet.vz = 0;
      bullet.headingSine = -1; bullet.headingCosine = 0;
      bullet.life = 3; bullet.bounces = 0;
      updateBullets(1 / 60); render(1 / 60);
    },
    probeBarrier() {
      const barrier = barriers.find((candidate) => candidate.active);
      if (!barrier) return;
      for (let hit = 0; hit < 2; hit++) {
        const bullet = bullets[0];
        setBulletActive(0, true); bullet.owner = 0;
        bullet.x = barrier.x; bullet.z = barrier.z;
        bullet.vx = 0; bullet.vz = 0; bullet.life = 1; bullet.bounces = 1;
        bullet.overCover = false;
        bullet.headingSine = 0; bullet.headingCosine = 1;
        updateBullets(1 / 60);
      }
      render(1 / 60);
    },
    step(frames = 1) {
      for (let at = 0; at < frames; at++) {
        update(1 / 60); render(1 / 60);
      }
    },
    stepSimulation(frames = 1) {
      for (let at = 0; at < frames; at++) {
        update(1 / 60);
        state.frames++;
      }
    },
    stepCamera(frames = 1) {
      for (let at = 0; at < frames; at++) {
        update(1 / 60);
        updateCamera(1 / 60);
        state.frames++;
      }
    },
    delayedFrame(elapsedSeconds) {
      const before = state.time;
      advanceElapsed(elapsedSeconds);
      return state.time - before;
    },
    setKeyboard(action, pressed) {
      if (Object.prototype.hasOwnProperty.call(input.keys, action)
          || ["a", "d", "w", "s", "fire", "secondary", "gadget", "ultimate", "aimLeft",
            "aimRight", "aimUp", "aimDown"].includes(action)) {
        input.keys[action] = !!pressed;
        setSource("keyboard");
      }
    },
    longSoakCommand(frameNumber) {
      const command = {left: 0, right: 0, reverse: false};
      setLongSoakMovement(command, Math.max(0, Number(frameNumber) | 0));
      return command;
    },
    saturateVisualLoad() {
      const positions = [-6, -5, -3.8, -4.8, 3.8, -4.8,
        -4.8, 4.6, 4.8, 4.6, 0, 5.8];
      for (let at = 0; at < MAX_TANKS; at++) {
        const tank = tanks[at];
        placeTank(tank, positions[at * 2], positions[at * 2 + 1],
          at * .55, at === 0 ? 0 : 1, tank.gadget,
          at === 0 ? "PLAYER" : "HUNTER", at % CLASSES.length);
        tank.spawnGrace = 0;
        tank.recoil = .1; tank.shield = 1; tank.repair = 1;
      }
      bulletActiveMask = 0;
      for (let at = 0; at < MAX_BULLETS; at++) {
        const bullet = bullets[at];
        setBulletActive(at, true);
        bullet.owner = 0; bullet.x = -7 + (at % 9) * 1.6;
        bullet.z = -6.8 + ((at / 9) | 0) * .35;
        bullet.vx = 0; bullet.vz = 1; bullet.life = 3;
        bullet.headingSine = 0; bullet.headingCosine = 1;
      }
      for (let at = 0; at < MAX_DECALS + 5; at++)
        spawnDecal(at & 1 ? 1 : 2, -5 + (at % 6) * 1.7,
          5.3 + ((at / 6) | 0) * .25, at * .19);
      for (let at = 0; at < 12; at++) {
        const particle = particles[at];
        setParticleActive(at, true);
        particle.x = -4 + at * .6; particle.y = .3; particle.z = 6;
        particle.vx = 1 + at * .1; particle.vy = .2; particle.vz = .5;
        particle.life = particle.maximum = 1;
      }
      render(1 / 60);
    },
    arenaStaticIndexCounts() {
      const counts = [];
      for (let at = 0; at < ARENAS.length; at++) {
        const cached = arenaGeometryCache[at];
        counts.push(arenaCommonIndexCount + (cached ? cached.indexCount : 0));
      }
      return counts;
    },
    arenaValidationMask(index, mode = 0) {
      index = Math.max(0, Math.min(ARENAS.length - 1, index | 0));
      const result = validateArenaLayout(index, mode | 0);
      return (result.connected ? 1 : 0) | (result.flanking ? 2 : 0)
        | (result.sightlines ? 4 : 0) | (result.cover ? 8 : 0)
        | (result.density ? 16 : 0) | (result.symmetry ? 32 : 0);
    },
    arenaValidationMetrics(index, mode = 0) {
      index = Math.max(0, Math.min(ARENAS.length - 1, index | 0));
      return validateArenaLayout(index, mode | 0);
    },
    generateArenaSeed(seed) {
      const gameplayBefore = randomState;
      const accepted = generateArena(Number(seed) >>> 0);
      return {accepted, checksum: arenaGenerationChecksum,
        attempts: arenaGenerationAttempts, fallback: arenaGenerationFallback,
        gameplayUnchanged: gameplayBefore === randomState,
        storageStable: arenaGenerator.storageStable()};
    },
    beginArenaGeneration(seed, attemptLimit = 20) {
      return arenaGenerator.beginGeneration(Number(seed) >>> 0,
        Number(attemptLimit) | 0);
    },
    stepArenaGeneration(maxAttempts = 2) {
      return arenaGenerator.stepGeneration(Number(maxAttempts) | 0);
    },
    generatedArenaProbe(seedCount = 100) {
      const gameplayBefore = randomState;
      let accepted = 0, fallbacks = 0, maximumAttempts = 0;
      seedCount = Math.max(1, Math.min(100, seedCount | 0));
      for (let seed = 1; seed <= seedCount; seed++) {
        if (generateArena(Math.imul(seed, 0x45d9f3b) >>> 0)) accepted++;
        else fallbacks++;
        maximumAttempts = Math.max(maximumAttempts, arenaGenerationAttempts);
      }
      return {accepted, fallbacks, maximumAttempts,
        obstacleCount: generatedArena.obstacleCount,
        barrierCount: generatedArena.barrierCount,
        rampCount: generatedArena.rampCount,
        checksum: arenaGenerationChecksum,
        gameplayUnchanged: gameplayBefore === randomState,
        storageStable: arenaGenerator.storageStable()};
    },
    onlineGeneratedSnapshot(seed) {
      const accepted = generateArena(Number(seed) >>> 0);
      beginArena(GENERATED_ARENA_INDEX); sendOnlineSnapshot();
      return {accepted, seed: state.arenaSeed,
        checksum: arenaGenerationChecksum, mask: activeBarrierMask()};
    },
    emitOnlineSnapshot() { sendOnlineSnapshot(); },
    applyOnlinePacket(buffer) { return applyOnlineSnapshot(buffer); },
    setBarrierActive(index, active) {
      const barrier = barriers[index | 0];
      if (barrier && barrier.present) barrier.active = !!active;
    },
    elevationBarrierProbe() {
      const barrier = barriers[0], bullet = bullets[0];
      const previousActive = barrier.active;
      const previousX = barrier.x, previousZ = barrier.z;
      const previousWidth = barrier.width, previousDepth = barrier.depth;
      const previousOverCover = bullet.overCover;
      barrier.active = true; barrier.x = 0; barrier.z = 0;
      barrier.width = 2; barrier.depth = .4;
      barrier.left = -1; barrier.right = 1;
      barrier.top = -.2; barrier.bottom = .2;
      bullet.overCover = false;
      const groundBlocked = (bullet.overCover ? null : barrierAt(0, 0, .1))
        === barrier;
      bullet.overCover = true;
      const elevatedBlocked = (bullet.overCover ? null : barrierAt(0, 0, .1))
        === barrier;
      const obstacle = ARENAS[state.arena].obstacles[0];
      const obstacleBlocked = bulletHitsObstacle(obstacle[0], obstacle[1]);
      bullet.overCover = previousOverCover;
      barrier.active = previousActive;
      barrier.x = previousX; barrier.z = previousZ;
      barrier.width = previousWidth; barrier.depth = previousDepth;
      if (previousActive) {
        barrier.left = previousX - previousWidth * .5;
        barrier.right = previousX + previousWidth * .5;
        barrier.top = previousZ - previousDepth * .5;
        barrier.bottom = previousZ + previousDepth * .5;
      }
      return groundBlocked && !elevatedBlocked && obstacleBlocked;
    },
    snapshot() {
      const player = tanks[0];
      let bulletCount = 0, particleCount = 0, pickupCount = 0;
      let piercingBullets = 0, bypassBullets = 0, maxBulletDamage = 0;
      let mineCount = 0, smokeCount = 0, barrierCount = 0, decalCount = 0;
      let blueTanks = 0, redTanks = 0;
      for (const bullet of bullets) if (bullet.active) {
        bulletCount++;
        if (bullet.pierce > 0) piercingBullets++;
        if (bullet.bypassShield) bypassBullets++;
        maxBulletDamage = Math.max(maxBulletDamage, bullet.damage);
      }
      for (const particle of particles) if (particle.active) particleCount++;
      for (const pickup of pickups) if (pickup.active) pickupCount++;
      for (const mine of mines) if (mine.active) mineCount++;
      for (const smoke of smokeClouds) if (smoke.active) smokeCount++;
      for (const decal of decals) if (decal.active) decalCount++;
      for (const barrier of barriers) if (barrier.active) barrierCount++;
      for (const tank of tanks) if (tank.active) {
        if (tank.team === 0) blueTanks++; else redTanks++;
      }
      return {
        mode: state.mode, gameMode: state.gameMode, arena: state.arena,
        score: state.score, wave: state.wave, multiplier: state.multiplier,
        bestWave: state.bestWave, bestScore: state.bestScore,
        simTime: state.time, wallTime: state.wallTime,
        audioRole: sounds.engineRole,
        killBeat: state.killBeat, pendingClear: state.pendingClear,
        botTelegraphs: state.botTelegraphs,
        botPlayerShots: state.botPlayerShots,
        botTelegraphViolations: state.botTelegraphViolations,
        botVolleyMinimum: state.botVolleyMinimum,
        lives: state.lives, enemies: activeEnemyCount(), frames: state.frames,
        playerX: player.x, playerZ: player.z, playerYaw: player.yaw,
        turret: player.turret, armor: player.health, maxArmor: player.maxHealth,
        classId: player.classId, className: CLASSES[player.classId].name,
        armorZone: state.armorZone, gadget: player.gadget,
        gadgetCooldown: player.gadgetCooldown, shield: player.shield,
        secondaryCooldown: player.secondaryCooldown,
        commandEnabled: preferences.command, commandMeter: state.commandMeter,
        commandBuff: player.commandBuff,
        repair: player.repair, boost: player.boost,
        bullets: bulletCount, piercingBullets, bypassBullets, maxBulletDamage,
        particles: particleCount, pickups: pickupCount,
        decals: decalCount, decalRecycles,
        mines: mineCount, smoke: smokeCount, barriers: barrierCount,
        barriersBroken: state.barriersBroken, ricochets: state.ricochets,
        blueTanks, redTanks, blueControl: state.blueControl,
        redControl: state.redControl, gateOpen: state.gateOpen,
        convoyActive: convoy.active, convoyProgress: convoy.progress,
        convoyHealth: convoy.health,
        difficulty: state.difficultyChoice, shots: state.shots, hits: state.hits,
        damageTaken: state.damageTaken, objectiveSeconds: state.objectiveTicks,
        controls: preferences.controls, assist: preferences.assist,
        cameraMode: preferences.camera, cameraYaw,
        cameraDistance, cameraSafeDistance, cameraSafetyUpdates,
        qualificationCameraSwitches,
        vertices: vertexCount, indices: indexCount, staticIndices: staticIndexCount,
        arenaPlaneMaxSpan,
        boxInstances: boxInstanceCount, tankBarrels: tankBarrelCount,
        renderedBulletInstances,
        instanceLimit: MAX_BOX_INSTANCES, instanceCapHitFrames,
        droppedDecalInstances, droppedParticleInstances,
        arenaMaximumStaticIndexCount,
        generatedStaticTailIndexLimit: GENERATED_STATIC_TAIL_INDEX_LIMIT,
        arenaSeed: state.arenaSeed, arenaGenerationAttempts,
        arenaGenerationFallback, arenaGenerationChecksum,
        arenaGenerationPhase,
        arenaGenerationAge: state.wallTime - arenaGenerationStartedAt,
        onlineArenaGeometryPending,
        hudCharacters: hudCharacterCount,
        hudPrimitives: (hudPublishedIndexCount
          + hudPublishedIndicatorIndexCount) / 6,
        hudTextPrimitives: hudPublishedIndexCount / 6,
        hudIndicatorPrimitives: hudPublishedIndicatorIndexCount / 6,
        objectiveIndicatorX: 160 + Math.sin(hudObjectiveAngle) * 116,
        objectiveIndicatorY: 90 - Math.cos(hudObjectiveAngle) * 57,
        hudTextUploads, hudIndicatorUploads, repeatedFrameCaptures,
        meshDrops, deadlineSceneReuses,
        inputSource: input.source, gamepadConnected: input.gamepadConnected,
        onlineActive: online.active, onlineRole: online.role,
        onlineInputSequence: online.receivedInputSequence,
        onlineInputValid: online.receivedInputValid,
        onlineSnapshotSequence: online.snapshotSequence,
        onlineSnapshotValid: online.snapshotValid,
        remotePlayerX: tanks[1].x, remotePlayerZ: tanks[1].z,
      };
    },
    stop() { state.running = false; },
  });
  Object.defineProperty(globalThis, "__treadlineDebug", {
    value: debug, configurable: false, writable: false,
  });
  }
  globalThis.pocSummary = "TREADLINE-READY";
  globalThis.__treadlineBootReady = true;
  const queuedDeploy = !!globalThis.__treadlineDeployQueued;
  globalThis.__treadlineDeployQueued = false;
  ui.play.removeAttribute("aria-busy");
  ui.play.textContent = "Deploy";
  loadPreferences();
  if (qualificationLongSoak) state.gameMode = qualificationGameMode;
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
  if (qualificationAutoStart) {
    if (resetGame()) setMode("playing");
    /* Attribute reflection is the standards path. The direct style is a
       validation-only belt-and-suspenders guard against measuring a stale
       pre-layout setup panel on an early deferred-script mutation. */
    if (qualificationLongSoak) ui.panel.style.display = "none";
  } else if (queuedDeploy) startOrResume();
  else render();
  if (qualificationLongSoak)
    globalThis.pocSummary = "TREADLINE-LONG-SOAK-ACTIVE";
  requestAnimationFrame(frame);
})();
