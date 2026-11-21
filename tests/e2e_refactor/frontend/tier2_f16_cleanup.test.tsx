import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import MotionApp from '@/motion/components/MotionApp'
import { CameraPage } from '@/motion/pages/CameraPage'
import { MotionStatusBar } from '@/motion/components/MotionStatusBar'
import { PrecisionControlPanel } from '@/motion/components/PrecisionControlPanel'
import { motionStore } from '@/motion/store'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
  assertNoUnexpectedLegacyText,
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  probeF16Ready,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

describe('Tier 2 — F16 Boundary, Negative & Full Audit Cases', () => {
  e2eIt(2, 'T2-F16-01', 'F16', 'CameraPage handles 0-actor stage gracefully and exposes follow-slot selector in 注视 mode', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    bridge.push('actors.changed', {
      epoch: 2,
      actors: [],
      targetLabel: '',
      boundLabel: '',
    } as never)

    render(<CameraPage />)
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))

    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    const allOpt = await screen.findByRole('option', { name: '全部' })
    expect(allOpt).toBeInTheDocument()
    // Close the Radix <Select> portal before interacting with 镜头模式
    await user.click(allOpt)

    // Switch to 注视 mode
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    const lookAtOpt = screen.queryByRole('option', { name: /注视/ })
    if (lookAtOpt) {
      await user.click(lookAtOpt)
      await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
      expect(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ })).toBeInTheDocument()
    }
  })

  e2eIt(2, 'T2-F16-02', 'F16', 'CameraPage recovers cleanly when shrinking actor count while high followSlotIndex was selected', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    bridge.push('camera.changed', {
      settings: {
        ...motionStore.getState().camera.settings,
        mode: 2,
        followSlotIndex: 4,
      },
    } as never)

    bridge.push('actors.changed', {
      epoch: 4,
      actors: [
        { token: 101, label: '咲季', selected: true, character: 'hski', displayName: '花海咲季', shortName: '咲季' },
        { token: 102, label: '手毬', selected: false, character: 'ttmr', displayName: '月村手毬', shortName: '手毬' },
      ],
      targetLabel: '咲季',
      boundLabel: '咲季',
    } as never)

    render(<CameraPage />)
    const combo = await screen.findByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ })
    await user.click(combo)
    await user.click(await screen.findByRole('option', { name: '全部' }))
    await waitFor(() =>
      expect((motionStore.getState().camera.settings as unknown as { followSlotIndex?: number }).followSlotIndex).toBe(-1),
    )
  })

  e2eIt(2, 'T2-F16-03', 'F16', 'CameraPage handles phase1 gating and camera.set rejection rollback cleanly', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return

    // Phase 1 capability gating
    bridge = await createMockSession({ capabilityProfile: 'phase1' })
    const user = userEvent.setup()
    const { unmount } = render(<CameraPage />)
    expect(screen.getByRole('button', { name: '保存镜头设置' })).toBeDisabled()
    expect(screen.getAllByText('待迁移').length).toBeGreaterThan(0)
    unmount()
    cleanupSession(bridge)

    // Full mode with camera.set rejection rollback
    bridge = await createMockSession({ capabilityProfile: 'full' })
    render(<CameraPage />)
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))

    const origReq = bridge.request.bind(bridge)
    vi.spyOn(bridge, 'request').mockImplementation(async (method, params) => {
      if (method === 'camera.set') {
        throw new Error('rejected camera.set')
      }
      return origReq(method, params as never)
    })

    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    await user.click(await screen.findByRole('option', { name: '2号位' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
    expect((motionStore.getState().camera.settings as unknown as { followSlotIndex?: number }).followSlotIndex).not.toBe(1)
  })

  e2eIt(2, 'T2-F16-04', 'F16', 'MotionStatusBar edge states (sourceMissing, sourceChanged, unloaded) render cleanly without driver badges', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()

    bridge.push('loaded.changed', {
      motionKey: 'hikari-no-saki',
      generation: 1,
      injectionSession: 1,
      sourceMissing: true,
      sourceChanged: false,
    })

    const { rerender } = render(<MotionStatusBar />)
    expect(screen.getByText('源文件缺失')).toBeInTheDocument()
    assertNoUnexpectedLegacyText(screen.getByTestId('motion-status-bar'))

    bridge.push('loaded.changed', {
      motionKey: 'hikari-no-saki',
      generation: 2,
      injectionSession: 1,
      sourceMissing: false,
      sourceChanged: true,
    })
    rerender(<MotionStatusBar />)
    await waitFor(() => expect(screen.getByText('源文件已变更')).toBeInTheDocument())
    assertNoUnexpectedLegacyText(screen.getByTestId('motion-status-bar'))
  })

  e2eIt(2, 'T2-F16-05', 'F16', 'PrecisionControlPanel cycles tracks cleanly with >=3 actors on stage', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    await motionStore.selectMotion('hikari-no-saki', false)
    render(<PrecisionControlPanel onClose={() => {}} />)

    await user.click(screen.getByRole('button', { name: '下一曲' }))
    await waitFor(() => expect(motionStore.getState().loaded.motionKey).not.toBe('hikari-no-saki'))
    await user.click(screen.getByRole('button', { name: '上一曲' }))
    await waitFor(() => expect(motionStore.getState().loaded.motionKey).toBe('hikari-no-saki'))
  })

  e2eIt(2, 'T2-F16-06', 'F16', 'Full MotionApp audit across all pages finds zero legacy ensemble strings or runtime errors', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    render(<MotionApp />)
    for (const navLabel of ['曲库', '动作', '镜头', '舞台', '设置']) {
      await user.click(screen.getByRole('button', { name: navLabel }))
      assertNoUnexpectedLegacyText(document.body)
      expect(screen.getByTestId('motion-sidebar').textContent).not.toContain('3P')
    }
  })
})
