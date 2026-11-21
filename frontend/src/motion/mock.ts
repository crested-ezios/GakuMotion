// DEV/PREVIEW mock of the GakuMotion web-panel bridge (`gakumotion.web-panel/v1`).
// Supports two capability profiles:
//   - 'full' (default): simulates all Phase 1 + Phase 2 native methods for rich
//     standalone preview (?preview=1) and interactive testing.
//   - 'phase1': exposes only core Phase 1 methods so capability-gating and
//     "待迁移" degradation UI can be verified directly.

import {
  BridgeError,
  DEFAULT_FACIAL_WEIGHTS,
  type DropPeek,
  type LibraryEntry,
  type MotionBridgeClient,
  type MotionEventName,
  type MotionEvents,
  type MotionMethodName,
  type MotionMethods,
  type MotionStateSnapshot,
  type RequestOptions,
  type SceneActor,
  type StageFormation,
} from './protocol'
import { PHASE1_METHODS, PHASE2_METHODS, type MockCapabilityProfile } from './mock/capabilities'
import { createInitialMockState, type MockHandlerContext, type MockHandlerMap, type MockRuntimeState } from './mock/context'
import { buildMockFormation } from './mock/seed'
import { createPlaybackHandlers } from './mock/handlers/playback'
import { createLibraryHandlers } from './mock/handlers/library'
import { createSceneHandlers } from './mock/handlers/scene'
import { createDevAndStudioHandlers } from './mock/handlers/devAndStudio'

export type { MockCapabilityProfile }

export class MotionMockBridgeClient implements MotionBridgeClient {
  readonly kind = 'mock' as const
  private readonly handlers = new Map<MotionEventName, Set<(payload: never) => void>>()
  private readonly state: MockRuntimeState
  private readonly methodHandlers: MockHandlerMap

  constructor(options?: { capabilityProfile?: MockCapabilityProfile }) {
    this.state = createInitialMockState(options?.capabilityProfile ?? 'full')
    const ctx: MockHandlerContext = {
      state: this.state,
      supportedMethods: () => this.supportedMethods(),
      emit: (event, payload) => this.emit(event, payload),
      emitPlayer: () => this.emitPlayer(),
      emitLibrary: () => this.emitLibrary(),
      emitFormation: () => this.emitFormation(),
      cloneFormation: () => this.cloneFormation(),
      snapshot: () => this.snapshot(),
      startTick: () => this.startTick(),
      loadEntry: (entry, autoplay) => this.loadEntry(entry, autoplay),
      cycle: (direction) => this.cycle(direction),
      requireClip: () => this.requireClip(),
      requireAllowed: () => this.requireAllowed(),
      requireCapability: (method) => this.requireCapability(method),
    }
    this.methodHandlers = {
      ...createPlaybackHandlers(ctx),
      ...createLibraryHandlers(ctx),
      ...createSceneHandlers(ctx),
      ...createDevAndStudioHandlers(ctx),
    }
  }

  getCapabilityProfile(): MockCapabilityProfile {
    return this.state.capabilityProfile
  }

  setCapabilityProfile(profile: MockCapabilityProfile): void {
    this.state.capabilityProfile = profile
    this.state.formation = {
      ...this.state.formation,
      available: profile === 'full',
      verified: profile === 'full',
    }
  }

  simulateFileDrop(paths: string[]): void {
    this.state.pendingDropFiles = [...paths]
    this.state.dropPeekState = {
      visual: 'none',
      fileCount: 0,
      accepted: 0,
      hostElevated: false,
      hostBlocked: false,
    }
  }

  private supportedMethods(): MotionMethodName[] {
    return this.state.capabilityProfile === 'full' ? [...PHASE1_METHODS, ...PHASE2_METHODS] : [...PHASE1_METHODS]
  }

  private emit<E extends MotionEventName>(event: E, payload: MotionEvents[E]): void {
    this.state.seq += 1
    const set = this.handlers.get(event)
    if (set) for (const handler of set) (handler as (p: unknown) => void)(payload)
  }

  push<E extends MotionEventName>(event: E, payload: MotionEvents[E]): void {
    const obj = payload as unknown as Record<string, unknown>
    if (event === 'library.changed' && obj) {
      if (typeof obj.revision === 'number') this.state.libraryRevision = obj.revision
      if (Array.isArray(obj.entries)) {
        this.state.entries = (obj.entries as LibraryEntry[]).map((entry) => ({
          ...entry,
          isGroup: Boolean(entry.isGroup),
          positionCount: entry.positionCount ?? (entry.isGroup ? 2 : 1),
          positions: Array.isArray(entry.positions) ? entry.positions.map((pos) => ({ ...pos })) : [],
        }))
      }
      if (typeof obj.notice === 'string') this.state.libraryNotice = obj.notice
      if (typeof obj.refreshing === 'boolean') this.state.refreshing = obj.refreshing
    } else if (event === 'actors.changed' && obj) {
      this.state.actors = {
        ...this.state.actors,
        ...(obj as Partial<typeof this.state.actors>),
        actors: Array.isArray(obj.actors)
          ? (obj.actors as SceneActor[]).map((actor) => ({ ...actor }))
          : this.state.actors.actors,
      }
    } else if (event === 'formation.changed' && obj) {
      const f = obj as unknown as Partial<StageFormation>
      const rawSlots = Array.isArray(f.slots) ? f.slots : this.state.formation.slots
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
      this.state.formation = {
        ...this.state.formation,
        ...f,
        slots,
      }
    } else if (event === 'camera.changed' && obj) {
      const c = obj as Partial<typeof this.state.camera>
      this.state.camera = {
        ...this.state.camera,
        ...c,
        settings: c.settings ? { ...this.state.camera.settings, ...c.settings } : this.state.camera.settings,
      }
    } else if (event === 'loaded.changed' && obj) {
      this.state.loaded = { ...this.state.loaded, ...(obj as Partial<typeof this.state.loaded>) }
    }
    this.emit(event, payload)
  }

  private emitPlayer(): void {
    this.emit('player.stateChanged', { ...this.state.player })
  }

  private emitLibrary(): void {
    this.emit('library.changed', {
      revision: this.state.libraryRevision,
      entries: this.state.entries.map((entry) => ({
        ...entry,
        positions: entry.positions.map((pos) => ({ ...pos })),
      })),
      refreshing: this.state.refreshing,
      notice: this.state.libraryNotice,
    })
  }

  private cloneFormation(): StageFormation {
    return {
      ...this.state.formation,
      slots: this.state.formation.slots.map((slot) => ({
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
    if (this.state.tickTimer) return
    this.state.tickTimer = setInterval(() => {
      if (this.state.closed || this.state.scrubbing || this.state.player.state !== 'playing') return
      const next = this.state.player.time + 0.1 * this.state.player.speed
      if (next >= this.state.player.duration) {
        if (this.state.player.playMode === 1) {
          this.state.player = { ...this.state.player, time: 0 }
        } else if (this.state.player.playMode === 2 || this.state.player.playMode === 3) {
          this.cycle(this.state.player.playMode === 3 ? 'random' : 1)
          return
        } else {
          this.state.player = { ...this.state.player, state: 'stopped', time: 0 }
        }
      } else {
        this.state.player = { ...this.state.player, time: next }
      }
      this.emitPlayer()
    }, 100)
  }

  private loadEntry(entry: LibraryEntry, autoplay: boolean): void {
    this.state.loaded = {
      motionKey: entry.id,
      generation: this.state.loaded.generation + 1,
      injectionSession: this.state.loaded.injectionSession,
      sourceMissing: false,
      sourceChanged: false,
    }
    this.state.player = {
      ...this.state.player,
      state: autoplay ? 'playing' : 'preview',
      title: entry.title,
      time: 0,
      duration: entry.durationSeconds,
      totalFrames: entry.totalFrames,
      fps: entry.totalFrames > 0 && entry.durationSeconds > 0 ? entry.totalFrames / entry.durationSeconds : 60,
    }
    this.state.camera = { ...this.state.camera, hasTrack: entry.hasCamera }
    this.state.formation = buildMockFormation(
      this.state.actors.actors,
      this.state.actors.epoch,
      entry,
      this.state.capabilityProfile === 'full',
      this.state.formation.revision + 1,
      this.state.formation.selectedSlotIndex,
    )
    this.emit('loaded.changed', { ...this.state.loaded })
    this.emit('formation.changed', this.cloneFormation())
    this.emit('camera.changed', { ...this.state.camera, settings: { ...this.state.camera.settings } })
    this.emitPlayer()
    if (autoplay) this.startTick()
  }

  private cycle(direction: number | 'random'): void {
    if (this.state.entries.length === 0) return
    const currentIndex = this.state.entries.findIndex((entry) => entry.id === this.state.loaded.motionKey)
    let nextIndex: number
    if (direction === 'random') nextIndex = Math.floor(Math.random() * this.state.entries.length)
    else nextIndex = (currentIndex + direction + this.state.entries.length) % this.state.entries.length
    this.loadEntry(this.state.entries[nextIndex], true)
  }

  private requireClip(): void {
    if (this.state.player.totalFrames <= 0 || this.state.player.duration <= 0) {
      throw new BridgeError('not_ready', '尚未加载曲目')
    }
  }

  private requireAllowed(): void {
    if (!this.state.permission.injectionAllowed) throw new BridgeError('not_allowed', 'GakuMotion 当前已禁用')
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
    if (this.state.closed) throw new BridgeError('closed', 'The bridge is closed')
    await new Promise((resolve) => setTimeout(resolve, 5))
    const handler = this.methodHandlers[method]
    if (!handler) {
      throw new BridgeError('not_implemented', `待迁移：${method} 尚未接入原生实现`)
    }
    return handler((params ?? {}) as Record<string, unknown>) as MotionMethods[M]['result']
  }

  snapshot(): MotionStateSnapshot {
    return {
      permission: { ...this.state.permission },
      player: { ...this.state.player },
      library: {
        revision: this.state.libraryRevision,
        entries: this.state.entries.map((entry) => ({
          ...entry,
          positions: entry.positions.map((pos) => ({ ...pos })),
        })),
        refreshing: this.state.refreshing,
        notice: this.state.libraryNotice,
      },
      loaded: { ...this.state.loaded },
      playlist: { ...this.state.playlist },
      actors: { ...this.state.actors, actors: this.state.actors.actors.map((actor) => ({ ...actor })) },
      formation: this.cloneFormation(),
      camera: { ...this.state.camera, settings: { ...this.state.camera.settings } },
      stage: { ...this.state.stage, settings: { ...this.state.stage.settings } },
      facial: { weights: { ...this.state.facial.weights }, notice: this.state.facial.notice },
      position: { offset: { ...this.state.position.offset }, notice: this.state.position.notice },
      physicsBackend: this.state.physicsBackend,
      musicVolumeSupport: 0,
      musicVolumeNotice: '',
      importProgress: null,
      motionsRoot: 'gakumas-local/motions',
      clock: { ...this.state.clock },
      audioEffect: {
        ...this.state.audioEffect,
        presets: this.state.audioEffect.presets.map((item) => ({ ...item })),
      },
    }
  }

  setPermission(injectionAllowed: boolean, notice = ''): void {
    this.state.permission = { injectionAllowed, notice }
    if (!injectionAllowed) this.state.scrubbing = false
    this.emit('permission.changed', { ...this.state.permission })
  }

  setAppearance(theme: number, uiScale: number): void {
    this.state.appearanceState = { ...this.state.appearanceState, theme, uiScale }
    this.emit('appearance.changed', { ...this.state.appearanceState })
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
      if (this.state.closed) return
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
      if (this.state.closed) return
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
    this.state.importTimers.push(t1, t2)
  }

  setDropPeekPreview(next: Partial<DropPeek>): void {
    const visual = next.visual ?? this.state.dropPeekState.visual
    this.state.dropTargetEnabled = visual !== 'none'
    this.state.dropPeekState = {
      ...this.state.dropPeekState,
      ...next,
      visual,
    }
  }

  savedVolume(): number {
    return this.state.volumeSaved
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
    this.state.closed = true
    if (this.state.tickTimer) clearInterval(this.state.tickTimer)
    if (this.state.refreshTimer) clearTimeout(this.state.refreshTimer)
    for (const timer of this.state.importTimers) clearTimeout(timer)
    this.state.tickTimer = null
    this.state.refreshTimer = null
    this.state.importTimers = []
  }
}
