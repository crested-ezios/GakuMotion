import { afterEach, describe, expect, it, vi } from 'vitest'
import { MotionMockBridgeClient, type MockCapabilityProfile } from '@/motion/mock'
import { BridgeError } from '@/motion/protocol'
import { MotionStore, shallowEqual } from '@/motion/store'

async function startedStore(capabilityProfile: MockCapabilityProfile = 'full') {
  const bridge = new MotionMockBridgeClient({ capabilityProfile })
  const store = new MotionStore(bridge)
  await store.start()
  return { bridge, store }
}

afterEach(() => {
  vi.restoreAllMocks()
})

describe('MotionStore startup', () => {
  it('negotiates full capabilities by default and supports phase1 degradation profile', async () => {
    const { bridge, store } = await startedStore('full')
    const state = store.getState()
    expect(state.ready).toBe(true)
    expect(state.connected).toBe(true)
    expect(state.bridgeKind).toBe('mock')
    expect(state.library.entries.length).toBe(5)
    expect(state.permission.injectionAllowed).toBe(true)
    expect(store.supports('player.play')).toBe(true)
    expect(store.supports('library.import')).toBe(true)
    expect(store.supports('actors.getPortrait')).toBe(true)
    expect(store.supports('formation.selectSlot')).toBe(true)
    expect(store.supports('formation.swapSlots')).toBe(true)
    bridge.close()

    const phase1 = await startedStore('phase1')
    expect(phase1.store.supports('player.play')).toBe(true)
    expect(phase1.store.supports('library.import')).toBe(false)
    expect(phase1.store.supports('dialog.pickFile')).toBe(false)
    expect(phase1.store.supports('actors.getPortrait')).toBe(false)
    expect(phase1.store.supports('formation.selectSlot')).toBe(false)
    phase1.bridge.close()
  })
})

describe('MotionStore transport', () => {
  it('plays, pauses and stops a loaded motion', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', true)
    expect(store.getState().player.state).toBe('playing')
    await store.pause()
    expect(store.getState().player.state).toBe('paused')
    await store.play()
    expect(store.getState().player.state).toBe('playing')
    await store.stop()
    expect(store.getState().player.state).toBe('stopped')
    expect(store.getState().player.time).toBe(0)
    bridge.close()
  })

  it('seeks relative within clip bounds via seekRelative', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', false)
    await store.seekRelative(10)
    expect(store.getState().player.time).toBeCloseTo(10)
    await store.seekRelative(-25)
    expect(store.getState().player.time).toBe(0)
    await store.seekRelative(9999)
    expect(store.getState().player.time).toBeCloseTo(store.getState().player.duration)
    bridge.close()
  })

  it('updates UI filter, sort, and precision panel preferences locally', async () => {
    const { bridge, store } = await startedStore()
    store.setLibraryFilter('camera')
    store.setLibrarySort('durationDesc')
    store.setSeekStepSeconds(10)
    store.setPrecisionPanelOpen(true)
    store.setPrecisionPanelPinned(true)
    expect(store.getState().libraryFilter).toBe('camera')
    expect(store.getState().librarySort).toBe('durationDesc')
    expect(store.getState().seekStepSeconds).toBe(10)
    expect(store.getState().precisionPanelOpen).toBe(true)
    expect(store.getState().precisionPanelPinned).toBe(true)
    bridge.close()
  })

  it('sends the three-stage scrub sequence', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', false)
    const spy = vi.spyOn(bridge, 'request')
    store.scrubStart(10)
    store.scrubMove(20)
    store.scrubCommit(30)
    expect(spy.mock.calls.map((call) => call[0])).toEqual([
      'player.scrubBegin',
      'player.scrubUpdate',
      'player.scrubEnd',
    ])
    expect(store.getState().scrub.active).toBe(false)
    bridge.close()
  })

  it('drops an active scrub without committing on permission loss', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', true)
    store.scrubStart(10)
    const spy = vi.spyOn(bridge, 'request')
    bridge.setPermission(false, '截图保护未就绪')
    expect(store.getState().scrub.active).toBe(false)
    expect(store.getState().permission.injectionAllowed).toBe(false)
    expect(spy.mock.calls.map((call) => call[0])).not.toContain('player.scrubEnd')
    bridge.close()
  })

  it('drops scrub when the loaded motion identity changes', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', true)
    store.scrubStart(10)
    await store.selectMotion('starlight-step', true)
    expect(store.getState().scrub.active).toBe(false)
    bridge.close()
  })

  it('validates time and frame navigation inputs', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', false)
    await store.seekTime('abc')
    expect(store.getState().navigationError).toContain('时间格式无效')
    await store.seekTime('99999')
    expect(store.getState().navigationError).toContain('时间超出演出时长')
    await store.seekTime('00:10.000')
    expect(store.getState().navigationError).toBeNull()
    expect(store.getState().player.time).toBeCloseTo(10)
    await store.seekFrame('-1')
    expect(store.getState().navigationError).toContain('帧号格式无效')
    await store.seekFrame('999999')
    expect(store.getState().navigationError).toContain('帧号超出范围')
    await store.seekFrame('60')
    expect(store.getState().navigationError).toBeNull()
    expect(store.getState().player.state).toBe('paused')
    bridge.close()
  })

  it('saves volume only on commit', async () => {
    const { bridge, store } = await startedStore()
    const spy = vi.spyOn(bridge, 'request')
    store.setVolume(40)
    expect(spy.mock.calls.map((call) => call[0])).not.toContain('player.saveVolume')
    store.commitVolume(40)
    const methods = spy.mock.calls.map((call) => call[0])
    expect(methods).toContain('player.setVolume')
    expect(methods).toContain('player.saveVolume')
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(bridge.savedVolume()).toBeCloseTo(0.4)
    bridge.close()
  })
})

describe('MotionStore library guards', () => {
  it('selects with the current revision and reports stale revisions', async () => {
    const { bridge, store } = await startedStore()
    await store.selectMotion('hikari-no-saki', true)
    expect(store.getState().loaded.motionKey).toBe('hikari-no-saki')
    await bridge.request('library.delete', { key: 'encore-demo', revision: store.getState().library.revision })
    vi.spyOn(bridge, 'request').mockImplementationOnce(async () => {
      throw new BridgeError('stale_revision', '曲库已变化')
    })
    await store.selectMotion('hikari-no-saki', true)
    expect(store.getState().actionError).toContain('曲库已变化')
    bridge.close()
  })

  it('deletes through the confirmation flow with a revision guard', async () => {
    const { bridge, store } = await startedStore()
    store.requestDelete('encore-demo')
    expect(store.getState().pendingDelete?.key).toBe('encore-demo')
    expect(store.deleteGuardChanged()).toBe(false)
    await store.confirmDelete()
    expect(store.getState().pendingDelete).toBeNull()
    expect(store.getState().library.entries.some((entry) => entry.id === 'encore-demo')).toBe(false)
    bridge.close()
  })

  it('blocks deletion when the library changed after the dialog opened', async () => {
    const { bridge, store } = await startedStore()
    store.requestDelete('encore-demo')
    await bridge.request('library.delete', { key: 'practice-room', revision: store.getState().library.revision })
    expect(store.deleteGuardChanged()).toBe(true)
    const spy = vi.spyOn(bridge, 'request')
    await store.confirmDelete()
    expect(spy).not.toHaveBeenCalled()
    expect(store.getState().pendingDelete).not.toBeNull()
    bridge.close()
  })

  it('marks the dialog stale when native rejects with stale_revision', async () => {
    const { bridge, store } = await startedStore()
    store.requestDelete('encore-demo')
    vi.spyOn(bridge, 'request').mockImplementationOnce(async () => {
      throw new BridgeError('stale_revision', '曲库已变化')
    })
    await store.confirmDelete()
    expect(store.getState().deleteStale).toBe(true)
    expect(store.deleteGuardChanged()).toBe(true)
    bridge.close()
  })

  it('cycles play modes through cyclePlayMode cleanly', async () => {
    const { bridge, store } = await startedStore()
    const initial = store.getState().player.playMode
    await store.cyclePlayMode()
    expect(store.getState().player.playMode).toBe((initial + 1) % 4)
    bridge.close()
  })
})

describe('MotionStore camera and stage', () => {
  it('runs the draft-expected-pending camera acknowledgement', async () => {
    const { bridge, store } = await startedStore()
    const promise = store.setCameraQuickMode(2)
    expect(store.getState().cameraPending).toBe(true)
    await promise
    expect(store.getState().cameraPending).toBe(false)
    expect(store.getState().camera.settings.mode).toBe(2)
    expect(store.getState().camera.settings.followBehavior).toBe(1)
    expect(store.cameraQuickModeIndex()).toBe(2)
    bridge.close()
  })

  it('keeps the lighting→camera shadow linkage via stage.set', async () => {
    const { bridge, store } = await startedStore()
    store.patchStage({ lightingEffectsEnabled: false })
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(store.getState().stage.settings.lightingEffectsEnabled).toBe(false)
    expect(store.getState().camera.settings.actorShadowExperimentMode).toBe(2)
    store.patchStage({ lightingEffectsEnabled: true })
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(store.getState().camera.settings.actorShadowExperimentMode).toBe(1)
    bridge.close()
  })

  it('selects actors with token+epoch and reports stale rosters', async () => {
    const { bridge, store } = await startedStore()
    await store.selectActor(102)
    expect(store.getState().actors.targetLabel).toBe('月村手毬 (ttmr) · 2')
    vi.spyOn(bridge, 'request').mockImplementationOnce(async () => {
      throw new BridgeError('stale_token', '角色名单已变化')
    })
    await store.selectActor(101)
    expect(store.getState().actionError).toContain('角色名单已变化')
    bridge.close()
  })

  it('compares shallow records and arrays accurately in shallowEqual', () => {
    expect(shallowEqual({ a: 1, b: 'x' }, { a: 1, b: 'x' })).toBe(true)
    expect(shallowEqual({ a: 1, b: 'x' }, { a: 2, b: 'x' })).toBe(false)
    expect(shallowEqual({ a: 1 }, { a: 1, b: 2 })).toBe(false)
    expect(shallowEqual([1, 2, 3], [1, 2, 3])).toBe(true)
    expect(shallowEqual([1, 2, 3], [1, 2, 4])).toBe(false)
    expect(shallowEqual(null, {})).toBe(false)
  })

  it('skips drop polling while document.hidden is true and resumes when visible', async () => {
    const { bridge, store } = await startedStore()
    const spy = vi.spyOn(bridge, 'request')
    const hiddenSpy = vi.spyOn(document, 'hidden', 'get').mockReturnValue(true)

    store.retainDropWatch()
    await new Promise((resolve) => setTimeout(resolve, 40))
    const hiddenCalls = spy.mock.calls.filter((c) => c[0] === 'drop.peek' || c[0] === 'drop.poll')
    expect(hiddenCalls.length).toBe(0)

    hiddenSpy.mockReturnValue(false)
    document.dispatchEvent(new Event('visibilitychange'))
    await new Promise((resolve) => setTimeout(resolve, 40))
    const visibleCalls = spy.mock.calls.filter((c) => c[0] === 'drop.peek' || c[0] === 'drop.poll')
    expect(visibleCalls.length).toBeGreaterThan(0)

    store.releaseDropWatch()
    bridge.close()
  })
})
