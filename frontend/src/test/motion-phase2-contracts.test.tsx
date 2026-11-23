import { act, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'
import MotionApp from '@/motion/components/MotionApp'
import * as ActorSlotStripMod from '@/motion/components/ActorSlotStrip'
import { MotionErrorBoundary } from '@/motion/components/MotionErrorBoundary'
import { setMotionBridge } from '@/motion/bridge'
import {
  DEFAULT_CAMERA_SETTINGS,
  DEFAULT_PLAYER_STATE,
  DEFAULT_STAGE_FORMATION,
  DEFAULT_STAGE_SETTINGS,
  defaultSoleSettings,
  defaultStudioSettings,
  type MotionBridgeClient,
  type MotionEventName,
  type MotionEvents,
  type MotionMethodName,
  type MotionMethods,
  type StageNode,
} from '@/motion/protocol'
import { motionStore } from '@/motion/store'

const PHASE2_CAPABILITIES: MotionMethodName[] = [
  'bridge.hello',
  'bridge.ping',
  'motion.hello',
  'state.get',
  'player.play',
  'player.pause',
  'player.resume',
  'player.stop',
  'player.cycle',
  'player.seek',
  'player.seekFrame',
  'player.stepFrame',
  'player.scrubBegin',
  'player.scrubUpdate',
  'player.scrubEnd',
  'player.setSpeed',
  'player.setVolume',
  'player.saveVolume',
  'player.reload',
  'playlist.setMode',
  'library.refresh',
  'library.select',
  'library.delete',
  'library.openFolder',
  'library.import',
  'library.probeFiles',
  'library.startEdit',
  'library.probeAudio',
  'library.getCover',
  'clock.setRunMode',
  'clock.configure',
  'audioEffect.set',
  'actors.selectTarget',
  'facial.setWeights',
  'facial.save',
  'position.set',
  'position.save',
  'camera.set',
  'camera.save',
  'stage.set',
  'stage.scan',
  'stage.toggleNode',
  'stage.setLoadReplacement',
  'settings.setPhysics',
  'shell.setTextInputActive',
  'appearance.get',
  'dialog.pickFile',
  'dialog.poll',
  'drop.setTarget',
  'drop.poll',
  'drop.peek',
  'actors.getPortrait',
  'formation.selectSlot',
  'formation.swapSlots',
  'formation.setSlotOffset',
  'formation.resetSlotOffset',
  'formation.saveSlotOffset',
  'formation.setSlotSource',
  'bone.get',
  'bone.set',
  'bone.reset',
  'bone.resetAll',
  'bone.setMaster',
  'bone.reload',
  'bone.saveToMotion',
  'bone.saveToGlobal',
  'gaze.get',
  'gaze.set',
  'sole.get',
  'sole.set',
  'sole.save',
  'sole.load',
  'solePitch.get',
  'solePitch.set',
  'studio.get',
  'studio.set',
  'studio.reset',
  'experiments.get',
  'experiments.set',
  'telemetry.get',
]

class FakeNativePhase2Bridge implements MotionBridgeClient {
  readonly kind = 'webview2' as const
  private handlers = new Map<MotionEventName, Set<(payload: never) => void>>()
  readonly calls: Array<{ method: MotionMethodName; params: unknown }> = []
  scanPayload: unknown = {
    nodes: [
      {
        index: 0,
        name: 'StageRoot',
        displayName: 'StageRoot',
        parentIndex: -1,
        depth: 0,
        hasChildren: true,
        currentlyActive: true,
        isColliderLocked: false,
        isIdentifiedBackground: true,
        isLightingRoot: false,
        isLightingNode: false,
      },
      {
        index: 1,
        name: 'Collider',
        displayName: 'Collider',
        parentIndex: 0,
        depth: 1,
        hasChildren: false,
        currentlyActive: true,
        isColliderLocked: true,
        isIdentifiedBackground: false,
        isLightingRoot: false,
        isLightingNode: false,
      },
    ] satisfies StageNode[],
  }
  dropPeekPayload: MotionMethods['drop.peek']['result'] = {
    visual: 'none',
    fileCount: 0,
    accepted: 0,
    hostElevated: false,
    hostBlocked: false,
  }

  emit<E extends MotionEventName>(event: E, payload: MotionEvents[E]): void {
    const set = this.handlers.get(event)
    if (!set) return
    for (const handler of set) handler(payload as never)
  }

  on<E extends MotionEventName>(event: E, handler: (payload: MotionEvents[E]) => void): () => void {
    let set = this.handlers.get(event)
    if (!set) {
      set = new Set()
      this.handlers.set(event, set)
    }
    set.add(handler as (payload: never) => void)
    return () => set?.delete(handler as (payload: never) => void)
  }

  close(): void {}

  async request<M extends MotionMethodName>(
    method: M,
    params?: MotionMethods[M]['params'],
  ): Promise<MotionMethods[M]['result']> {
    this.calls.push({ method, params })
    switch (method) {
      case 'bridge.hello':
        return { protocol: 1, sessionId: 1 } as MotionMethods[M]['result']
      case 'motion.hello':
        return {
          protocol: 1,
          sessionId: 1,
          pluginVersion: '0.1.1',
          supportedMethods: PHASE2_CAPABILITIES,
          supportedEvents: ['stage.nodesChanged', 'import.progress', 'bone.changed'],
        } as MotionMethods[M]['result']
      case 'state.get':
        return {
          permission: { injectionAllowed: true, notice: '' },
          player: { ...DEFAULT_PLAYER_STATE, duration: 120, totalFrames: 7200 },
          library: {
            revision: 7,
            refreshing: false,
            notice: '',
            entries: [
              {
                id: 'starlight-step',
                title: 'Starlight Step',
                directory: 'motions/starlight-step',
                durationSeconds: 120,
                totalFrames: 7200,
                hasMotion: true,
                hasFacial: true,
                hasCamera: false,
                hasAudio: true,
                isGroup: false,
                positionCount: 1,
                positions: [],
              },
            ],
          },
          loaded: {
            motionKey: '',
            generation: 1,
            injectionSession: 1,
            sourceMissing: false,
            sourceChanged: false,
          },
          playlist: { mode: 1, notice: '' },
          actors: {
            epoch: 2,
            actors: [
              { token: 101, label: '咲季 · 1', character: 'hski', displayName: '花海咲季', shortName: '咲季', selected: true },
              { token: 102, label: '凛 · 2', character: 'ttmr', displayName: '月村手毬', shortName: '凛', selected: false },
            ],
            targetLabel: '咲季 · 1',
            boundLabel: '尚未绑定角色',
          },
          formation: {
            ...DEFAULT_STAGE_FORMATION,
            slotCount: 2,
            slots: [
              {
                ...DEFAULT_STAGE_FORMATION.slots[0],
                slotIndex: 0,
                occupied: true,
                actorToken: 101,
                character: 'hski',
                shortName: '咲季',
                displayName: '花海咲季',
              },
              {
                ...DEFAULT_STAGE_FORMATION.slots[0],
                slotIndex: 1,
                occupied: true,
                actorToken: 102,
                character: 'ttmr',
                shortName: '凛',
                displayName: '月村手毬',
              },
            ],
          },
          camera: {
            settings: { ...DEFAULT_CAMERA_SETTINGS },
            available: true,
            owned: true,
            hasTrack: false,
            error: '',
            followTargetError: '',
            notice: '',
          },
          stage: {
            settings: { ...DEFAULT_STAGE_SETTINGS },
            sceneName: 'TestStage',
            statusNotice: '就绪',
          },
          facial: { weights: { overall: 1, brow: 1, eye: 1, mouth: 1 }, notice: '' },
          position: { offset: { x: 0, y: 0, z: 0 }, notice: '' },
          physicsBackend: 2,
          musicVolumeSupport: 1,
          musicVolumeNotice: '',
          importProgress: null,
          motionsRoot: 'GakuLoader/motions',
        } as MotionMethods[M]['result']
      case 'appearance.get':
        return { theme: 0, uiScale: 1, dpiScale: 1, scaleLimited: false } as MotionMethods[M]['result']
      case 'bone.get':
        return { masterEnabled: true, overrides: [], notice: '' } as MotionMethods[M]['result']
      case 'gaze.get':
        return {
          settings: { mode: 0, desktopLookAtEnabled: true, eyeWeight: 0.7, headWeight: 0.4, bodyWeight: 0 },
          runtimeAvailable: true,
          vrifyQueryAvailable: false,
          vrifyLiveGazeActive: false,
          notice: '',
        } as MotionMethods[M]['result']
      case 'experiments.get':
        return {
          settings: {
            mikuMouthFit: true,
            mikuEyeFit: true,
            mikuFitAllowUnverified: false,
            waistSupport: true,
            beforeQuartz: true,
          },
          mouthFitStatus: '尚未接管',
          eyeFitStatus: '尚未接管',
          fittedFaceLabel: '',
          notice: '',
        } as MotionMethods[M]['result']
      case 'sole.get':
        return {
          settings: defaultSoleSettings(),
          snapshot: { available: false, calibrated: false, clipTime: 0, feet: [] },
          generation: 1,
          notice: '',
        } as MotionMethods[M]['result']
      case 'solePitch.get':
        return {
          settings: { enabled: false, maxPitchDegrees: 20, useMeshEnvelope: false },
          clipTime: 0,
          feet: [
            {
              status: '待观察',
              applied: false,
              setterReadbackMatches: false,
              measuredAfterValid: false,
              measuredAfterMinimumDistanceM: 0,
              solution: { valid: false, pitchDegrees: 0, usedMeshEnvelope: false, beforePenetrationM: 0 },
            },
          ],
          notice: '',
        } as MotionMethods[M]['result']
      case 'studio.get':
        return {
          settings: defaultStudioSettings(),
          revision: 1,
          lightingNotice: '',
          videoNotice: '',
        } as MotionMethods[M]['result']
      case 'telemetry.get':
        return {
          statusLine: 'Ready (Test)',
          player: {
            state: 0,
            stateName: '已停止',
            time: 0,
            duration: 120,
            totalFrames: 7200,
            fps: 60,
            boneCount: 51,
            loadedMotionFile: '',
          },
          actor: {
            actorLabel: '咲季 · 1',
            boundBoneCount: 45,
            totalBoneCount: 51,
          },
        } as MotionMethods[M]['result']
      case 'stage.scan':
        return this.scanPayload as MotionMethods[M]['result']
      case 'stage.setLoadReplacement':
        return { notice: '舞台选择已保存' } as MotionMethods[M]['result']
      case 'drop.peek':
        return this.dropPeekPayload as MotionMethods[M]['result']
      case 'drop.poll':
        return { files: [] } as MotionMethods[M]['result']
      case 'library.startEdit':
        return {
          key: 'starlight-step',
          title: 'Starlight Step',
          hasFacial: true,
          hasCamera: false,
          hasAudio: true,
          timing: {
            motion: { speed: 1, offsetSeconds: 0 },
            facial: { speed: 1, offsetSeconds: 0 },
            camera: { speed: 1, offsetSeconds: 0 },
            audio: { speed: 1, offsetSeconds: 0 },
          },
        } as MotionMethods[M]['result']
      case 'library.import':
        return { started: true } as MotionMethods[M]['result']
      default:
        return {} as MotionMethods[M]['result']
    }
  }
}

afterEach(() => {
  motionStore.reset()
  setMotionBridge(null)
  vi.restoreAllMocks()
})

describe('Phase 2 C++ wire contract & crash resilience', () => {
  it('handles StagePage node scanning and tolerates malformed empty scan payloads without crashing', async () => {
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    setMotionBridge(bridge)
    render(<MotionApp />)

    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())
    await user.click(screen.getByRole('button', { name: '舞台' }))
    await user.click(screen.getByText('高级场景编辑'))
    await user.click(screen.getByLabelText('自定义场景节点 [开关]'))

    await waitFor(() => expect(screen.getByText('StageRoot')).toBeInTheDocument())
    expect(screen.getByText('Collider')).toBeInTheDocument()

    // Even if a bridge returns {}, StagePage normalizes nodes to [] instead of throwing.
    bridge.scanPayload = {}
    await user.click(screen.getByRole('button', { name: '扫描场景节点' }))
    await waitFor(() => expect(screen.getByText('尚未扫描或没有候选节点。')).toBeInTheDocument())
    expect(screen.queryByTestId('motion-error-boundary')).not.toBeInTheDocument()
  })

  it('catches page render failures in MotionErrorBoundary while keeping navigation alive', async () => {
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => {})
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    setMotionBridge(bridge)
    render(<MotionApp />)
    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())

    const spy = vi.spyOn(ActorSlotStripMod, 'ActorSlotStrip').mockImplementation(() => {
      throw new Error('synthetic render explosion')
    })
    try {
      await user.click(screen.getByRole('button', { name: '动作' }))
      const boundary = await screen.findByTestId('motion-error-boundary')
      expect(boundary).toHaveTextContent('当前页面渲染遇到异常')
      expect(boundary).toHaveTextContent('synthetic render explosion')
      expect(screen.getByTestId('motion-sidebar')).toBeInTheDocument()
      expect(screen.getByTestId('transport-bar')).toBeInTheDocument()

      await user.click(within(boundary).getByRole('button', { name: '返回曲库' }))
      await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())
      expect(screen.queryByTestId('motion-error-boundary')).not.toBeInTheDocument()
    } finally {
      spy.mockRestore()
      consoleError.mockRestore()
    }
  })

  it('renders DropOverlay, ImportDialog edit mode, and ImportProgressModal from real Phase 2 payloads', async () => {
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    bridge.dropPeekPayload = {
      visual: 'hover',
      fileCount: 2,
      accepted: 1,
      hostElevated: true,
      hostBlocked: true,
    }
    setMotionBridge(bridge)
    render(<MotionApp />)

    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())
    const overlay = await screen.findByTestId('drop-overlay')
    expect(overlay).toHaveTextContent('松开导入 1 个文件')
    expect(overlay).toHaveTextContent('管理员权限')

    await user.click(screen.getByTestId('library-row-starlight-step'))
    await user.click(screen.getByRole('button', { name: '编辑 Starlight Step' }))
    const importDialog = await screen.findByTestId('import-dialog')
    expect(importDialog).toBeInTheDocument()

    await user.click(within(importDialog).getByRole('combobox', { name: '源动作帧率' }))
    await user.click(await screen.findByRole('option', { name: '60 fps' }))
    await user.click(within(importDialog).getByRole('combobox', { name: '输出采样帧率' }))
    await user.click(await screen.findByRole('option', { name: '30 fps' }))

    await user.click(within(importDialog).getByRole('button', { name: '保存修改' }))
    const importCall = bridge.calls.find((call) => call.method === 'library.import')
    expect(importCall?.params).toMatchObject({
      request: {
        editKey: 'starlight-step',
        sourceFps: 60,
        fps: 30,
        rebakeOutput: true,
      },
    })
    const progressModal = await screen.findByTestId('import-progress-modal')
    expect(progressModal).toBeInTheDocument()

    act(() => {
      bridge.emit('import.progress', {
        active: false,
        finished: true,
        succeeded: false,
        progress: 0.4,
        stage: 'ERROR',
        statusText: '导入失败',
        errorDetail: 'invalid vmd magic',
      })
    })
    await waitFor(() => expect(screen.getByText('invalid vmd magic')).toBeInTheDocument())
    await user.click(within(screen.getByTestId('import-progress-modal')).getByRole('button', { name: '关闭' }))
    expect(screen.queryByTestId('import-progress-modal')).not.toBeInTheDocument()
  })

  it('supports editing unoverridden bones in BoneSection and selecting slots in ActorSlotStrip', async () => {
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    setMotionBridge(bridge)
    render(<MotionApp />)
    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())

    await user.click(screen.getByRole('button', { name: '动作' }))
    expect(screen.getByTestId('actor-slot-strip')).toBeInTheDocument()

    await user.click(screen.getByText('高级动作设置'))
    await user.click(screen.getByRole('button', { name: '头部 (Head)' }))
    const editor = screen.getByTestId('bone-editor')
    expect(editor).toHaveTextContent('头部 (Head)')
    await user.click(within(editor).getAllByRole('button', { name: '+5°' })[0])
    const boneSetCall = bridge.calls.find((call) => call.method === 'bone.set')
    expect(boneSetCall?.params).toMatchObject({ humanoidId: 10, boneName: 'Head', pitch: 5 })
  })

  it('renders all six DeveloperPage tabs and updates scrub probes in TelemetryTab', async () => {
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    setMotionBridge(bridge)
    render(<MotionApp />)
    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())

    motionStore.scrubStart(1.5)
    motionStore.scrubMove(2.5)
    motionStore.scrubCommit(3.5)

    await user.click(screen.getByRole('button', { name: '设置' }))
    await user.click(screen.getByRole('switch', { name: '开发者模式' }))
    await user.click(screen.getByRole('button', { name: '[开发]' }))
    expect(screen.getByTestId('developer-page')).toBeInTheDocument()

    await user.click(screen.getByRole('tab', { name: '遥测与诊断' }))
    const telemetryTab = await screen.findByTestId('telemetry-tab')
    expect(telemetryTab).toHaveTextContent('已停止')
    expect(telemetryTab).toHaveTextContent('咲季 · 1')
    expect(telemetryTab).toHaveTextContent('begin=1 update=1 end=1')

    await user.click(screen.getByRole('tab', { name: '鞋底测量 v2' }))
    expect(screen.getByTestId('sole-tab')).toBeInTheDocument()

    await user.click(screen.getByRole('tab', { name: 'Pitch Demo [Experimental]' }))
    expect(screen.getByTestId('sole-pitch-tab')).toBeInTheDocument()

    await user.click(screen.getByRole('tab', { name: '摄影舞台替换' }))
    expect(screen.getByTestId('stage-replacement-tab')).toBeInTheDocument()

    await user.click(screen.getByRole('tab', { name: '布光与视频' }))
    expect(screen.getByTestId('studio-tab')).toBeInTheDocument()
  })

  it('exposes stage replacement and studio controls inside PermissionGate when capabilities are present', async () => {
    const user = userEvent.setup()
    const bridge = new FakeNativePhase2Bridge()
    setMotionBridge(bridge)
    render(<MotionApp />)
    await waitFor(() => expect(screen.getByTestId('library-page')).toBeInTheDocument())

    act(() => {
      bridge.emit('permission.changed', { injectionAllowed: false, notice: '当前不在摄影场景' })
    })
    await waitFor(() => expect(screen.getByTestId('permission-gate')).toBeInTheDocument())
    await user.click(screen.getByText('开发者选项'))
    await user.click(screen.getByRole('switch', { name: '开发者模式' }))
    expect(screen.getByTestId('stage-replacement-tab')).toBeInTheDocument()
    expect(screen.getByTestId('studio-tab')).toBeInTheDocument()
    expect(screen.queryByTestId('unsupported-panel')).not.toBeInTheDocument()
  })

  it('supports resetting MotionErrorBoundary when resetKey changes', () => {
    let shouldThrow = true
    function MaybeThrow() {
      if (shouldThrow) throw new Error('boom')
      return <div data-testid="recovered">ok</div>
    }
    const { rerender } = render(
      <MotionErrorBoundary scope="page" resetKey="stage">
        <MaybeThrow />
      </MotionErrorBoundary>,
    )
    expect(screen.getByTestId('motion-error-boundary')).toBeInTheDocument()
    shouldThrow = false
    rerender(
      <MotionErrorBoundary scope="page" resetKey="library">
        <MaybeThrow />
      </MotionErrorBoundary>,
    )
    expect(screen.getByTestId('recovered')).toBeInTheDocument()
  })
})
