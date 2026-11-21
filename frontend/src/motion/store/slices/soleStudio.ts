import type {
  SolePitchState,
  SoleSettings,
  SoleState,
  StudioSettings,
  StudioState,
  Vector3,
} from '../../protocol'
import { deepEqual } from '../../utils'
import { validateSole, validateStudio } from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeSolePitchState, normalizeSoleState } from '../normalizersPhase2'
import type { MotionAppState } from '../types'

export interface SoleStudioEventHooks {
  onSoleChanged: (sole: SoleState) => void
  onStudioChanged: (studio: StudioState) => void
}

export function createSoleStudioSlice(
  ctx: StoreContext,
): SliceBundle<ReturnType<typeof buildActions>> & SoleStudioEventHooks {
  let soleTimer: ReturnType<typeof setInterval> | null = null
  let solePitchTimer: ReturnType<typeof setInterval> | null = null

  const onSoleChanged = (sole: SoleState): void => {
    const patch: Partial<MotionAppState> = { sole }
    if (ctx.getState().soleDraftGeneration !== sole.generation) {
      patch.soleDraftGeneration = sole.generation
      patch.soleDraft = { ...sole.settings, planeCalibrated: false, shoeCalibrated: false }
    }
    ctx.set(patch)
  }

  const onStudioChanged = (studio: StudioState): void => {
    const state = ctx.getState()
    const patch: Partial<MotionAppState> = { studio }
    const draft = state.studioDraft
    if (state.studioDraftRevision !== studio.revision) {
      const nextDraft = { ...draft }
      if (!deepEqual(draft.lighting, studio.settings.lighting)) nextDraft.lighting = studio.settings.lighting
      if (!deepEqual(draft.video, studio.settings.video)) {
        nextDraft.video = studio.settings.video
        patch.studioVideoPath = studio.settings.video.path
      }
      patch.studioDraft = nextDraft
      patch.studioDraftRevision = studio.revision
    }
    ctx.set(patch)
  }

  const actions = buildActions()

  function buildActions() {
    const patchSoleDraft = (patch: Partial<SoleSettings>): void => {
      ctx.set({ soleDraft: { ...ctx.getState().soleDraft, ...patch } })
    }

    const applySoleSettings = async (settings?: SoleSettings): Promise<void> => {
      const draft = settings ?? ctx.getState().soleDraft
      const error = validateSole(draft)
      if (error) {
        ctx.set({ actionError: error })
        return
      }
      try {
        await ctx.client().request('sole.set', { settings: draft })
        ctx.set({ actionError: null })
      } catch (error) {
        ctx.reportActionError(error)
      }
    }

    return {
      // ---- Sole measurement v2 (phase 2) ----
      patchSoleDraft,

      patchSoleDraftPoint: (side: number, index: number, point: Vector3): void => {
        const draft = ctx.getState().soleDraft
        const points = draft.points.map((foot, footIndex) =>
          footIndex === side ? foot.map((existing, pointIndex) => (pointIndex === index ? point : existing)) : foot,
        )
        ctx.set({ soleDraft: { ...draft, points } })
      },

      enableSoleBootProxy: (): void => {
        const draft = { ...ctx.getState().soleDraft, enabled: true, useBootProxy: true, shoeCalibrated: false }
        ctx.set({ soleDraft: draft })
        void applySoleSettings(draft)
      },

      useSoleManualFallback: (): void => {
        const draft = { ...ctx.getState().soleDraft, useBootProxy: false, shoeCalibrated: false }
        ctx.set({ soleDraft: draft })
        void applySoleSettings(draft)
      },

      normalizeSolePlaneNormal: (): void => {
        const normal = ctx.getState().soleDraft.planeNormal
        const norm = Math.hypot(normal.x, normal.y, normal.z)
        if (!Number.isFinite(norm) || norm < 1e-4) return
        patchSoleDraft({ planeNormal: { x: normal.x / norm, y: normal.y / norm, z: normal.z / norm } })
      },

      setSoleHorizontalPlane: (): void => {
        patchSoleDraft({ planeNormal: { x: 0, y: 1, z: 0 } })
      },

      applySoleSettings,

      reloadSoleDraft: (): void => {
        ctx.set({ soleDraft: { ...ctx.getState().sole.settings } })
      },

      saveSoleSettings: async (): Promise<void> => {
        try {
          await ctx.client().request('sole.save')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      loadSoleSettings: async (): Promise<void> => {
        try {
          await ctx.client().request('sole.load')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setSolePlotSpan: (span: number): void => {
        ctx.set({ solePlotSpanM: Math.min(Math.max(span, 0.25), 2) })
      },

      startSolePolling: (): void => {
        if (soleTimer || !ctx.supports('sole.get')) return
        soleTimer = setInterval(() => {
          void ctx.client().request('sole.get').then((sole) => onSoleChanged(normalizeSoleState(sole))).catch(() => {})
        }, 500)
      },

      stopSolePolling: (): void => {
        if (soleTimer) {
          clearInterval(soleTimer)
          soleTimer = null
        }
      },

      // ---- Pitch demo (phase 2) ----
      setSolePitchSettings: async (patch: Partial<SolePitchState['settings']>): Promise<void> => {
        const settings = { ...ctx.getState().solePitch.settings, ...patch }
        settings.maxPitchDegrees = Math.min(Math.max(settings.maxPitchDegrees, 0), 35)
        try {
          await ctx.client().request('solePitch.set', { settings })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      startSolePitchPolling: (): void => {
        if (solePitchTimer || !ctx.supports('solePitch.get')) return
        solePitchTimer = setInterval(() => {
          void ctx.client()
            .request('solePitch.get')
            .then((solePitch) => ctx.set({ solePitch: normalizeSolePitchState(solePitch) }))
            .catch(() => {})
        }, 500)
      },

      stopSolePitchPolling: (): void => {
        if (solePitchTimer) {
          clearInterval(solePitchTimer)
          solePitchTimer = null
        }
      },

      // ---- Studio lighting & video (phase 2) ----
      patchStudioLighting: (patch: Partial<StudioSettings['lighting']>): void => {
        const draft = ctx.getState().studioDraft
        ctx.set({ studioDraft: { ...draft, lighting: { ...draft.lighting, ...patch } } })
      },

      patchStudioVideo: (patch: Partial<StudioSettings['video']>): void => {
        const draft = ctx.getState().studioDraft
        ctx.set({ studioDraft: { ...draft, video: { ...draft.video, ...patch } } })
      },

      setStudioVideoPath: (path: string): void => {
        const draft = ctx.getState().studioDraft
        ctx.set({ studioVideoPath: path, studioDraft: { ...draft, video: { ...draft.video, path } } })
      },

      studioVideoError: (): string | null => {
        const state = ctx.getState()
        const next = { ...state.studio.settings, video: { ...state.studioDraft.video, path: state.studioVideoPath } }
        return validateStudio(next)
      },

      applyStudioLighting: async (): Promise<void> => {
        const state = ctx.getState()
        const next = { ...state.studio.settings, lighting: state.studioDraft.lighting }
        const error = validateStudio(next)
        if (error) {
          ctx.set({ actionError: error })
          return
        }
        try {
          await ctx.client().request('studio.set', { settings: next })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      disableStudioLighting: async (): Promise<void> => {
        const state = ctx.getState()
        const next = { ...state.studio.settings, lighting: { ...state.studio.settings.lighting, enabled: false } }
        ctx.set({
          studioDraft: { ...state.studioDraft, lighting: { ...state.studioDraft.lighting, enabled: false } },
        })
        try {
          await ctx.client().request('studio.set', { settings: next })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      applyStudioVideo: async (): Promise<void> => {
        const state = ctx.getState()
        const next = { ...state.studio.settings, video: { ...state.studioDraft.video, path: state.studioVideoPath } }
        const error = validateStudio(next)
        if (error) {
          ctx.set({ actionError: error })
          return
        }
        try {
          await ctx.client().request('studio.set', { settings: next })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      disableStudioVideo: async (): Promise<void> => {
        const state = ctx.getState()
        const next = { ...state.studio.settings, video: { ...state.studio.settings.video, enabled: false } }
        ctx.set({
          studioDraft: { ...state.studioDraft, video: { ...state.studioDraft.video, enabled: false } },
        })
        try {
          await ctx.client().request('studio.set', { settings: next })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      resetStudioSettings: async (): Promise<void> => {
        try {
          await ctx.client().request('studio.reset')
          ctx.set({ studioDraftRevision: -1 })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },
    }
  }

  const stopWatchers = () => {
    actions.stopSolePolling()
    actions.stopSolePitchPolling()
  }

  return {
    actions,
    onSoleChanged,
    onStudioChanged,
    stopWatchers,
    reset: stopWatchers,
  }
}

export type SoleStudioActions = ReturnType<typeof createSoleStudioSlice>['actions']
