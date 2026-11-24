# Time Machine's browser (Backup.framework 1.1, 10.6): what was recovered

What "Enter Time Machine" shows (the star field and galaxy, the desktop sliding away, the stack of
windows receding in time, the flights between snapshots, the timeline, the toolbar) is drawn by the
private Backup.framework, loaded into the Finder (the host). Nothing is ported yet: every Status below
is "Read". No Apple file is in the repository: the port reads the framework's Resources from the DVD at
run time like the rest of the project (`tools/extract.py`); the shaders and pictures are described
here, not copied.

## Where the files come from

| File | Source | Notes |
|---|---|---|
| `Backup.x86_64` (633 808 bytes) | the x86_64 slice of `/System/Library/PrivateFrameworks/Backup.framework/Versions/A/Backup` (universal x86_64, i386, ppc7400; `__TEXT` in the clear), cut with `llvm-lipo -thin x86_64` | 3 077 functions, 2 440 with a body; 26 Objective-C classes, 390 methods; C++ with RTTI (37 vtables recovered) |
| `Resources/*.png`, `*.tiff` | the framework's bundle | stars, sun, cloud, the static galaxy, the toolbar and timeline art (sizes below) |
| `Resources/RemapVortex.*`, `Twirl.*`, `VortexCache.*` | GLSL 1.10 (`varying vec2 TexCoord`, fixed-function matrices) | only RemapVortex is loaded at run time (section 4) |
| `English.lproj/Localizable.strings` | UTF-16 | `BU*` keys (the browser), `U*` keys (sizes, relative dates, durations) |

Scratch material, rebuilt by hand, lives in `.local/media/re/backup/` (ignored by git): `ckpt/` (the
Ghidra checkpoint), `dec/functions/0x<addr>.c` and `dec/decompile-index.json` (REA `decompile-all`,
the same parameters as `mx-TimeMachine`), `dec/named/` (the same files named `<Class>__<sel>@<addr>.c`,
the GL dispatch slots renamed `GL_<entry>`), `objc.txt` (`llvm-objdump --objc-meta-data`),
`methods.tsv`, `ivars.tsv`, `ivaraddr.tsv` (ivar offset variables), `vtables.tsv`, `gldispatch.txt`
(the 760 names of `__GLIFunctionDispatchRec`, from the ivar type string), `strings.txt`, `bind.txt`,
`full.dis`, and the readers `objcmap.py`, `clean.py`, `vtables.py`, `dis.py` (disassembly with GL
entry names, constant-pool values, selectors, ivars), `show.sh`, `h2d.py`, all run with `python3 -I`.
`art/` holds PNG conversions of the stars, sun and galaxy for viewing.

Conventions: "view" coordinates are Cocoa points, y up, origin at the bottom left of the main
display; a "slot" is a position in the stack (0 = front). Durations are the defaults; every one is
multiplied by **6 while Shift is held** (`FXEnableSlowAnimation`, `FUN_0003dbbd`: bit 9 of
`GetCurrentKeyModifiers`). All `_FX*`/`FX*` defaults are read from the **com.apple.finder** domain
(`FUN_0001bb12`).

## 1. The classes and the overall flow

| What | Where | Status |
|---|---|---|
| Objective-C classes: `TBackupView` (NSOpenGLView, the whole scene, 111 methods), `TBackupWindow` (borderless NSWindow), `TBackupLayer` (CALayer of one snapshot; ivars `fTarget` (a 0x50-byte `TTimeMachineTarget`), `fIndex`), `TBackupCompletionDelegate` (CAAnimation delegate: kinds 0 to 5), `MPStarField` (the star field and galaxy), `VortexGL` (the gas clouds and their twirl), `GVInterpolator` (a Quartz Composer Interpolation patch), `TMFloatParameterController` (a value eased towards a target), the controls `TMControl` → `TMPushButton` → `TMSkinnedPushButton` (the arrows), `TMToolbarPushButton` (Cancel, Restore, Restore All) → `TMDateText` (the date), `TMToolbarCheckbox`, `TMTimeline`; `BUCocoaWindowController`, `BURunSoonHelper`; `TMStarfieldLayer` (a CAOpenGLLayer variant of the background) and `TBackupBackgroundWindow` are dead code (never instantiated) | `objc.txt`, `methods.tsv` | Read |
| C++ classes (RTTI): `TBackup` (the session, vtable 0x80d00), `TTimelineControl` (0x80860) and its items `TTimelineItem` (0x80700) → `TTimelineLabelItem` (0x80020), `TTimelineMonthItem` (0x80180), `TTimelineCurrentItem` (0x7fec0), `TTimelineSnapshotItem` (0x802e0), `TTimelinePlaceHolderItem` (0x80440), `TTimelineItemCluster` (0x805a0); `TNaturalCubicSplineInterpolator` (0x7fa60), `TParametricInterpolator`; `TSnapshotValidator`, `TSnapshotsRootObserver`, `TWorkerThread*`, `TCarbonEventTimer`, `TCocoaWindowController` | `vtables.tsv` | Read |
| Entry: the menu extra or the Dock posts the distributed notification `com.apple.peabody.show` (`__BUUserInvokedTimeMachine` 0x553c5); the host calls `BUStartTimeMachine` (0x55901 → `TBackup::StartTimeMachine` 0x51098: trace "TBackup::StartTimeMachine inWindow: %d", the window number kept, a handshake with backupd's side: observe `com.apple.backup.BackupHandshakeNotification`, post `com.apple.backup.BackupRequestHandshakeNotification`) | 0x51098, 0x1b6a0, 0x1b60c | Read |
| The scene is built (`FUN_00029c9c`): `CoreDockDisableExposeKeysIfNecessary(1)`; a `TBackupWindow` the size of the main display (`CGDisplayBounds(CGMainDisplayID)`), style 0, buffered, black background, alpha 1, opaque, no shadow; its CGS tags \|= 0x18000; a `TBackupView` of the same frame as content view; `setTargets:` (the snapshots), `setIsFinderClient:`, `setRevealImages:window:reveal:` (the live window's centre, its picture, the desktop's picture); `FUN_000298de`: `setMaxLayerCount:bounds:` (the front window's size), one `addLayer:forTarget:` per snapshot, newest first | 0x29c9c, 0x307bc, 0x298de | Read |
| The reveal (`FUN_0004dc96`): the other displays fade to black (section 7), `prepareToReveal:YES`, the host window moved by `CGSMoveWindow` to `topBounds` (slot 0, the front of the stack), `SetSystemUIMode(kUIModeAllHidden, 0x48)` (unless `_FXDebugTMWithOneMonitor`), `showHideRevealAnimation:YES` | 0x4dc96, 0x28988, 0x339eb | Read |
| The front of the stack is **the host's real window** (the Finder browsing the snapshot's folder), placed over slot 0; the snapshot layer of slot 0 is kept at opacity 0 (`hideTopSnapshotNow`), the layers behind are pictures of the same window in older snapshots, asked from the host (`requestSnapshotImageFor:` → `__BURequestSnapshotImage`) | 0x2fa32, 0x33714, 0x55e4d | Read |
| Exit: Cancel, or a restore; `stabilizeForExitWithNowImage:` (the window layer shown over slot 0 with the "now" picture), `showHideRevealAnimation:NO`, then `revealOutAnimationComplete:` stops the display link, `FUN_0004dc1c` fades the displays back in, moves the host window back (`CGSMoveWindow` to its saved bounds) and deletes the `TBackup` | 0x33d9a, 0x36096, 0x4dc1c | Read |

## 2. The window, OpenGL and the frame loop

| What | Where | Status |
|---|---|---|
| Pixel format: accelerated, colour 32, double-buffered, allow offline renderers (`{0x49, 8, 32, 5, 0x60, 0}`); the context's swap interval 1 and surface opacity 1 (`CGLSetParameter` 222, 236) | `pixelFormat:` 0x30346, `initWithFrame:` 0x3be59 | Read |
| Frame loop: a CVDisplayLink on the view's displays (`_setupDisplayLink` 0x32b97), its callback (`FUN_000307a8`) calls `displayFrame:`: lock the CGL context, `_drawBackgroundAtTime:` at the time stamp's host time × the mach timebase (seconds), `drawCALayers:` (a `CARenderer` on the view's context renders the layer tree: `beginFrameAtTime:timeStamp:`, the view's bounds as update rect, `render`, `endFrame`), `CGLFlushDrawable` | `displayFrame:` 0x32a70, `drawCALayers:` 0x376fd, `_createRendererForLayer:` 0x31df9 | Read |
| `drawRect:` draws one frame immediately (at `CACurrentMediaTime()`) and marks the view initialised (the display link draws nothing before) | 0x329cc | Read |
| The layers' projection: viewport = view size, `glOrtho(0, w, 0, h, -1, 1)`, identity modelview (`updateMatrices`) | 0x327d3 | Read |
| The layer tree (all children of `fBaseLayer`): the snapshot layers (slot 0 above, each older one inserted below the previous), Cancel, the date, Restore All, Restore, Previous, Next, the timeline; then the window layer and the reveal layer (`setRevealImages:`), which cover everything during the reveal | `initWithFrame:` 0x3be59, `addLayer:forTarget:` 0x385bb, `setRevealImages:window:reveal:` 0x38985 | Read |
| Animated background only when `_FXShowBackgroundAnimation` (default YES) and `canShowBackgroundAnimation`: `hw.memsize` ≥ 512 MB, a renderer with more than 64 MB of video memory (`kCGLRPVideoMemory`), and `MPStarField isCompliantWithHardwareCapabilities` (the four `GL_ARB_shading_language_100`, `GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader` and `GL_ARB_vertex_buffer_object`); else the **static background**: `vortex.png` (1437×952, the galaxy pre-rendered) as a `GL_TEXTURE_RECTANGLE_EXT` quad stretched over the whole view (needs `GL_EXT_texture_rectangle`; without it a black clear) | 0x33181, 0x18b13, 0x1493a, `_drawBackgroundAtTime:` 0x31bc9 | Read |

## 3. The backdrop: `MPStarField`

Each frame (`_execute:pixelFormat:time:` 0x1720e), after clearing to black: the gas (the galaxy's
clouds), the sun, the background stars, the foreground stars. One camera: a symmetric frustum,
`glFrustum(±nw/2, ±nh/2, near 0.01, far 5.01)` with `nw = 2·0.01·tan 17.5°` and `nh = nw/aspect`:
**35° horizontal field of view** (`fTanFovHalf = tan(0.30543)`), aspect = viewport width/height
(1.6 if unknown).

| What | Where | Status |
|---|---|---|
| Constants: `fFarPlane` 5.0, `fBGNearPlane` 4.8 (the plane of the background stars, the gas and the sun), `fStarCount` 300 per colour, `fBGStarCount` 850 per colour (680 when `GL_RENDERER` contains "GMA 950"), `fMinSize = fMaxSize` 0.01, `fBGMinSize` 0.005, `fBGMaxSize` 0.015, `fBGMinXY = fBGMaxXY` 0.0005, `fSpeed` 0.0003, `fBGSpeed` 0.0001 (unused), `fBGBrightness` −0.6, `fGasRadius` 3.0, `fGasRadians` 7.0, `fGasPower` 75.0, `fGasFade` 0.9, `fGasWidth` 2.5, `fBGUpdateInterval` 0.2 s | `_setDefaultValues:` 0x170bc, `_setup:` 0x18e6e, 0x1720e | Read |
| Derived sizes (recomputed when the aspect changes): far plane `farW = 2·5·tan17.5° = 3.153`, `farH = farW/aspect`; background plane `bgW = 2·4.8·tan17.5° = 3.027`, `bgH = bgW/aspect` | 0x1720e | Read |
| Textures (`_loadTextures:`): the foreground stars `NormalWhiteStar`, `NormalRedStar`, `NormalBlueStar`, `NormalYellowStar` (64×64 TIFF, a glowing point with four spikes, black background); the background stars `HyperWhiteStar`, `HyperRedStar`, `HyperBlueStar`, `HyperYellowStar` (8×8 TIFF); `sun.tiff` (512×512, an orange glow with a white core). Order white, red, blue, yellow; one vertex buffer per colour; each drawn with blending | 0x188d2 | Read |
| Random numbers: `srandom(time(0))` (`FUN_00011bc3`), `U(a, b) = random()/2147483647·(b−a)+a` (`FUN_00011bd6`, bounds swapped if reversed) | 0x11bc3, 0x11bd6 | Read |
| **Foreground stars** (4 × 300 textured quads, `initStarsWithRandomZ:` 0x17be1): z = U(−5, 0); half size s = U(0.01, 0.01)/2 = 0.005; x = U(−farW/2, farW/2) and, if it falls in [−nearW/2−s, 0], drawn again in [−farW/2, −nearW/2−s], if in [0, nearW/2+s] in [nearW/2+s, farW/2] (keeps the axis clear); y likewise with farH/nearH. Quad (x−s, y+s), (x+s, y+s), (x+s, y−s), (x−s, y−s) at z; texture coordinates (0,0), (0,1), (1,1), (1,0) (`initVertexAttribs:` 0x1627f) | 0x17be1 | Read |
| Their motion (`checkAndUpdateStars:` 0x160f5, each frame): z += Δ(stars z offset) + 0.0003 × warp × elapsedFrames, warp = starsSpeed × 400, elapsedFrames = min((t − tprev)/(1/60), 1); if z ≥ 0 → z = −5 (back to the far plane); if z < −5 → z = 0. Idle starsSpeed = 0.007, so 2.8 × 0.0003 = 0.00084 per frame (0.05 unit/s: they drift slowly towards the viewer) | 0x160f5, 0x16673 | Read |
| Their drawing: blend `GL_SRC_ALPHA, GL_ONE` (additive), colour white, linear **fog** to black from 4.81 to 5.01 (stars fade in at the far plane), and `gluLookAt(eye (0,0,0.01), centre (0, −starOffsetY, −5.01), up (0,1,0))` where `starOffsetY = offsetY·farW/2`: the tunnel's vanishing point follows the galaxy's centre | 0x1720e, `drawStars:` 0x16b7c | Read |
| **Background stars** (4 × 850 quads on the plane z = −4.8, `initBackgroundStars:` 0x18bac): x = U(−bgW/2, bgW/2), y = U(−bgH/2, bgH/2), half size U(0.005, 0.015)/2; each has a radial direction θ = angle of (x, y − offsetY·bgW/farW) (`FUN_00011b29`, an atan2), a speed v = U(0.0005, 0.0005), velocity (v cos θ, v sin θ), a life U(0, 5), 2v kept | 0x18bac | Read |
| Their motion (`checkAndUpdateBackgroundStars:` 0x19306), every 0.2 s, or every frame while the stars' z offset moves: when the galaxy's y moves they shift by Δ·bgW/farW and wrap vertically; each moves along its direction by k·velocity, k = 1 at rest, else sign(Δz)·(\|Δz\|·50+1)·((\|x\|+0.15)² + (\|y−oy\|+0.15)²)·20 / (planeW² + planeH²) (faster towards the edges during a flight); flying back (Δz < 0) consumes their life, and a dead or off-screen one (`NSIntersectsRect` with the plane) is reborn at a random place, life U(0, 5) | 0x19306 | Read |
| Their drawing: colour (1 + fBGBrightness) = **0.4 grey**, same blend and fog, no `gluLookAt` | 0x1720e, 0x16a11 | Read |
| **The gas** (the galaxy's clouds): one textured quad at z = −4.8 centred on (0, gy), gy = offsetY·bgW/2, half width = fGasWidth/2·bgW/2 = 0.625·bgW (square), texture = `VortexGL vortexTexture` (section 4), blend `GL_ONE, GL_ONE`, `GL_MODULATE`, linear filtering | `drawGas:` 0x18493 | Read |
| **The sun**: the same quad × 0.375 (half width 0.234·bgW), texture `sun.tiff`, blend `GL_SRC_ALPHA, GL_ONE` | `drawSun:` 0x186c1 | Read |
| The galaxy's height: `offsetY = (2/aspect)·(Y/viewHeight − 0.5)` where Y (view points from the bottom) = `globalYOffset`, a `TMFloatParameterController` (eased with `kCAMediaTimingFunctionEaseInEaseOut` (0.42, 0, 0.58, 1)); so Y maps exactly onto the screen: 0 = bottom edge, h/2 = centre | 0x1720e, 0x19be0, 0x19d79 | Read |
| Speeds: `gasSpeed(t) = 0.3 + 2t − 0.3t²`, `starsSpeed(t) = 0.007 + 0.6t − 0.007t²` (idle t = 0, warp t = 1, reverse t = −1: gas 0.3 / 2.0 / −2.0, stars 0.007 / 0.6 / −0.6); `setSpeeds:` can ramp them with the curve (0.4, 0, 0.6, 1), but nothing starts that ramp (`fAnimatingSS` is never set): the gas always turns at 0.3 and the stars drift at 0.007; flights move the stars by the **z offset** instead (section 6) | 0x1664d, 0x16673, 0x16716 | Read |
| The "galaxy" therefore comes from: 100 copies of `Cloud.png` (32×32, a white puff, alpha 0.2 average) tinted cyan, twirled (section 4), plus `sun.tiff`, plus the stars; `vortex.png` is the same scene pre-rendered, used only by the static fallback | 0x14a22, 0x31bc9 | Read |

## 4. The twirling clouds: `VortexGL`

`MPStarField _setup:` creates it with the frame 512×512 and the inputs radius 3.0, one_over_radius
1/3, angle_radians 7.0, power 75.0, fade (1 − 0.9)·0.5 = 0.05, fadeDistInv 1/0.45; `VortexGL` keeps
radius, power, angle_radians and **fade = 0.05** (0x14a22).

| What | Where | Status |
|---|---|---|
| Two 512×512 pbuffers sharing the view's context: the cloud buffer (`fBufferContext`, perspective: `glFrustum(±0.1·tan30°, ±0.1·tan30°/aspect, 0.1, 100)` then `glTranslatef(0, 0, −1/tan30°)` so that [−1, 1] at z = 0 fills it) bound to `fBufferTexture`, and the draw buffer (`fDrawContext`, `gluOrtho2D(−1, 1, −1, 1)`) bound to `fVortexTexture` | `initWithFrame:context:…` 0x14a22, `_configureViewportForContext:` 0x12d00 | Read |
| `Cloud.png` uploaded mipmapped (`GL_BGRA`, `GL_UNSIGNED_INT_8_8_8_8_REV`, min filter linear-mipmap-linear, `GL_GENERATE_MIPMAP`, wrap repeat) | `_uploadTexture:` 0x14493 | Read |
| Each frame (`renderAtTime:` 0x13d81): T += Δt·speed (speed = the gas speed, 0.3; the first frame takes 1.0 as previous time and T); clear; blend `GL_ONE, GL_ONE`; for i = 0…99: phase = i/100·360°, t = frac((T+41)/10 + phase/360) (`_LFOTime:` 0x13cf9: period 10, amplitude 0.5, offset 0.5: a saw from 0 to 1); α = A(t), w = W(t), z = Z(t), x = X(t, I1(r1)), y = Y(t, I2(r2)) (interpolators below); colour (0.1544α, 0.7132α, 0.7574α, α); a quad from (x − w/2, y − 0.8205w/2) to (x + w/2, y + 0.8205w/2) at z with the cloud texture: each cloud flies from the centre at z = −5 out towards the viewer, swelling and fading, a new one every 0.1 s of T | 0x13d81 | Read |
| The interpolators (`_initInterpolators` 0x11cec, `GVInterpolator`): output = v1 + (v2 − v1)·S(t′); t′ = t warped by the tension τ: t′ = t² + (1 − (τ+1)/2)·2t(1−t); S a natural cubic spline through the control points (`FUN_0001aa7f`, Numerical Recipes' `spline`/`splint`; values clamped to [0, 1]); duration 1, repeat (`inputRepeat` 1: wraps) | `execute:` 0x1acad, `valueAtTime:` 0x1a8a5, `setState:` 0x1a1c1 | Read |
| A (alpha): v1 0, v2 0.03; points (0, 0.8846), (0.8538, 1.0), (1, 0). W (width): v1 0, v2 1.5, τ −0.25; points (0, 0.2731), (0.2923, 0.3269), (0.5731, 0.4846), (1, 1.0). Z: v1 −5, v2 0, linear. X, Y: v1 0, v2 = the cloud's target (`executeTime:input2:`), linear. I1: v1 −1, v2 1; points (0, 0), (0.2885, 0.3769), (0.5, 0.5), (0.7038, 0.65), (1, 1). I2: v1 −1, v2 1; points (0, 0), (0.2923, 0.3692), (0.5, 0.5), (0.7423, 0.6385), (1, 1) | 0x11cec | Read |
| The clouds' targets r1, r2 are fixed per cloud: `FUN_00011ad9(0, 1, i, seed)` with seeds 0x634846be and 0x0da4ba97: v = seed + lround(round(i·1000)·0.001·24.855·1000), then (\|v\| & 15) steps of Park–Miller (Schrage, a = 48271, q = 44488, r = 3399, 0 → 0x7fffffff), r = v/2147483647 | 0x119e9, 0x11ad9 | Read |
| **The twirl**: computed once on the CPU (`drawCachedVortex` 0x154b0, the same formula as the unused `VortexCache.frag`) into a 512×512 remap texture: for each texel (u, v), d = distance to (0.5, 0.5), a = atan(dy/dx) + 7·((3 − d)/3)^75·(3 − d)/3; if d < 3 the source point is (0.5, 0.5) + d·sign(dx)·(cos a, sin a), else (u, v); bytes (BGRA order): B = mask (255 for d ≤ 0.475, then 255·(1 − (d − 0.475)·40), 0 beyond 0.5), G = y·512 >> 1, R = x·512 >> 1, A = the two dropped low bits (x odd 85, y odd 170, both 255). The twist is 7 rad at the centre, 1.95 rad at d = 0.05, 0.53 at d = 0.1, nothing at the rim: a tight spiral core | 0x154b0 | Read |
| Each frame (`drawVortex` 0x13199) the draw buffer is cleared and one quad drawn with `RemapVortex` (uniforms `clouds` = unit 0 the cloud buffer, `remapTexture` = unit 1 the remap; it reads the remap texel, adds 1/512 to x and/or y according to A's thresholds 0.75, 0.5, 0.2, samples the clouds there and multiplies by the mask B); that texture is the gas quad of section 3 | 0x13199, `RemapVortex.frag` | Read |
| `Twirl.vert/.frag` (the same twirl evaluated per pixel, with `fade`, `fadeDistInv`) and `VortexCache.vert/.frag` are never loaded (no string names them; `_createGLSLProgramWithVertexShader:` is only called with "RemapVortex") | 0x13987, `strings.txt` | Read |

## 5. The stack of windows

| What | Where | Status |
|---|---|---|
| `fSnapshotsToShow` = **10** slots (0 = front … 9 = farthest); layers further than slot 9 sit on slot 9 at opacity 0 | `initWithFrame:` 0x3be59, `animationParamatersForLayer:atIndex:` 0x3836b | Read |
| The front slot (`calculateLayerBounds:baseBounds:withSunRaised:` 0x36b86, always "sun raised"): the window's size B (the host window's picture × `userSpaceScaleFactor`), x = round((V.w − B.w)/2 + V.x), y = round((V.h − (B.h + 140))/2 + V.y) (centred, 140 points lower than centre); the horizon T = maxY(V) + 140 (140 points above the top of the screen) | 0x36b86, `FUN_0004e122` 0x4e122 (the same rect in screen coordinates for the host window) | Read |
| Slot i (1…9), z = i/9 (`calculateLayerBoundsAtNormalizedZ:withBaseBounds:withTargetY:` 0x364b5): z′ = S(z), S a natural cubic spline through (0, 0), (0.7932, 0.6033), (0.9211, 0.82), (1, 1) (the "spacing curve", 0x3bf3b); s = B.w/80; depth D = 1 + (s − 1)·z′; R = B moved by z′·s·(T − maxY(B)) up and z′·s·(midX(V) − midX(B)) across; then projected towards the camera point c = (midX(B), maxY(B)): R′ = (R.origin − c)/D + c, size B/D. So the farthest window is **80 points wide**, centred horizontally, its top on the horizon; the windows recede upwards | 0x364b5, `projectedBoundsAtZPosition:forCameraPosition:withUnscaledBounds:` 0x34597 | Read |
| Shading: a `CAFilter` multiplyColor named `shadingFilter` on each layer, its colour for slot i (t = i/9) (`calculateShadingFilterColorForSnapshotIndex:` 0x3806a): e = 2t below 0.5, 1 up to 0.7, then 1 − (t − 0.7)/0.3; I = 0.3·(t/0.34 below 0.37, 1 up to 0.7, 1 − (t − 0.7)/0.2 up to 0.9, then 0); k = 1 − 0.8e; colour (k(1 − 0.2I), k(1 + I), k(1 + 0.75I), 1): white at the front, darkest and bluish in the middle (≈ (0.19, 0.26, 0.25) for t 0.5–0.7), white again at the back | 0x36981, 0x363b9, 0x3806a | Read |
| Opacity 1 for the 10 slots, 0 beyond; slot 0's layer is kept at 0 under the host window (`hideTopSnapshotNow`; `hideTopSnapshot` fades it 1 → 0 in 0.05 s, `showTopSnapshot` back) | 0x3836b, 0x2fa32, 0x2fb92, 0x2ff11 | Read |
| An unavailable snapshot (`FUN_0002ac52` false) shows `invalidTargetImage`: the 9-slice frame `InvalidTopLeft/Top/TopRight/Left/Right/BottomLeft/Bottom/BottomRight.png` around the window's size, filled with RGB 0.149 at alpha 0.85 | 0x351d8 | Read |

## 6. Flying through time

| What | Where | Status |
|---|---|---|
| `gotoSnapshot:` (from the timeline, the arrows, a click on a window) → `promoteDemoteSnapshot:` with the target layer; a request during an animation is kept (`fRequestedSnapshot`) and served when it ends | 0x33c88, 0x3a0c1 | Read |
| n = min(\|target − top\|, max(10, `FXBackupMaxSnapshotsToAnimate`)); the divisor d = n/(log₂ n + 1) (`divisorFor:` 0x375d8, base `_FXAnimationSpeedScaleTimesBase` 2); the step duration p = 0.5/d (`_FXBackupPromoteDuration_ms` 500) and the whole flight n·p = **0.5·(log₂ n + 1) s**: 1 step 0.5 s, 2: 1.0, 4: 1.5, 8: 2.0 | 0x3a0c1, 0x332b6 | Read |
| Each layer that is or will be in the 10 slots gets a `CAAnimationGroup` of `CAKeyframeAnimation`s on `opacity`, `position.x`, `position.y`, `bounds.size.width`, `bounds.size.height`, `filters.shadingFilter.inputColor`, one keyframe per slot it passes through (`animationParamatersForLayer:fromIndex:toIndex:` 0x30d15), duration n·p, timing function **(0.4, 0, 0.6, 1)** (`fEaseInEaseOut`), fill mode frozen, begun at the next free start time; the target's group carries the completion delegate (kind 5) | 0x3a0c1, 0x30d15 | Read |
| The stars fly with it (`triggerSnapshotWarp:duration:warpForward:` 0x32d17, unless `_FXEnableWarp` NO): the stars' z offset moves by ±n·0.5 over n·p with the ease (0.42, 0, 0.58, 1); towards older snapshots (index increasing) +n/2: the stars rush at the viewer; towards Now −n/2: they recede | 0x32d17 | Read |
| Towards Now the restored items' layers (`fRestoreLayers`) fade: opacity keyframes 1, 1, 0 at 0, 0.99, 1 over n·p + 2 s (`showRestoreAnimationStartingAt:withDuration:` 0x37a23) | 0x37a23 | Read |
| The pictures: the target and its neighbours (3 either side when jumping more than 3, then the slots behind) are asked from the host as the flight starts (`requestSnapshotImageFor:`); an arriving picture replaces the layer's contents with a 0.25 s cross-fade (`_FXBackupUpdateDuration_ms`, `updateLayer:forTarget:` 0x3111b) | 0x3a0c1, 0x3111b | Read |

## 7. Entering and leaving

There is no vortex/twirl transition on the screen's picture: the "twirl" is only the galaxy's
texture (section 4). The entry is the desktop sliding down while the galaxy rises and the stack
flies up from behind the window.

| What | Where | Status |
|---|---|---|
| Durations (milliseconds in com.apple.finder, `initAnimationConstants` 0x332b6): `_FXBackupPrepareRevealDuration_ms` 20, `_FXBackupRevealDuration_ms` **1500**, `_FXBackupPromoteDuration_ms` 500, `_FXBackupUpdateDuration_ms` 250, `_FXBackupShowHideDuration_ms` 50 | 0x332b6, 0x3326e | Read |
| The pictures (`setRevealImages:window:reveal:` 0x38985): the reveal layer = a capture of the screen's windows (`CGSCaptureWindowsContentsToRect`, the on-screen windows except the menu bar (height 22), the Dock and the entering window, `FUN_0002095f`, `FUN_000503b6`), full screen; the window layer = the host window's picture at its live position (`fLiveWindowPosition`); both start at opacity 0 and are shown by `prepareToReveal:` | 0x38985, 0x2095f, 0x503b6, 0x339eb | Read |
| Multi-display: unless `_FXBackupEnableDisplayFade` NO, every display but the main one fades to black in `_FXBackupDisplayFadeDuration_ms` 250 (15 steps of 1/60 s scaling its transfer formula) and is captured; on exit the reverse (`FUN_0002024c`) | 0x2024c, 0x51eba | Read |
| **Entry** (`showHideRevealAnimation:YES` 0x3afe6, begun at `nextStartTime` (now, or when the running animations end; `calculateNextStartTime:3` books the next 1.5 s)): the reveal layer's `position.y` → y − height (the desktop **slides down off the bottom**) and its opacity 1 → 1 (0.1 s), the window layer's position → the front slot's centre (`fStartPosition`) (or, with `fFadeWindowLayer`, its opacity 1 → 0), each 1.5 s with (0.4, 0, 0.6, 1); the galaxy rises: `globalYOffset` 0 → the top of the farthest visible window (`updateAnimationEnd:` 0x36305: round(maxY(slot min(9, count − top − 1)))) in 1.5 s (`triggerSunriseAnimationFromY:toY:duration:` 0x32e84; `_FXEnableSunriseAnimation` NO makes it a jump) | 0x3afe6, 0x36305, 0x32e84 | Read |
| The stack expands at the same time (`collapseOrExpand:YES` 0x39277): each visible layer's position goes from its "collapsed" place (the same slot geometry but based on the live window's rect and with the horizon at y = 0: the stack folded down behind the window, below the screen) to its slot, opacity 1, 1.5 s, (0.4, 0, 0.6, 1); layers past the 10 slots at opacity 0 | 0x39277 | Read |
| At the end (`showHideRevealAnimationCompleted:` 0x3181b, `revealInAnimationComplete:` 0x3719e): the window and reveal layers hidden (the reveal moved down by its height), the layers set to their slots, slot 0 at the front slot's centre, the other pictures asked | 0x3181b, 0x3719e | Read |
| **Exit** (`showHideRevealAnimation:NO`): the galaxy sets: `globalYOffset` from the top of the farthest visible window (`setSunAnimationStart:` 0x33954) → 0 in 1.5 s; the restore layers hidden; the reveal layer's `position.y` → y + height (the desktop **slides back up**), the window layer → the live window position; the stack collapses (`collapseOrExpand:NO`), all 1.5 s, (0.4, 0, 0.6, 1); then `revealOutAnimationComplete:` 0x36096 | 0x3afe6, 0x33954, 0x36096 | Read |
| A hot spot (`HotPoint.png`, 158×87) at the top of the farthest slot, only with `_FXEnableTimeMachineHotSpot` (default NO); a click on it picks the window under the point | 0x38985, 0x3719e, `mouseDown:` 0x34aa3 | Read |

## 8. The bottom toolbar

One perspective bar 44 points high, 10 points above the bottom of the screen, made of
`TMToolbarPushButton` CALayers drawn with Core Graphics: [Cancel | date | Restore All | Restore].

| What | Where | Status |
|---|---|---|
| Cancel ("BU9"): x 10, y 10, height 44, width `optimalSize`, left end cap; Restore ("BU12"): right-aligned 10 points from the right edge, right end cap; Restore All ("BU11"): only when the host allows it (`FUN_0004da6a`), its frame ending 46 points inside Restore's left edge, disabled at first; the date (`TMDateText`, accessibility "BU26"): from Cancel's maxX − 46 to (Restore All or Restore)'s minX + 46, its label centred on the screen's midX | `initWithFrame:` 0x3be59 | Read |
| `optimalSize` = (round(textWidth·1.75 + 92 + 20), round(textHeight)) for the title in **HelveticaNeue-Bold 12** scaled by (1.75, 1.18) | 0x42234, 0x43c91, 0x41429 | Read |
| The art (`TMToolbarPushButton init` 0x40818): `Left.png` / `LeftDown.png` (51×44), `Right.png` / `RightDown.png` (51×44), `Center.png` / `CenterDown.png` (1×44), the "Down" ones while pressed. `drawBackground:` 0x42737: the left cap image at the left (if it has one), the centre image stretched and clipped by two masks (left and right fill paths), the right cap at the right | 0x40818, 0x42737, 0x41b36, 0x41828 | Read |
| The dividers are slanted like the bar's perspective: for a point x (clamped to [23, W − 23], in screen coordinates, W = screen width), angle = atan(((W − 20)/2·tan 48° − 22)/(W/2 − x)) (`dividerAngleForPosition:withEndCapAngle:` 0x4257d, end-cap angle 48°), width = round((44 − 6)/tan angle) (0x424d6); a divider at 23 points inside each joining edge: four white strokes 1 point wide, alphas 0.3 (the slanted line from 6 points up to the top), 0.4, 0.15 (the 6-point stubs), 0.1 (`drawDividerLines:atX:withWidth:` 0x40a13) | 0x4257d, 0x424d6, 0x40a13, `renderIntoContext:` 0x40c14 | Read |
| The label (`drawPerspectiveText:withLeftOffset:withRightOffset:withBlur:` 0x42bab): the text frame = bounds inset 23 (13 less on the left cap's side, 10 more on the right cap's), drawn at 2× in an offscreen: once in black (the shadow), once in white (enabled) or 50 % grey (disabled), each warped into a trapezoid whose top corners move out by half the divider width at 44.5° and bottom corners in by half that plus 6/tan(angle) (`FUN_00040f2f`), the shadow 1 point lower; then drawn twice: alpha 0.05 offset 5 points up, then alpha 1; vertical offset 8.6 (`verticalTextOffset`) | 0x42bab, 0x43e16, 0x416eb | Read |
| Strings: Cancel "BU9", Restore "BU12", Restore All "BU11", Highlight Changes "BU10" (`TMToolbarCheckbox`, not created by `initWithFrame:`), Restore "^0" To… "BU35"/"BU35.1", Choose Folder "BU36" | Localizable.strings | Read |
| `Cancel*.png`, `Restore*.png`, `RestoreAll*.png`, `Changes*.png`, `ToolbarBackground3D.png`, `RestoreEndWedge.png`, `TopHighlight.png` are in the image table (0x81100, ids 6–21) but nothing loads them: the toolbar is drawn from Left/Center/Right | `FUN_0005873a`, `FUN_000586b0` | Read |
| **The date** (`TMDateText`, a `TMToolbarPushButton` without caps): HelveticaNeue-Bold **15**, vertical offset 9.6; the snapshot's date formatted by the first of five formatters that fits (text width·1.75 + 112 + 2·44·0.7071 < frame width): relative full date-time, relative long, relative medium, relative short (CFDateFormatter date style 4, 3, 2, 1 with time style short; "Today at ^0" "U18" / "Yesterday at ^0" "U19" for today and yesterday), short date-time; the live state "Today (Now)" ("BU20") | `setDate:` 0x442c0, 0x1f3a8, 0x1f79c…0x1f24a | Read |
| The arrows (`TMSkinnedPushButton`): Previous ("BU28", `PreviousButtonUp.png` / `PreviousButtonDown.png`, 144×48) and Next ("BU29", `NextButtonUp.png` / `NextButtonDown.png`, 144×55), at x = the front window's maxX + 40 (`bigArrowsLocationForWindowBounds:` 0x347b0), Previous's bottom on the window's bottom edge, Next 55 points below it; disabled at alpha 0.5 | 0x3be59, 0x40568, 0x420e9 | Read |
| Buttons' states (`updateButtonsEnableStateForTargetIndex:`): Next and Previous enabled from the timeline (`FUN_000469e6`, `FUN_00046948`: an older / newer snapshot exists); Restore (when the Finder is not the client) and Restore All only for a snapshot other than Now (index > 0) in a restorable location; the date text set to the timeline's current snapshot | 0x326a5, 0x32536 | Read |

## 9. The timeline on the right

A `TMTimeline` layer, frame x = screen width − 400, y = 50, 400 × (height − 50), drawing a
`TTimelineControl` (C++) into a **flipped** context (`FUN_0002081f`): item 0 at the top.

| What | Where | Status |
|---|---|---|
| Items (`FUN_0004a4c4`): placeholders for the months before the oldest snapshot (one per month until the column is full), then for each snapshot oldest → newest a `TTimelineSnapshotItem`, with a `TTimelineMonthItem` when the month changes (its name "MMMMyyyy" template, the year shown when it changes) and `TTimelineLabelItem`s "Yesterday" ("BU18") and "Today" ("BU17") before the first snapshot of those days; the last item is `TTimelineCurrentItem` "Now" ("BU19"), at the bottom | 0x4a4c4 | Read |
| Layout (`FUN_0004a37c`, `FUN_00048f68`): the column = bounds inset 30 vertically; with N items, if N·16 is shorter, the column is N·16 high and sits at the bottom; each item's slot = column height/N (`(H − 2N)/N + 2`); when that is under 4 points, runs of ordinary snapshots are merged into `TTimelineItemCluster`s (up to 31, `FUN_00049046`, `FUN_0004a03c`) | 0x4a37c, 0x48f68 | Read |
| Ticks, right-aligned on the column's right edge (`FUN_000487e0`): snapshot ticks 16 long × 2 thick at rest; month and label ticks 36 × 3 (`FUN_00044f58`, `FUN_00044f66`); drawn as white rectangles at the tick's alpha, or, while magnified, as `TickCap.png` (8×25, scaled down when the tick is thinner than 25) at the left plus `TickFill.png` (2×25) stretched (`FUN_000457fc`) | 0x487e0, 0x457fc | Read |
| **Magnification like the Dock** (`FUN_00046140`, `FUN_0004607a`, `FUN_00045f60`): with m = clamp(h·S(\|y − mouseY\|/110)), h the hover amount (0 → 1 in 225 ms, `TParametricInterpolator`, redraw every 0.04 s): length = 16(1 − m) + 69.44m (16·4.34), thickness = 2(1 − m) + 19.58m (2·9.789), the spacing of every item = lerp(slot, 16, h). S for the length: natural spline (0, 1), (0.19, 0.77), (0.295, 0.53), (1, 0); for the thickness: (0, 1), (0.673, 0.13), (1, 0) (set up in `FUN_00047b02`). The magnified tick under the mouse stays where it is (`FUN_000493bc` scrolls the column to compensate) | 0x46140, 0x4607a, 0x45f60, 0x47b02, 0x493bc | Read |
| Hover: magnification starts when the mouse is within the rightmost 54 points of the timeline (`FUN_00049b2a`, `FUN_00049c2e`); near either end (30 points) the column auto-scrolls by ±(1 + 39f) points per 0.04 s (f the depth into the 30-point zone) | 0x49b2a, 0x49620 | Read |
| Alphas (`FUN_0004856c`): the current snapshot 1.0, the one under the mouse 0.784 (200/255), an unavailable one 0.196 (50/255), others 0.6 (153/255); month/label ticks 0.6; all × a fade over the last 30 points at either end of the column (`FUN_00047ffe`) | 0x4856c, 0x48520, 0x47ffe | Read |
| Labels (`FUN_00046ff4`, `FUN_000481c2`, `FUN_00047e78`): LucidaGrande-Bold 12, right-aligned 8 points left of the tick, 16 high, centred on it (+2), white with a shadow (CGStyle {55, 90, 0, 2, 3}); the hovered snapshot shows its date (relative full date-time, or date only when its neighbours are other days; "? ? ? ? ?" without a date; "Today (Now)"); months and labels in 0.6 grey (white when current or hovered for Now) | 0x46ff4, 0x46c02, 0x47e78 | Read |
| Clicking a tick goes to its snapshot (vtable slot 0x50 → `gotoSnapshot:`); the scroll wheel over the timeline scrolls it | `FUN_00045dea`, `scrollWheel:` 0x301ac | Read |

## 10. Input

| What | Where | Status |
|---|---|---|
| Click on a stack window → `gotoSnapshot:` its index (on slot 0: the host window takes it); click on the arrows / buttons by their frames and `pixelAccurateHitTest:` | `mouseDown:` 0x34aa3 | Read |
| Previous / Next (`previousPressed:` 0x3278b, `nextPressed:` 0x327af): "smart" navigation by default (to the next snapshot where the selected item changed, `_FXSmartNavigationPreviousNext` YES), the adjacent snapshot with ⌘ (`_FXCommandKeyForSmartNavigation` NO inverts it) | 0x46990, 0x46a2e, 0x45e78 | Read |
| Keys: `TBackupView keyDown:` only reads the character and swallows it; the browser's keys are the host's (Escape, the arrows, ⌘↑/⌘↓), which call the framework's API (section 11) | 0x33348 | Read |
| Mouse moves come from a Carbon event monitor (`InstallEventHandler(GetEventMonitorTarget())`, mouse moved and dragged) | `setupEventMonitor` 0x315a4 | Read |

## 11. The host API and the notifications

| What | Where | Status |
|---|---|---|
| Exported constants: `kInvokeTimeMachineNotification` "com.apple.peabody.show", `kBackupNow` "com.apple.peabody.backupnow", `kBackupBrowseOtherDisks` "com.apple.peabody.browseOtherDisks", the `com.apple.backup.Backup*Notification` names (handshake, request start, target activate/activated/deactivate/deactivated/changed, dismissed, restore, request/update thumbnail with Height, BytesPerRow, BitsPerPixel, BitsPerComponent), RegisterHooks / PerformPreOrPostHook, the snapshot xattrs `com.apple.backupd.Snapshot*`, navigation keys SnapshotURL / SnapshotDate / SnapshotIsValid | `__const` 0x7f140… | Read |
| Posted: `com.apple.peabody.show` (distributed), `com.apple.backup.BackupRequestHandshakeNotification`, `com.apple.backup.AXTimeMachineStarted` / `…Ended` (object "com.apple.backup.BackupObject"); observed: `com.apple.backup.BackupHandshakeNotification`, backupd's `com.apple.backupd` and "ThinningBackup"; an Apple event handler 'peab'/'BROW' | 0x1b60c, 0x1b6a0, 0x51eba, 0x57f18 | Read |
| The host side (in process): it registers callbacks (`BURegisterActivateSnapshot`, `…DeactivateSnapshot`, `…RequestSnapshotImage`, `…RequestRestoreImages`, `…TimeMachineRestore`, `…TimeMachineDismissed`, `…NavigateForwardOrBackward`, `…ShowChangedItemsOnlyToggled`, `…UpdateEntryWindow`, `…RequestRevisionID`, `…StartTimeMachineFromDock`); the framework calls them with CF dictionaries (URL, bounds, ShowChangedItemsOnly; URL, URLInSnapshot, RestoreAll); the host answers with `BUActivatedSnapshot`/`BUDeactivatedSnapshot` (URL), `BUUpdateSnapshotImage` (a window's picture via `CGWindowContextCreateImage`), `BUUpdateGenericSnapshotImage`, `BUInvalidateAllSnapshotImages`, `BUTimeMachineAction` (an action code; `BUTimeMachineSetRestoreAllowed` sends 1001 / 1000), `BUChangeTimeMachineTarget`, `BUCopySnapshotArray`, `BUCopyCurrentTarget*`; window level `_BUGetTimeMachineWindowLevel` = `CGWindowLevelForKey(kCGNormalWindowLevelKey)` + 2 | 0x5525a…0x56843 (exports), 0x57a86…0x57d94 (dispatch) | Read (the host's use: `.local/media/re/backup/host-notes.md`, another pass) |

## 12. Not resolved

| What | Why |
|---|---|
| The exact window list kept in the desktop picture (`FUN_000503b6`'s filters: windows 22 high, the Dock's level, `CGSGetSystemWindow(4)`) | read only in outline; the host-side pass covers the Finder's part |
| Which of the two first-position rects (collapsed / slot) `collapseOrExpand:` uses as from and to | the two branches are decompiled identically; taken from the meaning (expanding goes collapsed → slot) |
| `GVInterpolator setState:`'s pairing of `controlPointTimes` / `controlPointValues` | the dictionary keys passed to `objectForKey:` are hidden in Ghidra's output; taken as times → x, values → y (the curves then read as fade-in/out and growth) |
| The CGStyle shadow's fields for the timeline labels ({55, 90, 0, 2, 3} at 0x5aa40) | the private struct's layout is not documented |
| The month labels' layout (`FUN_000481c2`: month, then the year in a second run) | partly read |
| The restore flow (choose folder, recreate enclosing folders, "BU35"–"BU41"), Highlight Changes, live window resize (`_FXLiveWindowResize`, `startResizingWindow`), accessibility | outside the visual path; not read |
