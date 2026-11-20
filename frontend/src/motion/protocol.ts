// Typed mirror of the GakuMotion web-panel bridge (gakumotion.web-panel/v1).
// The native service is authoritative; this file is the frontend contract.
// See frontend/docs/motion-bridge-protocol.md for the wire format and semantics.

import { BridgeError, PROTOCOL_VERSION, type RequestOptions } from '@/bridge/protocol'

export { BridgeError, PROTOCOL_VERSION }
export type { RequestOptions }

export interface Vector3 {
  x: number
  y: number
  z: number
}

export type PlaybackStatus = 'stopped' | 'playing' | 'paused' | 'preview'

export type ClockRunMode = 'realtime' | 'recording'
export type ClockSource = 'audio' | 'engine_delta' | 'fixed_step' | 'external'

export interface ClockState {
  runMode: ClockRunMode
  configuredSource: ClockSource
  effectiveSource: ClockSource
  fpsNumerator: number
  fpsDenominator: number
  waitingForRenderComplete: boolean
  leaseId: number
  leaseOwner: string
  evaluatedFrameSerial: number
}

export interface AudioEffectPreset {
  id: string
  name: string
  category: string
  description: string
}

export interface AudioEffectState {
  available: boolean
  activeId: string
  presets: AudioEffectPreset[]
}

export interface AudioProbeInfo {
  valid: boolean
  sampleRate: number
  channels: number
  durationSeconds: number
  codec: string
  title: string
  artist: string
  album: string
  hasCover: boolean
  coverDataUri: string
}

export interface PlayerState {
  state: PlaybackStatus
  title: string
  time: number
  duration: number
  totalFrames: number
  fps: number
  speed: number
  volume: number
  playMode: number
}

export interface LibraryEntry {
  id: string
  title: string
  subtitle?: string
  directory: string
  durationSeconds: number
  totalFrames: number
  hasMotion: boolean
  hasFacial: boolean
  hasCamera: boolean
  hasAudio: boolean
  audioFile?: string
  hasCover?: boolean
}

export interface LibraryState {
  revision: number
  entries: LibraryEntry[]
  refreshing: boolean
  notice: string
}

export interface LoadedState {
  motionKey: string
  generation: number
  injectionSession: number
  sourceMissing: boolean
  sourceChanged: boolean
}

export interface PermissionState {
  injectionAllowed: boolean
  notice: string
}

export interface SceneActor {
  token: number
  label: string
  selected: boolean
}

export interface ActorsState {
  epoch: number
  actors: SceneActor[]
  targetLabel: string
  boundLabel: string
}

export interface CameraTransform {
  position: Vector3
  rotation: Vector3
}

export interface CameraSettings {
  enabled: boolean
  mode: number
  followBehavior: number
  followTarget: number
  followActor: boolean
  limitToScene: boolean
  boundsMin: Vector3
  boundsMax: Vector3
  followOffset: Vector3
  targetOffset: Vector3
  vmdOffset: Vector3
  vmdRangeScale: number
  followFactor: Vector3
  followRate: Vector3
  followFov: number
  srpFinalOverwriteFallback: boolean
  actorShadowExperimentMode: number
  transformTrack: CameraTransform
  transformFollowMove: CameraTransform
  transformLookAt: CameraTransform
  usePerModeTransforms: boolean
}

export interface CameraState {
  settings: CameraSettings
  available: boolean
  owned: boolean
  hasTrack: boolean
  error: string
  followTargetError: string
  notice: string
}

export interface StageSettings {
  hideBackground: boolean
  greenScreenActive: boolean
  greenScreenColor: [number, number, number, number]
  screenDistance: number
  screenScale: number
  screenHeightOffset: number
  overrideCameraClear: boolean
  selectedPresetIndex: number
  customStageKey: string
  loadReplacementKey: string
  stageFloorYOffset: number
  stageYawOffset: number
  stagePosXOffset: number
  stagePosZOffset: number
  lightingEffectsEnabled: boolean
  customNodeControl: boolean
}

export interface StageState {
  settings: StageSettings
  sceneName: string
  statusNotice: string
}

export interface FacialWeights {
  overall: number
  brow: number
  eye: number
  mouth: number
}

export interface FacialState {
  weights: FacialWeights
  notice: string
}

export interface PositionState {
  offset: Vector3
  notice: string
}

export interface EnsembleSlot {
  enabled: boolean
  actorToken: number
  actorHint: string
  motionKey: string
  offset: Vector3
  yawDegrees: number
}

export interface EnsembleSettings {
  enabled: boolean
  masterSlot: number
  cameraSlot: number
  slots: EnsembleSlot[]
}

export interface EnsembleSnapshot {
  settings: EnsembleSettings
  revision: number
  actorEpoch: number
  available: boolean
  verified: boolean
  probeEnabled: boolean
  active: boolean
  notice: string
  physics: string[]
}

export interface ImportProgress {
  active: boolean
  finished: boolean
  succeeded: boolean
  progress: number
  stage: string
  statusText: string
  errorDetail: string
  importedKey?: string
}

export interface ScreenshotCapabilities {
  available: boolean
  runtimeReady: boolean
  cameraAvailable: boolean
  windowWidth: number
  windowHeight: number
  notice: string
}

export interface MotionAppearance {
  theme: number
  uiScale: number
  dpiScale: number
  scaleLimited: boolean
}

export interface MotionStateSnapshot {
  permission: PermissionState
  player: PlayerState
  library: LibraryState
  loaded: LoadedState
  playlist: { mode: number; notice: string }
  actors: ActorsState
  camera: CameraState
  stage: StageState
  facial: FacialState
  position: PositionState
  ensemble: EnsembleSnapshot
  physicsBackend: number
  musicVolumeSupport: number
  musicVolumeNotice: string
  importProgress: ImportProgress | null
  motionsRoot: string
  clock?: ClockState
  audioEffect?: AudioEffectState
}

export interface MotionHello {
  protocol: number
  sessionId: number
  pluginVersion: string
  supportedMethods: string[]
  supportedEvents: string[]
}

// ---- Phase 2 domain types (library import / drop / bone / gaze / stage nodes /
// experiments / telemetry / sole / solePitch / studio). Shapes mirror the ImGui
// reference (GakuMotion dev src/gui + Core headers); see motion-bridge-protocol.md.

export interface TrackTiming {
  speed: number
  offsetSeconds: number
}

export interface PerformanceTiming {
  motion: TrackTiming
  facial: TrackTiming
  camera: TrackTiming
  audio: TrackTiming
}

export type VmdTendency = 'invalid' | 'motionOnly' | 'motionAndFacial' | 'facialOnly' | 'cameraOnly' | 'empty'

export interface VmdProbe {
  path: string
  valid: boolean
  isGkCamera: boolean
  tendency: VmdTendency
  boneFrames: number
  bodyBoneFrames: number
  eyeBoneFrames: number
  morphFrames: number
  cameraFrames: number
}

export interface ImportClassification {
  suggestedTitle: string
  motionCandidate: string
  facialCandidate: string
  cameraCandidate: string
  audioCandidate: string
  unsupportedAudioCount: number
}

export interface ImportRequest {
  title: string
  subtitle?: string
  editKey: string
  motionFile: string
  facialFile: string
  cameraFile: string
  audioFile: string
  disableMorph: boolean
  clearCamera: boolean
  clearAudio: boolean
  coverDataUri?: string
  clearCover?: boolean
  fps: number
  sourceFps?: number
  rebakeOutput?: boolean
  timing: PerformanceTiming
}

export interface EditEntryInfo {
  key: string
  title: string
  subtitle?: string
  hasFacial: boolean
  hasCamera: boolean
  hasAudio: boolean
  audioFile?: string
  hasCover?: boolean
  timing: PerformanceTiming
}

export interface DropPeek {
  visual: 'none' | 'armed' | 'hover' | 'invalid'
  fileCount: number
  accepted: number
  hostElevated: boolean
  hostBlocked: boolean
}

export interface BoneOverrideEntry {
  humanoidId: number
  boneName: string
  enabled: boolean
  spaceWorld: boolean
  eulerDeg: Vector3
  isDynamic: boolean
}

export interface BoneState {
  masterEnabled: boolean
  overrides: BoneOverrideEntry[]
  notice: string
}

export interface GazeSettings {
  mode: number
  desktopLookAtEnabled: boolean
  eyeWeight: number
  headWeight: number
  bodyWeight: number
}

export interface GazeState {
  settings: GazeSettings
  runtimeAvailable: boolean
  vrifyQueryAvailable: boolean
  vrifyLiveGazeActive: boolean
  notice: string
}

export interface StageNode {
  index: number
  name: string
  displayName: string
  parentIndex: number
  depth: number
  hasChildren: boolean
  currentlyActive: boolean
  isColliderLocked: boolean
  isIdentifiedBackground: boolean
  isLightingRoot: boolean
  isLightingNode: boolean
}

export interface ExperimentSettings {
  mikuMouthFit: boolean
  mikuEyeFit: boolean
  mikuFitAllowUnverified: boolean
  waistSupport: boolean
  beforeQuartz: boolean
  muteNativeSoundEffects: boolean
}

export interface ExperimentsState {
  settings: ExperimentSettings
  mouthFitStatus: string
  eyeFitStatus: string
  fittedFaceLabel: string
  notice: string
}

export interface TelemetrySnapshot {
  statusLine: string
  player: {
    state: number
    stateName: string
    time: number
    duration: number
    totalFrames: number
    fps: number
    boneCount: number
    loadedMotionFile: string
  }
  actor: {
    actorLabel: string
    boundBoneCount: number
    totalBoneCount: number
  }
}

export interface Quaternion {
  x: number
  y: number
  z: number
  w: number
}

export interface SoleSettings {
  enabled: boolean
  useBootProxy: boolean
  planeCalibrated: boolean
  shoeCalibrated: boolean
  planeNormal: Vector3
  planeOffsetM: number
  displayBandM: number
  points: Vector3[][]
}

export interface SolePoint {
  world: Vector3
  signedDistanceM: number
  penetrationM: number
}

export interface SolePatch {
  corners: SolePoint[]
  minimumDistanceM: number
  maximumDistanceM: number
  maximumPenetrationM: number
}

export interface SoleFootPose {
  valid: boolean
  hip: Vector3
  knee: Vector3
  ankle: Vector3
  toe: Vector3
  footRotation: Quaternion
}

export interface SoleFoot {
  valid: boolean
  usingBootProxy: boolean
  pose: SoleFootPose
  points: SolePoint[]
  patches: SolePatch[]
  minimumDistanceM: number
  maximumPenetrationM: number
}

export interface SoleSnapshot {
  available: boolean
  calibrated: boolean
  clipTime: number
  feet: SoleFoot[]
}

export interface SoleState {
  settings: SoleSettings
  snapshot: SoleSnapshot
  generation: number
  notice: string
}

export interface SolePitchSettings {
  enabled: boolean
  maxPitchDegrees: number
  useMeshEnvelope: boolean
}

export interface SolePitchFoot {
  status: string
  applied: boolean
  setterReadbackMatches: boolean
  measuredAfterValid: boolean
  measuredAfterMinimumDistanceM: number
  solution: {
    valid: boolean
    pitchDegrees: number
    usedMeshEnvelope: boolean
    beforePenetrationM: number
  }
}

export interface SolePitchState {
  settings: SolePitchSettings
  clipTime: number
  feet: SolePitchFoot[]
  notice: string
}

export type RgbColor = [number, number, number]

export interface StudioLighting {
  enabled: boolean
  actorStyle: boolean
  decalEnabled: boolean
  keyColor: RgbColor
  fillColor: RgbColor
  rimColor: RgbColor
  decalTint: RgbColor
  keyIntensity: number
  fillIntensity: number
  rimIntensity: number
  keyYaw: number
  keyPitch: number
  fillYaw: number
  fillPitch: number
  rimYaw: number
  rimPitch: number
  actorIntensity: number
  rimPower: number
  decalIntensityScale: number
}

export interface StudioVideo {
  enabled: boolean
  paused: boolean
  loop: boolean
  path: string
  distance: number
  width: number
  offsetX: number
  offsetY: number
}

export interface StudioSettings {
  lighting: StudioLighting
  video: StudioVideo
}

export interface StudioState {
  settings: StudioSettings
  revision: number
  lightingNotice: string
  videoNotice: string
}

export interface MotionMethods {
  'bridge.hello': { params: void; result: { protocol: number; sessionId: number } }
  'bridge.ping': { params: void; result: { pong: boolean } }
  'motion.hello': { params: void; result: MotionHello }
  'state.get': { params: void; result: MotionStateSnapshot }
  'player.play': { params: void; result: Record<string, never> }
  'player.pause': { params: void; result: Record<string, never> }
  'player.resume': { params: void; result: Record<string, never> }
  'player.stop': { params: void; result: Record<string, never> }
  'player.cycle': { params: { delta: number }; result: Record<string, never> }
  'player.seek': { params: { seconds: number }; result: Record<string, never> }
  'player.seekFrame': { params: { frame: number }; result: Record<string, never> }
  'player.stepFrame': { params: { delta: number }; result: Record<string, never> }
  'player.scrubBegin': { params: { seconds: number }; result: Record<string, never> }
  'player.scrubUpdate': { params: { seconds: number }; result: Record<string, never> }
  'player.scrubEnd': { params: { seconds: number }; result: Record<string, never> }
  'player.setSpeed': { params: { speed: number }; result: Record<string, never> }
  'player.setVolume': { params: { volume: number }; result: Record<string, never> }
  'player.saveVolume': { params: void; result: Record<string, never> }
  'player.reload': { params: void; result: Record<string, never> }
  'playlist.setMode': { params: { mode: number }; result: Record<string, never> }
  'library.refresh': { params: void; result: { started: boolean } }
  'library.select': { params: { key: string; autoplay: boolean; revision: number }; result: { key: string } }
  'library.delete': { params: { key: string; revision: number }; result: Record<string, never> }
  'library.openFolder': { params: void; result: Record<string, never> }
  'actors.selectTarget': { params: { token: number; epoch: number }; result: Record<string, never> }
  'facial.setWeights': { params: FacialWeights; result: Record<string, never> }
  'facial.save': { params: void; result: Record<string, never> }
  'position.set': { params: Vector3; result: Record<string, never> }
  'position.save': { params: void; result: Record<string, never> }
  'camera.set': { params: { settings: CameraSettings }; result: Record<string, never> }
  'camera.save': { params: void; result: Record<string, never> }
  'stage.set': { params: { settings: StageSettings }; result: Record<string, never> }
  'settings.setPhysics': { params: { backend: number }; result: Record<string, never> }
  'shell.setTextInputActive': { params: { active: boolean }; result: Record<string, never> }
  'appearance.get': { params: void; result: MotionAppearance }
  // ---- Phase 2 methods: gated by motion.hello capabilities ----
  'dialog.pickFile': { params: { kind: number; multiple: boolean }; result: { dialogId: number } }
  'dialog.poll': {
    params: { dialogId: number }
    result: { state: 'pending' | 'accepted' | 'cancelled' | 'failed'; paths: string[]; notice: string }
  }
  'library.import': { params: { request: ImportRequest }; result: { started: boolean } }
  'library.probeFiles': {
    params: { paths: string[] }
    result: { classification: ImportClassification; probes: VmdProbe[] }
  }
  'library.startEdit': { params: { key: string; revision: number }; result: EditEntryInfo }
  'library.probeAudio': { params: { path: string; includeCover?: boolean }; result: AudioProbeInfo }
  'library.getCover': { params: { key?: string; directory?: string }; result: { hasCover: boolean; coverDataUri: string } }
  'library.setCover': {
    params: { key: string; coverDataUri?: string; clear?: boolean }
    result: { hasCover: boolean; coverDataUri: string; revision: number }
  }
  'library.readImagePreview': { params: { path: string }; result: { dataUri: string } }
  'screenshot.capabilities': { params: void; result: ScreenshotCapabilities }
  'screenshot.capture': { params: void; result: { dataUri: string; width: number; height: number } }
  'clock.setRunMode': { params: { runMode: ClockRunMode }; result: ClockState }
  'clock.configure': { params: { source?: ClockSource; fpsNumerator?: number; fpsDenominator?: number }; result: ClockState }
  'audioEffect.set': { params: { presetId: string }; result: AudioEffectState }
  'drop.setTarget': { params: { enabled: boolean }; result: Record<string, never> }
  'drop.poll': { params: void; result: { files: string[] } }
  'drop.peek': { params: void; result: DropPeek }
  'ensemble.set': {
    params: { settings: EnsembleSettings; revision: number; actorEpoch: number }
    result: Record<string, never>
  }
  'ensemble.presetSave': { params: { name: string; settings: EnsembleSettings }; result: Record<string, never> }
  'ensemble.presetLoad': { params: { name: string }; result: { settings: EnsembleSettings } }
  'ensemble.probe': { params: { enabled: boolean }; result: Record<string, never> }
  'bone.get': { params: void; result: BoneState }
  'bone.set': {
    params: {
      humanoidId: number
      boneName: string
      pitch: number
      yaw: number
      roll: number
      spaceWorld: boolean
      enabled: boolean
      symmetrical: boolean
    }
    result: Record<string, never>
  }
  'bone.reset': { params: { humanoidId: number; boneName: string; symmetrical: boolean }; result: Record<string, never> }
  'bone.resetAll': { params: void; result: Record<string, never> }
  'bone.setMaster': { params: { enabled: boolean }; result: Record<string, never> }
  'bone.reload': { params: void; result: Record<string, never> }
  'bone.saveToMotion': { params: void; result: Record<string, never> }
  'bone.saveToGlobal': { params: void; result: Record<string, never> }
  'gaze.get': { params: void; result: GazeState }
  'gaze.set': { params: { settings: GazeSettings }; result: Record<string, never> }
  'stage.scan': { params: void; result: { nodes: StageNode[] } }
  'stage.toggleNode': { params: { index: number; active: boolean }; result: Record<string, never> }
  'stage.setLoadReplacement': { params: { key: string }; result: { notice: string } }
  'experiments.get': { params: void; result: ExperimentsState }
  'experiments.set': { params: { patch: Partial<ExperimentSettings> }; result: Record<string, never> }
  'telemetry.get': { params: void; result: TelemetrySnapshot }
  'sole.get': { params: void; result: SoleState }
  'sole.set': { params: { settings: SoleSettings }; result: Record<string, never> }
  'sole.save': { params: void; result: Record<string, never> }
  'sole.load': { params: void; result: Record<string, never> }
  'solePitch.get': { params: void; result: SolePitchState }
  'solePitch.set': { params: { settings: SolePitchSettings }; result: Record<string, never> }
  'studio.get': { params: void; result: StudioState }
  'studio.set': { params: { settings: StudioSettings }; result: Record<string, never> }
  'studio.reset': { params: void; result: Record<string, never> }
}

export interface MotionEvents {
  'player.stateChanged': PlayerState
  'permission.changed': PermissionState
  'library.changed': LibraryState
  'loaded.changed': LoadedState
  'playlist.changed': { mode: number; notice: string }
  'actors.changed': ActorsState
  'camera.changed': CameraState
  'stage.changed': StageState
  'facial.changed': FacialState
  'position.changed': PositionState
  'ensemble.changed': EnsembleSnapshot
  'settings.changed': { physicsBackend: number }
  'import.progress': ImportProgress
  'appearance.changed': MotionAppearance
  'bridge.closed': { reason: string }
  'bone.changed': BoneState
  'gaze.changed': GazeState
  'stage.nodesChanged': { nodes: StageNode[] }
  'experiments.changed': ExperimentsState
  'sole.changed': SoleState
  'solePitch.changed': SolePitchState
  'studio.changed': StudioState
  'clock.changed': ClockState
  'audioEffect.changed': AudioEffectState
}

export type MotionMethodName = keyof MotionMethods
export type MotionEventName = keyof MotionEvents

export interface MotionBridgeClient {
  readonly kind: 'webview2' | 'mock'
  request<M extends MotionMethodName>(
    method: M,
    params?: MotionMethods[M]['params'],
    options?: RequestOptions,
  ): Promise<MotionMethods[M]['result']>
  on<E extends MotionEventName>(event: E, handler: (payload: MotionEvents[E]) => void): () => void
  close(): void
}

export const PLAY_MODES = ['播放一次', '单曲循环', '列表循环', '随机播放'] as const
export const ENSEMBLE_PLAY_MODES = ['演出一次', '整场循环'] as const
export const PLAYBACK_SPEEDS = [0.1, 0.2, 0.5, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0] as const
export const CAMERA_MODES = ['游戏', 'VMD', '跟随', '注视'] as const
export const CAMERA_QUICK_MODES = ['镜头: 游戏', '镜头: VMD', '镜头: 跟随', '镜头: 注视'] as const
export const PHYSICS_BACKENDS = ['关闭', '内建物理', '原生物理（默认，实验性）'] as const
export const CAMERA_FOLLOW_TARGETS = ['髋部', '胸部', '头部'] as const

export const DEFAULT_CAMERA_SETTINGS: CameraSettings = {
  enabled: true,
  mode: 0,
  followBehavior: 1,
  followTarget: 0,
  followActor: false,
  limitToScene: false,
  boundsMin: { x: -5, y: 0.05, z: -5 },
  boundsMax: { x: 5, y: 5, z: 5 },
  followOffset: { x: 0, y: 0.6, z: 3 },
  targetOffset: { x: 0, y: 0.4, z: 0 },
  vmdOffset: { x: 0, y: 0, z: 0 },
  vmdRangeScale: 1,
  followFactor: { x: 1, y: 1, z: 1 },
  followRate: { x: 0.1, y: 0.1, z: 0.1 },
  followFov: 45,
  srpFinalOverwriteFallback: false,
  actorShadowExperimentMode: 0,
  transformTrack: { position: { x: 0, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0 } },
  transformFollowMove: { position: { x: 0, y: 0.6, z: 3 }, rotation: { x: 0, y: 0, z: 0 } },
  transformLookAt: { position: { x: 0, y: 0.6, z: 3 }, rotation: { x: 0, y: 0, z: 0 } },
  usePerModeTransforms: false,
}

export const DEFAULT_STAGE_SETTINGS: StageSettings = {
  hideBackground: false,
  greenScreenActive: false,
  greenScreenColor: [0, 1, 0, 1],
  screenDistance: 4,
  screenScale: 35,
  screenHeightOffset: 1,
  overrideCameraClear: false,
  selectedPresetIndex: 0,
  customStageKey: '',
  loadReplacementKey: '',
  stageFloorYOffset: 0,
  stageYawOffset: 0,
  stagePosXOffset: 0,
  stagePosZOffset: 0,
  lightingEffectsEnabled: true,
  customNodeControl: false,
}

export const DEFAULT_FACIAL_WEIGHTS: FacialWeights = { overall: 1, brow: 1, eye: 1, mouth: 1 }

export const DEFAULT_CLOCK_STATE: ClockState = {
  runMode: 'realtime',
  configuredSource: 'audio',
  effectiveSource: 'audio',
  fpsNumerator: 60,
  fpsDenominator: 1,
  waitingForRenderComplete: false,
  leaseId: 0,
  leaseOwner: '',
  evaluatedFrameSerial: 0,
}

export const DEFAULT_AUDIO_EFFECT_STATE: AudioEffectState = {
  available: false,
  activeId: 'flat',
  presets: [{ id: 'flat', name: '原声', category: 'flat', description: '直通输出' }],
}

export const DEFAULT_PLAYER_STATE: PlayerState = {
  state: 'stopped',
  title: '未加载曲目',
  time: 0,
  duration: 0,
  totalFrames: 0,
  fps: 60,
  speed: 1,
  volume: 1,
  playMode: 1,
}

// ---- Phase 2 constants ----

// dialog.pickFile kind codes (wire numbers, filters match GuiOverlayCommon.hpp).
export const DIALOG_KIND = { all: 0, motion: 1, facial: 2, camera: 3, audio: 4, video: 5, image: 6 } as const

// Import slot sentinel values. They are UI markers, never disk paths; every
// "is a real file selected" check must exclude them (ImportSlot in GuiLibraryPage).
export const IMPORT_SLOT = {
  empty: '（留空）',
  clear: '（留空 / 清除）',
  keepTrack: '[保持当前已导入轨道]',
  keepMotion: '[保持当前动作]',
  autoFacial: '（自动：沿用动作 VMD 内表情）',
  autoAudio: '（自动：同名音频或 song.*）',
  skip: '（不导入）',
} as const

export function isSlotPlaceholder(path: string): boolean {
  return (
    path === '' ||
    path === IMPORT_SLOT.empty ||
    path === IMPORT_SLOT.clear ||
    path === IMPORT_SLOT.keepTrack ||
    path === IMPORT_SLOT.keepMotion ||
    path === IMPORT_SLOT.autoFacial ||
    path === IMPORT_SLOT.autoAudio ||
    path === IMPORT_SLOT.skip
  )
}

export const DEFAULT_TRACK_TIMING: TrackTiming = { speed: 1, offsetSeconds: 0 }

export function defaultPerformanceTiming(): PerformanceTiming {
  return {
    motion: { ...DEFAULT_TRACK_TIMING },
    facial: { ...DEFAULT_TRACK_TIMING },
    camera: { ...DEFAULT_TRACK_TIMING },
    audio: { ...DEFAULT_TRACK_TIMING },
  }
}

export function defaultImportRequest(): ImportRequest {
  return {
    title: '',
    subtitle: '',
    editKey: '',
    motionFile: '',
    facialFile: '',
    cameraFile: '',
    audioFile: '',
    disableMorph: false,
    clearCamera: false,
    clearAudio: false,
    fps: 0,
    sourceFps: 0,
    rebakeOutput: false,
    timing: defaultPerformanceTiming(),
  }
}

export const VMD_BADGE_TEXT: Record<VmdTendency, string> = {
  invalid: '[无效VMD]',
  motionOnly: '[动作]',
  motionAndFacial: '[动作+表情]',
  facialOnly: '[表情]',
  cameraOnly: '[镜头]',
  empty: '[空VMD]',
}

export function vmdBadgeText(probe: VmdProbe): string {
  if (probe.isGkCamera) return '[烘焙镜头]'
  return VMD_BADGE_TEXT[probe.tendency] ?? '[无效VMD]'
}

export const ENSEMBLE_INITIAL_SLOTS = 1

export function ensembleSlotName(index: number): string {
  return `演员${index + 1}`
}

// Developer-only photography stage replacement (GuiStageLoadReplacement.hpp).
export const STAGE_REPLACEMENT_OPTIONS: Array<{ key: string; label: string }> = [
  { key: '', label: '原生摄影背景' },
  { key: 'env_3d_live_all001-00-00-noon', label: '通用 Live 舞台（灯光未完成）' },
  { key: 'env_3d_prod_hifstage-00-00-noon', label: '初星舞台（灯光未完成）' },
  { key: 'env_3d_live_courtyard-00-00-noon', label: '中庭舞台（日中）' },
]

export const GAZE_MODES = [
  'Auto（VR 时自动避让 VRify）',
  'ForceOff（保留纯 VMD 头动）',
  'DesktopOn（强制桌面追焦）',
] as const

export const DEFAULT_GAZE_SETTINGS: GazeSettings = {
  mode: 0,
  desktopLookAtEnabled: false,
  eyeWeight: 1,
  headWeight: 0.5,
  bodyWeight: 0.25,
}

export const DEFAULT_EXPERIMENT_SETTINGS: ExperimentSettings = {
  mikuMouthFit: true,
  mikuEyeFit: true,
  mikuFitAllowUnverified: false,
  waistSupport: true,
  beforeQuartz: false,
  muteNativeSoundEffects: true,
}

export function defaultSoleSettings(): SoleSettings {
  return {
    enabled: false,
    useBootProxy: false,
    planeCalibrated: false,
    shoeCalibrated: false,
    planeNormal: { x: 0, y: 1, z: 0 },
    planeOffsetM: 0,
    displayBandM: 0.002,
    points: [0, 1].map(() => [0, 1, 2].map(() => ({ x: 0, y: 0, z: 0 }))),
  }
}

export const DEFAULT_SOLE_PITCH_SETTINGS: SolePitchSettings = {
  enabled: false,
  maxPitchDegrees: 20,
  useMeshEnvelope: false,
}

export function defaultStudioSettings(): StudioSettings {
  return {
    lighting: {
      enabled: false,
      actorStyle: true,
      decalEnabled: false,
      keyColor: [1, 0.9, 0.8],
      fillColor: [0.65, 0.78, 1],
      rimColor: [0.6, 0.7, 1],
      decalTint: [1, 1, 1],
      keyIntensity: 1.4,
      fillIntensity: 0.5,
      rimIntensity: 1,
      keyYaw: -30,
      keyPitch: 35,
      fillYaw: 40,
      fillPitch: 20,
      rimYaw: 170,
      rimPitch: 40,
      actorIntensity: 1,
      rimPower: 0.5,
      decalIntensityScale: 1,
    },
    video: {
      enabled: false,
      paused: false,
      loop: true,
      path: '',
      distance: 1.5,
      width: 1.2,
      offsetX: 0,
      offsetY: 0,
    },
  }
}

// ---- Bone dictionary (GkBoneOverride.hpp / GkGuiTabBone.hpp, 51 editable
// humanoid bones in six groups; hid 21..23 are not exposed by the reference). ----

export interface BoneDef {
  hid: number
  name: string
  labelZh: string
}

export interface BoneGroup {
  title: string
  bones: BoneDef[]
}

export const BONE_GROUPS: BoneGroup[] = [
  {
    title: '躯干 (Torso)',
    bones: [
      { hid: 0, name: 'Hips', labelZh: '骨盆 (Hips)' },
      { hid: 7, name: 'Spine', labelZh: '脊柱 (Spine)' },
      { hid: 8, name: 'Chest', labelZh: '胸腔 (Chest)' },
    ],
  },
  {
    title: '头颈 (Head & Neck)',
    bones: [
      { hid: 9, name: 'Neck', labelZh: '颈部 (Neck)' },
      { hid: 10, name: 'Head', labelZh: '头部 (Head)' },
    ],
  },
  {
    title: '上肢双臂 (Arms)',
    bones: [
      { hid: 11, name: 'LeftShoulder', labelZh: '左肩 (L Shoulder)' },
      { hid: 12, name: 'RightShoulder', labelZh: '右肩 (R Shoulder)' },
      { hid: 13, name: 'LeftUpperArm', labelZh: '左上臂 (L UpperArm)' },
      { hid: 14, name: 'RightUpperArm', labelZh: '右上臂 (R UpperArm)' },
      { hid: 15, name: 'LeftLowerArm', labelZh: '左小臂/手肘 (L Forearm)' },
      { hid: 16, name: 'RightLowerArm', labelZh: '右小臂/手肘 (R Forearm)' },
      { hid: 17, name: 'LeftHand', labelZh: '左手 (L Hand)' },
      { hid: 18, name: 'RightHand', labelZh: '右手 (R Hand)' },
    ],
  },
  {
    title: '下肢双腿 (Legs)',
    bones: [
      { hid: 1, name: 'LeftUpperLeg', labelZh: '左大腿 (L Thigh)' },
      { hid: 2, name: 'RightUpperLeg', labelZh: '右大腿 (R Thigh)' },
      { hid: 3, name: 'LeftLowerLeg', labelZh: '左小腿/膝盖 (L Calf)' },
      { hid: 4, name: 'RightLowerLeg', labelZh: '右小腿/膝盖 (R Calf)' },
      { hid: 5, name: 'LeftFoot', labelZh: '左足踝 (L Foot)' },
      { hid: 6, name: 'RightFoot', labelZh: '右足踝 (R Foot)' },
      { hid: 19, name: 'LeftToes', labelZh: '左脚趾 (L Toes)' },
      { hid: 20, name: 'RightToes', labelZh: '右脚趾 (R Toes)' },
    ],
  },
  {
    title: '左手手指 (L Fingers)',
    bones: [
      { hid: 24, name: 'LeftThumbProximal', labelZh: '左拇指根' },
      { hid: 25, name: 'LeftThumbIntermediate', labelZh: '左拇指中' },
      { hid: 26, name: 'LeftThumbDistal', labelZh: '左拇指尖' },
      { hid: 27, name: 'LeftIndexProximal', labelZh: '左食指根' },
      { hid: 28, name: 'LeftIndexIntermediate', labelZh: '左食指中' },
      { hid: 29, name: 'LeftIndexDistal', labelZh: '左食指尖' },
      { hid: 30, name: 'LeftMiddleProximal', labelZh: '左中指根' },
      { hid: 31, name: 'LeftMiddleIntermediate', labelZh: '左中指中' },
      { hid: 32, name: 'LeftMiddleDistal', labelZh: '左中指尖' },
      { hid: 33, name: 'LeftRingProximal', labelZh: '左无名根' },
      { hid: 34, name: 'LeftRingIntermediate', labelZh: '左无名中' },
      { hid: 35, name: 'LeftRingDistal', labelZh: '左无名尖' },
      { hid: 36, name: 'LeftLittleProximal', labelZh: '左小指根' },
      { hid: 37, name: 'LeftLittleIntermediate', labelZh: '左小指中' },
      { hid: 38, name: 'LeftLittleDistal', labelZh: '左小指尖' },
    ],
  },
  {
    title: '右手手指 (R Fingers)',
    bones: [
      { hid: 39, name: 'RightThumbProximal', labelZh: '右拇指根' },
      { hid: 40, name: 'RightThumbIntermediate', labelZh: '右拇指中' },
      { hid: 41, name: 'RightThumbDistal', labelZh: '右拇指尖' },
      { hid: 42, name: 'RightIndexProximal', labelZh: '右食指根' },
      { hid: 43, name: 'RightIndexIntermediate', labelZh: '右食指中' },
      { hid: 44, name: 'RightIndexDistal', labelZh: '右食指尖' },
      { hid: 45, name: 'RightMiddleProximal', labelZh: '右中指根' },
      { hid: 46, name: 'RightMiddleIntermediate', labelZh: '右中指中' },
      { hid: 47, name: 'RightMiddleDistal', labelZh: '右中指尖' },
      { hid: 48, name: 'RightRingProximal', labelZh: '右无名根' },
      { hid: 49, name: 'RightRingIntermediate', labelZh: '右无名中' },
      { hid: 50, name: 'RightRingDistal', labelZh: '右无名尖' },
      { hid: 51, name: 'RightLittleProximal', labelZh: '右小指根' },
      { hid: 52, name: 'RightLittleIntermediate', labelZh: '右小指中' },
      { hid: 53, name: 'RightLittleDistal', labelZh: '右小指尖' },
    ],
  },
]

export const ALL_BONES: BoneDef[] = BONE_GROUPS.flatMap((group) => group.bones)

export function boneNameByHid(hid: number): string {
  return ALL_BONES.find((bone) => bone.hid === hid)?.name ?? ''
}

export function symmetricalHid(hid: number): number {
  const pairs: Record<number, number> = {
    11: 12, 12: 11, 13: 14, 14: 13, 15: 16, 16: 15, 17: 18, 18: 17,
    1: 2, 2: 1, 3: 4, 4: 3, 5: 6, 6: 5, 19: 20, 20: 19,
  }
  if (hid in pairs) return pairs[hid]
  if (hid >= 24 && hid <= 38) return hid + 15
  if (hid >= 39 && hid <= 53) return hid - 15
  return -1
}
