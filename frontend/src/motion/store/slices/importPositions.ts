import {
  IMPORT_SLOT,
  defaultSlotPerformanceTiming,
  isSlotPlaceholder,
  type TrackTiming,
  type VmdProbe,
} from '../../protocol'
import { canUseAsFacial, canUseAsMotion, isSupportedAudioExtension } from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeVmdProbe } from '../normalizersCore'
import type { ImportPositionFormState, ImportSlotState } from '../types'

const round4 = (value: number): number => {
  const rounded = Math.round(value * 10000) / 10000
  return Object.is(rounded, -0) ? 0 : rounded
}

export function sanitizeTrackTiming(prev: TrackTiming, patch: Partial<TrackTiming>): TrackTiming {
  const nextSpeed = patch.speed !== undefined ? patch.speed : prev.speed
  const nextOffset = patch.offsetSeconds !== undefined ? patch.offsetSeconds : prev.offsetSeconds
  const fallbackSpeed = Number.isFinite(prev.speed) && prev.speed >= 0 ? prev.speed : 1
  const fallbackOffset = Number.isFinite(prev.offsetSeconds) ? prev.offsetSeconds : 0
  return {
    speed: Number.isFinite(nextSpeed) ? Math.max(0, nextSpeed) : fallbackSpeed,
    offsetSeconds: Number.isFinite(nextOffset) ? nextOffset : fallbackOffset,
  }
}

export function insertCandidate(list: string[], item: string, beforeSkip = false): number {
  const existing = list.indexOf(item)
  if (existing >= 0) return existing
  if (beforeSkip) {
    const skipIdx = list.indexOf(IMPORT_SLOT.skip)
    if (skipIdx >= 0) {
      list.splice(skipIdx, 0, item)
      return skipIdx
    }
  }
  list.push(item)
  return list.length - 1
}

function isVmdPath(path: string): boolean {
  return /\.vmd$/i.test(path)
}

function collectSharedPositionPools(
  positions: ImportPositionFormState[],
  probes: Record<string, VmdProbe>,
): { motionPool: string[]; facialPool: string[] } {
  const motionPool: string[] = []
  const facialPool: string[] = []
  const pushUnique = (list: string[], path: string) => {
    if (!isSlotPlaceholder(path) && !list.includes(path)) list.push(path)
  }

  for (const pos of positions) {
    for (const candidate of pos.motion.candidates) {
      const probe = probes[candidate]
      if (!probe || canUseAsMotion(probe)) pushUnique(motionPool, candidate)
    }
    for (const candidate of pos.facial.candidates) {
      const probe = probes[candidate]
      if (!probe || canUseAsFacial(probe)) pushUnique(facialPool, candidate)
    }
  }

  for (const [path, probe] of Object.entries(probes)) {
    if (!isVmdPath(path)) continue
    if (canUseAsMotion(probe)) pushUnique(motionPool, path)
    if (canUseAsFacial(probe)) pushUnique(facialPool, path)
  }

  return { motionPool, facialPool }
}

function syncSlotCandidates(slot: ImportSlotState, pool: string[], beforeSkip: boolean): ImportSlotState {
  const selected = slot.candidates[slot.selection] ?? ''
  const candidates = [...slot.candidates]
  for (const item of pool) {
    insertCandidate(candidates, item, beforeSkip)
  }
  const selection = selected !== '' ? candidates.indexOf(selected) : 0
  return {
    candidates,
    selection: selection >= 0 ? selection : 0,
  }
}

export function syncSharedPositionPools(
  positions: ImportPositionFormState[],
  probes: Record<string, VmdProbe>,
): ImportPositionFormState[] {
  const { motionPool, facialPool } = collectSharedPositionPools(positions, probes)
  return positions.map((pos) => ({
    ...pos,
    motion: syncSlotCandidates(pos.motion, motionPool, false),
    facial: syncSlotCandidates(pos.facial, facialPool, true),
  }))
}

export function recomputeAutoSlotOffsets(
  positions: ImportPositionFormState[],
  probes: Record<string, VmdProbe>,
  _mode: 'new' | 'edit',
): ImportPositionFormState[] {
  if (positions.length === 0) return positions
  const slot0Path = positions[0].motion.candidates[positions[0].motion.selection] ?? ''
  const probe0 = !isSlotPlaceholder(slot0Path) ? probes[slot0Path] : undefined
  const hasOrigin0 = Boolean(probe0?.hasRootOrigin && probe0.rootOrigin)

  return positions.map((pos, idx) => {
    if (idx === 0) {
      if (pos.x === 0 && pos.y === 0 && pos.z === 0 && pos.yawDegrees === 0) return pos
      return { ...pos, x: 0, y: 0, z: 0, yawDegrees: 0 }
    }
    if (pos.placementEdited || !hasOrigin0 || !probe0?.rootOrigin) return pos
    const slotKPath = pos.motion.candidates[pos.motion.selection] ?? ''
    const probeK = !isSlotPlaceholder(slotKPath) ? probes[slotKPath] : undefined
    if (!probeK?.hasRootOrigin || !probeK.rootOrigin) return pos

    const x = round4(probeK.rootOrigin.x - probe0.rootOrigin.x)
    const z = round4(probeK.rootOrigin.z - probe0.rootOrigin.z)
    const dy = probeK.rootOrigin.y - probe0.rootOrigin.y
    const y = Math.abs(dy) < 0.02 ? 0 : round4(dy)
    if (pos.x === x && pos.y === y && pos.z === z) return pos
    return { ...pos, x, y, z }
  })
}

export function createImportHelpers(ctx: StoreContext) {
  const probePaths = async (paths: string[]): Promise<VmdProbe[]> => {
    const targets = paths.filter((path) => /\.(vmd|gkcamera)$/i.test(path) && !ctx.getState().importForm.probes[path])
    if (targets.length === 0 || !ctx.supports('library.probeFiles')) return []
    try {
      const { probes } = await ctx.client().request('library.probeFiles', { paths: targets })
      const form = ctx.getState().importForm
      const merged = { ...form.probes }
      const normalizedProbes: VmdProbe[] = []
      for (const rawProbe of probes) {
        const probe = normalizeVmdProbe(rawProbe)
        if (probe.path) merged[probe.path] = probe
        normalizedProbes.push(probe)
      }
      ctx.set({ importForm: { ...form, probes: merged } })
      return normalizedProbes
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
        const { motionPool, facialPool } = collectSharedPositionPools(form.positions, form.probes)
        const nextPos: ImportPositionFormState = {
          motion: { candidates: [IMPORT_SLOT.empty, ...motionPool], selection: 0 },
          facial: {
            candidates:
              form.mode === 'edit'
                ? [IMPORT_SLOT.clear, ...facialPool]
                : [IMPORT_SLOT.autoFacial, ...facialPool, IMPORT_SLOT.skip],
            selection: 0,
          },
          timing: defaultSlotPerformanceTiming(),
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
        const remaining = recomputeAutoSlotOffsets(
          form.positions.filter((_, idx) => idx !== posIndex),
          form.probes,
          form.mode,
        )
        ctx.set({
          importForm: {
            ...form,
            positions: remaining,
            slots: {
              ...form.slots,
              motion: remaining[0].motion,
              facial: remaining[0].facial,
            },
          },
        })
      },

      setImportPositionSelection: (posIndex: number, kind: 'motion' | 'facial', selection: number): void => {
        const form = ctx.getState().importForm
        if (posIndex < 0 || posIndex >= form.positions.length) return
        let positions = form.positions.map((pos, idx) => {
          if (idx !== posIndex) return pos
          const chosen = pos[kind].candidates[selection] ?? ''
          return {
            ...pos,
            [kind]: { ...pos[kind], selection },
            ...(kind === 'motion' && chosen !== IMPORT_SLOT.keepMotion ? { preserveExisting: false } : {}),
          }
        })
        if (kind === 'motion') {
          positions = recomputeAutoSlotOffsets(positions, form.probes, form.mode)
        }
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
        const positions = form.positions.map((pos, idx) => {
          if (idx !== posIndex) return pos
          const nextX = finite(placement.x)
          const nextY = finite(placement.y)
          const nextZ = finite(placement.z)
          const xyzChanged = nextX !== pos.x || nextY !== pos.y || nextZ !== pos.z
          return {
            ...pos,
            x: nextX,
            y: nextY,
            z: nextZ,
            yawDegrees: finite(placement.yawDegrees),
            placementEdited: Boolean(pos.placementEdited) || xyzChanged,
          }
        })
        ctx.set({ importForm: { ...form, positions } })
      },

      setImportPositionTiming: (
        posIndex: number,
        kind: 'motion' | 'facial',
        patch: Partial<TrackTiming>,
      ): void => {
        const form = ctx.getState().importForm
        if (posIndex < 0 || posIndex >= form.positions.length) return
        const positions = form.positions.map((pos, idx) =>
          idx === posIndex
            ? {
                ...pos,
                timing: {
                  ...pos.timing,
                  [kind]: sanitizeTrackTiming(pos.timing[kind], patch),
                },
              }
            : pos,
        )
        const timing =
          posIndex === 0
            ? { ...form.timing, [kind]: { ...positions[0].timing[kind] } }
            : form.timing
        ctx.set({ importForm: { ...form, positions, timing } })
      },

      importPickPositionFile: async (posIndex: number, kind: 'motion' | 'facial', file: string): Promise<void> => {
        await helpers.probePaths([file])
        const form = ctx.getState().importForm
        if (posIndex < 0 || posIndex >= form.positions.length) return
        const probe = form.probes[file]
        const addMotion = kind === 'motion' || Boolean(probe && canUseAsMotion(probe))
        const addFacial = kind === 'facial' || Boolean(probe && canUseAsFacial(probe))
        let positions = form.positions.map((pos, idx) => {
          let nextMotion = pos.motion
          let nextFacial = pos.facial
          if (addMotion) {
            const candidates = [...pos.motion.candidates]
            const insertedIdx = insertCandidate(candidates, file, false)
            nextMotion = {
              candidates,
              selection: idx === posIndex && kind === 'motion' ? insertedIdx : pos.motion.selection,
            }
          }
          if (addFacial) {
            const candidates = [...pos.facial.candidates]
            const insertedIdx = insertCandidate(candidates, file, true)
            nextFacial = {
              candidates,
              selection: idx === posIndex && kind === 'facial' ? insertedIdx : pos.facial.selection,
            }
          }
          return {
            ...pos,
            motion: nextMotion,
            facial: nextFacial,
            ...(idx === posIndex && kind === 'motion' ? { preserveExisting: false } : {}),
          }
        })
        if (kind === 'motion') {
          positions = recomputeAutoSlotOffsets(positions, form.probes, form.mode)
        }
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
        let positions = next.positions.map((pos) => ({
          ...pos,
          motion: { ...pos.motion, candidates: [...pos.motion.candidates] },
          facial: { ...pos.facial, candidates: [...pos.facial.candidates] },
        }))
        const camera = { ...next.slots.camera, candidates: [...next.slots.camera.candidates] }
        const audio = { ...next.slots.audio, candidates: [...next.slots.audio.candidates] }

        let appendedAudio: string | null = null
        for (const file of files) {
          const dot = file.lastIndexOf('.')
          const ext = dot >= 0 ? file.slice(dot).toLowerCase() : ''
          if (isSupportedAudioExtension(file)) {
            const idx = insertCandidate(audio.candidates, file, true)
            const currentAudio = audio.candidates[audio.selection] ?? ''
            if (isSlotPlaceholder(currentAudio) && currentAudio !== IMPORT_SLOT.keepTrack) {
              audio.selection = idx
              appendedAudio = file
            }
          } else if (ext === '.gkcamera') {
            const idx = insertCandidate(camera.candidates, file, false)
            const currentCam = camera.candidates[camera.selection] ?? ''
            if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
              camera.selection = idx
            }
          } else if (ext === '.vmd') {
            const probe = probeMap[file]
            if (probe && !canUseAsMotion(probe)) {
              if (probe.cameraFrames > 0) {
                const idx = insertCandidate(camera.candidates, file, false)
                const currentCam = camera.candidates[camera.selection] ?? ''
                if (isSlotPlaceholder(currentCam) && currentCam !== IMPORT_SLOT.keepTrack) {
                  camera.selection = idx
                }
              }
              if (canUseAsFacial(probe)) {
                const idx = insertCandidate(positions[0].facial.candidates, file, true)
                const curFacial = positions[0].facial.candidates[positions[0].facial.selection] ?? ''
                if (isSlotPlaceholder(curFacial) && curFacial !== IMPORT_SLOT.keepTrack) {
                  positions[0].facial.selection = idx
                }
              }
            } else {
              const emptyIdx = positions.findIndex((pos) => {
                const picked = pos.motion.candidates[pos.motion.selection] ?? ''
                return picked === IMPORT_SLOT.empty
              })
              if (emptyIdx >= 0) {
                const idx = insertCandidate(positions[emptyIdx].motion.candidates, file, false)
                positions[emptyIdx].motion.selection = idx
                positions[emptyIdx].preserveExisting = false
              } else {
                positions.push({
                  motion: { candidates: [IMPORT_SLOT.empty, file], selection: 1 },
                  facial: {
                    candidates: next.mode === 'edit' ? [IMPORT_SLOT.clear] : [IMPORT_SLOT.autoFacial, IMPORT_SLOT.skip],
                    selection: 0,
                  },
                  timing: defaultSlotPerformanceTiming(),
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

        positions = syncSharedPositionPools(positions, probeMap)
        positions = recomputeAutoSlotOffsets(positions, probeMap, next.mode)

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
        const swapped = [...next.positions]
        const current = swapped[posIndex]
        const target = swapped[targetIndex]
        // Swap only the motion/facial track pair, timing, & preserveExisting/sourceSlot; keep x/y/z/yaw anchored to each slot index.
        swapped[posIndex] = {
          ...current,
          motion: target.motion,
          facial: target.facial,
          timing: target.timing,
          preserveExisting: target.preserveExisting,
          sourceSlot: target.sourceSlot,
        }
        swapped[targetIndex] = {
          ...target,
          motion: current.motion,
          facial: current.facial,
          timing: current.timing,
          preserveExisting: current.preserveExisting,
          sourceSlot: current.sourceSlot,
        }
        const positions = recomputeAutoSlotOffsets(swapped, next.probes, next.mode)
        ctx.set({
          importForm: {
            ...next,
            positions,
            timing: {
              ...next.timing,
              motion: { ...positions[0].timing.motion },
              facial: { ...positions[0].timing.facial },
            },
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
