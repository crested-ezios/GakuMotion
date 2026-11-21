import { useSyncExternalStore } from 'react'
import { getMotionBridge, MotionMockBridgeClient, setMotionBridge } from './bridge'
import { cameraControlModeIndex, setCameraControlMode, syncLegacyTransforms } from './cameraMode'
import {
  BridgeError,
  DEFAULT_AUDIO_EFFECT_STATE,
  DEFAULT_CAMERA_SETTINGS,
  DEFAULT_CLOCK_STATE,
  DEFAULT_FACIAL_WEIGHTS,
  DEFAULT_PLAYER_STATE,
  DEFAULT_STAGE_FORMATION,
  DEFAULT_STAGE_SETTINGS,
  DEFAULT_EXPERIMENT_SETTINGS,
  DEFAULT_GAZE_SETTINGS,
  DEFAULT_SOLE_PITCH_SETTINGS,
  IMPORT_SLOT,
  defaultImportRequest,
  defaultPerformanceTiming,
  defaultSoleSettings,
  defaultStudioSettings,
  isSlotPlaceholder,
  boneNameByHid,
  type ActorsState,
  type AudioEffectState,
  type AudioProbeInfo,
  type BoneState,
  type CameraSettings,
  type CameraState,
  type ClockRunMode,
  type ClockSource,
  type ClockState,
  type EditEntryInfo,
  type ExperimentsState,
  type FacialWeights,
  type GazeSettings,
  type GazeState,
  type ImportProgress,
  type ImportRequest,
  type PerformanceTiming,
  type LibraryEntry,
  type LibraryPositionEntry,
  type LibraryState,
  type LoadedState,
  type MotionAppearance,
  type MotionBridgeClient,
  type MotionMethodName,
  type MotionMethods,
  type MotionStateSnapshot,
  type PermissionState,
  type PlayerState,
  type SceneActor,
  type ScreenshotCapabilities,
  type SolePitchState,
  type SoleSettings,
  type SoleState,
  type StageFormation,
  type StageNode,
  type StageSlotOffset,
  type StageSlotState,
  type StageState,
  type StudioSettings,
  type StudioState,
  type TelemetrySnapshot,
  type TrackTiming,
  type Vector3,
  type VmdProbe,
} from './protocol'
import { parseTransportFrame, parseTransportTime } from './time'
import { deepEqual } from './utils'
import {
  canUseAsMotion,
  classifyImportFiles,
  isSupportedAudioExtension,
  isSupportedImageExtension,
  isSupportedLibraryFile,
  validateImportForm,
  validateSole,
  validateStudio,
} from './validation'

export type MotionPage = 'library' | 'motion' | 'camera' | 'stage' | 'settings' | 'developer'

export type LibraryFilter = 'all' | 'group' | 'camera' | 'audio' | 'facial' | 'motionOnly'
export type LibrarySort = 'default' | 'title' | 'durationDesc' | 'durationAsc'

export interface PendingDelete {
  key: string
  directory: string
  revision: number
}

export type ImportSlotKey = 'motion' | 'facial' | 'camera' | 'audio'

export interface ImportSlotState {
  candidates: string[]
  selection: number
}

export interface ImportPositionFormState {
  motion: ImportSlotState
  facial: ImportSlotState
  x: number
  y: number
  z: number
  yawDegrees: number
  preserveExisting: boolean
}

export type ImportCoverMode = 'auto' | 'custom' | 'clear'

export interface ImportFormState {
  open: boolean
  mode: 'new' | 'edit'
  editKey: string
  editRevision: number
  editInfo: EditEntryInfo | null
  title: string
  subtitle: string
  sourceFps: number
  fps: number
  timing: PerformanceTiming
  positions: ImportPositionFormState[]
  slots: Record<ImportSlotKey, ImportSlotState>
  unsupportedAudio: number
  probes: Record<string, VmdProbe>
  audioProbe: AudioProbeInfo | null
  coverMode: ImportCoverMode
  coverDataUri: string
  activeTrackWarning: string
  pendingStopAndSave: boolean
}

export interface CoverEditorState {
  open: boolean
  owner: 'library' | 'transport' | 'import'
  mode: 'track' | 'import'
  trackKey: string
  trackTitle: string
  sourceImageUri: string
  saving: boolean
  capturing: boolean
  screenshotCaps: ScreenshotCapabilities | null
  error: string
}

export interface ImportUiState {
  visible: boolean
  awaitingStart: boolean
  startTime: number
  minDisplayUntil: number
  displayProgress: number
  displayStatus: string
  indeterminate: boolean
  lastProgress: ImportProgress | null
  errorModalOpen: boolean
  errorDetail: string
}

export type DialogTarget =
  | 'library-add'
  | 'append'
  | ImportSlotKey
  | 'video'
  | 'cover-image'
  | `position-motion:${number}`
  | `position-facial:${number}`

export interface DropUiState {
  visual: 'none' | 'armed' | 'hover' | 'invalid'
  fileCount: number
  accepted: number
  hostElevated: boolean
  hostBlocked: boolean
}

function emptyCoverEditor(): CoverEditorState {
  return {
    open: false,
    owner: 'library',
    mode: 'track',
    trackKey: '',
    trackTitle: '',
    sourceImageUri: '',
    saving: false,
    capturing: false,
    screenshotCaps: null,
    error: '',
  }
}

function emptyImportPosition(): ImportPositionFormState {
  return {
    motion: { candidates: [], selection: 0 },
    facial: { candidates: [], selection: 0 },
    x: 0,
    y: 0,
    z: 0,
    yawDegrees: 0,
    preserveExisting: false,
  }
}

function emptyImportForm(): ImportFormState {
  const firstPos = emptyImportPosition()
  return {
    open: false,
    mode: 'new',
    editKey: '',
    editRevision: 0,
    editInfo: null,
    title: '',
    subtitle: '',
    sourceFps: 0,
    fps: 0,
    timing: defaultPerformanceTiming(),
    positions: [firstPos],
    slots: {
      motion: firstPos.motion,
      facial: firstPos.facial,
      camera: { candidates: [], selection: 0 },
      audio: { candidates: [], selection: 0 },
    },
    unsupportedAudio: 0,
    probes: {},
    audioProbe: null,
    coverMode: 'auto',
    coverDataUri: '',
    activeTrackWarning: '',
    pendingStopAndSave: false,
  }
}

function emptyImportUi(): ImportUiState {
  return {
    visible: false,
    awaitingStart: false,
    startTime: 0,
    minDisplayUntil: 0,
    displayProgress: 0,
    displayStatus: '',
    indeterminate: false,
    lastProgress: null,
    errorModalOpen: false,
    errorDetail: '',
  }
}

function asObj(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === 'object' ? (value as Record<string, unknown>) : {}
}

function asBool(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback
}

function asNum(value: unknown, fallback = 0): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

function asStr(value: unknown, fallback = ''): string {
  return typeof value === 'string' ? value : fallback
}

function asVec3(value: unknown, fallback: Vector3 = { x: 0, y: 0, z: 0 }): Vector3 {
  const obj = asObj(value)
  return {
    x: asNum(obj.x, fallback.x),
    y: asNum(obj.y, fallback.y),
    z: asNum(obj.z, fallback.z),
  }
}

function normalizeStageNodes(raw: unknown): StageNode[] {
  if (!Array.isArray(raw)) return []
  return raw.map((item, idx) => {
    const o = asObj(item)
    const name = asStr(o.name, `Node_${idx}`)
    return {
      index: asNum(o.index, idx),
      name,
      displayName: asStr(o.displayName, name) || name,
      parentIndex: asNum(o.parentIndex, -1),
      depth: asNum(o.depth, 0),
      hasChildren: asBool(o.hasChildren, false),
      currentlyActive: asBool(o.currentlyActive, true),
      isColliderLocked: asBool(o.isColliderLocked, false),
      isIdentifiedBackground: asBool(o.isIdentifiedBackground, false),
      isLightingRoot: asBool(o.isLightingRoot, false),
      isLightingNode: asBool(o.isLightingNode, false),
    }
  })
}

function normalizeBoneState(raw: unknown): BoneState {
  const obj = asObj(raw)
  const list = Array.isArray(obj.overrides) ? obj.overrides : []
  return {
    masterEnabled: asBool(obj.masterEnabled, true),
    overrides: list.map((entry) => {
      const o = asObj(entry)
      const humanoidId = asNum(o.humanoidId ?? o.hid, 0)
      return {
        humanoidId,
        boneName: asStr(o.boneName ?? o.name, boneNameByHid(humanoidId)),
        enabled: asBool(o.enabled, true),
        spaceWorld: asBool(o.spaceWorld, false),
        eulerDeg: asVec3(o.eulerDeg),
        isDynamic: asBool(o.isDynamic, false),
      }
    }),
    notice: asStr(obj.notice),
  }
}

function normalizeGazeState(raw: unknown): GazeState {
  const obj = asObj(raw)
  const settingsObj = obj.settings && typeof obj.settings === 'object' ? asObj(obj.settings) : obj
  return {
    settings: {
      mode: asNum(settingsObj.mode, DEFAULT_GAZE_SETTINGS.mode),
      desktopLookAtEnabled: asBool(
        settingsObj.desktopLookAtEnabled ?? settingsObj.desktopEnabled,
        DEFAULT_GAZE_SETTINGS.desktopLookAtEnabled,
      ),
      eyeWeight: asNum(settingsObj.eyeWeight, DEFAULT_GAZE_SETTINGS.eyeWeight),
      headWeight: asNum(settingsObj.headWeight, DEFAULT_GAZE_SETTINGS.headWeight),
      bodyWeight: asNum(settingsObj.bodyWeight, DEFAULT_GAZE_SETTINGS.bodyWeight),
    },
    runtimeAvailable: asBool(obj.runtimeAvailable, false),
    vrifyQueryAvailable: asBool(obj.vrifyQueryAvailable, false),
    vrifyLiveGazeActive: asBool(obj.vrifyLiveGazeActive, false),
    notice: asStr(obj.notice),
  }
}

function normalizeExperimentsState(raw: unknown): ExperimentsState {
  const obj = asObj(raw)
  const s = asObj(obj.settings)
  return {
    settings: {
      mikuMouthFit: asBool(s.mikuMouthFit, DEFAULT_EXPERIMENT_SETTINGS.mikuMouthFit),
      mikuEyeFit: asBool(s.mikuEyeFit, DEFAULT_EXPERIMENT_SETTINGS.mikuEyeFit),
      mikuFitAllowUnverified: asBool(
        s.mikuFitAllowUnverified,
        DEFAULT_EXPERIMENT_SETTINGS.mikuFitAllowUnverified,
      ),
      waistSupport: asBool(s.waistSupport, DEFAULT_EXPERIMENT_SETTINGS.waistSupport),
      beforeQuartz: asBool(s.beforeQuartz, DEFAULT_EXPERIMENT_SETTINGS.beforeQuartz),
      muteNativeSoundEffects: asBool(
        s.muteNativeSoundEffects,
        DEFAULT_EXPERIMENT_SETTINGS.muteNativeSoundEffects,
      ),
    },
    mouthFitStatus: asStr(obj.mouthFitStatus, '尚未接管'),
    eyeFitStatus: asStr(obj.eyeFitStatus, '尚未接管'),
    fittedFaceLabel: asStr(obj.fittedFaceLabel),
    notice: asStr(obj.notice),
  }
}

function normalizeSoleState(raw: unknown): SoleState {
  const defaults = defaultSoleSettings()
  const obj = asObj(raw)
  const s = asObj(obj.settings)
  const snap = asObj(obj.snapshot)
  const rawPoints = Array.isArray(s.points) ? s.points : []
  const points: Vector3[][] = [0, 1].map((side) => {
    const sideArr = Array.isArray(rawPoints[side]) ? (rawPoints[side] as unknown[]) : []
    return [0, 1, 2].map((ptIdx) => asVec3(sideArr[ptIdx], defaults.points[side][ptIdx]))
  })
  return {
    settings: {
      enabled: asBool(s.enabled, defaults.enabled),
      useBootProxy: asBool(s.useBootProxy, defaults.useBootProxy),
      planeCalibrated: asBool(s.planeCalibrated, defaults.planeCalibrated),
      shoeCalibrated: asBool(s.shoeCalibrated, defaults.shoeCalibrated),
      planeNormal: asVec3(s.planeNormal, defaults.planeNormal),
      planeOffsetM: asNum(s.planeOffsetM, defaults.planeOffsetM),
      displayBandM: asNum(s.displayBandM, defaults.displayBandM),
      points,
    },
    snapshot: {
      available: asBool(snap.available, false),
      calibrated: asBool(snap.calibrated, false),
      clipTime: asNum(snap.clipTime, 0),
      feet: Array.isArray(snap.feet) ? (snap.feet as SoleState['snapshot']['feet']) : [],
    },
    generation: asNum(obj.generation ?? obj.calibrationGeneration, 0),
    notice: asStr(obj.notice),
  }
}

function normalizeSolePitchState(raw: unknown): SolePitchState {
  const obj = asObj(raw)
  const s = asObj(obj.settings)
  const legacySnap = asObj(obj.snapshot)
  const rawFeet = Array.isArray(obj.feet)
    ? obj.feet
    : Array.isArray(legacySnap.feet)
      ? legacySnap.feet
      : []
  return {
    settings: {
      enabled: asBool(s.enabled ?? s.experimentalSolePitchCorrection, DEFAULT_SOLE_PITCH_SETTINGS.enabled),
      maxPitchDegrees: asNum(s.maxPitchDegrees, DEFAULT_SOLE_PITCH_SETTINGS.maxPitchDegrees),
      useMeshEnvelope: asBool(s.useMeshEnvelope, DEFAULT_SOLE_PITCH_SETTINGS.useMeshEnvelope),
    },
    clipTime: asNum(obj.clipTime ?? legacySnap.clipTime, 0),
    feet: rawFeet.map((foot) => {
      const f = asObj(foot)
      const sol = asObj(f.solution)
      return {
        status: asStr(f.status),
        applied: asBool(f.applied, false),
        setterReadbackMatches: asBool(f.setterReadbackMatches, false),
        measuredAfterValid: asBool(f.measuredAfterValid, false),
        measuredAfterMinimumDistanceM: asNum(f.measuredAfterMinimumDistanceM, 0),
        solution: {
          valid: asBool(sol.valid, false),
          pitchDegrees: asNum(sol.pitchDegrees, 0),
          usedMeshEnvelope: asBool(sol.usedMeshEnvelope, false),
          beforePenetrationM: asNum(sol.beforePenetrationM, 0),
        },
      }
    }),
    notice: asStr(obj.notice),
  }
}

function normalizeStudioState(raw: unknown): StudioState {
  const defaults = defaultStudioSettings()
  const obj = asObj(raw)
  const s = asObj(obj.settings)
  const l = asObj(s.lighting)
  const v = asObj(s.video)
  const rgb = (value: unknown, fallback: [number, number, number]): [number, number, number] =>
    Array.isArray(value) && value.length === 3
      ? [asNum(value[0], fallback[0]), asNum(value[1], fallback[1]), asNum(value[2], fallback[2])]
      : fallback
  return {
    settings: {
      lighting: {
        enabled: asBool(l.enabled, defaults.lighting.enabled),
        actorStyle: asBool(l.actorStyle, defaults.lighting.actorStyle),
        decalEnabled: asBool(l.decalEnabled, defaults.lighting.decalEnabled),
        keyColor: rgb(l.keyColor, defaults.lighting.keyColor),
        fillColor: rgb(l.fillColor, defaults.lighting.fillColor),
        rimColor: rgb(l.rimColor, defaults.lighting.rimColor),
        decalTint: rgb(l.decalTint, defaults.lighting.decalTint),
        keyIntensity: asNum(l.keyIntensity, defaults.lighting.keyIntensity),
        fillIntensity: asNum(l.fillIntensity, defaults.lighting.fillIntensity),
        rimIntensity: asNum(l.rimIntensity, defaults.lighting.rimIntensity),
        keyYaw: asNum(l.keyYaw, defaults.lighting.keyYaw),
        keyPitch: asNum(l.keyPitch, defaults.lighting.keyPitch),
        fillYaw: asNum(l.fillYaw, defaults.lighting.fillYaw),
        fillPitch: asNum(l.fillPitch, defaults.lighting.fillPitch),
        rimYaw: asNum(l.rimYaw, defaults.lighting.rimYaw),
        rimPitch: asNum(l.rimPitch, defaults.lighting.rimPitch),
        actorIntensity: asNum(l.actorIntensity, defaults.lighting.actorIntensity),
        rimPower: asNum(l.rimPower, defaults.lighting.rimPower),
        decalIntensityScale: asNum(l.decalIntensityScale, defaults.lighting.decalIntensityScale),
      },
      video: {
        enabled: asBool(v.enabled, defaults.video.enabled),
        paused: asBool(v.paused, defaults.video.paused),
        loop: asBool(v.loop, defaults.video.loop),
        path: asStr(v.path, defaults.video.path),
        distance: asNum(v.distance, defaults.video.distance),
        width: asNum(v.width, defaults.video.width),
        offsetX: asNum(v.offsetX, defaults.video.offsetX),
        offsetY: asNum(v.offsetY, defaults.video.offsetY),
      },
    },
    revision: asNum(obj.revision, 0),
    lightingNotice: asStr(obj.lightingNotice),
    videoNotice: asStr(obj.videoNotice),
  }
}

function normalizeTiming(raw: unknown): PerformanceTiming {
  const defaults = defaultPerformanceTiming()
  const obj = asObj(raw)
  const track = (key: keyof PerformanceTiming): TrackTiming => {
    const t = asObj(obj[key])
    return {
      speed: asNum(t.speed, defaults[key].speed),
      offsetSeconds: asNum(t.offsetSeconds, defaults[key].offsetSeconds),
    }
  }
  return {
    motion: track('motion'),
    facial: track('facial'),
    camera: track('camera'),
    audio: track('audio'),
  }
}

function cloneDefaultFormation(): StageFormation {
  return {
    ...DEFAULT_STAGE_FORMATION,
    slots: DEFAULT_STAGE_FORMATION.slots.map((s) => ({
      ...s,
      offset: { ...s.offset },
      facialWeights: { ...s.facialWeights },
    })),
  }
}

function normalizeLibraryPositions(raw: unknown): LibraryPositionEntry[] {
  if (!Array.isArray(raw)) return []
  return raw.map((item, idx) => {
    const obj = asObj(item)
    return {
      slot: asNum(obj.slot, idx + 1),
      x: asNum(obj.x, 0),
      y: asNum(obj.y, 0),
      z: asNum(obj.z, 0),
      yawDegrees: asNum(obj.yawDegrees ?? obj.yaw_degrees, 0),
      hasMotion: asBool(obj.hasMotion, true),
      hasFacial: asBool(obj.hasFacial, false),
    }
  })
}

function normalizeLibraryEntry(raw: unknown): LibraryEntry {
  const obj = asObj(raw)
  const positions = normalizeLibraryPositions(obj.positions)
  const isGroup = asBool(obj.isGroup, positions.length >= 2)
  const positionCount = asNum(obj.positionCount, isGroup ? Math.max(2, positions.length) : Math.max(1, positions.length))
  return {
    id: asStr(obj.id),
    title: asStr(obj.title),
    subtitle: typeof obj.subtitle === 'string' && obj.subtitle !== '' ? obj.subtitle : undefined,
    directory: asStr(obj.directory),
    durationSeconds: asNum(obj.durationSeconds, 0),
    totalFrames: asNum(obj.totalFrames, 0),
    hasMotion: asBool(obj.hasMotion, true),
    hasFacial: asBool(obj.hasFacial, false),
    hasCamera: asBool(obj.hasCamera, false),
    hasAudio: asBool(obj.hasAudio, false),
    audioFile: typeof obj.audioFile === 'string' && obj.audioFile !== '' ? obj.audioFile : undefined,
    hasCover: obj.hasCover !== undefined ? asBool(obj.hasCover, false) : undefined,
    isGroup,
    positionCount,
    positions,
  }
}

function normalizeLibraryState(raw: unknown): LibraryState {
  const obj = asObj(raw)
  const entries = Array.isArray(obj.entries) ? obj.entries.map(normalizeLibraryEntry) : []
  return {
    revision: asNum(obj.revision, 0),
    entries,
    refreshing: asBool(obj.refreshing, false),
    notice: asStr(obj.notice),
  }
}

function normalizeActorsState(raw: unknown): ActorsState {
  const obj = asObj(raw)
  const rawActors = Array.isArray(obj.actors) ? obj.actors : []
  const actors: SceneActor[] = rawActors.map((item) => {
    const a = asObj(item)
    return {
      token: asNum(a.token, 0),
      label: asStr(a.label),
      character: asStr(a.character),
      displayName: asStr(a.displayName),
      shortName: asStr(a.shortName),
      selected: asBool(a.selected, false),
    }
  })
  return {
    epoch: asNum(obj.epoch, 0),
    actors,
    targetLabel: asStr(obj.targetLabel),
    boundLabel: asStr(obj.boundLabel),
  }
}

function normalizeStageSlot(raw: unknown, idx: number): StageSlotState {
  const obj = asObj(raw)
  const offObj = asObj(obj.offset)
  const fwObj = asObj(obj.facialWeights)
  return {
    slotIndex: asNum(obj.slotIndex, idx),
    occupied: asBool(obj.occupied, false),
    actorToken: asNum(obj.actorToken, 0),
    actorInstanceId: asNum(obj.actorInstanceId, 0),
    character: asStr(obj.character),
    shortName: asStr(obj.shortName),
    displayName: asStr(obj.displayName),
    isOverflow: asBool(obj.isOverflow, false),
    hasTrackMotion: asBool(obj.hasTrackMotion, false),
    sourceSlotIndex: asNum(obj.sourceSlotIndex, 0),
    placementDirty: asBool(obj.placementDirty, false),
    hasActorBoneOverrides: asBool(obj.hasActorBoneOverrides, false),
    physicsBackend: asStr(obj.physicsBackend),
    offset: {
      x: asNum(offObj.x, 0),
      y: asNum(offObj.y, 0),
      z: asNum(offObj.z, 0),
      yawDegrees: asNum(offObj.yawDegrees, 0),
    },
    facialWeights: {
      overall: asNum(fwObj.overall, 1),
      brow: asNum(fwObj.brow, 1),
      eye: asNum(fwObj.eye, 1),
      mouth: asNum(fwObj.mouth, 1),
    },
  }
}

function normalizeFormation(raw: unknown): StageFormation {
  if (!raw || typeof raw !== 'object') return cloneDefaultFormation()
  const obj = asObj(raw)
  const rawSlots = Array.isArray(obj.slots) ? obj.slots : []
  const slots = rawSlots.length > 0 ? rawSlots.map(normalizeStageSlot) : cloneDefaultFormation().slots
  const slotCount = Math.max(1, asNum(obj.slotCount, slots.length))
  return {
    active: asBool(obj.active, false),
    available: asBool(obj.available, false),
    verified: asBool(obj.verified, false),
    isGroupSong: asBool(obj.isGroupSong, false),
    trackPositionCount: Math.max(1, asNum(obj.trackPositionCount, 1)),
    slotCount,
    selectedSlotIndex: Math.min(Math.max(0, asNum(obj.selectedSlotIndex, 0)), slotCount - 1),
    revision: asNum(obj.revision, 1),
    actorEpoch: asNum(obj.actorEpoch, 1),
    notice: asStr(obj.notice),
    slots,
  }
}

function normalizeCameraState(raw: CameraState): CameraState {
  const sObj = asObj(raw.settings)
  const settings = syncLegacyTransforms({
    ...DEFAULT_CAMERA_SETTINGS,
    ...raw.settings,
    followSlotIndex: asNum(sObj.followSlotIndex, DEFAULT_CAMERA_SETTINGS.followSlotIndex),
  })
  return { ...raw, settings }
}

function normalizeEditEntryInfo(raw: unknown, fallbackKey: string): EditEntryInfo {
  const obj = asObj(raw)
  const entry = obj.entry && typeof obj.entry === 'object' ? asObj(obj.entry) : obj
  const positions = normalizeLibraryPositions(obj.positions)
  const isGroup = asBool(obj.isGroup, positions.length >= 2)
  const positionCount = asNum(obj.positionCount, isGroup ? Math.max(2, positions.length) : Math.max(1, positions.length))
  return {
    key: asStr(obj.key ?? entry.editKey, fallbackKey),
    title: asStr(obj.title ?? entry.title, fallbackKey),
    subtitle: asStr(obj.subtitle ?? entry.subtitle, ''),
    hasFacial: asBool(obj.hasFacial, false),
    hasCamera: asBool(obj.hasCamera, false),
    hasAudio: asBool(obj.hasAudio, false),
    audioFile: asStr(obj.audioFile, ''),
    hasCover: asBool(obj.hasCover, false),
    isGroup,
    positionCount,
    positions,
    timing: normalizeTiming(obj.timing ?? entry.timing),
  }
}

function normalizeClockState(raw: unknown): ClockState {
  if (!raw || typeof raw !== 'object') return { ...DEFAULT_CLOCK_STATE }
  const obj = asObj(raw)
  const runMode: ClockRunMode = obj.runMode === 'recording' ? 'recording' : 'realtime'
  const validSrc = (v: unknown, fb: ClockSource): ClockSource =>
    v === 'audio' || v === 'engine_delta' || v === 'fixed_step' || v === 'external' ? v : fb
  const configuredSource = validSrc(obj.configuredSource, DEFAULT_CLOCK_STATE.configuredSource)
  const effectiveSource = validSrc(obj.effectiveSource, runMode === 'recording' ? 'fixed_step' : configuredSource)
  return {
    runMode,
    configuredSource,
    effectiveSource,
    fpsNumerator: Math.max(1, asNum(obj.fpsNumerator, 60)),
    fpsDenominator: Math.max(1, asNum(obj.fpsDenominator, 1)),
    waitingForRenderComplete: asBool(obj.waitingForRenderComplete, false),
    leaseId: asNum(obj.leaseId, 0),
    leaseOwner: asStr(obj.leaseOwner, ''),
    evaluatedFrameSerial: asNum(obj.evaluatedFrameSerial, 0),
  }
}

function normalizeAudioEffectState(raw: unknown): AudioEffectState {
  if (!raw || typeof raw !== 'object') {
    return {
      ...DEFAULT_AUDIO_EFFECT_STATE,
      presets: DEFAULT_AUDIO_EFFECT_STATE.presets.map((p) => ({ ...p })),
    }
  }
  const obj = asObj(raw)
  const presetsRaw = Array.isArray(obj.presets) ? obj.presets : []
  const presets = presetsRaw
    .map((item) => {
      const p = asObj(item)
      return {
        id: asStr(p.id),
        name: asStr(p.name),
        category: asStr(p.category, 'eq'),
        description: asStr(p.description),
      }
    })
    .filter((p) => p.id !== '')
  return {
    available: asBool(obj.available, false),
    activeId: asStr(obj.activeId, 'flat') || 'flat',
    presets: presets.length > 0 ? presets : DEFAULT_AUDIO_EFFECT_STATE.presets.map((p) => ({ ...p })),
  }
}

function normalizeTelemetrySnapshot(raw: unknown): TelemetrySnapshot | null {
  if (!raw || typeof raw !== 'object') return null
  const obj = asObj(raw)
  const p = asObj(obj.player)
  const a = asObj(obj.actor)
  const stateNum = asNum(p.state, 0)
  const fallbackStateName =
    stateNum === 1 ? '播放中' : stateNum === 2 ? '已暂停' : stateNum === 3 ? '预览中' : '已停止'
  return {
    statusLine: asStr(obj.statusLine),
    player: {
      state: stateNum,
      stateName: asStr(p.stateName, fallbackStateName),
      time: asNum(p.time, 0),
      duration: asNum(p.duration, 0),
      totalFrames: asNum(p.totalFrames, 0),
      fps: asNum(p.fps, 60),
      boneCount: asNum(p.boneCount, 0),
      loadedMotionFile: asStr(p.loadedMotionFile),
    },
    actor: {
      actorLabel: asStr(a.actorLabel, '尚未绑定角色'),
      boundBoneCount: asNum(a.boundBoneCount, 0),
      totalBoneCount: asNum(a.totalBoneCount, 0),
    },
  }
}

function normalizeDropPeek(raw: unknown): DropUiState {
  const obj = asObj(raw)
  const visualRaw = obj.visual
  const visual =
    visualRaw === 'armed' || visualRaw === 'hover' || visualRaw === 'invalid' ? visualRaw : 'none'
  return {
    visual,
    fileCount: asNum(obj.fileCount ?? obj.pendingCount, 0),
    accepted: asNum(obj.accepted, 0),
    hostElevated: asBool(obj.hostElevated, false),
    hostBlocked: asBool(obj.hostBlocked, false),
  }
}

export interface MotionAppState {
  ready: boolean
  connected: boolean
  bridgeKind: 'webview2' | 'mock'
  pluginVersion: string
  capabilities: string[]
  permission: PermissionState
  player: PlayerState
  library: LibraryState
  loaded: LoadedState
  playlist: { mode: number; notice: string }
  actors: ActorsState
  formation: StageFormation
  camera: CameraState
  stage: StageState
  facial: { weights: FacialWeights; notice: string }
  position: { offset: Vector3; notice: string }
  physicsBackend: number
  musicVolumeSupport: number
  musicVolumeNotice: string
  importProgress: ImportProgress | null
  motionsRoot: string
  appearance: MotionAppearance | null
  // ---- Phase 2 domain snapshots ----
  bone: BoneState
  gaze: GazeState
  stageNodes: StageNode[]
  experiments: ExperimentsState
  telemetry: TelemetrySnapshot | null
  sole: SoleState
  solePitch: SolePitchState
  studio: StudioState
  clock: ClockState
  audioEffect: AudioEffectState
  coverCache: Record<string, string>
  portraitCache: Record<string, string | null>
  // ---- UI-local state (never persisted by the bridge) ----
  page: MotionPage
  developerMode: boolean
  selectedLibraryKey: string | null
  librarySearch: string
  libraryFilter: LibraryFilter
  librarySort: LibrarySort
  seekStepSeconds: number
  precisionPanelOpen: boolean
  precisionPanelPinned: boolean
  pendingDelete: PendingDelete | null
  deleteStale: boolean
  importDialogOpen: boolean
  navigationError: string | null
  actionError: string | null
  scrub: { active: boolean; value: number }
  cameraDraft: CameraSettings
  cameraExpected: CameraSettings | null
  cameraPending: boolean
  cameraLastSnapshot: CameraSettings
  error: string | null
  // ---- Phase 2 UI-local state ----
  importForm: ImportFormState
  importUi: ImportUiState
  coverEditor: CoverEditorState
  dialogBusy: boolean
  drop: DropUiState
  dropNotice: string
  selectedBoneHid: number
  boneSymmetrical: boolean
  soleDraft: SoleSettings
  soleDraftGeneration: number
  solePlotSpanM: number
  studioDraft: StudioSettings
  studioDraftRevision: number
  studioVideoPath: string
  stageLoadNotice: string
  probeScrubBegin: number
  probeScrubUpdate: number
  probeScrubEnd: number
  probeLastScrubValue: number
}

function initialState(): MotionAppState {
  return {
    ready: false,
    connected: false,
    bridgeKind: 'mock',
    pluginVersion: '',
    capabilities: [],
    permission: { injectionAllowed: false, notice: '' },
    player: { ...DEFAULT_PLAYER_STATE },
    library: { revision: 0, entries: [], refreshing: false, notice: '' },
    loaded: { motionKey: '', generation: 0, injectionSession: 0, sourceMissing: false, sourceChanged: false },
    playlist: { mode: 1, notice: '' },
    actors: { epoch: 0, actors: [], targetLabel: '', boundLabel: '' },
    formation: cloneDefaultFormation(),
    camera: {
      settings: { ...DEFAULT_CAMERA_SETTINGS },
      available: false,
      owned: false,
      hasTrack: false,
      error: '',
      followTargetError: '',
      notice: '',
    },
    stage: { settings: { ...DEFAULT_STAGE_SETTINGS }, sceneName: '', statusNotice: '' },
    facial: { weights: { ...DEFAULT_FACIAL_WEIGHTS }, notice: '' },
    position: { offset: { x: 0, y: 0, z: 0 }, notice: '' },
    physicsBackend: 2,
    musicVolumeSupport: 0,
    musicVolumeNotice: '',
    importProgress: null,
    motionsRoot: 'gakumas-local/motions',
    appearance: null,
    bone: { masterEnabled: true, overrides: [], notice: '' },
    gaze: {
      settings: { ...DEFAULT_GAZE_SETTINGS },
      runtimeAvailable: false,
      vrifyQueryAvailable: false,
      vrifyLiveGazeActive: false,
      notice: '',
    },
    stageNodes: [],
    experiments: {
      settings: { ...DEFAULT_EXPERIMENT_SETTINGS },
      mouthFitStatus: '尚未接管',
      eyeFitStatus: '尚未接管',
      fittedFaceLabel: '',
      notice: '',
    },
    telemetry: null,
    sole: { settings: defaultSoleSettings(), snapshot: { available: false, calibrated: false, clipTime: 0, feet: [] }, generation: 0, notice: '' },
    solePitch: { settings: { ...DEFAULT_SOLE_PITCH_SETTINGS }, clipTime: 0, feet: [], notice: '' },
    studio: { settings: defaultStudioSettings(), revision: 0, lightingNotice: '', videoNotice: '' },
    clock: { ...DEFAULT_CLOCK_STATE },
    audioEffect: {
      ...DEFAULT_AUDIO_EFFECT_STATE,
      presets: DEFAULT_AUDIO_EFFECT_STATE.presets.map((p) => ({ ...p })),
    },
    coverCache: {},
    portraitCache: {},
    page: 'library',
    developerMode: false,
    selectedLibraryKey: null,
    librarySearch: '',
    libraryFilter: 'all',
    librarySort: 'default',
    seekStepSeconds: 5,
    precisionPanelOpen: false,
    precisionPanelPinned: false,
    pendingDelete: null,
    deleteStale: false,
    importDialogOpen: false,
    navigationError: null,
    actionError: null,
    scrub: { active: false, value: 0 },
    cameraDraft: { ...DEFAULT_CAMERA_SETTINGS },
    cameraExpected: null,
    cameraPending: false,
    cameraLastSnapshot: { ...DEFAULT_CAMERA_SETTINGS },
    error: null,
    importForm: emptyImportForm(),
    importUi: emptyImportUi(),
    coverEditor: emptyCoverEditor(),
    dialogBusy: false,
    drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false },
    dropNotice: '',
    selectedBoneHid: 0,
    boneSymmetrical: false,
    soleDraft: defaultSoleSettings(),
    soleDraftGeneration: -1,
    solePlotSpanM: 1,
    studioDraft: defaultStudioSettings(),
    studioDraftRevision: -1,
    studioVideoPath: '',
    stageLoadNotice: '',
    probeScrubBegin: 0,
    probeScrubUpdate: 0,
    probeScrubEnd: 0,
    probeLastScrubValue: 0,
  }
}

type Listener = () => void

export class MotionStore {
  private state: MotionAppState = initialState()
  private readonly listeners = new Set<Listener>()
  private bridge: MotionBridgeClient | null
  private started = false
  private unsubs: Array<() => void> = []
  private volumeTimer: ReturnType<typeof setTimeout> | null = null
  private pendingVolume: number | null = null
  private facialTimer: ReturnType<typeof setTimeout> | null = null
  private pendingFacial: FacialWeights | null = null
  private dialogTimer: ReturnType<typeof setInterval> | null = null
  private dialogId = 0
  private dialogTarget: DialogTarget | null = null
  private dropTimer: ReturnType<typeof setInterval> | null = null
  private dropWatchCount = 0
  private dropTargetActive = false
  private importUiTimer: ReturnType<typeof setInterval> | null = null
  private soleTimer: ReturnType<typeof setInterval> | null = null
  private solePitchTimer: ReturnType<typeof setInterval> | null = null
  private coverPending = new Set<string>()
  private portraitInflight = new Map<string, Promise<string | null>>()
  private coverGeneration = 0
  private coverQueue: string[] = []
  private coverWorkersRunning = 0
  private refreshCoverBustPending = false
  private pendingImportOutcome: {
    editKey: string
    existingIds: Set<string>
    coverMode: ImportCoverMode
    coverDataUri: string
    succeededKey: string | null
  } | null = null

  constructor(bridge?: MotionBridgeClient) {
    this.bridge = bridge ?? null
  }

  getState = (): MotionAppState => this.state

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }

  private set(partial: Partial<MotionAppState>): void {
    this.state = { ...this.state, ...partial }
    for (const listener of this.listeners) listener()
  }

  supports = (method: MotionMethodName): boolean => this.state.capabilities.includes(method)

  private client(): MotionBridgeClient {
    if (!this.bridge) throw new BridgeError('closed', 'The bridge is unavailable')
    return this.bridge
  }

  private reportActionError(error: unknown): void {
    const message = error instanceof Error ? error.message : String(error)
    this.set({ actionError: message })
  }

  async start(): Promise<void> {
    if (this.started) return
    this.started = true
    this.bridge = this.bridge ?? getMotionBridge()
    const bridge = this.bridge
    if (!bridge) {
      this.set({ ready: true, connected: false, error: '未在 WebView2 中运行，且当前构建不提供开发 Mock。' })
      return
    }
    this.unsubs.push(
      bridge.on('player.stateChanged', (player) => this.onPlayerStateChanged(player)),
      bridge.on('permission.changed', (permission) => {
        // Permission loss drops an in-flight scrub without committing it.
        const patch: Partial<MotionAppState> = { permission }
        if (!permission.injectionAllowed) {
          patch.scrub = { active: false, value: 0 }
          patch.navigationError = null
          patch.precisionPanelOpen = false
        }
        this.set(patch)
      }),
      bridge.on('library.changed', (rawLibrary) => {
        const library = normalizeLibraryState(rawLibrary)
        const previousEntries = this.state.library.entries
        const selectedKey = this.state.selectedLibraryKey
        const stillPresent = selectedKey !== null && library.entries.some((entry) => entry.id === selectedKey)
        const revisionChanged = library.revision !== this.state.library.revision
        let nextCoverCache = this.state.coverCache
        if (revisionChanged) {
          this.coverGeneration += 1
          this.coverPending.clear()
          this.coverQueue = []
          if (this.refreshCoverBustPending) {
            nextCoverCache = {}
            this.refreshCoverBustPending = false
          } else {
            const prevById = new Map(previousEntries.map((item) => [item.id, item]))
            const nextById = new Map(library.entries.map((item) => [item.id, item]))
            const filtered: Record<string, string> = {}
            for (const [key, uri] of Object.entries(this.state.coverCache)) {
              const nextEntry = nextById.get(key)
              if (!nextEntry) continue
              const prevEntry = prevById.get(key)
              const metaChanged =
                !prevEntry ||
                Boolean(prevEntry.hasCover) !== Boolean(nextEntry.hasCover) ||
                (prevEntry.audioFile ?? '') !== (nextEntry.audioFile ?? '') ||
                this.pendingImportOutcome?.editKey === key
              if (!metaChanged) filtered[key] = uri
            }
            nextCoverCache = filtered
          }
        }
        this.set({
          library,
          selectedLibraryKey: stillPresent ? selectedKey : null,
          ...(revisionChanged ? { coverCache: nextCoverCache } : {}),
        })
        this.maybeFinalizeImportSelection(library)
        this.preloadLibraryCovers(library.entries)
      }),
      bridge.on('loaded.changed', (loaded) => {
        const previous = this.state.loaded
        const identityChanged =
          loaded.motionKey !== previous.motionKey ||
          loaded.generation !== previous.generation ||
          loaded.injectionSession !== previous.injectionSession
        this.set({
          loaded,
          scrub: identityChanged ? { active: false, value: 0 } : this.state.scrub,
          navigationError: identityChanged ? null : this.state.navigationError,
        })
      }),
      bridge.on('playlist.changed', (playlist) => this.set({ playlist })),
      bridge.on('actors.changed', (rawActors) => {
        this.set({ actors: normalizeActorsState(rawActors) })
      }),
      bridge.on('formation.changed', (rawFormation) => {
        const formation = normalizeFormation(rawFormation)
        const sel = formation.slots[formation.selectedSlotIndex]
        const patch: Partial<MotionAppState> = { formation }
        if (sel && sel.occupied) {
          patch.facial = { ...this.state.facial, weights: { ...sel.facialWeights } }
          patch.position = {
            ...this.state.position,
            offset: { x: sel.offset.x, y: sel.offset.y, z: sel.offset.z },
          }
        }
        this.set(patch)
      }),
      bridge.on('camera.changed', (camera) => this.onCameraChanged(camera)),
      bridge.on('stage.changed', (stage) => this.set({ stage })),
      bridge.on('facial.changed', (facial) => {
        const idx = this.state.formation.selectedSlotIndex
        const slots = this.state.formation.slots.map((s, i) =>
          i === idx ? { ...s, facialWeights: { ...facial.weights } } : s,
        )
        this.set({ facial, formation: { ...this.state.formation, slots } })
      }),
      bridge.on('position.changed', (position) => this.set({ position })),
      bridge.on('settings.changed', (settings) => this.set({ physicsBackend: settings.physicsBackend })),
      bridge.on('import.progress', (importProgress) => this.onImportProgress(importProgress)),
      bridge.on('appearance.changed', (appearance) => this.set({ appearance })),
      bridge.on('bone.changed', (bone) => this.set({ bone: normalizeBoneState(bone) })),
      bridge.on('gaze.changed', (gaze) => this.set({ gaze: normalizeGazeState(gaze) })),
      bridge.on('stage.nodesChanged', (payload) =>
        this.set({ stageNodes: normalizeStageNodes(asObj(payload).nodes ?? payload) }),
      ),
      bridge.on('experiments.changed', (experiments) =>
        this.set({ experiments: normalizeExperimentsState(experiments) }),
      ),
      bridge.on('sole.changed', (sole) => this.onSoleChanged(normalizeSoleState(sole))),
      bridge.on('solePitch.changed', (solePitch) =>
        this.set({ solePitch: normalizeSolePitchState(solePitch) }),
      ),
      bridge.on('studio.changed', (studio) => this.onStudioChanged(normalizeStudioState(studio))),
      bridge.on('clock.changed', (clock) => this.set({ clock: normalizeClockState(clock) })),
      bridge.on('audioEffect.changed', (audioEffect) =>
        this.set({ audioEffect: normalizeAudioEffectState(audioEffect) }),
      ),
      bridge.on('bridge.closed', () => {
        this.stopWatchers()
        this.set({ connected: false, error: '原生桥已关闭', scrub: { active: false, value: 0 } })
      }),
    )
    await this.performHandshake()
  }

  private async performHandshake(): Promise<void> {
    const bridge = this.client()
    try {
      await bridge.request('bridge.hello')
      const hello = await bridge.request('motion.hello')
      const snapshot = await bridge.request('state.get')
      this.applySnapshot(snapshot)
      this.set({
        ready: true,
        connected: true,
        bridgeKind: bridge.kind,
        pluginVersion: hello.pluginVersion,
        capabilities: hello.supportedMethods,
        error: null,
        actionError: null,
      })
      if (hello.supportedMethods.includes('appearance.get')) {
        try {
          this.set({ appearance: await bridge.request('appearance.get') })
        } catch {
          // Appearance is optional; keep the default theme.
        }
      }
      this.preloadLibraryCovers(this.state.library.entries)
      await this.loadPhase2Domains()
    } catch (error) {
      this.set({ ready: true, connected: false, error: error instanceof Error ? error.message : String(error) })
    }
  }

  resync = async (): Promise<void> => {
    if (!this.bridge) return
    await this.performHandshake()
  }

  restart = async (): Promise<void> => {
    for (const unsub of this.unsubs) unsub()
    this.unsubs = []
    this.stopWatchers()
    this.started = false
    if (!this.state.connected && this.bridge) {
      this.bridge.close()
      this.bridge = null
      setMotionBridge(null)
    }
    await this.start()
  }

  private async loadPhase2Domains(): Promise<void> {
    const bridge = this.client()
    const loads: Array<Promise<void>> = []
    const fetch = async <M extends MotionMethodName>(method: M, apply: (result: MotionMethods[M]['result']) => void) => {
      if (!this.supports(method)) return
      try {
        apply(await bridge.request(method))
      } catch (error) {
        this.reportActionError(error)
      }
    }
    loads.push(fetch('bone.get', (bone) => this.set({ bone: normalizeBoneState(bone) })))
    loads.push(fetch('gaze.get', (gaze) => this.set({ gaze: normalizeGazeState(gaze) })))
    loads.push(fetch('experiments.get', (experiments) => this.set({ experiments: normalizeExperimentsState(experiments) })))
    loads.push(fetch('sole.get', (sole) => this.onSoleChanged(normalizeSoleState(sole))))
    loads.push(fetch('solePitch.get', (solePitch) => this.set({ solePitch: normalizeSolePitchState(solePitch) })))
    loads.push(fetch('studio.get', (studio) => this.onStudioChanged(normalizeStudioState(studio))))
    await Promise.all(loads)
  }

  private applySnapshot(snapshot: MotionStateSnapshot): void {
    const raw = asObj(snapshot)
    const library = normalizeLibraryState(snapshot.library)
    const actors = normalizeActorsState(snapshot.actors)
    const formation = normalizeFormation(raw.formation)
    const normalizedCamera = normalizeCameraState(snapshot.camera)
    const cameraSettings = normalizedCamera.settings
    const stageRaw = asObj(raw.stage)
    const stageNodesRaw = raw.stageNodes ?? stageRaw.candidates
    const patch: Partial<MotionAppState> = {
      permission: snapshot.permission,
      player: snapshot.player,
      library,
      loaded: snapshot.loaded,
      playlist: snapshot.playlist,
      actors,
      formation,
      camera: normalizedCamera,
      stage: snapshot.stage,
      facial: snapshot.facial,
      position: snapshot.position,
      physicsBackend: snapshot.physicsBackend,
      musicVolumeSupport: snapshot.musicVolumeSupport,
      musicVolumeNotice: snapshot.musicVolumeNotice,
      importProgress: snapshot.importProgress,
      motionsRoot: snapshot.motionsRoot,
      clock: normalizeClockState(raw.clock),
      audioEffect: normalizeAudioEffectState(raw.audioEffect),
      cameraDraft: cameraSettings,
      cameraLastSnapshot: cameraSettings,
      cameraExpected: null,
      cameraPending: false,
      scrub: { active: false, value: 0 },
    }
    if (Array.isArray(stageNodesRaw)) patch.stageNodes = normalizeStageNodes(stageNodesRaw)
    if (raw.bone) patch.bone = normalizeBoneState(raw.bone)
    if (raw.gaze) patch.gaze = normalizeGazeState(raw.gaze)
    if (raw.experiments) patch.experiments = normalizeExperimentsState(raw.experiments)
    if (raw.sole) this.onSoleChanged(normalizeSoleState(raw.sole))
    if (raw.solePitch) patch.solePitch = normalizeSolePitchState(raw.solePitch)
    if (raw.studio) this.onStudioChanged(normalizeStudioState(raw.studio))
    this.set(patch)
  }

  private onCameraChanged(rawCamera: CameraState): void {
    const camera = normalizeCameraState(rawCamera)
    const settings = camera.settings
    const patch: Partial<MotionAppState> = { camera }
    if (this.state.cameraPending && this.state.cameraExpected && deepEqual(settings, this.state.cameraExpected)) {
      patch.cameraPending = false
      patch.cameraExpected = null
      patch.cameraDraft = settings
      patch.cameraLastSnapshot = settings
    } else if (!this.state.cameraPending && !deepEqual(settings, this.state.cameraLastSnapshot)) {
      patch.cameraDraft = settings
      patch.cameraLastSnapshot = settings
    }
    this.set(patch)
  }

  // ---- UI-local actions ----

  setPage = (page: MotionPage): void => {
    if (page === 'developer' && !this.state.developerMode) return
    this.set({ page })
  }

  setDeveloperMode = (enabled: boolean): void => {
    this.set({
      developerMode: enabled,
      page: !enabled && this.state.page === 'developer' ? 'library' : this.state.page,
    })
  }

  selectLibrary = (key: string | null): void => this.set({ selectedLibraryKey: key })

  setLibrarySearch = (search: string): void => this.set({ librarySearch: search })

  setLibraryFilter = (libraryFilter: LibraryFilter): void => this.set({ libraryFilter })

  setLibrarySort = (librarySort: LibrarySort): void => this.set({ librarySort })

  setSeekStepSeconds = (seconds: number): void =>
    this.set({ seekStepSeconds: [1, 5, 10, 15].includes(seconds) ? seconds : 5 })

  setPrecisionPanelOpen = (precisionPanelOpen: boolean): void => this.set({ precisionPanelOpen })

  setPrecisionPanelPinned = (precisionPanelPinned: boolean): void => this.set({ precisionPanelPinned })

  setImportDialogOpen = (open: boolean): void => this.set({ importDialogOpen: open })

  clearActionError = (): void => this.set({ actionError: null })

  // ---- Transport ----

  play = async (): Promise<void> => {
    try {
      if (this.state.player.state === 'paused' || this.state.player.state === 'preview') {
        await this.client().request('player.resume')
      } else {
        await this.client().request('player.play')
      }
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  pause = async (): Promise<void> => {
    try {
      await this.client().request('player.pause')
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  stop = async (): Promise<void> => {
    try {
      await this.client().request('player.stop')
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  cycle = async (delta: number): Promise<void> => {
    try {
      await this.client().request('player.cycle', { delta })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  stepFrame = async (delta: number): Promise<void> => {
    try {
      await this.client().request('player.stepFrame', { delta })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  scrubStart = (seconds: number): void => {
    this.set({
      scrub: { active: true, value: seconds },
      probeScrubBegin: this.state.probeScrubBegin + 1,
      probeLastScrubValue: seconds,
    })
    void this.client().request('player.scrubBegin', { seconds }).catch((error) => this.reportActionError(error))
  }

  scrubMove = (seconds: number): void => {
    if (!this.state.scrub.active) return
    this.set({
      scrub: { active: true, value: seconds },
      probeScrubUpdate: this.state.probeScrubUpdate + 1,
      probeLastScrubValue: seconds,
    })
    void this.client().request('player.scrubUpdate', { seconds }).catch((error) => this.reportActionError(error))
  }

  scrubCommit = (seconds: number): void => {
    if (!this.state.scrub.active) return
    this.set({
      scrub: { active: false, value: seconds },
      probeScrubEnd: this.state.probeScrubEnd + 1,
      probeLastScrubValue: seconds,
    })
    void this.client().request('player.scrubEnd', { seconds }).catch((error) => this.reportActionError(error))
  }

  seekTime = async (input: string): Promise<void> => {
    const seconds = parseTransportTime(input)
    if (seconds === null) {
      this.set({ navigationError: '时间格式无效：请输入非负秒数或 mm:ss.mmm。' })
      return
    }
    if (seconds > this.state.player.duration) {
      this.set({ navigationError: '时间超出演出时长；可用动作帧输入检查末采样。' })
      return
    }
    try {
      await this.client().request('player.seek', { seconds })
      this.set({ navigationError: null, actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  seekRelative = async (deltaSeconds: number): Promise<void> => {
    const { totalFrames, duration, time } = this.state.player
    if (totalFrames <= 0 || duration <= 0) return
    const target = Math.min(Math.max(time + deltaSeconds, 0), duration)
    try {
      await this.client().request('player.seek', { seconds: target })
      this.set({ navigationError: null, actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  seekFrame = async (input: string): Promise<void> => {
    const frame = parseTransportFrame(input)
    if (frame === null) {
      this.set({ navigationError: '帧号格式无效：请输入从 0 开始的非负整数。' })
      return
    }
    const { totalFrames } = this.state.player
    if (totalFrames <= 0 || frame >= totalFrames) {
      this.set({ navigationError: '帧号超出范围；末帧为总采样帧数减 1。' })
      return
    }
    try {
      await this.client().request('player.seekFrame', { frame })
      this.set({ navigationError: null, actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setSpeed = async (speed: number): Promise<void> => {
    try {
      await this.client().request('player.setSpeed', { speed })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setPlayMode = async (mode: number): Promise<void> => {
    try {
      await this.client().request('playlist.setMode', { mode })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  cyclePlayMode = async (): Promise<void> => {
    const maxModes = 4
    const current = this.state.player.playMode
    const normalized = current >= 0 && current < maxModes ? current : 1
    const next = (normalized + 1) % maxModes
    await this.setPlayMode(next)
  }

  // Dragging sends throttled updates; releasing commits and saves (ImGui saves
  // on IsItemDeactivatedAfterEdit, never mid-drag).
  setVolume = (percent: number): void => {
    const volume = Math.min(Math.max(percent, 0), 100) / 100
    this.pendingVolume = volume
    if (this.volumeTimer) return
    this.volumeTimer = setTimeout(() => {
      this.volumeTimer = null
      this.flushVolume()
    }, 80)
  }

  private flushVolume(): void {
    if (this.pendingVolume === null) return
    const volume = this.pendingVolume
    this.pendingVolume = null
    void this.client().request('player.setVolume', { volume }).catch((error) => this.reportActionError(error))
  }

  commitVolume = (percent: number): void => {
    if (this.volumeTimer) {
      clearTimeout(this.volumeTimer)
      this.volumeTimer = null
    }
    this.pendingVolume = Math.min(Math.max(percent, 0), 100) / 100
    this.flushVolume()
    void this.client().request('player.saveVolume').catch((error) => this.reportActionError(error))
  }

  setCameraQuickMode = async (selection: number): Promise<void> => {
    const base = this.state.cameraPending ? this.state.cameraDraft : this.state.camera.settings
    const next = setCameraControlMode(base, selection)
    this.set({ cameraDraft: next, cameraExpected: next, cameraPending: true })
    try {
      await this.client().request('camera.set', { settings: next })
    } catch (error) {
      this.set({ cameraPending: false, cameraExpected: null, cameraDraft: this.state.cameraLastSnapshot })
      this.reportActionError(error)
    }
  }

  reloadMotion = async (): Promise<void> => {
    try {
      await this.client().request('player.reload')
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Library ----

  refreshLibrary = async (): Promise<void> => {
    this.refreshCoverBustPending = true
    try {
      await this.client().request('library.refresh')
      this.set({ actionError: null })
    } catch (error) {
      this.refreshCoverBustPending = false
      this.reportActionError(error)
    }
  }

  openLibraryFolder = async (): Promise<void> => {
    try {
      await this.client().request('library.openFolder')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  selectMotion = async (key: string, autoplay: boolean): Promise<void> => {
    try {
      await this.client().request('library.select', { key, autoplay, revision: this.state.library.revision })
      this.set({ selectedLibraryKey: key, actionError: null })
    } catch (error) {
      if (error instanceof BridgeError && error.code === 'stale_revision') {
        this.set({ actionError: '曲库已变化，请刷新后重试。' })
      } else {
        this.reportActionError(error)
      }
    }
  }

  requestDelete = (key: string): void => {
    const entry = this.state.library.entries.find((candidate) => candidate.id === key)
    if (!entry || this.state.library.refreshing) return
    this.set({
      pendingDelete: { key, directory: entry.directory, revision: this.state.library.revision },
      deleteStale: false,
    })
  }

  cancelDelete = (): void => this.set({ pendingDelete: null, deleteStale: false })

  deleteGuardChanged = (): boolean => {
    const pending = this.state.pendingDelete
    if (!pending) return false
    if (this.state.deleteStale) return true
    if (pending.revision !== this.state.library.revision) return true
    const entry = this.state.library.entries.find((candidate) => candidate.id === pending.key)
    return !entry || entry.directory !== pending.directory
  }

  confirmDelete = async (): Promise<void> => {
    const pending = this.state.pendingDelete
    if (!pending || this.deleteGuardChanged()) return
    try {
      await this.client().request('library.delete', { key: pending.key, revision: pending.revision })
      this.set({ pendingDelete: null, deleteStale: false, actionError: null })
    } catch (error) {
      if (error instanceof BridgeError && error.code === 'stale_revision') {
        this.set({ deleteStale: true })
      } else {
        this.reportActionError(error)
      }
    }
  }

  // ---- Motion page ----

  selectActor = async (token: number): Promise<void> => {
    try {
      await this.client().request('actors.selectTarget', { token, epoch: this.state.actors.epoch })
      this.set({ actionError: null })
    } catch (error) {
      if (error instanceof BridgeError && error.code === 'stale_token') {
        this.set({ actionError: '角色名单已变化，请重新选择。' })
      } else {
        this.reportActionError(error)
      }
    }
  }

  setFacialWeights = (weights: FacialWeights): void => {
    this.pendingFacial = weights
    if (this.facialTimer) return
    this.facialTimer = setTimeout(() => {
      this.facialTimer = null
      this.flushFacial()
    }, 80)
  }

  private flushFacial(): void {
    if (!this.pendingFacial) return
    const weights = this.pendingFacial
    this.pendingFacial = null
    void this.client().request('facial.setWeights', weights).catch((error) => this.reportActionError(error))
  }

  commitFacialWeights = (weights: FacialWeights): void => {
    if (this.facialTimer) {
      clearTimeout(this.facialTimer)
      this.facialTimer = null
    }
    this.pendingFacial = weights
    this.flushFacial()
  }

  resetFacialWeights = (): void => {
    this.commitFacialWeights({ ...DEFAULT_FACIAL_WEIGHTS })
  }

  saveFacialWeights = async (): Promise<void> => {
    try {
      await this.client().request('facial.save')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setPosition = async (offset: Vector3): Promise<void> => {
    try {
      await this.client().request('position.set', offset)
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  resetPosition = (): void => {
    void this.setPosition({ x: 0, y: 0, z: 0 })
  }

  savePosition = async (): Promise<void> => {
    try {
      await this.client().request('position.save')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Portraits & Formation (F5 / F7-F10 / F13) ----

  ensureActorPortrait = (character: string): Promise<string | null> => {
    const norm = character.trim().toLowerCase()
    if (!norm) return Promise.resolve(null)
    if (Object.prototype.hasOwnProperty.call(this.state.portraitCache, norm)) {
      return Promise.resolve(this.state.portraitCache[norm] ?? null)
    }
    const existing = this.portraitInflight.get(norm)
    if (existing) return existing
    if (!this.supports('actors.getPortrait')) {
      return Promise.resolve(null)
    }
    const promise = (async (): Promise<string | null> => {
      try {
        const res = await this.client().request('actors.getPortrait', { character: norm })
        const uri = res.cached && typeof res.dataUri === 'string' && res.dataUri !== '' ? res.dataUri : null
        this.set({ portraitCache: { ...this.state.portraitCache, [norm]: uri } })
        return uri
      } catch {
        return null
      } finally {
        this.portraitInflight.delete(norm)
      }
    })()
    this.portraitInflight.set(norm, promise)
    return promise
  }

  selectFormationSlot = async (slotIndex: number): Promise<void> => {
    try {
      await this.client().request('formation.selectSlot', { slotIndex })
      this.set({
        formation: { ...this.state.formation, selectedSlotIndex: slotIndex },
        actionError: null,
      })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  swapFormationSlots = async (fromSlotIndex: number, toSlotIndex: number): Promise<void> => {
    try {
      await this.client().request('formation.swapSlots', { fromSlotIndex, toSlotIndex })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setFormationSlotOffset = async (slotIndex: number, offset: StageSlotOffset): Promise<void> => {
    try {
      await this.client().request('formation.setSlotOffset', {
        slotIndex,
        x: offset.x,
        y: offset.y,
        z: offset.z,
        yawDegrees: offset.yawDegrees,
      })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  commitFormationSlotOffset = this.setFormationSlotOffset

  resetFormationSlotOffset = async (slotIndex: number): Promise<void> => {
    try {
      await this.client().request('formation.resetSlotOffset', { slotIndex })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  saveFormationSlotOffset = async (slotIndex: number): Promise<void> => {
    try {
      await this.client().request('formation.saveSlotOffset', { slotIndex })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setFormationSlotSource = async (slotIndex: number, sourceSlotIndex: number): Promise<void> => {
    try {
      await this.client().request('formation.setSlotSource', { slotIndex, sourceSlotIndex })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Camera page ----

  patchCameraDraft = (patch: Partial<CameraSettings>): void => {
    const next = { ...this.state.cameraDraft, ...patch }
    this.set({ cameraDraft: next, cameraExpected: next, cameraPending: true })
    void this.client()
      .request('camera.set', { settings: next })
      .catch((error) => {
        this.set({ cameraPending: false, cameraExpected: null, cameraDraft: this.state.cameraLastSnapshot })
        this.reportActionError(error)
      })
  }

  saveCamera = async (): Promise<void> => {
    try {
      await this.client().request('camera.save')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  cameraQuickModeIndex = (): number => {
    const settings = this.state.cameraPending ? this.state.cameraDraft : this.state.camera.settings
    return cameraControlModeIndex(settings)
  }

  // ---- Stage page ----

  patchStage = (patch: Partial<MotionStateSnapshot['stage']['settings']>): void => {
    const next = { ...this.state.stage.settings, ...patch }
    this.set({ stage: { ...this.state.stage, settings: next } })
    void this.client()
      .request('stage.set', { settings: next })
      .catch((error) => this.reportActionError(error))
  }

  // ---- Settings page ----

  setPhysicsBackend = async (backend: number): Promise<void> => {
    try {
      await this.client().request('settings.setPhysics', { backend })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Import form (phase 2) ----

  private probePaths = async (paths: string[]): Promise<VmdProbe[]> => {
    const targets = paths.filter((path) => /\.(vmd|gkcamera)$/i.test(path) && !this.state.importForm.probes[path])
    if (targets.length === 0 || !this.supports('library.probeFiles')) return []
    try {
      const { probes } = await this.client().request('library.probeFiles', { paths: targets })
      const merged = { ...this.state.importForm.probes }
      for (const probe of probes) merged[probe.path] = probe
      this.set({ importForm: { ...this.state.importForm, probes: merged } })
      return probes
    } catch (error) {
      this.reportActionError(error)
      return []
    }
  }

  openImportNew = async (files: string[]): Promise<void> => {
    const form = emptyImportForm()
    form.open = true
    form.mode = 'new'
    form.positions = [
      {
        motion: { candidates: [IMPORT_SLOT.empty], selection: 0 },
        facial: { candidates: [IMPORT_SLOT.autoFacial], selection: 0 },
        x: 0,
        y: 0,
        z: 0,
        yawDegrees: 0,
        preserveExisting: false,
      },
    ]
    form.slots.motion = form.positions[0].motion
    form.slots.facial = form.positions[0].facial
    form.slots.camera.candidates = [IMPORT_SLOT.empty]
    form.slots.audio.candidates = [IMPORT_SLOT.autoAudio]
    this.set({ importForm: form })
    await this.probePaths(files)
    const probeMap = this.state.importForm.probes
    const probes = Object.values(probeMap)
    const classification = classifyImportFiles(files, probes)

    const appendUnique = (list: string[], item: string) => {
      if (!list.includes(item)) list.push(item)
    }
    const findIndex = (list: string[], path: string): number => {
      if (path === '') return 0
      const index = list.indexOf(path)
      return index > 0 ? index : 0
    }

    const vmdFiles: string[] = []
    const cameraFiles: string[] = []
    const facialOnlyFiles: string[] = []
    const audioFiles: string[] = []
    for (const file of files) {
      const dot = file.lastIndexOf('.')
      const ext = dot >= 0 ? file.slice(dot).toLowerCase() : ''
      if (ext === '.vmd') {
        appendUnique(vmdFiles, file)
        const probe = probeMap[file]
        if (!probe) {
          appendUnique(cameraFiles, file)
        } else {
          if (probe.cameraFrames > 0) appendUnique(cameraFiles, file)
          if (probe.tendency === 'facialOnly') appendUnique(facialOnlyFiles, file)
        }
      } else if (ext === '.gkcamera') {
        appendUnique(cameraFiles, file)
      } else if (isSupportedAudioExtension(file)) {
        appendUnique(audioFiles, file)
      }
    }

    const motionCandidates = classification.motionCandidates
    let positions: ImportPositionFormState[]
    if (motionCandidates.length >= 2) {
      const separateFacial =
        classification.facialCandidate !== '' && probeMap[classification.facialCandidate]?.tendency === 'facialOnly'
          ? classification.facialCandidate
          : ''
      positions = motionCandidates.map((motionPath, idx) => {
        const motionCands = [IMPORT_SLOT.empty, motionPath]
        const facialCands = [IMPORT_SLOT.autoFacial, ...facialOnlyFiles, IMPORT_SLOT.skip]
        const facialSel = idx === 0 && separateFacial ? findIndex(facialCands, separateFacial) : 0
        return {
          motion: { candidates: motionCands, selection: 1 },
          facial: { candidates: facialCands, selection: facialSel },
          x: 0,
          y: 0,
          z: 0,
          yawDegrees: 0,
          preserveExisting: false,
        }
      })
    } else {
      const motionCands = [IMPORT_SLOT.empty, ...vmdFiles]
      const facialCands = [IMPORT_SLOT.autoFacial, ...vmdFiles, IMPORT_SLOT.skip]
      positions = [
        {
          motion: { candidates: motionCands, selection: findIndex(motionCands, classification.motionCandidate) },
          facial: { candidates: facialCands, selection: findIndex(facialCands, classification.facialCandidate) },
          x: 0,
          y: 0,
          z: 0,
          yawDegrees: 0,
          preserveExisting: false,
        },
      ]
    }

    const camCands = [IMPORT_SLOT.empty, ...(motionCandidates.length >= 2 ? cameraFiles : [...cameraFiles, ...vmdFiles.filter((f) => !cameraFiles.includes(f))])]
    const audioCands = [IMPORT_SLOT.autoAudio, ...audioFiles, IMPORT_SLOT.skip]
    const slots = {
      motion: positions[0].motion,
      facial: positions[0].facial,
      camera: { candidates: camCands, selection: findIndex(camCands, classification.cameraCandidate) },
      audio: { candidates: audioCands, selection: findIndex(audioCands, classification.audioCandidate) },
    }
    this.set({
      importForm: {
        ...this.state.importForm,
        title: classification.suggestedTitle,
        positions,
        slots,
        unsupportedAudio: classification.unsupportedAudioCount,
        audioProbe: null,
      },
    })
    if (classification.audioCandidate) {
      void this.probeImportAudio(classification.audioCandidate)
    }
  }

  openImportEdit = async (key: string): Promise<void> => {
    if (!this.supports('library.startEdit')) return
    const revision = this.state.library.revision
    try {
      const raw = await this.client().request('library.startEdit', { key, revision })
      const info = normalizeEditEntryInfo(raw, key)
      const form = emptyImportForm()
      form.open = true
      form.mode = 'edit'
      form.editKey = info.key
      form.editRevision = revision
      form.editInfo = info
      form.title = info.title
      form.subtitle = info.subtitle ?? ''
      form.timing = info.timing
      if (info.isGroup && info.positions && info.positions.length >= 2) {
        form.positions = info.positions.map((pos) => ({
          motion: { candidates: [IMPORT_SLOT.keepMotion], selection: 0 },
          facial: {
            candidates: pos.hasFacial ? [IMPORT_SLOT.clear, IMPORT_SLOT.keepTrack] : [IMPORT_SLOT.clear],
            selection: pos.hasFacial ? 1 : 0,
          },
          x: pos.x,
          y: pos.y,
          z: pos.z,
          yawDegrees: pos.yawDegrees,
          preserveExisting: true,
        }))
      } else {
        form.positions = [
          {
            motion: { candidates: [IMPORT_SLOT.keepMotion], selection: 0 },
            facial: {
              candidates: info.hasFacial ? [IMPORT_SLOT.clear, IMPORT_SLOT.keepTrack] : [IMPORT_SLOT.clear],
              selection: info.hasFacial ? 1 : 0,
            },
            x: 0,
            y: 0,
            z: 0,
            yawDegrees: 0,
            preserveExisting: true,
          },
        ]
      }
      form.slots.motion = form.positions[0].motion
      form.slots.facial = form.positions[0].facial
      form.slots.camera.candidates = [IMPORT_SLOT.clear]
      form.slots.audio.candidates = [IMPORT_SLOT.clear]
      if (info.hasCamera) {
        form.slots.camera.candidates.push(IMPORT_SLOT.keepTrack)
        form.slots.camera.selection = 1
      }
      if (info.hasAudio) {
        form.slots.audio.candidates.push(IMPORT_SLOT.keepTrack)
        form.slots.audio.selection = 1
      }
      const existingCover = this.state.coverCache[info.key] ?? ''
      if (existingCover) {
        form.coverDataUri = existingCover
      }
      this.set({ importForm: form, actionError: null })
      if ((info.hasCover || info.hasAudio) && !existingCover) {
        void this.ensureTrackCover(info.key).then(() => {
          const cur = this.state.importForm
          if (cur.open && cur.mode === 'edit' && cur.editKey === info.key && cur.coverMode === 'auto') {
            const loadedCover = this.state.coverCache[info.key] ?? ''
            if (loadedCover) this.set({ importForm: { ...this.state.importForm, coverDataUri: loadedCover } })
          }
        })
      }
    } catch (error) {
      if (error instanceof BridgeError && error.code === 'stale_revision') {
        this.set({ actionError: '曲库已变化，请刷新后重试。' })
      } else {
        this.reportActionError(error)
      }
    }
  }

  closeImportForm = (): void => {
    this.set({ importForm: emptyImportForm() })
  }

  addImportPosition = (): void => {
    const form = this.state.importForm
    if (!form.open) return
    const nextPos: ImportPositionFormState = {
      motion: { candidates: [IMPORT_SLOT.empty], selection: 0 },
      facial: {
        candidates: form.mode === 'edit' ? [IMPORT_SLOT.clear] : [IMPORT_SLOT.autoFacial, IMPORT_SLOT.skip],
        selection: 0,
      },
      x: 0,
      y: 0,
      z: 0,
      yawDegrees: 0,
      preserveExisting: false,
    }
    this.set({
      importForm: {
        ...form,
        positions: [...form.positions, nextPos],
      },
    })
  }

  removeImportPosition = (posIndex: number): void => {
    const form = this.state.importForm
    if (posIndex <= 0 || posIndex >= form.positions.length) return
    const removed = form.positions[posIndex]
    const removedMotion = removed.motion.candidates[removed.motion.selection] ?? ''
    const removedFacial = removed.facial.candidates[removed.facial.selection] ?? ''
    const remaining = form.positions.filter((_, idx) => idx !== posIndex)
    const stillUsedPaths = new Set<string>()
    for (const pos of remaining) {
      const m = pos.motion.candidates[pos.motion.selection] ?? ''
      const f = pos.facial.candidates[pos.facial.selection] ?? ''
      if (!isSlotPlaceholder(m)) stillUsedPaths.add(m)
      if (!isSlotPlaceholder(f)) stillUsedPaths.add(f)
    }
    const camSel = form.slots.camera.candidates[form.slots.camera.selection] ?? ''
    if (!isSlotPlaceholder(camSel)) stillUsedPaths.add(camSel)

    const pruneCandidates = (slot: ImportSlotState): ImportSlotState => {
      const selected = slot.candidates[slot.selection] ?? ''
      const nextCandidates = slot.candidates.filter(
        (c) =>
          (c !== removedMotion && c !== removedFacial) ||
          c === selected ||
          stillUsedPaths.has(c) ||
          isSlotPlaceholder(c),
      )
      const nextSelection = Math.max(0, nextCandidates.indexOf(selected))
      return { candidates: nextCandidates, selection: nextSelection }
    }

    const cleanedPositions = remaining.map((pos) => ({
      ...pos,
      motion: pruneCandidates(pos.motion),
      facial: pruneCandidates(pos.facial),
    }))
    const cleanedCamera = pruneCandidates(form.slots.camera)
    this.set({
      importForm: {
        ...form,
        positions: cleanedPositions,
        slots: {
          ...form.slots,
          motion: cleanedPositions[0].motion,
          facial: cleanedPositions[0].facial,
          camera: cleanedCamera,
        },
      },
    })
  }

  setImportPositionSelection = (posIndex: number, kind: 'motion' | 'facial', selection: number): void => {
    const form = this.state.importForm
    if (posIndex < 0 || posIndex >= form.positions.length) return
    const positions = form.positions.map((pos, idx) => {
      if (idx !== posIndex) return pos
      const chosen = pos[kind].candidates[selection] ?? ''
      return {
        ...pos,
        [kind]: { ...pos[kind], selection },
        ...(kind === 'motion' && chosen !== IMPORT_SLOT.keepMotion ? { preserveExisting: false } : {}),
      }
    })
    this.set({
      importForm: {
        ...form,
        positions,
        slots: {
          ...form.slots,
          motion: positions[0].motion,
          facial: positions[0].facial,
        },
      },
    })
  }

  setImportPositionPlacement = (
    posIndex: number,
    placement: { x: number; y: number; z: number; yawDegrees: number },
  ): void => {
    const form = this.state.importForm
    if (posIndex <= 0 || posIndex >= form.positions.length) return
    const finite = (v: number) => (Number.isFinite(v) ? v : 0)
    const positions = form.positions.map((pos, idx) =>
      idx === posIndex
        ? {
            ...pos,
            x: finite(placement.x),
            y: finite(placement.y),
            z: finite(placement.z),
            yawDegrees: finite(placement.yawDegrees),
          }
        : pos,
    )
    this.set({ importForm: { ...form, positions } })
  }

  importPickPositionFile = async (posIndex: number, kind: 'motion' | 'facial', file: string): Promise<void> => {
    await this.probePaths([file])
    const form = this.state.importForm
    if (posIndex < 0 || posIndex >= form.positions.length) return
    const positions = form.positions.map((pos, idx) => {
      if (idx !== posIndex) return pos
      const candidates = [...pos[kind].candidates]
      let selection = candidates.indexOf(file)
      if (selection < 0) {
        candidates.push(file)
        selection = candidates.length - 1
      }
      return {
        ...pos,
        [kind]: { candidates, selection },
        ...(kind === 'motion' ? { preserveExisting: false } : {}),
      }
    })
    this.set({
      importForm: {
        ...form,
        positions,
        slots: {
          ...form.slots,
          motion: positions[0].motion,
          facial: positions[0].facial,
        },
      },
    })
  }

  importAppendFiles = async (files: string[]): Promise<void> => {
    await this.probePaths(files)
    const next = this.state.importForm
    const probeMap = next.probes
    let positions = next.positions.map((pos) => ({
      ...pos,
      motion: { ...pos.motion, candidates: [...pos.motion.candidates] },
      facial: { ...pos.facial, candidates: [...pos.facial.candidates] },
    }))
    const camera = { ...next.slots.camera, candidates: [...next.slots.camera.candidates] }
    const audio = { ...next.slots.audio, candidates: [...next.slots.audio.candidates] }
    const appendUnique = (list: string[], item: string) => {
      if (!list.includes(item)) list.push(item)
    }
    const insertBeforeSkip = (list: string[], item: string): number => {
      const existing = list.indexOf(item)
      if (existing >= 0) return existing
      const skipIdx = list.indexOf(IMPORT_SLOT.skip)
      if (skipIdx >= 0) {
        list.splice(skipIdx, 0, item)
        return skipIdx
      }
      list.push(item)
      return list.length - 1
    }

    let appendedAudio: string | null = null
    for (const file of files) {
      const dot = file.lastIndexOf('.')
      const ext = dot >= 0 ? file.slice(dot).toLowerCase() : ''
      if (isSupportedAudioExtension(file)) {
        const idx = insertBeforeSkip(audio.candidates, file)
        const currentAudio = audio.candidates[audio.selection] ?? ''
        if (isSlotPlaceholder(currentAudio) && currentAudio !== IMPORT_SLOT.keepTrack) {
          audio.selection = idx
          appendedAudio = file
        }
      } else if (ext === '.gkcamera') {
        appendUnique(camera.candidates, file)
        const currentCam = camera.candidates[camera.selection] ?? ''
        if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
          camera.selection = camera.candidates.indexOf(file)
        }
      } else if (ext === '.vmd') {
        const probe = probeMap[file]
        if (probe && !canUseAsMotion(probe)) {
          if (probe.cameraFrames > 0) {
            appendUnique(camera.candidates, file)
            const currentCam = camera.candidates[camera.selection] ?? ''
            if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
              camera.selection = camera.candidates.indexOf(file)
            }
          }
          if (probe.morphFrames > 0 || probe.eyeBoneFrames > 0) {
            for (let p = 0; p < positions.length; p += 1) {
              const idx = insertBeforeSkip(positions[p].facial.candidates, file)
              const curFacial = positions[p].facial.candidates[positions[p].facial.selection] ?? ''
              if (p === 0 && isSlotPlaceholder(curFacial) && curFacial !== IMPORT_SLOT.keepTrack) {
                positions[p].facial.selection = idx
              }
            }
          }
        } else {
          const emptyIdx = positions.findIndex((pos) => {
            const picked = pos.motion.candidates[pos.motion.selection] ?? ''
            return picked === IMPORT_SLOT.empty
          })
          if (emptyIdx >= 0) {
            appendUnique(positions[emptyIdx].motion.candidates, file)
            positions[emptyIdx].motion.selection = positions[emptyIdx].motion.candidates.indexOf(file)
            positions[emptyIdx].preserveExisting = false
          } else {
            positions.push({
              motion: { candidates: [IMPORT_SLOT.empty, file], selection: 1 },
              facial: {
                candidates: next.mode === 'edit' ? [IMPORT_SLOT.clear] : [IMPORT_SLOT.autoFacial, IMPORT_SLOT.skip],
                selection: 0,
              },
              x: 0,
              y: 0,
              z: 0,
              yawDegrees: 0,
              preserveExisting: false,
            })
          }
        }
      }
    }

    this.set({
      importForm: {
        ...next,
        positions,
        slots: {
          motion: positions[0].motion,
          facial: positions[0].facial,
          camera,
          audio,
        },
        ...(appendedAudio ? { audioProbe: null } : {}),
      },
    })
    if (appendedAudio) {
      void this.probeImportAudio(appendedAudio)
    }
  }

  importPickSlotFile = async (slot: ImportSlotKey, file: string): Promise<void> => {
    if (slot === 'motion' || slot === 'facial') {
      await this.importPickPositionFile(0, slot, file)
      return
    }
    await this.probePaths([file])
    const slotState = this.state.importForm.slots[slot]
    const candidates = [...slotState.candidates]
    let selection = candidates.indexOf(file)
    if (selection < 0) {
      candidates.push(file)
      selection = candidates.length - 1
    }
    this.set({
      importForm: {
        ...this.state.importForm,
        slots: { ...this.state.importForm.slots, [slot]: { candidates, selection } },
        ...(slot === 'audio' ? { audioProbe: null } : {}),
      },
    })
    if (slot === 'audio' && !isSlotPlaceholder(file)) {
      void this.probeImportAudio(file)
    }
  }

  setImportSelection = (slot: ImportSlotKey, selection: number): void => {
    if (slot === 'motion' || slot === 'facial') {
      this.setImportPositionSelection(0, slot, selection)
      return
    }
    const picked = this.state.importForm.slots[slot].candidates[selection] ?? ''
    this.set({
      importForm: {
        ...this.state.importForm,
        slots: { ...this.state.importForm.slots, [slot]: { ...this.state.importForm.slots[slot], selection } },
        ...(slot === 'audio' ? { audioProbe: null } : {}),
      },
    })
    if (slot === 'audio' && !isSlotPlaceholder(picked)) {
      void this.probeImportAudio(picked)
    }
  }

  probeImportAudio = async (path: string): Promise<void> => {
    if (!path || isSlotPlaceholder(path) || !this.supports('library.probeAudio')) return
    try {
      const result = await this.client().request('library.probeAudio', { path, includeCover: true })
      const currentAudio = this.importFormPicks().audio
      if (currentAudio !== path) return
      const form = this.state.importForm
      const nextPatch: Partial<ImportFormState> = { audioProbe: result }
      if (result.valid) {
        if (form.subtitle.trim() === '' && result.artist.trim() !== '') {
          nextPatch.subtitle = result.artist.trim()
        }
        if (form.mode === 'new' && form.title.trim() === '' && result.title.trim() !== '') {
          nextPatch.title = result.title.trim()
        }
      }
      this.set({ importForm: { ...form, ...nextPatch } })
    } catch {
      // Probe failures fall back gracefully without blocking validation.
    }
  }

  ensureTrackCover = async (key: string): Promise<void> => {
    if (!key) return
    if (Object.prototype.hasOwnProperty.call(this.state.coverCache, key) || this.coverPending.has(key)) return
    const entry = this.state.library.entries.find((item) => item.id === key)
    if (!entry) return
    if (!entry.hasCover && !entry.hasAudio) {
      this.set({ coverCache: { ...this.state.coverCache, [key]: '' } })
      return
    }
    if (!this.supports('library.getCover')) return
    const generation = this.coverGeneration
    this.coverPending.add(key)
    try {
      const res = await this.client().request('library.getCover', { key })
      if (generation !== this.coverGeneration) return
      if (!this.state.library.entries.some((item) => item.id === key)) return
      const uri = res.hasCover && res.coverDataUri ? res.coverDataUri : ''
      this.set({ coverCache: { ...this.state.coverCache, [key]: uri } })
    } catch {
      // Leave cache clean on transient bridge error so it can retry later.
    } finally {
      this.coverPending.delete(key)
    }
  }

  preloadLibraryCovers = (entries: LibraryEntry[], priorityIds: string[] = []): void => {
    if (!this.supports('library.getCover') || entries.length === 0) return
    const entryById = new Map(entries.map((item) => [item.id, item]))
    const orderedIds: string[] = []
    const seen = new Set<string>()
    const enqueueIfNeeded = (id: string | null | undefined) => {
      if (!id || seen.has(id)) return
      seen.add(id)
      const entry = entryById.get(id)
      if (!entry || (!entry.hasCover && !entry.hasAudio)) return
      if (
        Object.prototype.hasOwnProperty.call(this.state.coverCache, id) ||
        this.coverPending.has(id) ||
        this.coverQueue.includes(id)
      ) {
        return
      }
      orderedIds.push(id)
    }

    enqueueIfNeeded(this.state.loaded.motionKey)
    enqueueIfNeeded(this.state.selectedLibraryKey)
    for (const id of priorityIds) enqueueIfNeeded(id)
    for (const entry of entries) enqueueIfNeeded(entry.id)

    if (orderedIds.length === 0) return
    this.coverQueue.push(...orderedIds)

    const CONCURRENCY = 5
    const spawnWorker = () => {
      this.coverWorkersRunning += 1
      void (async () => {
        try {
          while (this.started && this.coverQueue.length > 0) {
            const nextId = this.coverQueue.shift()
            if (!nextId) continue
            await this.ensureTrackCover(nextId)
          }
        } finally {
          this.coverWorkersRunning -= 1
        }
      })()
    }
    while (this.coverWorkersRunning < CONCURRENCY && this.coverQueue.length > 0) {
      spawnWorker()
    }
  }

  setImportCover = (coverMode: ImportCoverMode, coverDataUri = ''): void => {
    this.set({
      importForm: {
        ...this.state.importForm,
        coverMode,
        coverDataUri: coverMode === 'clear' ? '' : coverDataUri,
      },
    })
  }

  openCoverEditorForTrack = (key: string, owner: 'library' | 'transport' = 'library'): void => {
    const entry = this.state.library.entries.find((item) => item.id === key)
    if (!entry) return
    const cached = this.state.coverCache[key] ?? ''
    this.set({
      coverEditor: {
        ...emptyCoverEditor(),
        open: true,
        owner,
        mode: 'track',
        trackKey: entry.id,
        trackTitle: entry.title,
        sourceImageUri: cached,
      },
    })
    this.syncDropTarget()
    if (!cached && (entry.hasCover || entry.hasAudio)) {
      void this.ensureTrackCover(entry.id).then(() => {
        const cur = this.state.coverEditor
        if (cur.open && cur.mode === 'track' && cur.trackKey === entry.id && !cur.sourceImageUri) {
          const loaded = this.state.coverCache[entry.id] ?? ''
          if (loaded) this.set({ coverEditor: { ...this.state.coverEditor, sourceImageUri: loaded } })
        }
      })
    }
    void this.refreshScreenshotCapabilities()
  }

  openCoverEditorForImport = (): void => {
    const form = this.state.importForm
    if (!form.open) return
    const initial =
      form.coverMode === 'custom'
        ? form.coverDataUri
        : form.coverMode === 'auto'
          ? form.audioProbe?.hasCover && form.audioProbe.coverDataUri
            ? form.audioProbe.coverDataUri
            : form.coverDataUri
          : ''
    this.set({
      coverEditor: {
        ...emptyCoverEditor(),
        open: true,
        owner: 'import',
        mode: 'import',
        trackKey: form.editKey,
        trackTitle: form.title || '新导入曲目',
        sourceImageUri: initial,
      },
    })
    this.syncDropTarget()
    void this.refreshScreenshotCapabilities()
  }

  closeCoverEditor = (): void => {
    this.set({ coverEditor: emptyCoverEditor() })
    this.syncDropTarget()
  }

  setCoverEditorImageUri = (uri: string): void => {
    this.set({
      coverEditor: {
        ...this.state.coverEditor,
        sourceImageUri: uri,
        error: '',
      },
    })
  }

  setCoverEditorError = (error: string): void => {
    if (!this.state.coverEditor.open) return
    this.set({
      coverEditor: {
        ...this.state.coverEditor,
        error,
      },
    })
  }

  refreshScreenshotCapabilities = async (): Promise<void> => {
    if (!this.supports('screenshot.capabilities')) {
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          screenshotCaps: {
            available: false,
            runtimeReady: false,
            cameraAvailable: false,
            windowWidth: 0,
            windowHeight: 0,
            notice: '当前桥接不支持 ScreenshotManager 拍照联动',
          },
        },
      })
      return
    }
    try {
      const caps = await this.client().request('screenshot.capabilities')
      if (!this.state.coverEditor.open) return
      this.set({ coverEditor: { ...this.state.coverEditor, screenshotCaps: caps } })
    } catch {
      if (!this.state.coverEditor.open) return
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          screenshotCaps: {
            available: false,
            runtimeReady: false,
            cameraAvailable: false,
            windowWidth: 0,
            windowHeight: 0,
            notice: 'ScreenshotManager 联动服务暂不可用',
          },
        },
      })
    }
  }

  captureScreenshotForCover = async (): Promise<void> => {
    if (!this.supports('screenshot.capture') || this.state.coverEditor.capturing) return
    this.set({ coverEditor: { ...this.state.coverEditor, capturing: true, error: '' } })
    try {
      const res = await this.client().request('screenshot.capture')
      if (!this.state.coverEditor.open) return
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          capturing: false,
          sourceImageUri: res.dataUri,
          error: '',
        },
      })
    } catch (error) {
      if (!this.state.coverEditor.open) return
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          capturing: false,
          error: error instanceof Error ? error.message : String(error),
        },
      })
    }
  }

  saveCoverEditor = async (croppedPngDataUri: string): Promise<void> => {
    const editor = this.state.coverEditor
    if (!editor.open || editor.saving || !croppedPngDataUri) return
    if (editor.mode === 'import') {
      this.setImportCover('custom', croppedPngDataUri)
      this.closeCoverEditor()
      return
    }
    if (!this.supports('library.setCover')) {
      this.set({ coverEditor: { ...editor, error: '当前原生桥接不支持保存封面（library.setCover）' } })
      return
    }
    this.set({ coverEditor: { ...editor, saving: true, error: '' } })
    try {
      const res = await this.client().request('library.setCover', {
        key: editor.trackKey,
        coverDataUri: croppedPngDataUri,
      })
      const thumb = res.coverDataUri || croppedPngDataUri
      this.coverGeneration += 1
      this.coverPending.delete(editor.trackKey)
      this.set({
        coverCache: { ...this.state.coverCache, [editor.trackKey]: thumb },
        coverEditor: emptyCoverEditor(),
        actionError: null,
      })
    } catch (error) {
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          saving: false,
          error: error instanceof Error ? error.message : String(error),
        },
      })
    }
  }

  clearCoverFromEditor = async (): Promise<void> => {
    const editor = this.state.coverEditor
    if (!editor.open || editor.saving) return
    if (editor.mode === 'import') {
      this.setImportCover('clear', '')
      this.closeCoverEditor()
      return
    }
    if (!this.supports('library.setCover')) {
      this.set({ coverEditor: { ...editor, error: '当前原生桥接不支持清除封面（library.setCover）' } })
      return
    }
    this.set({ coverEditor: { ...editor, saving: true, error: '' } })
    try {
      await this.client().request('library.setCover', { key: editor.trackKey, clear: true })
      this.coverGeneration += 1
      this.coverPending.delete(editor.trackKey)
      this.set({
        coverCache: { ...this.state.coverCache, [editor.trackKey]: '' },
        coverEditor: emptyCoverEditor(),
        actionError: null,
      })
    } catch (error) {
      this.set({
        coverEditor: {
          ...this.state.coverEditor,
          saving: false,
          error: error instanceof Error ? error.message : String(error),
        },
      })
    }
  }

  setClockRunMode = async (runMode: ClockRunMode): Promise<void> => {
    try {
      const next = await this.client().request('clock.setRunMode', { runMode })
      this.set({ clock: normalizeClockState(next), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  configureClock = async (patch: {
    source?: ClockSource
    fpsNumerator?: number
    fpsDenominator?: number
  }): Promise<void> => {
    try {
      const next = await this.client().request('clock.configure', patch)
      this.set({ clock: normalizeClockState(next), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setAudioEffect = async (presetId: string): Promise<void> => {
    try {
      const next = await this.client().request('audioEffect.set', { presetId })
      this.set({ audioEffect: normalizeAudioEffectState(next), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setImportTitle = (title: string): void => {
    this.set({ importForm: { ...this.state.importForm, title } })
  }

  setImportSubtitle = (subtitle: string): void => {
    this.set({ importForm: { ...this.state.importForm, subtitle } })
  }

  setImportSourceFps = (sourceFps: number): void => {
    const normalized = sourceFps === 25 || sourceFps === 30 || sourceFps === 60 ? sourceFps : 0
    this.set({ importForm: { ...this.state.importForm, sourceFps: normalized } })
  }

  setImportOutputFps = (fps: number): void => {
    const normalized = fps === 30 || fps === 60 ? fps : 0
    this.set({ importForm: { ...this.state.importForm, fps: normalized } })
  }

  setImportTiming = (slot: ImportSlotKey, patch: Partial<TrackTiming>): void => {
    const timing = { ...this.state.importForm.timing, [slot]: { ...this.state.importForm.timing[slot], ...patch } }
    const track = timing[slot]
    if (!Number.isFinite(track.speed) || track.speed < 0) track.speed = track.speed === 0 ? 0 : 1
    if (!Number.isFinite(track.offsetSeconds)) track.offsetSeconds = 0
    this.set({ importForm: { ...this.state.importForm, timing } })
  }

  importFormPicks = () => {
    const form = this.state.importForm
    const pickSlot = (slotState: ImportSlotState): string => slotState.candidates[slotState.selection] ?? ''
    const posPicks = form.positions.map((pos) => ({
      motion: pickSlot(pos.motion),
      facial: pickSlot(pos.facial),
      preserveExisting: pos.preserveExisting,
    }))
    return {
      isEditMode: form.mode === 'edit',
      motion: posPicks[0]?.motion ?? '',
      facial: posPicks[0]?.facial ?? '',
      camera: pickSlot(form.slots.camera),
      audio: pickSlot(form.slots.audio),
      title: form.title,
      positions: posPicks,
    }
  }

  importFormError = (): string | null => {
    const form = this.state.importForm
    const editMissing =
      form.mode === 'edit' && !this.state.library.entries.some((entry) => entry.id === form.editKey)
    return validateImportForm(
      this.importFormPicks(),
      (path) => this.state.importForm.probes[path] ?? null,
      this.state.importProgress?.active === true,
      editMissing,
    )
  }

  isEditingActiveTrack = (): boolean => {
    const form = this.state.importForm
    const { loaded, player } = this.state
    return (
      form.mode === 'edit' &&
      form.editKey !== '' &&
      loaded.motionKey === form.editKey &&
      (player.state === 'playing' || player.state === 'paused')
    )
  }

  buildImportRequest = (): ImportRequest => {
    const form = this.state.importForm
    const picks = this.importFormPicks()
    const request = defaultImportRequest()
    request.title = form.title.trim() !== '' ? form.title : '未命名演出'
    request.subtitle = form.subtitle.trim()
    request.fps = form.fps
    request.sourceFps = form.sourceFps
    request.rebakeOutput = form.mode === 'edit' && form.fps > 0
    request.timing = form.timing
    const isDisk = (path: string) => !isSlotPlaceholder(path)
    const isGroup = form.positions.length >= 2
    request.isGroup = isGroup

    if (isGroup) {
      request.positions = form.positions.map((pos, idx) => {
        const mPick = pos.motion.candidates[pos.motion.selection] ?? ''
        const fPick = pos.facial.candidates[pos.facial.selection] ?? ''
        const preserveExisting = form.mode === 'edit' && pos.preserveExisting && isSlotPlaceholder(mPick)
        const disableMorph = fPick === IMPORT_SLOT.clear || fPick === IMPORT_SLOT.skip
        return {
          slot: idx + 1,
          ...(isDisk(mPick) ? { motionFile: mPick, motionVmd: mPick } : {}),
          ...(isDisk(fPick) ? { facialFile: fPick, morphVmd: fPick } : {}),
          disableMorph,
          preserveExisting,
          x: pos.x,
          y: pos.y,
          z: pos.z,
          yawDegrees: pos.yawDegrees,
        }
      })
    } else {
      request.positions = []
    }

    if (form.mode === 'edit') {
      request.editKey = form.editKey
      if (isDisk(picks.motion)) request.motionFile = picks.motion
      if (picks.facial === IMPORT_SLOT.clear || picks.facial === IMPORT_SLOT.empty || picks.facial === IMPORT_SLOT.skip) {
        request.disableMorph = true
      } else if (isDisk(picks.facial)) {
        request.facialFile = picks.facial
      }
      if (picks.camera === IMPORT_SLOT.clear || picks.camera === IMPORT_SLOT.empty) {
        request.clearCamera = true
      } else if (isDisk(picks.camera)) {
        request.cameraFile = picks.camera
      }
      if (picks.audio === IMPORT_SLOT.clear || picks.audio === IMPORT_SLOT.empty || picks.audio === IMPORT_SLOT.skip) {
        request.clearAudio = true
      } else if (isDisk(picks.audio)) {
        request.audioFile = picks.audio
      }
    } else {
      request.motionFile = picks.motion
      if (picks.facial === IMPORT_SLOT.skip) request.disableMorph = true
      else if (isDisk(picks.facial)) request.facialFile = picks.facial
      if (isDisk(picks.camera)) request.cameraFile = picks.camera
      if (picks.audio === IMPORT_SLOT.skip) request.clearAudio = true
      else if (isDisk(picks.audio)) request.audioFile = picks.audio
    }
    if (form.coverMode === 'clear') {
      request.clearCover = true
    } else if (form.coverMode === 'custom' && form.coverDataUri) {
      request.coverDataUri = form.coverDataUri
    } else if (form.mode === 'new' && form.audioProbe?.hasCover && form.audioProbe.coverDataUri) {
      request.coverDataUri = form.audioProbe.coverDataUri
    }
    return request
  }

  submitImport = async (): Promise<void> => {
    if (this.importFormError()) return
    if (this.isEditingActiveTrack()) {
      this.set({
        importForm: {
          ...this.state.importForm,
          activeTrackWarning: '当前曲目正在播放，无法替换其资源文件。请停止播放后重试。你的修改尚未丢失。',
        },
      })
      return
    }
    const formSnapshot = this.state.importForm
    const request = this.buildImportRequest()
    this.pendingImportOutcome = {
      editKey: formSnapshot.mode === 'edit' ? formSnapshot.editKey : '',
      existingIds: new Set(this.state.library.entries.map((entry) => entry.id)),
      coverMode: formSnapshot.coverMode,
      coverDataUri: request.coverDataUri ?? '',
      succeededKey: null,
    }
    try {
      await this.client().request('library.import', { request })
    } catch (error) {
      this.pendingImportOutcome = null
      this.reportActionError(error)
      return
    }
    const now = Date.now()
    this.set({
      importForm: emptyImportForm(),
      importUi: {
        ...emptyImportUi(),
        visible: true,
        awaitingStart: true,
        startTime: now,
        minDisplayUntil: now + 550,
        displayStatus: '正在启动导入流程...',
        lastProgress: this.state.importProgress,
      },
      actionError: null,
    })
    this.startImportUiTimer()
  }

  stopPlaybackAndSave = async (): Promise<void> => {
    if (!this.isEditingActiveTrack()) return
    this.set({ importForm: { ...this.state.importForm, pendingStopAndSave: true } })
    try {
      await this.client().request('player.stop')
    } catch (error) {
      this.set({ importForm: { ...this.state.importForm, pendingStopAndSave: false } })
      this.reportActionError(error)
      return
    }
    this.maybeAutoSubmitAfterStop()
  }

  private onPlayerStateChanged(player: PlayerState): void {
    this.set({ player })
    this.maybeAutoSubmitAfterStop()
  }

  private maybeAutoSubmitAfterStop(): void {
    const form = this.state.importForm
    if (!form.open || !form.pendingStopAndSave) return
    if (this.isEditingActiveTrack()) return
    if (this.importFormError()) return
    void this.submitImport()
  }

  // ---- Global import progress modal (fix-issue-5 semantics) ----

  private maybeFinalizeImportSelection(library: LibraryState): void {
    const pending = this.pendingImportOutcome
    if (!pending) return
    let targetKey = ''
    if (pending.succeededKey && library.entries.some((entry) => entry.id === pending.succeededKey)) {
      targetKey = pending.succeededKey
    } else if (pending.editKey !== '') {
      if (pending.succeededKey === null) return
      if (library.entries.some((entry) => entry.id === pending.editKey)) {
        targetKey = pending.editKey
      }
    } else {
      const added = library.entries.find((entry) => !pending.existingIds.has(entry.id))
      if (added) targetKey = added.id
    }
    if (!targetKey) return
    const isNew = pending.editKey === ''
    const customCover = pending.coverMode === 'custom' ? pending.coverDataUri : ''
    this.pendingImportOutcome = null
    const patch: Partial<MotionAppState> = { selectedLibraryKey: targetKey }
    if (customCover) {
      patch.coverCache = { ...this.state.coverCache, [targetKey]: customCover }
    }
    this.set(patch)
    if (
      isNew &&
      this.state.loaded.motionKey !== targetKey &&
      this.state.player.state !== 'playing' &&
      this.supports('library.select')
    ) {
      void this.selectMotion(targetKey, false)
    }
  }

  private onImportProgress(importProgress: ImportProgress): void {
    this.set({ importProgress })
    const ui = this.state.importUi
    const now = Date.now()
    const staleSuccess =
      ui.visible && ui.awaitingStart && importProgress.finished && importProgress.succeeded && now < ui.startTime + 250
    if (!staleSuccess && importProgress.finished) {
      if (importProgress.succeeded) {
        if (this.pendingImportOutcome) {
          this.pendingImportOutcome.succeededKey = importProgress.importedKey || this.pendingImportOutcome.editKey || ''
          this.maybeFinalizeImportSelection(this.state.library)
        }
      } else {
        this.pendingImportOutcome = null
      }
    }
    if (!ui.visible) return
    const next = { ...ui, lastProgress: importProgress }
    if (importProgress.active) next.awaitingStart = false
    if (!staleSuccess && importProgress.finished) {
      next.awaitingStart = false
      if (importProgress.succeeded) {
        if (now >= ui.minDisplayUntil) {
          next.visible = false
        } else {
          next.displayProgress = 1
          next.displayStatus = importProgress.statusText !== '' ? importProgress.statusText : '导入完成'
          next.indeterminate = false
        }
      } else {
        next.visible = false
        next.errorModalOpen = true
        next.errorDetail =
          importProgress.errorDetail !== ''
            ? importProgress.errorDetail
            : this.state.playlist.notice !== ''
              ? this.state.playlist.notice
              : '原生导入遇到未知错误，请查看导入日志'
      }
    } else if (!staleSuccess) {
      next.displayProgress = importProgress.progress
      next.displayStatus =
        importProgress.statusText !== ''
          ? importProgress.statusText
          : importProgress.stage !== ''
            ? importProgress.stage
            : '正在处理演出数据...'
      next.indeterminate = false
    }
    this.set({ importUi: next })
    if (!next.visible) this.stopImportUiTimer()
  }

  private startImportUiTimer(): void {
    if (this.importUiTimer) return
    this.importUiTimer = setInterval(() => this.tickImportUi(), 100)
  }

  private stopImportUiTimer(): void {
    if (this.importUiTimer) {
      clearInterval(this.importUiTimer)
      this.importUiTimer = null
    }
  }

  private tickImportUi(): void {
    const ui = this.state.importUi
    if (!ui.visible) {
      this.stopImportUiTimer()
      return
    }
    const now = Date.now()
    const progress = ui.lastProgress
    const importBusy = this.state.importProgress?.active === true
    if (progress) {
      if (progress.active || importBusy) {
        if (ui.awaitingStart) this.set({ importUi: { ...ui, awaitingStart: false } })
        return
      }
      if (progress.finished) {
        // Completion handling happened in onImportProgress; only min-display
        // expiry for an already-finished success remains.
        if (progress.succeeded && now >= ui.minDisplayUntil) {
          this.set({ importUi: { ...ui, visible: false } })
          this.stopImportUiTimer()
        }
        return
      }
      return
    }
    if (!importBusy && (!ui.awaitingStart || now >= ui.minDisplayUntil)) {
      this.set({ importUi: { ...ui, visible: false } })
      this.stopImportUiTimer()
      return
    }
    if (!ui.indeterminate || ui.displayStatus === '') {
      this.set({
        importUi: {
          ...ui,
          indeterminate: true,
          displayProgress: 0.5,
          displayStatus: ui.displayStatus !== '' ? ui.displayStatus : '正在导入演出曲目...',
        },
      })
    }
  }

  closeImportError = (): void => {
    this.set({ importUi: { ...this.state.importUi, errorModalOpen: false, errorDetail: '' } })
  }

  // ---- Preview-only modal & state triggers ----

  previewImportFlow = (outcome: 'success' | 'error'): void => {
    const now = Date.now()
    this.set({
      importUi: {
        ...emptyImportUi(),
        visible: true,
        awaitingStart: true,
        startTime: now,
        minDisplayUntil: now + (outcome === 'success' ? 1350 : 550),
        displayProgress: 0.15,
        displayStatus: outcome === 'error' ? '正在启动模拟导入（将触发出错）...' : '正在启动模拟导入流程...',
        lastProgress: null,
      },
      actionError: null,
    })
    this.startImportUiTimer()
    if (this.bridge instanceof MotionMockBridgeClient) {
      this.bridge.simulateImportProgress(outcome)
    }
  }

  previewImportErrorDirect = (
    detail = "System.IO.IOException: The process cannot access the file 'audio.wav' because it is being used by another process.",
  ): void => {
    this.stopImportUiTimer()
    this.set({
      importUi: {
        ...emptyImportUi(),
        errorModalOpen: true,
        errorDetail: detail,
      },
    })
  }

  previewDeleteDialog = (stale = false): void => {
    const entries = this.state.library.entries
    const target =
      entries.find((item) => item.id === this.state.selectedLibraryKey) ??
      entries[0] ??
      null
    if (!target) return
    this.set({
      page: 'library',
      selectedLibraryKey: target.id,
      pendingDelete: {
        key: target.id,
        directory: target.directory,
        revision: this.state.library.revision,
      },
      deleteStale: stale,
    })
  }

  previewDropOverlay = (drop: DropUiState): void => {
    this.set({ drop })
    if (this.bridge instanceof MotionMockBridgeClient) {
      this.bridge.setDropPeekPreview(drop)
    }
  }

  previewActionError = (actionError: string | null): void => {
    this.set({ actionError })
  }

  previewDisconnect = (message = '模拟原生桥接异常断开（可用于验收断连重连界面）'): void => {
    this.set({ ready: true, connected: false, error: message })
  }

  // ---- File dialog (dialog.pickFile / dialog.poll) ----

  browseFiles = async (target: DialogTarget): Promise<void> => {
    if (this.state.dialogBusy || !this.supports('dialog.pickFile')) return
    const kind =
      target === 'library-add' || target === 'append'
        ? 0
        : target === 'motion' || target.startsWith('position-motion:')
          ? 1
          : target === 'facial' || target.startsWith('position-facial:')
            ? 2
            : target === 'camera'
              ? 3
              : target === 'audio'
                ? 4
                : target === 'cover-image'
                  ? 6
                  : 5
    const multiple = target === 'library-add' || target === 'append'
    try {
      const { dialogId } = await this.client().request('dialog.pickFile', { kind, multiple })
      this.dialogId = dialogId
      this.dialogTarget = target
      this.set({ dialogBusy: true })
      this.dialogTimer = setInterval(() => void this.pollDialog(), 200)
    } catch (error) {
      this.reportActionError(error)
    }
  }

  private async pollDialog(): Promise<void> {
    if (!this.state.dialogBusy) return
    try {
      const result = await this.client().request('dialog.poll', { dialogId: this.dialogId })
      if (result.state === 'pending') return
      this.finishDialog()
      if (result.state === 'accepted' && result.paths.length > 0) {
        const target = this.dialogTarget
        this.dialogTarget = null
        if (target === 'library-add') await this.openImportNew(result.paths)
        else if (target === 'append') await this.importAppendFiles(result.paths)
        else if (target === 'video') this.set({ studioVideoPath: result.paths[0] })
        else if (target === 'cover-image') await this.loadCoverPreviewFromPath(result.paths[0])
        else if (target?.startsWith('position-motion:')) {
          const posIndex = Number(target.slice('position-motion:'.length))
          await this.importPickPositionFile(posIndex, 'motion', result.paths[0])
        } else if (target?.startsWith('position-facial:')) {
          const posIndex = Number(target.slice('position-facial:'.length))
          await this.importPickPositionFile(posIndex, 'facial', result.paths[0])
        } else if (target === 'motion' || target === 'facial' || target === 'camera' || target === 'audio') {
          await this.importPickSlotFile(target, result.paths[0])
        }
      } else if (result.state === 'failed' && result.notice !== '') {
        this.set({ actionError: result.notice })
      }
    } catch (error) {
      this.finishDialog()
      this.reportActionError(error)
    }
  }

  loadCoverPreviewFromPath = async (path: string): Promise<void> => {
    if (!path) return
    if (!this.supports('library.readImagePreview')) {
      this.setCoverEditorError('当前原生桥接不支持读取本地图片预览（library.readImagePreview）')
      return
    }
    try {
      const { dataUri } = await this.client().request('library.readImagePreview', { path })
      if (dataUri) this.setCoverEditorImageUri(dataUri)
    } catch (error) {
      if (this.state.coverEditor.open) {
        this.set({
          coverEditor: {
            ...this.state.coverEditor,
            error: error instanceof Error ? error.message : String(error),
          },
        })
      } else {
        this.reportActionError(error)
      }
    }
  }

  private finishDialog(): void {
    if (this.dialogTimer) {
      clearInterval(this.dialogTimer)
      this.dialogTimer = null
    }
    this.set({ dialogBusy: false })
  }

  // ---- Whole-window drop (drop.setTarget / poll / peek) ----

  canAcceptDrop = (): boolean => {
    const state = this.state
    if (!state.permission.injectionAllowed) return false
    if (state.importProgress?.active === true) return false
    if (state.dialogBusy || state.pendingDelete !== null) return false
    if (state.coverEditor.open) {
      return !state.coverEditor.saving && !state.coverEditor.capturing
    }
    return (
      state.page === 'library' &&
      !state.library.refreshing &&
      !state.importForm.open
    )
  }

  retainDropWatch = (): void => {
    this.dropWatchCount += 1
    this.startDropWatch()
    this.syncDropTarget()
  }

  releaseDropWatch = (): void => {
    this.dropWatchCount = Math.max(0, this.dropWatchCount - 1)
    if (this.dropWatchCount === 0) {
      this.stopDropWatch()
    }
  }

  startDropWatch = (): void => {
    if (this.dropTimer || !this.supports('drop.setTarget') || !this.supports('drop.poll')) return
    this.dropTimer = setInterval(() => void this.tickDrop(), 200)
    void this.tickDrop()
  }

  stopDropWatch = (): void => {
    if (this.dropTimer) {
      clearInterval(this.dropTimer)
      this.dropTimer = null
    }
    if (this.dropTargetActive) {
      this.dropTargetActive = false
      void this.client().request('drop.setTarget', { enabled: false }).catch(() => {})
    }
    if (this.state.drop.visual !== 'none') {
      this.set({ drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false } })
    }
  }

  syncDropTarget = (): void => {
    if (!this.dropTimer) return
    if (this.canAcceptDrop() !== this.dropTargetActive) {
      void this.tickDrop()
    }
  }

  private async tickDrop(): Promise<void> {
    const ready = this.canAcceptDrop()
    if (ready !== this.dropTargetActive) {
      this.dropTargetActive = ready
      if (!ready && this.state.drop.visual !== 'none') {
        this.set({ drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false } })
      }
      try {
        await this.client().request('drop.setTarget', { enabled: ready })
      } catch {
        this.dropTargetActive = false
      }
    }
    if (!ready) return
    try {
      const peek = normalizeDropPeek(await this.client().request('drop.peek'))
      this.set({ drop: peek })
      const poll = await this.client().request('drop.poll')
      const files = Array.isArray(poll?.files) ? poll.files : []
      if (files.length > 0) this.handleDropFiles(files)
    } catch {
      // Transient polling failures are non-fatal; the next tick retries.
    }
  }

  handleDropFiles = (files: string[]): void => {
    if (this.state.coverEditor.open) {
      const imagePath = files.find((file) => isSupportedImageExtension(file))
      if (!imagePath) {
        this.setCoverEditorError('仅支持拖入图片文件（.png / .jpg / .jpeg / .webp / .bmp）。')
        return
      }
      void this.loadCoverPreviewFromPath(imagePath)
      return
    }
    const accepted: string[] = []
    let ignored = 0
    for (const file of files) {
      if (!isSupportedLibraryFile(file)) {
        ignored += 1
        continue
      }
      if (!accepted.includes(file)) accepted.push(file)
    }
    if (accepted.length === 0) {
      this.set({ dropNotice: '未找到可添加的文件。请拖入 VMD、GKCamera 或音频文件（.mp3/.flac/.wav/.ogg/.m4a/.aac/.opus）；不支持拖入文件夹。' })
      return
    }
    this.set({ dropNotice: ignored > 0 ? `已忽略 ${ignored} 个不支持或无法读取的文件。` : '' })
    void this.openImportNew(accepted)
  }

  // ---- Bone overrides (phase 2) ----

  selectBone = (hid: number): void => this.set({ selectedBoneHid: hid })

  setBoneSymmetrical = (enabled: boolean): void => this.set({ boneSymmetrical: enabled })

  boneOverrideFor = (hid: number) => this.state.bone.overrides.find((entry) => entry.humanoidId === hid) ?? null

  setBoneOverride = async (
    hid: number,
    pitch: number,
    yaw: number,
    roll: number,
    spaceWorld: boolean,
    enabled: boolean,
  ): Promise<void> => {
    try {
      await this.client().request('bone.set', {
        humanoidId: hid,
        boneName: boneNameByHid(hid),
        pitch,
        yaw,
        roll,
        spaceWorld,
        enabled,
        symmetrical: this.state.boneSymmetrical,
      })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  resetBoneOverride = async (hid: number): Promise<void> => {
    try {
      await this.client().request('bone.reset', {
        humanoidId: hid,
        boneName: boneNameByHid(hid),
        symmetrical: this.state.boneSymmetrical,
      })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  resetAllBoneOverrides = async (): Promise<void> => {
    try {
      await this.client().request('bone.resetAll')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setBoneMaster = async (enabled: boolean): Promise<void> => {
    try {
      await this.client().request('bone.setMaster', { enabled })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  reloadBoneOverrides = async (): Promise<void> => {
    try {
      await this.client().request('bone.reload')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  saveBoneOverridesToMotion = async (): Promise<void> => {
    try {
      await this.client().request('bone.saveToMotion')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  saveBoneOverridesToGlobal = async (): Promise<void> => {
    try {
      await this.client().request('bone.saveToGlobal')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Gaze (phase 2) ----

  setGazeSettings = async (patch: Partial<GazeSettings>): Promise<void> => {
    const settings = { ...this.state.gaze.settings, ...patch }
    try {
      await this.client().request('gaze.set', { settings })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  cycleGazeMode = (): void => {
    void this.setGazeSettings({ mode: (this.state.gaze.settings.mode + 1) % 3 })
  }

  // ---- Stage nodes / replacement (phase 2) ----

  scanStageNodes = async (): Promise<void> => {
    if (!this.supports('stage.scan')) return
    try {
      const res = await this.client().request('stage.scan')
      this.set({ stageNodes: normalizeStageNodes(asObj(res).nodes), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  toggleStageNode = async (index: number, active: boolean): Promise<void> => {
    try {
      await this.client().request('stage.toggleNode', { index, active })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setStageLoadReplacement = async (key: string): Promise<void> => {
    if (!this.supports('stage.setLoadReplacement')) return
    try {
      const res = await this.client().request('stage.setLoadReplacement', { key })
      this.set({ stageLoadNotice: asStr(asObj(res).notice), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Experiments / telemetry (phase 2) ----

  setExperiment = async (patch: Partial<ExperimentsState['settings']>): Promise<void> => {
    try {
      await this.client().request('experiments.set', { patch })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  refreshTelemetry = async (): Promise<void> => {
    if (!this.supports('telemetry.get')) return
    try {
      const raw = await this.client().request('telemetry.get')
      this.set({ telemetry: normalizeTelemetrySnapshot(raw), actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  // ---- Sole measurement v2 (phase 2) ----

  private onSoleChanged(sole: SoleState): void {
    const patch: Partial<MotionAppState> = { sole }
    if (this.state.soleDraftGeneration !== sole.generation) {
      patch.soleDraftGeneration = sole.generation
      patch.soleDraft = { ...sole.settings, planeCalibrated: false, shoeCalibrated: false }
    }
    this.set(patch)
  }

  patchSoleDraft = (patch: Partial<SoleSettings>): void => {
    this.set({ soleDraft: { ...this.state.soleDraft, ...patch } })
  }

  patchSoleDraftPoint = (side: number, index: number, point: Vector3): void => {
    const points = this.state.soleDraft.points.map((foot, footIndex) =>
      footIndex === side ? foot.map((existing, pointIndex) => (pointIndex === index ? point : existing)) : foot,
    )
    this.set({ soleDraft: { ...this.state.soleDraft, points } })
  }

  enableSoleBootProxy = (): void => {
    const draft = { ...this.state.soleDraft, enabled: true, useBootProxy: true, shoeCalibrated: false }
    this.set({ soleDraft: draft })
    void this.applySoleSettings(draft)
  }

  useSoleManualFallback = (): void => {
    const draft = { ...this.state.soleDraft, useBootProxy: false, shoeCalibrated: false }
    this.set({ soleDraft: draft })
    void this.applySoleSettings(draft)
  }

  normalizeSolePlaneNormal = (): void => {
    const normal = this.state.soleDraft.planeNormal
    const norm = Math.hypot(normal.x, normal.y, normal.z)
    if (!Number.isFinite(norm) || norm < 1e-4) return
    this.patchSoleDraft({ planeNormal: { x: normal.x / norm, y: normal.y / norm, z: normal.z / norm } })
  }

  setSoleHorizontalPlane = (): void => {
    this.patchSoleDraft({ planeNormal: { x: 0, y: 1, z: 0 } })
  }

  applySoleSettings = async (settings?: SoleSettings): Promise<void> => {
    const draft = settings ?? this.state.soleDraft
    const error = validateSole(draft)
    if (error) {
      this.set({ actionError: error })
      return
    }
    try {
      await this.client().request('sole.set', { settings: draft })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  reloadSoleDraft = (): void => {
    this.set({ soleDraft: { ...this.state.sole.settings } })
  }

  saveSoleSettings = async (): Promise<void> => {
    try {
      await this.client().request('sole.save')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  loadSoleSettings = async (): Promise<void> => {
    try {
      await this.client().request('sole.load')
    } catch (error) {
      this.reportActionError(error)
    }
  }

  setSolePlotSpan = (span: number): void => {
    this.set({ solePlotSpanM: Math.min(Math.max(span, 0.25), 2) })
  }

  startSolePolling = (): void => {
    if (this.soleTimer || !this.supports('sole.get')) return
    this.soleTimer = setInterval(() => {
      void this.client().request('sole.get').then((sole) => this.onSoleChanged(normalizeSoleState(sole))).catch(() => {})
    }, 500)
  }

  stopSolePolling = (): void => {
    if (this.soleTimer) {
      clearInterval(this.soleTimer)
      this.soleTimer = null
    }
  }

  // ---- Pitch demo (phase 2) ----

  setSolePitchSettings = async (patch: Partial<SolePitchState['settings']>): Promise<void> => {
    const settings = { ...this.state.solePitch.settings, ...patch }
    settings.maxPitchDegrees = Math.min(Math.max(settings.maxPitchDegrees, 0), 35)
    try {
      await this.client().request('solePitch.set', { settings })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  startSolePitchPolling = (): void => {
    if (this.solePitchTimer || !this.supports('solePitch.get')) return
    this.solePitchTimer = setInterval(() => {
      void this.client()
        .request('solePitch.get')
        .then((solePitch) => this.set({ solePitch: normalizeSolePitchState(solePitch) }))
        .catch(() => {})
    }, 500)
  }

  stopSolePitchPolling = (): void => {
    if (this.solePitchTimer) {
      clearInterval(this.solePitchTimer)
      this.solePitchTimer = null
    }
  }

  // ---- Studio lighting & video (phase 2) ----

  private onStudioChanged(studio: StudioState): void {
    const patch: Partial<MotionAppState> = { studio }
    const draft = this.state.studioDraft
    if (this.state.studioDraftRevision !== studio.revision) {
      const nextDraft = { ...draft }
      if (!deepEqual(draft.lighting, studio.settings.lighting)) nextDraft.lighting = studio.settings.lighting
      if (!deepEqual(draft.video, studio.settings.video)) {
        nextDraft.video = studio.settings.video
        patch.studioVideoPath = studio.settings.video.path
      }
      patch.studioDraft = nextDraft
      patch.studioDraftRevision = studio.revision
    }
    this.set(patch)
  }

  patchStudioLighting = (patch: Partial<StudioSettings['lighting']>): void => {
    this.set({ studioDraft: { ...this.state.studioDraft, lighting: { ...this.state.studioDraft.lighting, ...patch } } })
  }

  patchStudioVideo = (patch: Partial<StudioSettings['video']>): void => {
    this.set({ studioDraft: { ...this.state.studioDraft, video: { ...this.state.studioDraft.video, ...patch } } })
  }

  setStudioVideoPath = (path: string): void => {
    this.set({ studioVideoPath: path, studioDraft: { ...this.state.studioDraft, video: { ...this.state.studioDraft.video, path } } })
  }

  studioVideoError = (): string | null => {
    const next = { ...this.state.studio.settings, video: { ...this.state.studioDraft.video, path: this.state.studioVideoPath } }
    return validateStudio(next)
  }

  applyStudioLighting = async (): Promise<void> => {
    const next = { ...this.state.studio.settings, lighting: this.state.studioDraft.lighting }
    const error = validateStudio(next)
    if (error) {
      this.set({ actionError: error })
      return
    }
    try {
      await this.client().request('studio.set', { settings: next })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  disableStudioLighting = async (): Promise<void> => {
    const next = { ...this.state.studio.settings, lighting: { ...this.state.studio.settings.lighting, enabled: false } }
    this.set({ studioDraft: { ...this.state.studioDraft, lighting: { ...this.state.studioDraft.lighting, enabled: false } } })
    try {
      await this.client().request('studio.set', { settings: next })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  applyStudioVideo = async (): Promise<void> => {
    const next = { ...this.state.studio.settings, video: { ...this.state.studioDraft.video, path: this.state.studioVideoPath } }
    const error = validateStudio(next)
    if (error) {
      this.set({ actionError: error })
      return
    }
    try {
      await this.client().request('studio.set', { settings: next })
      this.set({ actionError: null })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  disableStudioVideo = async (): Promise<void> => {
    const next = { ...this.state.studio.settings, video: { ...this.state.studio.settings.video, enabled: false } }
    this.set({ studioDraft: { ...this.state.studioDraft, video: { ...this.state.studioDraft.video, enabled: false } } })
    try {
      await this.client().request('studio.set', { settings: next })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  resetStudioSettings = async (): Promise<void> => {
    try {
      await this.client().request('studio.reset')
      this.set({ studioDraftRevision: -1 })
    } catch (error) {
      this.reportActionError(error)
    }
  }

  private stopWatchers(): void {
    this.stopImportUiTimer()
    this.stopSolePolling()
    this.stopSolePitchPolling()
    if (this.dialogTimer) {
      clearInterval(this.dialogTimer)
      this.dialogTimer = null
    }
    if (this.dropTimer) {
      clearInterval(this.dropTimer)
      this.dropTimer = null
    }
    this.dropWatchCount = 0
    this.dropTargetActive = false
    this.dialogTarget = null
  }

  // ---- Lifecycle ----

  reset(): void {
    for (const unsub of this.unsubs) unsub()
    this.unsubs = []
    this.stopWatchers()
    if (this.volumeTimer) clearTimeout(this.volumeTimer)
    if (this.facialTimer) clearTimeout(this.facialTimer)
    this.volumeTimer = null
    this.facialTimer = null
    this.pendingVolume = null
    this.pendingFacial = null
    this.coverPending.clear()
    this.portraitInflight.clear()
    this.coverQueue = []
    this.coverWorkersRunning = 0
    this.coverGeneration += 1
    this.refreshCoverBustPending = false
    this.pendingImportOutcome = null
    this.bridge = null
    this.started = false
    this.state = initialState()
    for (const listener of this.listeners) listener()
  }
}

export const motionStore = new MotionStore()

export function useMotionStore<T>(selector: (state: MotionAppState) => T): T {
  return useSyncExternalStore(motionStore.subscribe, () => selector(motionStore.getState()))
}

export function useSupported(method: MotionMethodName): boolean {
  return useMotionStore((state) => state.capabilities.includes(method))
}
