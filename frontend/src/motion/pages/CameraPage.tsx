import {
  RiCameraFill,
  RiCompass3Fill,
  RiEqualizerFill,
  RiEyeFill,
  RiGamepadFill,
  RiRestartFill,
  RiSave3Fill,
  RiVideoFill,
  type RemixiconComponentType,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Slider } from '@/components/ui/slider'
import { cn } from '@/lib/utils'
import { CAMERA_FOLLOW_TARGETS, CAMERA_MODES, type CameraSettings, type CameraTransform } from '../protocol'
import { cameraControlModeIndex, setCameraControlMode, upgradePerModeTransforms } from '../cameraMode'
import { motionStore, useMotionStore } from '../store'
import { Vector3Field } from '../components/NumberField'
import { CollapsiblePropertySection, PropertyRow, PropertySection } from '../components/PropertyGroup'
import { PendingBadge } from '../components/Unsupported'

const MODE_OPTIONS: Array<{
  index: number
  label: string
  desc: string
  icon: RemixiconComponentType
}> = [
  { index: 0, label: '游戏', desc: '交还游戏原生手动镜头', icon: RiGamepadFill },
  { index: 1, label: 'VMD', desc: '按 VMD / GKCamera 运镜轨迹', icon: RiVideoFill },
  { index: 2, label: '跟随', desc: '跟随角色位置与朝向移动', icon: RiCompass3Fill },
  { index: 3, label: '注视', desc: '固定机位并持续追踪角色', icon: RiEyeFill },
]

function activeTransformKey(selection: number): keyof Pick<CameraSettings, 'transformTrack' | 'transformFollowMove' | 'transformLookAt'> {
  if (selection === 1) return 'transformTrack'
  if (selection === 2) return 'transformFollowMove'
  return 'transformLookAt'
}

export function CameraPage() {
  const camera = useMotionStore((state) => state.camera)
  const draft = useMotionStore((state) => state.cameraDraft)
  const pending = useMotionStore((state) => state.cameraPending)
  const formationSlotCount = useMotionStore((state) => state.formation.slotCount)
  const actorCount = useMotionStore((state) => state.actors.actors.length)
  const supported = useMotionStore((state) => state.capabilities.includes('camera.set'))
  const saveSupported = useMotionStore((state) => state.capabilities.includes('camera.save'))
  const slotCount = Math.max(formationSlotCount, actorCount)

  const selection = cameraControlModeIndex(draft)
  const userMode = selection - 1
  const transformKey = activeTransformKey(selection)
  const activeTransform: CameraTransform = draft[transformKey]

  const patch = (next: Partial<CameraSettings>) => {
    if (!supported) return
    motionStore.patchCameraDraft(next)
  }

  const selectMode = (modeIdx: number) => {
    const next = setCameraControlMode(upgradePerModeTransforms(draft), modeIdx)
    patch(next)
  }

  const patchTransform = (next: CameraTransform) => {
    const upgraded = upgradePerModeTransforms(draft)
    const merged: CameraSettings = { ...upgraded, [transformKey]: next }
    if (selection === 1) merged.vmdOffset = { ...next.position }
    else merged.followOffset = { ...next.position }
    patch(merged)
  }

  const resetTransform = () => {
    const defaults: Record<number, CameraTransform> = {
      1: { position: { x: 0, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0 } },
      2: { position: { x: 0, y: 0.6, z: 3 }, rotation: { x: 0, y: 0, z: 0 } },
      3: { position: { x: 0, y: 0.6, z: 3 }, rotation: { x: 0, y: 0, z: 0 } },
    }
    patchTransform(defaults[selection] ?? defaults[1])
  }

  const disabled = !supported

  return (
    <div className="mx-auto max-w-5xl space-y-4" data-testid="camera-page">
      {/* Camera Mode & Status */}
      <PropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiCameraFill className="size-3.5 text-primary" />
            <span>镜头接管模式</span>
          </span>
        }
        description="选择“游戏”恢复游戏手动镜头，并保留 GakuMotion 镜头参数。"
        badge={
          !supported ? (
            <PendingBadge />
          ) : pending ? (
            <Badge variant="outline" className="rounded-[4px] border-primary/35 bg-primary/10 text-primary">
              正在应用…
            </Badge>
          ) : undefined
        }
        actions={
          <Select
            items={Object.fromEntries(CAMERA_MODES.map((label, index) => [String(index), label]))}
            value={String(selection)}
            disabled={disabled}
            onValueChange={(value) => selectMode(Number(value))}
          >
            <SelectTrigger className="h-8 w-[132px] text-xs" aria-label="镜头模式">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {CAMERA_MODES.map((label, index) => (
                <SelectItem key={label} value={String(index)}>
                  {label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        }
      >
        <div className="grid grid-cols-2 divide-y divide-border/60 sm:grid-cols-4 sm:divide-x sm:divide-y-0">
          {MODE_OPTIONS.map((item) => {
            const Icon = item.icon
            const active = selection === item.index
            return (
              <button
                key={item.index}
                type="button"
                aria-pressed={active}
                disabled={disabled}
                onClick={() => selectMode(item.index)}
                className={cn(
                  'relative flex flex-col items-start gap-1 px-3.5 py-2.5 text-left transition-colors disabled:pointer-events-none disabled:opacity-50',
                  active ? 'bg-primary/[0.08]' : 'hover:bg-accent/45',
                )}
              >
                {active && <span aria-hidden="true" className="absolute inset-x-0 top-0 h-[2px] bg-primary" />}
                <div className="flex w-full items-center justify-between gap-2">
                  <span className={cn('text-base font-semibold', active ? 'text-primary' : 'text-foreground')}>
                    {item.label}
                  </span>
                  <Icon className={cn('size-4', active ? 'text-primary' : 'text-muted-foreground')} />
                </div>
                <span className="text-xs leading-snug text-muted-foreground">{item.desc}</span>
              </button>
            )
          })}
        </div>

        <div className="flex flex-wrap items-center gap-3 bg-muted/35 px-3.5 py-2 text-xs text-muted-foreground">
          <span>
            镜头接管: {camera.owned ? '已生效' : '未接管'} | 运镜轨道: {camera.hasTrack ? '已加载' : '无'}
          </span>
          {!camera.available && <span className="text-warning">当前镜头接口未就绪。</span>}
          {camera.error && <span className="text-destructive">镜头轨道错误: {camera.error}</span>}
          {camera.followTargetError && <span className="text-destructive">跟随目标错误: {camera.followTargetError}</span>}
          {userMode === 0 && !camera.hasTrack && (
            <span className="text-warning">当前曲目没有可用运镜轨道，VMD 模式将保持固定机位。</span>
          )}
        </div>
      </PropertySection>

      {draft.enabled && (
        <div className="grid items-start gap-4 lg:grid-cols-2">
          {/* Position & Rotation Offset */}
          <PropertySection
            title="镜头位置与旋转偏移"
            description="在当前镜头模式的基础上叠加机位位置和朝向微调"
            actions={
              <Button variant="outline" size="sm" className="h-7 px-2 text-xs" disabled={disabled} onClick={resetTransform}>
                <RiRestartFill className="size-3.5" />
                重置位置与旋转偏移
              </Button>
            }
          >
            {userMode === 0 && (
              <div className="space-y-1.5 px-3.5 py-2.5">
                <div className="flex items-center justify-between text-xs">
                  <span className="font-medium text-foreground">VMD 运镜范围</span>
                  <span className="font-semibold tabular-nums text-primary">
                    {draft.vmdRangeScale.toFixed(2)} 倍
                  </span>
                </div>
                <Slider
                  aria-label="VMD 运镜范围"
                  min={0.3}
                  max={1}
                  step={0.01}
                  disabled={disabled}
                  value={[draft.vmdRangeScale]}
                  onValueChange={([value]) => patch({ vmdRangeScale: value })}
                />
                <p className="text-xs leading-relaxed text-muted-foreground">
                  以角色附近为中心缩小轨迹；会改变构图，不能保证避免穿模。点击下方保存镜头设置可保留。
                </p>
              </div>
            )}

            <div className="space-y-1.5 px-3.5 py-2.5">
              <Label className="text-xs text-muted-foreground">位置 (米)</Label>
              <Vector3Field
                labelPrefix="镜头位置"
                value={activeTransform.position}
                min={-50}
                max={50}
                step={0.01}
                disabled={disabled}
                onChange={(position) => patchTransform({ ...activeTransform, position })}
                onCommit={(position) => patchTransform({ ...activeTransform, position })}
              />
            </div>

            <div className="space-y-1.5 px-3.5 py-2.5">
              <Label className="text-xs text-muted-foreground">旋转 (度)</Label>
              <Vector3Field
                labelPrefix="镜头旋转"
                value={activeTransform.rotation}
                min={-180}
                max={180}
                step={0.5}
                decimals={1}
                disabled={disabled}
                onChange={(rotation) => patchTransform({ ...activeTransform, rotation })}
                onCommit={(rotation) => patchTransform({ ...activeTransform, rotation })}
              />
            </div>
          </PropertySection>

          {/* Follow target & rate */}
          {(userMode === 1 || userMode === 2 || draft.followActor) && (
            <PropertySection
              title="追随速率 (Target Rate)"
              description="0 停止追随，0.1 平滑追随，1 即时追随。X / Y / Z 为接管时镜头的右 / 上 / 前方向。"
              actions={
                <Button
                  variant="outline"
                  size="sm"
                  className="h-7 px-2 text-xs"
                  disabled={disabled}
                  onClick={() => patch({ followRate: { x: 0.1, y: 0.1, z: 0.1 } })}
                >
                  <RiRestartFill className="size-3.5" />
                  重置追随速率
                </Button>
              }
            >
              {draft.mode === 2 && (
                <>
                  <PropertyRow label="跟随槽位" description="追踪全员舞台几何中心或指定编号槽位">
                    <Select
                      items={{
                        '-1': '全部',
                        ...Object.fromEntries(
                          Array.from(
                            { length: Math.max(slotCount, (draft.followSlotIndex ?? -1) + 1) },
                            (_, idx) => [String(idx), `${idx + 1}号位`],
                          ),
                        ),
                      }}
                      value={String(draft.followSlotIndex ?? -1)}
                      disabled={disabled}
                      onValueChange={(value) => patch({ followSlotIndex: Number(value) })}
                    >
                      <SelectTrigger className="h-8 w-[140px] text-xs" aria-label="跟随槽位">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        <SelectItem value="-1">全部</SelectItem>
                        {Array.from({ length: slotCount }, (_, idx) => (
                          <SelectItem key={idx} value={String(idx)}>
                            {idx + 1}号位
                          </SelectItem>
                        ))}
                        {(draft.followSlotIndex ?? -1) >= slotCount && (
                          <SelectItem value={String(draft.followSlotIndex)}>
                            {(draft.followSlotIndex ?? 0) + 1}号位
                          </SelectItem>
                        )}
                      </SelectContent>
                    </Select>
                  </PropertyRow>
                  <PropertyRow label="跟随部位" description="决定自动跟踪的角色骨骼焦点">
                    <Select
                      items={Object.fromEntries(CAMERA_FOLLOW_TARGETS.map((label, index) => [String(index), label]))}
                      value={String(draft.followTarget)}
                      disabled={disabled}
                      onValueChange={(value) => patch({ followTarget: Number(value) })}
                    >
                      <SelectTrigger className="h-8 w-[140px] text-xs" aria-label="跟随部位">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        {CAMERA_FOLLOW_TARGETS.map((label, index) => (
                          <SelectItem key={label} value={String(index)}>
                            {label}
                          </SelectItem>
                        ))}
                      </SelectContent>
                    </Select>
                  </PropertyRow>
                </>
              )}

              <div className="px-3.5 py-2.5">
                <Vector3Field
                  labelPrefix="追随速率"
                  value={draft.followRate}
                  min={0}
                  max={1}
                  step={0.01}
                  disabled={disabled}
                  onChange={(followRate) => patch({ followRate })}
                  onCommit={(followRate) => patch({ followRate })}
                />
              </div>
            </PropertySection>
          )}
        </div>
      )}

      {/* Progressive Disclosure: Advanced Camera Settings */}
      <CollapsiblePropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiEqualizerFill className="size-3.5 text-primary" />
            <span>高级镜头设置</span>
          </span>
        }
        description="镜头随角色平移、位移比例、注视偏移、FOV 视场角及场景边界钳制"
        disabled={!draft.enabled}
        bodyClassName="divide-y divide-border/60 p-0"
      >
        {userMode === 0 && (
          <div className="flex items-center justify-between gap-3 px-3.5 py-2.5">
            <Label htmlFor="camera-follow-actor" className="cursor-pointer text-sm font-medium">
              镜头随角色平移
            </Label>
            <Checkbox
              id="camera-follow-actor"
              checked={draft.followActor}
              disabled={disabled}
              onCheckedChange={(checked) => patch({ followActor: checked === true })}
            />
          </div>
        )}

        {(userMode === 1 || userMode === 2 || draft.followActor) && (
          <div className="space-y-2 px-3.5 py-3">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <h4 className="text-sm font-medium">位移比例（旧版跟随幅度）</h4>
              <Button
                variant="outline"
                size="sm"
                className="h-7 px-2 text-xs"
                disabled={disabled}
                onClick={() => patch({ followFactor: { x: 1, y: 1, z: 1 } })}
              >
                恢复完整位移
              </Button>
            </div>
            <p className="text-xs text-muted-foreground">
              缩放最终跟随的角色位移：0 不跟随该轴，0.5 跟一半，1 完整跟随。此项决定目标幅度，追随速率决定追赶快慢。
            </p>
            <Vector3Field
              labelPrefix="位移比例"
              value={draft.followFactor}
              min={0}
              max={1}
              step={0.01}
              disabled={disabled}
              onChange={(followFactor) => patch({ followFactor })}
              onCommit={(followFactor) => patch({ followFactor })}
            />
          </div>
        )}

        {(userMode === 1 || userMode === 2) && (
          <div className="space-y-3 px-3.5 py-3">
            <h4 className="text-sm font-medium">注视目标偏移 (Target Offset，相对所选部位)</h4>
            <Vector3Field
              labelPrefix="注视偏移"
              value={draft.targetOffset}
              min={-5}
              max={5}
              step={0.01}
              disabled={disabled}
              onChange={(targetOffset) => patch({ targetOffset })}
              onCommit={(targetOffset) => patch({ targetOffset })}
            />
            <div className="flex items-center gap-3 pt-1">
              <span className="w-20 shrink-0 text-xs font-medium text-muted-foreground">镜头 FOV</span>
              <Slider
                aria-label="镜头 FOV"
                className="max-w-xs flex-1"
                min={15}
                max={90}
                step={0.5}
                disabled={disabled}
                value={[draft.followFov]}
                onValueChange={([value]) => patch({ followFov: value })}
              />
              <span className="text-xs font-semibold tabular-nums text-primary">
                {draft.followFov.toFixed(1)}°
              </span>
            </div>
          </div>
        )}

        <div className="space-y-2.5 px-3.5 py-3">
          <div className="flex items-center justify-between gap-3">
            <Label htmlFor="camera-limit-scene" className="cursor-pointer text-sm font-medium">
              限制镜头在场景范围内
            </Label>
            <Checkbox
              id="camera-limit-scene"
              checked={draft.limitToScene}
              disabled={disabled}
              onCheckedChange={(checked) => patch({ limitToScene: checked === true })}
            />
          </div>
          {draft.limitToScene && (
            <div className="space-y-2 pt-1">
              <Label className="block text-xs text-muted-foreground">最小边界 (Bounds Min)</Label>
              <Vector3Field
                labelPrefix="最小边界"
                value={draft.boundsMin}
                min={-50}
                max={50}
                step={0.1}
                decimals={1}
                disabled={disabled}
                onCommit={(boundsMin) => patch({ boundsMin })}
              />
              <Label className="block text-xs text-muted-foreground">最大边界 (Bounds Max)</Label>
              <Vector3Field
                labelPrefix="最大边界"
                value={draft.boundsMax}
                min={-50}
                max={50}
                step={0.1}
                decimals={1}
                disabled={disabled}
                onCommit={(boundsMax) => patch({ boundsMax })}
              />
            </div>
          )}
        </div>
      </CollapsiblePropertySection>

      {/* Save Camera Settings */}
      <PropertySection
        divided={false}
        bodyClassName="flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5"
      >
        <div className="space-y-0.5">
          <p className="text-xs text-muted-foreground">
            修改立即生效；保存后下次启动自动载入，作用于全部曲目。
          </p>
          {camera.notice && (
            <p className={cn('text-xs', /失败|无效/.test(camera.notice) ? 'text-destructive' : 'text-primary')}>
              {camera.notice}
            </p>
          )}
        </div>
        <Button variant="default" size="sm" className="h-8 text-xs" disabled={!saveSupported} onClick={() => void motionStore.saveCamera()}>
          <RiSave3Fill className="size-3.5" />
          保存镜头设置
        </Button>
      </PropertySection>
    </div>
  )
}
