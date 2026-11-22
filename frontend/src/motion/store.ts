import { useSyncExternalStore } from 'react'
import { getMotionBridge, setMotionBridge } from './bridge'
import {
  BridgeError,
  type CameraState,
  type MotionBridgeClient,
  type MotionMethodName,
  type MotionMethods,
  type MotionStateSnapshot,
  type PlayerState,
} from './protocol'
import { deepEqual } from './utils'
import type { StoreContext } from './store/context'
import {
  asObj,
  normalizeActorsState,
  normalizeAudioEffectState,
  normalizeCameraState,
  normalizeClockState,
  normalizeFormation,
  normalizeLibraryState,
} from './store/normalizersCore'
import {
  normalizeBoneState,
  normalizeExperimentsState,
  normalizeGazeState,
  normalizeSolePitchState,
  normalizeSoleState,
  normalizeStageNodes,
  normalizeStudioState,
} from './store/normalizersPhase2'
import { createCoverEditorSlice, type CoverEditorActions } from './store/slices/coverEditor'
import { createDevToolsSlice, type DevToolsActions } from './store/slices/devTools'
import { createDialogDropSlice, type DialogDropActions } from './store/slices/dialogDrop'
import { createImportFormSlice, type ImportFormActions } from './store/slices/importForm'
import {
  createImportHelpers,
  createImportPositionsSlice,
  type ImportPositionsActions,
} from './store/slices/importPositions'
import { createImportProgressSlice, type ImportProgressActions } from './store/slices/importProgress'
import { createLibrarySlice, type LibraryActions } from './store/slices/library'
import { createMotionFormationSlice, type MotionFormationActions } from './store/slices/motionFormation'
import { createSoleStudioSlice, type SoleStudioActions } from './store/slices/soleStudio'
import { createTransportSlice, type TransportActions } from './store/slices/transport'
import { initialState, type MotionAppState } from './store/types'

export type {
  CoverEditorState,
  DialogTarget,
  DropUiState,
  ImportCoverMode,
  ImportFormState,
  ImportPositionFormState,
  ImportSlotKey,
  ImportSlotState,
  ImportUiState,
  LibraryFilter,
  LibrarySort,
  MotionAppState,
  MotionPage,
  PendingDelete,
} from './store/types'

type Listener = () => void

export interface MotionStore
  extends TransportActions,
    LibraryActions,
    CoverEditorActions,
    MotionFormationActions,
    ImportPositionsActions,
    ImportFormActions,
    ImportProgressActions,
    DialogDropActions,
    DevToolsActions,
    SoleStudioActions {}

export class MotionStore {
  private state: MotionAppState = initialState()
  private readonly listeners = new Set<Listener>()
  private bridge: MotionBridgeClient | null
  private started = false
  private unsubs: Array<() => void> = []
  private readonly watcherStoppers: Array<() => void> = []
  private readonly sliceResetters: Array<() => void> = []
  private readonly hooks: {
    onLibraryChangedCovers: ReturnType<typeof createLibrarySlice>['onLibraryChangedCovers']
    onCoverReady: ReturnType<typeof createLibrarySlice>['onCoverReady']
    maybeAutoSubmitAfterStop: () => void
    maybeFinalizeImportSelection: ReturnType<typeof createImportProgressSlice>['maybeFinalizeImportSelection']
    onImportProgress: ReturnType<typeof createImportProgressSlice>['onImportProgress']
    getPendingEditKey: () => string | null
    onSoleChanged: ReturnType<typeof createSoleStudioSlice>['onSoleChanged']
    onStudioChanged: ReturnType<typeof createSoleStudioSlice>['onStudioChanged']
  }

  constructor(bridge?: MotionBridgeClient) {
    this.bridge = bridge ?? null
    const ctx: StoreContext = {
      getState: () => this.state,
      set: (partial) => this.set(partial),
      client: () => this.client(),
      getBridge: () => this.bridge,
      isStarted: () => this.started,
      supports: (method) => this.supports(method),
      reportActionError: (error) => this.reportActionError(error),
      store: this,
    }

    const transport = createTransportSlice(ctx)
    const library = createLibrarySlice(ctx)
    const coverEditor = createCoverEditorSlice(ctx, library.invalidateCoverCacheKey)
    const motionFormation = createMotionFormationSlice(ctx)
    const importHelpers = createImportHelpers(ctx)
    const importProgress = createImportProgressSlice(ctx)
    const importPositions = createImportPositionsSlice(ctx, importHelpers)
    const importForm = createImportFormSlice(ctx, importHelpers, importProgress)
    const dialogDrop = createDialogDropSlice(ctx)
    const devTools = createDevToolsSlice(ctx)
    const soleStudio = createSoleStudioSlice(ctx)

    const slices = [
      transport,
      library,
      coverEditor,
      motionFormation,
      importPositions,
      importForm,
      importProgress,
      dialogDrop,
      devTools,
      soleStudio,
    ]
    for (const slice of slices) {
      Object.assign(this, slice.actions)
      if (slice.stopWatchers) this.watcherStoppers.push(slice.stopWatchers)
      if (slice.reset) this.sliceResetters.push(slice.reset)
    }

    this.hooks = {
      onLibraryChangedCovers: library.onLibraryChangedCovers,
      onCoverReady: library.onCoverReady,
      maybeAutoSubmitAfterStop: importForm.maybeAutoSubmitAfterStop,
      maybeFinalizeImportSelection: importProgress.maybeFinalizeImportSelection,
      onImportProgress: importProgress.onImportProgress,
      getPendingEditKey: importProgress.getPendingEditKey,
      onSoleChanged: soleStudio.onSoleChanged,
      onStudioChanged: soleStudio.onStudioChanged,
    }
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
        const nextCoverCache = this.hooks.onLibraryChangedCovers(
          library,
          previousEntries,
          this.hooks.getPendingEditKey(),
        )
        this.set({
          library,
          selectedLibraryKey: stillPresent ? selectedKey : null,
          ...(nextCoverCache !== null ? { coverCache: nextCoverCache } : {}),
        })
        this.hooks.maybeFinalizeImportSelection(library)
  
      }),
      bridge.on('library.coverReady', ({ key, hasCover, coverDataUri }) =>
        this.hooks.onCoverReady(key, hasCover, coverDataUri),
      ),
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
      bridge.on('import.progress', (importProgress) => this.hooks.onImportProgress(importProgress)),
      bridge.on('appearance.changed', (appearance) => this.set({ appearance })),
      bridge.on('bone.changed', (bone) => this.set({ bone: normalizeBoneState(bone) })),
      bridge.on('gaze.changed', (gaze) => this.set({ gaze: normalizeGazeState(gaze) })),
      bridge.on('stage.nodesChanged', (payload) =>
        this.set({ stageNodes: normalizeStageNodes(asObj(payload).nodes ?? payload) }),
      ),
      bridge.on('experiments.changed', (experiments) =>
        this.set({ experiments: normalizeExperimentsState(experiments) }),
      ),
      bridge.on('sole.changed', (sole) => this.hooks.onSoleChanged(normalizeSoleState(sole))),
      bridge.on('solePitch.changed', (solePitch) =>
        this.set({ solePitch: normalizeSolePitchState(solePitch) }),
      ),
      bridge.on('studio.changed', (studio) => this.hooks.onStudioChanged(normalizeStudioState(studio))),
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
    loads.push(fetch('sole.get', (sole) => this.hooks.onSoleChanged(normalizeSoleState(sole))))
    loads.push(fetch('solePitch.get', (solePitch) => this.set({ solePitch: normalizeSolePitchState(solePitch) })))
    loads.push(fetch('studio.get', (studio) => this.hooks.onStudioChanged(normalizeStudioState(studio))))
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
    if (raw.sole) this.hooks.onSoleChanged(normalizeSoleState(raw.sole))
    if (raw.solePitch) patch.solePitch = normalizeSolePitchState(raw.solePitch)
    if (raw.studio) this.hooks.onStudioChanged(normalizeStudioState(raw.studio))
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

  private onPlayerStateChanged(player: PlayerState): void {
    this.set({ player })
    this.hooks.maybeAutoSubmitAfterStop()
  }

  private stopWatchers(): void {
    for (const stop of this.watcherStoppers) stop()
  }

  // ---- Lifecycle ----

  reset(): void {
    for (const unsub of this.unsubs) unsub()
    this.unsubs = []
    this.stopWatchers()
    for (const resetSlice of this.sliceResetters) resetSlice()
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
