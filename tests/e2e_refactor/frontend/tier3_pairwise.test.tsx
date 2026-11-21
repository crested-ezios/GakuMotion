import { fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import MotionApp from '@/motion/components/MotionApp'
import { LibraryPage } from '@/motion/pages/LibraryPage'
import { MotionPage } from '@/motion/pages/MotionPage'
import { CameraPage } from '@/motion/pages/CameraPage'
import { BridgeError, type MotionBridgeClient } from '@/motion/protocol'
import { setMotionBridge } from '@/motion/bridge'
import { motionStore } from '@/motion/store'
import {
  RealWireReplayBridge,
  assertNoUnexpectedLegacyText,
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  loadCppBridgeFixture,
  motionFileExists,
  probeCrossLayerReady,
} from './helpers'

let bridge: MotionBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

describe('Tier 3 — Cross-Layer & Pairwise Interactions (F13, F14, F15, F16)', () => {
  e2eIt(3, 'T3-FE-01', 'F15', 'F14<->F15: Selecting 4-position group song in Library propagates slotCount & offsets to ActorSlotStrip & BoneSection', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF14: true, requireF15: true }))) return
    const mock = await createMockSession()
    bridge = mock
    const spy = vi.spyOn(mock, 'request')
    const user = userEvent.setup()

    mock.push('library.changed', {
      revision: 20,
      entries: [
        {
          id: 'group-4p',
          title: 'Four Idol Stage',
          subtitle: '初星学園',
          directory: 'gakumas-local/motions/group-4p',
          durationSeconds: 180,
          totalFrames: 10800,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: true,
          positionCount: 4,
          positions: [
            { slot: 1, x: 0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: true },
            { slot: 2, x: 1.1, y: 0, z: 0.3, yawDegrees: 10, hasMotion: true, hasFacial: true },
            { slot: 3, x: -1.1, y: 0, z: 0.3, yawDegrees: -10, hasMotion: true, hasFacial: true },
            { slot: 4, x: 2.0, y: 0, z: 0.6, yawDegrees: 20, hasMotion: true, hasFacial: false },
          ],
        },
      ],
    } as never)

    render(<MotionApp />)
    await user.dblClick(await screen.findByTestId('library-row-group-4p'))
    await user.click(screen.getByRole('button', { name: '动作' }))

    await waitFor(() => expect(screen.getByText('4号位')).toBeInTheDocument())
    await user.click(screen.getByText('高级动作设置'))
    await user.click(screen.getByRole('button', { name: '保存至当前曲目' }))
    expect(spy.mock.calls.some((c) => c[0] === 'bone.saveToMotion')).toBe(true)
  })

  e2eIt(3, 'T3-FE-02', 'F16', 'F14<->F16: Switching solo vs 4-position group song updates CameraPage followSlotIndex options', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF14: true, requireF16: true }))) return
    const mock = await createMockSession()
    bridge = mock
    const user = userEvent.setup()

    mock.push('library.changed', {
      revision: 21,
      entries: [
        {
          id: 'solo-song',
          title: 'Solo Song',
          subtitle: '咲季',
          directory: 'gakumas-local/motions/solo-song',
          durationSeconds: 120,
          totalFrames: 7200,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: false,
          positionCount: 1,
        },
        {
          id: 'group-4p',
          title: 'Group 4P',
          subtitle: '初星学園',
          directory: 'gakumas-local/motions/group-4p',
          durationSeconds: 180,
          totalFrames: 10800,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: true,
          positionCount: 4,
          positions: [1, 2, 3, 4].map((slot) => ({
            slot,
            x: 0,
            y: 0,
            z: 0,
            yawDegrees: 0,
            hasMotion: true,
            hasFacial: true,
          })),
        },
      ],
    } as never)

    render(<MotionApp />)
    await user.dblClick(await screen.findByTestId('library-row-group-4p'))
    await user.click(screen.getByRole('button', { name: '镜头' }))
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))

    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    for (const label of ['全部', '1号位', '2号位', '3号位', '4号位']) {
      expect(await screen.findByRole('option', { name: label })).toBeInTheDocument()
    }
  })

  e2eIt(3, 'T3-FE-03', 'F15', 'F15<->F16: Swapping slots in ActorSlotStrip syncs with CameraPage slot follow and verifies cleanup', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF15: true, requireF16: true }))) return
    const mock = await createMockSession()
    bridge = mock
    const spy = vi.spyOn(mock, 'request')
    const user = userEvent.setup()

    expect(motionFileExists('components/EnsembleSection.tsx')).toBe(false)
    render(<MotionApp />)

    await user.click(screen.getByRole('button', { name: '动作' }))
    const slot1 = (await screen.findByText('1号位')).closest('[draggable="true"]') ?? screen.getByText('1号位').parentElement!
    const slot3 = screen.getByText('3号位').closest('[draggable="true"], [data-slot-index]') ?? screen.getByText('3号位').parentElement!
    fireEvent.dragStart(slot1)
    fireEvent.dragOver(slot3)
    fireEvent.drop(slot3)
    await waitFor(() => expect(spy.mock.calls.some((c) => (c[0] as string) === 'formation.swapSlots')).toBe(true))

    await user.click(screen.getByRole('button', { name: '镜头' }))
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    await user.click(await screen.findByRole('option', { name: '3号位' }))
    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            c[0] === 'camera.set' &&
            (c[1] as { settings?: { followSlotIndex?: number } }).settings?.followSlotIndex === 2,
        ),
      ).toBe(true),
    )

    await user.click(screen.getByRole('button', { name: '舞台' }))
    assertNoUnexpectedLegacyText(document.body)
  })

  e2eIt(3, 'T3-FE-04', 'F13', 'F13<->F14: Real C++ GkWebPanelBridge wire JSON drives LibraryPage 团曲 badge & ImportDialog', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF14: true, requireCpp: true }))) return
    const fixture = loadCppBridgeFixture()
    const replay = new RealWireReplayBridge(fixture)
    bridge = replay
    setMotionBridge(replay)
    motionStore.reset()
    await motionStore.start()
    replay.replayCapturedEvents()

    render(<LibraryPage />)
    const row = await screen.findByTestId('library-row-group-live-3p')
    expect(within(row).getByText('团曲')).toBeInTheDocument()
  })

  e2eIt(3, 'T3-FE-05', 'F13', 'F13<->F15: Real C++ GkWebPanelBridge formation & portrait wire JSON drives MotionPage', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF15: true, requireCpp: true }))) return
    const fixture = loadCppBridgeFixture()
    const replay = new RealWireReplayBridge(fixture)
    bridge = replay
    setMotionBridge(replay)
    motionStore.reset()
    await motionStore.start()
    replay.replayCapturedEvents()

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('1号位')).toBeInTheDocument())
    expect(screen.getByText('2号位')).toBeInTheDocument()
    expect(screen.getByText('3号位')).toBeInTheDocument()
  })

  e2eIt(3, 'T3-FE-06', 'F13', 'F13<->F16: Real C++ GkWebPanelBridge camera.set & camera.changed wire JSON round-trips in CameraPage', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF16: true, requireCpp: true }))) return
    const fixture = loadCppBridgeFixture()
    expect(fixture.responses?.['camera.set.all']?.ok).toBe(true)
    expect(fixture.responses?.['camera.set.slot2']?.ok).toBe(true)
    expect(fixture.events?.some((e) => e.name === 'camera.changed')).toBe(true)

    const replay = new RealWireReplayBridge(fixture)
    bridge = replay
    const spy = vi.spyOn(replay, 'request')
    setMotionBridge(replay)
    motionStore.reset()
    await motionStore.start()
    replay.replayCapturedEvents()

    const user = userEvent.setup()
    render(<CameraPage />)
    await waitFor(() => expect(screen.getByTestId('camera-page')).toBeInTheDocument())
    expect(motionStore.getState().cameraPending).toBe(false)

    // Verify replayed C++ camera.changed state is reflected (mode=2 跟随, followSlotIndex=1 -> 2号位)
    const replayedCam = motionStore.getState().camera.settings as unknown as {
      mode?: number
      followSlotIndex?: number
    }
    expect(replayedCam.mode).toBe(2)
    expect(replayedCam.followSlotIndex).toBe(1)

    // Ensure 跟随 mode exposes "全部" (-1) and 1号位..3号位 options and can switch to 全部
    let slotCombo = screen.queryByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ })
    if (!slotCombo) {
      await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
      await user.click(await screen.findByRole('option', { name: '跟随' }))
      await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
      slotCombo = screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ })
    }

    await user.click(slotCombo)
    for (const label of ['全部', '1号位', '2号位', '3号位']) {
      expect(await screen.findByRole('option', { name: label })).toBeInTheDocument()
    }
    await user.click(screen.getByRole('option', { name: '全部' }))
    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            c[0] === 'camera.set' &&
            (c[1] as { settings?: { followSlotIndex?: number } }).settings?.followSlotIndex === -1,
        ),
      ).toBe(true),
    )
  })

  e2eIt(3, 'T3-FE-07', 'F14', 'F13<->F14<->F15: Structured protected/stale_revision/stale_token errors propagate cleanly without crashing UI', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF14: true, requireF15: true }))) return
    const mock = await createMockSession()
    bridge = mock
    const origReq = mock.request.bind(mock)

    vi.spyOn(mock, 'request').mockImplementation(async (method, params) => {
      if (method === 'library.select') {
        throw new BridgeError('stale_revision', '曲库版本已过期')
      }
      if ((method as string) === 'formation.swapSlots') {
        throw new BridgeError('stale_token', '角色列表已刷新，请重试')
      }
      if (method === 'library.import') {
        throw new BridgeError('protected', '正在播放时禁止覆盖当前曲目')
      }
      return origReq(method, params as never)
    })

    const user = userEvent.setup()
    render(<MotionApp />)
    await motionStore.selectMotion('hikari-no-saki', true).catch(() => {})
    await waitFor(() => expect(motionStore.getState().navigationError || motionStore.getState().actionError).toBeTruthy())

    // Ensure ImportDialog preserves unsaved title on error
    await motionStore.openImportNew(['C:\\Test\\01_a.vmd', 'C:\\Test\\02_b.vmd'])
    const dialog = await screen.findByTestId('import-dialog')
    const titleInput = within(dialog).getByLabelText('曲目标题')
    await user.clear(titleInput)
    await user.type(titleInput, 'KeepDraftTitle')
    await user.click(within(dialog).getByRole('button', { name: '导入' }))
    await waitFor(() => expect(within(dialog).getByLabelText('曲目标题')).toHaveValue('KeepDraftTitle'))
  })
})
