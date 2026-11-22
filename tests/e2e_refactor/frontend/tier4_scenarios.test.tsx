import { fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import MotionApp from '@/motion/components/MotionApp'
import type { MotionMockBridgeClient } from '@/motion/mock'
import { motionStore } from '@/motion/store'
import {
  SAMPLE_PORTRAIT_DATA_URI,
  assertBadgeBeforeAuthor,
  assertNoUnexpectedLegacyText,
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  findNumericInputByName,
  findNumericInputs,
  findSlotCardElement,
  getStoreFormation,
  motionFileExists,
  pointerDragSlotCard,
  probeCrossLayerReady,
  setNumericInputValue,
  unwrapFacialWeights,
  unwrapImportRequest,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

describe('Tier 4 — Real-World Frontend + Cross-Layer Application Scenarios', () => {
  e2eIt(4, 'T4-FE-01', 'F14', 'Full Group-Song Import, Placement Modal, Slot Renumber, Gold Badge Filter & Zero-Rebake Edit (图3 & 图4)', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF14: true }))) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<MotionApp />)
    await motionStore.openImportNew([
      'C:\\Stage\\char_c.vmd',
      'C:\\Stage\\char_a.vmd',
      'C:\\Stage\\char_b.vmd',
      'C:\\Stage\\cam.vmd',
      'C:\\Stage\\bgm.flac',
    ])

    const dialog = await screen.findByTestId('import-dialog')
    const dialogText = dialog.textContent ?? ''
    expect(dialogText.indexOf('char_a.vmd')).toBeLessThan(dialogText.indexOf('char_b.vmd'))
    expect(dialogText.indexOf('char_b.vmd')).toBeLessThan(dialogText.indexOf('char_c.vmd'))
    expect(dialogText).toContain('1号位')
    expect(dialogText).toContain('2号位')
    expect(dialogText).toContain('3号位')
    expect(dialogText).toContain('cam.vmd')
    expect(dialogText).toContain('bgm.flac')

    // Append 4号位 and assign a 4th body VMD so deleting 3号位 leaves 3号位 valid (never empty)
    await user.click(within(dialog).getByRole('button', { name: /追加位置/ }))
    await waitFor(() => expect(within(dialog).getByText('4号位')).toBeInTheDocument())
    await motionStore.importAppendFiles(['C:\\Stage\\char_d.vmd'])
    await waitFor(() => expect(dialog.textContent).toContain('char_d.vmd'))

    // Set author/subtitle so LibraryPage displays Author clearly next to gold 团曲 badge
    const subtitleInput = within(dialog).getByLabelText(/副标题|作者/) as HTMLInputElement
    await user.clear(subtitleInput)
    await user.type(subtitleInput, '初星学園, 花海咲季')

    // Set placement on 2号位
    const editPosBtns = within(dialog).getAllByRole('button', { name: /编辑初始位置/ })
    await user.click(editPosBtns[0])
    const modal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    const nums = findNumericInputs(modal)
    expect(nums.length).toBeGreaterThanOrEqual(4)
    await setNumericInputValue(user, nums[0], '1.2')
    await setNumericInputValue(user, nums[2], '-0.5')
    await setNumericInputValue(user, nums[3], '15')
    await user.click(within(modal).getByRole('button', { name: /确定|保存|应用/ }))

    // Delete 3号位 (char_c.vmd) -> 4号位 (char_d.vmd) renumbers to 3号位 intact
    const delBtns = within(dialog).getAllByRole('button', { name: /删除位置|移除位置|删除\s*\d号位|×/ })
    await user.click(delBtns[1])
    await waitFor(() => expect(within(dialog).queryByText('4号位')).not.toBeInTheDocument())
    expect(dialog.textContent).toContain('char_d.vmd')
    expect(dialog.textContent).not.toContain('char_c.vmd')

    // Submit import (now all 3 positions have valid body VMDs)
    await user.click(within(dialog).getByRole('button', { name: '导入' }))
    await waitFor(() => expect(spy.mock.calls.some((c) => c[0] === 'library.import')).toBe(true))
    const firstImportCall = spy.mock.calls.find((c) => c[0] === 'library.import')
    const firstReq = unwrapImportRequest(firstImportCall?.[1])
    expect(Boolean(firstReq.isGroup)).toBe(true)
    expect(firstReq.positions?.length).toBe(3)
    expect(firstReq.positions?.[2]?.motionFile ?? firstReq.positions?.[2]?.motionVmd).toContain('char_d.vmd')

    // Wait for mock import to finish and import progress modal to close
    await waitFor(() => expect(motionStore.getState().importUi.visible).toBe(false), { timeout: 3000 })

    // Filter library by 团曲 and verify gold badge left of Author
    await user.click(screen.getByRole('button', { name: '轨道筛选' }))
    await user.click(await screen.findByRole('menuitemcheckbox', { name: /团曲/ }))

    const groupEntries = motionStore
      .getState()
      .library.entries.filter((e) => (e as unknown as { isGroup?: boolean }).isGroup)
    expect(groupEntries.length).toBeGreaterThanOrEqual(1)
    const importedGroup = groupEntries[0]
    const groupRow = await screen.findByTestId(`library-row-${importedGroup.id}`)
    const badge = within(groupRow).getByText('团曲')
    const authorEl = within(groupRow).getByText(/初星学園/)
    assertBadgeBeforeAuthor(groupRow, badge, authorEl)
    expect(badge.className).toMatch(/amber|yellow|gold|warning/i)

    // Zero-rebake edit flow: edit 2号位 initial placement only -> rebakeOutput === false
    await user.click(within(groupRow).getByRole('button', { name: `编辑 ${importedGroup.title}` }))
    const editDialog = await screen.findByTestId('import-dialog')
    const editDialogPosBtns = within(editDialog).getAllByRole('button', { name: /编辑初始位置/ })
    await user.click(editDialogPosBtns[0])
    const editModal = await screen.findByRole('dialog', { name: /初始位置|编辑初始位置/ })
    const editNums = findNumericInputs(editModal)
    await setNumericInputValue(user, editNums[0], '2.5')
    await user.click(within(editModal).getByRole('button', { name: /确定|保存|应用/ }))

    const prevImportCount = spy.mock.calls.filter((c) => c[0] === 'library.import').length
    await user.click(within(editDialog).getByRole('button', { name: '保存修改' }))
    await waitFor(
      () => expect(spy.mock.calls.filter((c) => c[0] === 'library.import').length).toBeGreaterThan(prevImportCount),
    )
    const editImportCalls = spy.mock.calls.filter((c) => c[0] === 'library.import')
    const editReq = unwrapImportRequest(editImportCalls[editImportCalls.length - 1]?.[1])
    expect(editReq.editKey).toBe(importedGroup.id)
    expect(editReq.rebakeOutput).toBe(false)
    expect(editReq.positions?.[1]?.x).toBeCloseTo(2.5, 2)
  })

  e2eIt(4, 'T4-FE-02', 'F15', 'Full Motion Portrait Strip, Overflow, Drag-Drop Swap, 2x2 Rotary Knobs, Bone Save & Camera Cleanup (图1, 图2, 图5)', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeCrossLayerReady({ requireF15: true, requireF16: true }))) return
    bridge = await createMockSession()
    const origReq = bridge.request.bind(bridge)
    const spy = vi.spyOn(bridge, 'request').mockImplementation(async (method, params) => {
      if ((method as string) === 'actors.getPortrait') {
        const p = (params ?? {}) as { character?: string }
        return {
          ok: true,
          character: p.character ?? '',
          dataUri: p.character ? SAMPLE_PORTRAIT_DATA_URI : null,
          cached: Boolean(p.character),
        } as never
      }
      return origReq(method, params as never)
    })

    const user = userEvent.setup()
    expect(motionFileExists('components/EnsembleSection.tsx')).toBe(false)

    // First verify live slot-vs-actor swap invariant on active mock formation (1号位 <-> 2号位)
    await bridge.request('formation.setSlotOffset' as never, { slotIndex: 0, x: 0.3, y: 0, z: 0, yawDegrees: 10 } as never)
    await bridge.request('formation.setSlotOffset' as never, { slotIndex: 1, x: -0.7, y: 0, z: 0.2, yawDegrees: -20 } as never)
    await bridge.request('formation.selectSlot' as never, { slotIndex: 0 } as never)
    motionStore.commitFacialWeights({ overall: 1.35, brow: 1.1, eye: 1.6, mouth: 0.9 })

    await waitFor(() => {
      const f = getStoreFormation()
      expect(f.slots[0]?.offset?.x).toBeCloseTo(0.3, 2)
      expect(f.slots[1]?.offset?.x).toBeCloseTo(-0.7, 2)
      expect(f.slots[0]?.facialWeights?.eye).toBeCloseTo(1.6, 2)
    })
    const preSwap0 = structuredClone(getStoreFormation().slots[0])
    const preSwap1 = structuredClone(getStoreFormation().slots[1])

    render(<MotionApp />)
    await user.click(screen.getByRole('button', { name: '动作' }))
    await waitFor(() => expect(screen.getByText('2号位')).toBeInTheDocument())

    const liveSlot1 = findSlotCardElement(screen.getByText('1号位'))
    const liveSlot2 = findSlotCardElement(screen.getByText('2号位'))
    pointerDragSlotCard(liveSlot1, liveSlot2)

    await waitFor(() => expect(getStoreFormation().slots[0]?.actorToken).toBe(preSwap1.actorToken))
    const postSwap = getStoreFormation()
    expect(postSwap.slots[0].offset.x).toBeCloseTo(0.3, 2)
    expect(postSwap.slots[0].offset.yawDegrees).toBeCloseTo(10, 1)
    expect(postSwap.slots[0].sourceSlotIndex).toBe(preSwap0.sourceSlotIndex)
    expect(postSwap.slots[1].offset.x).toBeCloseTo(-0.7, 2)
    expect(postSwap.slots[1].offset.yawDegrees).toBeCloseTo(-20, 1)
    expect(postSwap.slots[1].sourceSlotIndex).toBe(preSwap1.sourceSlotIndex)
    expect(postSwap.slots[1].actorToken).toBe(preSwap0.actorToken)
    expect(postSwap.slots[1].character).toBe(preSwap0.character)
    expect(postSwap.slots[1].shortName).toBe(preSwap0.shortName)
    expect(postSwap.slots[1].hasActorBoneOverrides).toBe(preSwap0.hasActorBoneOverrides)
    expect(postSwap.slots[1].facialWeights.eye).toBeCloseTo(1.6, 2)

    if (preSwap1.shortName) {
      expect(within(findSlotCardElement(screen.getByText('1号位'))).getByText(new RegExp(preSwap1.shortName))).toBeInTheDocument()
    }
    if (preSwap0.shortName) {
      expect(within(findSlotCardElement(screen.getByText('2号位'))).getByText(new RegExp(preSwap0.shortName))).toBeInTheDocument()
    }

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 5,
      selectedSlotIndex: 0,
      slots: [
        {
          slotIndex: 0,
          occupied: true,
          actorToken: 101,
          character: 'hski',
          shortName: '咲季',
          displayName: '花海咲季',
          isOverflow: false,
          hasTrackMotion: true,
          sourceSlotIndex: 0,
          offset: { x: 0, y: 0, z: 0, yawDegrees: 0 },
          placementDirty: false,
          facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
          hasActorBoneOverrides: false,
        },
        {
          slotIndex: 1,
          occupied: true,
          actorToken: 102,
          character: 'ttmr',
          shortName: '手毬',
          displayName: '月村手毬',
          isOverflow: false,
          hasTrackMotion: true,
          sourceSlotIndex: 1,
          offset: { x: 0, y: 0, z: 0, yawDegrees: 0 },
          placementDirty: false,
          facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
          hasActorBoneOverrides: false,
        },
        {
          slotIndex: 2,
          occupied: true,
          actorToken: 103,
          character: 'fktn',
          shortName: 'ことね',
          displayName: '藤田ことね',
          isOverflow: false,
          hasTrackMotion: true,
          sourceSlotIndex: 2,
          offset: { x: 0, y: 0, z: 0, yawDegrees: 0 },
          placementDirty: false,
          facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
          hasActorBoneOverrides: false,
        },
        {
          slotIndex: 3,
          occupied: true,
          actorToken: 104,
          character: '',
          shortName: 'NPC_A',
          displayName: 'NPC_A',
          isOverflow: true,
          hasTrackMotion: false,
          sourceSlotIndex: 0,
          offset: { x: 0.5, y: 0, z: 0, yawDegrees: 0 },
          placementDirty: false,
          facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
          hasActorBoneOverrides: false,
        },
        {
          slotIndex: 4,
          occupied: false,
          actorToken: 0,
          character: '',
          shortName: '',
          displayName: '',
          isOverflow: true,
          hasTrackMotion: false,
          sourceSlotIndex: 1,
          offset: { x: 1.0, y: 0, z: 0, yawDegrees: 0 },
          placementDirty: false,
          facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
          hasActorBoneOverrides: false,
        },
      ],
    } as never)

    await waitFor(() => expect(screen.getByText('5号位')).toBeInTheDocument())

    // Drag 4号位 onto empty 5号位 (+)
    const slot4 = findSlotCardElement(screen.getByText('4号位'))
    const slot5 = findSlotCardElement(screen.getByText('5号位'))
    pointerDragSlotCard(slot4, slot5)
    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            (c[0] as string) === 'formation.swapSlots' &&
            (c[1] as { fromSlotIndex?: number; toSlotIndex?: number }).fromSlotIndex === 3 &&
            (c[1] as { toSlotIndex?: number }).toSlotIndex === 4,
        ),
      ).toBe(true),
    )

    // Adjust 2x2 RotaryKnobCard
    const overallInput = findNumericInputByName(/整体/)
    await setNumericInputValue(user, overallInput, '145')
    await waitFor(() => {
      const calls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights')
      expect(calls.some((c) => Math.abs((unwrapFacialWeights(c[1]).overall ?? 0) - 1.45) < 0.02)).toBe(true)
    })
    await user.click(screen.getByRole('button', { name: '保存表情幅度' }))
    expect(spy.mock.calls.some((c) => c[0] === 'facial.save')).toBe(true)

    // Bone save slot & global
    await user.click(screen.getByText('高级动作设置'))
    await user.click(screen.getByRole('button', { name: '保存至当前曲目' }))
    await user.click(screen.getByRole('button', { name: '保存至全局' }))
    expect(spy.mock.calls.some((c) => c[0] === 'bone.saveToMotion')).toBe(true)

    // CameraPage "全部" & 2号位
    await user.click(screen.getByRole('button', { name: '镜头' }))
    await user.click(screen.getByRole('combobox', { name: '镜头模式' }))
    await user.click(await screen.findByRole('option', { name: '跟随' }))
    await waitFor(() => expect(motionStore.getState().cameraPending).toBe(false))
    await user.click(screen.getByRole('combobox', { name: /跟随位置|跟随目标|跟随槽位/ }))
    await user.click(await screen.findByRole('option', { name: '全部' }))
    await user.click(screen.getByRole('button', { name: '保存镜头设置' }))

    // StagePage & StatusBar cleanup check
    await user.click(screen.getByRole('button', { name: '舞台' }))
    assertNoUnexpectedLegacyText(document.body)
  })
})
