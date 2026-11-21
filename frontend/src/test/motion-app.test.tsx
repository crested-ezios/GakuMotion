import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import MotionApp from '@/motion/components/MotionApp'
import { getMotionBridge, setMotionBridge } from '@/motion/bridge'
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
  getMotionBridge()?.close()
  bridge.close()
  motionStore.reset()
  setMotionBridge(null)
  vi.restoreAllMocks()
})

describe('MotionApp shell', () => {
  it('renders sidebar, status bar, transport and the dev-mock banner', async () => {
    render(<MotionApp />)
    expect(screen.getByTestId('mock-banner')).toBeInTheDocument()
    expect(screen.getByTestId('motion-sidebar')).toBeInTheDocument()
    expect(screen.getByTestId('motion-status-bar')).toBeInTheDocument()
    expect(screen.getByTestId('transport-bar')).toBeInTheDocument()
    for (const label of ['曲库', '动作', '镜头', '舞台', '设置']) {
      expect(screen.getByRole('button', { name: label })).toBeInTheDocument()
    }
    expect(screen.queryByRole('button', { name: '[开发]' })).not.toBeInTheDocument()
    expect(screen.getByTestId('motion-status-bar')).toHaveTextContent('曲目: 未加载曲目')
    expect(screen.getByTestId('motion-status-bar')).toHaveTextContent('摄影棚 A')
  })

  it('navigates between pages while keeping player bar and pinned precision panel intact', async () => {
    const user = userEvent.setup()
    render(<MotionApp />)
    await motionStore.selectMotion('hikari-no-saki', true)
    expect(screen.getByTestId('library-page')).toBeInTheDocument()

    await user.click(screen.getByTestId('precision-control-trigger'))
    await user.click(screen.getByRole('button', { name: '固定面板' }))
    expect(screen.getByTestId('precision-control-panel')).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: '镜头' }))
    expect(screen.getByTestId('camera-page')).toBeInTheDocument()
    expect(screen.getByTestId('transport-title')).toHaveTextContent('輝きの先へ')
    expect(screen.getByTestId('precision-control-panel')).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: '舞台' }))
    expect(screen.getByTestId('stage-page')).toBeInTheDocument()
    await user.click(screen.getByRole('button', { name: '动作' }))
    expect(screen.getByTestId('motion-page')).toBeInTheDocument()
    expect(screen.getByTestId('precision-control-panel')).toBeInTheDocument()
  })

  it('shows the developer page only after enabling developer mode', async () => {
    const user = userEvent.setup()
    render(<MotionApp />)
    await user.click(screen.getByRole('button', { name: '设置' }))
    expect(screen.getByTestId('settings-page')).toBeInTheDocument()
    await user.click(screen.getByRole('switch', { name: '开发者模式' }))
    await user.click(screen.getByRole('button', { name: '[开发]' }))
    expect(screen.getByTestId('developer-page')).toBeInTheDocument()
    expect(screen.getByRole('tab', { name: '动作' })).toBeInTheDocument()
    expect(screen.getByRole('tab', { name: '遥测与诊断' })).toBeInTheDocument()
    expect(screen.getByRole('tab', { name: '鞋底测量 v2' })).toBeInTheDocument()
    expect(screen.getByRole('tab', { name: '摄影舞台替换' })).toBeInTheDocument()
    expect(screen.getByRole('tab', { name: '布光与视频' })).toBeInTheDocument()
  })

  it('degrades to the permission gate when injection is disallowed (phase1 unsupported panels)', async () => {
    bridge.close()
    bridge = new MotionMockBridgeClient({ capabilityProfile: 'phase1' })
    setMotionBridge(bridge)
    motionStore.reset()
    await motionStore.start()

    render(<MotionApp />)
    bridge.setPermission(false, '截图保护未就绪')
    await waitFor(() => expect(screen.getByTestId('permission-gate')).toBeInTheDocument())
    expect(screen.getByText('控制已禁用')).toBeInTheDocument()
    expect(screen.getByText('截图保护未就绪')).toBeInTheDocument()
    expect(screen.queryByTestId('transport-bar')).not.toBeInTheDocument()
    expect(screen.queryByTestId('motion-sidebar')).not.toBeInTheDocument()
    const user = userEvent.setup()
    await user.click(screen.getByText('开发者选项'))
    await user.click(screen.getByRole('switch', { name: '开发者模式' }))
    expect(screen.getAllByTestId('unsupported-panel').length).toBeGreaterThan(0)
  })

  it('handles keyboard shortcuts when idle and suppresses them when typing in search', async () => {
    const user = userEvent.setup()
    render(<MotionApp />)
    await motionStore.selectMotion('hikari-no-saki', false)
    const spy = vi.spyOn(bridge, 'request')

    // ArrowRight seeks forward by seekStepSeconds (default 5s)
    await user.keyboard('{ArrowRight}')
    await waitFor(() =>
      expect(spy.mock.calls.some((c) => c[0] === 'player.seek' && (c[1] as { seconds: number }).seconds === 5)).toBe(true),
    )

    // '/' focuses search box; typing inside search must not trigger Space/Arrow playback shortcuts
    await user.keyboard('/')
    const searchInput = screen.getByRole('textbox', { name: '搜索曲目' })
    expect(searchInput).toHaveFocus()
    const seekCallsBefore = spy.mock.calls.filter((c) => c[0] === 'player.seek').length
    await user.type(searchInput, 'a ')
    expect(spy.mock.calls.filter((c) => c[0] === 'player.seek').length).toBe(seekCallsBefore)
  })

  it('renders standalone preview toolbar when ?preview=1 is active', async () => {
    const previousUrl = window.location.href
    window.history.pushState({}, '', '/?preview=1')
    try {
      const user = userEvent.setup()
      render(<MotionApp />)
      expect(screen.getByTestId('preview-toolbar')).toBeInTheDocument()
      await user.selectOptions(screen.getByLabelText('预览主题'), '2')
      await waitFor(() => expect(document.documentElement.dataset.theme).toBe('paper'))
      await user.click(screen.getByRole('button', { name: '模拟禁用控制' }))
      await waitFor(() => expect(screen.getByTestId('permission-gate')).toBeInTheDocument())
      await user.click(screen.getByRole('button', { name: '重置会话' }))
      await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())
      await waitFor(() => expect(document.documentElement.dataset.theme).toBe('dark'))
    } finally {
      window.history.pushState({}, '', previousUrl)
    }
  })

  it('reconnects with a fresh bridge after bridge.closed', async () => {
    render(<MotionApp />)
    bridge.push('bridge.closed', { reason: 'detach' })
    await waitFor(() => expect(screen.getByRole('button', { name: '重新连接' })).toBeInTheDocument())
    const user = userEvent.setup()
    await user.click(screen.getByRole('button', { name: '重新连接' }))
    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())
  })
})
