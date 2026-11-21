import {
  BridgeError,
  DEFAULT_PLAYER_STATE,
  defaultPerformanceTiming,
  type AudioProbeInfo,
  type EditEntryInfo,
  type ImportRequest,
  type LibraryEntry,
  type LibraryPositionEntry,
} from '../../protocol'
import { classifyImportFiles } from '../../validation'
import type { MockHandlerContext, MockHandlerMap } from '../context'
import { MOCK_COVER_PNG, probeFromPath } from '../seed'

export function createLibraryHandlers(ctx: MockHandlerContext): MockHandlerMap {
  const { state } = ctx
  return {
    'library.refresh': () => {
      if (state.refreshing) throw new BridgeError('busy', '正在刷新曲库')
      state.refreshing = true
      ctx.emitLibrary()
      state.refreshTimer = setTimeout(() => {
        state.refreshing = false
        ctx.emitLibrary()
      }, 250)
      return { started: true }
    },
    'library.select': (p) => {
      ctx.requireAllowed()
      if (Number(p.revision) !== state.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请刷新后重试')
      const entry = state.entries.find((candidate) => candidate.id === p.key)
      if (!entry) throw new BridgeError('invalid_params', '曲目不存在')
      ctx.loadEntry(entry, Boolean(p.autoplay))
      return { key: entry.id }
    },
    'library.delete': (p) => {
      ctx.requireAllowed()
      if (state.refreshing) throw new BridgeError('busy', '曲库刷新中')
      if (Number(p.revision) !== state.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请取消后重新选择曲目')
      const index = state.entries.findIndex((candidate) => candidate.id === p.key)
      if (index < 0) throw new BridgeError('invalid_params', '曲目不存在')
      state.entries.splice(index, 1)
      state.libraryRevision += 1
      if (state.loaded.motionKey === p.key) {
        state.loaded = { ...state.loaded, motionKey: '', generation: state.loaded.generation + 1 }
        state.player = {
          ...DEFAULT_PLAYER_STATE,
          volume: state.player.volume,
          speed: state.player.speed,
          playMode: state.player.playMode,
        }
        ctx.emit('loaded.changed', { ...state.loaded })
        ctx.emitPlayer()
      }
      ctx.emitLibrary()
      return {}
    },
    'library.openFolder': () => ({}),
    'dialog.pickFile': (p) => {
      ctx.requireCapability('dialog.pickFile')
      const kind = Number(p.kind)
      const multiple = Boolean(p.multiple)
      const dialogId = state.nextDialogId++
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
      state.dialogResults.set(dialogId, { paths: sampleByKind[kind] ?? sampleByKind[0] })
      return { dialogId }
    },
    'dialog.poll': (p) => {
      ctx.requireCapability('dialog.poll')
      const dialogId = Number(p.dialogId)
      const picked = state.dialogResults.get(dialogId)
      state.dialogResults.delete(dialogId)
      if (!picked) return { state: 'cancelled', paths: [], notice: '' }
      return { state: 'accepted', paths: picked.paths, notice: '' }
    },
    'library.probeFiles': (p) => {
      ctx.requireCapability('library.probeFiles')
      const paths = Array.isArray(p.paths) ? (p.paths as string[]) : []
      const probes = paths.filter((item) => /\.(vmd|gkcamera)$/i.test(item)).map(probeFromPath)
      const classification = classifyImportFiles(paths, probes)
      return { classification, probes }
    },
    'library.startEdit': (p) => {
      ctx.requireCapability('library.startEdit')
      if (Number(p.revision) !== state.libraryRevision) throw new BridgeError('stale_revision', '曲库已变化，请刷新后重试')
      const entry = state.entries.find((item) => item.id === p.key)
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
        timing: state.timings.get(entry.id) ?? defaultPerformanceTiming(),
      }
      return result
    },
    'library.probeAudio': (p) => {
      ctx.requireCapability('library.probeAudio')
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
      return info
    },
    'library.getCover': (p) => {
      ctx.requireCapability('library.getCover')
      const key = String(p.key ?? '')
      const dir = String(p.directory ?? '')
      const entry = state.entries.find((item) => (key && item.id === key) || (dir && item.directory === dir))
      const cover = entry ? (state.coverByKey.get(entry.id) ?? (entry.hasCover ? MOCK_COVER_PNG : '')) : ''
      return { hasCover: cover !== '', coverDataUri: cover }
    },
    'library.setCover': (p) => {
      ctx.requireCapability('library.setCover')
      const key = String(p.key ?? '')
      const clear = Boolean(p.clear)
      const coverDataUri = String(p.coverDataUri ?? '')
      const index = state.entries.findIndex((item) => item.id === key)
      if (index < 0) throw new BridgeError('not_found', '曲目不存在')
      if (clear) {
        state.coverByKey.delete(key)
        state.entries[index] = { ...state.entries[index], hasCover: false }
      } else {
        if (!coverDataUri.startsWith('data:image/')) throw new BridgeError('invalid_params', '无效封面数据')
        state.coverByKey.set(key, coverDataUri)
        state.entries[index] = { ...state.entries[index], hasCover: true }
      }
      state.libraryRevision += 1
      ctx.emitLibrary()
      return {
        hasCover: !clear,
        coverDataUri: clear ? '' : coverDataUri,
        revision: state.libraryRevision,
      }
    },
    'library.readImagePreview': (p) => {
      ctx.requireCapability('library.readImagePreview')
      const path = String(p.path ?? '')
      if (!path) throw new BridgeError('invalid_params', '路径不能为空')
      return { dataUri: MOCK_COVER_PNG }
    },
    'screenshot.capabilities': () => {
      ctx.requireCapability('screenshot.capabilities')
      return {
        available: true,
        runtimeReady: true,
        cameraAvailable: true,
        windowWidth: 1280,
        windowHeight: 720,
        notice: '',
      }
    },
    'screenshot.capture': () => {
      ctx.requireCapability('screenshot.capture')
      return {
        dataUri: MOCK_COVER_PNG,
        width: 1280,
        height: 720,
      }
    },
    'library.import': (p) => {
      ctx.requireCapability('library.import')
      ctx.requireAllowed()
      const req = p.request as ImportRequest
      if (!req || !req.title.trim()) throw new BridgeError('invalid_params', '无效的导入请求')
      if (req.editKey && state.loaded.motionKey === req.editKey && (state.player.state === 'playing' || state.player.state === 'paused')) {
        throw new BridgeError('protected', '播放中禁止覆盖当前曲目')
      }
      const applyImport = () => {
        if (state.closed) return
        const isEdit = (req.editKey ?? '') !== ''
        const id = isEdit
          ? req.editKey
          : `${req.title.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-') || 'custom'}-${Date.now().toString(36)}`
        const existingIndex = state.entries.findIndex((item) => item.id === id)
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
          const current = state.entries[existingIndex]
          let nextHasCover = current.hasCover ?? false
          if (req.clearCover) {
            state.coverByKey.delete(id)
            nextHasCover = false
          } else if (customCover) {
            state.coverByKey.set(id, customCover)
            nextHasCover = true
          } else if (req.clearAudio) {
            state.coverByKey.delete(id)
            nextHasCover = false
          } else if (audioEmbeddedCover) {
            state.coverByKey.set(id, MOCK_COVER_PNG)
            nextHasCover = true
          }
          state.entries[existingIndex] = {
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
            state.coverByKey.set(id, customCover || MOCK_COVER_PNG)
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
          state.entries.unshift(created)
        }
        state.timings.set(id, req.timing ?? defaultPerformanceTiming())
        state.libraryRevision += 1
        state.libraryNotice = isEdit ? `已更新曲目：${req.title}` : `已导入曲目：${req.title}`
        if (!isEdit && state.player.state !== 'playing') {
          const targetEntry = state.entries.find((item) => item.id === id)
          if (targetEntry) ctx.loadEntry(targetEntry, false)
        }
        ctx.emit('import.progress', {
          active: false,
          finished: true,
          succeeded: true,
          progress: 1,
          stage: 'DONE',
          statusText: '导入完成',
          errorDetail: '',
          importedKey: id,
        })
        ctx.emitLibrary()
      }
      ctx.emit('import.progress', {
        active: true,
        finished: false,
        succeeded: false,
        progress: 0.25,
        stage: 'READING',
        statusText: '正在解析 VMD 与音频轨道...',
        errorDetail: '',
      })
      const t1 = setTimeout(() => {
        if (!state.closed) {
          ctx.emit('import.progress', {
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
      state.importTimers.push(t1, t2)
      return { started: true }
    },
    'drop.setTarget': (p) => {
      ctx.requireCapability('drop.setTarget')
      state.dropTargetEnabled = Boolean(p.enabled)
      return {}
    },
    'drop.peek': () => {
      ctx.requireCapability('drop.peek')
      return {
        visual: state.dropTargetEnabled ? state.dropPeekState.visual : 'none',
        fileCount: state.dropPeekState.fileCount,
        accepted: state.dropPeekState.accepted,
        hostElevated: state.dropPeekState.hostElevated,
        hostBlocked: state.dropPeekState.hostBlocked,
      }
    },
    'drop.poll': () => {
      ctx.requireCapability('drop.poll')
      const files = state.pendingDropFiles
      state.pendingDropFiles = []
      return { files }
    },
  }
}
