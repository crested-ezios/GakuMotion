import {
  DEFAULT_FACIAL_WEIGHTS,
  type LibraryEntry,
  type SceneActor,
  type StageFormation,
  type StageNode,
  type StageSlotOffset,
  type VmdProbe,
} from '../protocol'

export const MOCK_COVER_PNG =
  'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='

export const MOCK_AUDIO_EFFECT_PRESETS = [
  { id: 'flat', name: '原声', category: 'flat', description: '直通输出（保留原始母带音色）' },
  { id: 'concert_arena', name: '演唱会现场', category: 'live', description: '万人巨蛋空间混响与氛围临场感' },
  { id: 'livehouse', name: 'LiveHouse现场', category: 'live', description: '近距离中小场地节奏冲击与清晰人声' },
  { id: 'mega_bass', name: '极重低音', category: 'bass', description: 'Sub-bass 与低频弹性强化' },
  { id: 'vocal_clear', name: '清澈人声', category: 'vocal', description: '突出偶像人声泛音与齿音透明度' },
  { id: 'symphony_hall', name: '音乐厅', category: 'space', description: '古典音乐厅自然早期反射' },
  { id: 'surround_3d', name: '3D环绕', category: 'space', description: 'Haas + 立体声宽度增强' },
  { id: 'pure_acg', name: '纯净ACG', category: 'eq', description: '二次元流行乐高频通透调音' },
]

export const MOCK_KNOWN_IDOL_CODES = new Set([
  'hski', 'ttmr', 'fktn', 'amao', 'kllj', 'ssmk', 'hume', 'kcna',
  'jsna', 'shro', 'hrnm', 'atbm', 'hmsz', 'hkne', 'krnh', 'nasr',
])

export function seedEntries(): LibraryEntry[] {
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

export function buildMockFormation(
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

export function seedStageNodes(): StageNode[] {
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

export function probeFromPath(filePath: string): VmdProbe {
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
