import { syncLegacyTransforms } from '../cameraMode'
import {
  DEFAULT_AUDIO_EFFECT_STATE,
  DEFAULT_CAMERA_SETTINGS,
  DEFAULT_CLOCK_STATE,
  defaultPerformanceTiming,
  type ActorsState,
  type AudioEffectState,
  type CameraState,
  type ClockRunMode,
  type ClockSource,
  type ClockState,
  type EditEntryInfo,
  type LibraryEntry,
  type LibraryPositionEntry,
  type LibraryState,
  type PerformanceTiming,
  type SceneActor,
  type StageFormation,
  type StageSlotState,
  type TrackTiming,
  type Vector3,
} from '../protocol'
import { cloneDefaultFormation } from './types'

export function asObj(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === 'object' ? (value as Record<string, unknown>) : {}
}

export function asBool(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback
}

export function asNum(value: unknown, fallback = 0): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

export function asStr(value: unknown, fallback = ''): string {
  return typeof value === 'string' ? value : fallback
}

export function asVec3(value: unknown, fallback: Vector3 = { x: 0, y: 0, z: 0 }): Vector3 {
  const obj = asObj(value)
  return {
    x: asNum(obj.x, fallback.x),
    y: asNum(obj.y, fallback.y),
    z: asNum(obj.z, fallback.z),
  }
}

export function normalizeTiming(raw: unknown): PerformanceTiming {
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

export function normalizeLibraryPositions(raw: unknown): LibraryPositionEntry[] {
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

export function normalizeLibraryEntry(raw: unknown): LibraryEntry {
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

export function normalizeLibraryState(raw: unknown): LibraryState {
  const obj = asObj(raw)
  const entries = Array.isArray(obj.entries) ? obj.entries.map(normalizeLibraryEntry) : []
  return {
    revision: asNum(obj.revision, 0),
    entries,
    refreshing: asBool(obj.refreshing, false),
    notice: asStr(obj.notice),
  }
}

export function normalizeActorsState(raw: unknown): ActorsState {
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

export function normalizeStageSlot(raw: unknown, idx: number): StageSlotState {
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

export function normalizeFormation(raw: unknown): StageFormation {
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

export function normalizeCameraState(raw: CameraState): CameraState {
  const sObj = asObj(raw.settings)
  const settings = syncLegacyTransforms({
    ...DEFAULT_CAMERA_SETTINGS,
    ...raw.settings,
    followSlotIndex: asNum(sObj.followSlotIndex, DEFAULT_CAMERA_SETTINGS.followSlotIndex),
  })
  return { ...raw, settings }
}

export function normalizeEditEntryInfo(raw: unknown, fallbackKey: string): EditEntryInfo {
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

export function normalizeClockState(raw: unknown): ClockState {
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

export function normalizeAudioEffectState(raw: unknown): AudioEffectState {
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
