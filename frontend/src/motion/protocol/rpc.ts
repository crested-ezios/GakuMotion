import type { RequestOptions } from '@/bridge/protocol'
import type {
  ActorsState,
  ActorPortraitResult,
  AudioEffectState,
  AudioProbeInfo,
  CameraSettings,
  CameraState,
  ClockRunMode,
  ClockSource,
  ClockState,
  FacialState,
  FacialWeights,
  ImportProgress,
  LibraryState,
  LoadedState,
  MotionAppearance,
  MotionHello,
  MotionStateSnapshot,
  PermissionState,
  PlayerState,
  PositionState,
  ScreenshotCapabilities,
  StageFormation,
  StageSettings,
  StageState,
  Vector3,
} from './coreTypes'
import type {
  BoneState,
  DropPeek,
  EditEntryInfo,
  ExperimentSettings,
  ExperimentsState,
  GazeSettings,
  GazeState,
  ImportClassification,
  ImportRequest,
  SolePitchSettings,
  SolePitchState,
  SoleSettings,
  SoleState,
  StageNode,
  StudioSettings,
  StudioState,
  TelemetrySnapshot,
  VmdProbe,
} from './phase2Types'

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
  'actors.getPortrait': { params: { character?: string; token?: number }; result: ActorPortraitResult }
  'formation.selectSlot': { params: { slotIndex: number }; result: Record<string, never> }
  'formation.swapSlots': { params: { fromSlotIndex: number; toSlotIndex: number }; result: Record<string, never> }
  'formation.setSlotOffset': {
    params: { slotIndex: number; x: number; y: number; z: number; yawDegrees?: number }
    result: Record<string, never>
  }
  'formation.resetSlotOffset': { params: { slotIndex: number }; result: Record<string, never> }
  'formation.saveSlotOffset': { params: { slotIndex: number }; result: Record<string, never> }
  'formation.setSlotSource': { params: { slotIndex: number; sourceSlotIndex: number }; result: Record<string, never> }
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
  'formation.changed': StageFormation
  'camera.changed': CameraState
  'stage.changed': StageState
  'facial.changed': FacialState
  'position.changed': PositionState
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
