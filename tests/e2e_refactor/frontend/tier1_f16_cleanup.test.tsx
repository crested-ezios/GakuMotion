import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import { CameraPage } from '@/motion/pages/CameraPage'
import { StagePage } from '@/motion/pages/StagePage'
import { MotionPage } from '@/motion/pages/MotionPage'
import { MotionStatusBar } from '@/motion/components/MotionStatusBar'
import { MotionSidebar } from '@/motion/components/MotionSidebar'
import { TransportBar } from '@/motion/components/TransportBar'
import { PrecisionControlPanel } from '@/motion/components/PrecisionControlPanel'
import { motionStore } from '@/motion/store'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  motionFileExists,
  probeF16Ready,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

describe('Tier 1 — F16 Camera, Stage, Status Bar & Cleanup UI', () => {
  e2eIt(1, 'T1-F16-01', 'F16', 'CameraPage includes 全部 (-1) and 1号位..N号位 follow slot options', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<CameraPage />)
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))

    const slotCombo = screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ })
    await user.click(slotCombo)
    expect(await screen.findByRole('option', { name: '全部' })).toBeInTheDocument()
    expect(screen.getByRole('option', { name: '1号位' })).toBeInTheDocument()
    expect(screen.getByRole('option', { name: '2号位' })).toBeInTheDocument()
    expect(screen.getByRole('option', { name: '3号位' })).toBeInTheDocument()

    await user.click(screen.getByRole('option', { name: '2号位' }))
    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            c[0] === 'camera.set' &&
            (c[1] as { settings?: { followSlotIndex?: number } }).settings?.followSlotIndex === 1,
        ),
      ).toBe(true),
    )

    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    await user.click(await screen.findByRole('option', { name: '全部' }))
    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            c[0] === 'camera.set' &&
            (c[1] as { settings?: { followSlotIndex?: number } }).settings?.followSlotIndex === -1,
        ),
      ).toBe(true),
    )

    await user.click(screen.getByRole('button', { name: '保存镜头设置' }))
    expect(spy.mock.calls.some((c) => c[0] === 'camera.save')).toBe(true)
  })

  e2eIt(1, 'T1-F16-02', 'F16', 'StagePage removes 舞台角色阵容 while keeping stage visual and node controls', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()

    render(<StagePage />)
    const text = document.body.textContent ?? ''
    expect(text).not.toContain('舞台角色阵容')
    expect(text).not.toContain('舞台驱动角色')
    expect(text).not.toContain('前往编排')
    expect(text).not.toContain('多人舞台编排模式')
    expect(text).not.toContain('演员1')

    expect(screen.getByText('当前舞台环境')).toBeInTheDocument()
    expect(screen.getByText('舞台视觉控制')).toBeInTheDocument()
    expect(screen.getByText('高级场景编辑')).toBeInTheDocument()
  })

  e2eIt(1, 'T1-F16-03', 'F16', 'MotionStatusBar removes 当前驱动角色 and 多人舞台激活 badge', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    await motionStore.selectMotion('hikari-no-saki', false)

    render(<MotionStatusBar />)
    const bar = screen.getByTestId('motion-status-bar')
    const text = bar.textContent ?? ''
    expect(text).not.toContain('多人舞台激活')
    expect(text).not.toContain('当前驱动角色')
    expect(text).not.toMatch(/驱动[:：]/)
    expect(text).not.toMatch(/绑定[:：]/)
    expect(text).toContain('曲目: 輝きの先へ')
    expect(text).toContain('摄影棚 A')
  })

  e2eIt(1, 'T1-F16-04', 'F16', 'MotionSidebar, TransportBar, and PrecisionControlPanel remove legacy ensemble UI', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()
    await motionStore.selectMotion('hikari-no-saki', false)

    render(
      <>
        <MotionSidebar />
        <TransportBar />
        <PrecisionControlPanel onClose={() => {}} />
      </>,
    )

    expect(screen.getByTestId('motion-sidebar').textContent).not.toContain('3P')
    expect(screen.getByTestId('transport-bar').textContent).not.toContain('多人舞台')
    expect(document.body.textContent).not.toContain('多人舞台激活时禁用上下曲')
    expect(screen.getByRole('button', { name: '上一曲' })).toBeEnabled()
    expect(screen.getByRole('button', { name: '下一曲' })).toBeEnabled()
  })

  e2eIt(1, 'T1-F16-05', 'F16', 'EnsembleSection.tsx is deleted and MotionPage renders zero legacy ensemble controls', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()

    expect(motionFileExists('components/EnsembleSection.tsx')).toBe(false)
    render(<MotionPage />)
    expect(screen.queryByTestId('ensemble-section')).not.toBeInTheDocument()
    const text = document.body.textContent ?? ''
    expect(text).not.toContain('多人舞台')
    expect(text).not.toContain('摄影槽位验证探针')
    expect(text).not.toContain('使用 演员1')
  })

  e2eIt(1, 'T1-F16-06', 'F16', 'CameraPage slot options scale dynamically to 5 slots (1号位..5号位)', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF16Ready())) return
    bridge = await createMockSession()

    bridge.push('actors.changed', {
      epoch: 3,
      actors: [
        { token: 101, label: '咲季', selected: true, character: 'hski', displayName: '花海咲季', shortName: '咲季' },
        { token: 102, label: '手毬', selected: false, character: 'ttmr', displayName: '月村手毬', shortName: '手毬' },
        { token: 103, label: 'ことね', selected: false, character: 'fktn', displayName: '藤田ことね', shortName: 'ことね' },
        { token: 104, label: '千奈', selected: false, character: 'kcna', displayName: '倉本千奈', shortName: '千奈' },
        { token: 105, label: '広', selected: false, character: 'shro', displayName: '篠澤広', shortName: '広' },
      ],
      targetLabel: '咲季',
      boundLabel: '咲季',
    } as never)

    const user = userEvent.setup()
    render(<CameraPage />)
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))

    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    for (const label of ['全部', '1号位', '2号位', '3号位', '4号位', '5号位']) {
      expect(await screen.findByRole('option', { name: label })).toBeInTheDocument()
    }
  })
})
