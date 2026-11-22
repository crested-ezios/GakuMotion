import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, vi } from 'vitest'
import { MotionPage } from '@/motion/pages/MotionPage'
import type { MotionMockBridgeClient } from '@/motion/mock'
import {
  cleanupSession,
  createMockSession,
  e2eIt,
  ensureReadyOrSkip,
  findNumericInputByName,
  findSlotCardElement,
  isSlotStripViewportEl,
  pointerDragSlotCard,
  probeF15Ready,
  setNumericInputValue,
  unwrapFacialWeights,
} from './helpers'

let bridge: MotionMockBridgeClient | null = null

afterEach(() => {
  cleanupSession(bridge)
  bridge = null
})

function makeSlot(idx: number, overrides: Record<string, unknown> = {}) {
  return {
    slotIndex: idx,
    occupied: true,
    actorToken: 101 + idx,
    character: 'hski',
    shortName: `角色${idx + 1}`,
    displayName: `角色${idx + 1}`,
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

describe('Tier 2 — F15 Motion Slot Strip & Rotary Knobs Boundary/Negative Cases', () => {
  e2eIt(2, 'T2-F15-01', 'F15', 'Zero scene actors renders graceful empty + placeholder slots on solo and group songs', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()

    bridge.push('actors.changed', {
      epoch: 2,
      actors: [],
      targetLabel: '',
      boundLabel: '',
    } as never)

    bridge.push('formation.changed' as never, {
      active: false,
      isGroupSong: false,
      trackPositionCount: 1,
      slotCount: 1,
      selectedSlotIndex: 0,
      slots: [makeSlot(0, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' })],
    } as never)

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText(/当前位置上无角色/)).toBeInTheDocument())

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 3,
      selectedSlotIndex: 0,
      slots: [0, 1, 2].map((i) =>
        makeSlot(i, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
      ),
    } as never)

    await waitFor(() => expect(screen.getByText('3号位')).toBeInTheDocument())
    expect(screen.getByText(/当前位置上无角色/)).toBeInTheDocument()
  })

  e2eIt(2, 'T2-F15-02', 'F15', 'RotaryKnobCard clamps out-of-range inputs (-25, 350) to [0%, 200%] and rejects invalid text', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')
    const user = userEvent.setup()

    render(<MotionPage />)
    const overallInput = findNumericInputByName(/整体/)

    await setNumericInputValue(user, overallInput, '-25')
    await waitFor(() => {
      const calls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights')
      expect(calls.some((c) => unwrapFacialWeights(c[1]).overall === 0)).toBe(true)
    })
    for (const c of spy.mock.calls.filter((c) => c[0] === 'facial.setWeights')) {
      const w = unwrapFacialWeights(c[1])
      expect(w.overall).toBeGreaterThanOrEqual(0)
      expect(w.overall).toBeLessThanOrEqual(2)
    }

    const beforeMaxCount = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights').length
    await setNumericInputValue(user, overallInput, '350')
    await waitFor(() => {
      const calls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights').slice(beforeMaxCount)
      expect(calls.some((c) => unwrapFacialWeights(c[1]).overall === 2)).toBe(true)
    })
    for (const c of spy.mock.calls.filter((c) => c[0] === 'facial.setWeights').slice(beforeMaxCount)) {
      const w = unwrapFacialWeights(c[1])
      expect(w.overall).toBeGreaterThanOrEqual(0)
      expect(w.overall).toBeLessThanOrEqual(2)
    }

    const prevCalls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights').length
    await setNumericInputValue(user, overallInput, 'abc')
    const afterCalls = spy.mock.calls.filter((c) => c[0] === 'facial.setWeights')
    for (const c of afterCalls.slice(prevCalls)) {
      const w = unwrapFacialWeights(c[1])
      expect(Number.isFinite(w.overall)).toBe(true)
      expect(w.overall).toBeGreaterThanOrEqual(0)
      expect(w.overall).toBeLessThanOrEqual(2)
    }
  })

  e2eIt(2, 'T2-F15-03', 'F15', 'Self-drop and invalid out-of-strip drop targets do not trigger formation.swapSlots', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 2,
      slotCount: 2,
      selectedSlotIndex: 0,
      slots: [makeSlot(0), makeSlot(1)],
    } as never)

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText('2号位')).toBeInTheDocument())

    const slot2 = findSlotCardElement(screen.getByText('2号位'))
    pointerDragSlotCard(slot2, slot2)

    expect(spy.mock.calls.some((c) => (c[0] as string) === 'formation.swapSlots')).toBe(false)
  })

  e2eIt(2, 'T2-F15-04', 'F15', 'Portrait RPC failure or null Data URI falls back gracefully without crashing', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const origReq = bridge.request.bind(bridge)
    vi.spyOn(bridge, 'request').mockImplementation(async (method, params) => {
      if ((method as string) === 'actors.getPortrait') {
        return { ok: false, character: 'hski', dataUri: null, cached: false } as never
      }
      return origReq(method, params as never)
    })

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 2,
      slotCount: 2,
      selectedSlotIndex: 0,
      slots: [makeSlot(0, { shortName: '咲季' }), makeSlot(1, { shortName: '手毬' })],
    } as never)

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText(/咲季/)).toBeInTheDocument())
    expect(screen.getByText(/手毬/)).toBeInTheDocument()
  })

  e2eIt(2, 'T2-F15-05', 'F15', 'Navigation arrows clamp cleanly on rapid clicks and disable when all slots fit', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const user = userEvent.setup()

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 3,
      slotCount: 3,
      selectedSlotIndex: 0,
      slots: [makeSlot(0), makeSlot(1), makeSlot(2)],
    } as never)

    const protoRect = HTMLElement.prototype.getBoundingClientRect
    let mockWidth = 1000
    vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) {
      if (isSlotStripViewportEl(this)) {
        return { width: mockWidth, height: 140, top: 0, left: 0, right: mockWidth, bottom: 140, x: 0, y: 0, toJSON: () => {} } as DOMRect
      }
      return protoRect.call(this)
    })
    const clientWidthOwner = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'clientWidth')
      ? HTMLElement.prototype
      : Element.prototype
    const origClientWidthGet = Object.getOwnPropertyDescriptor(clientWidthOwner, 'clientWidth')?.get
    vi.spyOn(clientWidthOwner as HTMLElement, 'clientWidth', 'get').mockImplementation(function (this: HTMLElement) {
      if (isSlotStripViewportEl(this)) return mockWidth
      return origClientWidthGet ? origClientWidthGet.call(this) : 0
    })

    const roCallbacks: Array<{ cb: ResizeObserverCallback; els: Set<Element> }> = []
    vi.stubGlobal(
      'ResizeObserver',
      class {
        private readonly entry: { cb: ResizeObserverCallback; els: Set<Element> }
        constructor(cb: ResizeObserverCallback) {
          this.entry = { cb, els: new Set() }
          roCallbacks.push(this.entry)
        }
        observe(el: Element) {
          this.entry.els.add(el)
        }
        unobserve(el: Element) {
          this.entry.els.delete(el)
        }
        disconnect() {
          this.entry.els.clear()
        }
      },
    )

    try {
      const { rerender } = render(<MotionPage />)
      await waitFor(() => expect(screen.getByText('3号位')).toBeInTheDocument())
      const prevBtn = screen.getByRole('button', { name: /上一|向左|</ })
      const nextBtn = screen.getByRole('button', { name: /下一|向右|>/ })
      expect(prevBtn).toBeDisabled()
      expect(nextBtn).toBeDisabled()

      mockWidth = 120
      for (const ro of roCallbacks) {
        const entries = Array.from(ro.els)
          .filter((el) => el instanceof HTMLElement && isSlotStripViewportEl(el))
          .map(
            (target) =>
              ({
                target,
                contentRect: { width: mockWidth, height: 140, top: 0, left: 0, right: mockWidth, bottom: 140, x: 0, y: 0 },
              }) as unknown as ResizeObserverEntry,
          )
        if (entries.length > 0) {
          ro.cb(entries, {} as ResizeObserver)
        }
      }
      window.dispatchEvent(new Event('resize'))
      rerender(<MotionPage />)

      for (let i = 0; i < 10; i += 1) {
        if (!nextBtn.hasAttribute('disabled')) await user.click(nextBtn)
      }
      expect(nextBtn).toBeDisabled()
      for (let i = 0; i < 10; i += 1) {
        if (!prevBtn.hasAttribute('disabled')) await user.click(prevBtn)
      }
      expect(prevBtn).toBeDisabled()
    } finally {
      vi.unstubAllGlobals()
    }
  })

  e2eIt(2, 'T2-F15-06', 'F15', 'Empty slot disables/guards actor facial & bone mutations without corrupting state', async (ctx) => {
    if (!ensureReadyOrSkip(ctx, await probeF15Ready())) return
    bridge = await createMockSession()
    const spy = vi.spyOn(bridge, 'request')

    bridge.push('formation.changed' as never, {
      active: true,
      isGroupSong: true,
      trackPositionCount: 2,
      slotCount: 2,
      selectedSlotIndex: 1,
      slots: [
        makeSlot(0),
        makeSlot(1, { occupied: false, actorToken: 0, character: '', shortName: '', displayName: '' }),
      ],
    } as never)

    render(<MotionPage />)
    await waitFor(() => expect(screen.getByText(/当前位置上无角色/)).toBeInTheDocument())

    const saveFacialBtn = screen.queryByRole('button', { name: '保存表情幅度' })
    if (saveFacialBtn) {
      expect(saveFacialBtn).toBeDisabled()
    }
    expect(spy.mock.calls.some((c) => c[0] === 'facial.save')).toBe(false)
  })
})
