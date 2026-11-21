import { fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import { MotionPage } from '@/motion/pages/MotionPage'
import { motionStore } from '@/motion/store'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
  SAMPLE_PORTRAIT_DATA_URI,
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  findDashedSlotCard,
  findNumericInputByName,
  findSlotCardElement,
  getStoreFormation,
  isSlotStripViewportEl,
  probeF15Ready,
  setNumericInputValue,
  unwrapFacialWeights,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

function pushFormationSnapshot(b: MotionMockBridgeClient, payload: Record<string, unknown>) {
  b.push('formation.changed' as never, payload as never)
}

function makeSlot(idx: number, overrides: Record<string, unknown> = {}) {
  return {
    slotIndex: idx,
    occupied: true,
    actorToken: 101 + idx,
    character: idx === 0 ? 'hski' : idx === 1 ? 'ttmr' : 'fktn',
    shortName: idx === 0 ? '咲季' : idx === 1 ? '手毬' : 'ことね',
    displayName: idx === 0 ? '花海咲季' : idx === 1 ? '月村手毬' : '藤田ことね',
    isOverflow: false,
    hasTrackMotion: true,
    sourceSlotIndex: idx,
    offset: { x: 0, y: 0, z: 0, yawDegrees: 0 },
    placementDirty: false,
    facialWeights: { overall: 1, brow: 1, eye: 1, mouth: 1 },
    hasActorBoneOverrides: false,
    ...overrides,
  }
}

describe('Tier 1 — F15 Motion Slot Strip, Rotary Knobs & Bones UI', () => {
  e2eIt(1, 'T1-F15-01', 'F15', 'Slot strip renders bust-up portrait cards with slot labels and NPC/uncached fallback', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()

    const origRequest = bridge.request.bind(bridge)
    vi.spyOn(bridge, 'request').mockImplementation(async (method, params) => {
      if ((method as string) === 'actors.getPortrait') {
        const p = (params ?? {}) as { character?: string }
        if (p.character === 'hski' || p.character === 'ttmr') {
          return { ok: true, character: p.character, dataUri: SAMPLE_PORTRAIT_DATA_URI, cached: true } as never
        }
        return { ok: true, character: p.character ?? '', dataUri: null, cached: false } as never
      }
      return origRequest(method, params as never)
    })

    bridge.push('actors.changed', {
      epoch: 5,
      actors: [
        { token: 101, label: '花海咲季 · 1', selected: true, character: 'hski', displayName: '花海咲季', shortName: '咲季' },
        { token: 102, label: '月村手毬 · 2', selected: false, character: 'ttmr', displayName: '月村手毬', shortName: '手毬' },
        { token: 103, label: 'NPC舞者 · 3', selected: false, character: '', displayName: 'NPC舞者', shortName: 'NPC舞者' },
      ],
      targetLabel: '花海咲季 · 1',
      boundLabel: '花海咲季 · 1',
    } as never)

    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 3,
      selectedSlotIndex: 0,
      slots: [
        makeSlot(0, { actorToken: 101, character: 'hski', shortName: '咲季' }),
        makeSlot(1, { actorToken: 102, character: 'ttmr', shortName: '手毬' }),
        makeSlot(2, { actorToken: 103, character: '', shortName: 'NPC舞者', displayName: 'NPC舞者' }),
      ],
    })

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('1号位')).toBeInTheDocument())
    expect(screen.getByText('2号位')).toBeInTheDocument()
    expect(screen.getByText('3号位')).toBeInTheDocument()
    expect(screen.getByText(/咲季/)).toBeInTheDocument()
    expect(screen.getByText(/NPC舞者/)).toBeInTheDocument()

    await waitFor(() => {
      const imgs = document.querySelectorAll('img')
      const dataImgs = Array.from(imgs).filter((i) => i.getAttribute('src')?.startsWith('data:image/'))
      expect(dataImgs.length).toBeGreaterThanOrEqual(2)
    })
  })

  e2eIt(1, 'T1-F15-02', 'F15', 'Slot count follows max(actors, groupPositions) for group songs and actors for solo', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    render(<MotionPage />)

    // (a) 2 actors + unselected -> 2 slots
    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: false,
      trackPositionCount: 1,
      slotCount: 2,
      selectedSlotIndex: 0,
      slots: [makeSlot(0), makeSlot(1)],
    })
    await waitFor(() => {
      expect(screen.getByText('1号位')).toBeInTheDocument()
      expect(screen.getByText('2号位')).toBeInTheDocument()
      expect(screen.queryByText('3号位')).not.toBeInTheDocument()
    })

    // (b) 2 actors + 4-position group song -> 4 slots (slots 3-4 empty)
    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 4,
      slotCount: 4,
      selectedSlotIndex: 0,
      slots: [
        makeSlot(0),
        makeSlot(1),
        makeSlot(2, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
        makeSlot(3, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
      ],
    })
    await waitFor(() => expect(screen.getByText('4号位')).toBeInTheDocument())

    // (c) 5 actors + 3-position group song -> 5 slots (slots 4-5 overflow)
    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 5,
      selectedSlotIndex: 0,
      slots: [
        makeSlot(0),
        makeSlot(1),
        makeSlot(2),
        makeSlot(3, { isOverflow: true, hasTrackMotion: false, sourceSlotIndex: 0, shortName: '千奈' }),
        makeSlot(4, { isOverflow: true, hasTrackMotion: false, sourceSlotIndex: 1, shortName: '広' }),
      ],
    })
    await waitFor(() => expect(screen.getByText('5号位')).toBeInTheDocument())
  })

  e2eIt(1, 'T1-F15-03', 'F15', 'Slot strip supports [<]/[>] boundary-disabled navigation and auto-scroll', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 6,
      slotCount: 6,
      selectedSlotIndex: 0,
      slots: [0, 1, 2, 3, 4, 5].map((i) => makeSlot(i)),
    })

    const protoRect = HTMLElement.prototype.getBoundingClientRect
    vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) {
      if (isSlotStripViewportEl(this)) {
        return { width: 340, height: 140, top: 0, left: 0, right: 340, bottom: 140, x: 0, y: 0, toJSON: () => {} } as DOMRect
      }
      return protoRect.call(this)
    })
    const clientWidthOwner = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'clientWidth')
      ? HTMLElement.prototype
      : Element.prototype
    const origClientWidthGet = Object.getOwnPropertyDescriptor(clientWidthOwner, 'clientWidth')?.get
    vi.spyOn(clientWidthOwner as HTMLElement, 'clientWidth', 'get').mockImplementation(function (this: HTMLElement) {
      if (isSlotStripViewportEl(this)) return 340
      return origClientWidthGet ? origClientWidthGet.call(this) : 0
    })

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('1号位')).toBeInTheDocument())

    const prevBtn = screen.getByRole('button', { name: /上一|向左|</ })
    const nextBtn = screen.getByRole('button', { name: /下一|向右|>/ })
    expect(prevBtn).toBeDisabled()
    expect(nextBtn).toBeEnabled()

    await user.click(nextBtn)
    expect(prevBtn).toBeEnabled()

    // Click until end
    for (let i = 0; i < 6 && !nextBtn.hasAttribute('disabled'); i += 1) {
      await user.click(nextBtn)
    }
    expect(nextBtn).toBeDisabled()
  })

  e2eIt(1, 'T1-F15-04', 'F15', 'Empty + slot shows 当前位置上无角色 and overflow slot shows dashed border + reuse dropdown', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 4,
      selectedSlotIndex: 2,
      slots: [
        makeSlot(0),
        makeSlot(1),
        makeSlot(2, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
        makeSlot(3, { isOverflow: true, hasTrackMotion: false, sourceSlotIndex: 0, shortName: '千奈' }),
      ],
    })

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText(/当前位置上无角色/)).toBeInTheDocument())

    // Select overflow slot 4号位
    await user.click(screen.getByText('4号位'))
    await waitFor(() =>
      expect(spy.mock.calls.some((c) => (c[0] as string) === 'formation.selectSlot')).toBe(true),
    )

    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 4,
      selectedSlotIndex: 3,
      slots: [
        makeSlot(0),
        makeSlot(1),
        makeSlot(2, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
        makeSlot(3, { isOverflow: true, hasTrackMotion: false, sourceSlotIndex: 0, shortName: '千奈' }),
      ],
    })

    const slot4Card = findDashedSlotCard(screen.getByText('4号位'))
    expect(slot4Card).not.toBeNull()

    // Reused slot selector should be visible for overflow slot
    const reuseTrigger = await screen.findByRole('combobox', { name: /复用|动作来源|复用槽位/ })
    await user.click(reuseTrigger)
    await user.click(await screen.findByRole('option', { name: /2号位/ }))
    await waitFor(() =>
      expect(spy.mock.calls.some((c) => (c[0] as string) === 'formation.setSlotSource')).toBe(true),
    )
  })

  e2eIt(1, 'T1-F15-05', 'F15', 'Drag-and-drop swaps occupied slots and occupied-to-empty + slots', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    // Configure distinct offsets & facial weights via real RPCs so mock swaps authentic state
    await bridge.request('formation.setSlotOffset' as never, { slotIndex: 0, x: 0.25, y: 0, z: 0.1, yawDegrees: 5 } as never)
    await bridge.request('formation.setSlotOffset' as never, { slotIndex: 1, x: 1.0, y: 0, z: 0, yawDegrees: 15 } as never)
    await bridge.request('formation.selectSlot' as never, { slotIndex: 0 } as never)
    motionStore.commitFacialWeights({ overall: 1.4, brow: 1.2, eye: 1.5, mouth: 0.8 })

    await waitFor(() => {
      const f = getStoreFormation()
      expect(f.slots[0]?.offset?.x).toBeCloseTo(0.25, 2)
      expect(f.slots[1]?.offset?.x).toBeCloseTo(1.0, 2)
      expect(f.slots[0]?.facialWeights?.eye).toBeCloseTo(1.5, 2)
    })

    const pre = getStoreFormation()
    const preSlot0 = structuredClone(pre.slots[0])
    const preSlot1 = structuredClone(pre.slots[1])

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('1号位')).toBeInTheDocument())

    const slot1 = findSlotCardElement(screen.getByText('1号位'))
    const slot2 = findSlotCardElement(screen.getByText('2号位'))

    fireEvent.dragStart(slot1)
    fireEvent.dragOver(slot2)
    fireEvent.drop(slot2)

    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            (c[0] as string) === 'formation.swapSlots' &&
            (c[1] as { fromSlotIndex?: number; toSlotIndex?: number }).fromSlotIndex === 0 &&
            (c[1] as { fromSlotIndex?: number; toSlotIndex?: number }).toSlotIndex === 1,
        ),
      ).toBe(true),
    )

    // Verify post-swap slot-vs-actor invariant in store state:
    // offset & sourceSlotIndex stay with the slot index; actor entity + facialWeights + boneOverrides follow the actor
    await waitFor(() => expect(getStoreFormation().slots[0]?.actorToken).toBe(preSlot1.actorToken))
    const post = getStoreFormation()
    expect(post.slots[0].offset.x).toBeCloseTo(0.25, 2)
    expect(post.slots[0].offset.z).toBeCloseTo(0.1, 2)
    expect(post.slots[0].offset.yawDegrees).toBeCloseTo(5, 1)
    expect(post.slots[0].sourceSlotIndex).toBe(preSlot0.sourceSlotIndex)

    expect(post.slots[1].offset.x).toBeCloseTo(1.0, 2)
    expect(post.slots[1].offset.z).toBeCloseTo(0, 2)
    expect(post.slots[1].offset.yawDegrees).toBeCloseTo(15, 1)
    expect(post.slots[1].sourceSlotIndex).toBe(preSlot1.sourceSlotIndex)

    expect(post.slots[0].actorToken).toBe(preSlot1.actorToken)
    expect(post.slots[0].character).toBe(preSlot1.character)
    expect(post.slots[0].shortName).toBe(preSlot1.shortName)
    expect(post.slots[0].hasActorBoneOverrides).toBe(preSlot1.hasActorBoneOverrides)
    expect(post.slots[0].facialWeights.overall).toBeCloseTo(preSlot1.facialWeights.overall, 2)

    expect(post.slots[1].actorToken).toBe(preSlot0.actorToken)
    expect(post.slots[1].character).toBe(preSlot0.character)
    expect(post.slots[1].shortName).toBe(preSlot0.shortName)
    expect(post.slots[1].hasActorBoneOverrides).toBe(preSlot0.hasActorBoneOverrides)
    expect(post.slots[1].facialWeights.overall).toBeCloseTo(1.4, 2)
    expect(post.slots[1].facialWeights.eye).toBeCloseTo(1.5, 2)

    // Verify rendered slot UI reflects swapped actor names on 1号位 & 2号位
    const postSlot1Card = findSlotCardElement(screen.getByText('1号位'))
    const postSlot2Card = findSlotCardElement(screen.getByText('2号位'))
    if (preSlot1.shortName) {
      expect(within(postSlot1Card).getByText(new RegExp(preSlot1.shortName))).toBeInTheDocument()
    }
    if (preSlot0.shortName) {
      expect(within(postSlot2Card).getByText(new RegExp(preSlot0.shortName))).toBeInTheDocument()
    }

    // Occupied-to-empty + slot drag-drop test
    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 3,
      selectedSlotIndex: 0,
      slots: [
        makeSlot(0, { offset: { x: 0, y: 0, z: 0, yawDegrees: 0 } }),
        makeSlot(1, { offset: { x: 1.0, y: 0, z: 0, yawDegrees: 15 } }),
        makeSlot(2, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '', offset: { x: -1.0, y: 0, z: 0, yawDegrees: -15 } }),
      ],
    })
    await waitFor(() => expect(screen.getByText('3号位')).toBeInTheDocument())
    const slot1After = findSlotCardElement(screen.getByText('1号位'))
    const slot3 = findSlotCardElement(screen.getByText('3号位'))

    fireEvent.dragStart(slot1After)
    fireEvent.dragOver(slot3)
    fireEvent.drop(slot3)

    await waitFor(() =>
      expect(
        spy.mock.calls.some(
          (c) =>
            (c[0] as string) === 'formation.swapSlots' &&
            (c[1] as { fromSlotIndex?: number; toSlotIndex?: number }).fromSlotIndex === 0 &&
            (c[1] as { toSlotIndex?: number }).toSlotIndex === 2,
        ),
      ).toBe(true),
    )
  })

  e2eIt(1, 'T1-F15-06', 'F15', 'Solo mode hides reuse dropdown while multi-actor solo exposes placement offset and reuse', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    // Normal solo mode: 1 actor, not group song
    pushFormationSnapshot(bridge, {
      active: false,
      isGroupSong: false,
      trackPositionCount: 1,
      slotCount: 1,
      selectedSlotIndex: 0,
      slots: [makeSlot(0)],
    })

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('1号位')).toBeInTheDocument())
    expect(screen.queryByRole('combobox', { name: /复用|动作来源|复用槽位/ })).not.toBeInTheDocument()

    // Multi-actor solo mode: 2 actors on single song, select 2号位
    pushFormationSnapshot(bridge, {
      active: true,
      isGroupSong: false,
      trackPositionCount: 1,
      slotCount: 2,
      selectedSlotIndex: 1,
      slots: [
        makeSlot(0),
        makeSlot(1, { isOverflow: false, hasTrackMotion: false, sourceSlotIndex: 0, offset: { x: 0.5, y: 0, z: 0, yawDegrees: 0 } }),
      ],
    })

    await waitFor(() => expect(screen.getByText('2号位')).toBeInTheDocument())
    const savePosBtn = screen.getByRole('button', { name: /保存.*位置|保存站位|保存当前曲目位置/ })
    await userEvent.click(savePosBtn)
    await waitFor(() =>
      expect(spy.mock.calls.some((c) => (c[0] as string) === 'formation.saveSlotOffset')).toBe(true),
    )
  })

  e2eIt(1, 'T1-F15-07', 'F15', '2x2 RotaryKnobCard (0%-200%, default 100%) editable % & BoneSection slot/global save', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<MotionPage />)
    for (const label of ['整体', '眉毛', '眼部', '嘴部']) {
      expect(screen.getByText(label)).toBeInTheDocument()
    }
    expect(screen.getAllByText(/100\s*%/).length).toBeGreaterThanOrEqual(4)

    // Edit bottom-right % on 眼部 knob to 150
    const eyeInput = findNumericInputByName(/眼部/)
    await setNumericInputValue(user, eyeInput, '150')

    await waitFor(() => {
      const calls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights')
      expect(calls.some((c) => unwrapFacialWeights(c[1]).eye === 1.5)).toBe(true)
    })

    await user.click(screen.getByRole('button', { name: '保存表情幅度' }))
    expect(spy.mock.calls.some((c) => c[0] === 'facial.save')).toBe(true)

    await user.click(screen.getByText('高级动作设置'))
    await user.click(screen.getByRole('button', { name: '保存至当前曲目' }))
    expect(spy.mock.calls.some((c) => c[0] === 'bone.saveToMotion')).toBe(true)

    await user.click(screen.getByRole('button', { name: '保存至全局' }))
    expect(spy.mock.calls.some((c) => c[0] === 'bone.saveToGlobal' || (c[0] as string) === 'bone.saveGlobal')).toBe(true)
  })
})
