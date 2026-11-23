import { fireEvent, render, screen, waitFor, within } from '@testing-library/react'
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

  it('persists per-position motion/facial timing across UI, moveImportPosition, buildImportRequest, openImportEdit, and group-shrink', async () => {
    const user = userEvent.setup()
    await motionStore.start()
    const spy = vi.spyOn(bridge, 'request')
    render(<LibraryPage />)

    await motionStore.openImportNew(['C:\\Timing\\01_a.vmd', 'C:\\Timing\\02_b.vmd'])
    const dialog = await screen.findByTestId('import-dialog')

    // All positions render motion & facial speed/offset DragFloat inputs with exact aria-labels
    const slot1MotionSpeed = within(dialog).getByRole('textbox', { name: '动作速率' })
    const slot1FacialOffset = within(dialog).getByRole('textbox', { name: '表情偏移' })
    const slot2MotionSpeed = within(dialog).getByRole('textbox', { name: '2号位动作速率' })
    const slot2MotionOffset = within(dialog).getByRole('textbox', { name: '2号位动作偏移' })
    const slot2FacialSpeed = within(dialog).getByRole('textbox', { name: '2号位表情速率' })
    const slot2FacialOffset = within(dialog).getByRole('textbox', { name: '2号位表情偏移' })

    const setDragFloatText = async (input: HTMLElement, nextText: string) => {
      await user.click(input)
      await user.clear(input)
      await user.type(input, `${nextText}{Enter}`)
      fireEvent.blur(input)
    }

    await setDragFloatText(slot1MotionSpeed, '1.25')
    await setDragFloatText(slot1FacialOffset, '-0.30')
    await setDragFloatText(slot2MotionSpeed, '0.75')
    await setDragFloatText(slot2MotionOffset, '0.50')
    await setDragFloatText(slot2FacialSpeed, '1.15')
    await setDragFloatText(slot2FacialOffset, '-0.25')

    // Speed clamping & finite guard
    motionStore.setImportPositionTiming(1, 'motion', { speed: -3 })
    expect(motionStore.getState().importForm.positions[1].timing.motion.speed).toBe(0)
    motionStore.setImportPositionTiming(1, 'motion', { speed: 0.75 })
    motionStore.setImportPositionTiming(1, 'motion', { speed: Number.NaN, offsetSeconds: Number.NaN })
    expect(motionStore.getState().importForm.positions[1].timing.motion).toEqual({ speed: 0.75, offsetSeconds: 0.5 })

    // setImportTiming mirrors slot 1 into positions[0].timing
    motionStore.setImportTiming('facial', { speed: 1.4 })
    expect(motionStore.getState().importForm.positions[0].timing.facial.speed).toBe(1.4)
    expect(motionStore.getState().importForm.timing.motion.speed).toBe(1.25)

    // Swap 1号位 <-> 2号位 -> timing swaps and form.timing syncs from new positions[0]
    await user.click(within(dialog).getByRole('button', { name: '1号位下移' }))
    let form = motionStore.getState().importForm
    expect(form.positions[0].timing).toEqual({
      motion: { speed: 0.75, offsetSeconds: 0.5 },
      facial: { speed: 1.15, offsetSeconds: -0.25 },
    })
    expect(form.positions[1].timing).toEqual({
      motion: { speed: 1.25, offsetSeconds: 0 },
      facial: { speed: 1.4, offsetSeconds: -0.3 },
    })
    expect(form.timing.motion).toEqual({ speed: 0.75, offsetSeconds: 0.5 })
    expect(form.timing.facial).toEqual({ speed: 1.15, offsetSeconds: -0.25 })

    // buildImportRequest carries per-slot timing and top-level slot 1 timing
    const built = motionStore.buildImportRequest()
    expect(built.timing.motion).toEqual({ speed: 0.75, offsetSeconds: 0.5 })
    expect(built.positions?.[0]?.timing).toEqual({
      motion: { speed: 0.75, offsetSeconds: 0.5 },
      facial: { speed: 1.15, offsetSeconds: -0.25 },
    })
    expect(built.positions?.[1]?.timing).toEqual({
      motion: { speed: 1.25, offsetSeconds: 0 },
      facial: { speed: 1.4, offsetSeconds: -0.3 },
    })

    // Import and re-open edit -> mock persists per-slot timing & openImportEdit marks slot 2+ placementEdited
    const initialIds = new Set(motionStore.getState().library.entries.map((entry) => entry.id))
    await user.click(within(dialog).getByRole('button', { name: '导入' }))
    await waitFor(
      () => {
        const state = motionStore.getState()
        expect(state.library.entries.some((entry) => !initialIds.has(entry.id))).toBe(true)
        expect(state.importUi.visible).toBe(false)
      },
      { timeout: 3000 },
    )

    const createdId = motionStore.getState().library.entries.find((entry) => !initialIds.has(entry.id))!.id
    await motionStore.openImportEdit(createdId)
    const editDialog = await screen.findByTestId('import-dialog')
    form = motionStore.getState().importForm
    expect(form.positions[0].timing.motion).toEqual({ speed: 0.75, offsetSeconds: 0.5 })
    expect(form.positions[1].timing.motion).toEqual({ speed: 1.25, offsetSeconds: 0 })
    expect(form.positions[1].placementEdited).toBe(true)

    // Group-shrink: delete 2号位 from a group song -> request still sends positions: [builtSlot1] with isGroup: false
    await user.click(within(editDialog).getByRole('button', { name: '删除 2号位' }))
    expect(motionStore.getState().importForm.positions.length).toBe(1)
    const shrinkReq = motionStore.buildImportRequest()
    expect(shrinkReq.isGroup).toBe(false)
    expect(shrinkReq.positions?.length).toBe(1)
    expect(shrinkReq.positions?.[0]?.slot).toBe(1)
    expect(shrinkReq.positions?.[0]?.timing?.motion).toEqual({ speed: 0.75, offsetSeconds: 0.5 })

    await user.click(within(editDialog).getByRole('button', { name: '保存修改' }))
    await waitFor(() => expect(spy.mock.calls.filter((c) => c[0] === 'library.import').length).toBe(2))
    await waitFor(() => {
      const updated = motionStore.getState().library.entries.find((e) => e.id === createdId)
      expect(updated?.isGroup).toBe(false)
      expect(updated?.positionCount).toBe(1)
    })

    // Solo new import must omit `positions` entirely (never send `positions: []`, which C++ rejects)
    await motionStore.openImportNew(['C:\\Solo\\solo.vmd'])
    const soloReq = motionStore.buildImportRequest()
    expect(soloReq.isGroup).toBe(false)
    expect(soloReq.positions).toBeUndefined()
    expect('positions' in soloReq).toBe(false)
  })

  it('auto-computes inter-slot XYZ offsets from probed VMD rootOrigin (with |dy|<0.02 snap) while respecting placementEdited', async () => {
    await motionStore.start()
    const origReq = bridge.request.bind(bridge)
    const originMap: Record<string, { x: number; y: number; z: number }> = {
      'C:\\Origin\\01_a.vmd': { x: 0.1, y: 1.0, z: -0.2 },
      'C:\\Origin\\02_b.vmd': { x: -0.65, y: 1.015, z: 0.3 },
      'C:\\Origin\\03_c.vmd': { x: 0.9, y: 1.08, z: 0.25 },
      'C:\\Origin\\04_d.vmd': { x: 0.5, y: 0.99, z: 0.1 },
    }
    vi.spyOn(bridge, 'request').mockImplementation((async (method: string, params?: unknown, options?: unknown) => {
      if (method === 'library.probeFiles') {
        const paths = ((params as { paths?: string[] })?.paths ?? []) as string[]
        return {
          probes: paths.map((path) => ({
            path,
            valid: true,
            isGkCamera: false,
            tendency: 'motionAndFacial',
            boneFrames: 1000,
            bodyBoneFrames: 900,
            eyeBoneFrames: 50,
            morphFrames: 100,
            cameraFrames: 0,
            hasRootOrigin: Boolean(originMap[path]),
            ...(originMap[path] ? { rootOrigin: originMap[path] } : {}),
          })),
        } as never
      }
      return origReq(method as never, params as never, options as never)
    }) as typeof bridge.request)

    await motionStore.openImportNew([
      'C:\\Origin\\01_a.vmd',
      'C:\\Origin\\02_b.vmd',
      'C:\\Origin\\03_c.vmd',
    ])

    let pos = motionStore.getState().importForm.positions
    expect(pos[0]).toMatchObject({ x: 0, y: 0, z: 0, yawDegrees: 0 })
    // |1.015 - 1.0| = 0.015 < 0.02 -> y snaps to 0
    expect(pos[1].x).toBeCloseTo(-0.75, 4)
    expect(pos[1].y).toBe(0)
    expect(pos[1].z).toBeCloseTo(0.5, 4)
    expect(pos[1].placementEdited).toBeFalsy()
    // |1.08 - 1.0| = 0.08 >= 0.02 -> y kept
    expect(pos[2].x).toBeCloseTo(0.8, 4)
    expect(pos[2].y).toBeCloseTo(0.08, 4)
    expect(pos[2].z).toBeCloseTo(0.45, 4)

    // Yaw-only change on slot 3 does NOT mark placementEdited, so changing slot 1 motion updates slot 3 XYZ while keeping yaw
    motionStore.setImportPositionPlacement(2, { x: pos[2].x, y: pos[2].y, z: pos[2].z, yawDegrees: 25 })
    expect(motionStore.getState().importForm.positions[2].placementEdited).toBe(false)

    // Manual XYZ change on slot 2 marks placementEdited: true
    motionStore.setImportPositionPlacement(1, { x: 2.0, y: 0.5, z: -1.0, yawDegrees: 30 })
    expect(motionStore.getState().importForm.positions[1].placementEdited).toBe(true)

    // Picking a new VMD on slot 1 updates slot 3 XYZ (since !placementEdited) and leaves slot 2 manual XYZ intact
    await motionStore.importPickPositionFile(0, 'motion', 'C:\\Origin\\04_d.vmd')
    pos = motionStore.getState().importForm.positions
    expect(pos[1]).toMatchObject({ x: 2.0, y: 0.5, z: -1.0, yawDegrees: 30, placementEdited: true })
    expect(pos[2].x).toBeCloseTo(0.4, 4) // 0.9 - 0.5
    expect(pos[2].y).toBeCloseTo(0.09, 4) // 1.08 - 0.99
    expect(pos[2].z).toBeCloseTo(0.15, 4) // 0.25 - 0.1
    expect(pos[2].yawDegrees).toBe(25)

    // Swapping slot 1 and slot 2 updates slot 1's VMD and recalculates slot 3 against the new slot 1
    motionStore.moveImportPosition(0, 1)
    pos = motionStore.getState().importForm.positions
    expect(pos[1]).toMatchObject({ x: 2.0, y: 0.5, z: -1.0, yawDegrees: 30, placementEdited: true })
    expect(pos[2].x).toBeCloseTo(1.55, 4)
    expect(pos[2].y).toBeCloseTo(0.065, 4)
    expect(pos[2].z).toBeCloseTo(-0.05, 4)
    expect(pos[2].yawDegrees).toBe(25)
    const req = motionStore.buildImportRequest()
    expect(req.positions?.[1]?.hasExplicitPlacement).toBe(true)
    expect(req.positions?.[2]?.hasExplicitPlacement).toBe(false)
  })
})
