import { IMPORT_SLOT, isSlotPlaceholder, type VmdProbe } from '../../protocol'
import { canUseAsMotion, isSupportedAudioExtension } from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import type { ImportPositionFormState, ImportSlotState } from '../types'

export function createImportHelpers(ctx: StoreContext) {
  const probePaths = async (paths: string[]): Promise<VmdProbe[]> => {
    const targets = paths.filter((path) => /\.(vmd|gkcamera)$/i.test(path) && !ctx.getState().importForm.probes[path])
    if (targets.length === 0 || !ctx.supports('library.probeFiles')) return []
    try {
      const { probes } = await ctx.client().request('library.probeFiles', { paths: targets })
      const form = ctx.getState().importForm
      const merged = { ...form.probes }
      for (const probe of probes) merged[probe.path] = probe
      ctx.set({ importForm: { ...form, probes: merged } })
      return probes
    } catch (error) {
      ctx.reportActionError(error)
      return []
    }
  }

  const probeImportAudio = async (path: string): Promise<void> => {
    if (!path || isSlotPlaceholder(path) || !ctx.supports('library.probeAudio')) return
    try {
      const result = await ctx.client().request('library.probeAudio', { path, includeCover: true })
      const currentAudio = ctx.store.importFormPicks().audio
      if (currentAudio !== path) return
      const form = ctx.getState().importForm
      const nextPatch: Partial<typeof form> = { audioProbe: result }
      if (result.valid) {
        if (form.subtitle.trim() === '' && result.artist.trim() !== '') {
          nextPatch.subtitle = result.artist.trim()
        }
        if (form.mode === 'new' && form.title.trim() === '' && result.title.trim() !== '') {
          nextPatch.title = result.title.trim()
        }
      }
      ctx.set({ importForm: { ...form, ...nextPatch } })
    } catch {
      // Probe failures fall back gracefully without blocking validation.
    }
  }

  return { probePaths, probeImportAudio }
}

export function createImportPositionsSlice(
  ctx: StoreContext,
  helpers: ReturnType<typeof createImportHelpers>,
): SliceBundle<ReturnType<typeof buildActions>> {
  const actions = buildActions()

  function buildActions() {
    return {
      addImportPosition: (): void => {
        const form = ctx.getState().importForm
        if (!form.open) return
        const nextPos: ImportPositionFormState = {
          motion: { candidates: [IMPORT_SLOT.empty], selection: 0 },
          facial: {
            candidates: form.mode === 'edit' ? [IMPORT_SLOT.clear] : [IMPORT_SLOT.autoFacial, IMPORT_SLOT.skip],
            selection: 0,
          },
          x: 0,
          y: 0,
          z: 0,
          yawDegrees: 0,
          preserveExisting: false,
        }
        ctx.set({
          importForm: {
            ...form,
            positions: [...form.positions, nextPos],
          },
        })
      },

      removeImportPosition: (posIndex: number): void => {
        const form = ctx.getState().importForm
        if (posIndex <= 0 || posIndex >= form.positions.length) return
        const removed = form.positions[posIndex]
        const removedMotion = removed.motion.candidates[removed.motion.selection] ?? ''
        const removedFacial = removed.facial.candidates[removed.facial.selection] ?? ''
        const remaining = form.positions.filter((_, idx) => idx !== posIndex)
        const stillUsedPaths = new Set<string>()
        for (const pos of remaining) {
          const m = pos.motion.candidates[pos.motion.selection] ?? ''
          const f = pos.facial.candidates[pos.facial.selection] ?? ''
          if (!isSlotPlaceholder(m)) stillUsedPaths.add(m)
          if (!isSlotPlaceholder(f)) stillUsedPaths.add(f)
        }
        const camSel = form.slots.camera.candidates[form.slots.camera.selection] ?? ''
        if (!isSlotPlaceholder(camSel)) stillUsedPaths.add(camSel)

        const pruneCandidates = (slot: ImportSlotState): ImportSlotState => {
          const selected = slot.candidates[slot.selection] ?? ''
          const nextCandidates = slot.candidates.filter(
            (c) =>
              (c !== removedMotion && c !== removedFacial) ||
              c === selected ||
              stillUsedPaths.has(c) ||
              isSlotPlaceholder(c),
          )
          const nextSelection = Math.max(0, nextCandidates.indexOf(selected))
          return { candidates: nextCandidates, selection: nextSelection }
        }

        const cleanedPositions = remaining.map((pos) => ({
          ...pos,
          motion: pruneCandidates(pos.motion),
          facial: pruneCandidates(pos.facial),
        }))
        const cleanedCamera = pruneCandidates(form.slots.camera)
        ctx.set({
          importForm: {
            ...form,
            positions: cleanedPositions,
            slots: {
              ...form.slots,
              motion: cleanedPositions[0].motion,
              facial: cleanedPositions[0].facial,
              camera: cleanedCamera,
            },
          },
        })
      },

      setImportPositionSelection: (posIndex: number, kind: 'motion' | 'facial', selection: number): void => {
        const form = ctx.getState().importForm
        if (posIndex < 0 || posIndex >= form.positions.length) return
        const positions = form.positions.map((pos, idx) => {
          if (idx !== posIndex) return pos
          const chosen = pos[kind].candidates[selection] ?? ''
          return {
            ...pos,
            [kind]: { ...pos[kind], selection },
            ...(kind === 'motion' && chosen !== IMPORT_SLOT.keepMotion ? { preserveExisting: false } : {}),
          }
        })
        ctx.set({
          importForm: {
            ...form,
            positions,
            slots: {
              ...form.slots,
              motion: positions[0].motion,
              facial: positions[0].facial,
            },
          },
        })
      },

      setImportPositionPlacement: (
        posIndex: number,
        placement: { x: number; y: number; z: number; yawDegrees: number },
      ): void => {
        const form = ctx.getState().importForm
        if (posIndex <= 0 || posIndex >= form.positions.length) return
        const finite = (v: number) => (Number.isFinite(v) ? v : 0)
        const positions = form.positions.map((pos, idx) =>
          idx === posIndex
            ? {
                ...pos,
                x: finite(placement.x),
                y: finite(placement.y),
                z: finite(placement.z),
                yawDegrees: finite(placement.yawDegrees),
              }
            : pos,
        )
        ctx.set({ importForm: { ...form, positions } })
      },

      importPickPositionFile: async (posIndex: number, kind: 'motion' | 'facial', file: string): Promise<void> => {
        await helpers.probePaths([file])
        const form = ctx.getState().importForm
        if (posIndex < 0 || posIndex >= form.positions.length) return
        const positions = form.positions.map((pos, idx) => {
          if (idx !== posIndex) return pos
          const candidates = [...pos[kind].candidates]
          let selection = candidates.indexOf(file)
          if (selection < 0) {
            candidates.push(file)
            selection = candidates.length - 1
          }
          return {
            ...pos,
            [kind]: { candidates, selection },
            ...(kind === 'motion' ? { preserveExisting: false } : {}),
          }
        })
        ctx.set({
          importForm: {
            ...form,
            positions,
            slots: {
              ...form.slots,
              motion: positions[0].motion,
              facial: positions[0].facial,
            },
          },
        })
      },

      importAppendFiles: async (files: string[]): Promise<void> => {
        await helpers.probePaths(files)
        const next = ctx.getState().importForm
        const probeMap = next.probes
        const positions = next.positions.map((pos) => ({
          ...pos,
          motion: { ...pos.motion, candidates: [...pos.motion.candidates] },
          facial: { ...pos.facial, candidates: [...pos.facial.candidates] },
        }))
        const camera = { ...next.slots.camera, candidates: [...next.slots.camera.candidates] }
        const audio = { ...next.slots.audio, candidates: [...next.slots.audio.candidates] }
        const appendUnique = (list: string[], item: string) => {
          if (!list.includes(item)) list.push(item)
        }
        const insertBeforeSkip = (list: string[], item: string): number => {
          const existing = list.indexOf(item)
          if (existing >= 0) return existing
          const skipIdx = list.indexOf(IMPORT_SLOT.skip)
          if (skipIdx >= 0) {
            list.splice(skipIdx, 0, item)
            return skipIdx
          }
          list.push(item)
          return list.length - 1
        }

        let appendedAudio: string | null = null
        for (const file of files) {
          const dot = file.lastIndexOf('.')
          const ext = dot >= 0 ? file.slice(dot).toLowerCase() : ''
          if (isSupportedAudioExtension(file)) {
            const idx = insertBeforeSkip(audio.candidates, file)
            const currentAudio = audio.candidates[audio.selection] ?? ''
            if (isSlotPlaceholder(currentAudio) && currentAudio !== IMPORT_SLOT.keepTrack) {
              audio.selection = idx
              appendedAudio = file
            }
          } else if (ext === '.gkcamera') {
            appendUnique(camera.candidates, file)
            const currentCam = camera.candidates[camera.selection] ?? ''
            if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
              camera.selection = camera.candidates.indexOf(file)
            }
          } else if (ext === '.vmd') {
            const probe = probeMap[file]
            if (probe && !canUseAsMotion(probe)) {
              if (probe.cameraFrames > 0) {
                appendUnique(camera.candidates, file)
                const currentCam = camera.candidates[camera.selection] ?? ''
                if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
                  camera.selection = camera.candidates.indexOf(file)
                }
              }
              if (probe.morphFrames > 0 || probe.eyeBoneFrames > 0) {
                for (let p = 0; p < positions.length; p += 1) {
                  const idx = insertBeforeSkip(positions[p].facial.candidates, file)
                  const curFacial = positions[p].facial.candidates[positions[p].facial.selection] ?? ''
                  if (p === 0 && isSlotPlaceholder(curFacial) && curFacial !== IMPORT_SLOT.keepTrack) {
                    positions[p].facial.selection = idx
                  }
                }
              }
            } else {
              const emptyIdx = positions.findIndex((pos) => {
                const picked = pos.motion.candidates[pos.motion.selection] ?? ''
                return picked === IMPORT_SLOT.empty
              })
              if (emptyIdx >= 0) {
                appendUnique(positions[emptyIdx].motion.candidates, file)
                positions[emptyIdx].motion.selection = positions[emptyIdx].motion.candidates.indexOf(file)
                positions[emptyIdx].preserveExisting = false
              } else {
                positions.push({
                  motion: { candidates: [IMPORT_SLOT.empty, file], selection: 1 },
                  facial: {
                    candidates: next.mode === 'edit' ? [IMPORT_SLOT.clear] : [IMPORT_SLOT.autoFacial, IMPORT_SLOT.skip],
                    selection: 0,
                  },
                  x: 0,
                  y: 0,
                  z: 0,
                  yawDegrees: 0,
                  preserveExisting: false,
                })
              }
            }
          }
        }

        ctx.set({
          importForm: {
            ...next,
            positions,
            slots: {
              motion: positions[0].motion,
              facial: positions[0].facial,
              camera,
              audio,
            },
            ...(appendedAudio ? { audioProbe: null } : {}),
          },
        })
        if (appendedAudio) {
          void helpers.probeImportAudio(appendedAudio)
        }
      },

      moveImportPosition: (posIndex: number, direction: -1 | 1): void => {
        const next = ctx.getState().importForm
        const targetIndex = posIndex + direction
        if (
          posIndex < 0 ||
          posIndex >= next.positions.length ||
          targetIndex < 0 ||
          targetIndex >= next.positions.length
        ) {
          return
        }
        const positions = [...next.positions]
        const current = positions[posIndex]
        const target = positions[targetIndex]
        // Swap only the motion/facial track pair & preserveExisting/sourceSlot; keep x/y/z/yaw anchored to each slot index.
        positions[posIndex] = {
          ...current,
          motion: target.motion,
          facial: target.facial,
          preserveExisting: target.preserveExisting,
          sourceSlot: target.sourceSlot,
        }
        positions[targetIndex] = {
          ...target,
          motion: current.motion,
          facial: current.facial,
          preserveExisting: current.preserveExisting,
          sourceSlot: current.sourceSlot,
        }
        ctx.set({
          importForm: {
            ...next,
            positions,
            slots: {
              ...next.slots,
              motion: positions[0].motion,
              facial: positions[0].facial,
            },
          },
        })
      },
    }
  }

  return { actions }
}

export type ImportPositionsActions = ReturnType<typeof createImportPositionsSlice>['actions']
