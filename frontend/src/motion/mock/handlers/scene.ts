import {
  BridgeError,
  type ActorPortraitResult,
  type CameraSettings,
  type MotionStateSnapshot,
} from '../../protocol'
import { clamp } from '../../utils'
import type { MockHandlerContext, MockHandlerMap } from '../context'
import { MOCK_COVER_PNG, MOCK_KNOWN_IDOL_CODES, buildMockFormation } from '../seed'

export function createSceneHandlers(ctx: MockHandlerContext): MockHandlerMap {
  const { state } = ctx
  return {
    'actors.selectTarget': (p) => {
      ctx.requireAllowed()
      if (Number(p.epoch) !== state.actors.epoch) throw new BridgeError('stale_token', '角色名单已变化')
      const actor = state.actors.actors.find((candidate) => candidate.token === Number(p.token))
      if (!actor) throw new BridgeError('stale_token', '角色不存在')
      state.actors = {
        ...state.actors,
        actors: state.actors.actors.map((candidate) => ({ ...candidate, selected: candidate.token === actor.token })),
        targetLabel: actor.label,
      }
      ctx.emit('actors.changed', { ...state.actors, actors: state.actors.actors.map((a) => ({ ...a })) })
      return {}
    },
    'actors.getPortrait': (p) => {
      ctx.requireCapability('actors.getPortrait')
      let normCode = ''
      if ('character' in p) {
        if (typeof p.character !== 'string' || !/^[a-zA-Z0-9_-]{1,32}$/.test(p.character)) {
          throw new BridgeError('invalid_params', 'character code is invalid')
        }
        normCode = p.character.toLowerCase()
      } else if ('token' in p) {
        const tok = p.token
        if (typeof tok !== 'number' || !Number.isInteger(tok) || tok <= 0) {
          throw new BridgeError('invalid_params', 'token must be a positive integer')
        }
        const actor = state.actors.actors.find((a) => a.token === tok)
        normCode = actor && /^[a-zA-Z0-9_-]{1,32}$/.test(actor.character) ? actor.character.toLowerCase() : ''
        if (!normCode) {
          return { ok: true, character: '', cached: false, dataUri: null } satisfies ActorPortraitResult
        }
      } else {
        throw new BridgeError('invalid_params', 'character or token is required')
      }
      const hit = MOCK_KNOWN_IDOL_CODES.has(normCode)
      return {
        ok: true,
        character: normCode,
        cached: hit,
        dataUri: hit ? MOCK_COVER_PNG : null,
      } satisfies ActorPortraitResult
    },
    'formation.selectSlot': (p) => {
      ctx.requireCapability('formation.selectSlot')
      const idx = p.slotIndex
      if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= state.formation.slots.length) {
        throw new BridgeError('invalid_params', 'slotIndex is out of range')
      }
      ctx.requireAllowed()
      state.formation = {
        ...state.formation,
        selectedSlotIndex: idx,
        revision: state.formation.revision + 1,
      }
      const selSlot = state.formation.slots[idx]
      if (selSlot) {
        state.facial = { ...state.facial, weights: { ...selSlot.facialWeights } }
        state.position = {
          ...state.position,
          offset: { x: selSlot.offset.x, y: selSlot.offset.y, z: selSlot.offset.z },
        }
        ctx.emit('facial.changed', { weights: { ...state.facial.weights }, notice: state.facial.notice })
        ctx.emit('position.changed', { offset: { ...state.position.offset }, notice: state.position.notice })
      }
      ctx.emitFormation()
      return {}
    },
    'formation.swapSlots': (p) => {
      ctx.requireCapability('formation.swapSlots')
      const from = p.fromSlotIndex
      const to = p.toSlotIndex
      if (
        typeof from !== 'number' ||
        typeof to !== 'number' ||
        !Number.isInteger(from) ||
        !Number.isInteger(to) ||
        from < 0 ||
        to < 0 ||
        from === to ||
        from >= state.formation.slots.length ||
        to >= state.formation.slots.length
      ) {
        throw new BridgeError('invalid_params', 'fromSlotIndex and toSlotIndex must be distinct valid indices')
      }
      ctx.requireAllowed()
      const slots = state.formation.slots.map((s) => ({
        ...s,
        offset: { ...s.offset },
        facialWeights: { ...s.facialWeights },
      }))
      const a = slots[from]
      const b = slots[to]
      slots[from] = {
        ...a,
        occupied: b.occupied,
        actorToken: b.actorToken,
        actorInstanceId: b.actorInstanceId,
        character: b.character,
        shortName: b.shortName,
        displayName: b.displayName,
        facialWeights: { ...b.facialWeights },
        hasActorBoneOverrides: b.hasActorBoneOverrides,
        physicsBackend: b.physicsBackend,
      }
      slots[to] = {
        ...b,
        occupied: a.occupied,
        actorToken: a.actorToken,
        actorInstanceId: a.actorInstanceId,
        character: a.character,
        shortName: a.shortName,
        displayName: a.displayName,
        facialWeights: { ...a.facialWeights },
        hasActorBoneOverrides: a.hasActorBoneOverrides,
        physicsBackend: a.physicsBackend,
      }
      state.formation = {
        ...state.formation,
        slots,
        revision: state.formation.revision + 1,
      }
      ctx.emitFormation()
      return {}
    },
    'formation.setSlotOffset': (p) => {
      ctx.requireCapability('formation.setSlotOffset')
      const idx = p.slotIndex
      const x = p.x
      const y = p.y
      const z = p.z
      const yaw = 'yawDegrees' in p ? p.yawDegrees : 0
      if (
        typeof idx !== 'number' ||
        !Number.isInteger(idx) ||
        idx < 0 ||
        idx >= state.formation.slots.length ||
        typeof x !== 'number' ||
        !Number.isFinite(x) ||
        x < -10 ||
        x > 10 ||
        typeof y !== 'number' ||
        !Number.isFinite(y) ||
        y < -10 ||
        y > 10 ||
        typeof z !== 'number' ||
        !Number.isFinite(z) ||
        z < -10 ||
        z > 10 ||
        typeof yaw !== 'number' ||
        !Number.isFinite(yaw) ||
        yaw < -360 ||
        yaw > 360
      ) {
        throw new BridgeError('invalid_params', 'Invalid formation slot offset parameters')
      }
      ctx.requireAllowed()
      const slots = state.formation.slots.map((s, i) =>
        i === idx
          ? {
              ...s,
              offset: { x, y, z, yawDegrees: yaw },
              placementDirty: true,
            }
          : s,
      )
      state.formation = { ...state.formation, slots, revision: state.formation.revision + 1 }
      if (idx === state.formation.selectedSlotIndex) {
        state.position = { ...state.position, offset: { x, y, z } }
        ctx.emit('position.changed', { ...state.position, offset: { ...state.position.offset } })
      }
      ctx.emitFormation()
      return {}
    },
    'formation.resetSlotOffset': (p) => {
      ctx.requireCapability('formation.resetSlotOffset')
      const idx = p.slotIndex
      if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= state.formation.slots.length) {
        throw new BridgeError('invalid_params', 'slotIndex is out of range')
      }
      ctx.requireAllowed()
      const loadedEntry = state.entries.find((e) => e.id === state.loaded.motionKey)
      const defaults = buildMockFormation(state.actors.actors, state.actors.epoch, loadedEntry, true)
      const defOff = defaults.slots[idx]?.offset ?? { x: 0, y: 0, z: 0, yawDegrees: 0 }
      const slots = state.formation.slots.map((s, i) =>
        i === idx ? { ...s, offset: { ...defOff }, placementDirty: false } : s,
      )
      state.formation = { ...state.formation, slots, revision: state.formation.revision + 1 }
      ctx.emitFormation()
      return {}
    },
    'formation.saveSlotOffset': (p) => {
      ctx.requireCapability('formation.saveSlotOffset')
      const idx = p.slotIndex
      if (typeof idx !== 'number' || !Number.isInteger(idx) || idx < 0 || idx >= state.formation.slots.length) {
        throw new BridgeError('invalid_params', 'slotIndex is out of range')
      }
      ctx.requireAllowed()
      const slot = state.formation.slots[idx]
      if (idx >= 1 && state.formation.isGroupSong && idx < state.formation.trackPositionCount) {
        const loadedEntry = state.entries.find((e) => e.id === state.loaded.motionKey)
        if (loadedEntry) {
          const pos = loadedEntry.positions.find((entryPos) => entryPos.slot === idx + 1)
          if (pos && slot) {
            pos.x = slot.offset.x
            pos.y = slot.offset.y
            pos.z = slot.offset.z
            pos.yawDegrees = slot.offset.yawDegrees
          }
        }
      }
      const slots = state.formation.slots.map((s, i) => (i === idx ? { ...s, placementDirty: false } : s))
      state.formation = {
        ...state.formation,
        slots,
        revision: state.formation.revision + 1,
        notice: '已保存槽位初始位置',
      }
      ctx.emitFormation()
      return {}
    },
    'formation.setSlotSource': (p) => {
      ctx.requireCapability('formation.setSlotSource')
      const idx = p.slotIndex
      const src = p.sourceSlotIndex
      const maxSource = Math.max(1, state.formation.trackPositionCount)
      if (
        typeof idx !== 'number' ||
        !Number.isInteger(idx) ||
        idx < 0 ||
        idx >= state.formation.slots.length ||
        typeof src !== 'number' ||
        !Number.isInteger(src) ||
        src < 0 ||
        src >= maxSource
      ) {
        throw new BridgeError('invalid_params', 'slotIndex or sourceSlotIndex is out of range')
      }
      ctx.requireAllowed()
      const slots = state.formation.slots.map((s, i) => (i === idx ? { ...s, sourceSlotIndex: src } : s))
      state.formation = { ...state.formation, slots, revision: state.formation.revision + 1 }
      ctx.emitFormation()
      return {}
    },
    'facial.setWeights': (p) => {
      ctx.requireAllowed()
      const nextWeights = {
        overall: clamp(Number(p.overall), 0, 2),
        brow: clamp(Number(p.brow), 0, 2),
        eye: clamp(Number(p.eye), 0, 2),
        mouth: clamp(Number(p.mouth), 0, 2),
      }
      state.facial = {
        weights: nextWeights,
        notice: '',
      }
      const selIdx = state.formation.selectedSlotIndex
      if (selIdx >= 0 && selIdx < state.formation.slots.length) {
        const slots = state.formation.slots.map((s, i) =>
          i === selIdx ? { ...s, facialWeights: { ...nextWeights } } : s,
        )
        state.formation = { ...state.formation, slots, revision: state.formation.revision + 1 }
        ctx.emitFormation()
      }
      ctx.emit('facial.changed', { weights: { ...state.facial.weights }, notice: '' })
      return {}
    },
    'facial.save': () => {
      state.facial = { ...state.facial, notice: '已保存表情幅度（mock）' }
      ctx.emit('facial.changed', { weights: { ...state.facial.weights }, notice: state.facial.notice })
      return {}
    },
    'position.set': (p) => {
      ctx.requireAllowed()
      state.position = {
        offset: { x: clamp(Number(p.x), -10, 10), y: clamp(Number(p.y), -10, 10), z: clamp(Number(p.z), -10, 10) },
        notice: '',
      }
      ctx.emit('position.changed', { offset: { ...state.position.offset }, notice: '' })
      return {}
    },
    'position.save': () => {
      state.position = { ...state.position, notice: '已保存初始位置为默认（mock）' }
      ctx.emit('position.changed', { offset: { ...state.position.offset }, notice: state.position.notice })
      return {}
    },
    'camera.set': (p) => {
      ctx.requireAllowed()
      const nextCam = p.settings as CameraSettings
      if (
        nextCam &&
        'followSlotIndex' in nextCam &&
        (typeof nextCam.followSlotIndex !== 'number' ||
          !Number.isInteger(nextCam.followSlotIndex) ||
          nextCam.followSlotIndex < -1)
      ) {
        throw new BridgeError('invalid_params', 'followSlotIndex must be an integer >= -1')
      }
      state.camera = {
        ...state.camera,
        settings: { ...nextCam },
        owned: nextCam.enabled,
      }
      ctx.emit('camera.changed', { ...state.camera, settings: { ...state.camera.settings } })
      return {}
    },
    'camera.save': () => {
      state.camera = { ...state.camera, notice: '已保存镜头设置（mock）' }
      ctx.emit('camera.changed', { ...state.camera, settings: { ...state.camera.settings } })
      return {}
    },
    'stage.set': (p) => {
      ctx.requireAllowed()
      const settings = { ...(p.settings as MotionStateSnapshot['stage']['settings']) }
      state.stage = { ...state.stage, settings }
      ctx.emit('stage.changed', { ...state.stage, settings: { ...settings } })
      const shadowMode = settings.lightingEffectsEnabled ? 1 : 2
      if (state.camera.settings.actorShadowExperimentMode !== shadowMode) {
        state.camera = {
          ...state.camera,
          settings: { ...state.camera.settings, actorShadowExperimentMode: shadowMode },
        }
        ctx.emit('camera.changed', { ...state.camera, settings: { ...state.camera.settings } })
      }
      return {}
    },
    'settings.setPhysics': (p) => {
      const backend = Number(p.backend)
      if (backend < 0 || backend > 2) throw new BridgeError('invalid_params', '无效的物理后端')
      state.physicsBackend = backend
      ctx.emit('settings.changed', { physicsBackend: backend })
      return {}
    },
    'shell.setTextInputActive': () => ({}),
    'appearance.get': () => ({ ...state.appearanceState }),
  }
}
