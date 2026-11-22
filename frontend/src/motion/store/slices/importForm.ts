import {
  BridgeError,
  IMPORT_SLOT,
  defaultImportRequest,
  defaultSlotPerformanceTiming,
  isSlotPlaceholder,
  type ImportRequest,
  type TrackTiming,
} from '../../protocol'
import {
  canUseAsFacial,
  canUseAsMotion,
  classifyImportFiles,
  isSupportedAudioExtension,
  validateImportForm,
} from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeEditEntryInfo } from '../normalizersCore'
import {
  emptyImportForm,
  emptyImportUi,
  type ImportCoverMode,
  type ImportPositionFormState,
  type ImportSlotKey,
  type ImportSlotState,
} from '../types'
import {
  recomputeAutoSlotOffsets,
  sanitizeTrackTiming,
  type createImportHelpers,
} from './importPositions'
import type { ImportProgressHooks } from './importProgress'

export function createImportFormSlice(
  ctx: StoreContext,
  helpers: ReturnType<typeof createImportHelpers>,
  progressHooks: ImportProgressHooks,
): SliceBundle<ReturnType<typeof buildActions>> & {
  maybeAutoSubmitAfterStop: () => void
} {
  const actions = buildActions()

  const maybeAutoSubmitAfterStop = (): void => {
    const form = ctx.getState().importForm
    if (!form.open || !form.pendingStopAndSave) return
    if (actions.isEditingActiveTrack()) return
    if (actions.importFormError()) return
    void actions.submitImport()
  }

  function buildActions() {
    return {
      openImportNew: async (files: string[]): Promise<void> => {
        const form = emptyImportForm()
        form.open = true
        form.mode = 'new'
        form.positions = [
          {
            motion: { candidates: [IMPORT_SLOT.empty], selection: 0 },
            facial: { candidates: [IMPORT_SLOT.autoFacial], selection: 0 },
            timing: defaultSlotPerformanceTiming(),
            x: 0,
            y: 0,
            z: 0,
            yawDegrees: 0,
            preserveExisting: false,
          },
        ]
        form.slots.motion = form.positions[0].motion
        form.slots.facial = form.positions[0].facial
        form.slots.camera.candidates = [IMPORT_SLOT.empty]
        form.slots.audio.candidates = [IMPORT_SLOT.autoAudio]
        ctx.set({ importForm: form })
        await helpers.probePaths(files)
        const probeMap = ctx.getState().importForm.probes
        const probes = Object.values(probeMap)
        const classification = classifyImportFiles(files, probes)

        const appendUnique = (list: string[], item: string) => {
          if (!list.includes(item)) list.push(item)
        }
        const findIndex = (list: string[], path: string): number => {
          if (path === '') return 0
          const index = list.indexOf(path)
          return index > 0 ? index : 0
        }

        const vmdFiles: string[] = []
        const cameraFiles: string[] = []
        const facialOnlyFiles: string[] = []
        const audioFiles: string[] = []
        for (const file of files) {
          const dot = file.lastIndexOf('.')
          const ext = dot >= 0 ? file.slice(dot).toLowerCase() : ''
          if (ext === '.vmd') {
            appendUnique(vmdFiles, file)
            const probe = probeMap[file]
            if (!probe) {
              appendUnique(cameraFiles, file)
            } else {
              if (probe.cameraFrames > 0) appendUnique(cameraFiles, file)
              if (probe.tendency === 'facialOnly') appendUnique(facialOnlyFiles, file)
            }
          } else if (ext === '.gkcamera') {
            appendUnique(cameraFiles, file)
          } else if (isSupportedAudioExtension(file)) {
            appendUnique(audioFiles, file)
          }
        }

        const motionCandidates = classification.motionCandidates
        const motionPool = [
          ...motionCandidates,
          ...vmdFiles.filter((f) => (!probeMap[f] || canUseAsMotion(probeMap[f])) && !motionCandidates.includes(f)),
        ]
        const facialPool: string[] = []
        for (const f of facialOnlyFiles) appendUnique(facialPool, f)
        for (const f of motionPool) {
          const probe = probeMap[f]
          if (!probe || canUseAsFacial(probe)) appendUnique(facialPool, f)
        }
        for (const f of vmdFiles) {
          const probe = probeMap[f]
          if (!probe || canUseAsFacial(probe)) appendUnique(facialPool, f)
        }

        let positions: ImportPositionFormState[]
        if (motionCandidates.length >= 2) {
          const separateFacial =
            classification.facialCandidate !== '' && probeMap[classification.facialCandidate]?.tendency === 'facialOnly'
              ? classification.facialCandidate
              : ''
          positions = motionCandidates.map((motionPath, idx) => {
            const motionCands = [IMPORT_SLOT.empty, ...motionPool]
            const facialCands = [IMPORT_SLOT.autoFacial, ...facialPool, IMPORT_SLOT.skip]
            const facialSel = idx === 0 && separateFacial ? findIndex(facialCands, separateFacial) : 0
            return {
              motion: { candidates: motionCands, selection: findIndex(motionCands, motionPath) },
              facial: { candidates: facialCands, selection: facialSel },
              timing: defaultSlotPerformanceTiming(),
              x: 0,
              y: 0,
              z: 0,
              yawDegrees: 0,
              preserveExisting: false,
            }
          })
        } else {
          const motionCands = [IMPORT_SLOT.empty, ...motionPool]
          const facialCands = [IMPORT_SLOT.autoFacial, ...facialPool, IMPORT_SLOT.skip]
          positions = [
            {
              motion: { candidates: motionCands, selection: findIndex(motionCands, classification.motionCandidate) },
              facial: { candidates: facialCands, selection: findIndex(facialCands, classification.facialCandidate) },
              timing: defaultSlotPerformanceTiming(),
              x: 0,
              y: 0,
              z: 0,
              yawDegrees: 0,
              preserveExisting: false,
            },
          ]
        }
        positions = recomputeAutoSlotOffsets(positions, probeMap, 'new')

        const camCands = [
          IMPORT_SLOT.empty,
          ...(motionCandidates.length >= 2 ? cameraFiles : [...cameraFiles, ...vmdFiles.filter((f) => !cameraFiles.includes(f))]),
        ]
        const audioCands = [IMPORT_SLOT.autoAudio, ...audioFiles, IMPORT_SLOT.skip]
        const slots = {
          motion: positions[0].motion,
          facial: positions[0].facial,
          camera: { candidates: camCands, selection: findIndex(camCands, classification.cameraCandidate) },
          audio: { candidates: audioCands, selection: findIndex(audioCands, classification.audioCandidate) },
        }
        ctx.set({
          importForm: {
            ...ctx.getState().importForm,
            title: classification.suggestedTitle,
            positions,
            slots,
            unsupportedAudio: classification.unsupportedAudioCount,
            audioProbe: null,
          },
        })
        if (classification.audioCandidate) {
          void helpers.probeImportAudio(classification.audioCandidate)
        }
      },

      openImportEdit: async (key: string): Promise<void> => {
        if (!ctx.supports('library.startEdit')) return
        const state = ctx.getState()
        const revision = state.library.revision
        try {
          const raw = await ctx.client().request('library.startEdit', { key, revision })
          const info = normalizeEditEntryInfo(raw, key)
          const form = emptyImportForm()
          form.open = true
          form.mode = 'edit'
          form.editKey = info.key
          form.editRevision = revision
          form.editInfo = info
          form.title = info.title
          form.subtitle = info.subtitle ?? ''
          form.timing = info.timing
          const slotTimingAt = (idx: number) => {
            const t = info.positions?.[idx]?.timing
            return {
              motion: { ...(t?.motion ?? info.timing.motion) },
              facial: { ...(t?.facial ?? info.timing.facial) },
            }
          }
          if (info.isGroup && info.positions && info.positions.length >= 2) {
            form.positions = info.positions.map((pos, idx) => ({
              motion: { candidates: [IMPORT_SLOT.keepMotion], selection: 0 },
              facial: {
                candidates: pos.hasFacial ? [IMPORT_SLOT.clear, IMPORT_SLOT.keepTrack] : [IMPORT_SLOT.clear],
                selection: pos.hasFacial ? 1 : 0,
              },
              timing: slotTimingAt(idx),
              x: pos.x,
              y: pos.y,
              z: pos.z,
              yawDegrees: pos.yawDegrees,
              preserveExisting: true,
              sourceSlot: pos.slot,
              ...(idx >= 1 ? { placementEdited: true } : {}),
            }))
          } else {
            form.positions = [
              {
                motion: { candidates: [IMPORT_SLOT.keepMotion], selection: 0 },
                facial: {
                  candidates: info.hasFacial ? [IMPORT_SLOT.clear, IMPORT_SLOT.keepTrack] : [IMPORT_SLOT.clear],
                  selection: info.hasFacial ? 1 : 0,
                },
                timing: slotTimingAt(0),
                x: 0,
                y: 0,
                z: 0,
                yawDegrees: 0,
                preserveExisting: true,
                sourceSlot: 1,
              },
            ]
          }
          form.timing = {
            ...info.timing,
            motion: { ...form.positions[0].timing.motion },
            facial: { ...form.positions[0].timing.facial },
          }
          form.slots.motion = form.positions[0].motion
          form.slots.facial = form.positions[0].facial
          form.slots.camera.candidates = [IMPORT_SLOT.clear]
          form.slots.audio.candidates = [IMPORT_SLOT.clear]
          if (info.hasCamera) {
            form.slots.camera.candidates.push(IMPORT_SLOT.keepTrack)
            form.slots.camera.selection = 1
          }
          if (info.hasAudio) {
            form.slots.audio.candidates.push(IMPORT_SLOT.keepTrack)
            form.slots.audio.selection = 1
          }
          const existingCover = ctx.getState().coverCache[info.key] ?? ''
          if (existingCover) {
            form.coverDataUri = existingCover
          }
          ctx.set({ importForm: form, actionError: null })
          if ((info.hasCover || info.hasAudio) && !existingCover) {
            void ctx.store.ensureTrackCover(info.key).then(() => {
              const st = ctx.getState()
              const cur = st.importForm
              if (cur.open && cur.mode === 'edit' && cur.editKey === info.key && cur.coverMode === 'auto') {
                const loadedCover = st.coverCache[info.key] ?? ''
                if (loadedCover) ctx.set({ importForm: { ...cur, coverDataUri: loadedCover } })
              }
            })
          }
        } catch (error) {
          if (error instanceof BridgeError && error.code === 'stale_revision') {
            ctx.set({ actionError: '曲库已变化，请刷新后重试。' })
          } else {
            ctx.reportActionError(error)
          }
        }
      },

      closeImportForm: (): void => {
        ctx.set({ importForm: emptyImportForm() })
      },

      importPickSlotFile: async (slot: ImportSlotKey, file: string): Promise<void> => {
        if (slot === 'motion' || slot === 'facial') {
          await ctx.store.importPickPositionFile(0, slot, file)
          return
        }
        await helpers.probePaths([file])
        const form = ctx.getState().importForm
        const slotState = form.slots[slot]
        const candidates = [...slotState.candidates]
        let selection = candidates.indexOf(file)
        if (selection < 0) {
          candidates.push(file)
          selection = candidates.length - 1
        }
        ctx.set({
          importForm: {
            ...form,
            slots: { ...form.slots, [slot]: { candidates, selection } },
            ...(slot === 'audio' ? { audioProbe: null } : {}),
          },
        })
        if (slot === 'audio' && !isSlotPlaceholder(file)) {
          void helpers.probeImportAudio(file)
        }
      },

      setImportSelection: (slot: ImportSlotKey, selection: number): void => {
        if (slot === 'motion' || slot === 'facial') {
          ctx.store.setImportPositionSelection(0, slot, selection)
          return
        }
        const form = ctx.getState().importForm
        const picked = form.slots[slot].candidates[selection] ?? ''
        ctx.set({
          importForm: {
            ...form,
            slots: { ...form.slots, [slot]: { ...form.slots[slot], selection } },
            ...(slot === 'audio' ? { audioProbe: null } : {}),
          },
        })
        if (slot === 'audio' && !isSlotPlaceholder(picked)) {
          void helpers.probeImportAudio(picked)
        }
      },

      probeImportAudio: helpers.probeImportAudio,

      setImportCover: (coverMode: ImportCoverMode, coverDataUri = ''): void => {
        ctx.set({
          importForm: {
            ...ctx.getState().importForm,
            coverMode,
            coverDataUri: coverMode === 'clear' ? '' : coverDataUri,
          },
        })
      },

      setImportTitle: (title: string): void => {
        ctx.set({ importForm: { ...ctx.getState().importForm, title } })
      },

      setImportSubtitle: (subtitle: string): void => {
        ctx.set({ importForm: { ...ctx.getState().importForm, subtitle } })
      },

      setImportSourceFps: (sourceFps: number): void => {
        const normalized = sourceFps === 25 || sourceFps === 30 || sourceFps === 60 ? sourceFps : 0
        ctx.set({ importForm: { ...ctx.getState().importForm, sourceFps: normalized } })
      },

      setImportOutputFps: (fps: number): void => {
        const normalized = fps === 30 || fps === 60 ? fps : 0
        ctx.set({ importForm: { ...ctx.getState().importForm, fps: normalized } })
      },

      setImportTiming: (slot: ImportSlotKey, patch: Partial<TrackTiming>): void => {
        const form = ctx.getState().importForm
        const nextTrack = sanitizeTrackTiming(form.timing[slot], patch)
        const timing = { ...form.timing, [slot]: nextTrack }
        if ((slot === 'motion' || slot === 'facial') && form.positions.length > 0) {
          const positions = form.positions.map((pos, idx) =>
            idx === 0
              ? { ...pos, timing: { ...pos.timing, [slot]: { ...nextTrack } } }
              : pos,
          )
          ctx.set({ importForm: { ...form, timing, positions } })
          return
        }
        ctx.set({ importForm: { ...form, timing } })
      },

      importFormPicks: () => {
        const form = ctx.getState().importForm
        const pickSlot = (slotState: ImportSlotState): string => slotState.candidates[slotState.selection] ?? ''
        const posPicks = form.positions.map((pos) => ({
          motion: pickSlot(pos.motion),
          facial: pickSlot(pos.facial),
          preserveExisting: pos.preserveExisting,
        }))
        return {
          isEditMode: form.mode === 'edit',
          motion: posPicks[0]?.motion ?? '',
          facial: posPicks[0]?.facial ?? '',
          camera: pickSlot(form.slots.camera),
          audio: pickSlot(form.slots.audio),
          title: form.title,
          positions: posPicks,
        }
      },

      importFormError: (): string | null => {
        const state = ctx.getState()
        const form = state.importForm
        const editMissing =
          form.mode === 'edit' && !state.library.entries.some((entry) => entry.id === form.editKey)
        return validateImportForm(
          actions.importFormPicks(),
          (path) => form.probes[path] ?? null,
          state.importProgress?.active === true,
          editMissing,
        )
      },

      isEditingActiveTrack: (): boolean => {
        const { importForm: form, loaded, player } = ctx.getState()
        return (
          form.mode === 'edit' &&
          form.editKey !== '' &&
          loaded.motionKey === form.editKey &&
          (player.state === 'playing' || player.state === 'paused')
        )
      },

      buildImportRequest: (): ImportRequest => {
        const form = ctx.getState().importForm
        const picks = actions.importFormPicks()
        const request = defaultImportRequest()
        request.title = form.title.trim() !== '' ? form.title : '未命名演出'
        request.subtitle = form.subtitle.trim()
        request.fps = form.fps
        request.sourceFps = form.sourceFps
        request.rebakeOutput = form.mode === 'edit' && form.fps > 0
        const slot0Timing = form.positions[0]?.timing ?? {
          motion: form.timing.motion,
          facial: form.timing.facial,
        }
        request.timing = {
          ...form.timing,
          motion: { ...slot0Timing.motion },
          facial: { ...slot0Timing.facial },
        }
        const isDisk = (path: string) => !isSlotPlaceholder(path)
        const isGroup = form.positions.length >= 2
        request.isGroup = isGroup
        const includePositions =
          isGroup || (form.mode === 'edit' && Boolean(form.editInfo?.isGroup) && form.positions.length === 1)

        if (includePositions) {
          request.positions = form.positions.map((pos, idx) => {
            const mPick = pos.motion.candidates[pos.motion.selection] ?? ''
            const fPick = pos.facial.candidates[pos.facial.selection] ?? ''
            const preserveExisting = form.mode === 'edit' && pos.preserveExisting && isSlotPlaceholder(mPick)
            const disableMorph = fPick === IMPORT_SLOT.clear || fPick === IMPORT_SLOT.skip
            return {
              slot: idx + 1,
              ...(preserveExisting && pos.sourceSlot && pos.sourceSlot > 0 ? { sourceSlot: pos.sourceSlot } : {}),
              ...(isDisk(mPick) ? { motionFile: mPick, motionVmd: mPick } : {}),
              ...(isDisk(fPick) ? { facialFile: fPick, morphVmd: fPick } : {}),
              disableMorph,
              preserveExisting,
              hasExplicitPlacement: Boolean(pos.placementEdited),
              timing: {
                motion: { ...pos.timing.motion },
                facial: { ...pos.timing.facial },
              },
              x: pos.x,
              y: pos.y,
              z: pos.z,
              yawDegrees: pos.yawDegrees,
            }
          })
        }

        if (form.mode === 'edit') {
          request.editKey = form.editKey
          if (isDisk(picks.motion)) request.motionFile = picks.motion
          if (picks.facial === IMPORT_SLOT.clear || picks.facial === IMPORT_SLOT.empty || picks.facial === IMPORT_SLOT.skip) {
            request.disableMorph = true
          } else if (isDisk(picks.facial)) {
            request.facialFile = picks.facial
          }
          if (picks.camera === IMPORT_SLOT.clear || picks.camera === IMPORT_SLOT.empty) {
            request.clearCamera = true
          } else if (isDisk(picks.camera)) {
            request.cameraFile = picks.camera
          }
          if (picks.audio === IMPORT_SLOT.clear || picks.audio === IMPORT_SLOT.empty || picks.audio === IMPORT_SLOT.skip) {
            request.clearAudio = true
          } else if (isDisk(picks.audio)) {
            request.audioFile = picks.audio
          }
        } else {
          request.motionFile = picks.motion
          if (picks.facial === IMPORT_SLOT.skip) request.disableMorph = true
          else if (isDisk(picks.facial)) request.facialFile = picks.facial
          if (isDisk(picks.camera)) request.cameraFile = picks.camera
          if (picks.audio === IMPORT_SLOT.skip) request.clearAudio = true
          else if (isDisk(picks.audio)) request.audioFile = picks.audio
        }
        if (form.coverMode === 'clear') {
          request.clearCover = true
        } else if (form.coverMode === 'custom' && form.coverDataUri) {
          request.coverDataUri = form.coverDataUri
        } else if (form.mode === 'new' && form.audioProbe?.hasCover && form.audioProbe.coverDataUri) {
          request.coverDataUri = form.audioProbe.coverDataUri
        }
        return request
      },

      submitImport: async (): Promise<void> => {
        if (actions.importFormError()) return
        const state = ctx.getState()
        if (actions.isEditingActiveTrack()) {
          ctx.set({
            importForm: {
              ...state.importForm,
              activeTrackWarning: '当前曲目正在播放，无法替换其资源文件。请停止播放后重试。你的修改尚未丢失。',
            },
          })
          return
        }
        const formSnapshot = state.importForm
        const request = actions.buildImportRequest()
        progressHooks.setPendingOutcome({
          editKey: formSnapshot.mode === 'edit' ? formSnapshot.editKey : '',
          existingIds: new Set(state.library.entries.map((entry) => entry.id)),
          coverMode: formSnapshot.coverMode,
          coverDataUri: request.coverDataUri ?? '',
          succeededKey: null,
        })
        try {
          await ctx.client().request('library.import', { request })
        } catch (error) {
          progressHooks.setPendingOutcome(null)
          ctx.reportActionError(error)
          return
        }
        const now = Date.now()
        ctx.set({
          importForm: emptyImportForm(),
          importUi: {
            ...emptyImportUi(),
            visible: true,
            awaitingStart: true,
            startTime: now,
            minDisplayUntil: now + 550,
            displayStatus: '正在启动导入流程...',
            lastProgress: ctx.getState().importProgress,
          },
          actionError: null,
        })
        progressHooks.startImportUiTimer()
      },

      stopPlaybackAndSave: async (): Promise<void> => {
        if (!actions.isEditingActiveTrack()) return
        ctx.set({ importForm: { ...ctx.getState().importForm, pendingStopAndSave: true } })
        try {
          await ctx.client().request('player.stop')
        } catch (error) {
          ctx.set({ importForm: { ...ctx.getState().importForm, pendingStopAndSave: false } })
          ctx.reportActionError(error)
          return
        }
        maybeAutoSubmitAfterStop()
      },
    }
  }

  return {
    actions,
    maybeAutoSubmitAfterStop,
  }
}

export type ImportFormActions = ReturnType<typeof createImportFormSlice>['actions']
