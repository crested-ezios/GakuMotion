import {
  DEFAULT_EXPERIMENT_SETTINGS,
  DEFAULT_GAZE_SETTINGS,
  DEFAULT_SOLE_PITCH_SETTINGS,
  boneNameByHid,
  defaultSoleSettings,
  defaultStudioSettings,
  type BoneState,
  type ExperimentsState,
  type GazeState,
  type SolePitchState,
  type SoleState,
  type StageNode,
  type StudioState,
  type TelemetrySnapshot,
  type Vector3,
} from '../protocol'
import { asBool, asNum, asObj, asStr, asVec3 } from './normalizersCore'
import type { DropUiState } from './types'

export function normalizeStageNodes(raw: unknown): StageNode[] {
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

export function normalizeBoneState(raw: unknown): BoneState {
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

export function normalizeGazeState(raw: unknown): GazeState {
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

export function normalizeExperimentsState(raw: unknown): ExperimentsState {
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

export function normalizeSoleState(raw: unknown): SoleState {
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

export function normalizeSolePitchState(raw: unknown): SolePitchState {
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

export function normalizeStudioState(raw: unknown): StudioState {
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

export function normalizeTelemetrySnapshot(raw: unknown): TelemetrySnapshot | null {
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

export function normalizeDropPeek(raw: unknown): DropUiState {
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
