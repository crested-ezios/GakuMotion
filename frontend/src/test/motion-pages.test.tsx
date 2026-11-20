import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { CameraPage } from '@/motion/pages/CameraPage'
import { DeveloperPage } from '@/motion/pages/DeveloperPage'
import { StagePage } from '@/motion/pages/StagePage'
import { SettingsPage } from '@/motion/pages/SettingsPage'
import { MotionPage } from '@/motion/pages/MotionPage'
import { setMotionBridge } from '@/motion/bridge'
import { MotionMockBridgeClient } from '@/motion/mock'
import { motionStore } from '@/motion/store'

let bridge: MotionMockBridgeClient

beforeEach(async () => {
  bridge = new MotionMockBridgeClient()
  setMotionBridge(bridge)
  motionStore.reset()
  await motionStore.start()
})

afterEach(() => {
  bridge.close()
  motionStore.reset()
  setMotionBridge(null)
  vi.restoreAllMocks()
})

describe('CameraPage', () => {
  it('applies mode changes through the pending acknowledgement flow', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    render(<CameraPage />)
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
    expect(motionStore.getState().camera.settings.mode).toBe(2)
    expect(motionStore.getState().camera.settings.followBehavior).toBe(1)
    expect(spy.mock.calls.some((call) => call[0] === 'camera.set')).toBe(true)
  })

  it('saves camera settings explicitly', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    render(<CameraPage />)
    await user.click(screen.getByRole('button', { name: '保存镜头设置' }))
    expect(spy.mock.calls.some((call) => call[0] === 'camera.save')).toBe(true)
  })
})

describe('StagePage', () => {
  it('toggles lighting and keeps the native camera shadow linkage', async () => {
    const user = userEvent.setup()
    render(<StagePage />)
    await user.click(screen.getByRole('checkbox', { name: '光影效果' }))
    await waitFor(() => expect(motionStore.getState().camera.settings.actorShadowExperimentMode).toBe(2))
    expect(motionStore.getState().stage.settings.lightingEffectsEnabled).toBe(false)
  })

  it('switches to solid background with green-screen defaults', async () => {
    const user = userEvent.setup()
    render(<StagePage />)
    await user.click(screen.getByRole('checkbox', { name: '场景背景' }))
    await waitFor(() => expect(motionStore.getState().stage.settings.hideBackground).toBe(true))
    const settings = motionStore.getState().stage.settings
    expect(settings.greenScreenActive).toBe(true)
    expect(settings.overrideCameraClear).toBe(true)
  })
})

describe('DeveloperPage', () => {
  it('defaults muteNativeSoundEffects on and toggles it via experiments.set', async () => {
    const user = userEvent.setup()
    render(<DeveloperPage />)
    const checkbox = screen.getByRole('checkbox', { name: '播放 MMD 时屏蔽游戏音效（默认开启）' })
    expect(checkbox).toBeChecked()
    await user.click(checkbox)
    await waitFor(() => expect(motionStore.getState().experiments.settings.muteNativeSoundEffects).toBe(false))
  })
})

describe('SettingsPage', () => {
  it('lists the static hotkey table and switches physics backend', async () => {
    const user = userEvent.setup()
    render(<SettingsPage />)
    expect(screen.getByText('Ctrl + M')).toBeInTheDocument()
    expect(screen.getByText('F9')).toBeInTheDocument()
    expect(screen.getByText('Shift + F11')).toBeInTheDocument()
    await user.click(screen.getByRole('combobox', { name: '物理引擎' }))
    await user.click(await screen.findByRole('option', { name: '内建物理' }))
    await waitFor(() => expect(motionStore.getState().physicsBackend).toBe(1))
  })
})

describe('MotionPage', () => {
  it('selects the drive actor, expands ensemble & bone sections, and labels phase1 unsupported badges', async () => {
    bridge.close()
    bridge = new MotionMockBridgeClient({ capabilityProfile: 'phase1' })
    setMotionBridge(bridge)
    motionStore.reset()
    await motionStore.start()

    const user = userEvent.setup()
    render(<MotionPage />)
    await user.click(screen.getByRole('combobox', { name: '驱动角色' }))
    await user.click(await screen.findByRole('option', { name: '凛' }))
    await waitFor(() => expect(motionStore.getState().actors.targetLabel).toBe('凛'))
    await user.click(screen.getByText('多人舞台'))
    expect(screen.getByTestId('ensemble-section')).toBeInTheDocument()
    await user.click(screen.getByText('高级动作设置'))
    expect(screen.getByTestId('bone-section')).toBeInTheDocument()
    expect(within(screen.getByTestId('ensemble-section')).getByText('主槽（音频与运镜）')).toBeInTheDocument()
    expect(screen.getAllByText('待迁移').length).toBeGreaterThan(0)
  })

  it('commits facial weight changes and saves them', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    render(<MotionPage />)
    const slider = screen.getByRole('slider', { name: '表情幅度 整体' })
    slider.focus()
    await user.keyboard('{ArrowRight}')
    await waitFor(() => expect(spy.mock.calls.some((call) => call[0] === 'facial.setWeights')).toBe(true))
    await user.click(screen.getByRole('button', { name: '保存表情幅度' }))
    expect(spy.mock.calls.some((call) => call[0] === 'facial.save')).toBe(true)
  })

  it('auto-syncs ensemble slots to >3 detected actors and skips unbound slots on apply', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    const baseActors = motionStore.getState().actors
    const firstKey = motionStore.getState().library.entries[0]?.id ?? 'demo-song'
    bridge.push('actors.changed', {
      ...baseActors,
      actors: [
        { token: 101, label: '咲季', selected: true },
        { token: 102, label: '凛', selected: false },
        { token: 103, label: '千奈', selected: false },
        { token: 104, label: 'ことね', selected: false },
        { token: 105, label: '手毬', selected: false },
      ],
    })
    await waitFor(() => expect(motionStore.getState().ensembleDraft.slots).toHaveLength(5))

    const { rerender } = render(<MotionPage />)
    await user.click(screen.getByText('多人舞台'))
    const section = screen.getByTestId('ensemble-section')
    expect(within(section).getByText('使用 演员4')).toBeInTheDocument()
    expect(within(section).getByText('使用 演员5')).toBeInTheDocument()

    await user.click(within(section).getByRole('checkbox', { name: '启用多人舞台' }))
    motionStore.patchEnsembleSlot(0, { enabled: true, actorToken: 101, actorHint: '咲季', motionKey: firstKey })
    motionStore.patchEnsembleSlot(0, { offset: { x: 256, y: -256, z: 120 } })
    await waitFor(() => expect(motionStore.ensembleDraftError()).toBeNull())

    await user.click(within(section).getByRole('button', { name: '应用多人舞台配置' }))
    await waitFor(() => expect(motionStore.getState().ensemble.active).toBe(true))

    const applyCall = spy.mock.calls.find((call) => call[0] === 'ensemble.set')
    expect(applyCall).toBeDefined()
    const sentSettings = (applyCall?.[1] as { settings: { slots: Array<{ enabled: boolean; offset: { x: number; y: number; z: number } }> } }).settings
    expect(sentSettings.slots).toHaveLength(5)
    expect(sentSettings.slots[0].enabled).toBe(true)
    expect(sentSettings.slots[0].offset).toEqual({ x: 256, y: -256, z: 120 })
    expect(sentSettings.slots.slice(1).every((slot) => !slot.enabled)).toBe(true)

    rerender(<StagePage />)
    expect(screen.getByText('演员4')).toBeInTheDocument()
    expect(screen.getByText('演员5')).toBeInTheDocument()
  })
})
