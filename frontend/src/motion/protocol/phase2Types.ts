import type { LibraryPositionEntry, TrackTiming, Vector3 } from './coreTypes'

export type { TrackTiming }

// Phase 2 domain types (library import / drop / bone / gaze / stage nodes /
// experiments / telemetry / sole / solePitch / studio). Shapes mirror the ImGui
// reference (GakuMotion dev src/gui + Core headers); see motion-bridge-protocol.md.

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
  hasRootOrigin?: boolean
  rootOrigin?: Vector3
}

export interface ImportClassification {
  suggestedTitle: string
  motionCandidate: string
  motionCandidates: string[]
  facialCandidate: string
  cameraCandidate: string
  audioCandidate: string
  unsupportedAudioCount: number
}

export interface ImportPositionRequest {
  slot: number
  sourceSlot?: number
  motionFile?: string
  motionVmd?: string
  facialFile?: string
  morphVmd?: string
  disableMorph?: boolean
  clearMorph?: boolean
  sourceFps?: number
  rebakeOutput?: boolean
  preserveExisting?: boolean
  hasExplicitPlacement?: boolean
  timing?: {
    motion?: TrackTiming
    facial?: TrackTiming
  }
  x?: number
  y?: number
  z?: number
  yawDegrees?: number
}

export interface ImportRequest {
  title: string
  subtitle?: string
  editKey: string
  motionFile: string
  motionVmd?: string
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
  isGroup?: boolean
  positions?: ImportPositionRequest[]
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
  isGroup?: boolean
  positionCount?: number
  positions?: LibraryPositionEntry[]
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
