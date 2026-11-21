// DEV/PREVIEW mock of the GakuMotion web-panel bridge (`gakumotion.web-panel/v1`).
// Supports two capability profiles:
//   - 'full' (default): simulates all Phase 1 + Phase 2 native methods for rich
//     standalone preview (?preview=1) and interactive testing.
//   - 'phase1': exposes only core Phase 1 methods so capability-gating and
//     "待迁移" degradation UI can be verified directly.

import {
  BridgeError,
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
  boneNameByHid,
  defaultPerformanceTiming,
  defaultSoleSettings,
  defaultStudioSettings,
  symmetricalHid,
  type ActorPortraitResult,
  type AudioEffectState,
  type AudioProbeInfo,
  type BoneState,
  type CameraSettings,
  type ClockRunMode,
  type ClockSource,
  type ClockState,
  type DropPeek,
  type EditEntryInfo,
  type ExperimentsState,
  type GazeState,
  type ImportRequest,
  type LibraryEntry,
  type LibraryPositionEntry,
  type MotionBridgeClient,
  type MotionEventName,
  type MotionEvents,
  type MotionMethodName,
  type MotionMethods,
  type MotionStateSnapshot,
  type PerformanceTiming,
  type PlayerState,
  type RequestOptions,
  type SceneActor,
  type SolePitchState,
  type SoleState,
  type StageFormation,
  type StageNode,
  type StageSlotOffset,
  type StudioSettings,
  type StudioState,
  type TelemetrySnapshot,
  type VmdProbe,
} from './protocol'
import { steppedFrame } from './time'
import { clamp } from './utils'
import { classifyImportFiles, validateSole, validateStudio } from './validation'

export type MockCapabilityProfile = 'full' | 'phase1'

const PHASE1_METHODS: MotionMethodName[] = [
  'bridge.hello',
  'bridge.ping',
  'motion.hello',
  'state.get',
  'player.play',
  'player.pause',
  'player.resume',
  'player.stop',
  'player.cycle',
  'player.seek',
  'player.seekFrame',
  'player.stepFrame',
  'player.scrubBegin',
  'player.scrubUpdate',
  'player.scrubEnd',
  'player.setSpeed',
  'player.setVolume',
  'player.saveVolume',
  'player.reload',
  'playlist.setMode',
  'library.refresh',
  'library.select',
  'library.delete',
  'library.openFolder',
  'actors.selectTarget',
  'facial.setWeights',
  'facial.save',
  'position.set',
  'position.save',
  'stage.set',
  'settings.setPhysics',
  'shell.setTextInputActive',
  'appearance.get',
]

const PHASE2_METHODS: MotionMethodName[] = [
  'camera.set',
  'camera.save',
  'library.import',
  'library.probeFiles',
  'library.startEdit',
  'library.probeAudio',
  'library.getCover',
  'library.setCover',
  'library.readImagePreview',
  'screenshot.capabilities',
  'screenshot.capture',
  'clock.setRunMode',
  'clock.configure',
  'audioEffect.set',
  'dialog.pickFile',
  'dialog.poll',
  'drop.setTarget',
  'drop.poll',
  'drop.peek',
  'actors.getPortrait',
  'formation.selectSlot',
  'formation.swapSlots',
  'formation.setSlotOffset',
  'formation.resetSlotOffset',
  'formation.saveSlotOffset',
  'formation.setSlotSource',
  'bone.get',
  'bone.set',
  'bone.reset',
  'bone.resetAll',
  'bone.setMaster',
  'bone.reload',
  'bone.saveToMotion',
  'bone.saveToGlobal',
  'gaze.get',
  'gaze.set',
  'stage.scan',
  'stage.toggleNode',
  'stage.setLoadReplacement',
  'experiments.get',
  'experiments.set',
  'telemetry.get',
  'sole.get',
  'sole.set',
  'sole.save',
  'sole.load',
  'solePitch.get',
  'solePitch.set',
  'studio.get',
  'studio.set',
  'studio.reset',
]

const SUPPORTED_EVENTS: MotionEventName[] = [
  'player.stateChanged',
  'permission.changed',
  'library.changed',
  'loaded.changed',
  'playlist.changed',
  'actors.changed',
  'formation.changed',
  'camera.changed',
  'stage.changed',
  'stage.nodesChanged',
  'facial.changed',
  'position.changed',
  'settings.changed',
  'import.progress',
  'appearance.changed',
  'bridge.closed',
  'bone.changed',
  'gaze.changed',
  'experiments.changed',
  'sole.changed',
  'solePitch.changed',
  'studio.changed',
  'clock.changed',
  'audioEffect.changed',
]

const MOCK_COVER_PNG =
  'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='

const MOCK_AUDIO_EFFECT_PRESETS = [
  { id: 'flat', name: '原声', category: 'flat', description: '直通输出（保留原始母带音色）' },
  { id: 'concert_arena', name: '演唱会现场', category: 'live', description: '万人巨蛋空间混响与氛围临场感' },
  { id: 'livehouse', name: 'LiveHouse现场', category: 'live', description: '近距离中小场地节奏冲击与清晰人声' },
  { id: 'mega_bass', name: '极重低音', category: 'bass', description: 'Sub-bass 与低频弹性强化' },
  { id: 'vocal_clear', name: '清澈人声', category: 'vocal', description: '突出偶像人声泛音与齿音透明度' },
  { id: 'symphony_hall', name: '音乐厅', category: 'space', description: '古典音乐厅自然早期反射' },
  { id: 'surround_3d', name: '3D环绕', category: 'space', description: 'Haas + 立体声宽度增强' },
  { id: 'pure_acg', name: '纯净ACG', category: 'eq', description: '二次元流行乐高频通透调音' },
]

const MOCK_KNOWN_IDOL_CODES = new Set([
  'hski', 'ttmr', 'fktn', 'amao', 'kllj', 'ssmk', 'hume', 'kcna',
  'jsna', 'shro', 'hrnm', 'atbm', 'hmsz', 'hkne', 'krnh', 'nasr',
])

function seedEntries(): LibraryEntry[] {
  return [
    {
      id: 'hikari-no-saki',
      title: '輝きの先へ',
      subtitle: '初星学園, 花海咲季',
      directory: 'gakumas-local/motions/hikari-no-saki',
      durationSeconds: 212,
      totalFrames: 12720,
      hasMotion: true,
      hasFacial: true,
      hasCamera: true,
      hasAudio: true,
      audioFile: 'song.flac',
      hasCover: true,
      isGroup: false,
      positionCount: 1,
      positions: [],
    },
    {
      id: 'starlight-step',
      title: 'Starlight Step',
      subtitle: '初星学園, 篠澤広, 姫崎莉波',
      directory: 'gakumas-local/motions/starlight-step',
      durationSeconds: 185.5,
      totalFrames: 11130,
      hasMotion: true,
      hasFacial: false,
      hasCamera: true,
      hasAudio: true,
      isGroup: true,
      positionCount: 3,
      positions: [
        { slot: 1, x: 0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: true },
        { slot: 2, x: -0.6, y: 0, z: -0.2, yawDegrees: 10, hasMotion: true, hasFacial: false },
        { slot: 3, x: 0.6, y: 0, z: -0.2, yawDegrees: -10, hasMotion: true, hasFacial: false },
      ],
    },
    {
      id: 'aoi-haru',
      title: '青い春',
      subtitle: '初星学園, 月村手毬',
      directory: 'gakumas-local/motions/aoi-haru',
      durationSeconds: 240,
      totalFrames: 14400,
      hasMotion: true,
      hasFacial: true,
      hasCamera: false,
      hasAudio: true,
      isGroup: false,
      positionCount: 1,
      positions: [],
    },
    {
      id: 'practice-room',
      title: '练习室镜像',
      directory: 'gakumas-local/motions/practice-room',
      durationSeconds: 96,
      totalFrames: 5760,
      hasMotion: true,
      hasFacial: false,
      hasCamera: false,
      hasAudio: false,
      isGroup: false,
      positionCount: 1,
      positions: [],
    },
    {
      id: 'encore-demo',
      title: 'Encore（演示）',
      directory: 'gakumas-local/motions/encore-demo',
      durationSeconds: 30,
      totalFrames: 1800,
      hasMotion: true,
      hasFacial: true,
      hasCamera: true,
      hasAudio: false,
      isGroup: false,
      positionCount: 1,
      positions: [],
    },
  ]
}

function buildMockFormation(
  actors: SceneActor[],
  actorEpoch: number,
  loadedEntry: LibraryEntry | undefined,
  available: boolean,
  revision = 1,
  selectedSlotIndex = 0,
): StageFormation {
  const isGroupSong = Boolean(loadedEntry?.isGroup && (loadedEntry.positionCount ?? 1) >= 2)
  const trackPositionCount = isGroupSong ? Math.max(2, loadedEntry!.positionCount ?? 1) : 1
  const slotCount = Math.max(1, actors.length, trackPositionCount)
  const slots: StageFormation['slots'] = []
  for (let i = 0; i < slotCount; i += 1) {
    const actor = actors[i]
    const isOverflow = Boolean(loadedEntry) && i >= trackPositionCount
    const hasTrackMotion = Boolean(loadedEntry) && !isOverflow
    let offset: StageSlotOffset = { x: 0, y: 0, z: 0, yawDegrees: 0 }
    if (i > 0) {
      const posMeta = isGroupSong && i < trackPositionCount
        ? loadedEntry?.positions?.find((p) => p.slot === i + 1)
        : undefined
      if (posMeta) {
        offset = { x: posMeta.x, y: posMeta.y, z: posMeta.z, yawDegrees: posMeta.yawDegrees }
      } else {
        const prev = slots[i - 1]?.offset ?? { x: 0, y: 0, z: 0, yawDegrees: 0 }
        offset = { x: prev.x + 0.5, y: prev.y, z: prev.z, yawDegrees: 0 }
      }
    }
    slots.push({
      slotIndex: i,
      occupied: Boolean(actor),
      actorToken: actor?.token ?? 0,
      actorInstanceId: actor ? 1000 + i : 0,
      character: actor?.character ?? '',
      shortName: actor?.shortName ?? '',
      displayName: actor?.displayName ?? '',
      isOverflow,
      hasTrackMotion,
      sourceSlotIndex: isOverflow ? 0 : i,
      placementDirty: false,
      hasActorBoneOverrides: false,
      physicsBackend: actor ? '原生' : '',
      offset,
      facialWeights: { ...DEFAULT_FACIAL_WEIGHTS },
    })
  }
  return {
    active: actors.length >= 2,
    available,
    verified: available,
    isGroupSong,
    trackPositionCount,
    slotCount,
    selectedSlotIndex: Math.min(Math.max(0, selectedSlotIndex), slotCount - 1),
    revision,
    actorEpoch,
    notice: '',
    slots,
  }
}

function seedStageNodes(): StageNode[] {
  return [
    {
      index: 0,
      name: 'StudioStageRoot',
      displayName: 'StudioStageRoot',
      parentIndex: -1,
      depth: 0,
      hasChildren: true,
      currentlyActive: true,
      isColliderLocked: false,
      isIdentifiedBackground: false,
      isLightingRoot: false,
      isLightingNode: false,
    },
    {
      index: 1,
      name: 'StageLights',
      displayName: 'StageLights（舞台灯光组）',
      parentIndex: 0,
      depth: 1,
      hasChildren: true,
      currentlyActive: true,
      isColliderLocked: false,
      isIdentifiedBackground: false,
      isLightingRoot: true,
      isLightingNode: true,
    },
    {
      index: 2,
      name: 'BackdropWall',
      displayName: 'BackdropWall（背景幕布）',
      parentIndex: 0,
      depth: 1,
      hasChildren: false,
      currentlyActive: true,
      isColliderLocked: false,
      isIdentifiedBackground: true,
      isLightingRoot: false,
      isLightingNode: false,
    },
    {
      index: 3,
      name: 'StageFloorCollider',
      displayName: 'StageFloorCollider（地面碰撞）',
      parentIndex: 0,
      depth: 1,
      hasChildren: false,
      currentlyActive: true,
      isColliderLocked: true,
      isIdentifiedBackground: false,
      isLightingRoot: false,
      isLightingNode: false,
    },
    {
      index: 4,
      name: 'SpotDecalProps',
      displayName: 'SpotDecalProps（舞台装饰）',
      parentIndex: 0,
      depth: 1,
      hasChildren: false,
      currentlyActive: true,
      isColliderLocked: false,
      isIdentifiedBackground: false,
      isLightingRoot: false,
      isLightingNode: false,
    },
  ]
}

function probeFromPath(filePath: string): VmdProbe {
  const lower = filePath.toLowerCase()
  if (lower.endsWith('.gkcamera')) {
    return {
      path: filePath,
      valid: true,
      isGkCamera: true,
      tendency: 'cameraOnly',
      boneFrames: 0,
      bodyBoneFrames: 0,
      eyeBoneFrames: 0,
      morphFrames: 0,
      cameraFrames: 4200,
    }
  }
  if (lower.includes('cam') || filePath.includes('镜头') || filePath.includes('运镜')) {
    return {
      path: filePath,
      valid: true,
      isGkCamera: false,
      tendency: 'cameraOnly',
      boneFrames: 0,
      bodyBoneFrames: 0,
      eyeBoneFrames: 0,
      morphFrames: 0,
      cameraFrames: 3600,
    }
  }
  if (lower.includes('face') || lower.includes('facial') || lower.includes('morph') || filePath.includes('表情')) {
    return {
      path: filePath,
      valid: true,
      isGkCamera: false,
      tendency: 'facialOnly',
      boneFrames: 120,
      bodyBoneFrames: 0,
      eyeBoneFrames: 120,
      morphFrames: 1850,
      cameraFrames: 0,
    }
  }
  return {
    path: filePath,
    valid: true,
    isGkCamera: false,
    tendency: 'motionAndFacial',
    boneFrames: 11200,
    bodyBoneFrames: 10900,
    eyeBoneFrames: 300,
    morphFrames: 1400,
    cameraFrames: 0,
  }
}

export class MotionMockBridgeClient implements MotionBridgeClient {
  readonly kind = 'mock' as const
  private capabilityProfile: MockCapabilityProfile
  private readonly handlers = new Map<MotionEventName, Set<(payload: never) => void>>()
  private appearanceState = { theme: 0, uiScale: 1, dpiScale: 1, scaleLimited: false }
  private closed = false
  private sessionId = 1
  private seq = 0
  private tickTimer: ReturnType<typeof setInterval> | null = null
  private refreshTimer: ReturnType<typeof setTimeout> | null = null
  private importTimers: Array<ReturnType<typeof setTimeout>> = []
  private scrubbing = false

  private permission = { injectionAllowed: true, notice: '' }
  private player: PlayerState = { ...DEFAULT_PLAYER_STATE }
  private entries = seedEntries()
  private timings = new Map<string, PerformanceTiming>()
  private libraryRevision = 1
  private libraryNotice = ''
  private refreshing = false
  private loaded = { motionKey: '', generation: 0, injectionSession: 1, sourceMissing: false, sourceChanged: false }
  private playlist = { mode: 1, notice: '' }
  private actors = {
    epoch: 1,
    actors: [
      { token: 101, label: '花海咲季 (hski) · 1', character: 'hski', displayName: '花海咲季', shortName: '咲季', selected: true },
      { token: 102, label: '月村手毬 (ttmr) · 2', character: 'ttmr', displayName: '月村手毬', shortName: '手毬', selected: false },
      { token: 103, label: '藤田琴音 (fktn) · 3', character: 'fktn', displayName: '藤田琴音', shortName: '琴音', selected: false },
    ],
    targetLabel: '花海咲季 (hski) · 1',
    boundLabel: '花海咲季 (hski) · 1',
  }
  private camera = {
    settings: { ...DEFAULT_CAMERA_SETTINGS },
    available: true,
    owned: false,
    hasTrack: true,
    error: '',
    followTargetError: '',
    notice: '',
  }
  private stage = { settings: { ...DEFAULT_STAGE_SETTINGS }, sceneName: '摄影棚 A', statusNotice: '就绪' }
  private stageNodes: StageNode[] = seedStageNodes()
  private facial = { weights: { ...DEFAULT_FACIAL_WEIGHTS }, notice: '' }
  private position = { offset: { x: 0, y: 0, z: 0 }, notice: '' }
  private formation: StageFormation = { ...DEFAULT_STAGE_FORMATION }
  private physicsBackend = 2
  private volumeSaved = 1

  private bone: BoneState = {
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
  }
  private gaze: GazeState = {
    settings: { ...DEFAULT_GAZE_SETTINGS },
    runtimeAvailable: true,
    vrifyQueryAvailable: false,
    vrifyLiveGazeActive: false,
    notice: '',
  }
  private experiments: ExperimentsState = {
    settings: { ...DEFAULT_EXPERIMENT_SETTINGS },
    mouthFitStatus: '已适配（合成预览）',
    eyeFitStatus: '已适配（合成预览）',
    fittedFaceLabel: '咲季 · Standard',
    notice: '',
  }
  private sole: SoleState = {
    settings: defaultSoleSettings(),
    snapshot: { available: true, calibrated: false, clipTime: 0, feet: [] },
    generation: 1,
    notice: '',
  }
  private solePitch: SolePitchState = {
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
  }
  private studio: StudioState = {
    settings: defaultStudioSettings(),
    revision: 1,
    lightingNotice: '',
    videoNotice: '',
  }
  private clock: ClockState = { ...DEFAULT_CLOCK_STATE }
  private audioEffect: AudioEffectState = {
    ...DEFAULT_AUDIO_EFFECT_STATE,
    available: true,
    presets: MOCK_AUDIO_EFFECT_PRESETS.map((item) => ({ ...item })),
  }
  private coverByKey = new Map<string, string>([['hikari-no-saki', MOCK_COVER_PNG]])

  private nextDialogId = 1
  private dialogResults = new Map<number, { paths: string[] }>()
  private dropTargetEnabled = false
  private pendingDropFiles: string[] = []
  private dropPeekState: DropPeek = {
    visual: 'none',
    fileCount: 0,
    accepted: 0,
    hostElevated: false,
    hostBlocked: false,
  }

  constructor(options?: { capabilityProfile?: MockCapabilityProfile }) {
    this.capabilityProfile = options?.capabilityProfile ?? 'full'
    this.formation = buildMockFormation(
      this.actors.actors,
      this.actors.epoch,
      undefined,
      this.capabilityProfile === 'full',
    )
  }

  getCapabilityProfile(): MockCapabilityProfile {
    return this.capabilityProfile
  }

  setCapabilityProfile(profile: MockCapabilityProfile): void {
    this.capabilityProfile = profile
    this.formation = {
      ...this.formation,
      available: profile === 'full',
      verified: profile === 'full',
    }
  }

  simulateFileDrop(paths: string[]): void {
    this.pendingDropFiles = [...paths]
    this.dropPeekState = {
      visual: 'none',
      fileCount: 0,
      accepted: 0,
      hostElevated: false,
      hostBlocked: false,
    }
  }

  private supportedMethods(): MotionMethodName[] {
    return this.capabilityProfile === 'full' ? [...PHASE1_METHODS, ...PHASE2_METHODS] : [...PHASE1_METHODS]
  }

  private emit<E extends MotionEventName>(event: E, payload: MotionEvents[E]): void {
    this.seq += 1
    const set = this.handlers.get(event)
    if (set) for (const handler of set) (handler as (p: unknown) => void)(payload)
  }

  push<E extends MotionEventName>(event: E, payload: MotionEvents[E]): void {
    const obj = payload as unknown as Record<string, unknown>
    if (event === 'library.changed' && obj) {
      if (typeof obj.revision === 'number') this.libraryRevision = obj.revision
      if (Array.isArray(obj.entries)) {
        this.entries = (obj.entries as LibraryEntry[]).map((entry) => ({
          ...entry,
          isGroup: Boolean(entry.isGroup),
          positionCount: entry.positionCount ?? (entry.isGroup ? 2 : 1),
          positions: Array.isArray(entry.positions) ? entry.positions.map((pos) => ({ ...pos })) : [],
        }))
      }
      if (typeof obj.notice === 'string') this.libraryNotice = obj.notice
      if (typeof obj.refreshing === 'boolean') this.refreshing = obj.refreshing
    } else if (event === 'actors.changed' && obj) {
      this.actors = {
        ...this.actors,
        ...(obj as Partial<typeof this.actors>),
        actors: Array.isArray(obj.actors)
          ? (obj.actors as SceneActor[]).map((actor) => ({ ...actor }))
          : this.actors.actors,
      }
    } else if (event === 'formation.changed' && obj) {
      const f = obj as unknown as Partial<StageFormation>
      const rawSlots = Array.isArray(f.slots) ? f.slots : this.formation.slots
      const slots = rawSlots.map((slot, idx) => ({
        slotIndex: slot.slotIndex ?? idx,
        occupied: Boolean(slot.occupied),
        actorToken: slot.actorToken ?? 0,
        actorInstanceId: slot.actorInstanceId ?? 0,
        character: slot.character ?? '',
        shortName: slot.shortName ?? '',
        displayName: slot.displayName ?? '',
        isOverflow: Boolean(slot.isOverflow),
        hasTrackMotion: Boolean(slot.hasTrackMotion),
        sourceSlotIndex: slot.sourceSlotIndex ?? idx,
        placementDirty: Boolean(slot.placementDirty),
        hasActorBoneOverrides: Boolean(slot.hasActorBoneOverrides),
        physicsBackend: slot.physicsBackend ?? '',
        offset: slot.offset ? { ...slot.offset } : { x: 0, y: 0, z: 0, yawDegrees: 0 },
        facialWeights: slot.facialWeights ? { ...slot.facialWeights } : { ...DEFAULT_FACIAL_WEIGHTS },
      }))
      this.formation = {
        ...this.formation,
        ...f,
        slots,
      }
    } else if (event === 'camera.changed' && obj) {
      const c = obj as Partial<typeof this.camera>
      this.camera = {
        ...this.camera,
        ...c,
        settings: c.settings ? { ...this.camera.settings, ...c.settings } : this.camera.settings,
      }
    } else if (event === 'loaded.changed' && obj) {
      this.loaded = { ...this.loaded, ...(obj as Partial<typeof this.loaded>) }
    }
    this.emit(event, payload)
  }

  private emitPlayer(): void {
    this.emit('player.stateChanged', { ...this.player })
  }

  private emitLibrary(): void {
    this.emit('library.changed', {
      revision: this.libraryRevision,
      entries: this.entries.map((entry) => ({
        ...entry,
        positions: entry.positions.map((pos) => ({ ...pos })),
      })),
      refreshing: this.refreshing,
      notice: this.libraryNotice,
    })
  }

  private cloneFormation(): StageFormation {
    return {
      ...this.formation,
      slots: this.formation.slots.map((slot) => ({
        ...slot,
        offset: { ...slot.offset },
        facialWeights: { ...slot.facialWeights },
      })),
    }
  }

  private emitFormation(): void {
    this.emit('formation.changed', this.cloneFormation())
  }

  private startTick(): void {
    if (this.tickTimer) return
    this.tickTimer = setInterval(() => {
      if (this.closed || this.scrubbing || this.player.state !== 'playing') return
      const next = this.player.time + 0.1 * this.player.speed
      if (next >= this.player.duration) {
        if (this.player.playMode === 1) {
          this.player = { ...this.player, time: 0 }
        } else if (this.player.playMode === 2 || this.player.playMode === 3) {
          this.cycle(this.player.playMode === 3 ? 'random' : 1)
          return
        } else {
          this.player = { ...this.player, state: 'stopped', time: 0 }
        }
      } else {
        this.player = { ...this.player, time: next }
      }
      this.emitPlayer()
    }, 100)
  }

  private loadEntry(entry: LibraryEntry, autoplay: boolean): void {
    this.loaded = {
      motionKey: entry.id,
      generation: this.loaded.generation + 1,
      injectionSession: this.loaded.injectionSession,
      sourceMissing: false,
      sourceChanged: false,
    }
    this.player = {
      ...this.player,
      state: autoplay ? 'playing' : 'preview',
      title: entry.title,
      time: 0,
      duration: entry.durationSeconds,
      totalFrames: entry.totalFrames,
      fps: entry.totalFrames > 0 && entry.durationSeconds > 0 ? entry.totalFrames / entry.durationSeconds : 60,
    }
    this.camera = { ...this.camera, hasTrack: entry.hasCamera }
    this.formation = buildMockFormation(
      this.actors.actors,
      this.actors.epoch,
      entry,
      this.capabilityProfile === 'full',
      this.formation.revision + 1,
      this.formation.selectedSlotIndex,
    )
    this.emit('loaded.changed', { ...this.loaded })
    this.emit('formation.changed', this.cloneFormation())
    this.emit('camera.changed', { ...this.camera, settings: { ...this.camera.settings } })
    this.emitPlayer()
    if (autoplay) this.startTick()
  }

  private cycle(direction: number | 'random'): void {
    if (this.entries.length === 0) return
    const currentIndex = this.entries.findIndex((entry) => entry.id === this.loaded.motionKey)
    let nextIndex: number
    if (direction === 'random') nextIndex = Math.floor(Math.random() * this.entries.length)
    else nextIndex = (currentIndex + direction + this.entries.length) % this.entries.length
    this.loadEntry(this.entries[nextIndex], true)
  }

  private requireClip(): void {
    if (this.player.totalFrames <= 0 || this.player.duration <= 0) {
      throw new BridgeError('not_ready', '尚未加载曲目')
    }
  }

  private requireAllowed(): void {
    if (!this.permission.injectionAllowed) throw new BridgeError('not_allowed', 'GakuMotion 当前已禁用')
  }

  private requireCapability(method: MotionMethodName): void {
    if (!this.supportedMethods().includes(method)) {
      throw new BridgeError('not_implemented', `待迁移：${method} 尚未接入原生实现`)
    }
  }

  async request<M extends MotionMethodName>(
    method: M,
    params?: MotionMethods[M]['params'],
    _options?: RequestOptions,
  ): Promise<MotionMethods[M]['result']> {
    if (this.closed) throw new BridgeError('closed', 'The bridge is closed')
    await new Promise((resolve) => setTimeout(resolve, 5))
    const p = (params ?? {}) as Record<string, unknown>

    switch (method) {
      case 'bridge.hello':
        return { protocol: 1, sessionId: this.sessionId } as MotionMethods[M]['result']
      case 'bridge.ping':
        return { pong: true } as MotionMethods[M]['result']
      case 'motion.hello':
        return {
          protocol: 1,
          sessionId: this.sessionId,
          pluginVersion: '0.2.0-mock',
          supportedMethods: this.supportedMethods(),
          supportedEvents: [...SUPPORTED_EVENTS],
        } as MotionMethods[M]['result']
      case 'state.get':
        return this.snapshot() as MotionMethods[M]['result']
      case 'player.play':
        this.requireAllowed()
        this.requireClip()
        this.player = { ...this.player, state: 'playing' }
        this.startTick()
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.pause':
        this.requireAllowed()
        if (this.player.state === 'playing') this.player = { ...this.player, state: 'paused' }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.resume':
        this.requireAllowed()
        this.requireClip()
        if (this.player.state === 'paused' || this.player.state === 'preview') {
          this.player = { ...this.player, state: 'playing' }
          this.startTick()
        }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.stop':
        this.requireAllowed()
        this.player = { ...this.player, state: 'stopped', time: 0 }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.cycle': {
        this.requireAllowed()
        this.cycle(Number(p.delta) >= 0 ? 1 : -1)
        return {} as MotionMethods[M]['result']
      }
      case 'player.seek': {
        this.requireAllowed()
        this.requireClip()
        const seconds = Number(p.seconds)
        if (!Number.isFinite(seconds) || seconds < 0 || seconds > this.player.duration) {
          throw new BridgeError('invalid_params', '时间超出范围')
        }
        this.player = { ...this.player, time: seconds }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      }
      case 'player.seekFrame': {
        this.requireAllowed()
        this.requireClip()
        const frame = Number(p.frame)
        if (!Number.isInteger(frame) || frame < 0 || frame >= this.player.totalFrames) {
          throw new BridgeError('invalid_params', '帧号超出范围；末帧为总采样帧数减 1')
        }
        this.player = { ...this.player, state: 'paused', time: frame / this.player.fps }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      }
      case 'player.stepFrame': {
        this.requireAllowed()
        this.requireClip()
        const delta = Number(p.delta) >= 0 ? 1 : -1
        const frame = steppedFrame(this.player.time, this.player.fps, this.player.totalFrames, delta)
        this.player = { ...this.player, state: 'paused', time: frame / this.player.fps }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      }
      case 'player.scrubBegin':
        this.requireAllowed()
        this.requireClip()
        this.scrubbing = true
        this.player = { ...this.player, time: clamp(Number(p.seconds), 0, this.player.duration) }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.scrubUpdate':
        this.requireAllowed()
        this.player = { ...this.player, time: clamp(Number(p.seconds), 0, this.player.duration) }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.scrubEnd':
        this.requireAllowed()
        this.scrubbing = false
        this.player = { ...this.player, time: clamp(Number(p.seconds), 0, this.player.duration) }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.setSpeed':
        this.player = { ...this.player, speed: clamp(Number(p.speed), 0.1, 3) }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.setVolume':
        this.player = { ...this.player, volume: clamp(Number(p.volume), 0, 1) }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      case 'player.saveVolume':
        this.volumeSaved = this.player.volume
        return {} as MotionMethods[M]['result']
      case 'player.reload': {
        this.requireAllowed()
        const entry = this.entries.find((candidate) => candidate.id === this.loaded.motionKey)
        if (!entry) throw new BridgeError('not_ready', '没有已加载曲目')
        const time = this.player.time
        this.loadEntry(entry, false)
        this.player = { ...this.player, state: 'paused', time }
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      }
      case 'playlist.setMode': {
        const mode = Number(p.mode)
        this.playlist = { ...this.playlist, mode: mode >= 0 && mode <= 3 ? mode : 1 }
        this.player = { ...this.player, playMode: this.playlist.mode }
        this.emit('playlist.changed', { ...this.playlist })
        this.emitPlayer()
        return {} as MotionMethods[M]['result']
      }
      case 'library.refresh': {
        if (this.refreshing) throw new BridgeError('busy', '正在刷新曲库')
        this.refreshing = true
        this.emitLibrary()
        this.refreshTimer = setTimeout(() => {
          this.refreshing = false
          this.emitLibrary()
        }, 250)
        return { started: true } as MotionMethods[M]['result']
      }
      case 'library.select': {
        this.requireAllowed()
        if (Number(p.revision) !== this.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请刷新后重试')
        const entry = this.entries.find((candidate) => candidate.id === p.key)
        if (!entry) throw new BridgeError('invalid_params', '曲目不存在')
        this.loadEntry(entry, Boolean(p.autoplay))
        return { key: entry.id } as MotionMethods[M]['result']
      }
      case 'library.delete': {
        this.requireAllowed()
        if (this.refreshing) throw new BridgeError('busy', '曲库刷新中')
        if (Number(p.revision) !== this.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请取消后重新选择曲目')
        const index = this.entries.findIndex((candidate) => candidate.id === p.key)
        if (index < 0) throw new BridgeError('invalid_params', '曲目不存在')
        this.entries.splice(index, 1)
        this.libraryRevision += 1
        if (this.loaded.motionKey === p.key) {
          this.loaded = { ...this.loaded, motionKey: '', generation: this.loaded.generation + 1 }
          this.player = {
            ...DEFAULT_PLAYER_STATE,
            volume: this.player.volume,
            speed: this.player.speed,
            playMode: this.player.playMode,
          }
          this.emit('loaded.changed', { ...this.loaded })
          this.emitPlayer()
        }
        this.emitLibrary()
        return {} as MotionMethods[M]['result']
      }
      case 'library.openFolder':
        return {} as MotionMethods[M]['result']
      case 'actors.selectTarget': {
        this.requireAllowed()
        if (Number(p.epoch) !== this.actors.epoch) throw new BridgeError('stale_token', '角色名单已变化')
        const actor = this.actors.actors.find((candidate) => candidate.token === Number(p.token))
        if (!actor) throw new BridgeError('stale_token', '角色不存在')
        this.actors = {
          ...this.actors,
          actors: this.actors.actors.map((candidate) => ({ ...candidate, selected: candidate.token === actor.token })),
          targetLabel: actor.label,
        }
        this.emit('actors.changed', { ...this.actors, actors: this.actors.actors.map((a) => ({ ...a })) })
        return {} as MotionMethods[M]['result']
      }
      case 'actors.getPortrait': {
        this.requireCapability('actors.getPortrait')
        let normCode = ''
        if ('character' in p) {
          if (typeof p.character !== 'string' || !/^[a-zA-Z0-9_-]{1,32}$/.test(p.character)) {
            throw new BridgeError('invalid_params', 'character code is invalid')
          }
          normCode = p.character.toLowerCase()
        } else if ('token' in p) {
          const tok = p.token
          if (typeof tok !== 'number' || !Number.isInteger(tok) || tok <= 0) {
            throw new BridgeError('invalid_params', 'token must be a positive integer')
          }
          const actor = this.actors.actors.find((a) => a.token === tok)
          normCode = actor && /^[a-zA-Z0-9_-]{1,32}$/.test(actor.character) ? actor.character.toLowerCase() : ''
          if (!normCode) {
            return { ok: true, character: '', cached: false, dataUri: null } satisfies ActorPortraitResult as MotionMethods[M]['result']
          }
        } else {
          throw new BridgeError('invalid_params', 'character or token is required')
        }
        const hit = MOCK_KNOWN_IDOL_CODES.has(normCode)
        return {
          ok: true,
          character: normCode,
          cached: hit,
          dataUri: hit ? MOCK_COVER_PNG : null,
        } satisfies ActorPortraitResult as MotionMethods[M]['result']
      }
      case 'formation.selectSlot': {
        this.requireCapability('formation.selectSlot')
        const idx = p.slotIndex
        if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= this.formation.slots.length) {
          throw new BridgeError('invalid_params', 'slotIndex is out of range')
        }
        this.requireAllowed()
        this.formation = {
          ...this.formation,
          selectedSlotIndex: idx,
          revision: this.formation.revision + 1,
        }
        const selSlot = this.formation.slots[idx]
        if (selSlot) {
          this.facial = { ...this.facial, weights: { ...selSlot.facialWeights } }
          this.position = {
            ...this.position,
            offset: { x: selSlot.offset.x, y: selSlot.offset.y, z: selSlot.offset.z },
          }
          this.emit('facial.changed', { weights: { ...this.facial.weights }, notice: this.facial.notice })
          this.emit('position.changed', { offset: { ...this.position.offset }, notice: this.position.notice })
        }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'formation.swapSlots': {
        this.requireCapability('formation.swapSlots')
        const from = p.fromSlotIndex
        const to = p.toSlotIndex
        if (
          typeof from !== 'number' ||
          typeof to !== 'number' ||
          !Number.isInteger(from) ||
          !Number.isInteger(to) ||
          from < 0 ||
          to < 0 ||
          from === to ||
          from >= this.formation.slots.length ||
          to >= this.formation.slots.length
        ) {
          throw new BridgeError('invalid_params', 'fromSlotIndex and toSlotIndex must be distinct valid indices')
        }
        this.requireAllowed()
        const slots = this.formation.slots.map((s) => ({
          ...s,
          offset: { ...s.offset },
          facialWeights: { ...s.facialWeights },
        }))
        const a = slots[from]
        const b = slots[to]
        slots[from] = {
          ...a,
          occupied: b.occupied,
          actorToken: b.actorToken,
          actorInstanceId: b.actorInstanceId,
          character: b.character,
          shortName: b.shortName,
          displayName: b.displayName,
          facialWeights: { ...b.facialWeights },
          hasActorBoneOverrides: b.hasActorBoneOverrides,
          physicsBackend: b.physicsBackend,
        }
        slots[to] = {
          ...b,
          occupied: a.occupied,
          actorToken: a.actorToken,
          actorInstanceId: a.actorInstanceId,
          character: a.character,
          shortName: a.shortName,
          displayName: a.displayName,
          facialWeights: { ...a.facialWeights },
          hasActorBoneOverrides: a.hasActorBoneOverrides,
          physicsBackend: a.physicsBackend,
        }
        this.formation = {
          ...this.formation,
          slots,
          revision: this.formation.revision + 1,
        }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'formation.setSlotOffset': {
        this.requireCapability('formation.setSlotOffset')
        const idx = p.slotIndex
        const x = p.x
        const y = p.y
        const z = p.z
        const yaw = 'yawDegrees' in p ? p.yawDegrees : 0
        if (
          typeof idx !== 'number' ||
          !Number.isInteger(idx) ||
          idx < 0 ||
          idx >= this.formation.slots.length ||
          typeof x !== 'number' ||
          !Number.isFinite(x) ||
          x < -10 ||
          x > 10 ||
          typeof y !== 'number' ||
          !Number.isFinite(y) ||
          y < -10 ||
          y > 10 ||
          typeof z !== 'number' ||
          !Number.isFinite(z) ||
          z < -10 ||
          z > 10 ||
          typeof yaw !== 'number' ||
          !Number.isFinite(yaw) ||
          yaw < -360 ||
          yaw > 360
        ) {
          throw new BridgeError('invalid_params', 'Invalid formation slot offset parameters')
        }
        this.requireAllowed()
        const slots = this.formation.slots.map((s, i) =>
          i === idx
            ? {
                ...s,
                offset: { x, y, z, yawDegrees: yaw },
                placementDirty: true,
              }
            : s,
        )
        this.formation = { ...this.formation, slots, revision: this.formation.revision + 1 }
        if (idx === this.formation.selectedSlotIndex) {
          this.position = { ...this.position, offset: { x, y, z } }
          this.emit('position.changed', { ...this.position, offset: { ...this.position.offset } })
        }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'formation.resetSlotOffset': {
        this.requireCapability('formation.resetSlotOffset')
        const idx = p.slotIndex
        if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= this.formation.slots.length) {
          throw new BridgeError('invalid_params', 'slotIndex is out of range')
        }
        this.requireAllowed()
        const loadedEntry = this.entries.find((e) => e.id === this.loaded.motionKey)
        const defaults = buildMockFormation(this.actors.actors, this.actors.epoch, loadedEntry, true)
        const defOff = defaults.slots[idx]?.offset ?? { x: 0, y: 0, z: 0, yawDegrees: 0 }
        const slots = this.formation.slots.map((s, i) =>
          i === idx ? { ...s, offset: { ...defOff }, placementDirty: false } : s,
        )
        this.formation = { ...this.formation, slots, revision: this.formation.revision + 1 }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'formation.saveSlotOffset': {
        this.requireCapability('formation.saveSlotOffset')
        const idx = p.slotIndex
        if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= this.formation.slots.length) {
          throw new BridgeError('invalid_params', 'slotIndex is out of range')
        }
        this.requireAllowed()
        const slot = this.formation.slots[idx]
        if (idx >= 1 && this.formation.isGroupSong && idx < this.formation.trackPositionCount) {
          const loadedEntry = this.entries.find((e) => e.id === this.loaded.motionKey)
          if (loadedEntry) {
            const pos = loadedEntry.positions.find((entryPos) => entryPos.slot === idx + 1)
            if (pos && slot) {
              pos.x = slot.offset.x
              pos.y = slot.offset.y
              pos.z = slot.offset.z
              pos.yawDegrees = slot.offset.yawDegrees
            }
          }
        }
        const slots = this.formation.slots.map((s, i) => (i === idx ? { ...s, placementDirty: false } : s))
        this.formation = {
          ...this.formation,
          slots,
          revision: this.formation.revision + 1,
          notice: '已保存槽位初始位置',
        }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'formation.setSlotSource': {
        this.requireCapability('formation.setSlotSource')
        const idx = p.slotIndex
        const src = p.sourceSlotIndex
        const maxSource = Math.max(1, this.formation.trackPositionCount)
        if (
          typeof idx !== 'number' ||
          !Number.isInteger(idx) ||
          idx < 0 ||
          idx >= this.formation.slots.length ||
          typeof src !== 'number' ||
          !Number.isInteger(src) ||
          src < 0 ||
          src >= maxSource
        ) {
          throw new BridgeError('invalid_params', 'slotIndex or sourceSlotIndex is out of range')
        }
        this.requireAllowed()
        const slots = this.formation.slots.map((s, i) => (i === idx ? { ...s, sourceSlotIndex: src } : s))
        this.formation = { ...this.formation, slots, revision: this.formation.revision + 1 }
        this.emitFormation()
        return {} as MotionMethods[M]['result']
      }
      case 'facial.setWeights': {
        this.requireAllowed()
        const nextWeights = {
          overall: clamp(Number(p.overall), 0, 2),
          brow: clamp(Number(p.brow), 0, 2),
          eye: clamp(Number(p.eye), 0, 2),
          mouth: clamp(Number(p.mouth), 0, 2),
        }
        this.facial = {
          weights: nextWeights,
          notice: '',
        }
        const selIdx = this.formation.selectedSlotIndex
        if (selIdx >= 0 && selIdx < this.formation.slots.length) {
          const slots = this.formation.slots.map((s, i) =>
            i === selIdx ? { ...s, facialWeights: { ...nextWeights } } : s,
          )
          this.formation = { ...this.formation, slots, revision: this.formation.revision + 1 }
          this.emitFormation()
        }
        this.emit('facial.changed', { weights: { ...this.facial.weights }, notice: '' })
        return {} as MotionMethods[M]['result']
      }
      case 'facial.save':
        this.facial = { ...this.facial, notice: '已保存表情幅度（mock）' }
        this.emit('facial.changed', { weights: { ...this.facial.weights }, notice: this.facial.notice })
        return {} as MotionMethods[M]['result']
      case 'position.set': {
        this.requireAllowed()
        this.position = {
          offset: { x: clamp(Number(p.x), -10, 10), y: clamp(Number(p.y), -10, 10), z: clamp(Number(p.z), -10, 10) },
          notice: '',
        }
        this.emit('position.changed', { offset: { ...this.position.offset }, notice: '' })
        return {} as MotionMethods[M]['result']
      }
      case 'position.save':
        this.position = { ...this.position, notice: '已保存初始位置为默认（mock）' }
        this.emit('position.changed', { offset: { ...this.position.offset }, notice: this.position.notice })
        return {} as MotionMethods[M]['result']
      case 'camera.set': {
        this.requireAllowed()
        const nextCam = p.settings as CameraSettings
        if (
          nextCam &&
          'followSlotIndex' in nextCam &&
          (typeof nextCam.followSlotIndex !== 'number' ||
            !Number.isInteger(nextCam.followSlotIndex) ||
            nextCam.followSlotIndex < -1)
        ) {
          throw new BridgeError('invalid_params', 'followSlotIndex must be an integer >= -1')
        }
        this.camera = {
          ...this.camera,
          settings: { ...nextCam },
          owned: nextCam.enabled,
        }
        this.emit('camera.changed', { ...this.camera, settings: { ...this.camera.settings } })
        return {} as MotionMethods[M]['result']
      }
      case 'camera.save':
        this.camera = { ...this.camera, notice: '已保存镜头设置（mock）' }
        this.emit('camera.changed', { ...this.camera, settings: { ...this.camera.settings } })
        return {} as MotionMethods[M]['result']
      case 'stage.set': {
        this.requireAllowed()
        const settings = { ...(p.settings as MotionStateSnapshot['stage']['settings']) }
        this.stage = { ...this.stage, settings }
        this.emit('stage.changed', { ...this.stage, settings: { ...settings } })
        const shadowMode = settings.lightingEffectsEnabled ? 1 : 2
        if (this.camera.settings.actorShadowExperimentMode !== shadowMode) {
          this.camera = {
            ...this.camera,
            settings: { ...this.camera.settings, actorShadowExperimentMode: shadowMode },
          }
          this.emit('camera.changed', { ...this.camera, settings: { ...this.camera.settings } })
        }
        return {} as MotionMethods[M]['result']
      }
      case 'settings.setPhysics': {
        const backend = Number(p.backend)
        if (backend < 0 || backend > 2) throw new BridgeError('invalid_params', '无效的物理后端')
        this.physicsBackend = backend
        this.emit('settings.changed', { physicsBackend: backend })
        return {} as MotionMethods[M]['result']
      }
      case 'shell.setTextInputActive':
        return {} as MotionMethods[M]['result']
      case 'appearance.get':
        return { ...this.appearanceState } as MotionMethods[M]['result']

      // ---- Phase 2 methods ----
      case 'dialog.pickFile': {
        this.requireCapability('dialog.pickFile')
        const kind = Number(p.kind)
        const multiple = Boolean(p.multiple)
        const dialogId = this.nextDialogId++
        const sampleByKind: Record<number, string[]> = {
          0: multiple
            ? ['C:\\Sample\\StageLive_motion.vmd', 'C:\\Sample\\StageLive_camera.vmd', 'C:\\Sample\\StageLive.wav']
            : ['C:\\Sample\\StageLive_motion.vmd'],
          1: ['C:\\Sample\\NewDance_motion.vmd'],
          2: ['C:\\Sample\\NewDance_facial.vmd'],
          3: ['C:\\Sample\\NewDance_camera.vmd'],
          4: ['C:\\Sample\\NewDance_song.mp3'],
          5: ['C:\\Sample\\StageBackdrop.mp4'],
          6: ['C:\\Sample\\CoverArt.png'],
        }
        this.dialogResults.set(dialogId, { paths: sampleByKind[kind] ?? sampleByKind[0] })
        return { dialogId } as MotionMethods[M]['result']
      }
      case 'dialog.poll': {
        this.requireCapability('dialog.poll')
        const dialogId = Number(p.dialogId)
        const picked = this.dialogResults.get(dialogId)
        this.dialogResults.delete(dialogId)
        if (!picked) return { state: 'cancelled', paths: [], notice: '' } as MotionMethods[M]['result']
        return { state: 'accepted', paths: picked.paths, notice: '' } as MotionMethods[M]['result']
      }
      case 'library.probeFiles': {
        this.requireCapability('library.probeFiles')
        const paths = Array.isArray(p.paths) ? (p.paths as string[]) : []
        const probes = paths.filter((item) => /\.(vmd|gkcamera)$/i.test(item)).map(probeFromPath)
        const classification = classifyImportFiles(paths, probes)
        return { classification, probes } as MotionMethods[M]['result']
      }
      case 'library.startEdit': {
        this.requireCapability('library.startEdit')
        if (Number(p.revision) !== this.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请刷新后重试')
        const entry = this.entries.find((item) => item.id === p.key)
        if (!entry) throw new BridgeError('invalid_params', '曲目不存在')
        const result: EditEntryInfo = {
          key: entry.id,
          title: entry.title,
          subtitle: entry.subtitle ?? '',
          hasFacial: entry.hasFacial,
          hasCamera: entry.hasCamera,
          hasAudio: entry.hasAudio,
          audioFile: entry.audioFile,
          hasCover: entry.hasCover,
          isGroup: entry.isGroup,
          positionCount: entry.positionCount,
          positions: entry.positions.map((pos) => ({ ...pos })),
          timing: this.timings.get(entry.id) ?? defaultPerformanceTiming(),
        }
        return result as MotionMethods[M]['result']
      }
      case 'library.probeAudio': {
        this.requireCapability('library.probeAudio')
        const path = String(p.path ?? '')
        if (!path || /^[a-zA-Z]+:\/\//.test(path) || path.startsWith('\\\\') || path.startsWith('//')) {
          throw new BridgeError('invalid_params', '不支持网络或流媒体路径')
        }
        const ext = path.slice(path.lastIndexOf('.') + 1).toLowerCase()
        const stem = (path.split(/[\\/]/).pop() ?? '').replace(/\.[^.]+$/, '')
        const info: AudioProbeInfo = {
          valid: ['wav', 'mp3', 'flac', 'ogg', 'oga', 'm4a', 'aac', 'opus'].includes(ext),
          sampleRate: 48000,
          channels: 2,
          durationSeconds: 185,
          codec: ext || 'wav',
          title: stem || 'Mock Track',
          artist: ext === 'wav' ? '' : '初星学園',
          album: 'Live Collection',
          hasCover: ext !== 'wav',
          coverDataUri: ext !== 'wav' ? MOCK_COVER_PNG : '',
        }
        return info as MotionMethods[M]['result']
      }
      case 'library.getCover': {
        this.requireCapability('library.getCover')
        const key = String(p.key ?? '')
        const dir = String(p.directory ?? '')
        const entry = this.entries.find((item) => (key && item.id === key) || (dir && item.directory === dir))
        const cover = entry ? (this.coverByKey.get(entry.id) ?? (entry.hasCover ? MOCK_COVER_PNG : '')) : ''
        return { hasCover: cover !== '', coverDataUri: cover } as MotionMethods[M]['result']
      }
      case 'library.setCover': {
        this.requireCapability('library.setCover')
        const key = String(p.key ?? '')
        const clear = Boolean(p.clear)
        const coverDataUri = String(p.coverDataUri ?? '')
        const index = this.entries.findIndex((item) => item.id === key)
        if (index < 0) throw new BridgeError('not_found', '曲目不存在')
        if (clear) {
          this.coverByKey.delete(key)
          this.entries[index] = { ...this.entries[index], hasCover: false }
        } else {
          if (!coverDataUri.startsWith('data:image/')) throw new BridgeError('invalid_params', '无效封面数据')
          this.coverByKey.set(key, coverDataUri)
          this.entries[index] = { ...this.entries[index], hasCover: true }
        }
        this.libraryRevision += 1
        this.emitLibrary()
        return {
          hasCover: !clear,
          coverDataUri: clear ? '' : coverDataUri,
          revision: this.libraryRevision,
        } as MotionMethods[M]['result']
      }
      case 'library.readImagePreview': {
        this.requireCapability('library.readImagePreview')
        const path = String(p.path ?? '')
        if (!path) throw new BridgeError('invalid_params', '路径不能为空')
        return { dataUri: MOCK_COVER_PNG } as MotionMethods[M]['result']
      }
      case 'screenshot.capabilities': {
        this.requireCapability('screenshot.capabilities')
        return {
          available: true,
          runtimeReady: true,
          cameraAvailable: true,
          windowWidth: 1280,
          windowHeight: 720,
          notice: '',
        } as MotionMethods[M]['result']
      }
      case 'screenshot.capture': {
        this.requireCapability('screenshot.capture')
        return {
          dataUri: MOCK_COVER_PNG,
          width: 1280,
          height: 720,
        } as MotionMethods[M]['result']
      }
      case 'clock.setRunMode': {
        this.requireCapability('clock.setRunMode')
        const runMode: ClockRunMode = p.runMode === 'recording' ? 'recording' : 'realtime'
        this.clock = {
          ...this.clock,
          runMode,
          effectiveSource: runMode === 'recording' ? 'fixed_step' : this.clock.configuredSource,
        }
        this.emit('clock.changed', { ...this.clock })
        return { ...this.clock } as MotionMethods[M]['result']
      }
      case 'clock.configure': {
        this.requireCapability('clock.configure')
        const source = (p.source as ClockSource | undefined) ?? this.clock.configuredSource
        const fpsNumerator = Number(p.fpsNumerator ?? this.clock.fpsNumerator) || 60
        const fpsDenominator = Number(p.fpsDenominator ?? this.clock.fpsDenominator) || 1
        this.clock = {
          ...this.clock,
          configuredSource: source,
          effectiveSource: this.clock.runMode === 'recording' ? 'fixed_step' : source,
          fpsNumerator,
          fpsDenominator,
        }
        this.emit('clock.changed', { ...this.clock })
        return { ...this.clock } as MotionMethods[M]['result']
      }
      case 'audioEffect.set': {
        this.requireCapability('audioEffect.set')
        const presetId = String(p.presetId ?? '')
        if (!this.audioEffect.presets.some((item) => item.id === presetId)) {
          throw new BridgeError('failed', `未知音效预设: ${presetId}`)
        }
        this.audioEffect = { ...this.audioEffect, activeId: presetId }
        this.emit('audioEffect.changed', { ...this.audioEffect, presets: this.audioEffect.presets.map((x) => ({ ...x })) })
        return { ...this.audioEffect, presets: this.audioEffect.presets.map((x) => ({ ...x })) } as MotionMethods[M]['result']
      }
      case 'library.import': {
        this.requireCapability('library.import')
        this.requireAllowed()
        const req = p.request as ImportRequest
        if (!req || !req.title.trim()) throw new BridgeError('invalid_params', '无效的导入请求')
        if (req.editKey && this.loaded.motionKey === req.editKey && (this.player.state === 'playing' || this.player.state === 'paused')) {
          throw new BridgeError('protected', '播放中禁止覆盖当前曲目')
        }
        const applyImport = () => {
          if (this.closed) return
          const isEdit = (req.editKey ?? '') !== ''
          const id = isEdit
            ? req.editKey
            : `${req.title.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-') || 'custom'}-${Date.now().toString(36)}`
          const existingIndex = this.entries.findIndex((item) => item.id === id)
          const audioFile = req.audioFile ?? ''
          const facialFile = req.facialFile ?? ''
          const cameraFile = req.cameraFile ?? ''
          const hasAudioTrack = !req.clearAudio && audioFile !== ''
          const customCover = req.coverDataUri && !req.clearCover ? req.coverDataUri : ''
          const audioEmbeddedCover = hasAudioTrack && !audioFile.toLowerCase().endsWith('.wav')
          const rawPositions = Array.isArray(req.positions) ? req.positions : []
          const builtPositions: LibraryPositionEntry[] = rawPositions.map((pos, idx) => ({
            slot: pos.slot ?? idx + 1,
            x: pos.x ?? 0,
            y: pos.y ?? 0,
            z: pos.z ?? 0,
            yawDegrees: pos.yawDegrees ?? 0,
            hasMotion: true,
            hasFacial: !pos.disableMorph && Boolean(pos.facialFile || pos.morphVmd || facialFile),
          }))
          const nextIsGroup = builtPositions.length >= 2
          const nextPositionCount = Math.max(1, builtPositions.length)
          if (isEdit && existingIndex >= 0) {
            const current = this.entries[existingIndex]
            let nextHasCover = current.hasCover ?? false
            if (req.clearCover) {
              this.coverByKey.delete(id)
              nextHasCover = false
            } else if (customCover) {
              this.coverByKey.set(id, customCover)
              nextHasCover = true
            } else if (req.clearAudio) {
              this.coverByKey.delete(id)
              nextHasCover = false
            } else if (audioEmbeddedCover) {
              this.coverByKey.set(id, MOCK_COVER_PNG)
              nextHasCover = true
            }
            this.entries[existingIndex] = {
              ...current,
              title: req.title.trim(),
              subtitle: req.subtitle !== undefined ? req.subtitle.trim() : current.subtitle,
              hasFacial: req.disableMorph ? false : facialFile !== '' || current.hasFacial,
              hasCamera: req.clearCamera ? false : cameraFile !== '' || current.hasCamera,
              hasAudio: req.clearAudio ? false : audioFile !== '' || current.hasAudio,
              hasCover: nextHasCover,
              isGroup: rawPositions.length > 0 ? nextIsGroup : current.isGroup,
              positionCount: rawPositions.length > 0 ? nextPositionCount : current.positionCount,
              positions: rawPositions.length > 0 ? builtPositions : current.positions,
            }
          } else {
            let nextHasCover = false
            if (!req.clearCover && (customCover || audioEmbeddedCover)) {
              this.coverByKey.set(id, customCover || MOCK_COVER_PNG)
              nextHasCover = true
            }
            const created: LibraryEntry = {
              id,
              title: req.title.trim(),
              subtitle: req.subtitle?.trim() || undefined,
              directory: `gakumas-local/motions/${id}`,
              durationSeconds: 180,
              totalFrames: 10800,
              hasMotion: true,
              hasFacial: !req.disableMorph,
              hasCamera: cameraFile !== '',
              hasAudio: hasAudioTrack,
              hasCover: nextHasCover,
              isGroup: nextIsGroup,
              positionCount: nextPositionCount,
              positions: builtPositions,
            }
            this.entries.unshift(created)
          }
          this.timings.set(id, req.timing ?? defaultPerformanceTiming())
          this.libraryRevision += 1
          this.libraryNotice = isEdit ? `已更新曲目：${req.title}` : `已导入曲目：${req.title}`
          if (!isEdit && this.player.state !== 'playing') {
            const targetEntry = this.entries.find((item) => item.id === id)
            if (targetEntry) this.loadEntry(targetEntry, false)
          }
          this.emit('import.progress', {
            active: false,
            finished: true,
            succeeded: true,
            progress: 1,
            stage: 'DONE',
            statusText: '导入完成',
            errorDetail: '',
            importedKey: id,
          })
          this.emitLibrary()
        }
        this.emit('import.progress', {
          active: true,
          finished: false,
          succeeded: false,
          progress: 0.25,
          stage: 'READING',
          statusText: '正在解析 VMD 与音频轨道...',
          errorDetail: '',
        })
        const t1 = setTimeout(() => {
          if (!this.closed) {
            this.emit('import.progress', {
              active: true,
              finished: false,
              succeeded: false,
              progress: 0.72,
              stage: 'BUILDING',
              statusText: '正在生成标准化演出缓存...',
              errorDetail: '',
            })
          }
        }, 120)
        const t2 = setTimeout(applyImport, 280)
        this.importTimers.push(t1, t2)
        return { started: true } as MotionMethods[M]['result']
      }
      case 'drop.setTarget':
        this.requireCapability('drop.setTarget')
        this.dropTargetEnabled = Boolean(p.enabled)
        return {} as MotionMethods[M]['result']
      case 'drop.peek':
        this.requireCapability('drop.peek')
        return {
          visual: this.dropTargetEnabled ? this.dropPeekState.visual : 'none',
          fileCount: this.dropPeekState.fileCount,
          accepted: this.dropPeekState.accepted,
          hostElevated: this.dropPeekState.hostElevated,
          hostBlocked: this.dropPeekState.hostBlocked,
        } as MotionMethods[M]['result']
      case 'drop.poll': {
        this.requireCapability('drop.poll')
        const files = this.pendingDropFiles
        this.pendingDropFiles = []
        return { files } as MotionMethods[M]['result']
      }
      case 'bone.get':
        this.requireCapability('bone.get')
        return { ...this.bone, overrides: this.bone.overrides.map((item) => ({ ...item, eulerDeg: { ...item.eulerDeg } })) } as MotionMethods[M]['result']
      case 'bone.set': {
        this.requireCapability('bone.set')
        this.requireAllowed()
        const hid = Number(p.humanoidId)
        const pitch = Number(p.pitch)
        const yaw = Number(p.yaw)
        const roll = Number(p.roll)
        const spaceWorld = Boolean(p.spaceWorld)
        const enabled = Boolean(p.enabled)
        const symmetrical = Boolean(p.symmetrical)
        const upsert = (targetHid: number, sign: number) => {
          const name = boneNameByHid(targetHid) || String(p.boneName ?? '')
          const nextEntry = {
            humanoidId: targetHid,
            boneName: name,
            enabled,
            spaceWorld,
            eulerDeg: { x: pitch, y: yaw * sign, z: roll * sign },
            isDynamic: false,
          }
          const idx = this.bone.overrides.findIndex((o) => o.humanoidId === targetHid)
          if (idx >= 0) this.bone.overrides[idx] = nextEntry
          else this.bone.overrides.push(nextEntry)
        }
        upsert(hid, 1)
        if (symmetrical) {
          const mirror = symmetricalHid(hid)
          if (mirror >= 0) upsert(mirror, -1)
        }
        this.bone = { ...this.bone, notice: '' }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      }
      case 'bone.reset': {
        this.requireCapability('bone.reset')
        const hid = Number(p.humanoidId)
        const mirror = Boolean(p.symmetrical) ? symmetricalHid(hid) : -1
        this.bone = {
          ...this.bone,
          overrides: this.bone.overrides.filter((o) => o.humanoidId !== hid && o.humanoidId !== mirror),
          notice: '已重置指定骨骼',
        }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      }
      case 'bone.resetAll':
        this.requireCapability('bone.resetAll')
        this.bone = { ...this.bone, overrides: [], notice: '已清空全部骨骼覆盖' }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      case 'bone.setMaster':
        this.requireCapability('bone.setMaster')
        this.bone = { ...this.bone, masterEnabled: Boolean(p.enabled) }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      case 'bone.reload':
        this.requireCapability('bone.reload')
        this.bone = { ...this.bone, notice: '已从磁盘重载骨骼覆盖（mock）' }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      case 'bone.saveToMotion':
        this.requireCapability('bone.saveToMotion')
        this.bone = { ...this.bone, notice: '已保存骨骼覆盖至当前曲目（mock）' }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      case 'bone.saveToGlobal':
        this.requireCapability('bone.saveToGlobal')
        this.bone = { ...this.bone, notice: '已保存骨骼覆盖至全局配置（mock）' }
        this.emit('bone.changed', { ...this.bone })
        return {} as MotionMethods[M]['result']
      case 'gaze.get':
        this.requireCapability('gaze.get')
        return { ...this.gaze, settings: { ...this.gaze.settings } } as MotionMethods[M]['result']
      case 'gaze.set':
        this.requireCapability('gaze.set')
        this.gaze = { ...this.gaze, settings: { ...(p.settings as GazeState['settings']) } }
        this.emit('gaze.changed', { ...this.gaze })
        return {} as MotionMethods[M]['result']
      case 'stage.scan':
        this.requireCapability('stage.scan')
        this.emit('stage.nodesChanged', { nodes: this.stageNodes.map((n) => ({ ...n })) })
        return { nodes: this.stageNodes.map((n) => ({ ...n })) } as MotionMethods[M]['result']
      case 'stage.toggleNode': {
        this.requireCapability('stage.toggleNode')
        const idx = Number(p.index)
        const active = Boolean(p.active)
        this.stageNodes = this.stageNodes.map((node) =>
          node.index === idx && !node.isColliderLocked ? { ...node, currentlyActive: active } : node,
        )
        this.emit('stage.nodesChanged', { nodes: this.stageNodes.map((n) => ({ ...n })) })
        return {} as MotionMethods[M]['result']
      }
      case 'stage.setLoadReplacement': {
        this.requireCapability('stage.setLoadReplacement')
        const key = String(p.key ?? '')
        this.stage = { ...this.stage, settings: { ...this.stage.settings, loadReplacementKey: key } }
        this.emit('stage.changed', { ...this.stage, settings: { ...this.stage.settings } })
        return { notice: key ? `已预约舞台替换：${key}` : '已恢复原生摄影背景' } as MotionMethods[M]['result']
      }
      case 'experiments.get':
        this.requireCapability('experiments.get')
        return { ...this.experiments, settings: { ...this.experiments.settings } } as MotionMethods[M]['result']
      case 'experiments.set':
        this.requireCapability('experiments.set')
        this.experiments = {
          ...this.experiments,
          settings: { ...this.experiments.settings, ...(p.patch as ExperimentsState['settings']) },
        }
        this.emit('experiments.changed', { ...this.experiments })
        return {} as MotionMethods[M]['result']
      case 'telemetry.get': {
        this.requireCapability('telemetry.get')
        const stateCode =
          this.player.state === 'playing' ? 1 : this.player.state === 'paused' ? 2 : this.player.state === 'preview' ? 3 : 0
        const stateName =
          this.player.state === 'playing'
            ? '播放中'
            : this.player.state === 'paused'
              ? '已暂停'
              : this.player.state === 'preview'
                ? '预览中'
                : '已停止'
        const snap: TelemetrySnapshot = {
          statusLine: `Mock Ready | ${this.stage.sceneName}`,
          player: {
            state: stateCode,
            stateName,
            time: this.player.time,
            duration: this.player.duration,
            totalFrames: this.player.totalFrames,
            fps: this.player.fps,
            boneCount: 51,
            loadedMotionFile: this.loaded.motionKey ? `${this.loaded.motionKey}/motion.vmd` : '',
          },
          actor: {
            actorLabel: this.actors.boundLabel || '尚未绑定角色',
            boundBoneCount: 51,
            totalBoneCount: 51,
          },
        }
        return snap as MotionMethods[M]['result']
      }
      case 'sole.get':
        this.requireCapability('sole.get')
        return { ...this.sole } as MotionMethods[M]['result']
      case 'sole.set': {
        this.requireCapability('sole.set')
        const nextSettings = p.settings as SoleState['settings']
        const err = validateSole(nextSettings)
        if (err) throw new BridgeError('invalid_params', err)
        this.sole = {
          ...this.sole,
          settings: nextSettings,
          generation: this.sole.generation + 1,
          notice: '已应用鞋底测量配置（mock）',
        }
        this.emit('sole.changed', { ...this.sole })
        return {} as MotionMethods[M]['result']
      }
      case 'sole.save':
        this.requireCapability('sole.save')
        this.sole = { ...this.sole, notice: '已保存鞋底设置（mock）' }
        this.emit('sole.changed', { ...this.sole })
        return {} as MotionMethods[M]['result']
      case 'sole.load':
        this.requireCapability('sole.load')
        this.sole = { ...this.sole, notice: '已载入鞋底设置（mock）' }
        this.emit('sole.changed', { ...this.sole })
        return {} as MotionMethods[M]['result']
      case 'solePitch.get':
        this.requireCapability('solePitch.get')
        return { ...this.solePitch } as MotionMethods[M]['result']
      case 'solePitch.set':
        this.requireCapability('solePitch.set')
        this.solePitch = {
          ...this.solePitch,
          settings: { ...(p.settings as SolePitchState['settings']) },
        }
        this.emit('solePitch.changed', { ...this.solePitch })
        return {} as MotionMethods[M]['result']
      case 'studio.get':
        this.requireCapability('studio.get')
        return { ...this.studio } as MotionMethods[M]['result']
      case 'studio.set': {
        this.requireCapability('studio.set')
        const nextSettings = p.settings as StudioSettings
        const err = validateStudio(nextSettings)
        if (err) throw new BridgeError('invalid_params', err)
        this.studio = {
          settings: nextSettings,
          revision: this.studio.revision + 1,
          lightingNotice: nextSettings.lighting.enabled ? '自定义布光已启用（mock）' : '',
          videoNotice: nextSettings.video.enabled ? '幕布视频已启用（mock）' : '',
        }
        this.emit('studio.changed', { ...this.studio })
        return {} as MotionMethods[M]['result']
      }
      case 'studio.reset':
        this.requireCapability('studio.reset')
        this.studio = {
          settings: defaultStudioSettings(),
          revision: this.studio.revision + 1,
          lightingNotice: '已恢复默认摄影棚配置',
          videoNotice: '',
        }
        this.emit('studio.changed', { ...this.studio })
        return {} as MotionMethods[M]['result']
      default:
        throw new BridgeError('not_implemented', `待迁移：${method} 尚未接入原生实现`)
    }
  }

  snapshot(): MotionStateSnapshot {
    return {
      permission: { ...this.permission },
      player: { ...this.player },
      library: {
        revision: this.libraryRevision,
        entries: this.entries.map((entry) => ({
          ...entry,
          positions: entry.positions.map((pos) => ({ ...pos })),
        })),
        refreshing: this.refreshing,
        notice: this.libraryNotice,
      },
      loaded: { ...this.loaded },
      playlist: { ...this.playlist },
      actors: { ...this.actors, actors: this.actors.actors.map((actor) => ({ ...actor })) },
      formation: this.cloneFormation(),
      camera: { ...this.camera, settings: { ...this.camera.settings } },
      stage: { ...this.stage, settings: { ...this.stage.settings } },
      facial: { weights: { ...this.facial.weights }, notice: this.facial.notice },
      position: { offset: { ...this.position.offset }, notice: this.position.notice },
      physicsBackend: this.physicsBackend,
      musicVolumeSupport: 0,
      musicVolumeNotice: '',
      importProgress: null,
      motionsRoot: 'gakumas-local/motions',
      clock: { ...this.clock },
      audioEffect: {
        ...this.audioEffect,
        presets: this.audioEffect.presets.map((item) => ({ ...item })),
      },
    }
  }

  setPermission(injectionAllowed: boolean, notice = ''): void {
    this.permission = { injectionAllowed, notice }
    if (!injectionAllowed) this.scrubbing = false
    this.emit('permission.changed', { ...this.permission })
  }

  setAppearance(theme: number, uiScale: number): void {
    this.appearanceState = { ...this.appearanceState, theme, uiScale }
    this.emit('appearance.changed', { ...this.appearanceState })
  }

  simulateImportProgress(outcome: 'success' | 'error'): void {
    this.emit('import.progress', {
      active: true,
      finished: false,
      succeeded: false,
      progress: 0.22,
      stage: 'READING',
      statusText: '正在解析 VMD 与音频轨道...',
      errorDetail: '',
    })
    const t1 = setTimeout(() => {
      if (this.closed) return
      this.emit('import.progress', {
        active: true,
        finished: false,
        succeeded: false,
        progress: 0.68,
        stage: outcome === 'error' ? 'COPYING' : 'BUILDING',
        statusText: outcome === 'error' ? '正在写入演出资源文件...' : '正在生成标准化演出缓存...',
        errorDetail: '',
      })
    }, 420)
    const t2 = setTimeout(() => {
      if (this.closed) return
      if (outcome === 'error') {
        this.emit('import.progress', {
          active: false,
          finished: true,
          succeeded: false,
          progress: 0.68,
          stage: 'ERROR',
          statusText: '导入失败',
          errorDetail:
            "System.IO.IOException: The process cannot access the file 'gakumas-local\\motions\\hikari-no-saki\\audio.wav' because it is being used by another process.\r\n   at GakuMotion.VmdImporter.Native.ImportPipeline.RunEdit(ImportRequest request)",
        })
      } else {
        this.emit('import.progress', {
          active: false,
          finished: true,
          succeeded: true,
          progress: 1,
          stage: 'DONE',
          statusText: '导入完成',
          errorDetail: '',
        })
      }
    }, 900)
    this.importTimers.push(t1, t2)
  }

  setDropPeekPreview(next: Partial<DropPeek>): void {
    const visual = next.visual ?? this.dropPeekState.visual
    this.dropTargetEnabled = visual !== 'none'
    this.dropPeekState = {
      ...this.dropPeekState,
      ...next,
      visual,
    }
  }

  savedVolume(): number {
    return this.volumeSaved
  }

  on<E extends MotionEventName>(event: E, handler: (payload: MotionEvents[E]) => void): () => void {
    let set = this.handlers.get(event)
    if (!set) {
      set = new Set()
      this.handlers.set(event, set)
    }
    set.add(handler as (payload: never) => void)
    return () => {
      set?.delete(handler as (payload: never) => void)
    }
  }

  close(): void {
    this.closed = true
    if (this.tickTimer) clearInterval(this.tickTimer)
    if (this.refreshTimer) clearTimeout(this.refreshTimer)
    for (const timer of this.importTimers) clearTimeout(timer)
    this.tickTimer = null
    this.refreshTimer = null
    this.importTimers = []
  }
}
