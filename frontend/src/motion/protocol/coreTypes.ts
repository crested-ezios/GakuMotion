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

export interface LibraryPositionEntry {
  slot: number
  x: number
  y: number
  z: number
  yawDegrees: number
  hasMotion: boolean
  hasFacial: boolean
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
  isGroup: boolean
  positionCount: number
  positions: LibraryPositionEntry[]
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
  character: string
  displayName: string
  shortName: string
  selected: boolean
}

export interface ActorsState {
  epoch: number
  actors: SceneActor[]
  targetLabel: string
  boundLabel: string
}

export interface ActorPortraitResult {
  ok: boolean
  character: string
  cached: boolean
  dataUri: string | null
}

export interface StageSlotOffset {
  x: number
  y: number
  z: number
  yawDegrees: number
}

export interface StageSlotState {
  slotIndex: number
  occupied: boolean
  actorToken: number
  actorInstanceId: number
  character: string
  shortName: string
  displayName: string
  isOverflow: boolean
  hasTrackMotion: boolean
  sourceSlotIndex: number
  placementDirty: boolean
  hasActorBoneOverrides: boolean
  physicsBackend: string
  offset: StageSlotOffset
  facialWeights: FacialWeights
}

export interface StageFormation {
  active: boolean
  available: boolean
  verified: boolean
  isGroupSong: boolean
  trackPositionCount: number
  slotCount: number
  selectedSlotIndex: number
  revision: number
  actorEpoch: number
  notice: string
  slots: StageSlotState[]
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
  followSlotIndex: number
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
  formation: StageFormation
  camera: CameraState
  stage: StageState
  facial: FacialState
  position: PositionState
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
