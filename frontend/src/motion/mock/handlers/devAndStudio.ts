import {
  BridgeError,
  boneNameByHid,
  defaultStudioSettings,
  symmetricalHid,
  type ExperimentsState,
  type GazeState,
  type SolePitchState,
  type SoleState,
  type StudioSettings,
  type TelemetrySnapshot,
} from '../../protocol'
import { validateSole, validateStudio } from '../../validation'
import type { MockHandlerContext, MockHandlerMap } from '../context'

export function createDevAndStudioHandlers(ctx: MockHandlerContext): MockHandlerMap {
  const { state } = ctx
  return {
    'bone.get': () => {
      ctx.requireCapability('bone.get')
      return {
        ...state.bone,
        overrides: state.bone.overrides.map((item) => ({ ...item, eulerDeg: { ...item.eulerDeg } })),
      }
    },
    'bone.set': (p) => {
      ctx.requireCapability('bone.set')
      ctx.requireAllowed()
      const hid = Number(p.humanoidId)
      const pitch = Number(p.pitch)
      const yaw = Number(p.yaw)
      const roll = Number(p.roll)
      const spaceWorld = Boolean(p.spaceWorld)
      const enabled = Boolean(p.enabled)
      const symmetrical = Boolean(p.symmetrical)
      const upsert = (targetHid: number, sign: number) => {
        const name = boneNameByHid(targetHid) || String(p.boneName ?? '')
        const nextEntry = {
          humanoidId: targetHid,
          boneName: name,
          enabled,
          spaceWorld,
          eulerDeg: { x: pitch, y: yaw * sign, z: roll * sign },
          isDynamic: false,
        }
        const idx = state.bone.overrides.findIndex((o) => o.humanoidId === targetHid)
        if (idx >= 0) state.bone.overrides[idx] = nextEntry
        else state.bone.overrides.push(nextEntry)
      }
      upsert(hid, 1)
      if (symmetrical) {
        const mirror = symmetricalHid(hid)
        if (mirror >= 0) upsert(mirror, -1)
      }
      state.bone = { ...state.bone, notice: '' }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.reset': (p) => {
      ctx.requireCapability('bone.reset')
      const hid = Number(p.humanoidId)
      const mirror = Boolean(p.symmetrical) ? symmetricalHid(hid) : -1
      state.bone = {
        ...state.bone,
        overrides: state.bone.overrides.filter((o) => o.humanoidId !== hid && o.humanoidId !== mirror),
        notice: '已重置指定骨骼',
      }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.resetAll': () => {
      ctx.requireCapability('bone.resetAll')
      state.bone = { ...state.bone, overrides: [], notice: '已清空全部骨骼覆盖' }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.setMaster': (p) => {
      ctx.requireCapability('bone.setMaster')
      state.bone = { ...state.bone, masterEnabled: Boolean(p.enabled) }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.reload': () => {
      ctx.requireCapability('bone.reload')
      state.bone = { ...state.bone, notice: '已从磁盘重载骨骼覆盖（mock）' }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.saveToMotion': () => {
      ctx.requireCapability('bone.saveToMotion')
      state.bone = { ...state.bone, notice: '已保存骨骼覆盖至当前曲目（mock）' }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'bone.saveToGlobal': () => {
      ctx.requireCapability('bone.saveToGlobal')
      state.bone = { ...state.bone, notice: '已保存骨骼覆盖至全局配置（mock）' }
      ctx.emit('bone.changed', { ...state.bone })
      return {}
    },
    'gaze.get': () => {
      ctx.requireCapability('gaze.get')
      return { ...state.gaze, settings: { ...state.gaze.settings } }
    },
    'gaze.set': (p) => {
      ctx.requireCapability('gaze.set')
      state.gaze = { ...state.gaze, settings: { ...(p.settings as GazeState['settings']) } }
      ctx.emit('gaze.changed', { ...state.gaze })
      return {}
    },
    'stage.scan': () => {
      ctx.requireCapability('stage.scan')
      ctx.emit('stage.nodesChanged', { nodes: state.stageNodes.map((n) => ({ ...n })) })
      return { nodes: state.stageNodes.map((n) => ({ ...n })) }
    },
    'stage.toggleNode': (p) => {
      ctx.requireCapability('stage.toggleNode')
      const idx = Number(p.index)
      const active = Boolean(p.active)
      state.stageNodes = state.stageNodes.map((node) =>
        node.index === idx && !node.isColliderLocked ? { ...node, currentlyActive: active } : node,
      )
      ctx.emit('stage.nodesChanged', { nodes: state.stageNodes.map((n) => ({ ...n })) })
      return {}
    },
    'stage.setLoadReplacement': (p) => {
      ctx.requireCapability('stage.setLoadReplacement')
      const key = String(p.key ?? '')
      state.stage = { ...state.stage, settings: { ...state.stage.settings, loadReplacementKey: key } }
      ctx.emit('stage.changed', { ...state.stage, settings: { ...state.stage.settings } })
      return { notice: key ? `已预约舞台替换：${key}` : '已恢复原生摄影背景' }
    },
    'experiments.get': () => {
      ctx.requireCapability('experiments.get')
      return { ...state.experiments, settings: { ...state.experiments.settings } }
    },
    'experiments.set': (p) => {
      ctx.requireCapability('experiments.set')
      state.experiments = {
        ...state.experiments,
        settings: { ...state.experiments.settings, ...(p.patch as ExperimentsState['settings']) },
      }
      ctx.emit('experiments.changed', { ...state.experiments })
      return {}
    },
    'telemetry.get': () => {
      ctx.requireCapability('telemetry.get')
      const stateCode =
        state.player.state === 'playing' ? 1 : state.player.state === 'paused' ? 2 : state.player.state === 'preview' ? 3 : 0
      const stateName =
        state.player.state === 'playing'
          ? '播放中'
          : state.player.state === 'paused'
            ? '已暂停'
            : state.player.state === 'preview'
              ? '预览中'
              : '已停止'
      const snap: TelemetrySnapshot = {
        statusLine: `Mock Ready | ${state.stage.sceneName}`,
        player: {
          state: stateCode,
          stateName,
          time: state.player.time,
          duration: state.player.duration,
          totalFrames: state.player.totalFrames,
          fps: state.player.fps,
          boneCount: 51,
          loadedMotionFile: state.loaded.motionKey ? `${state.loaded.motionKey}/motion.vmd` : '',
        },
        actor: {
          actorLabel: state.actors.boundLabel || '尚未绑定角色',
          boundBoneCount: 51,
          totalBoneCount: 51,
        },
      }
      return snap
    },
    'sole.get': () => {
      ctx.requireCapability('sole.get')
      return { ...state.sole }
    },
    'sole.set': (p) => {
      ctx.requireCapability('sole.set')
      const nextSettings = p.settings as SoleState['settings']
      const err = validateSole(nextSettings)
      if (err) throw new BridgeError('invalid_params', err)
      state.sole = {
        ...state.sole,
        settings: nextSettings,
        generation: state.sole.generation + 1,
        notice: '已应用鞋底测量配置（mock）',
      }
      ctx.emit('sole.changed', { ...state.sole })
      return {}
    },
    'sole.save': () => {
      ctx.requireCapability('sole.save')
      state.sole = { ...state.sole, notice: '已保存鞋底设置（mock）' }
      ctx.emit('sole.changed', { ...state.sole })
      return {}
    },
    'sole.load': () => {
      ctx.requireCapability('sole.load')
      state.sole = { ...state.sole, notice: '已载入鞋底设置（mock）' }
      ctx.emit('sole.changed', { ...state.sole })
      return {}
    },
    'solePitch.get': () => {
      ctx.requireCapability('solePitch.get')
      return { ...state.solePitch }
    },
    'solePitch.set': (p) => {
      ctx.requireCapability('solePitch.set')
      state.solePitch = {
        ...state.solePitch,
        settings: { ...(p.settings as SolePitchState['settings']) },
      }
      ctx.emit('solePitch.changed', { ...state.solePitch })
      return {}
    },
    'studio.get': () => {
      ctx.requireCapability('studio.get')
      return { ...state.studio }
    },
    'studio.set': (p) => {
      ctx.requireCapability('studio.set')
      const nextSettings = p.settings as StudioSettings
      const err = validateStudio(nextSettings)
      if (err) throw new BridgeError('invalid_params', err)
      state.studio = {
        settings: nextSettings,
        revision: state.studio.revision + 1,
        lightingNotice: nextSettings.lighting.enabled ? '自定义布光已启用（mock）' : '',
        videoNotice: nextSettings.video.enabled ? '幕布视频已启用（mock）' : '',
      }
      ctx.emit('studio.changed', { ...state.studio })
      return {}
    },
    'studio.reset': () => {
      ctx.requireCapability('studio.reset')
      state.studio = {
        settings: defaultStudioSettings(),
        revision: state.studio.revision + 1,
        lightingNotice: '已恢复默认摄影棚配置',
        videoNotice: '',
      }
      ctx.emit('studio.changed', { ...state.studio })
      return {}
    },
  }
}
