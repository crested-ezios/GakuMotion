import { BridgeError, type ClockRunMode, type ClockSource } from '../../protocol'
import { steppedFrame } from '../../time'
import { clamp } from '../../utils'
import { SUPPORTED_EVENTS } from '../capabilities'
import type { MockHandlerContext, MockHandlerMap } from '../context'

export function createPlaybackHandlers(ctx: MockHandlerContext): MockHandlerMap {
  const { state } = ctx
  return {
    'bridge.hello': () => ({ protocol: 1, sessionId: state.sessionId }),
    'bridge.ping': () => ({ pong: true }),
    'motion.hello': () => ({
      protocol: 1,
      sessionId: state.sessionId,
      pluginVersion: '0.2.0-mock',
      supportedMethods: ctx.supportedMethods(),
      supportedEvents: [...SUPPORTED_EVENTS],
    }),
    'state.get': () => ctx.snapshot(),
    'player.play': () => {
      ctx.requireAllowed()
      ctx.requireClip()
      state.player = { ...state.player, state: 'playing' }
      ctx.startTick()
      ctx.emitPlayer()
      return {}
    },
    'player.pause': () => {
      ctx.requireAllowed()
      if (state.player.state === 'playing') state.player = { ...state.player, state: 'paused' }
      ctx.emitPlayer()
      return {}
    },
    'player.resume': () => {
      ctx.requireAllowed()
      ctx.requireClip()
      if (state.player.state === 'paused' || state.player.state === 'preview') {
        state.player = { ...state.player, state: 'playing' }
        ctx.startTick()
      }
      ctx.emitPlayer()
      return {}
    },
    'player.stop': () => {
      ctx.requireAllowed()
      state.player = { ...state.player, state: 'stopped', time: 0 }
      ctx.emitPlayer()
      return {}
    },
    'player.cycle': (p) => {
      ctx.requireAllowed()
      ctx.cycle(Number(p.delta) >= 0 ? 1 : -1)
      return {}
    },
    'player.seek': (p) => {
      ctx.requireAllowed()
      ctx.requireClip()
      const seconds = Number(p.seconds)
      if (!Number.isFinite(seconds) || seconds < 0 || seconds > state.player.duration) {
        throw new BridgeError('invalid_params', '时间超出范围')
      }
      state.player = { ...state.player, time: seconds }
      ctx.emitPlayer()
      return {}
    },
    'player.seekFrame': (p) => {
      ctx.requireAllowed()
      ctx.requireClip()
      const frame = Number(p.frame)
      if (!Number.isInteger(frame) || frame < 0 || frame >= state.player.totalFrames) {
        throw new BridgeError('invalid_params', '帧号超出范围；末帧为总采样帧数减 1')
      }
      state.player = { ...state.player, state: 'paused', time: frame / state.player.fps }
      ctx.emitPlayer()
      return {}
    },
    'player.stepFrame': (p) => {
      ctx.requireAllowed()
      ctx.requireClip()
      const delta = Number(p.delta) >= 0 ? 1 : -1
      const frame = steppedFrame(state.player.time, state.player.fps, state.player.totalFrames, delta)
      state.player = { ...state.player, state: 'paused', time: frame / state.player.fps }
      ctx.emitPlayer()
      return {}
    },
    'player.scrubBegin': (p) => {
      ctx.requireAllowed()
      ctx.requireClip()
      state.scrubbing = true
      state.player = { ...state.player, time: clamp(Number(p.seconds), 0, state.player.duration) }
      ctx.emitPlayer()
      return {}
    },
    'player.scrubUpdate': (p) => {
      ctx.requireAllowed()
      state.player = { ...state.player, time: clamp(Number(p.seconds), 0, state.player.duration) }
      ctx.emitPlayer()
      return {}
    },
    'player.scrubEnd': (p) => {
      ctx.requireAllowed()
      state.scrubbing = false
      state.player = { ...state.player, time: clamp(Number(p.seconds), 0, state.player.duration) }
      ctx.emitPlayer()
      return {}
    },
    'player.setSpeed': (p) => {
      state.player = { ...state.player, speed: clamp(Number(p.speed), 0.1, 3) }
      ctx.emitPlayer()
      return {}
    },
    'player.setVolume': (p) => {
      state.player = { ...state.player, volume: clamp(Number(p.volume), 0, 1) }
      ctx.emitPlayer()
      return {}
    },
    'player.saveVolume': () => {
      state.volumeSaved = state.player.volume
      return {}
    },
    'player.reload': () => {
      ctx.requireAllowed()
      const entry = state.entries.find((candidate) => candidate.id === state.loaded.motionKey)
      if (!entry) throw new BridgeError('not_ready', '没有已加载曲目')
      const time = state.player.time
      ctx.loadEntry(entry, false)
      state.player = { ...state.player, state: 'paused', time }
      ctx.emitPlayer()
      return {}
    },
    'playlist.setMode': (p) => {
      const mode = Number(p.mode)
      state.playlist = { ...state.playlist, mode: mode >= 0 && mode <= 3 ? mode : 1 }
      state.player = { ...state.player, playMode: state.playlist.mode }
      ctx.emit('playlist.changed', { ...state.playlist })
      ctx.emitPlayer()
      return {}
    },
    'clock.setRunMode': (p) => {
      ctx.requireCapability('clock.setRunMode')
      const runMode: ClockRunMode = p.runMode === 'recording' ? 'recording' : 'realtime'
      state.clock = {
        ...state.clock,
        runMode,
        effectiveSource: runMode === 'recording' ? 'fixed_step' : state.clock.configuredSource,
      }
      ctx.emit('clock.changed', { ...state.clock })
      return { ...state.clock }
    },
    'clock.configure': (p) => {
      ctx.requireCapability('clock.configure')
      const source = (p.source as ClockSource | undefined) ?? state.clock.configuredSource
      const fpsNumerator = Number(p.fpsNumerator ?? state.clock.fpsNumerator) || 60
      const fpsDenominator = Number(p.fpsDenominator ?? state.clock.fpsDenominator) || 1
      state.clock = {
        ...state.clock,
        configuredSource: source,
        effectiveSource: state.clock.runMode === 'recording' ? 'fixed_step' : source,
        fpsNumerator,
        fpsDenominator,
      }
      ctx.emit('clock.changed', { ...state.clock })
      return { ...state.clock }
    },
    'audioEffect.set': (p) => {
      ctx.requireCapability('audioEffect.set')
      const presetId = String(p.presetId ?? '')
      if (!state.audioEffect.presets.some((item) => item.id === presetId)) {
        throw new BridgeError('failed', `未知音效预设: ${presetId}`)
      }
      state.audioEffect = { ...state.audioEffect, activeId: presetId }
      ctx.emit('audioEffect.changed', {
        ...state.audioEffect,
        presets: state.audioEffect.presets.map((x) => ({ ...x })),
      })
      return {
        ...state.audioEffect,
        presets: state.audioEffect.presets.map((x) => ({ ...x })),
      }
    },
  }
}
