/* Built-in-material shaders in the shape a scene library emits (preprocessor
   switches, light-struct uniform arrays, helper functions, inout accumulator
   structs, SHADER_NAME markers), mapped onto GE material terms. The shaders
   are written for this test; each check names the GE term it covers. */
(() => {
  "use strict";
  const canvas = document.createElement("canvas");
  canvas.width = 16;
  canvas.height = 16;
  document.body.appendChild(canvas);
  const gl = canvas.getContext("webgl");
  const failures = [];
  const build = (vs, fs) => {
    const program = gl.createProgram();
    for (const [kind, source] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
      const shader = gl.createShader(kind);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      gl.attachShader(program, shader);
      gl.deleteShader(shader);
    }
    gl.linkProgram(program);
    return program;
  };
  const expectLink = (label, program, linked, patterns = []) => {
    const log = gl.getProgramInfoLog(program);
    const ok = gl.getProgramParameter(program, gl.LINK_STATUS) === linked
      && (patterns.length ? patterns.every((p) => p.test(log)) : log === "");
    if (!ok) failures.push(label + "=" + JSON.stringify(log));
    return ok;
  };

  const lightStructs = `
struct DirectionalLight { vec3 direction; vec3 color; };
struct PointLight { vec3 position; vec3 color; float distance; float decay; };`;
  const lambertVs = (name, directional) => `#define SHADER_NAME ${name}
#define DIRECTIONAL_COUNT ${directional}
precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform mat3 normalMatrix;
attribute vec3 position;
attribute vec3 normal;
${lightStructs}
uniform DirectionalLight directionalLights[DIRECTIONAL_COUNT];
uniform PointLight pointLights[1];
uniform vec3 ambientLightColor;
varying vec3 vLight;
vec3 ambientIrradiance(const in vec3 ambientLightColor) { return ambientLightColor; }
vec3 directIrradiance(const in DirectionalLight light, const in vec3 n) {
  return light.color * max(dot(n, light.direction), 0.0);
}
vec3 pointIrradiance(const in PointLight light, const in vec3 n, const in vec3 eye) {
  return light.color * max(dot(n, normalize(light.position - eye)), 0.0);
}
void main() {
  vec4 eye = modelViewMatrix * vec4(position, 1.0);
  vec3 n = normalize(normalMatrix * normal);
  vLight = ambientIrradiance(ambientLightColor);
#if DIRECTIONAL_COUNT > 4
  vLight += directIrradiance(directionalLights[4], n);
  vLight += directIrradiance(directionalLights[3], n);
  vLight += directIrradiance(directionalLights[2], n);
  vLight += directIrradiance(directionalLights[1], n);
#endif
  vLight += directIrradiance(directionalLights[0], n);
  vLight += pointIrradiance(pointLights[0], n, eye.xyz);
  gl_Position = projectionMatrix * eye;
}`;
  const lambertFs = `precision mediump float;
uniform vec3 diffuse;
uniform float opacity;
varying vec3 vLight;
void main() {
  vec4 diffuseColor = vec4(diffuse, opacity);
  gl_FragColor = vec4(vLight * diffuseColor.rgb, diffuseColor.a);
}`;
  const phongVs = `#define SHADER_NAME MeshPhongMaterial
precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform mat3 normalMatrix;
attribute vec3 position;
attribute vec3 normal;
varying vec3 vViewPosition;
varying vec3 vNormal;
varying float vFogDepth;
void main() {
  vec4 eye = modelViewMatrix * vec4(position, 1.0);
  vNormal = normalize(normalMatrix * normal);
  vViewPosition = -eye.xyz;
  vFogDepth = -eye.z;
  gl_Position = projectionMatrix * eye;
}`;
  const phongFs = (extraLight) => `precision mediump float;
#define USE_FOG
uniform vec3 diffuse;
uniform vec3 emissive;
uniform vec3 specular;
uniform float shininess;
uniform float opacity;
uniform vec3 ambientLightColor;
uniform vec3 fogColor;
uniform float fogNear;
uniform float fogFar;
${lightStructs}
uniform DirectionalLight directionalLights[1];
struct AreaLight { vec3 position; vec3 color; };
uniform AreaLight areaLights[1];
struct Surface { vec3 diffuseColor; vec3 specularColor; float specularShininess; };
struct Reflected { vec3 diffuse; vec3 specular; vec3 ambient; };
varying vec3 vViewPosition;
varying vec3 vNormal;
varying float vFogDepth;
void addDirect(const in DirectionalLight light, const in vec3 n, const in vec3 v,
    const in Surface surface, inout Reflected reflected) {
  float lambert = max(dot(n, light.direction), 0.0);
  vec3 h = normalize(light.direction + v);
  reflected.diffuse += light.color * lambert * surface.diffuseColor;
  reflected.specular += light.color * lambert * surface.specularColor
    * pow(max(dot(n, h), 0.0), surface.specularShininess);
}
void addArea(const in AreaLight light, const in vec3 n, inout Reflected reflected) {
  reflected.diffuse += light.color * max(dot(n, light.position), 0.0);
}
void addAmbient(const in vec3 irradiance, const in Surface surface,
    inout Reflected reflected) {
  reflected.ambient += irradiance * surface.diffuseColor;
}
void main() {
  vec3 n = normalize(vNormal);
  vec3 v = normalize(vViewPosition);
  Surface surface;
  surface.diffuseColor = diffuse;
  surface.specularColor = specular;
  surface.specularShininess = shininess;
  Reflected reflected = Reflected(vec3(0.0), vec3(0.0), vec3(0.0));
  addDirect(directionalLights[0], n, v, surface, reflected);
  ${extraLight ? "addArea(areaLights[0], n, reflected);" : ""}
  addAmbient(ambientLightColor, surface, reflected);
  vec3 outgoing = reflected.diffuse + reflected.specular + reflected.ambient + emissive;
  gl_FragColor = vec4(outgoing, opacity);
#ifdef USE_FOG
  float fogFactor = smoothstep(fogNear, fogFar, vFogDepth);
  gl_FragColor.rgb = mix(gl_FragColor.rgb, fogColor, fogFactor);
#endif
}`;
  const basicVs = `#define SHADER_NAME MeshBasicMaterial
precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform mat3 uvTransform;
attribute vec3 position;
attribute vec2 uv;
attribute vec3 color;
varying vec2 vUv;
varying vec3 vColor;
varying float vFogDepth;
void main() {
  vUv = (uvTransform * vec3(uv, 1.0)).xy;
  vColor = color;
  vec4 eye = modelViewMatrix * vec4(position, 1.0);
  vFogDepth = -eye.z;
  gl_Position = projectionMatrix * eye;
}`;
  const basicFs = `precision mediump float;
uniform vec3 diffuse;
uniform float opacity;
uniform sampler2D map;
uniform vec3 fogColor;
uniform float fogDensity;
varying vec2 vUv;
varying vec3 vColor;
varying float vFogDepth;
void main() {
  vec4 diffuseColor = vec4(diffuse, opacity);
  diffuseColor *= texture2D(map, vUv);
  diffuseColor.rgb *= vColor;
  gl_FragColor = diffuseColor;
  float fogFactor = 1.0 - exp(-fogDensity * fogDensity * vFogDepth * vFogDepth);
  gl_FragColor.rgb = mix(gl_FragColor.rgb, fogColor, fogFactor);
}`;
  const pointsVs = `#define SHADER_NAME PointsMaterial
precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform float size;
uniform float scale;
attribute vec3 position;
void main() {
  vec4 eye = modelViewMatrix * vec4(position, 1.0);
  gl_PointSize = size * (scale / -eye.z);
  gl_Position = projectionMatrix * eye;
}`;
  const pointsFs = `precision mediump float;
uniform vec3 diffuse;
uniform float opacity;
uniform sampler2D map;
uniform mat3 uvTransform;
uniform float alphaTest;
void main() {
  vec2 uv = (uvTransform * vec3(gl_PointCoord.x, 1.0 - gl_PointCoord.y, 1.0)).xy;
  vec4 color = vec4(diffuse, opacity) * texture2D(map, uv);
  if (color.a < alphaTest) discard;
  gl_FragColor = color;
}`;

  /* Recognition: exact, approximated (warned) and refused programs. */
  const lambert = build(lambertVs("MeshLambertMaterial", 1), lambertFs);
  expectLink("lambert", lambert, true);
  const phong = build(phongVs, phongFs(false));
  expectLink("phong", phong, true, [/^WARNING: /,
    /smoothstep fog is drawn as the GE's linear fog/,
    /per-pixel lighting is evaluated per vertex on the PSP/]);
  const basic = build(basicVs, basicFs);
  expectLink("basic", basic, true,
    [/exponential fog is approximated by the GE's linear fog/]);
  const points = build(pointsVs, pointsFs);
  expectLink("points", points, true);
  const many = build(lambertVs("MeshLambertMaterial", 5), lambertFs);
  expectLink("many-lights", many, true, [/6 lights; the GE applies the first 4/]);
  const area = build(phongVs, phongFs(true));
  expectLink("area-light", area, false, [/^ERROR: /, /AreaLight has no GE equivalent/]);
  const unlisted = build(lambertVs("CustomMaterial", 1), lambertFs);
  expectLink("unlisted-lit", unlisted, false,
    [/lighting functions outside a known lit material/]);
  for (const done of [many, area, unlisted]) gl.deleteProgram(done);

  /* One full-viewport triangle facing the camera at eye depth 0.5. */
  const buffer = (data) => {
    const made = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, made);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.STATIC_DRAW);
    return made;
  };
  const positions = buffer([-1, -1, -0.5, 3, -1, -0.5, -1, 3, -0.5]);
  const normals = buffer([0, 0, 1, 0, 0, 1, 0, 0, 1]);
  const uvs = buffer([0.1, 0.5, 0.1, 0.5, 0.1, 0.5]);
  const colors = buffer([1, 1, 0.5, 1, 1, 0.5, 1, 1, 0.5]);
  const identity4 = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
  const identity3 = new Float32Array([1,0,0, 0,1,0, 0,0,1]);
  const enabled = [];
  const attribute = (program, name, source, size) => {
    const at = gl.getAttribLocation(program, name);
    if (at < 0) { failures.push("attribute " + name); return; }
    gl.bindBuffer(gl.ARRAY_BUFFER, source);
    gl.vertexAttribPointer(at, size, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(at);
    enabled.push(at);
  };
  const uniform = (program, name, value) => {
    const at = gl.getUniformLocation(program, name);
    if (!at) { failures.push("uniform " + name); return; }
    if (typeof value === "number") gl.uniform1f(at, value);
    else if (value.length === 16) gl.uniformMatrix4fv(at, false, value);
    else if (value.length === 9) gl.uniformMatrix3fv(at, false, value);
    else gl.uniform3fv(at, value);
  };
  const pixel = new Uint8Array(4);
  const drawAndRead = (program, uniforms, mode = gl.TRIANGLES, count = 3,
      x = 8, y = 8) => {
    gl.useProgram(program);
    uniform(program, "modelViewMatrix", identity4);
    uniform(program, "projectionMatrix", identity4);
    for (const [name, value] of Object.entries(uniforms)) uniform(program, name, value);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(mode, 0, count);
    gl.finish();
    gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
    const error = gl.getError();
    if (error) failures.push("gl-error=" + error);
    return Array.from(pixel.subarray(0, 3));
  };
  const near = (label, got, want, tolerance = 6) => {
    if (got.some((v, k) => Math.abs(v - want[k]) > tolerance))
      failures.push(label + "=" + got.join(",") + "/" + want.join(","));
  };
  const release = () => { while (enabled.length) gl.disableVertexAttribArray(enabled.pop()); };

  /* Lambert: ambient + directional N.L, base colour from `diffuse`. */
  attribute(lambert, "position", positions, 3);
  attribute(lambert, "normal", normals, 3);
  const unlit = {
    diffuse: [1, 0.5, 0], opacity: 1, ambientLightColor: [0.25, 0.25, 0.25],
    "directionalLights[0].direction": [0, 0, 1],
    "directionalLights[0].color": [0.5, 0.5, 0.5],
    "pointLights[0].position": [0, 0, 10], "pointLights[0].color": [0, 0, 0],
    "pointLights[0].distance": 0, "pointLights[0].decay": 1
  };
  near("lambert-directional", drawAndRead(lambert, unlit), [191, 96, 0]);
  /* Point light far ahead: N.L ~0.96-0.99 at the vertices, no range. */
  const pointOnly = { ...unlit, diffuse: [1, 0, 0],
    ambientLightColor: [0, 0, 0], "directionalLights[0].color": [0, 0, 0],
    "pointLights[0].color": [1, 1, 1] };
  near("lambert-point", drawAndRead(lambert, pointOnly), [248, 0, 0], 10);
  /* The same light with a 20-unit range: three's (1 - d/20) ~0.47 at
     d ~10.5, drawn as GE 1/(1 + 0.1 d). */
  near("lambert-point-range", drawAndRead(lambert,
    { ...pointOnly, "pointLights[0].distance": 20 }), [118, 0, 0], 16);
  release();

  /* Phong: emissive, normalized Blinn-Phong specular, linear fog. */
  attribute(phong, "position", positions, 3);
  attribute(phong, "normal", normals, 3);
  const phongBase = {
    diffuse: [0, 0, 0], emissive: [0.25, 0, 0], specular: [0, 0, 0],
    shininess: 2, opacity: 1, ambientLightColor: [0, 0, 0],
    "directionalLights[0].direction": [0, 0, 1],
    "directionalLights[0].color": [1, 1, 1],
    fogColor: [0, 1, 0], fogNear: 10, fogFar: 20
  };
  near("phong-emissive", drawAndRead(phong, phongBase), [64, 0, 0]);
  /* S * (n/2 + 1) / 4 = 0.2 * 2 / 4 = 0.1 at N.H = 1. */
  near("phong-specular", drawAndRead(phong,
    { ...phongBase, emissive: [0, 0, 0], specular: [0.2, 0.2, 0.2] }), [26, 26, 26]);
  /* Eye depth 0.5 halfway between fogNear 0 and fogFar 1. */
  near("phong-fog", drawAndRead(phong,
    { ...phongBase, fogNear: 0, fogFar: 1 }), [32, 128, 0]);
  near("phong-emissive-again", drawAndRead(phong, phongBase), [64, 0, 0]);
  near("phong-specular-again", drawAndRead(phong,
    { ...phongBase, emissive: [0, 0, 0], specular: [0.2, 0.2, 0.2] }), [26, 26, 26]);
  release();

  /* Basic: map through uvTransform, vertex colour, diffuse. The texture's
     left texel is red and its right texel blue; uv u=0.1 samples red until
     uvTransform maps it to u = 0.5 * 0.1 + 0.5. */
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
    new Uint8Array([255, 0, 0, 255, 0, 0, 255, 255]));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  attribute(basic, "position", positions, 3);
  attribute(basic, "uv", uvs, 2);
  attribute(basic, "color", colors, 3);
  const basicBase = { diffuse: [1, 1, 1], opacity: 1, uvTransform: identity3,
    fogColor: [0, 1, 0], fogDensity: 0 };
  gl.useProgram(basic);
  gl.uniform1i(gl.getUniformLocation(basic, "map"), 0);
  near("basic-map", drawAndRead(basic, basicBase), [255, 0, 0]);
  near("basic-uv-transform", drawAndRead(basic, { ...basicBase,
    uvTransform: new Float32Array([0.5, 0, 0, 0, 1, 0, 0.5, 0, 1]) }), [0, 0, 128]);
  near("basic-map-again", drawAndRead(basic, basicBase), [255, 0, 0]);
  /* The sampler set to the unit it already names keeps the record. */
  gl.uniform1i(gl.getUniformLocation(basic, "map"), 0);
  if (!basic._materialCache.valid) failures.push("basic-sampler-same-value");
  release();

  /* Points: attenuated size (4 * 1 / depth 0.5 = 8 px), white map,
     alpha test discards below the reference. */
  const white = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, white);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
    new Uint8Array([255, 255, 255, 255]));
  const center = buffer([0, 0, -0.5]);
  attribute(points, "position", center, 3);
  gl.useProgram(points);
  gl.uniform1i(gl.getUniformLocation(points, "map"), 0);
  const pointBase = { diffuse: [0, 1, 0], opacity: 1, size: 4, scale: 1,
    uvTransform: identity3, alphaTest: 0.5 };
  near("points-center", drawAndRead(points, pointBase, gl.POINTS, 1, 8, 8), [0, 255, 0]);
  near("points-edge", drawAndRead(points, pointBase, gl.POINTS, 1, 11, 8), [0, 255, 0]);
  near("points-outside", drawAndRead(points, pointBase, gl.POINTS, 1, 14, 8), [0, 0, 0]);
  near("points-unattenuated", drawAndRead(points,
    { ...pointBase, scale: 0.25 }, gl.POINTS, 1, 11, 8), [0, 0, 0]);
  near("points-alpha-test", drawAndRead(points,
    { ...pointBase, opacity: 0.4 }, gl.POINTS, 1, 8, 8), [0, 0, 0]);
  near("points-alpha-reference", drawAndRead(points,
    { ...pointBase, opacity: 0.4, alphaTest: 0.25 }, gl.POINTS, 1, 8, 8),
    [0, 255, 0]);
  near("points-center-again", drawAndRead(points, pointBase, gl.POINTS, 1, 8, 8),
    [0, 255, 0]);
  release();

  /* A point textured from a vertex attribute (not gl_PointCoord) takes
     the one texel at its own uv: the red/blue map's right texel here. */
  const uvPointsVs = `precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
attribute vec3 position;
attribute vec2 uv;
varying vec2 vUv;
void main() {
  vUv = uv;
  gl_PointSize = 4.0;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;
  const uvPointsFs = `precision mediump float;
uniform vec3 diffuse;
uniform sampler2D map;
varying vec2 vUv;
void main() { gl_FragColor = vec4(diffuse, 1.0) * texture2D(map, vUv); }`;
  const uvPoints = build(uvPointsVs, uvPointsFs);
  if (expectLink("points-uv", uvPoints, true)) {
    gl.bindTexture(gl.TEXTURE_2D, texture);
    attribute(uvPoints, "position", center, 3);
    const pointUv = buffer([0.75, 0.5]);
    attribute(uvPoints, "uv", pointUv, 2);
    gl.useProgram(uvPoints);
    gl.uniform1i(gl.getUniformLocation(uvPoints, "map"), 0);
    near("points-uv-attribute", drawAndRead(uvPoints, { diffuse: [1, 1, 1] },
      gl.POINTS, 1, 8, 8), [0, 0, 255]);
    release();
    gl.deleteBuffer(pointUv);
    gl.bindTexture(gl.TEXTURE_2D, white);
  }
  gl.deleteProgram(uvPoints);

  /* A per-draw material term that reads a position matrix follows it:
     the material record is kept between draws, but this point size is
     4 * projectionMatrix[1][1] (4 px, then 8 px once that entry is 2). */
  const projectedVs = `precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
attribute vec3 position;
void main() {
  gl_PointSize = 4.0 * projectionMatrix[1][1];
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;
  const solidFs = `precision mediump float;
uniform vec3 diffuse;
void main() { gl_FragColor = vec4(diffuse, 1.0); }`;
  const projected = build(projectedVs, solidFs);
  if (expectLink("points-projected", projected, true)) {
    attribute(projected, "position", center, 3);
    const tall = new Float32Array(identity4);
    tall[5] = 2;
    /* Only the matrices change between these draws, so nothing else
       invalidates the kept record. */
    gl.useProgram(projected);
    uniform(projected, "diffuse", [0, 0, 1]);
    near("points-projected-4", drawAndRead(projected, {}, gl.POINTS, 1, 11, 8),
      [0, 0, 0]);
    near("points-projected-4-center", drawAndRead(projected, {}, gl.POINTS, 1,
      8, 8), [0, 0, 255]);
    near("points-projected-8", drawAndRead(projected,
      { projectionMatrix: tall }, gl.POINTS, 1, 11, 8), [0, 0, 255]);
    near("points-projected-4-again", drawAndRead(projected, {}, gl.POINTS, 1,
      11, 8), [0, 0, 0]);
    /* The attenuated points program reads its matrices only through eye
       depth, so a draw whose matrices alone changed keeps its record. */
    attribute(points, "position", center, 3);
    gl.useProgram(points);
    for (const [name, value] of Object.entries(pointBase))
      uniform(points, name, value);
    const kept = { hit: 0, built: 0 }, prepare = gl._prepareMaterial;
    gl._prepareMaterial = function (program, plan) {
      if (program === points) kept[program._materialCache.valid ? "hit" : "built"]++;
      return prepare.call(this, program, plan);
    };
    for (let at = 0; at < 4; at++) {
      const moved = new Float32Array(identity4);
      moved[12] = at * 0.01;
      uniform(points, "modelViewMatrix", moved);
      uniform(points, "projectionMatrix", at % 2 ? tall : identity4);
      gl.drawArrays(gl.POINTS, 0, 1);
      gl.finish();
    }
    gl._prepareMaterial = prepare;
    if (kept.built > 1 || kept.hit < 3)
      failures.push("points-kept=" + kept.hit + "/" + kept.built);
    release();
  }
  gl.deleteProgram(projected);

  /* A lit program shared by two materials keeps a record for each value
     set: the materials alternate, every uniform is set again before each
     draw (as three.js re-uploads its light arrays) and the object moves,
     so only two records are built. Each changed input is still seen. */
  const litKept = build(lambertVs("MeshLambertMaterial", 1), lambertFs);
  if (expectLink("lit-kept", litKept, true)) {
    attribute(litKept, "position", positions, 3);
    attribute(litKept, "normal", normals, 3);
    const counted = { built: 0 }, buildMaterial = gl._buildMaterial;
    gl._buildMaterial = function (program, plan) {
      if (program === litKept) counted.built++;
      return buildMaterial.call(this, program, plan);
    };
    const red = { ...unlit, diffuse: [1, 0, 0] },
      blue = { ...unlit, diffuse: [0, 0, 1] };
    for (let at = 0; at < 8; at++) {
      const moved = new Float32Array(identity4);
      moved[12] = at * 0.01;
      near("lit-kept-" + at, drawAndRead(litKept,
        { ...(at % 2 ? blue : red), modelViewMatrix: moved }),
        at % 2 ? [0, 0, 191] : [191, 0, 0]);
    }
    if (counted.built !== 2) failures.push("lit-kept-built=" + counted.built);
    const shifted = new Float32Array(identity4);
    shifted[12] = 2.5;
    /* ambient 0.25 plus N.L times the light colour, times diffuse */
    for (const [label, change, want] of [
      ["light-colour", { "directionalLights[0].color": [0.25, 0.25, 0.25] },
        [128, 0, 0]],
      ["light-direction", { "directionalLights[0].direction": [0, 0, -1] },
        [64, 0, 0]],
      ["ambient", { ambientLightColor: [0.5, 0.5, 0.5] }, [255, 0, 0]],
      ["material-colour", { diffuse: [0, 1, 0] }, [0, 191, 0]],
      ["object-transform", { modelViewMatrix: shifted }, [0, 0, 0]],
      ["camera-move", { projectionMatrix: shifted }, [0, 0, 0]],
      ["unchanged", {}, [191, 0, 0]]])
      near("lit-" + label, drawAndRead(litKept, { ...red, ...change }), want);
    /* A point light alone, ahead of the surface and then behind it. */
    const lamp = { ...red, ambientLightColor: [0, 0, 0],
      "directionalLights[0].color": [0, 0, 0],
      "pointLights[0].color": [0.5, 0.5, 0.5] };
    near("lit-point-ahead", drawAndRead(litKept, lamp), [124, 0, 0], 10);
    near("lit-point-behind", drawAndRead(litKept,
      { ...lamp, "pointLights[0].position": [0, 0, -10] }), [0, 0, 0]);
    near("lit-point-ahead-again", drawAndRead(litKept, lamp), [124, 0, 0], 10);
    /* Setting an input to the value it holds keeps the current record;
       another value, even -0 for 0, drops it. */
    drawAndRead(litKept, red);
    const valid = () => litKept._materialCache.valid;
    const kept = [];
    uniform(litKept, "ambientLightColor", [0.25, 0.25, 0.25]); kept.push(valid());
    uniform(litKept, "pointLights[0].distance", 0); kept.push(valid());
    gl.uniform3f(gl.getUniformLocation(litKept, "diffuse"), 1, 0, 0);
    kept.push(valid());
    uniform(litKept, "pointLights[0].distance", -0); kept.push(valid());
    drawAndRead(litKept, red);
    gl.uniform3f(gl.getUniformLocation(litKept, "diffuse"), 1, 0, 0.5);
    kept.push(valid());
    if (kept.join() !== "true,true,true,false,false")
      failures.push("lit-same-value=" + kept.join());
    gl._buildMaterial = buildMaterial;
    const stats = globalThis.__tilefinchWebGLDiagnostics,
      builds = () => stats.materialRecordBuilds;
    /* Two programs of one material type alternate, each with its own
       colour, and keep their own records. */
    let before = builds();
    for (let at = 0; at < 6; at++) {
      const other = at % 2 === 1;
      near("lit-two-programs-" + at, drawAndRead(other ? lambert : litKept,
        { ...unlit, diffuse: other ? [0, 1, 0] : [1, 0, 0] }),
        other ? [0, 191, 0] : [191, 0, 0]);
    }
    if (builds() - before > 2)
      failures.push("lit-two-programs-built=" + (builds() - before));
    /* Linking again drops the kept records. */
    drawAndRead(litKept, red);
    before = builds();
    gl.linkProgram(litKept);
    near("lit-relinked", drawAndRead(litKept, red), [191, 0, 0]);
    if (builds() - before !== 1)
      failures.push("lit-relinked-built=" + (builds() - before));
    /* More value sets (20) than kept records (16): each draw still has
       its own colour, 0.75 of its diffuse. */
    const hues = Array.from({ length: 20 }, (_, k) => [k / 19, 1 - k / 19, 0.5]);
    for (let pass = 0; pass < 2; pass++)
      hues.forEach((diffuse, k) => near("lit-cycle-" + pass + "-" + k,
        drawAndRead(litKept, { ...unlit, diffuse }),
        diffuse.map((v) => Math.round(v * 0.75 * 255))));
    /* After more misses than reuses a program looks for a kept record
       less and less often, down to every 64th draw; after 130 more value
       sets, each drawn once, a value set that then stays is kept again
       within 64 draws. */
    for (let k = 0; k < 130; k++) {
      const diffuse = [(k % 13) / 12, ((k * 7) % 10) / 9, 0.25];
      near("lit-streak-" + k, drawAndRead(litKept, { ...unlit, diffuse }),
        diffuse.map((v) => Math.round(v * 0.75 * 255)));
    }
    let rearmedAfter = 0;
    while (!litKept._materialCache.valid && rearmedAfter < 64) {
      near("lit-rearming", drawAndRead(litKept, red), [191, 0, 0]);
      rearmedAfter++;
    }
    if (!litKept._materialCache.valid) failures.push("lit-rearmed");
    before = builds();
    for (let at = 0; at < 4; at++) drawAndRead(litKept, red);
    if (builds() !== before) failures.push("lit-rearmed-built=" + (builds() - before));
    /* Keys compare exact values: -0 is not 0. */
    before = builds();
    drawAndRead(litKept, { ...red, ambientLightColor: [0, 0, 0] });
    drawAndRead(litKept, { ...red, ambientLightColor: [-0, 0, 0] });
    if (builds() - before !== 2)
      failures.push("lit-negative-zero-built=" + (builds() - before));
    release();
  }
  gl.deleteProgram(litKept);

  /* A light-probe array (three.js re-uploads it before every draw): set
     whole, through an element location and in its last element only. */
  const probeVs = lambertVs("MeshLambertMaterial", 1)
    .replace("uniform vec3 ambientLightColor;", `uniform vec3 ambientLightColor;
uniform vec3 lightProbe[9];
vec3 getLightProbeIrradiance(const in vec3 lightProbe[9], const in vec3 n) {
  return lightProbe[0] * 0.886227;
}`).replace("  vLight += directIrradiance(directionalLights[0], n);",
    `  vLight += directIrradiance(directionalLights[0], n);
  vLight += getLightProbeIrradiance(lightProbe, n);`);
  const probe = build(probeVs, lambertFs);
  if (expectLink("probe", probe, true)) {
    attribute(probe, "position", positions, 3);
    attribute(probe, "normal", normals, 3);
    const stats = globalThis.__tilefinchWebGLDiagnostics,
      builds = () => stats.materialRecordBuilds,
      valid = () => probe._materialCache.valid;
    const dark = new Float32Array(27), lit = new Float32Array(27);
    lit[0] = lit[1] = lit[2] = 1;
    const base = { ...unlit, diffuse: [1, 0, 0], ambientLightColor: [0, 0, 0] };
    const draw = (label, values, want) => {
      gl.useProgram(probe);
      gl.uniform3fv(gl.getUniformLocation(probe, "lightProbe"), values);
      near(label, drawAndRead(probe, base), want);
    };
    /* directional 0.5, plus band 0 over three's PI convention: 0.282 */
    draw("probe-dark", dark, [128, 0, 0]);
    draw("probe-lit", lit, [199, 0, 0]);
    draw("probe-dark-again", dark, [128, 0, 0]);
    const flags = [];
    gl.uniform3fv(gl.getUniformLocation(probe, "lightProbe"), dark);
    flags.push(valid());
    gl.uniform3fv(gl.getUniformLocation(probe, "lightProbe[3]"), [0, 0, 0]);
    flags.push(valid());
    gl.uniform3fv(gl.getUniformLocation(probe, "lightProbe[3]"), [0, 0, 0.5]);
    flags.push(valid());
    if (flags.join() !== "true,true,false")
      failures.push("probe-same-value=" + flags.join());
    /* Only the last element differs: a new record, the same picture. */
    const late = new Float32Array(27);
    late[26] = 0.5;
    draw("probe-dark-3", dark, [128, 0, 0]);
    let before = builds();
    draw("probe-late", late, [128, 0, 0]);
    if (builds() - before !== 1)
      failures.push("probe-late-built=" + (builds() - before));
    before = builds();
    draw("probe-element", (() => { const v = new Float32Array(dark);
      v[9] = 0.5; return v; })(), [128, 0, 0]);
    draw("probe-lit-again", lit, [199, 0, 0]);
    if (builds() - before !== 1)
      failures.push("probe-element-built=" + (builds() - before));
    release();
  }
  gl.deleteProgram(probe);

  /* Per-vertex shading: a colour term of attributes and uniforms read
     through varyings is evaluated per vertex and interpolated (linked with
     a warning); past the per-draw step budget the draw is refused. */
  const shadedVs = `precision highp float;
uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
attribute vec3 position;
attribute vec2 uv;
varying vec2 vUv;
varying vec3 vPos;
void main() {
  vUv = uv;
  vPos = position;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;
  const waveFs = `precision mediump float;
uniform float uTime;
uniform vec3 uTint;
varying vec2 vUv;
void main() {
  vec3 wave = 0.5 + 0.5 * cos(uTime + vUv.xyx * 3.0 + vec3(0.0, 2.0, 4.0));
  gl_FragColor = vec4(uTint * wave, 1.0);
}`;
  const rampFs = `precision mediump float;
varying vec3 vPos;
void main() { gl_FragColor = vec4(vec3(vPos.x * 0.25 + 0.25), 1.0); }`;
  const heavyFs = `precision mediump float;
varying vec2 vUv;
void main() {
  float a = sin(vUv.x * 2.0) * cos(vUv.y * 3.0) + sin(vUv.y * 5.0) * cos(vUv.x * 7.0)
    + sin(vUv.x * 11.0) * cos(vUv.y * 13.0) + sin(vUv.y * 17.0) * cos(vUv.x * 19.0);
  gl_FragColor = vec4(vec3(a), 1.0);
}`;
  const shaded = /is shaded per vertex on the PSP/;
  const wave = build(shadedVs, waveFs);
  const ramp = build(shadedVs, rampFs);
  const heavy = build(shadedVs, heavyFs);
  expectLink("shaded-wave", wave, true, [/^WARNING: /, shaded]);
  expectLink("shaded-ramp", ramp, true, [shaded]);
  expectLink("shaded-heavy", heavy, true, [shaded]);
  /* uv (0.1, 0.5) at every vertex: 0.5 + 0.5 cos(0.3, 3.5, 4.3). */
  attribute(wave, "position", positions, 3);
  attribute(wave, "uv", uvs, 2);
  near("shaded-wave", drawAndRead(wave, { uTime: 0, uTint: [1, 1, 1] }),
    [249, 8, 76]);
  near("shaded-wave-tint", drawAndRead(wave, { uTime: 0, uTint: [0, 1, 0.5] }),
    [0, 8, 38]);
  /* The first value set again: its kept record, vertex program included. */
  near("shaded-wave-again", drawAndRead(wave, { uTime: 0, uTint: [1, 1, 1] }),
    [249, 8, 76]);
  /* uTime pi: 0.5 + 0.5 cos(3.44, 6.64, 7.44) */
  near("shaded-wave-time", drawAndRead(wave, { uTime: Math.PI, uTint: [1, 1, 1] }),
    [6, 247, 179]);
  release();
  /* Linear in position, so per vertex equals per pixel: x = 0.0625 at the
     sampled pixel gives 0.265625. */
  attribute(ramp, "position", positions, 3);
  near("shaded-ramp", drawAndRead(ramp, {}), [68, 68, 68]);
  release();
  /* 4096 vertices of a program past 16 steps exceed 65536 steps per draw. */
  const bulk = buffer(new Array(4096 * 3).fill(0));
  const bulkUv = buffer(new Array(4096 * 2).fill(0));
  attribute(heavy, "position", positions, 3);
  attribute(heavy, "uv", uvs, 2);
  drawAndRead(heavy, {});
  release();
  attribute(heavy, "position", bulk, 3);
  attribute(heavy, "uv", bulkUv, 2);
  gl.drawArrays(gl.TRIANGLES, 0, 4096);
  const refused = gl.getError();
  if (refused !== gl.OUT_OF_MEMORY) failures.push("shaded-step-limit=" + refused);
  release();
  for (const done of [wave, ramp, heavy]) gl.deleteProgram(done);

  globalThis.pocSummary = failures.length
    ? "WEBGL-MATERIALS-FAIL:" + failures.join(" | ")
    : "WEBGL-MATERIALS-PASS";
})();
