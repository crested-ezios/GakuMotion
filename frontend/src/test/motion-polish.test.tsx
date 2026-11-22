import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { TooltipProvider } from '@/components/ui/tooltip'
import { setMotionBridge } from '@/motion/bridge'
import MotionApp from '@/motion/components/MotionApp'
import { PrecisionSeekSection } from '@/motion/components/precision/PrecisionSeekSection'
import { TransportBar } from '@/motion/components/TransportBar'
import { MotionMockBridgeClient } from '@/motion/mock'
import { LibraryPage } from '@/motion/pages/LibraryPage'
import { IMPORT_SLOT } from '@/motion/protocol'
import { motionStore } from '@/motion/store'

let bridge: MotionMockBridgeClient

beforeEach(() => {
  bridge = new MotionMockBridgeClient()
  setMotionBridge(bridge)
  motionStore.reset()
})

afterEach(() => {
  bridge.close()
  motionStore.reset()
  setMotionBridge(null)
  vi.restoreAllMocks()
})

describe('UX polish contracts (Phases 2-4)', () => {
  it('renders MotionBootSkeleton while booting instead of blocking blank spinner', async () => {
    let resolveHello!: () => void
    const origRequest = bridge.request.bind(bridge)
    vi.spyOn(bridge, 'request').mockImplementation((async (
      method: string,
      params?: unknown,
      options?: unknown,
    ) => {
      if (method === 'motion.hello') {
        await new Promise<void>((resolve) => {
          resolveHello = resolve
        })
      }
      return origRequest(method as never, params as never, options as never)
    }) as typeof bridge.request)

    render(<MotionApp />)
    expect(screen.getByTestId('motion-boot-skeleton')).toBeInTheDocument()
    expect(screen.getByText('正在连接 GakuMotion…')).toBeInTheDocument()

    await waitFor(() => expect(resolveHello).toBeTypeOf('function'))
    resolveHello()
    await waitFor(() => expect(screen.queryByTestId('motion-boot-skeleton')).not.toBeInTheDocument())
  })

  it('handles async pending library.getCover and waits for library.coverReady without duplicate RPCs', async () => {
    await motionStore.start()
    const origRequest = bridge.request.bind(bridge)
    let coverCalls = 0
    vi.spyOn(bridge, 'request').mockImplementation((async (
      method: string,
      params?: unknown,
      options?: unknown,
    ) => {
      if (method === 'library.getCover') {
        coverCalls += 1
        return { hasCover: false, coverDataUri: '', pending: true } as never
      }
      return origRequest(method as never, params as never, options as never)
    }) as typeof bridge.request)

    await motionStore.ensureTrackCover('hikari-no-saki')
    await motionStore.ensureTrackCover('hikari-no-saki')
    expect(coverCalls).toBe(1)
    expect(Object.prototype.hasOwnProperty.call(motionStore.getState().coverCache, 'hikari-no-saki')).toBe(false)

    bridge.push('library.coverReady', {
      key: 'hikari-no-saki',
      hasCover: true,
      coverDataUri: 'data:image/png;base64,TESTCOVER',
    })
    await waitFor(() =>
      expect(motionStore.getState().coverCache['hikari-no-saki']).toBe('data:image/png;base64,TESTCOVER'),
    )
  })

  it('never renders transport duration mismatch warnings in TransportBar or PrecisionSeekSection', async () => {
    await motionStore.start()
    await motionStore.selectMotion('hikari-no-saki', false)
    bridge.push('player.stateChanged', {
      ...motionStore.getState().player,
      totalFrames: 600,
      fps: 60,
      duration: 9.5,
    })

    const st = motionStore.getState()
    render(
      <TooltipProvider>
        <TransportBar />
        <PrecisionSeekSection
          player={st.player}
          loaded={st.loaded}
          hasClip={true}
          seekStepSeconds={st.seekStepSeconds}
          navigationError={null}
          supports={(m) => st.capabilities.includes(m as never)}
        />
      </TooltipProvider>,
    )

    expect(screen.queryByTestId('transport-duration-warning')).not.toBeInTheDocument()
    expect(screen.queryByText(/源动作头部时长与采样帧不一致/)).not.toBeInTheDocument()
  })

  it('shows X号位 Up/Down track swap buttons for multi-slot tracks, swaps track only, and preserves sourceSlot in Edit mode', async () => {
    const user = userEvent.setup()
    await motionStore.start()

    render(<LibraryPage />)

    await motionStore.openImportNew(['C:\\Motion\\slot1_body.vmd'])
    const dialog = await screen.findByTestId('import-dialog')

    // Single slot: reorder arrows are not rendered
    expect(within(dialog).queryByRole('button', { name: '1号位上移' })).not.toBeInTheDocument()
    expect(within(dialog).queryByRole('button', { name: '1号位下移' })).not.toBeInTheDocument()

    // Add 2nd and 3rd positions
    motionStore.addImportPosition()
    motionStore.addImportPosition()
    await motionStore.importPickPositionFile(1, 'motion', 'C:\\Motion\\slot2_body.vmd')
    motionStore.setImportPositionPlacement(1, { x: 1.5, y: 0, z: -0.25, yawDegrees: 45 })

    const up1 = within(dialog).getByRole('button', { name: '1号位上移' })
    const down1 = within(dialog).getByRole('button', { name: '1号位下移' })
    const up2 = within(dialog).getByRole('button', { name: '2号位上移' })
    const down2 = within(dialog).getByRole('button', { name: '2号位下移' })
    const up3 = within(dialog).getByRole('button', { name: '3号位上移' })
    const down3 = within(dialog).getByRole('button', { name: '3号位下移' })

    expect(up1).toBeDisabled()
    expect(down1).toBeEnabled()
    expect(up2).toBeEnabled()
    expect(down2).toBeEnabled()
    expect(up3).toBeEnabled()
    expect(down3).toBeDisabled()

    // Swap 1号位 down -> exchanges tracks with 2号位 while keeping placement fixed at 2号位
    await user.click(down1)
    let positions = motionStore.getState().importForm.positions
    expect(positions[0].motion.candidates[positions[0].motion.selection]).toBe('C:\\Motion\\slot2_body.vmd')
    expect(positions[0].x).toBe(0)
    expect(positions[1].motion.candidates[positions[1].motion.selection]).toBe('C:\\Motion\\slot1_body.vmd')
    expect(positions[1].x).toBe(1.5)
    expect(positions[1].yawDegrees).toBe(45)

    // Swap 2号位 down into empty 3号位
    await user.click(down2)
    positions = motionStore.getState().importForm.positions
    expect(positions[1].motion.candidates[positions[1].motion.selection]).toBe(IMPORT_SLOT.empty)
    expect(positions[1].x).toBe(1.5)
    expect(positions[2].motion.candidates[positions[2].motion.selection]).toBe('C:\\Motion\\slot1_body.vmd')
    expect(positions[2].x).toBe(0)

    motionStore.closeImportForm()

    // Verify Edit mode preserves swapped sourceSlot in library.import request payload
    const origRequest = bridge.request.bind(bridge)
    const sentRequests: Array<{ method: string; params: unknown }> = []
    vi.spyOn(bridge, 'request').mockImplementation((async (
      method: string,
      params?: unknown,
      options?: unknown,
    ) => {
      sentRequests.push({ method, params })
      if (method === 'library.startEdit') {
        return {
          key: 'starlight-step',
          title: 'Starlight Group',
          hasFacial: true,
          hasCamera: false,
          hasAudio: false,
          hasCover: false,
          isGroup: true,
          positionCount: 2,
          positions: [
            { slot: 1, hasFacial: true, x: 0, y: 0, z: 0, yawDegrees: 0 },
            { slot: 2, hasFacial: false, x: 0.8, y: 0, z: 0, yawDegrees: 15 },
          ],
          timing: motionStore.getState().importForm.timing,
          sourceFps: 0,
          fps: 0,
          revision: motionStore.getState().library.revision,
        } as never
      }
      if (method === 'library.import') {
        return { started: true } as never
      }
      return origRequest(method as never, params as never, options as never)
    }) as typeof bridge.request)

    await motionStore.openImportEdit('starlight-step')
    const editDialog = await screen.findByTestId('import-dialog')
    await waitFor(() => expect(within(editDialog).getByRole('button', { name: '1号位下移' })).toBeInTheDocument())
    await user.click(within(editDialog).getByRole('button', { name: '1号位下移' }))
    await user.click(within(editDialog).getByRole('button', { name: '保存修改' }))

    const importCall = sentRequests.find((c) => c.method === 'library.import')
    expect(importCall).toBeDefined()
    const payload = importCall!.params as { request: { positions: Array<{ slot: number; sourceSlot?: number; x: number; yawDegrees: number }> } }
    expect(payload.request.positions[0]).toMatchObject({ slot: 1, sourceSlot: 2, x: 0, yawDegrees: 0 })
    expect(payload.request.positions[1]).toMatchObject({ slot: 2, sourceSlot: 1, x: 0.8, yawDegrees: 15 })
  })
})
