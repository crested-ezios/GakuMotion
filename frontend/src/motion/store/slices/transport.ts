import { setCameraControlMode } from '../../cameraMode'
import type { ClockRunMode, ClockSource } from '../../protocol'
import { parseTransportFrame, parseTransportTime } from '../../time'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeAudioEffectState, normalizeClockState } from '../normalizersCore'
import type { LibraryFilter, LibrarySort, MotionPage } from '../types'

export function createTransportSlice(ctx: StoreContext): SliceBundle<ReturnType<typeof buildActions>> {
  let volumeTimer: ReturnType<typeof setTimeout> | null = null
  let pendingVolume: number | null = null

  const flushVolume = () => {
    if (pendingVolume === null) return
    const volume = pendingVolume
    pendingVolume = null
    void ctx.client().request('player.setVolume', { volume }).catch((error) => ctx.reportActionError(error))
  }

  const actions = buildActions()

  function buildActions() {
    return {
      // ---- UI-local actions ----
      setPage: (page: MotionPage): void => {
        if (page === 'developer' && !ctx.getState().developerMode) return
        ctx.set({ page })
      },

      setDeveloperMode: (enabled: boolean): void => {
        ctx.set({
          developerMode: enabled,
          page: !enabled && ctx.getState().page === 'developer' ? 'library' : ctx.getState().page,
        })
      },

      selectLibrary: (key: string | null): void => ctx.set({ selectedLibraryKey: key }),

      setLibrarySearch: (search: string): void => ctx.set({ librarySearch: search }),

      setLibraryFilter: (libraryFilter: LibraryFilter): void => ctx.set({ libraryFilter }),

      setLibrarySort: (librarySort: LibrarySort): void => ctx.set({ librarySort }),

      setSeekStepSeconds: (seconds: number): void =>
        ctx.set({ seekStepSeconds: [1, 5, 10, 15].includes(seconds) ? seconds : 5 }),

      setPrecisionPanelOpen: (precisionPanelOpen: boolean): void => ctx.set({ precisionPanelOpen }),

      setPrecisionPanelPinned: (precisionPanelPinned: boolean): void => ctx.set({ precisionPanelPinned }),

      setImportDialogOpen: (open: boolean): void => ctx.set({ importDialogOpen: open }),

      clearActionError: (): void => ctx.set({ actionError: null }),

      // ---- Transport ----
      play: async (): Promise<void> => {
        try {
          const state = ctx.getState().player.state
          if (state === 'paused' || state === 'preview') {
            await ctx.client().request('player.resume')
          } else {
            await ctx.client().request('player.play')
          }
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      pause: async (): Promise<void> => {
        try {
          await ctx.client().request('player.pause')
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      stop: async (): Promise<void> => {
        try {
          await ctx.client().request('player.stop')
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      cycle: async (delta: number): Promise<void> => {
        try {
          await ctx.client().request('player.cycle', { delta })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      stepFrame: async (delta: number): Promise<void> => {
        try {
          await ctx.client().request('player.stepFrame', { delta })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      scrubStart: (seconds: number): void => {
        const state = ctx.getState()
        ctx.set({
          scrub: { active: true, value: seconds },
          probeScrubBegin: state.probeScrubBegin + 1,
          probeLastScrubValue: seconds,
        })
        void ctx.client().request('player.scrubBegin', { seconds }).catch((error) => ctx.reportActionError(error))
      },

      scrubMove: (seconds: number): void => {
        const state = ctx.getState()
        if (!state.scrub.active) return
        ctx.set({
          scrub: { active: true, value: seconds },
          probeScrubUpdate: state.probeScrubUpdate + 1,
          probeLastScrubValue: seconds,
        })
        void ctx.client().request('player.scrubUpdate', { seconds }).catch((error) => ctx.reportActionError(error))
      },

      scrubCommit: (seconds: number): void => {
        const state = ctx.getState()
        if (!state.scrub.active) return
        ctx.set({
          scrub: { active: false, value: seconds },
          probeScrubEnd: state.probeScrubEnd + 1,
          probeLastScrubValue: seconds,
        })
        void ctx.client().request('player.scrubEnd', { seconds }).catch((error) => ctx.reportActionError(error))
      },

      seekTime: async (input: string): Promise<void> => {
        const seconds = parseTransportTime(input)
        if (seconds === null) {
          ctx.set({ navigationError: '时间格式无效：请输入非负秒数或 mm:ss.mmm。' })
          return
        }
        if (seconds > ctx.getState().player.duration) {
          ctx.set({ navigationError: '时间超出演出时长；可用动作帧输入检查末采样。' })
          return
        }
        try {
          await ctx.client().request('player.seek', { seconds })
          ctx.set({ navigationError: null, actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      seekRelative: async (deltaSeconds: number): Promise<void> => {
        const { totalFrames, duration, time } = ctx.getState().player
        if (totalFrames <= 0 || duration <= 0) return
        const target = Math.min(Math.max(time + deltaSeconds, 0), duration)
        try {
          await ctx.client().request('player.seek', { seconds: target })
          ctx.set({ navigationError: null, actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      seekFrame: async (input: string): Promise<void> => {
        const frame = parseTransportFrame(input)
        if (frame === null) {
          ctx.set({ navigationError: '帧号格式无效：请输入从 0 开始的非负整数。' })
          return
        }
        const { totalFrames } = ctx.getState().player
        if (totalFrames <= 0 || frame >= totalFrames) {
          ctx.set({ navigationError: '帧号超出范围；末帧为总采样帧数减 1。' })
          return
        }
        try {
          await ctx.client().request('player.seekFrame', { frame })
          ctx.set({ navigationError: null, actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setSpeed: async (speed: number): Promise<void> => {
        try {
          await ctx.client().request('player.setSpeed', { speed })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setPlayMode: async (mode: number): Promise<void> => {
        try {
          await ctx.client().request('playlist.setMode', { mode })
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      cyclePlayMode: async (): Promise<void> => {
        const maxModes = 4
        const current = ctx.getState().player.playMode
        const normalized = current >= 0 && current < maxModes ? current : 1
        const next = (normalized + 1) % maxModes
        await actions.setPlayMode(next)
      },

      // Dragging sends throttled updates; releasing commits and saves (ImGui saves
      // on IsItemDeactivatedAfterEdit, never mid-drag).
      setVolume: (percent: number): void => {
        const volume = Math.min(Math.max(percent, 0), 100) / 100
        pendingVolume = volume
        if (volumeTimer) return
        volumeTimer = setTimeout(() => {
          volumeTimer = null
          flushVolume()
        }, 80)
      },

      commitVolume: (percent: number): void => {
        if (volumeTimer) {
          clearTimeout(volumeTimer)
          volumeTimer = null
        }
        pendingVolume = Math.min(Math.max(percent, 0), 100) / 100
        flushVolume()
        void ctx.client().request('player.saveVolume').catch((error) => ctx.reportActionError(error))
      },

      setCameraQuickMode: async (selection: number): Promise<void> => {
        const state = ctx.getState()
        const base = state.cameraPending ? state.cameraDraft : state.camera.settings
        const next = setCameraControlMode(base, selection)
        ctx.set({ cameraDraft: next, cameraExpected: next, cameraPending: true })
        try {
          await ctx.client().request('camera.set', { settings: next })
        } catch (error) {
          ctx.set({ cameraPending: false, cameraExpected: null, cameraDraft: ctx.getState().cameraLastSnapshot })
          ctx.reportActionError(error)
        }
      },

      reloadMotion: async (): Promise<void> => {
        try {
          await ctx.client().request('player.reload')
          ctx.set({ actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setClockRunMode: async (runMode: ClockRunMode): Promise<void> => {
        try {
          const next = await ctx.client().request('clock.setRunMode', { runMode })
          ctx.set({ clock: normalizeClockState(next), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      configureClock: async (patch: {
        source?: ClockSource
        fpsNumerator?: number
        fpsDenominator?: number
      }): Promise<void> => {
        try {
          const next = await ctx.client().request('clock.configure', patch)
          ctx.set({ clock: normalizeClockState(next), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      setAudioEffect: async (presetId: string): Promise<void> => {
        try {
          const next = await ctx.client().request('audioEffect.set', { presetId })
          ctx.set({ audioEffect: normalizeAudioEffectState(next), actionError: null })
        } catch (error) {
          ctx.reportActionError(error)
        }
      },
    }
  }

  return {
    actions,
    reset: () => {
      if (volumeTimer) clearTimeout(volumeTimer)
      volumeTimer = null
      pendingVolume = null
    },
  }
}

export type TransportActions = ReturnType<typeof createTransportSlice>['actions']
