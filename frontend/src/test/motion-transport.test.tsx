import { act, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { TooltipProvider } from '@/components/ui/tooltip'
import { TransportBar } from '@/motion/components/TransportBar'
import { setMotionBridge } from '@/motion/bridge'
import { MotionMockBridgeClient } from '@/motion/mock'
import { motionStore } from '@/motion/store'

let bridge: MotionMockBridgeClient

function renderBar() {
  return render(
    <TooltipProvider>
      <TransportBar />
    </TooltipProvider>,
  )
}

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

describe('TransportBar', () => {
  it('shows the stopped state and disables playback without a clip', () => {
    renderBar()
    expect(screen.getByTestId('transport-title')).toHaveTextContent('未加载曲目')
    expect(screen.getByRole('button', { name: '播放' })).toBeDisabled()
    expect(screen.getByRole('button', { name: '快退' })).toBeDisabled()
    expect(screen.getByRole('button', { name: '快进' })).toBeDisabled()
    expect(screen.getByRole('slider', { name: '播放进度' })).toHaveAttribute('data-disabled', '')
    // Secondary prev/next controls are tucked inside PrecisionControlPanel, not in the center main bar
    expect(screen.queryByRole('button', { name: '上一曲' })).not.toBeInTheDocument()
  })

  it('plays, pauses, fast-forwards, rewinds, and stops a loaded motion', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    const user = userEvent.setup()
    renderBar()
    expect(screen.getByTestId('transport-title')).toHaveTextContent('輝きの先へ')
    await user.click(screen.getByRole('button', { name: '播放' }))
    await waitFor(() => expect(motionStore.getState().player.state).toBe('playing'))

    await user.click(screen.getByRole('button', { name: '快进' }))
    await waitFor(() => expect(motionStore.getState().player.time).toBeGreaterThanOrEqual(4.9))

    await user.click(screen.getByRole('button', { name: '快退' }))
    await waitFor(() => expect(motionStore.getState().player.time).toBeLessThan(1.5))

    await user.click(screen.getByRole('button', { name: '暂停' }))
    await waitFor(() => expect(motionStore.getState().player.state).toBe('paused'))

    await user.click(screen.getByRole('button', { name: '停止' }))
    await waitFor(() => expect(motionStore.getState().player.state).toBe('stopped'))
  })

  it('runs the three-stage scrub from the full-width slider keyboard interaction', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    const spy = vi.spyOn(bridge, 'request')
    renderBar()
    const slider = screen.getByRole('slider', { name: '播放进度' })
    slider.focus()
    const user = userEvent.setup()
    await user.keyboard('{ArrowRight}')
    await waitFor(() => expect(motionStore.getState().scrub.active).toBe(false))
    const methods = spy.mock.calls.map((call) => call[0])
    expect(methods).toContain('player.scrubBegin')
    expect(methods).toContain('player.scrubEnd')
    expect(methods.indexOf('player.scrubBegin')).toBeLessThan(methods.indexOf('player.scrubEnd'))
  })

  it('opens precision control panel for frame stepping, time/frame seek, and fine speed', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()
    renderBar()

    await user.click(screen.getByTestId('precision-control-trigger'))
    const panel = screen.getByTestId('precision-control-panel')
    expect(panel).toBeInTheDocument()

    // Invalid time & out-of-range frame checks
    const timeInput = within(panel).getByRole('textbox', { name: '时间定位' })
    await user.clear(timeInput)
    await user.type(timeInput, 'abc{Enter}')
    expect(within(panel).getByTestId('transport-navigation-error')).toHaveTextContent('时间格式无效')

    const frameInput = within(panel).getByRole('textbox', { name: '动作帧定位' })
    await user.clear(frameInput)
    await user.type(frameInput, '999999{Enter}')
    expect(within(panel).getByTestId('transport-navigation-error')).toHaveTextContent('帧号超出范围')

    await user.clear(frameInput)
    await user.type(frameInput, '120{Enter}')
    await waitFor(() => expect(within(panel).queryByTestId('transport-navigation-error')).not.toBeInTheDocument())

    // Step frame & fine speed nudge
    await user.click(within(panel).getByRole('button', { name: '下一帧' }))
    expect(spy.mock.calls.some((c) => c[0] === 'player.stepFrame')).toBe(true)

    await user.click(within(panel).getByRole('button', { name: '+0.05x' }))
    await waitFor(() => expect(motionStore.getState().player.speed).toBeCloseTo(1.05))
    expect(within(panel).queryByRole('combobox', { name: '播放模式' })).not.toBeInTheDocument()

    // Close panel does not affect playback/loaded state
    await user.click(within(panel).getByRole('button', { name: '关闭精细控制' }))
    expect(screen.queryByTestId('precision-control-panel')).not.toBeInTheDocument()
    expect(motionStore.getState().loaded.motionKey).toBe('hikari-no-saki')
  })

  it('hot-cycles playMode and selects camera mode from badge popover', async () => {
    const user = userEvent.setup()
    renderBar()
    const before = motionStore.getState().player.playMode
    await user.click(screen.getByTestId('transport-playmode-toggle'))
    await waitFor(() => expect(motionStore.getState().player.playMode).toBe((before + 1) % 4))

    await user.click(screen.getByTestId('camera-mode-badge'))
    await user.click(await screen.findByRole('menuitemradio', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().camera.settings.mode).toBe(2))
  })

  it('keeps prev/next enabled inside precision panel with multi-actor stage formation', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    const user = userEvent.setup()
    renderBar()
    await user.click(screen.getByTestId('precision-control-trigger'))

    expect(screen.getByRole('button', { name: '上一曲' })).toBeEnabled()
    expect(screen.getByRole('button', { name: '下一曲' })).toBeEnabled()
    await user.click(screen.getByRole('button', { name: '下一曲' }))
    await waitFor(() => expect(motionStore.getState().loaded.motionKey).not.toBe('hikari-no-saki'))
  })

  it('opens vertical volume popover and saves volume on slider commit', async () => {
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()
    renderBar()
    await user.click(screen.getByRole('button', { name: '音量控制' }))
    const slider = screen.getByRole('slider', { name: '音乐音量' })
    expect(slider).toHaveAttribute('aria-orientation', 'vertical')
    slider.focus()
    await user.keyboard('{ArrowDown}')
    const methods = spy.mock.calls.map((call) => call[0])
    expect(methods).toContain('player.setVolume')
    expect(methods).toContain('player.saveVolume')
    expect(motionStore.getState().player.volume).toBeLessThan(1)
  })

  it('opens speed popover and applies speed preset', async () => {
    const user = userEvent.setup()
    renderBar()
    await user.click(screen.getByRole('button', { name: '播放速度' }))
    await user.click(screen.getByRole('button', { name: '1.5x' }))
    await waitFor(() => expect(motionStore.getState().player.speed).toBeCloseTo(1.5))
  })

  it('places audio-effect-badge left of camera-mode-badge and switches presets', async () => {
    const user = userEvent.setup()
    renderBar()
    const audioFx = screen.getByTestId('audio-effect-badge')
    const camera = screen.getByTestId('camera-mode-badge')
    expect(audioFx.compareDocumentPosition(camera) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    await user.click(audioFx)
    await user.click(await screen.findByRole('menuitemradio', { name: /演唱会现场/ }))
    await waitFor(() => expect(motionStore.getState().audioEffect.activeId).toBe('concert_arena'))
    expect(screen.getByTestId('audio-effect-badge')).toHaveTextContent('演唱会现场')
  })

  it('toggles realtime/recording play mode and clock source in PrecisionControlPanel', async () => {
    const user = userEvent.setup()
    renderBar()
    await user.click(screen.getByTestId('precision-control-trigger'))
    const panel = await screen.findByTestId('precision-control-panel')
    await user.click(within(panel).getByTestId('run-mode-recording'))
    await waitFor(() => expect(motionStore.getState().clock.runMode).toBe('recording'))
    expect(screen.getByTestId('transport-recording-badge')).toHaveTextContent('供录制')
    await user.click(within(panel).getByTestId('clock-source-engine_delta'))
    await waitFor(() => expect(motionStore.getState().clock.configuredSource).toBe('engine_delta'))
  })

  it('renders embedded track cover image in TransportBar when available and keeps cover across track switch and refresh', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    renderBar()
    await waitFor(() => expect(screen.getByTestId('transport-cover-img')).toBeInTheDocument())

    // Refreshing library re-fetches cover in TransportBar instead of staying blank
    await motionStore.refreshLibrary()
    await waitFor(() => expect(screen.getByTestId('transport-cover-img')).toBeInTheDocument())

    // Switching to a track without cover displays the default album cover
    await motionStore.selectMotion('encore-demo', false)
    await waitFor(() => expect(screen.getByTestId('transport-default-cover')).toBeInTheDocument())

    // Switching back restores cover image
    await motionStore.selectMotion('hikari-no-saki', false)
    await waitFor(() => expect(screen.getByTestId('transport-cover-img')).toBeInTheDocument())
  })

  it('isolates 10Hz player.time and clock.changed updates to TransportScrubber without re-rendering TransportBar', async () => {
    await motionStore.selectMotion('hikari-no-saki', false)
    const rightSpy = vi.fn()
    const origChild = await import('@/motion/components/transport/TransportRightControls')
    vi.spyOn(origChild, 'TransportRightControls').mockImplementation((props) => {
      rightSpy()
      return <div data-testid="mock-right-controls" data-speed={props.speed} />
    })

    renderBar()
    const initialRenders = rightSpy.mock.calls.length
    const basePlayer = motionStore.getState().player
    const baseClock = motionStore.getState().clock

    act(() => {
      for (let i = 1; i <= 5; i += 1) {
        bridge.push('player.stateChanged', { ...basePlayer, time: i * 0.5 })
        bridge.push('clock.changed', { ...baseClock, evaluatedFrameSerial: 100 + i })
      }
    })

    expect(rightSpy.mock.calls.length).toBe(initialRenders)
    expect(screen.getByRole('slider', { name: '播放进度' })).toHaveAttribute('aria-valuenow', '2.5')
  })
})
