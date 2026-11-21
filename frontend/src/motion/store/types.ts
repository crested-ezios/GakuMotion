import {
  DEFAULT_AUDIO_EFFECT_STATE,
  DEFAULT_CAMERA_SETTINGS,
  DEFAULT_CLOCK_STATE,
  DEFAULT_EXPERIMENT_SETTINGS,
  DEFAULT_FACIAL_WEIGHTS,
  DEFAULT_GAZE_SETTINGS,
  DEFAULT_PLAYER_STATE,
  DEFAULT_SOLE_PITCH_SETTINGS,
  DEFAULT_STAGE_FORMATION,
  DEFAULT_STAGE_SETTINGS,
  defaultPerformanceTiming,
  defaultSoleSettings,
  defaultStudioSettings,
  type ActorsState,
  type AudioEffectState,
  type AudioProbeInfo,
  type BoneState,
  type CameraSettings,
  type CameraState,
  type ClockState,
  type EditEntryInfo,
  type ExperimentsState,
  type FacialWeights,
  type GazeState,
  type ImportProgress,
  type LibraryState,
  type LoadedState,
  type MotionAppearance,
  type PerformanceTiming,
  type PermissionState,
  type PlayerState,
  type ScreenshotCapabilities,
  type SolePitchState,
  type SoleSettings,
  type SoleState,
  type StageFormation,
  type StageNode,
  type StageState,
  type StudioSettings,
  type StudioState,
  type TelemetrySnapshot,
  type Vector3,
  type VmdProbe,
} from '../protocol'

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

export function emptyCoverEditor(): CoverEditorState {
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

export function emptyImportPosition(): ImportPositionFormState {
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

export function emptyImportForm(): ImportFormState {
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

export function emptyImportUi(): ImportUiState {
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

export function cloneDefaultFormation(): StageFormation {
  return {
    ...DEFAULT_STAGE_FORMATION,
    slots: DEFAULT_STAGE_FORMATION.slots.map((s) => ({
      ...s,
      offset: { ...s.offset },
      facialWeights: { ...s.facialWeights },
    })),
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

export function initialState(): MotionAppState {
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
