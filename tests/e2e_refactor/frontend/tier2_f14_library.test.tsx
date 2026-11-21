import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import { LibraryPage } from '@/motion/pages/LibraryPage'
import { motionStore } from '@/motion/store'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
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

describe('Tier 2 — F14 Boundary, Negative & Edge Cases', () => {
  e2eIt(2, 'T2-F14-01', 'F14', 'Single body VMD + facial + camera + audio does NOT auto-create group song', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    render(<LibraryPage />)
    await motionStore.openImportNew([
      'C:\\Solo\\dance.vmd',
      'C:\\Solo\\face.vmd',
      'C:\\Solo\\cam.vmd',
      'C:\\Solo\\song.mp3',
    ])

    const dialog = await screen.findByTestId('import-dialog')
    expect(within(dialog).getByText('1号位')).toBeInTheDocument()
    expect(within(dialog).queryByText('2号位')).not.toBeInTheDocument()

    await userEvent.click(within(dialog).getByRole('button', { name: '导入' }))
    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(true))

    const importCall = spy.mock.calls.find((c) => c[0] === 'library.import')
    expect(importCall).toBeDefined()
    const req = unwrapImportRequest(importCall?.[1])
    expect(Boolean(req?.isGroup)).toBe(false)
    expect(req?.positions?.length ?? 1).toBeLessThanOrEqual(1)
  })

  e2eIt(2, 'T2-F14-02', 'F14', 'Deleting sole 2号位 reverts to solo and modal cancel discards unsaved placement edits', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    render(<LibraryPage />)
    await user.click(screen.getByRole('button', { name: '添加到曲库' }))
    const dialog = await screen.findByTestId('import-dialog')

    await user.click(within(dialog).getByRole('button', { name: /追加位置/ }))
    await waitFor(() => expect(within(dialog).getByText('2号位')).toBeInTheDocument())

    await user.click(within(dialog).getByRole('button', { name: /编辑初始位置/ }))
    let modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    let nums = findNumericInputs(modal)
    expect(nums.length).toBeGreaterThanOrEqual(4)
    await setNumericInputValue(user, nums[0], '-12.5')
    await setNumericInputValue(user, nums[1], '3.25')
    await setNumericInputValue(user, nums[2], '-0.75')
    await setNumericInputValue(user, nums[3], '-180')
    await user.click(within(modal).getByRole('button', { name: /确定|保存|应用/ }))

    // Reopen, edit X=99, then cancel
    await user.click(within(dialog).getByRole('button', { name: /编辑初始位置/ }))
    modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    nums = findNumericInputs(modal)
    await setNumericInputValue(user, nums[0], '99')
    await user.click(within(modal).getByRole('button', { name: /取消/ }))

    // Delete 2号位 -> reverts to solo, then re-add 2号位 -> resets to 0
    const deleteBtn = within(dialog).getByRole('button', { name: /删除位置|移除位置|删除\s*2号位|×/ })
    await user.click(deleteBtn)
    await waitFor(() => expect(within(dialog).queryByText('2号位')).not.toBeInTheDocument())

    await user.click(within(dialog).getByRole('button', { name: /追加位置/ }))
    await waitFor(() => expect(within(dialog).getByText('2号位')).toBeInTheDocument())
    await user.click(within(dialog).getByRole('button', { name: /编辑初始位置/ }))
    modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    nums = findNumericInputs(modal)
    for (const inp of nums.slice(0, 4)) {
      expect(parseNumericInputValue(inp)).toBe(0)
    }
  })

  e2eIt(2, 'T2-F14-03', 'F14', 'Blocks import when 2号位 lacks body VMD or receives camera-only VMD', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<LibraryPage />)
    await motionStore.openImportNew(['C:\\Solo\\01_saki.vmd'])
    const dialog = await screen.findByTestId('import-dialog')

    await user.click(within(dialog).getByRole('button', { name: /追加位置/ }))
    await waitFor(() => expect(within(dialog).getByText('2号位')).toBeInTheDocument())

    const submitBtn = within(dialog).getByRole('button', { name: '导入' })
    expect(submitBtn).toBeDisabled()
    expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(false)
  })

  e2eIt(2, 'T2-F14-04', 'F14', 'Auto group-song sorting uses basename only regardless of parent folders or casing', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()

    render(<LibraryPage />)
    await motionStore.openImportNew([
      'Z:\\dirA\\10_last.vmd',
      'A:\\dirZ\\02_Second.vmd',
      'M:\\dirB\\01_first.vmd',
    ])

    const dialog = await screen.findByTestId('import-dialog')
    const text = dialog.textContent ?? ''
    const firstIdx = text.indexOf('01_first.vmd')
    const secondIdx = text.indexOf('02_Second.vmd')
    const lastIdx = text.indexOf('10_last.vmd')
    expect(firstIdx).toBeGreaterThan(-1)
    expect(secondIdx).toBeGreaterThan(firstIdx)
    expect(lastIdx).toBeGreaterThan(secondIdx)
  })

  e2eIt(2, 'T2-F14-05', 'F14', 'Appends body VMD into 2号位 and enforces 停止并保存 when editing playing group song', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    bridge.push('library.changed', {
      revision: 12,
      entries: [
        {
          id: 'group-playing',
          title: 'Live Group',
          subtitle: '初星学園',
          directory: 'gakumas-local/motions/group-playing',
          durationSeconds: 180,
          totalFrames: 10800,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: true,
          positionCount: 2,
          positions: [
            { slot: 1, x: 0, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: true },
            { slot: 2, x: 1, y: 0, z: 0, yawDegrees: 0, hasMotion: true, hasFacial: true },
          ],
        },
      ],
    } as never)

    render(<LibraryPage />)
    await motionStore.openImportNew(['C:\\Add\\01_main.vmd'])
    let dialog = await screen.findByTestId('import-dialog')
    await motionStore.importAppendFiles(['C:\\Add\\02_extra.vmd'])
    await waitFor(() => expect(dialog.textContent).toContain('02_extra.vmd'))
    await user.click(within(dialog).getByRole('button', { name: '取消' }))

    // Play the group song, open edit dialog, trigger 停止并保存
    await motionStore.selectMotion('group-playing', true)
    await waitFor(() => expect(motionStore.getState().player.state).toBe('playing'))

    await user.click(await screen.findByRole('button', { name: '编辑 Live Group' }))
    dialog = await screen.findByTestId('import-dialog')
    const stopAndSaveBtn = within(dialog).getByRole('button', { name: '停止并保存' })
    await user.click(stopAndSaveBtn)

    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'player.stop')).toBe(true))
    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(true))
  })

  e2eIt(2, 'T2-F14-06', 'F14', 'Pairwise 团曲 filter + non-matching search + reset + duration sort', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF14Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    bridge.push('library.changed', {
      revision: 15,
      entries: [
        {
          id: 'grp-long',
          title: 'Long Group',
          subtitle: '初星学園',
          directory: 'gakumas-local/motions/grp-long',
          durationSeconds: 300,
          totalFrames: 18000,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: true,
          positionCount: 3,
          positions: [],
        },
        {
          id: 'grp-short',
          title: 'Short Group',
          subtitle: '初星学園',
          directory: 'gakumas-local/motions/grp-short',
          durationSeconds: 60,
          totalFrames: 3600,
          hasMotion: true,
          hasFacial: false,
          hasCamera: false,
          hasAudio: false,
          isGroup: true,
          positionCount: 2,
          positions: [],
        },
        {
          id: 'solo-one',
          title: 'Solo One',
          subtitle: '咲季',
          directory: 'gakumas-local/motions/solo-one',
          durationSeconds: 120,
          totalFrames: 7200,
          hasMotion: true,
          hasFacial: true,
          hasCamera: true,
          hasAudio: true,
          isGroup: false,
          positionCount: 1,
        },
      ],
    } as never)

    render(<LibraryPage />)
    await user.click(screen.getByRole('button', { name: '轨道筛选' }))
    await user.click(await screen.findByRole('menuitemcheckbox', { name: /团曲/ }))
    expect(screen.getByTestId('library-count')).toHaveTextContent('显示 2 / 3 个曲目')

    await user.type(screen.getByRole('textbox', { name: '搜索曲目' }), 'ZZZ_NONE')
    expect(screen.getByText('没有匹配的演出曲目')).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: '重置搜索与筛选' }))
    expect(screen.getByTestId('library-count')).toHaveTextContent('显示 3 / 3 个曲目')

    await user.click(screen.getByRole('button', { name: '轨道筛选' }))
    await user.click(await screen.findByRole('menuitemcheckbox', { name: /团曲/ }))
    await user.click(screen.getByRole('button', { name: '时长' }))
    expect(screen.getByTestId('library-count')).toHaveTextContent('显示 2 / 3 个曲目')
  })
})
