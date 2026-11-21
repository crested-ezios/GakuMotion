// Pure validation/classification ports of the ImGui reference semantics
// (GkStageStudioSettings.hpp, GkSoleMeasurement.hpp,
// GuiLibraryPage.hpp, GuiOverlayCommon.hpp). Shared by the store, the dev mock
// and the tests; the native service re-validates independently.

import {
  isSlotPlaceholder,
  type ImportClassification,
  type SoleSettings,
  type StudioSettings,
  type VmdProbe,
} from './protocol'

function finiteInRange(value: number, low: number, high: number): boolean {
  return Number.isFinite(value) && value >= low && value <= high
}

export function validateStudio(settings: StudioSettings): string | null {
  const lighting = settings.lighting
  for (const color of [lighting.keyColor, lighting.fillColor, lighting.rimColor, lighting.decalTint]) {
    for (const value of color) {
      if (!finiteInRange(value, 0, 1)) return 'Invalid studio setting: RGB must be finite and in [0,1]'
    }
  }
  if (
    !finiteInRange(lighting.keyIntensity, 0, 8) ||
    !finiteInRange(lighting.fillIntensity, 0, 8) ||
    !finiteInRange(lighting.rimIntensity, 0, 8) ||
    !finiteInRange(lighting.actorIntensity, 0, 4) ||
    !finiteInRange(lighting.rimPower, 0, 8) ||
    !finiteInRange(lighting.decalIntensityScale, 0, 10)
  ) {
    return 'Invalid studio setting: intensity out of range'
  }
  for (const yaw of [lighting.keyYaw, lighting.fillYaw, lighting.rimYaw]) {
    if (!finiteInRange(yaw, -180, 180)) return 'Invalid studio setting: light yaw'
  }
  for (const pitch of [lighting.keyPitch, lighting.fillPitch, lighting.rimPitch]) {
    if (!finiteInRange(pitch, -90, 90)) return 'Invalid studio setting: light pitch'
  }
  const video = settings.video
  if (
    !finiteInRange(video.distance, 0.25, 100) ||
    !finiteInRange(video.width, 0.1, 100) ||
    !finiteInRange(video.offsetX, -50, 50) ||
    !finiteInRange(video.offsetY, -50, 50)
  ) {
    return 'Invalid studio setting: video placement out of range'
  }
  if (
    video.path.length > 32767 ||
    video.path.includes('\0') ||
    video.path.includes('://') ||
    video.path.startsWith('\\\\') ||
    video.path.startsWith('//')
  ) {
    return 'Video input must be a local file path'
  }
  if (video.enabled && video.path === '') return 'Choose a local video file before enabling video'
  if (video.path !== '' && !isAbsoluteLocalPath(video.path)) {
    return 'Video input must use an absolute local file path'
  }
  return null
}

export function isAbsoluteLocalPath(path: string): boolean {
  return /^[A-Za-z]:[\\/]/.test(path) || path.startsWith('\\')
}

function dot(a: { x: number; y: number; z: number }, b: { x: number; y: number; z: number }): number {
  return a.x * b.x + a.y * b.y + a.z * b.z
}

// Frontend-checkable part of SoleMeasurementSettings::IsValid. The boot-proxy
// profile geometry lives in native code; native re-validates the full contract.
export function validateSole(settings: SoleSettings): string | null {
  const normal = settings.planeNormal
  const norm = dot(normal, normal)
  if (
    !Number.isFinite(normal.x) ||
    !Number.isFinite(normal.y) ||
    !Number.isFinite(normal.z) ||
    Math.abs(norm - 1) > 0.001 ||
    normal.y < 0.1
  ) {
    return '平面法线必须是单位向量且 Y 分量不小于 0.1'
  }
  if (!Number.isFinite(settings.planeOffsetM) || Math.abs(settings.planeOffsetM) > 10000) {
    return '平面距离超出范围（±10000 m）'
  }
  if (!Number.isFinite(settings.displayBandM) || settings.displayBandM < 0 || settings.displayBandM > 0.05) {
    return '显示容差必须在 0–0.05 m 之间'
  }
  for (const foot of settings.points) {
    for (const point of foot) {
      if (!Number.isFinite(point.x) || !Number.isFinite(point.y) || !Number.isFinite(point.z) || dot(point, point) > 0.25) {
        return '手工代理点必须是有限值且距 ankle 不超过 0.5 m'
      }
    }
    if (settings.shoeCalibrated && !settings.useBootProxy) {
      const heelFore = Math.hypot(foot[0].x - foot[1].x, foot[0].y - foot[1].y, foot[0].z - foot[1].z)
      const foreToe = Math.hypot(foot[1].x - foot[2].x, foot[1].y - foot[2].y, foot[1].z - foot[2].z)
      if (heelFore * heelFore < 1e-6 || foreToe * foreToe < 1e-6) {
        return '已核对的六个鞋底点不能重合'
      }
    }
  }
  return null
}

export function canUseAsMotion(probe: VmdProbe): boolean {
  return probe.valid && probe.bodyBoneFrames > 0
}

export function canUseAsFacial(probe: VmdProbe): boolean {
  return probe.valid && (probe.morphFrames > 0 || probe.eyeBoneFrames > 0)
}

export function canUseAsCamera(probe: VmdProbe): boolean {
  return probe.valid && probe.cameraFrames > 0
}

export interface ImportPositionPicks {
  motion: string
  facial: string
  preserveExisting?: boolean
}

export interface ImportFormPicks {
  isEditMode: boolean
  motion: string
  facial: string
  camera: string
  audio: string
  title: string
  positions?: ImportPositionPicks[]
}

// Fool-proof validation from DrawAddEditModal, in the same priority order.
export function validateImportForm(
  picks: ImportFormPicks,
  probeOf: (path: string) => VmdProbe | null,
  importBusy: boolean,
  editTargetMissing: boolean,
): string | null {
  if (importBusy) return '正在导入，请等待完成后保存'
  if (picks.isEditMode && editTargetMissing) return '原曲目已不在曲库中；表单已保留，请刷新后重试'

  const positions = picks.positions && picks.positions.length > 0
    ? picks.positions
    : [{ motion: picks.motion, facial: picks.facial, preserveExisting: picks.isEditMode && isSlotPlaceholder(picks.motion) }]

  for (const pos of positions) {
    const requireMotion = !picks.isEditMode || !pos.preserveExisting
    if (requireMotion) {
      if (isSlotPlaceholder(pos.motion)) return '请选择有效的身体动作 (.vmd) 文件'
      const probe = probeOf(pos.motion)
      if (!probe || !canUseAsMotion(probe)) {
        return '所选动作文件不包含身体骨骼动作关键帧（不能用纯镜头或纯表情文件作为动作）'
      }
    } else if (!isSlotPlaceholder(pos.motion)) {
      const probe = probeOf(pos.motion)
      if (!probe || !canUseAsMotion(probe)) return '所选动作文件不包含身体骨骼动作关键帧'
    }

    if (!isSlotPlaceholder(pos.facial)) {
      const probe = probeOf(pos.facial)
      if (!probe || !canUseAsFacial(probe)) return '所选表情文件不包含表情或眼神关键帧'
    }
  }
  if (!isSlotPlaceholder(picks.camera)) {
    const probe = probeOf(picks.camera)
    if (!probe || !canUseAsCamera(probe)) return '所选镜头文件不包含镜头关键帧'
  }
  if (!isSlotPlaceholder(picks.audio)) {
    if (/^[a-zA-Z]+:\/\//.test(picks.audio) || picks.audio.startsWith('\\\\') || picks.audio.startsWith('//')) {
      return '不支持网络或流媒体路径，请选择本地音频文件'
    }
    if (!isSupportedAudioExtension(picks.audio)) {
      return '音频轨道仅支持 .wav/.mp3/.flac/.ogg/.m4a/.aac/.opus 本地文件'
    }
  }
  if (picks.title.trim() === '') return '请输入有效的演出曲目名称'
  return null
}

function fileStem(path: string): string {
  const name = path.split(/[\\/]/).pop() ?? ''
  const dotIndex = name.lastIndexOf('.')
  return dotIndex > 0 ? name.slice(0, dotIndex) : name
}

function fileExt(path: string): string {
  const name = path.split(/[\\/]/).pop() ?? ''
  const dotIndex = name.lastIndexOf('.')
  return dotIndex >= 0 ? name.slice(dotIndex).toLowerCase() : ''
}

function hasCamKeyword(path: string): boolean {
  const stem = fileStem(path)
  const lower = stem.toLowerCase()
  return (
    lower.includes('camera') ||
    lower.includes('cam') ||
    stem.includes('カメラ') ||
    stem.includes('镜头') ||
    stem.includes('運鏡') ||
    stem.includes('运镜')
  )
}

function hasFaceKeyword(path: string): boolean {
  const stem = fileStem(path)
  const lower = stem.toLowerCase()
  return (
    lower.includes('face') ||
    lower.includes('facial') ||
    lower.includes('morph') ||
    lower.includes('eye') ||
    stem.includes('表情') ||
    stem.includes('口型') ||
    stem.includes('目')
  )
}

function hasMotionKeyword(path: string): boolean {
  const stem = fileStem(path)
  const lower = stem.toLowerCase()
  return (
    lower.includes('motion') ||
    lower.includes('dance') ||
    lower.includes('choreo') ||
    stem.includes('动作') ||
    stem.includes('モーション')
  )
}

function pickBest(paths: string[], predicate: (path: string) => boolean): string {
  if (paths.length === 0) return ''
  return paths.find(predicate) ?? paths[0]
}

function cleanTitleSuffix(title: string): string {
  let result = title
  for (const suffix of ['_motion', '_dance', '_choreo', '_vmd']) {
    if (result.toLowerCase().endsWith(suffix)) result = result.slice(0, result.length - suffix.length)
  }
  return result
}

// Supported local audio formats (decoded via GakuAudio miniaudio / Media Foundation).
export const SUPPORTED_AUDIO_EXTS = ['.wav', '.mp3', '.flac', '.ogg', '.oga', '.m4a', '.aac', '.opus', '.wma'] as const

export function isSupportedAudioExtension(path: string): boolean {
  return (SUPPORTED_AUDIO_EXTS as readonly string[]).includes(fileExt(path))
}

export const SUPPORTED_IMAGE_EXTS = ['.png', '.jpg', '.jpeg', '.webp', '.bmp'] as const

export function isSupportedImageExtension(path: string): boolean {
  return (SUPPORTED_IMAGE_EXTS as readonly string[]).includes(fileExt(path))
}

// ClassifyImportFiles port: probes are supplied by the caller (native probe or
// the dev mock). Accepts all supported local audio formats.
export function classifyImportFiles(paths: string[], probes: VmdProbe[]): ImportClassification {
  const result: ImportClassification = {
    suggestedTitle: '',
    motionCandidate: '',
    motionCandidates: [],
    facialCandidate: '',
    cameraCandidate: '',
    audioCandidate: '',
    unsupportedAudioCount: 0,
  }
  const probeByPath = new Map(probes.map((probe) => [probe.path, probe]))
  const audios: string[] = []
  const vmdAndCam: Array<{ path: string; probe: VmdProbe }> = []
  for (const path of paths) {
    const ext = fileExt(path)
    if (ext === '.vmd' || ext === '.gkcamera') {
      const probe = probeByPath.get(path)
      if (probe) vmdAndCam.push({ path, probe })
    } else if ((SUPPORTED_AUDIO_EXTS as readonly string[]).includes(ext)) {
      audios.push(path)
    }
  }
  if (audios.length > 0) result.audioCandidate = audios[0]

  const cameraOnlyList = vmdAndCam.filter((item) => item.probe.tendency === 'cameraOnly').map((item) => item.path)
  const facialOnlyList = vmdAndCam.filter((item) => item.probe.tendency === 'facialOnly').map((item) => item.path)
  const motionAndFacialList = vmdAndCam
    .filter((item) => item.probe.tendency === 'motionAndFacial')
    .map((item) => item.path)
  const motionOnlyList = vmdAndCam.filter((item) => item.probe.tendency === 'motionOnly').map((item) => item.path)

  if (cameraOnlyList.length > 0) {
    result.cameraCandidate = pickBest(cameraOnlyList, hasCamKeyword)
  } else {
    result.cameraCandidate = vmdAndCam.find((item) => canUseAsCamera(item.probe))?.path ?? ''
  }
  if (facialOnlyList.length > 0) result.facialCandidate = pickBest(facialOnlyList, hasFaceKeyword)

  const motionPool = [...motionOnlyList, ...motionAndFacialList]
  const fallbackMotionPool =
    motionPool.length > 0
      ? motionPool
      : vmdAndCam.filter((item) => canUseAsMotion(item.probe)).map((item) => item.path)
  const uniqueMotionPaths = Array.from(new Set(fallbackMotionPool))
  const baseName = (p: string) => p.split(/[\\/]/).pop() ?? p
  uniqueMotionPaths.sort((a, b) => {
    const ba = baseName(a)
    const bb = baseName(b)
    const la = ba.toLowerCase()
    const lb = bb.toLowerCase()
    if (la < lb) return -1
    if (la > lb) return 1
    return ba < bb ? -1 : ba > bb ? 1 : a < b ? -1 : a > b ? 1 : 0
  })
  result.motionCandidates = uniqueMotionPaths

  if (motionPool.length > 0) {
    result.motionCandidate = pickBest(motionPool, hasMotionKeyword)
  } else {
    result.motionCandidate = result.motionCandidates[0] ?? ''
  }

  if (result.facialCandidate === '' && result.motionCandidate !== '') {
    if (motionAndFacialList.includes(result.motionCandidate)) {
      result.facialCandidate = result.motionCandidate
    } else if (motionAndFacialList.length > 0) {
      result.facialCandidate = motionAndFacialList[0]
    }
  }

  const titleSource =
    result.motionCandidate !== '' ? result.motionCandidate : result.audioCandidate !== '' ? result.audioCandidate : paths[0] ?? ''
  result.suggestedTitle = titleSource !== '' ? cleanTitleSuffix(fileStem(titleSource)) : '新演出条目'
  return result
}

// Extensions accepted by the whole-window drop target (IsSupportedLibraryFile).
export function isSupportedLibraryFile(path: string): boolean {
  const ext = fileExt(path)
  return ['.vmd', '.gkcamera', ...SUPPORTED_AUDIO_EXTS].includes(ext)
}
