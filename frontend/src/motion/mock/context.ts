import {
  DEFAULT_AUDIO_EFFECT_STATE,
  DEFAULT_CAMERA_SETTINGS,
  DEFAULT_CLOCK_STATE,
  DEFAULT_EXPERIMENT_SETTINGS,
  DEFAULT_FACIAL_WEIGHTS,
  DEFAULT_GAZE_SETTINGS,
  DEFAULT_PLAYER_STATE,
  DEFAULT_SOLE_PITCH_SETTINGS,
  DEFAULT_STAGE_SETTINGS,
  defaultSoleSettings,
  defaultStudioSettings,
  type ActorsState,
  type AudioEffectState,
  type BoneState,
  type CameraState,
  type ClockState,
  type DropPeek,
  type ExperimentsState,
  type FacialState,
  type GazeState,
  type LibraryEntry,
  type LoadedState,
  type MotionAppearance,
  type MotionEventName,
  type MotionEvents,
  type MotionMethodName,
  type MotionStateSnapshot,
  type PerformanceTiming,
  type PermissionState,
  type PlayerState,
  type PositionState,
  type SolePitchState,
  type SoleState,
  type StageFormation,
  type StageNode,
  type StageState,
  type StudioState,
} from '../protocol'
import type { MockCapabilityProfile } from './capabilities'
import {
  MOCK_AUDIO_EFFECT_PRESETS,
  MOCK_COVER_PNG,
  buildMockFormation,
  seedEntries,
  seedStageNodes,
} from './seed'

export interface MockRuntimeState {
  capabilityProfile: MockCapabilityProfile
  appearanceState: MotionAppearance
  closed: boolean
  sessionId: number
  seq: number
  tickTimer: ReturnType<typeof setInterval> | null
  refreshTimer: ReturnType<typeof setTimeout> | null
  importTimers: Array<ReturnType<typeof setTimeout>>
  scrubbing: boolean
  permission: PermissionState
  player: PlayerState
  entries: LibraryEntry[]
  timings: Map<string, PerformanceTiming>
  libraryRevision: number
  libraryNotice: string
  refreshing: boolean
  loaded: LoadedState
  playlist: { mode: number; notice: string }
  actors: ActorsState
  camera: CameraState
  stage: StageState
  stageNodes: StageNode[]
  facial: FacialState
  position: PositionState
  formation: StageFormation
  physicsBackend: number
  volumeSaved: number
  bone: BoneState
  gaze: GazeState
  experiments: ExperimentsState
  sole: SoleState
  solePitch: SolePitchState
  studio: StudioState
  clock: ClockState
  audioEffect: AudioEffectState
  coverByKey: Map<string, string>
  nextDialogId: number
  dialogResults: Map<number, { paths: string[] }>
  dropTargetEnabled: boolean
  pendingDropFiles: string[]
  dropPeekState: DropPeek
}

export function createInitialMockState(capabilityProfile: MockCapabilityProfile): MockRuntimeState {
  const actors: ActorsState = {
    epoch: 1,
    actors: [
      { token: 101, label: '花海咲季 (hski) · 1', character: 'hski', displayName: '花海咲季', shortName: '咲季', selected: true },
      { token: 102, label: '月村手毬 (ttmr) · 2', character: 'ttmr', displayName: '月村手毬', shortName: '手毬', selected: false },
      { token: 103, label: '藤田琴音 (fktn) · 3', character: 'fktn', displayName: '藤田琴音', shortName: '琴音', selected: false },
    ],
    targetLabel: '花海咲季 (hski) · 1',
    boundLabel: '花海咲季 (hski) · 1',
  }
  return {
    capabilityProfile,
    appearanceState: { theme: 0, uiScale: 1, dpiScale: 1, scaleLimited: false },
    closed: false,
    sessionId: 1,
    seq: 0,
    tickTimer: null,
    refreshTimer: null,
    importTimers: [],
    scrubbing: false,
    permission: { injectionAllowed: true, notice: '' },
    player: { ...DEFAULT_PLAYER_STATE },
    entries: seedEntries(),
    timings: new Map(),
    libraryRevision: 1,
    libraryNotice: '',
    refreshing: false,
    loaded: { motionKey: '', generation: 0, injectionSession: 1, sourceMissing: false, sourceChanged: false },
    playlist: { mode: 1, notice: '' },
    actors,
    camera: {
      settings: { ...DEFAULT_CAMERA_SETTINGS },
      available: true,
      owned: false,
      hasTrack: true,
      error: '',
      followTargetError: '',
      notice: '',
    },
    stage: { settings: { ...DEFAULT_STAGE_SETTINGS }, sceneName: '摄影棚 A', statusNotice: '就绪' },
    stageNodes: seedStageNodes(),
    facial: { weights: { ...DEFAULT_FACIAL_WEIGHTS }, notice: '' },
    position: { offset: { x: 0, y: 0, z: 0 }, notice: '' },
    formation: buildMockFormation(actors.actors, actors.epoch, undefined, capabilityProfile === 'full'),
    physicsBackend: 2,
    volumeSaved: 1,
    bone: {
      masterEnabled: true,
      overrides: [
        {
          humanoidId: 10,
          boneName: 'Head',
          enabled: true,
          spaceWorld: false,
          eulerDeg: { x: 2.5, y: -4.0, z: 0 },
          isDynamic: false,
        },
      ],
      notice: '',
    },
    gaze: {
      settings: { ...DEFAULT_GAZE_SETTINGS },
      runtimeAvailable: true,
      vrifyQueryAvailable: false,
      vrifyLiveGazeActive: false,
      notice: '',
    },
    experiments: {
      settings: { ...DEFAULT_EXPERIMENT_SETTINGS },
      mouthFitStatus: '已适配（合成预览）',
      eyeFitStatus: '已适配（合成预览）',
      fittedFaceLabel: '咲季 · Standard',
      notice: '',
    },
    sole: {
      settings: defaultSoleSettings(),
      snapshot: { available: true, calibrated: false, clipTime: 0, feet: [] },
      generation: 1,
      notice: '',
    },
    solePitch: {
      settings: { ...DEFAULT_SOLE_PITCH_SETTINGS },
      clipTime: 0,
      feet: [
        {
          status: '观测中',
          applied: false,
          setterReadbackMatches: true,
          measuredAfterValid: true,
          measuredAfterMinimumDistanceM: 0.0012,
          solution: { valid: true, pitchDegrees: 3.4, usedMeshEnvelope: false, beforePenetrationM: 0.0021 },
        },
        {
          status: '观测中',
          applied: false,
          setterReadbackMatches: true,
          measuredAfterValid: true,
          measuredAfterMinimumDistanceM: 0.0005,
          solution: { valid: true, pitchDegrees: 1.2, usedMeshEnvelope: false, beforePenetrationM: 0.0008 },
        },
      ],
      notice: '',
    },
    studio: {
      settings: defaultStudioSettings(),
      revision: 1,
      lightingNotice: '',
      videoNotice: '',
    },
    clock: { ...DEFAULT_CLOCK_STATE },
    audioEffect: {
      ...DEFAULT_AUDIO_EFFECT_STATE,
      available: true,
      presets: MOCK_AUDIO_EFFECT_PRESETS.map((item) => ({ ...item })),
    },
    coverByKey: new Map([['hikari-no-saki', MOCK_COVER_PNG]]),
    nextDialogId: 1,
    dialogResults: new Map(),
    dropTargetEnabled: false,
    pendingDropFiles: [],
    dropPeekState: {
      visual: 'none',
      fileCount: 0,
      accepted: 0,
      hostElevated: false,
      hostBlocked: false,
    },
  }
}

export interface MockHandlerContext {
  state: MockRuntimeState
  supportedMethods: () => MotionMethodName[]
  emit: <E extends MotionEventName>(event: E, payload: MotionEvents[E]) => void
  emitPlayer: () => void
  emitLibrary: () => void
  emitFormation: () => void
  cloneFormation: () => StageFormation
  snapshot: () => MotionStateSnapshot
  startTick: () => void
  loadEntry: (entry: LibraryEntry, autoplay: boolean) => void
  cycle: (direction: number | 'random') => void
  requireClip: () => void
  requireAllowed: () => void
  requireCapability: (method: MotionMethodName) => void
}

export type MockHandlerMap = Partial<Record<MotionMethodName, (p: Record<string, unknown>) => unknown>>
