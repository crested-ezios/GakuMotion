import fs from 'node:fs'
import path from 'node:path'
import { cleanup, fireEvent, render, screen, within } from '@testing-library/react'
import type userEvent from '@testing-library/user-event'
import { expect, it, vi, type TestContext } from 'vitest'
import { getMotionBridge, setMotionBridge } from '@/motion/bridge'
import { MotionMockBridgeClient, type MockCapabilityProfile } from '@/motion/mock'
import {
  DEFAULT_CAMERA_SETTINGS,
  type MotionBridgeClient,
  type MotionEventName,
  type MotionEvents,
  type MotionMethodName,
  type MotionMethods,
} from '@/motion/protocol'
import { motionStore } from '@/motion/store'
import { LibraryPage } from '@/motion/pages/LibraryPage'
import { StagePage } from '@/motion/pages/StagePage'
import { MotionStatusBar } from '@/motion/components/MotionStatusBar'

export const REPO_ROOT = path.resolve(import.meta.dirname, '..', '..', '..')
export const FRONTEND_MOTION_DIR = path.join(REPO_ROOT, 'frontend', 'src', 'motion')

export type E2ETier = 1 | 2 | 3 | 4
export type E2EFeature = 'F13' | 'F14' | 'F15' | 'F16'

export interface ProbeResult {
  ready: boolean
  reason?: string
}

export function e2eIt(
  tier: E2ETier,
  id: string,
  feature: E2EFeature,
  name: string,
  fn: (ctx: TestContext) => Promise<void> | void,
) {
  return it(`${id}: ${name}`, async (ctx) => {
    const meta = ctx.task.meta as Record<string, unknown>
    meta.e2eId = id
    meta.e2eTier = tier
    meta.e2eFeature = feature
    meta.e2eName = name
    await fn(ctx)
  })
}

export function motionFileExists(relFromMotion: string): boolean {
  return fs.existsSync(path.join(FRONTEND_MOTION_DIR, relFromMotion))
}

export async function importOptionalMotionModule<T = Record<string, unknown>>(
  relNoExtFromMotion: string,
): Promise<T | null> {
  const hasTsx = motionFileExists(`${relNoExtFromMotion}.tsx`)
  const hasTs = motionFileExists(`${relNoExtFromMotion}.ts`)
  if (!hasTsx && !hasTs) return null
  const spec = `@/motion/${relNoExtFromMotion}`
  return (await import(/* @vite-ignore */ spec)) as T
}

export interface MockSessionOptions {
  capabilityProfile?: MockCapabilityProfile
}

export async function createMockSession(options: MockSessionOptions = {}): Promise<MotionMockBridgeClient> {
  const bridge = new MotionMockBridgeClient(
    options.capabilityProfile ? { capabilityProfile: options.capabilityProfile } : undefined,
  )
  setMotionBridge(bridge)
  motionStore.reset()
  await motionStore.start()
  return bridge
}

export function cleanupSession(bridge?: MotionBridgeClient | null): void {
  try {
    bridge?.close()
  } catch {
    // ignore
  }
  try {
    getMotionBridge()?.close()
  } catch {
    // ignore
  }
  motionStore.reset()
  setMotionBridge(null)
  cleanup()
  vi.restoreAllMocks()
}

export function hasFormationProtocol(capabilities: readonly string[]): boolean {
  return (
    capabilities.includes('formation.selectSlot') &&
    capabilities.includes('formation.swapSlots') &&
    capabilities.includes('formation.setSlotOffset') &&
    capabilities.includes('formation.saveSlotOffset') &&
    capabilities.includes('formation.setSlotSource')
  )
}

export function hasPortraitProtocol(capabilities: readonly string[]): boolean {
  return capabilities.includes('actors.getPortrait')
}

export function hasCameraFollowSlotIndexField(): boolean {
  return Object.prototype.hasOwnProperty.call(DEFAULT_CAMERA_SETTINGS, 'followSlotIndex')
}

export async function probeM4Contract(): Promise<ProbeResult> {
  const bridge = await createMockSession()
  try {
    const state = motionStore.getState() as unknown as Record<string, unknown>
    const caps = (state.capabilities as string[] | undefined) ?? []
    if (!hasFormationProtocol(caps) || caps.includes('ensemble.set')) {
      return {
        ready: false,
        reason: 'M4 protocol/mock not merged: missing formation.* RPCs or legacy ensemble.set still active',
      }
    }
    if (!hasPortraitProtocol(caps)) {
      return {
        ready: false,
        reason: 'M4 protocol/mock not merged: missing actors.getPortrait capability',
      }
    }
    if (!hasCameraFollowSlotIndexField()) {
      return {
        ready: false,
        reason: 'M4 protocol not merged: CameraSettings.followSlotIndex absent',
      }
    }
    if (!('formation' in state)) {
      return {
        ready: false,
        reason: 'M4 store not merged: motionStore state missing formation snapshot',
      }
    }
    return { ready: true }
  } finally {
    cleanupSession(bridge)
  }
}

export async function probeF14Ready(): Promise<ProbeResult> {
  const m4 = await probeM4Contract()
  if (!m4.ready) {
    return {
      ready: false,
      reason: `PENDING M4/M5 (F14): ${m4.reason}`,
    }
  }

  const bridge = await createMockSession()
  try {
    const view = render(<LibraryPage />)
    const text = view.container.textContent ?? ''
    const hasFilterBtn = view.queryByRole('button', { name: '轨道筛选' }) !== null
    await motionStore.openImportNew([])
    await new Promise((r) => setTimeout(r, 0))
    const bodyText = document.body.textContent ?? ''
    const hasPositionCards = bodyText.includes('1号位') || bodyText.includes('追加位置')
    const hasGroupFilter = text.includes('团曲') || (hasFilterBtn && 'group' in motionStore.getState())

    if (!hasPositionCards && !hasGroupFilter) {
      return {
        ready: false,
        reason: 'PENDING M5 (F14): LibraryPage 团曲 filter/badge & ImportDialog position cards not implemented',
      }
    }
    return { ready: true }
  } finally {
    cleanupSession(bridge)
  }
}

export async function probeF15Ready(): Promise<ProbeResult> {
  if (!motionFileExists('components/ActorSlotStrip.tsx')) {
    return {
      ready: false,
      reason: 'PENDING M5 (F15): frontend/src/motion/components/ActorSlotStrip.tsx not implemented',
    }
  }
  if (!motionFileExists('components/RotaryKnobCard.tsx')) {
    return {
      ready: false,
      reason: 'PENDING M5 (F15): frontend/src/motion/components/RotaryKnobCard.tsx not implemented',
    }
  }
  const m4 = await probeM4Contract()
  if (!m4.ready) {
    return {
      ready: false,
      reason: `PENDING M4 (F15): ${m4.reason}`,
    }
  }
  return { ready: true }
}

export async function probeF16Ready(): Promise<ProbeResult> {
  const m4 = await probeM4Contract()
  if (!m4.ready) {
    return {
      ready: false,
      reason: `PENDING M4/M5 (F16): ${m4.reason}`,
    }
  }
  const ensembleRemoved = !motionFileExists('components/EnsembleSection.tsx')
  const bridge = await createMockSession()
  try {
    const stageView = render(<StagePage />)
    const stageHasLegacy = (stageView.container.textContent ?? '').includes('舞台角色阵容')
    cleanup()
    const statusView = render(<MotionStatusBar />)
    const statusHasLegacy = /驱动[:：]/.test(statusView.container.textContent ?? '')
    if (!ensembleRemoved && stageHasLegacy && statusHasLegacy) {
      return {
        ready: false,
        reason:
          'PENDING M5 (F16): StagePage/MotionStatusBar cleanup and EnsembleSection.tsx deletion not yet implemented',
      }
    }
    return { ready: true }
  } finally {
    cleanupSession(bridge)
  }
}

export interface CppBridgeFixture {
  m4Ready: boolean
  reason?: string
  responses?: Record<string, Record<string, unknown>>
  events?: Array<{
    v: number
    type: 'event'
    name: MotionEventName
    payload: unknown
    seq: number
    sid: number
  }>
}

export function loadCppBridgeFixture(): CppBridgeFixture {
  const fixturePath = process.env.GKM_E2E_BRIDGE_JSON
  if (!fixturePath || !fs.existsSync(fixturePath)) {
    return { m4Ready: false, reason: 'bridge_wire_fixtures.json not generated' }
  }
  try {
    const raw = fs.readFileSync(fixturePath, 'utf8').replace(/^\uFEFF/, '')
    return JSON.parse(raw) as CppBridgeFixture
  } catch (err) {
    return { m4Ready: false, reason: `Failed to parse bridge_wire_fixtures.json: ${String(err)}` }
  }
}

export async function probeCrossLayerReady(opts: {
  requireF14?: boolean
  requireF15?: boolean
  requireF16?: boolean
  requireCpp?: boolean
}): Promise<ProbeResult> {
  if (opts.requireCpp) {
    const cpp = loadCppBridgeFixture()
    if (!cpp.m4Ready) {
      return {
        ready: false,
        reason: `PENDING C++ M4: ${cpp.reason || 'GkWebPanelBridge M4 contract not merged'}`,
      }
    }
  }
  if (opts.requireF14) {
    const p = await probeF14Ready()
    if (!p.ready) return p
  }
  if (opts.requireF15) {
    const p = await probeF15Ready()
    if (!p.ready) return p
  }
  if (opts.requireF16) {
    const p = await probeF16Ready()
    if (!p.ready) return p
  }
  return { ready: true }
}

export function ensureReadyOrSkip(ctx: TestContext, probe: ProbeResult): boolean {
  if (!probe.ready) {
    ctx.skip(`PENDING_IMPL: ${probe.reason || 'Not implemented yet'}`)
    return false
  }
  return true
}

/**
 * Replays real C++ GkWebPanelBridge JSON wire responses and event envelopes
 * captured by frontend_bridge_probe.cpp into MotionStore, while delegating
 * any unscripted follow-up requests to an underlying mock fallback.
 */
export class RealWireReplayBridge implements MotionBridgeClient {
  readonly kind = 'webview2' as const
  private readonly handlers = new Map<MotionEventName, Set<(payload: never) => void>>()
  private readonly fixture: CppBridgeFixture
  private readonly fallback: MotionMockBridgeClient
  private closed = false

  constructor(fixture: CppBridgeFixture) {
    this.fixture = fixture
    this.fallback = new MotionMockBridgeClient()
  }

  async request<M extends MotionMethodName>(
    method: M,
    params?: MotionMethods[M]['params'],
  ): Promise<MotionMethods[M]['result']> {
    if (this.closed) {
      throw new Error('Bridge closed')
    }
    const resp = this.fixture.responses?.[method]
    if (resp && resp.ok === true && 'result' in resp) {
      return resp.result as MotionMethods[M]['result']
    }
    return await this.fallback.request(method, params)
  }

  on<E extends MotionEventName>(event: E, handler: (payload: MotionEvents[E]) => void): () => void {
    let set = this.handlers.get(event)
    if (!set) {
      set = new Set()
      this.handlers.set(event, set)
    }
    set.add(handler as (payload: never) => void)
    const unsubFallback = this.fallback.on(event, handler)
    return () => {
      set?.delete(handler as (payload: never) => void)
      unsubFallback()
    }
  }

  emitWireEvent(name: MotionEventName, payload: unknown): void {
    const set = this.handlers.get(name)
    if (!set) return
    for (const fn of set) {
      fn(payload as never)
    }
  }

  replayCapturedEvents(): void {
    for (const evt of this.fixture.events ?? []) {
      if (evt && evt.type === 'event' && evt.name) {
        this.emitWireEvent(evt.name, evt.payload)
      }
    }
  }

  close(): void {
    this.closed = true
    this.handlers.clear()
    this.fallback.close()
  }
}

export const SAMPLE_PORTRAIT_DATA_URI =
  'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR42mP8z8BQz0AEYBxVAAAwBAJ/7xE7bAAAAABJRU5ErkJggg=='

export async function waitForCondition(predicate: () => boolean, timeoutMs = 2000): Promise<void> {
  const start = Date.now()
  while (!predicate()) {
    if (Date.now() - start > timeoutMs) {
      throw new Error('Timed out waiting for condition')
    }
    await new Promise((r) => setTimeout(r, 15))
  }
}

export function assertNoUnexpectedLegacyText(root: HTMLElement): void {
  const text = root.textContent ?? ''
  for (const legacy of [
    '多人舞台激活',
    '摄影槽位验证探针',
    '舞台角色阵容',
    '当前驱动角色',
    '多人舞台激活时禁用上下曲',
  ]) {
    expect(text).not.toContain(legacy)
  }
}

export interface UnwrappedImportPosition {
  slot?: number
  motionFile?: string
  motionVmd?: string
  facialFile?: string
  morphVmd?: string
  x?: number
  y?: number
  z?: number
  yawDegrees?: number
  preserveExisting?: boolean
}

export interface UnwrappedImportRequest {
  editKey?: string
  isGroup?: boolean
  rebakeOutput?: boolean
  motionFile?: string
  positions?: UnwrappedImportPosition[]
  [key: string]: unknown
}

export function unwrapImportRequest(params: unknown): UnwrappedImportRequest {
  const raw = (params ?? {}) as { request?: UnwrappedImportRequest } & UnwrappedImportRequest
  return (raw.request ?? raw) as UnwrappedImportRequest
}

export interface UnwrappedFacialWeights {
  overall?: number
  brow?: number
  eye?: number
  mouth?: number
}

export function unwrapFacialWeights(params: unknown): UnwrappedFacialWeights {
  const raw = (params ?? {}) as { weights?: UnwrappedFacialWeights } & UnwrappedFacialWeights
  return (raw.weights ?? raw) as UnwrappedFacialWeights
}

export function assertBadgeBeforeAuthor(
  container: HTMLElement,
  badgeEl: HTMLElement,
  authorEl: HTMLElement,
): void {
  expect(container.contains(badgeEl)).toBe(true)
  expect(container.contains(authorEl)).toBe(true)

  if (authorEl.contains(badgeEl)) {
    const walker = document.createTreeWalker(authorEl, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT)
    let sawBadge = false
    let badgeBeforeAuthorText = false
    let node: Node | null = walker.nextNode()
    while (node) {
      if (node === badgeEl) {
        sawBadge = true
      } else if (node.nodeType === Node.TEXT_NODE && !badgeEl.contains(node)) {
        const text = (node.textContent ?? '').trim()
        if (text.length > 0 && !/^[·•|/,\-:\s]+$/.test(text)) {
          badgeBeforeAuthorText = sawBadge
          break
        }
      }
      node = walker.nextNode()
    }
    expect(badgeBeforeAuthorText).toBe(true)
  } else {
    expect(badgeEl.compareDocumentPosition(authorEl) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0)
  }
}

export function findNumericInputs(container: HTMLElement): HTMLInputElement[] {
  const spins = within(container).queryAllByRole('spinbutton') as HTMLInputElement[]
  if (spins.length > 0) return spins
  const boxes = within(container).queryAllByRole('textbox') as HTMLElement[]
  return boxes.filter((el): el is HTMLInputElement => el.tagName === 'INPUT')
}

export function findNumericInputByName(name: RegExp | string, container?: HTMLElement): HTMLInputElement {
  const scope = container ? within(container) : screen
  const spin = scope.queryByRole('spinbutton', { name })
  if (spin && spin.tagName === 'INPUT') return spin as HTMLInputElement
  const box = scope.queryByRole('textbox', { name })
  if (box && box.tagName === 'INPUT') return box as HTMLInputElement

  // Fallback: locate the card/label matching `name` and find the numeric input inside it
  const labelEl = scope.getByText(name)
  let cur: HTMLElement | null = labelEl
  while (cur && cur !== (container ?? document.body)) {
    const candidates = [
      ...(within(cur).queryAllByRole('spinbutton') as HTMLElement[]),
      ...(within(cur).queryAllByRole('textbox') as HTMLElement[]),
    ].filter((el): el is HTMLInputElement => el.tagName === 'INPUT')
    if (candidates.length === 1) return candidates[0]
    if (candidates.length > 1) {
      const exact = candidates.find((el) => {
        const label = el.getAttribute('aria-label') ?? ''
        return typeof name === 'string' ? label.includes(name) : name.test(label)
      })
      if (exact) return exact
      return candidates[0]
    }
    cur = cur.parentElement
  }
  throw new Error(`Unable to find numeric input for ${String(name)}`)
}

export function parseNumericInputValue(input: HTMLInputElement): number {
  const cleaned = input.value.replace(/[^0-9eE+\-.]/g, '')
  return Number.parseFloat(cleaned)
}

export async function setNumericInputValue(
  user: ReturnType<typeof userEvent.setup>,
  input: HTMLInputElement,
  nextValue: string,
): Promise<void> {
  await user.click(input)
  if (input.readOnly) {
    await user.keyboard('{Enter}')
  }
  if (input.readOnly) {
    await user.dblClick(input)
  }
  await user.clear(input)
  await user.type(input, `${nextValue}{Enter}`)
  if (document.activeElement === input) {
    fireEvent.blur(input)
  }
}

export function isSlotStripViewportEl(el: HTMLElement): boolean {
  const testId = el.dataset?.testid ?? ''
  if (testId.includes('slot-strip') || testId.includes('actor-strip')) return true
  if (el.getAttribute('role') === 'listbox') return true
  const text = el.textContent ?? ''
  if (
    text.includes('1号位') &&
    text.includes('2号位') &&
    !text.includes('高级动作设置') &&
    !text.includes('保存表情幅度') &&
    (el.className.includes('overflow') || el.style.overflowX !== '' || el.style.overflow !== '')
  ) {
    return true
  }
  return false
}

export function findDashedSlotCard(labelEl: HTMLElement): HTMLElement | null {
  let cur: HTMLElement | null = labelEl
  while (cur && cur !== document.body) {
    const cls = typeof cur.className === 'string' ? cur.className : ''
    const borderStyle = cur.style?.borderStyle ?? ''
    if (/dashed/i.test(cls) || /dashed/i.test(borderStyle)) {
      return cur
    }
    const text = cur.textContent ?? ''
    if (text.includes('1号位') && text.includes('2号位')) {
      break
    }
    cur = cur.parentElement
  }
  return null
}

export function findSlotCardElement(labelEl: HTMLElement): HTMLElement {
  let cur: HTMLElement | null = labelEl
  while (cur && cur !== document.body) {
    const text = cur.textContent ?? ''
    if (cur !== labelEl && text.includes('1号位') && text.includes('2号位')) {
      break
    }
    const testId = cur.dataset?.testid ?? ''
    if (
      cur.getAttribute('draggable') === 'true' ||
      cur.hasAttribute('data-slot-index') ||
      testId.includes('slot') ||
      cur.tagName === 'BUTTON' ||
      cur.getAttribute('role') === 'option'
    ) {
      return cur
    }
    cur = cur.parentElement
  }
  return labelEl.parentElement ?? labelEl
}

export function pointerDragSlotCard(source: HTMLElement, target: HTMLElement): void {
  fireEvent.pointerDown(source, { button: 0, clientX: 20, clientY: 20 })
  fireEvent.pointerMove(target, { clientX: 200, clientY: 20 })
  fireEvent.pointerUp(target, { button: 0, clientX: 200, clientY: 20 })
}

export interface FormationSlotShape {
  slotIndex: number
  occupied: boolean
  actorToken: number
  character: string
  shortName: string
  displayName: string
  isOverflow: boolean
  hasTrackMotion: boolean
  sourceSlotIndex: number
  offset: { x: number; y: number; z: number; yawDegrees: number }
  placementDirty: boolean
  facialWeights: { overall: number; brow: number; eye: number; mouth: number }
  hasActorBoneOverrides: boolean
}

export interface FormationSnapshotShape {
  active: boolean
  isGroupSong: boolean
  trackPositionCount: number
  slotCount: number
  selectedSlotIndex: number
  slots: FormationSlotShape[]
}

export function getStoreFormation(): FormationSnapshotShape {
  return (motionStore.getState() as unknown as { formation: FormationSnapshotShape }).formation
}
