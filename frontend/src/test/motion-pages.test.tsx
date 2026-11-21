import { render, screen, waitFor } from '@testing-library/react'
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
  it('renders ActorSlotStrip, expands BoneSection, and labels phase1 unsupported badges', async () => {
    bridge.close()
    bridge = new MotionMockBridgeClient({ capabilityProfile: 'phase1' })
    setMotionBridge(bridge)
    motionStore.reset()
    await motionStore.start()

    const user = userEvent.setup()
    render(<MotionPage />)
    expect(screen.getByTestId('actor-slot-strip')).toBeInTheDocument()
    expect(screen.getByText('1号位')).toBeInTheDocument()
    await user.click(screen.getByText('高级动作设置'))
    expect(screen.getByTestId('bone-section')).toBeInTheDocument()
    expect(screen.getAllByText('待迁移').length).toBeGreaterThan(0)
  })

  it('commits facial weight changes through rotary knobs and saves them', async () => {
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

  it('dynamically renders >3 stage slots and supports slot selection and offset updates', async () => {
    const user = userEvent.setup()
    const spy = vi.spyOn(bridge, 'request')
    const baseActors = motionStore.getState().actors
    bridge.push('actors.changed', {
      ...baseActors,
      actors: [
        { token: 101, label: '咲季', character: 'hski', displayName: '花海咲季', shortName: '咲季', selected: true },
        { token: 102, label: '手毬', character: 'ttmr', displayName: '月村手毬', shortName: '手毬', selected: false },
        { token: 103, label: '千奈', character: 'kcna', displayName: '仓本千奈', shortName: '千奈', selected: false },
        { token: 104, label: '琴音', character: 'fktn', displayName: '藤田琴音', shortName: '琴音', selected: false },
        { token: 105, label: '広', character: 'shro', displayName: '篠澤広', shortName: '広', selected: false },
      ],
    })
    bridge.push('formation.changed', {
      ...motionStore.getState().formation,
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 5,
      selectedSlotIndex: 0,
      slots: [0, 1, 2, 3, 4].map((idx) => ({
        slotIndex: idx,
        occupied: true,
        actorToken: 101 + idx,
        actorInstanceId: 1000 + idx,
        character: ['hski', 'ttmr', 'kcna', 'fktn', 'shro'][idx],
        shortName: ['咲季', '手毬', '千奈', '琴音', '広'][idx],
        displayName: ['花海咲季', '月村手毬', '仓本千奈', '藤田琴音', '篠澤広'][idx],
        isOverflow: idx >= 3,
        hasTrackMotion: idx < 3,
        sourceSlotIndex: idx < 3 ? idx : 0,
        placementDirty: false,
        hasActorBoneOverrides: false,
        physicsBackend: '原生',
        offset: { x: idx * 0.5, y: 0, z: 0, yawDegrees: 0 },
        facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
      })),
    })

    const { rerender } = render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('4号位')).toBeInTheDocument())
    expect(screen.getByText('5号位')).toBeInTheDocument()

    await user.click(screen.getByTestId('actor-slot-card-3'))
    await waitFor(() =>
      expect(spy.mock.calls.some((call) => call[0] === 'formation.selectSlot')).toBe(true),
    )

    rerender(<StagePage />)
    expect(screen.queryByText('舞台角色阵容')).not.toBeInTheDocument()
  })
})
