import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { afterAll, afterEach, describe, expect, it, vi } from 'vitest'
import { MotionWebView2Client } from '@/motion/bridge'
import { MotionMockBridgeClient, type MockCapabilityProfile } from '@/motion/mock'
import { BridgeError } from '@/motion/protocol'
import { MotionStore } from '@/motion/store'
import { classifyImportFiles } from '@/motion/validation'
import { flushTsE2EReport, runProbeCase } from './ts_e2e_report'

const here = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(here, '../../..')
const protocolTsPath = path.join(repoRoot, 'frontend', 'src', 'motion', 'protocol.ts')

const tierFilter = (process.env.GKE2E_TIER_FILTER ?? 'All').trim()
function shouldRegisterTier(tier: number): boolean {
  return tierFilter === 'All' || tierFilter === 'all' || Number(tierFilter) === tier
}

type Listener = (event: { data: unknown }) => void

class FakeWebview {
  posted: Record<string, unknown>[] = []
  private readonly listeners = new Set<Listener>()
  postMessage(message: unknown) {
    this.posted.push(message as Record<string, unknown>)
  }
  addEventListener(_type: 'message', listener: Listener) {
    this.listeners.add(listener)
  }
  removeEventListener(_type: 'message', listener: Listener) {
    this.listeners.delete(listener)
  }
  emit(data: unknown) {
    for (const listener of this.listeners) listener({ data })
  }
}

async function createStartedStore(capabilityProfile: MockCapabilityProfile = 'full') {
  const bridge = new MotionMockBridgeClient({ capabilityProfile })
  const store = new MotionStore(bridge)
  await store.start()
  return { bridge, store }
}

function extractTypeBlock(src: string, typeName: string): string | null {
  const re = new RegExp(`(?:export\\s+)?(?:interface|type)\\s+${typeName}\\b[^{]*\\{`, 'm')
  const match = re.exec(src)
  if (!match) return null
  const openIdx = match.index + match[0].length - 1
  let depth = 0
  for (let i = openIdx; i < src.length; i++) {
    const ch = src[i]
    if (ch === '{') depth++
    else if (ch === '}') {
      depth--
      if (depth === 0) {
        return src.slice(openIdx, i + 1)
      }
    }
  }
  return null
}

function probeProtocolTsM4(): string[] {
  const src = fs.readFileSync(protocolTsPath, 'utf8')
  const missing: string[] = []

  const checkBlock = (typeName: string, patterns: Array<[string, RegExp]>) => {
    const block = extractTypeBlock(src, typeName)
    if (!block) {
      missing.push(`protocol.ts:${typeName}(missing block)`)
      return
    }
    for (const [label, rx] of patterns) {
      if (!rx.test(block)) {
        missing.push(`protocol.ts:${typeName}.${label}`)
      }
    }
  }

  checkBlock('LibraryEntry', [
    ['positions', /\bpositions\b\s*\??:/],
    ['isGroup', /\bisGroup\b\s*\??:/],
    ['positionCount', /\bpositionCount\b\s*\??:/],
  ])
  checkBlock('SceneActor', [
    ['character', /\bcharacter\b\s*\??:/],
    ['shortName', /\bshortName\b\s*\??:/],
  ])
  checkBlock('CameraSettings', [['followSlotIndex', /\bfollowSlotIndex\b\s*\??:/]])
  checkBlock('MotionStateSnapshot', [['formation', /\bformation\b\s*\??:/]])

  const methodsBlock = extractTypeBlock(src, 'MotionMethods')
  if (!methodsBlock) {
    missing.push('protocol.ts:MotionMethods(missing block)')
  } else {
    for (const m of [
      'actors.getPortrait',
      'formation.selectSlot',
      'formation.swapSlots',
      'formation.setSlotOffset',
      'formation.saveSlotOffset',
      'formation.setSlotSource',
    ]) {
      if (!methodsBlock.includes(`'${m}'`) && !methodsBlock.includes(`"${m}"`)) {
        missing.push(`protocol.ts:MotionMethods.${m}`)
      }
    }
  }

  const eventsBlock = extractTypeBlock(src, 'MotionEvents')
  if (!eventsBlock) {
    missing.push('protocol.ts:MotionEvents(missing block)')
  } else if (!eventsBlock.includes("'formation.changed'") && !eventsBlock.includes('"formation.changed"')) {
    missing.push('protocol.ts:MotionEvents.formation.changed')
  }

  return missing
}

function extractSortedMotionPaths(cls: Record<string, unknown>): string[] | null {
  if (Array.isArray(cls.motionCandidates) && cls.motionCandidates.every((x) => typeof x === 'string')) {
    return cls.motionCandidates as string[]
  }
  if (Array.isArray(cls.positions)) {
    const out: string[] = []
    for (const item of cls.positions) {
      if (typeof item === 'string') {
        out.push(item)
      } else if (item && typeof item === 'object') {
        const rec = item as Record<string, unknown>
        const candidate = rec.motionFile ?? rec.motionCandidate ?? rec.motionPath ?? rec.path
        if (typeof candidate !== 'string') return null
        out.push(candidate)
      } else {
        return null
      }
    }
    return out
  }
  return null
}

function resolveStoreMethod(
  store: MotionStore,
  candidates: string[],
): ((...args: unknown[]) => Promise<unknown>) | null {
  const storeAny = store as unknown as Record<string, unknown>
  for (const name of candidates) {
    const fn = storeAny[name]
    if (typeof fn === 'function') {
      return (fn as (...args: unknown[]) => Promise<unknown>).bind(store)
    }
  }
  return null
}

async function invokeSetSlotOffset(
  fn: (...args: unknown[]) => Promise<unknown>,
  slotIndex: number,
  offset: { x: number; y: number; z: number; yawDegrees: number },
): Promise<unknown> {
  if (fn.length >= 5) {
    return fn(slotIndex, offset.x, offset.y, offset.z, offset.yawDegrees)
  }
  if (fn.length === 2) {
    return fn(slotIndex, offset)
  }
  return fn({ slotIndex, ...offset })
}

async function probeMockAndStoreM4(): Promise<string | null> {
  const missingProto = probeProtocolTsM4()
  if (missingProto.length > 0) {
    return `M4 pending in protocol.ts (${missingProto.join(', ')})`
  }
  const { bridge, store } = await createStartedStore('full')
  try {
    const hello = (await bridge.request('motion.hello')) as unknown as Record<string, unknown>
    const methods = Array.isArray(hello.supportedMethods) ? (hello.supportedMethods as string[]) : []
    const events = Array.isArray(hello.supportedEvents) ? (hello.supportedEvents as string[]) : []
    const requiredMethods = [
      'actors.getPortrait',
      'formation.selectSlot',
      'formation.swapSlots',
      'formation.setSlotOffset',
      'formation.saveSlotOffset',
      'formation.setSlotSource',
    ]
    const missingMethods = requiredMethods.filter((m) => !methods.includes(m))
    if (missingMethods.length > 0) {
      return `M4 pending in MotionMockBridgeClient.supportedMethods (missing: ${missingMethods.join(', ')})`
    }
    if (!events.includes('formation.changed')) {
      return 'M4 pending in MotionMockBridgeClient.supportedEvents (missing: formation.changed)'
    }
    const snapshot = (await bridge.request('state.get')) as unknown as Record<string, unknown>
    if (!('formation' in snapshot)) {
      return 'M4 pending in state.get snapshot (missing: formation)'
    }
    const cam = (snapshot.camera ?? {}) as Record<string, unknown>
    const camSettings = (cam.settings ?? {}) as Record<string, unknown>
    if (!('followSlotIndex' in camSettings)) {
      return 'M4 pending in CameraSettings (missing: followSlotIndex)'
    }
    const actorsState = (snapshot.actors ?? {}) as Record<string, unknown>
    const actors = Array.isArray(actorsState.actors) ? (actorsState.actors as Record<string, unknown>[]) : []
    if (actors.length === 0 || !('character' in actors[0]) || !('shortName' in actors[0])) {
      return 'M4 pending in SceneActor (missing: character/shortName)'
    }
    const libState = (snapshot.library ?? {}) as Record<string, unknown>
    const entries = Array.isArray(libState.entries) ? (libState.entries as Record<string, unknown>[]) : []
    if (
      entries.length === 0 ||
      !('isGroup' in entries[0]) ||
      !('positionCount' in entries[0]) ||
      !('positions' in entries[0]) ||
      !Array.isArray(entries[0].positions)
    ) {
      return 'M4 pending in LibraryEntry (missing: isGroup/positionCount/positions)'
    }
    if (!store.getState().connected) {
      return 'Store failed to connect during M4 probe'
    }
    return null
  } finally {
    bridge.close()
  }
}

async function probeMultiVmdClassification(): Promise<string | null> {
  const m4Reason = await probeMockAndStoreM4()
  if (m4Reason !== null) return m4Reason

  const samplePaths = ['b_slot2.vmd', 'a_slot1.vmd', 'cam.vmd', 'song.wav']
  const sampleProbes = [
    { path: 'b_slot2.vmd', valid: true, isGkCamera: false, tendency: 'motionOnly', boneFrames: 120, bodyBoneFrames: 120, eyeBoneFrames: 0, morphFrames: 0, cameraFrames: 0 },
    { path: 'a_slot1.vmd', valid: true, isGkCamera: false, tendency: 'motionAndFacial', boneFrames: 120, bodyBoneFrames: 120, eyeBoneFrames: 0, morphFrames: 30, cameraFrames: 0 },
    { path: 'cam.vmd', valid: true, isGkCamera: false, tendency: 'cameraOnly', boneFrames: 0, bodyBoneFrames: 0, eyeBoneFrames: 0, morphFrames: 0, cameraFrames: 60 },
  ]
  const classified = classifyImportFiles(samplePaths, sampleProbes as never) as unknown as Record<string, unknown>
  const motions = extractSortedMotionPaths(classified)
  if (!motions || motions.length < 2) {
    return 'M4 pending in validation.ts classifyImportFiles (missing multi-VMD motionCandidates/positions)'
  }
  return null
}

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome
  vi.restoreAllMocks()
})

afterAll(() => {
  flushTsE2EReport()
})

describe('E2E-3 F13 TypeScript Bridge/Store/Protocol Contracts', () => {
  if (shouldRegisterTier(1)) {
    it('[T1-F13-06] protocol.ts & MotionStore/Mock M4 normalization', async (ctx) => {
      await runProbeCase(
        'T1-F13-06',
        1,
        'F13',
        'protocol.ts & MotionStore/Mock M4 normalization',
        ctx,
        probeMockAndStoreM4,
        async () => {
          const { bridge, store } = await createStartedStore('full')
          try {
            const state = store.getState() as unknown as Record<string, unknown>
            expect(state.ready).toBe(true)
            expect(state.connected).toBe(true)

            const hello = (await bridge.request('motion.hello')) as unknown as {
              supportedMethods: string[]
              supportedEvents: string[]
            }
            expect(hello.supportedMethods).toContain('actors.getPortrait')
            expect(hello.supportedMethods).toContain('formation.selectSlot')
            expect(hello.supportedMethods).toContain('formation.swapSlots')
            expect(hello.supportedMethods).toContain('formation.setSlotOffset')
            expect(hello.supportedMethods).toContain('formation.saveSlotOffset')
            expect(hello.supportedMethods).toContain('formation.setSlotSource')
            expect(hello.supportedMethods).not.toContain('ensemble.set')
            expect(hello.supportedMethods).not.toContain('ensemble.probe')
            expect(hello.supportedMethods).not.toContain('ensemble.presetSave')
            expect(hello.supportedMethods).not.toContain('ensemble.presetLoad')
            expect(hello.supportedEvents).toContain('formation.changed')
            expect(hello.supportedEvents).not.toContain('ensemble.changed')

            const actorsState = state.actors as { actors: Array<Record<string, unknown>> }
            expect(actorsState.actors.length).toBeGreaterThan(0)
            for (const actor of actorsState.actors) {
              expect(typeof actor.character).toBe('string')
              expect(typeof actor.shortName).toBe('string')
            }

            const libState = state.library as { entries: Array<Record<string, unknown>> }
            expect(libState.entries.length).toBeGreaterThan(0)
            for (const entry of libState.entries) {
              expect(typeof entry.isGroup).toBe('boolean')
              expect(typeof entry.positionCount).toBe('number')
              expect(Array.isArray(entry.positions)).toBe(true)
              const positions = entry.positions as Array<Record<string, unknown>>
              for (const p of positions) {
                expect(typeof p.slot).toBe('number')
                expect(Number.isFinite(p.x as number)).toBe(true)
                expect(Number.isFinite(p.y as number)).toBe(true)
                expect(Number.isFinite(p.z as number)).toBe(true)
                expect(typeof p.yawDegrees === 'number' || typeof p.yaw_degrees === 'number').toBe(true)
              }
            }

            const cam = state.camera as { settings: Record<string, unknown> }
            expect(typeof cam.settings.followSlotIndex).toBe('number')

            const fm = state.formation as Record<string, unknown>
            expect(fm).toBeDefined()
            expect(typeof fm.active).toBe('boolean')
            expect(typeof fm.isGroupSong).toBe('boolean')
            expect(typeof fm.trackPositionCount).toBe('number')
            expect(typeof fm.slotCount).toBe('number')
            expect(typeof fm.selectedSlotIndex).toBe('number')
            const slots = fm.slots as Array<Record<string, unknown>>
            expect(Array.isArray(slots)).toBe(true)
            expect(slots.length).toBeGreaterThanOrEqual(1)
            const s0 = slots[0]
            expect(s0.slotIndex).toBe(0)
            expect(typeof s0.occupied).toBe('boolean')
            expect(typeof s0.actorToken).toBe('number')
            expect(typeof s0.character).toBe('string')
            expect(typeof s0.shortName).toBe('string')
            expect(typeof s0.displayName).toBe('string')
            expect(typeof s0.isOverflow).toBe('boolean')
            expect(typeof s0.hasTrackMotion).toBe('boolean')
            expect(typeof s0.sourceSlotIndex).toBe('number')
            expect(typeof s0.placementDirty).toBe('boolean')
            expect(typeof s0.hasActorBoneOverrides).toBe('boolean')
            const off0 = s0.offset as Record<string, unknown>
            expect(typeof off0.x).toBe('number')
            expect(typeof off0.y).toBe('number')
            expect(typeof off0.z).toBe('number')
            expect(typeof off0.yawDegrees).toBe('number')
            const fw0 = s0.facialWeights as Record<string, unknown>
            expect(typeof fw0.overall).toBe('number')
            expect(typeof fw0.brow).toBe('number')
            expect(typeof fw0.eye).toBe('number')
            expect(typeof fw0.mouth).toBe('number')

            const rawSnap = (await bridge.request('state.get')) as unknown as Record<string, unknown>
            expect('formation' in rawSnap).toBe(true)
            expect(Array.isArray((rawSnap.library as { entries: unknown[] }).entries)).toBe(true)
            return 'M4 protocol.ts, motion.hello, and state.get snapshot normalized cleanly.'
          } finally {
            bridge.close()
          }
        },
      )
    })

    it('[T1-F13-07] MotionStore actors.getPortrait caching/dedup & formation mutations', async (ctx) => {
      await runProbeCase(
        'T1-F13-07',
        1,
        'F13',
        'MotionStore actors.getPortrait caching/dedup & formation mutations',
        ctx,
        probeMockAndStoreM4,
        async () => {
          const bridge = new MotionMockBridgeClient({ capabilityProfile: 'full' })
          const reqSpy = vi.spyOn(bridge, 'request')
          const store = new MotionStore(bridge)
          await store.start()
          try {
            const portraitFn = resolveStoreMethod(store, [
              'ensureActorPortrait',
              'fetchActorPortrait',
              'getActorPortrait',
              'loadActorPortrait',
            ])
            expect(portraitFn).not.toBeNull()

            await portraitFn!('hski')
            await portraitFn!('hski')
            await Promise.all([portraitFn!('ttmr'), portraitFn!('ttmr')])

            const hskiCalls = reqSpy.mock.calls.filter(
              ([m, p]) => m === 'actors.getPortrait' && (p as { character?: string } | undefined)?.character === 'hski',
            )
            const ttmrCalls = reqSpy.mock.calls.filter(
              ([m, p]) => m === 'actors.getPortrait' && (p as { character?: string } | undefined)?.character === 'ttmr',
            )
            expect(hskiCalls.length).toBe(1)
            expect(ttmrCalls.length).toBe(1)

            const stPortrait = store.getState() as unknown as Record<string, unknown>
            const pCache = (stPortrait.portraitCache ?? stPortrait.portraits ?? {}) as Record<string, unknown>
            expect(typeof pCache.hski === 'string' && (pCache.hski as string).startsWith('data:image/')).toBe(true)
            expect(typeof pCache.ttmr === 'string' && (pCache.ttmr as string).startsWith('data:image/')).toBe(true)

            const selectSlotFn = resolveStoreMethod(store, ['selectFormationSlot', 'selectSlot'])
            const setOffsetFn = resolveStoreMethod(store, [
              'commitFormationSlotOffset',
              'setFormationSlotOffset',
              'setSlotOffset',
            ])
            const saveOffsetFn = resolveStoreMethod(store, ['saveFormationSlotOffset', 'saveSlotOffset'])
            expect(selectSlotFn).not.toBeNull()
            expect(setOffsetFn).not.toBeNull()
            expect(saveOffsetFn).not.toBeNull()

            const formationEvents: unknown[] = []
            bridge.on('formation.changed' as never, ((payload: unknown) => {
              formationEvents.push(payload)
            }) as never)

            reqSpy.mockClear()
            await selectSlotFn!(1)
            expect(
              reqSpy.mock.calls.some(
                ([m, p]) => m === 'formation.selectSlot' && (p as { slotIndex?: number } | undefined)?.slotIndex === 1,
              ),
            ).toBe(true)
            const afterSelect = store.getState() as unknown as {
              formation: { selectedSlotIndex: number }
            }
            expect(afterSelect.formation.selectedSlotIndex).toBe(1)
            expect(formationEvents.length).toBeGreaterThanOrEqual(1)

            await invokeSetSlotOffset(setOffsetFn!, 1, { x: 0.5, y: 0, z: -0.25, yawDegrees: 15 })
            if (!reqSpy.mock.calls.some(([m]) => m === 'formation.setSlotOffset')) {
              await new Promise((r) => setTimeout(r, 150))
            }
            expect(reqSpy.mock.calls.some(([m]) => m === 'formation.setSlotOffset')).toBe(true)
            const afterSet = store.getState() as unknown as {
              formation: {
                slots: Array<{ offset: { x: number; z: number; yawDegrees: number }; placementDirty: boolean }>
              }
            }
            expect(afterSet.formation.slots[1].offset.x).toBeCloseTo(0.5)
            expect(afterSet.formation.slots[1].offset.z).toBeCloseTo(-0.25)
            expect(afterSet.formation.slots[1].offset.yawDegrees).toBeCloseTo(15)
            expect(afterSet.formation.slots[1].placementDirty).toBe(true)

            await saveOffsetFn!(1)
            expect(
              reqSpy.mock.calls.some(
                ([m, p]) => m === 'formation.saveSlotOffset' && (p as { slotIndex?: number } | undefined)?.slotIndex === 1,
              ),
            ).toBe(true)
            const afterSave = store.getState() as unknown as {
              formation: {
                slots: Array<{ offset: { x: number; yawDegrees: number }; placementDirty: boolean }>
              }
            }
            expect(afterSave.formation.slots[1].placementDirty).toBe(false)

            const snap = (await bridge.request('state.get')) as unknown as {
              formation: {
                selectedSlotIndex: number
                slots: Array<{ offset: { x: number; yawDegrees: number }; placementDirty: boolean }>
              }
            }
            expect(snap.formation.selectedSlotIndex).toBe(1)
            expect(snap.formation.slots[1].offset.x).toBeCloseTo(0.5)
            expect(snap.formation.slots[1].offset.yawDegrees).toBeCloseTo(15)
            expect(snap.formation.slots[1].placementDirty).toBe(false)
            return 'actors.getPortrait caching/dedup and MotionStore formation mutations verified.'
          } finally {
            bridge.close()
          }
        },
      )
    })
  }

  if (shouldRegisterTier(2)) {
    it('[T2-F13-06] Mock phase1 degradation & MotionWebView2Client wire correlation / stale-sid / invalid swap', async (ctx) => {
      await runProbeCase(
        'T2-F13-06',
        2,
        'F13',
        'Mock phase1 degradation & MotionWebView2Client wire correlation / stale-sid / invalid swap',
        ctx,
        probeMockAndStoreM4,
        async () => {
          const phase1 = await createStartedStore('phase1')
          try {
            expect(phase1.store.supports('actors.getPortrait' as never)).toBe(false)
            expect(phase1.store.supports('formation.selectSlot' as never)).toBe(false)
            await expect(
              (phase1.bridge as unknown as { request: (m: string, p: unknown) => Promise<unknown> }).request(
                'actors.getPortrait',
                { character: 'hski' },
              ),
            ).rejects.toMatchObject({ code: 'not_implemented' })
          } finally {
            phase1.bridge.close()
          }

          const full = await createStartedStore('full')
          try {
            await expect(
              (full.bridge as unknown as { request: (m: string, p: unknown) => Promise<unknown> }).request(
                'formation.swapSlots',
                { fromSlotIndex: 0, toSlotIndex: 999 },
              ),
            ).rejects.toMatchObject({ code: 'invalid_params' })
          } finally {
            full.bridge.close()
          }

          const fake = new FakeWebview()
          ;(window as unknown as { chrome: { webview: FakeWebview } }).chrome = { webview: fake }
          const wv = new MotionWebView2Client()
          try {
            const helloP = wv.request('bridge.hello')
            fake.emit({ v: 1, type: 'response', id: fake.posted[0].id, ok: true, result: { protocol: 1, sessionId: 10 } })
            await helloP

            const seenFormation: number[] = []
            wv.on('formation.changed' as never, ((payload: { selectedSlotIndex: number }) => {
              seenFormation.push(payload.selectedSlotIndex)
            }) as never)
            fake.emit({ v: 1, type: 'event', name: 'formation.changed', payload: { selectedSlotIndex: 2 }, seq: 1, sid: 9 })
            expect(seenFormation).toEqual([])
            fake.emit({ v: 1, type: 'event', name: 'formation.changed', payload: { selectedSlotIndex: 2 }, seq: 2, sid: 10 })
            expect(seenFormation).toEqual([2])
          } finally {
            wv.close()
          }
          return 'Phase1 degradation, invalid swap error, and WebView2 sid filtering verified.'
        },
      )
    })
  }

  if (shouldRegisterTier(3)) {
    it('[T3-F13-01] Pairwise Store x Mock x Validation: multi-VMD probeFiles -> formation edits -> WebView2 parity', async (ctx) => {
      await runProbeCase(
        'T3-F13-01',
        3,
        'F13',
        'Pairwise Store x Mock x Validation multi-VMD probeFiles & formation edits',
        ctx,
        probeMultiVmdClassification,
        async () => {
          const paths = ['03_miku.vmd', '01_hski.vmd', '02_ttmr.vmd', 'cam.vmd', 'song.wav']
          const probes = [
            { path: '03_miku.vmd', valid: true, isGkCamera: false, tendency: 'motionOnly', boneFrames: 120, bodyBoneFrames: 120, eyeBoneFrames: 0, morphFrames: 0, cameraFrames: 0 },
            { path: '01_hski.vmd', valid: true, isGkCamera: false, tendency: 'motionAndFacial', boneFrames: 120, bodyBoneFrames: 120, eyeBoneFrames: 0, morphFrames: 20, cameraFrames: 0 },
            { path: '02_ttmr.vmd', valid: true, isGkCamera: false, tendency: 'motionOnly', boneFrames: 120, bodyBoneFrames: 120, eyeBoneFrames: 0, morphFrames: 0, cameraFrames: 0 },
            { path: 'cam.vmd', valid: true, isGkCamera: false, tendency: 'cameraOnly', boneFrames: 0, bodyBoneFrames: 0, eyeBoneFrames: 0, morphFrames: 0, cameraFrames: 60 },
          ]
          const cls = classifyImportFiles(paths, probes as never) as unknown as Record<string, unknown>
          expect(cls.cameraCandidate).toBe('cam.vmd')
          expect(cls.audioCandidate).toBe('song.wav')
          expect(extractSortedMotionPaths(cls)).toEqual(['01_hski.vmd', '02_ttmr.vmd', '03_miku.vmd'])

          const { bridge, store } = await createStartedStore('full')
          try {
            const probeRes = (await bridge.request('library.probeFiles', { paths })) as unknown as {
              classification: Record<string, unknown>
              probes: Array<Record<string, unknown>>
            }
            expect(probeRes.probes.length).toBeGreaterThanOrEqual(4)
            const camProbe = probeRes.probes.find((p) => p.path === 'cam.vmd')
            const bodyProbe = probeRes.probes.find((p) => p.path === '01_hski.vmd')
            expect(camProbe?.tendency).toBe('cameraOnly')
            expect((camProbe?.cameraFrames as number) ?? 0).toBeGreaterThan(0)
            expect((bodyProbe?.bodyBoneFrames as number) ?? 0).toBeGreaterThan(0)
            expect(probeRes.classification.cameraCandidate).toBe('cam.vmd')
            expect(probeRes.classification.audioCandidate).toBe('song.wav')
            expect(extractSortedMotionPaths(probeRes.classification)).toEqual([
              '01_hski.vmd',
              '02_ttmr.vmd',
              '03_miku.vmd',
            ])

            const setOffsetFn = resolveStoreMethod(store, [
              'commitFormationSlotOffset',
              'setFormationSlotOffset',
              'setSlotOffset',
            ])
            const saveOffsetFn = resolveStoreMethod(store, ['saveFormationSlotOffset', 'saveSlotOffset'])
            expect(setOffsetFn).not.toBeNull()
            expect(saveOffsetFn).not.toBeNull()
            await invokeSetSlotOffset(setOffsetFn!, 0, { x: 0.25, y: 0, z: 0.15, yawDegrees: -10 })
            await saveOffsetFn!(0)

            const snap = (await bridge.request('state.get')) as unknown as {
              formation: { slots: Array<{ offset: { x: number; z: number; yawDegrees: number }; placementDirty: boolean }> }
            }
            const storeFm = (store.getState() as unknown as {
              formation: { slots: Array<{ offset: { x: number; z: number; yawDegrees: number }; placementDirty: boolean }> }
            }).formation
            expect(storeFm.slots[0].offset.x).toBeCloseTo(snap.formation.slots[0].offset.x)
            expect(storeFm.slots[0].offset.yawDegrees).toBeCloseTo(snap.formation.slots[0].offset.yawDegrees)
            expect(storeFm.slots[0].placementDirty).toBe(false)
            expect(store.getState().connected).toBe(true)
          } finally {
            bridge.close()
          }
          return 'Multi-VMD filename sorting, library.probeFiles classification & Store/state.get parity verified.'
        },
      )
    })
  }

  if (shouldRegisterTier(4)) {
    it('[T4-F13-01] Multi-actor group-song Store/Mock lifecycle (portraits, slot swap, overflow source, camera follow)', async (ctx) => {
      await runProbeCase(
        'T4-F13-01',
        4,
        'F13',
        'Multi-actor group-song Store/Mock lifecycle',
        ctx,
        probeMockAndStoreM4,
        async () => {
          const { bridge, store } = await createStartedStore('full')
          try {
            const req = (
              bridge as unknown as { request: (m: string, p?: unknown) => Promise<Record<string, unknown>> }
            ).request.bind(bridge)

            // 1. Ensure a 2-position group song exists so 3 mock actors produce an overflow slot
            let entries = (store.getState().library.entries as unknown as Array<Record<string, unknown>>) ?? []
            let groupEntry = entries.find((e) => e.isGroup === true && Number(e.positionCount) === 2)
            if (!groupEntry) {
              groupEntry = entries.find((e) => e.isGroup === true && Number(e.positionCount) >= 2)
            }
            if (!groupEntry || Number(groupEntry.positionCount) !== 2) {
              await new Promise<void>((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error('Timed out waiting for group-song import')), 5000)
                const offProg = bridge.on('import.progress', (p) => {
                  if (p.finished) {
                    clearTimeout(timer)
                    offProg()
                    resolve()
                  }
                })
                void req('library.import', {
                  request: {
                    title: 'E2E Duo Group Song',
                    subtitle: 'E2E',
                    motionVmd: '01_hski.vmd',
                    positions: [
                      { slot: 1, motionVmd: '01_hski.vmd', x: 0, y: 0, z: 0, yawDegrees: 0 },
                      { slot: 2, motionVmd: '02_ttmr.vmd', x: 0.6, y: 0, z: 0, yawDegrees: 0 },
                    ],
                  },
                }).catch((err) => {
                  clearTimeout(timer)
                  offProg()
                  reject(err)
                })
              })
              entries = (store.getState().library.entries as unknown as Array<Record<string, unknown>>) ?? []
              groupEntry = entries.find((e) => e.isGroup === true && Number(e.positionCount) === 2)
            }
            expect(groupEntry).toBeDefined()
            expect(groupEntry!.isGroup).toBe(true)
            expect(Number(groupEntry!.positionCount)).toBeGreaterThanOrEqual(2)
            expect(Array.isArray(groupEntry!.positions)).toBe(true)
            expect((groupEntry!.positions as unknown[]).length).toBe(Number(groupEntry!.positionCount))

            await store.selectMotion(String(groupEntry!.id), false)
            expect(store.getState().loaded.motionKey).toBe(String(groupEntry!.id))
            const fmSelected = (store.getState() as unknown as {
              formation: { isGroupSong: boolean; trackPositionCount: number }
            }).formation
            expect(fmSelected.isGroupSong).toBe(true)
            expect(fmSelected.trackPositionCount).toBe(Number(groupEntry!.positionCount))

            // 2. Portrait Data URI & NPC miss
            const portraitOk = await req('actors.getPortrait', { character: 'hski' })
            expect(portraitOk.ok).toBe(true)
            expect(portraitOk.character).toBe('hski')
            expect(typeof portraitOk.dataUri === 'string' && portraitOk.dataUri.startsWith('data:image/')).toBe(true)
            const portraitNpc = await req('actors.getPortrait', { character: 'unknown_npc' })
            expect(portraitNpc.dataUri ?? null).toBeNull()

            // 3. Slot swap invariants: actor fields swap, slot offset/sourceSlotIndex stay on slot index
            await req('formation.setSlotOffset', { slotIndex: 0, x: 0.35, y: 0.1, z: -0.2, yawDegrees: 12 })
            await req('formation.setSlotOffset', { slotIndex: 1, x: -0.45, y: 0, z: 0.3, yawDegrees: -25 })

            type FmSlot = {
              slotIndex: number
              occupied: boolean
              actorToken: number
              character: string
              shortName: string
              displayName: string
              isOverflow: boolean
              sourceSlotIndex: number
              offset: { x: number; y: number; z: number; yawDegrees: number }
              facialWeights: { overall: number; brow: number; eye: number; mouth: number }
              hasActorBoneOverrides: boolean
            }
            type FmState = {
              isGroupSong: boolean
              trackPositionCount: number
              slots: FmSlot[]
            }

            const before = structuredClone((store.getState() as unknown as { formation: FmState }).formation)
            expect(before.slots.length).toBeGreaterThanOrEqual(2)
            expect(before.slots[0].actorToken).not.toBe(before.slots[1].actorToken)

            const swapFn = resolveStoreMethod(store, ['swapFormationSlots', 'swapSlots'])
            if (swapFn) {
              await swapFn(0, 1)
            } else {
              await req('formation.swapSlots', { fromSlotIndex: 0, toSlotIndex: 1 })
            }

            const after = (store.getState() as unknown as { formation: FmState }).formation
            const snapAfterSwap = (await bridge.request('state.get')) as unknown as { formation: FmState }
            for (const view of [after, snapAfterSwap.formation]) {
              expect(view.slots[0].actorToken).toBe(before.slots[1].actorToken)
              expect(view.slots[1].actorToken).toBe(before.slots[0].actorToken)
              expect(view.slots[0].character).toBe(before.slots[1].character)
              expect(view.slots[1].character).toBe(before.slots[0].character)
              expect(view.slots[0].shortName).toBe(before.slots[1].shortName)
              expect(view.slots[1].shortName).toBe(before.slots[0].shortName)
              expect(view.slots[0].displayName).toBe(before.slots[1].displayName)
              expect(view.slots[1].displayName).toBe(before.slots[0].displayName)
              expect(view.slots[0].occupied).toBe(before.slots[1].occupied)
              expect(view.slots[1].occupied).toBe(before.slots[0].occupied)
              expect(view.slots[0].facialWeights).toEqual(before.slots[1].facialWeights)
              expect(view.slots[1].facialWeights).toEqual(before.slots[0].facialWeights)
              expect(view.slots[0].hasActorBoneOverrides).toBe(before.slots[1].hasActorBoneOverrides)
              expect(view.slots[1].hasActorBoneOverrides).toBe(before.slots[0].hasActorBoneOverrides)

              expect(view.slots[0].offset).toEqual(before.slots[0].offset)
              expect(view.slots[1].offset).toEqual(before.slots[1].offset)
              expect(view.slots[0].sourceSlotIndex).toBe(before.slots[0].sourceSlotIndex)
              expect(view.slots[1].sourceSlotIndex).toBe(before.slots[1].sourceSlotIndex)
            }

            // 4. Overflow slot sourceSlotIndex reassignment
            const overflowSlot = after.slots.find((s) => s.isOverflow)
            expect(overflowSlot).toBeDefined()
            expect(after.trackPositionCount).toBeGreaterThanOrEqual(2)
            await req('formation.setSlotSource', { slotIndex: overflowSlot!.slotIndex, sourceSlotIndex: 1 })
            expect(
              (store.getState() as unknown as { formation: FmState }).formation.slots[overflowSlot!.slotIndex]
                .sourceSlotIndex,
            ).toBe(1)
            const snapSrc1 = (await bridge.request('state.get')) as unknown as { formation: FmState }
            expect(snapSrc1.formation.slots[overflowSlot!.slotIndex].sourceSlotIndex).toBe(1)

            await req('formation.setSlotSource', { slotIndex: overflowSlot!.slotIndex, sourceSlotIndex: 0 })
            expect(
              (store.getState() as unknown as { formation: FmState }).formation.slots[overflowSlot!.slotIndex]
                .sourceSlotIndex,
            ).toBe(0)

            // 5. Camera followSlotIndex (-1 then 1)
            const currentCam = store.getState().camera.settings as unknown as Record<string, unknown>
            await req('camera.set', { settings: { ...currentCam, followSlotIndex: -1 } })
            expect((store.getState().camera.settings as unknown as { followSlotIndex: number }).followSlotIndex).toBe(-1)
            const snapCamAll = (await bridge.request('state.get')) as unknown as {
              camera: { settings: { followSlotIndex: number } }
            }
            expect(snapCamAll.camera.settings.followSlotIndex).toBe(-1)

            await req('camera.set', { settings: { ...currentCam, followSlotIndex: 1 } })
            expect((store.getState().camera.settings as unknown as { followSlotIndex: number }).followSlotIndex).toBe(1)
            const snapCam1 = (await bridge.request('state.get')) as unknown as {
              camera: { settings: { followSlotIndex: number } }
            }
            expect(snapCam1.camera.settings.followSlotIndex).toBe(1)
            return 'Full multi-actor Store/Mock lifecycle verified.'
          } finally {
            bridge.close()
          }
        },
      )
    })
  }
})
