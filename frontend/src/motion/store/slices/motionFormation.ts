import { cameraControlModeIndex } from '../../cameraMode'
import {
  BridgeError,
  DEFAULT_FACIAL_WEIGHTS,
  type CameraSettings,
  type FacialWeights,
  type MotionStateSnapshot,
  type StageSlotOffset,
  type Vector3,
} from '../../protocol'
import type { SliceBundle, StoreContext } from '../context'

export function createMotionFormationSlice(ctx: StoreContext): SliceBundle<ReturnType<typeof buildActions>> {
  let facialTimer: ReturnType<typeof setTimeout> | null = null
  let pendingFacial: FacialWeights | null = null
  const portraitInflight = new Map<string, Promise<string | null>>()

  const flushFacial = () => {
    if (!pendingFacial) return
    const weights = pendingFacial
    pendingFacial = null
    void ctx.client().request('facial.setWeights', weights).catch((error) => ctx.reportActionError(error))
  }

  const actions = buildActions()

  function buildActions() {
    const setPosition = async (offset: Vector3): Promise<void> => {
      try {
        await ctx.client().request('position.set', offset)
        ctx.set({ actionError: null })
      } catch (error) {
        ctx.reportActionError(error)
      }
    }

    const setFormationSlotOffset = async (slotIndex: number, offset: StageSlotOffset): Promise<void> => {
      try {
        await ctx.client().request('formation.setSlotOffset', {
          slotIndex,
          x: offset.x,
          y: offset.y,
          z: offset.z,
          yawDegrees: offset.yawDegrees,
        })
        ctx.set({ actionError: null })
      } catch (error) {
        ctx.reportActionError(error)
      }
    }

    return {
      // ---- Motion page ----
      selectActor: async (token: number): Promise<void> => {
        try {
          await ctx.client().request('actors.selectTarget', { token, epoch: ctx.getState().actors.epoch })
          ctx.set({ actionError: null })
        } catch (error) {
          if (error instanceof BridgeError && error.code === 'stale_token') {
            ctx.set({ actionError: '角色名单已变化，请重新选择。' })
          } else {
            ctx.reportActionError(error)
          }
        }
      },

      setFacialWeights: (weights: FacialWeights): void => {
        pendingFacial = weights
        if (facialTimer) return
        facialTimer = setTimeout(() => {
          facialTimer = null
          flushFacial()
        }, 80)
      },

      commitFacialWeights: (weights: FacialWeights): void => {
        if (facialTimer) {
          clearTimeout(facialTimer)
          facialTimer = null
        }
        pendingFacial = weights
        flushFacial()
      },

      resetFacialWeights: (): void => {
        actions.commitFacialWeights({ ...DEFAULT_FACIAL_WEIGHTS })
      },

      saveFacialWeights: async (): Promise<void> => {
        try {
          await ctx.client().request('facial.save')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setPosition,

      resetPosition: (): void => {
        void setPosition({ x: 0, y: 0, z: 0 })
      },

      savePosition: async (): Promise<void> => {
        try {
          await ctx.client().request('position.save')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      // ---- Portraits & Formation (F5 / F7-F10 / F13) ----
      ensureActorPortrait: (character: string): Promise<string | null> => {
        const norm = character.trim().toLowerCase()
        if (!norm) return Promise.resolve(null)
        const state = ctx.getState()
        if (Object.prototype.hasOwnProperty.call(state.portraitCache, norm)) {
          return Promise.resolve(state.portraitCache[norm] ?? null)
        }
        const existing = portraitInflight.get(norm)
        if (existing) return existing
        if (!ctx.supports('actors.getPortrait')) {
          return Promise.resolve(null)
        }
        const promise = (async (): Promise<string | null> => {
          try {
            const res = await ctx.client().request('actors.getPortrait', { character: norm })
            const uri = res.cached && typeof res.dataUri === 'string' && res.dataUri !== '' ? res.dataUri : null
            ctx.set({ portraitCache: { ...ctx.getState().portraitCache, [norm]: uri } })
            return uri
          } catch {
            return null
          } finally {
            portraitInflight.delete(norm)
          }
        })()
        portraitInflight.set(norm, promise)
        return promise
      },

      selectFormationSlot: async (slotIndex: number): Promise<void> => {
        try {
          await ctx.client().request('formation.selectSlot', { slotIndex })
          ctx.set({
            formation: { ...ctx.getState().formation, selectedSlotIndex: slotIndex },
            actionError: null,
          })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      swapFormationSlots: async (fromSlotIndex: number, toSlotIndex: number): Promise<void> => {
        try {
          await ctx.client().request('formation.swapSlots', { fromSlotIndex, toSlotIndex })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setFormationSlotOffset,
      commitFormationSlotOffset: setFormationSlotOffset,

      resetFormationSlotOffset: async (slotIndex: number): Promise<void> => {
        try {
          await ctx.client().request('formation.resetSlotOffset', { slotIndex })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      saveFormationSlotOffset: async (slotIndex: number): Promise<void> => {
        try {
          await ctx.client().request('formation.saveSlotOffset', { slotIndex })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setFormationSlotSource: async (slotIndex: number, sourceSlotIndex: number): Promise<void> => {
        try {
          await ctx.client().request('formation.setSlotSource', { slotIndex, sourceSlotIndex })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      // ---- Camera page ----
      patchCameraDraft: (patch: Partial<CameraSettings>): void => {
        const next = { ...ctx.getState().cameraDraft, ...patch }
        ctx.set({ cameraDraft: next, cameraExpected: next, cameraPending: true })
        void ctx.client()
          .request('camera.set', { settings: next })
          .catch((error) => {
            ctx.set({ cameraPending: false, cameraExpected: null, cameraDraft: ctx.getState().cameraLastSnapshot })
            ctx.reportActionError(error)
          })
      },

      saveCamera: async (): Promise<void> => {
        try {
          await ctx.client().request('camera.save')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      cameraQuickModeIndex: (): number => {
        const state = ctx.getState()
        const settings = state.cameraPending ? state.cameraDraft : state.camera.settings
        return cameraControlModeIndex(settings)
      },

      // ---- Stage page ----
      patchStage: (patch: Partial<MotionStateSnapshot['stage']['settings']>): void => {
        const state = ctx.getState()
        const next = { ...state.stage.settings, ...patch }
        ctx.set({ stage: { ...state.stage, settings: next } })
        void ctx.client()
          .request('stage.set', { settings: next })
          .catch((error) => ctx.reportActionError(error))
      },

      // ---- Settings page ----
      setPhysicsBackend: async (backend: number): Promise<void> => {
        try {
          await ctx.client().request('settings.setPhysics', { backend })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },
    }
  }

  return {
    actions,
    reset: () => {
      if (facialTimer) clearTimeout(facialTimer)
      facialTimer = null
      pendingFacial = null
      portraitInflight.clear()
    },
  }
}

export type MotionFormationActions = ReturnType<typeof createMotionFormationSlice>['actions']
