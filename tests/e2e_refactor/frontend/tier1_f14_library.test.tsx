import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import { LibraryPage } from '@/motion/pages/LibraryPage'
import { motionStore } from '@/motion/store'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
  assertBadgeBeforeAuthor,
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  findNumericInputs,
  parseNumericInputValue,
  probeF14Ready,
  setNumericInputValue,
  unwrapImportRequest,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

function seedLibraryWithGroupAndSolo(b: MotionMockBridgeClient) {
  const entries = [
    {
      id: 'group-campus',
      title: 'Campus Mode!!',
      subtitle: '初星学園, 花海咲季',
      directory: 'gakumas-local/motions/group-campus',
      durationSeconds: 185,
      totalFrames: 11100,
      hasMotion: true,
      hasFacial: true,
      hasCamera: true,
      hasAudio: true,
      audioFile: 'song.flac',
      isGroup: true,
      positionCount: 3,
      positions: [
        { slot: 1, x: 0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: true },
        { slot: 2, x: -0.8, y: 0, z: 0.2, yawDegrees: 15, hasMotion: true, hasFacial: true },
        { slot: 3, x: 0.8, y: 0, z: 0.2, yawDegrees: -15, hasMotion: true, hasFacial: false },
      ],
    },
    {
      id: 'group-encore',
      title: 'Starlight Group',
      subtitle: '初星学園, 藤田ことね',
      directory: 'gakumas-local/motions/group-encore',
      durationSeconds: 160,
      totalFrames: 9600,
      hasMotion: true,
      hasFacial: false,
      hasCamera: true,
      hasAudio: false,
      isGroup: true,
      positionCount: 2,
      positions: [
        { slot: 1, x: 0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: false },
        { slot: 2, x: 1.0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: false },
      ],
    },
    {
      id: 'solo-temari',
      title: 'Luna Say Maybe',
      subtitle: '月村手毬',
      directory: 'gakumas-local/motions/solo-temari',
      durationSeconds: 210,
      totalFrames: 12600,
      hasMotion: true,
      hasFacial: true,
      hasCamera: false,
      hasAudio: true,
      isGroup: false,
      positionCount: 1,
    },
    {
      id: 'solo-saki',
      title: 'Fighting My Way',
      subtitle: '花海咲季',
      directory: 'gakumas-local/motions/solo-saki',
      durationSeconds: 195,
      totalFrames: 11700,
      hasMotion: true,
      hasFacial: true,
      hasCamera: true,
      hasAudio: true,
      isGroup: false,
      positionCount: 1,
    },
    {
      id: 'solo-practice',
      title: 'Practice Step',
      subtitle: '十王星南',
      directory: 'gakumas-local/motions/solo-practice',
      durationSeconds: 90,
      totalFrames: 5400,
      hasMotion: true,
      hasFacial: false,
      hasCamera: false,
      hasAudio: false,
      isGroup: false,
      positionCount: 1,
    },
  ]
  b.push('library.changed', { revision: 10, entries } as never)
}

describe('Tier 1 — F14 Library & Group-Song Import UI', () => {
  e2eIt(1, 'T1-F14-01', 'F14', 'Gold 团曲 badge appears strictly left of Author on group-song rows', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    seedLibraryWithGroupAndSolo(bridge)

    render(<LibraryPage />)
    const groupRow = await screen.findByTestId('library-row-group-campus')
    const soloRow = screen.getByTestId('library-row-solo-temari')

    const badge = within(groupRow).getByText('团曲')
    const authorEl = within(groupRow).getByText(/初星学園/)
    expect(badge).toBeInTheDocument()
    assertBadgeBeforeAuthor(groupRow, badge, authorEl)
    expect(badge.className).toMatch(/amber|yellow|gold|warning/i)

    expect(within(soloRow).queryByText('团曲')).not.toBeInTheDocument()
  })

  e2eIt(1, 'T1-F14-02', 'F14', '团曲 filter option narrows library rows to group songs only', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    seedLibraryWithGroupAndSolo(bridge)

    const user = userEvent.setup()
    render(<LibraryPage />)
    await waitFor(() => expect(screen.getByTestId('library-count')).toHaveTextContent('显示 5 / 5 个曲目'))

    await user.click(screen.getByRole('button', { name: '轨道筛选' }))
    await user.click(await screen.findByRole('menuitemcheckbox', { name: /团曲/ }))

    expect(screen.getByTestId('library-count')).toHaveTextContent('显示 2 / 5 个曲目')
    expect(screen.getByTestId('library-row-group-campus')).toBeInTheDocument()
    expect(screen.getByTestId('library-row-group-encore')).toBeInTheDocument()
    expect(screen.queryByTestId('library-row-solo-temari')).not.toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: '轨道筛选' }))
    await user.click(await screen.findByRole('menuitemcheckbox', { name: '全部曲目' }))
    expect(screen.getByTestId('library-count')).toHaveTextContent('显示 5 / 5 个曲目')
  })

  e2eIt(1, 'T1-F14-03', 'F14', 'Auto-creates filename-sorted group song when >=2 body VMDs are selected', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    render(<LibraryPage />)
    await motionStore.openImportNew([
      'C:\\Songs\\03_kotone.vmd',
      'C:\\Songs\\01_saki.vmd',
      'C:\\Songs\\02_temari.vmd',
      'C:\\Songs\\cam.vmd',
      'C:\\Songs\\song.flac',
    ])

    const dialog = await screen.findByTestId('import-dialog')
    const text = dialog.textContent ?? ''
    expect(text).toContain('1号位')
    expect(text).toContain('2号位')
    expect(text).toContain('3号位')

    // Verify filename-sorted assignment order in DOM: 01_saki -> 02_temari -> 03_kotone
    const sakiPos = text.indexOf('01_saki.vmd')
    const temariPos = text.indexOf('02_temari.vmd')
    const kotonePos = text.indexOf('03_kotone.vmd')
    expect(sakiPos).toBeGreaterThan(-1)
    expect(temariPos).toBeGreaterThan(sakiPos)
    expect(kotonePos).toBeGreaterThan(temariPos)

    expect(text).toContain('cam.vmd')
    expect(text).toContain('song.flac')

    await userEvent.click(within(dialog).getByRole('button', { name: '导入' }))
    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(true))
    const importCall = spy.mock.calls.find((c) => c[0] === 'library.import')
    const req = unwrapImportRequest(importCall?.[1])
    expect(req.positions?.length).toBe(3)
    expect(req.positions?.[0]?.motionFile ?? req.positions?.[0]?.motionVmd).toContain('01_saki.vmd')
    expect(req.positions?.[1]?.motionFile ?? req.positions?.[1]?.motionVmd).toContain('02_temari.vmd')
    expect(req.positions?.[2]?.motionFile ?? req.positions?.[2]?.motionVmd).toContain('03_kotone.vmd')
  })

  e2eIt(1, 'T1-F14-04', 'F14', 'ImportDialog renders 1号位 locked card, 追加位置 button, 2号位+ cards, and separate camera+audio card', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    render(<LibraryPage />)
    await user.click(screen.getByRole('button', { name: '添加到曲库' }))
    const dialog = await screen.findByTestId('import-dialog')

    const appendFilesBtn = within(dialog).getByRole('button', { name: /追加文件/ })
    const addPosBtn = within(dialog).getByRole('button', { name: /追加位置/ })
    expect(appendFilesBtn.compareDocumentPosition(addPosBtn) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0)

    await user.click(addPosBtn)
    await waitFor(() => expect(within(dialog).getByText('2号位')).toBeInTheDocument())

    const slot1Header = within(dialog).getByText('1号位')
    const slot1Card = slot1Header.closest('[data-testid]') ?? slot1Header.parentElement?.parentElement ?? dialog
    expect(within(slot1Card as HTMLElement).queryByRole('button', { name: /编辑初始位置/ })).not.toBeInTheDocument()

    const slot2Header = within(dialog).getByText('2号位')
    const slot2Card = slot2Header.closest('[data-testid]') ?? slot2Header.parentElement?.parentElement ?? dialog
    expect(within(slot2Card as HTMLElement).getByRole('button', { name: /编辑初始位置/ })).toBeInTheDocument()

    expect(within(dialog).getByText('镜头')).toBeInTheDocument()
    expect(within(dialog).getByText('音频')).toBeInTheDocument()
  })

  e2eIt(1, 'T1-F14-05', 'F14', '编辑初始位置 modal edits X/Y/Z/Yaw and × delete renumbers remaining slots', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    render(<LibraryPage />)
    await motionStore.openImportNew([
      'C:\\Songs\\01_a.vmd',
      'C:\\Songs\\02_b.vmd',
      'C:\\Songs\\03_c.vmd',
    ])
    const dialog = await screen.findByTestId('import-dialog')
    const editButtons = within(dialog).getAllByRole('button', { name: /编辑初始位置/ })
    expect(editButtons.length).toBeGreaterThanOrEqual(2)

    // Open modal for 3号位 (last edit button)
    await user.click(editButtons[editButtons.length - 1])
    const modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    const numInputs = findNumericInputs(modal)
    expect(numInputs.length).toBeGreaterThanOrEqual(4)
    for (const inp of numInputs.slice(0, 4)) {
      expect(parseNumericInputValue(inp)).toBe(0)
    }

    await setNumericInputValue(user, numInputs[0], '1.5')
    await setNumericInputValue(user, numInputs[1], '0.2')
    await setNumericInputValue(user, numInputs[2], '-0.8')
    await setNumericInputValue(user, numInputs[3], '45')

    const confirmBtn = within(modal).getByRole('button', { name: /确定|保存|应用/ })
    await user.click(confirmBtn)

    // Delete 2号位 (first delete slot button)
    const deleteBtns = within(dialog).getAllByRole('button', { name: /删除位置|移除位置|删除\s*2号位|×/ })
    await user.click(deleteBtns[0])

    await waitFor(() => expect(within(dialog).queryByText('3号位')).not.toBeInTheDocument())
    expect(within(dialog).getByText('2号位')).toBeInTheDocument()
    expect(dialog.textContent).toContain('03_c.vmd')
    expect(dialog.textContent).not.toContain('02_b.vmd')
  })

  e2eIt(1, 'T1-F14-06', 'F14', 'Zero-rebake group-song edit preserves unmodified slots when only placement changes', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    seedLibraryWithGroupAndSolo(bridge)
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<LibraryPage />)
    const editBtn = await screen.findByRole('button', { name: '编辑 Campus Mode!!' })
    await user.click(editBtn)

    const dialog = await screen.findByTestId('import-dialog')
    const editPosBtns = within(dialog).getAllByRole('button', { name: /编辑初始位置/ })
    await user.click(editPosBtns[0])

    const modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    const numInputs = findNumericInputs(modal)
    expect(numInputs.length).toBeGreaterThanOrEqual(4)
    await setNumericInputValue(user, numInputs[0], '2.25')
    await setNumericInputValue(user, numInputs[3], '30')
    await user.click(within(modal).getByRole('button', { name: /确定|保存|应用/ }))

    await user.click(within(dialog).getByRole('button', { name: '保存修改' }))
    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(true))

    const importCall = spy.mock.calls.find((c) => c[0] === 'library.import')
    const req = unwrapImportRequest(importCall?.[1])
    expect(req.editKey).toBe('group-campus')
    expect(req.rebakeOutput).toBe(false)
    expect(req.positions?.[1]?.x).toBeCloseTo(2.25, 2)
    expect(req.positions?.[1]?.yawDegrees).toBeCloseTo(30, 1)
  })
})
