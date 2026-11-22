import type {
  AudioEffectState,
  CameraSettings,
  ClockState,
  FacialWeights,
  PlayerState,
  SlotPerformanceTiming,
  StageFormation,
  StageSettings,
  TrackTiming,
} from './coreTypes'
import type {
  ExperimentSettings,
  GazeSettings,
  ImportRequest,
  PerformanceTiming,
  SolePitchSettings,
  SoleSettings,
  StudioSettings,
  VmdProbe,
  VmdTendency,
} from './phase2Types'

export const PLAY_MODES = ['播放一次', '单曲循环', '列表循环', '随机播放'] as const
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
  followSlotIndex: -1,
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

export const DEFAULT_STAGE_FORMATION: StageFormation = {
  active: false,
  available: false,
  verified: false,
  isGroupSong: false,
  trackPositionCount: 1,
  slotCount: 1,
  selectedSlotIndex: 0,
  revision: 1,
  actorEpoch: 1,
  notice: '',
  slots: [
    {
      slotIndex: 0,
      occupied: false,
      actorToken: 0,
      actorInstanceId: 0,
      character: '',
      shortName: '',
      displayName: '',
      isOverflow: false,
      hasTrackMotion: false,
      sourceSlotIndex: 0,
      placementDirty: false,
      hasActorBoneOverrides: false,
      physicsBackend: '',
      offset: { x: 0, y: 0, z: 0, yawDegrees: 0 },
      facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
    },
  ],
}

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

export function defaultSlotPerformanceTiming(): SlotPerformanceTiming {
  return {
    motion: { ...DEFAULT_TRACK_TIMING },
    facial: { ...DEFAULT_TRACK_TIMING },
  }
}

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
