import * as React from 'react'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Separator } from '@/components/ui/separator'
import { Slider } from '@/components/ui/slider'
import { Switch } from '@/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { GAZE_MODES, STAGE_REPLACEMENT_OPTIONS, type RgbColor } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { DragFloat } from '../components/DragFloat'
import { Vector3Field } from '../components/NumberField'
import { PropertySection } from '../components/PropertyGroup'
import { PendingBadge } from '../components/Unsupported'

function ColorField({ label, value, disabled, onChange }: {
  label: string
  value: RgbColor
  disabled?: boolean
  onChange: (next: RgbColor) => void
}) {
  const toHex = (rgb: RgbColor) =>
    `#${rgb.map((channel) => Math.round(channel * 255).toString(16).padStart(2, '0')).join('')}`
  const fromHex = (hex: string): RgbColor | null => {
    const match = /^#?([0-9a-f]{6})$/i.exec(hex)
    if (!match) return null
    const raw = match[1]
    return [0, 2, 4].map((offset) => Number.parseInt(raw.slice(offset, offset + 2), 16) / 255) as RgbColor
  }
  return (
    <label className="flex items-center gap-2 text-sm">
      <span className="w-16 shrink-0 text-xs text-muted-foreground">{label}</span>
      <input
        type="color"
        className="size-7 cursor-pointer rounded border"
        disabled={disabled}
        value={toHex(value)}
        onChange={(event) => {
          const next = fromHex(event.target.value)
          if (next) onChange(next)
        }}
      />
    </label>
  )
}

function ExperimentsTab() {
  const experiments = useMotionStore((state) => state.experiments)
  const gaze = useMotionStore((state) => state.gaze)
  const capabilities = useMotionStore((state) => state.capabilities)
  const supports = (method: string) => capabilities.includes(method)
  const settings = experiments.settings
  return (
    <PropertySection title="动作实验与视线控制" description="Miku 表情拟合、腰部支撑与眼部/头部追焦实验参数" divided={false} bodyClassName="p-3.5">
      <div className="space-y-4">
        <div className="space-y-2">
          <p className="text-sm font-medium">Miku 拟合</p>
          {([
            ['mikuMouthFit', '嘴形拟合'],
            ['mikuEyeFit', '眼眉拟合'],
            ['mikuFitAllowUnverified', '允许未验收分量'],
          ] as const).map(([key, label]) => (
            <label key={key} className="flex items-center gap-2 text-sm">
              <Checkbox
                checked={settings[key]}
                disabled={!supports('experiments.set')}
                onCheckedChange={(checked) => void motionStore.setExperiment({ [key]: checked === true })}
              />
              {label}
            </label>
          ))}
          <p className="text-xs text-muted-foreground">嘴形：{experiments.mouthFitStatus}；眼眉：{experiments.eyeFitStatus}</p>
          {experiments.fittedFaceLabel && <p className="text-xs text-muted-foreground">脸型：{experiments.fittedFaceLabel}</p>}
        </div>
        <Separator />
        <div className="space-y-2">
          <label className="flex items-center gap-2 text-sm">
            <Checkbox
              checked={settings.waistSupport}
              disabled={!supports('experiments.set')}
              onCheckedChange={(checked) => void motionStore.setExperiment({ waistSupport: checked === true })}
            />
            腰部支撑
          </label>
          <label className="flex items-center gap-2 text-sm">
            <Checkbox
              checked={settings.beforeQuartz}
              disabled={!supports('experiments.set')}
              onCheckedChange={(checked) => void motionStore.setExperiment({ beforeQuartz: checked === true })}
            />
            BeforeQuartz
          </label>
          <div className="space-y-1 pt-1">
            <label className="flex items-center gap-2 text-sm">
              <Checkbox
                checked={settings.muteNativeSoundEffects}
                disabled={!supports('experiments.set')}
                onCheckedChange={(checked) =>
                  void motionStore.setExperiment({ muteNativeSoundEffects: checked === true })
                }
              />
              播放 MMD 时屏蔽游戏音效（默认开启）
            </label>
            <p className="pl-6 text-xs text-muted-foreground">
              仅在 MMD 表演期间静音游戏 Se 音效（含界面音效），停止播放、离开场景或受保护拍照时自动恢复，修改会持久化保存。
            </p>
          </div>
        </div>
        <Separator />
        <div className="space-y-3">
          <div className="flex items-center gap-2">
            <p className="text-sm font-medium">视线</p>
            {!supports('gaze.set') && <PendingBadge />}
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <Select
              items={Object.fromEntries(GAZE_MODES.map((mode, index) => [String(index), mode]))}
              value={String(gaze.settings.mode)}
              onValueChange={(value) => void motionStore.setGazeSettings({ mode: Number(value) })}
            >
              <SelectTrigger className="h-8 w-80"><SelectValue /></SelectTrigger>
              <SelectContent>
                {GAZE_MODES.map((mode, index) => (
                  <SelectItem key={mode} value={String(index)}>{mode}</SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Button variant="outline" size="sm" onClick={() => motionStore.cycleGazeMode()}>循环切换</Button>
          </div>
          <label className="flex items-center gap-2 text-sm">
            <Checkbox
              checked={gaze.settings.desktopLookAtEnabled}
              onCheckedChange={(checked) => void motionStore.setGazeSettings({ desktopLookAtEnabled: checked === true })}
            />
            桌面追焦
          </label>
          {([
            ['eyeWeight', '眼睛权重'],
            ['headWeight', '头部权重'],
            ['bodyWeight', '身体权重'],
          ] as const).map(([key, label]) => (
            <div key={key} className="flex items-center gap-3">
              <span className="w-16 text-xs text-muted-foreground">{label}</span>
              <Slider
                className="max-w-xs flex-1"
                min={0}
                max={1}
                step={0.01}
                value={[gaze.settings[key]]}
                onValueChange={([next]) => void motionStore.setGazeSettings({ [key]: next })}
              />
              <span className="w-10 text-xs tabular-nums">{gaze.settings[key].toFixed(2)}</span>
            </div>
          ))}
          <p className="text-xs text-muted-foreground">运行时{gaze.runtimeAvailable ? '已接入' : '未接入'}；{gaze.notice}</p>
        </div>
      </div>
    </PropertySection>
  )
}

function TelemetryTab() {
  const telemetry = useMotionStore((state) => state.telemetry)
  const begin = useMotionStore((state) => state.probeScrubBegin)
  const update = useMotionStore((state) => state.probeScrubUpdate)
  const end = useMotionStore((state) => state.probeScrubEnd)
  React.useEffect(() => {
    void motionStore.refreshTelemetry()
    const timer = setInterval(() => void motionStore.refreshTelemetry(), 1000)
    return () => clearInterval(timer)
  }, [])
  return (
    <PropertySection title="实时遥测与诊断" description="每秒刷新播放器时基、绑定骨骼数量与 Scrub 探针计数" divided={false} bodyClassName="p-3.5">
      {!telemetry ? (
        <p className="text-sm text-muted-foreground">遥测不可用。</p>
      ) : (
        <div className="space-y-2 text-sm" data-testid="telemetry-tab">
          <p className="text-muted-foreground">{telemetry.statusLine}</p>
          <p>
            播放器：{telemetry.player.stateName} / {telemetry.player.time.toFixed(3)}s /{' '}
            {telemetry.player.duration.toFixed(3)}s / {telemetry.player.totalFrames} 帧 @ {telemetry.player.fps}fps
          </p>
          <p>骨骼：{telemetry.player.boneCount}；文件：{telemetry.player.loadedMotionFile || '无'}</p>
          <p>角色：{telemetry.actor.actorLabel}（绑定 {telemetry.actor.boundBoneCount}/{telemetry.actor.totalBoneCount} 骨）</p>
          <p className="font-mono text-xs text-muted-foreground">Scrub 探针：begin={begin} update={update} end={end}</p>
        </div>
      )}
    </PropertySection>
  )
}

function SoleTab() {
  const sole = useMotionStore((state) => state.sole)
  const draft = useMotionStore((state) => state.soleDraft)
  const span = useMotionStore((state) => state.solePlotSpanM)
  const capabilities = useMotionStore((state) => state.capabilities)
  const supports = (method: string) => capabilities.includes(method)
  React.useEffect(() => {
    motionStore.startSolePolling()
    return () => motionStore.stopSolePolling()
  }, [])
  return (
    <PropertySection title="鞋底平面与足部代理测量 v2" description="校准地面法线、偏移距离与左右靴代理测量点" divided={false} bodyClassName="p-3.5">
      <div className="space-y-3" data-testid="sole-tab">
        <div className="flex flex-wrap gap-3">
          {([
            ['enabled', '测量开关'],
            ['useBootProxy', '使用培育靴粗略代理'],
            ['planeCalibrated', '地面已校准'],
            ['shoeCalibrated', '鞋型已核对'],
          ] as const).map(([key, label]) => (
            <label key={key} className="flex items-center gap-2 text-sm">
              <Checkbox
                checked={draft[key]}
                onCheckedChange={(checked) => motionStore.patchSoleDraft({ [key]: checked === true })}
              />
              {label}
            </label>
          ))}
          <Button variant="outline" size="sm" onClick={() => motionStore.useSoleManualFallback()}>切回手工点</Button>
        </div>
        <Vector3Field
          labelPrefix="平面法线"
          value={draft.planeNormal}
          min={-1}
          max={1}
          step={0.01}
          onCommit={(next) => motionStore.patchSoleDraft({ planeNormal: next })}
        />
        <div className="grid gap-2 sm:grid-cols-2">
          <div className="space-y-1">
            <Label className="text-xs">平面距离 (米)</Label>
            <DragFloat
              ariaLabel="平面距离"
              className="w-full"
              inputClassName="h-8 w-full text-xs"
              value={draft.planeOffsetM}
              min={-10000}
              max={10000}
              step={0.001}
              decimals={4}
              unit="m"
              onChange={(planeOffsetM) => motionStore.patchSoleDraft({ planeOffsetM })}
              onCommit={(planeOffsetM) => motionStore.patchSoleDraft({ planeOffsetM })}
            />
          </div>
          <div className="space-y-1">
            <Label className="text-xs">显示容差 (米)</Label>
            <DragFloat
              ariaLabel="显示容差"
              className="w-full"
              inputClassName="h-8 w-full text-xs"
              value={draft.displayBandM}
              min={0}
              max={0.05}
              step={0.0001}
              decimals={4}
              unit="m"
              onChange={(displayBandM) => motionStore.patchSoleDraft({ displayBandM })}
              onCommit={(displayBandM) => motionStore.patchSoleDraft({ displayBandM })}
            />
          </div>
        </div>
        <div className="flex gap-2">
          <Button variant="outline" size="sm" onClick={() => motionStore.normalizeSolePlaneNormal()}>归一化法线</Button>
          <Button variant="outline" size="sm" onClick={() => motionStore.setSoleHorizontalPlane()}>水平世界平面</Button>
        </div>
        {[0, 1].map((side) => (
          <div key={side} className="space-y-1">
            <p className="text-xs font-medium">{side === 0 ? '左靴' : '右靴'}手工三点</p>
            {draft.points[side].map((point, index) => (
              <Vector3Field
                key={index}
                labelPrefix={`点${index + 1}`}
                value={point}
                min={-10}
                max={10}
                step={0.001}
                onCommit={(next) => motionStore.patchSoleDraftPoint(side, index, next)}
              />
            ))}
          </div>
        ))}
        <div className="flex flex-wrap gap-2">
          <Button size="sm" disabled={!supports('sole.set')} onClick={() => void motionStore.applySoleSettings()}>应用</Button>
          <Button variant="outline" size="sm" onClick={() => motionStore.reloadSoleDraft()}>读取已应用值</Button>
          <Button variant="outline" size="sm" disabled={!supports('sole.save')} onClick={() => void motionStore.saveSoleSettings()}>保存</Button>
          <Button variant="outline" size="sm" disabled={!supports('sole.load')} onClick={() => void motionStore.loadSoleSettings()}>载入</Button>
        </div>
        <div className="space-y-1">
          <Label className="text-xs">视图跨度 (米)</Label>
          <Slider min={0.2} max={3} step={0.1} value={[span]} onValueChange={([next]) => motionStore.setSolePlotSpan(next)} />
        </div>
        <p className="text-xs text-muted-foreground">{sole.notice}</p>
      </div>
    </PropertySection>
  )
}

function SolePitchTab() {
  const state = useMotionStore((state) => state.solePitch)
  React.useEffect(() => {
    motionStore.startSolePitchPolling()
    return () => motionStore.stopSolePitchPolling()
  }, [])
  return (
    <PropertySection title="Pitch Demo [Experimental]" description="实验性足部俯仰穿模修正求解器" divided={false} bodyClassName="p-3.5">
      <div className="space-y-3" data-testid="sole-pitch-tab">
        <div className="flex flex-wrap gap-4">
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={state.settings.enabled} onCheckedChange={(checked) => void motionStore.setSolePitchSettings({ enabled: checked })} />
            Demo 开关
          </label>
          <label className="flex items-center gap-2 text-sm">
            <Checkbox checked={state.settings.useMeshEnvelope} onCheckedChange={(checked) => void motionStore.setSolePitchSettings({ useMeshEnvelope: checked === true })} />
            实测蓝靴包络
          </label>
        </div>
        <div className="flex items-center gap-3">
          <span className="w-20 text-xs text-muted-foreground">最大 pitch</span>
          <Slider className="max-w-xs flex-1" min={0} max={35} step={0.5}
            value={[state.settings.maxPitchDegrees]}
            onValueChange={([next]) => void motionStore.setSolePitchSettings({ maxPitchDegrees: next })} />
          <span className="w-12 text-xs tabular-nums">{state.settings.maxPitchDegrees}°</span>
        </div>
        {state.feet.map((foot, index) => (
          <p key={index} className="text-xs text-muted-foreground">
            {index === 0 ? '左' : '右'}：{foot.status}；pitch={foot.solution.pitchDegrees.toFixed(1)}°；穿透={foot.solution.beforePenetrationM.toFixed(4)}m
          </p>
        ))}
        <p className="text-xs text-muted-foreground">{state.notice}</p>
      </div>
    </PropertySection>
  )
}

export function StageReplacementTab() {
  const stage = useMotionStore((state) => state.stage)
  const notice = useMotionStore((state) => state.stageLoadNotice)
  return (
    <div className="space-y-2" data-testid="stage-replacement-tab">
      <Select
        items={Object.fromEntries(STAGE_REPLACEMENT_OPTIONS.map((option) => [option.key || 'native', option.label]))}
        value={stage.settings.loadReplacementKey || 'native'}
        onValueChange={(value) => void motionStore.setStageLoadReplacement(value === 'native' ? '' : value)}
      >
        <SelectTrigger className="h-8 w-80"><SelectValue /></SelectTrigger>
        <SelectContent>
          {STAGE_REPLACEMENT_OPTIONS.map((option) => (
            <SelectItem key={option.key || 'native'} value={option.key || 'native'}>{option.label}</SelectItem>
          ))}
        </SelectContent>
      </Select>
      <p className="text-xs text-muted-foreground">下次进入摄影模式时生效；设置自动保存。</p>
      {notice && <p className="text-sm text-muted-foreground">{notice}</p>}
    </div>
  )
}

export function StudioTab() {
  const studio = useMotionStore((state) => state.studio)
  const draft = useMotionStore((state) => state.studioDraft)
  const videoPath = useMotionStore((state) => state.studioVideoPath)
  const busy = useMotionStore((state) => state.dialogBusy)
  const capabilities = useMotionStore((state) => state.capabilities)
  const supports = (method: string) => capabilities.includes(method)
  const lighting = draft.lighting
  const video = draft.video
  return (
    <div className="space-y-4" data-testid="studio-tab">
      <div className="space-y-3">
        <div className="flex items-center gap-4">
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={lighting.enabled} onCheckedChange={(checked) => motionStore.patchStudioLighting({ enabled: checked })} />
            启用自定义布光
          </label>
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={lighting.actorStyle} onCheckedChange={(checked) => motionStore.patchStudioLighting({ actorStyle: checked })} />
            人物风格光
          </label>
        </div>
        <div className="flex flex-wrap gap-3">
          <ColorField label="主光" value={lighting.keyColor} onChange={(next) => motionStore.patchStudioLighting({ keyColor: next })} />
          <ColorField label="补光" value={lighting.fillColor} onChange={(next) => motionStore.patchStudioLighting({ fillColor: next })} />
          <ColorField label="轮廓光" value={lighting.rimColor} onChange={(next) => motionStore.patchStudioLighting({ rimColor: next })} />
          <ColorField label="装饰灯" value={lighting.decalTint} onChange={(next) => motionStore.patchStudioLighting({ decalTint: next })} />
        </div>
        <div className="grid gap-3 sm:grid-cols-3">
          {([
            ['keyIntensity', '主光强度', 0, 8],
            ['fillIntensity', '补光强度', 0, 8],
            ['rimIntensity', '轮廓强度', 0, 8],
            ['actorIntensity', '人物受光', 0, 4],
            ['rimPower', '轮廓光功率', 0, 8],
            ['decalIntensityScale', '装饰倍率', 0, 10],
          ] as const).map(([key, label, min, max]) => (
            <div key={key} className="space-y-1">
              <Label className="text-xs">{label}</Label>
              <Slider min={min} max={max} step={0.05} value={[lighting[key]]}
                onValueChange={([next]) => motionStore.patchStudioLighting({ [key]: next })} />
            </div>
          ))}
        </div>
        <div className="flex flex-wrap gap-2">
          <Button size="sm" disabled={!supports('studio.set')} onClick={() => void motionStore.applyStudioLighting()}>应用布光</Button>
          <Button variant="outline" size="sm" disabled={!supports('studio.set')} onClick={() => void motionStore.disableStudioLighting()}>停用布光</Button>
        </div>
        {studio.lightingNotice && <p className="text-sm text-muted-foreground">{studio.lightingNotice}</p>}
      </div>
      <Separator />
      <div className="space-y-3">
        <div className="flex flex-wrap items-center gap-4">
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={video.enabled} onCheckedChange={(checked) => motionStore.patchStudioVideo({ enabled: checked })} />
            启用视频
          </label>
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={video.paused} onCheckedChange={(checked) => motionStore.patchStudioVideo({ paused: checked })} />
            暂停
          </label>
          <label className="flex items-center gap-2 text-sm">
            <Switch checked={video.loop} onCheckedChange={(checked) => motionStore.patchStudioVideo({ loop: checked })} />
            循环
          </label>
        </div>
        <div className="flex gap-2">
          <Input className="h-8 flex-1" placeholder="本地视频绝对路径" value={videoPath}
            onChange={(event) => motionStore.setStudioVideoPath(event.target.value)} />
          <Button variant="outline" size="sm" disabled={busy || !supports('dialog.pickFile')} onClick={() => void motionStore.browseFiles('video')}>
            浏览…
          </Button>
        </div>
        <div className="grid gap-3 sm:grid-cols-2">
          {([
            ['distance', '幕布距离', 0.25, 100],
            ['width', '幕布宽度', 0.1, 100],
            ['offsetX', '水平偏移', -50, 50],
            ['offsetY', '垂直偏移', -50, 50],
          ] as const).map(([key, label, min, max]) => (
            <div key={key} className="space-y-1">
              <Label className="text-xs">{label}</Label>
              <Slider min={min} max={max} step={0.01} value={[video[key]]}
                onValueChange={([next]) => motionStore.patchStudioVideo({ [key]: next })} />
            </div>
          ))}
        </div>
        {motionStore.studioVideoError() && <p className="text-sm text-destructive">{motionStore.studioVideoError()}</p>}
        <div className="flex flex-wrap gap-2">
          <Button size="sm" disabled={!supports('studio.set') || !!motionStore.studioVideoError()} onClick={() => void motionStore.applyStudioVideo()}>
            应用视频
          </Button>
          <Button variant="outline" size="sm" disabled={!supports('studio.set')} onClick={() => void motionStore.disableStudioVideo()}>
            停用视频
          </Button>
          <Button variant="outline" size="sm" disabled={!supports('studio.reset')} onClick={() => void motionStore.resetStudioSettings()}>
            恢复默认值
          </Button>
        </div>
        {studio.videoNotice && <p className="text-sm text-muted-foreground">{studio.videoNotice}</p>}
      </div>
    </div>
  )
}

export function DeveloperPage() {
  return (
    <Tabs defaultValue="motion" className="mx-auto max-w-5xl space-y-3" data-testid="developer-page">
      <TabsList className="h-auto flex-wrap">
        <TabsTrigger value="motion">动作</TabsTrigger>
        <TabsTrigger value="telemetry">遥测与诊断</TabsTrigger>
        <TabsTrigger value="sole">鞋底测量 v2</TabsTrigger>
        <TabsTrigger value="pitch">Pitch Demo [Experimental]</TabsTrigger>
        <TabsTrigger value="replacement">摄影舞台替换</TabsTrigger>
        <TabsTrigger value="studio">布光与视频</TabsTrigger>
      </TabsList>
      <TabsContent value="motion" className="mt-3"><ExperimentsTab /></TabsContent>
      <TabsContent value="telemetry" className="mt-3"><TelemetryTab /></TabsContent>
      <TabsContent value="sole" className="mt-3"><SoleTab /></TabsContent>
      <TabsContent value="pitch" className="mt-3"><SolePitchTab /></TabsContent>
      <TabsContent value="replacement" className="mt-3">
        <PropertySection title="摄影舞台替换" description="预约下次进入摄影模式时替换的场景 AssetBundle" divided={false} bodyClassName="p-3.5">
          <StageReplacementTab />
        </PropertySection>
      </TabsContent>
      <TabsContent value="studio" className="mt-3">
        <PropertySection title="摄影棚自定义布光与视频幕布" description="三点布光颜色/强度控制及背景本地视频投影" divided={false} bodyClassName="p-3.5">
          <StudioTab />
        </PropertySection>
      </TabsContent>
    </Tabs>
  )
}
