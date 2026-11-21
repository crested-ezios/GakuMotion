import { boneNameByHid, type ExperimentsState, type GazeSettings } from '../../protocol'
import type { SliceBundle, StoreContext } from '../context'
import { asObj, asStr } from '../normalizersCore'
import { normalizeStageNodes, normalizeTelemetrySnapshot } from '../normalizersPhase2'

export function createDevToolsSlice(ctx: StoreContext): SliceBundle<ReturnType<typeof buildActions>> {
  const actions = buildActions()

  function buildActions() {
    const setGazeSettings = async (patch: Partial<GazeSettings>): Promise<void> => {
      const settings = { ...ctx.getState().gaze.settings, ...patch }
      try {
        await ctx.client().request('gaze.set', { settings })
        ctx.set({ actionError: null })
      } catch (error) {
        ctx.reportActionError(error)
      }
    }

    return {
      // ---- Bone overrides (phase 2) ----
      selectBone: (hid: number): void => ctx.set({ selectedBoneHid: hid }),

      setBoneSymmetrical: (enabled: boolean): void => ctx.set({ boneSymmetrical: enabled }),

      boneOverrideFor: (hid: number) =>
        ctx.getState().bone.overrides.find((entry) => entry.humanoidId === hid) ?? null,

      setBoneOverride: async (
        hid: number,
        pitch: number,
        yaw: number,
        roll: number,
        spaceWorld: boolean,
        enabled: boolean,
      ): Promise<void> => {
        try {
          await ctx.client().request('bone.set', {
            humanoidId: hid,
            boneName: boneNameByHid(hid),
            pitch,
            yaw,
            roll,
            spaceWorld,
            enabled,
            symmetrical: ctx.getState().boneSymmetrical,
          })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      resetBoneOverride: async (hid: number): Promise<void> => {
        try {
          await ctx.client().request('bone.reset', {
            humanoidId: hid,
            boneName: boneNameByHid(hid),
            symmetrical: ctx.getState().boneSymmetrical,
          })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      resetAllBoneOverrides: async (): Promise<void> => {
        try {
          await ctx.client().request('bone.resetAll')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setBoneMaster: async (enabled: boolean): Promise<void> => {
        try {
          await ctx.client().request('bone.setMaster', { enabled })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      reloadBoneOverrides: async (): Promise<void> => {
        try {
          await ctx.client().request('bone.reload')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      saveBoneOverridesToMotion: async (): Promise<void> => {
        try {
          await ctx.client().request('bone.saveToMotion')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      saveBoneOverridesToGlobal: async (): Promise<void> => {
        try {
          await ctx.client().request('bone.saveToGlobal')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      // ---- Gaze (phase 2) ----
      setGazeSettings,

      cycleGazeMode: (): void => {
        void setGazeSettings({ mode: (ctx.getState().gaze.settings.mode + 1) % 3 })
      },

      // ---- Stage nodes / replacement (phase 2) ----
      scanStageNodes: async (): Promise<void> => {
        if (!ctx.supports('stage.scan')) return
        try {
          const res = await ctx.client().request('stage.scan')
          ctx.set({ stageNodes: normalizeStageNodes(asObj(res).nodes), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      toggleStageNode: async (index: number, active: boolean): Promise<void> => {
        try {
          await ctx.client().request('stage.toggleNode', { index, active })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setStageLoadReplacement: async (key: string): Promise<void> => {
        if (!ctx.supports('stage.setLoadReplacement')) return
        try {
          const res = await ctx.client().request('stage.setLoadReplacement', { key })
          ctx.set({ stageLoadNotice: asStr(asObj(res).notice), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      // ---- Experiments / telemetry (phase 2) ----
      setExperiment: async (patch: Partial<ExperimentsState['settings']>): Promise<void> => {
        try {
          await ctx.client().request('experiments.set', { patch })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      refreshTelemetry: async (): Promise<void> => {
        if (!ctx.supports('telemetry.get')) return
        try {
          const raw = await ctx.client().request('telemetry.get')
          ctx.set({ telemetry: normalizeTelemetrySnapshot(raw), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },
    }
  }

  return { actions }
}

export type DevToolsActions = ReturnType<typeof createDevToolsSlice>['actions']
