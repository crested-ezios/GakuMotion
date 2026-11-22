import * as React from 'react'
import {
  RiAlertFill,
  RiBodyScanFill,
  RiDiscFill,
  RiEmotionHappyFill,
  RiGroup2Fill,
  RiMapPin2Fill,
  RiPlayList2Fill,
  RiRefreshFill,
  RiRestartFill,
  RiSave3Fill,
  RiUserUnfollowLine,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { cn } from '@/lib/utils'
import type { FacialWeights, StageSlotOffset } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { formatMmSs } from '../time'
import { ActorSlotStrip } from '../components/ActorSlotStrip'
import { BoneSection } from '../components/BoneSection'
import { DragFloat } from '../components/DragFloat'
import { Vector3Field } from '../components/NumberField'
import { CollapsiblePropertySection, PropertyRow, PropertySection } from '../components/PropertyGroup'
import { RotaryKnobCard } from '../components/RotaryKnobCard'
import { PendingBadge } from '../components/Unsupported'

const FACIAL_KNOBS: Array<{ key: keyof FacialWeights; label: string; hint: string }> = [
  { key: 'overall', label: '整体', hint: '总倍率' },
  { key: 'brow', label: '眉毛', hint: '眉峰与皱眉' },
  { key: 'eye', label: '眼部', hint: '眨眼开合' },
  { key: 'mouth', label: '嘴部', hint: '口型发声' },
]

export function MotionPage() {
  const formation = useMotionStore((state) => state.formation)
  const facial = useMotionStore((state) => state.facial)
  const player = useMotionStore((state) => state.player)
  const loaded = useMotionStore((state) => state.loaded)
  const libraryEntries = useMotionStore((state) => state.library.entries)
  const boneCount = useMotionStore((state) => state.bone.overrides.length)
  const capabilities = useMotionStore((state) => state.capabilities)
  const supports = (method: string) => capabilities.includes(method)

  const loadedEntry = React.useMemo(
    () => libraryEntries.find((entry) => entry.id === loaded.motionKey) ?? null,
    [libraryEntries, loaded.motionKey],
  )

  const selectedSlot = formation.slots[formation.selectedSlotIndex] ?? formation.slots[0] ?? null
  const [facialDraft, setFacialDraft] = React.useState<FacialWeights | null>(null)
  const [offsetDraft, setOffsetDraft] = React.useState<StageSlotOffset | null>(null)

  React.useEffect(() => {
    setFacialDraft(null)
  }, [
    selectedSlot?.slotIndex,
    selectedSlot?.facialWeights.overall,
    selectedSlot?.facialWeights.brow,
    selectedSlot?.facialWeights.eye,
    selectedSlot?.facialWeights.mouth,
  ])

  React.useEffect(() => {
    setOffsetDraft(null)
  }, [
    selectedSlot?.slotIndex,
    selectedSlot?.offset.x,
    selectedSlot?.offset.y,
    selectedSlot?.offset.z,
    selectedSlot?.offset.yawDegrees,
  ])

  const activeFacialWeights = facialDraft ?? (selectedSlot?.occupied ? selectedSlot.facialWeights : facial.weights)
  const activeOffset: StageSlotOffset = offsetDraft ?? (selectedSlot?.offset ?? { x: 0, y: 0, z: 0, yawDegrees: 0 })

  const updateOffset = (next: StageSlotOffset) => {
    if (!selectedSlot) return
    setOffsetDraft(next)
    void motionStore.setFormationSlotOffset(selectedSlot.slotIndex, next)
  }

  return (
    <div className="mx-auto max-w-5xl space-y-4" data-testid="motion-page">
      {/* Top: Interactive Actor & Stage Slot Strip */}
      <PropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiGroup2Fill className="size-3.5 text-primary" />
            <span>舞台站位与角色编排</span>
          </span>
        }
        description="点击槽位选中并配置该位置角色的偏移、表情与骨骼；拖拽角色卡片可在槽位间互换位置。"
        badge={!supports('formation.selectSlot') ? <PendingBadge /> : undefined}
        actions={
          <span className="rounded-[4px] border border-border bg-muted/60 px-2 py-0.5 text-xs tabular-nums text-muted-foreground">
            {formation.slotCount} 个站位
          </span>
        }
        divided={false}
        bodyClassName="px-3.5 pb-3 pt-1"
      >
        <ActorSlotStrip />
      </PropertySection>

      {/* Compact Active Track Strip */}
      <PropertySection divided={false} bodyClassName="px-3.5 py-3">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div className="flex min-w-0 items-center gap-3">
            <div
              aria-hidden="true"
              className={cn(
                'flex size-9 shrink-0 items-center justify-center rounded-md border',
                loadedEntry || loaded.motionKey
                  ? 'border-primary/30 bg-primary/10 text-primary'
                  : 'border-border bg-muted/60 text-muted-foreground',
              )}
            >
              <RiDiscFill className="size-4" />
            </div>
            <div className="min-w-0 space-y-0.5">
              <div className="flex flex-wrap items-center gap-2">
                <h1 className="truncate text-lg font-semibold text-foreground">
                  {loadedEntry?.title ?? (loaded.motionKey ? player.title : '尚未加载动作素材')}
                </h1>
                {loadedEntry?.isGroup && (
                  <Badge variant="group" className="shrink-0 rounded px-1.5 py-0 text-[10px] leading-4">
                    团曲
                  </Badge>
                )}
                {loadedEntry && (
                  <span className="rounded-[4px] border border-border bg-muted/60 px-2 py-0.5 text-xs tabular-nums text-muted-foreground">
                    {formatMmSs(loadedEntry.durationSeconds)} · {loadedEntry.totalFrames} 帧 · {Number(player.fps.toPrecision(4))} fps
                  </span>
                )}
              </div>
              {loadedEntry ? (
                <div className="flex flex-wrap items-center gap-1.5 text-xs text-muted-foreground">
                  <span className="truncate">目录：{loadedEntry.directory}</span>
                  {loadedEntry.hasMotion && <span className="rounded-[4px] border border-border bg-muted/50 px-1.5 py-0 text-micro">动作</span>}
                  {loadedEntry.hasFacial && <span className="rounded-[4px] border border-border bg-muted/50 px-1.5 py-0 text-micro">表情</span>}
                  {loadedEntry.hasCamera && <span className="rounded-[4px] border border-border bg-muted/50 px-1.5 py-0 text-micro">镜头</span>}
                  {loadedEntry.hasAudio && <span className="rounded-[4px] border border-border bg-muted/50 px-1.5 py-0 text-micro">音乐</span>}
                </div>
              ) : (
                <p className="text-xs text-muted-foreground">
                  从曲库加载或播放演出后，可在此针对每个位置单独调整站位偏移、表情幅度与骨骼覆盖。
                </p>
              )}
            </div>
          </div>

          <div className="flex shrink-0 flex-wrap items-center gap-1.5">
            {loaded.motionKey !== '' && (
              <Button
                variant="outline"
                size="sm"
                className="h-8 text-xs"
                disabled={loaded.sourceMissing || !supports('player.reload')}
                onClick={() => void motionStore.reloadMotion()}
              >
                <RiRefreshFill className="size-3.5" />
                重载当前动作
              </Button>
            )}
            <Button variant="outline" size="sm" className="h-8 text-xs" onClick={() => motionStore.setPage('library')}>
              <RiPlayList2Fill className="size-3.5" />
              前往曲库
            </Button>
          </div>
        </div>

        {(loaded.sourceMissing || loaded.sourceChanged) && (
          <div className="mt-2.5 flex items-center gap-2 rounded-md border border-warning/40 bg-warning/10 px-2.5 py-1.5 text-xs text-warning">
            <RiAlertFill className="size-3.5 shrink-0" />
            <span>
              {loaded.sourceMissing
                ? '当前曲目的源文件已移除，无法重载；请先恢复磁盘文件。'
                : '检测到曲目源文件已更新，可点击右侧“重载当前动作”应用最新改动。'}
            </span>
          </div>
        )}
      </PropertySection>

      {/* Per-slot Inspector */}
      {!selectedSlot || !selectedSlot.occupied ? (
        <PropertySection divided={false} bodyClassName="flex flex-col items-center justify-center gap-2 px-4 py-10 text-center">
          <div className="flex size-11 items-center justify-center rounded-full border border-border/70 bg-muted/40 text-muted-foreground">
            <RiUserUnfollowLine className="size-5" />
          </div>
          <p className="text-base font-semibold text-foreground">当前位置上无角色</p>
          <p className="max-w-md text-xs text-muted-foreground">
            当前选中的 {selectedSlot ? `${selectedSlot.slotIndex + 1}号位` : '位置'} 尚未分配场景角色；可将上方其他槽位的角色卡片拖拽到此位置进行对位互换。
          </p>
        </PropertySection>
      ) : (
        <>
          <div className="grid items-start gap-4 lg:grid-cols-2">
            {/* Selected Slot Position Offset & Overflow Source Reuse */}
            <PropertySection
              title={
                <span className="inline-flex items-center gap-1.5">
                  <RiMapPin2Fill className="size-3.5 text-primary" />
                  <span>初始角色位置偏移</span>
                </span>
              }
              description="相对舞台基准点的世界坐标偏移（米）与朝向旋转角度（°）"
              badge={!supports('formation.setSlotOffset') ? <PendingBadge /> : undefined}
            >
              {formation.isGroupSong && selectedSlot.isOverflow && (
                <PropertyRow label="动作来源复用" description="超出团曲编制时选择复用的动作与表情轨道">
                  <Select
                    items={Object.fromEntries(
                      Array.from({ length: Math.max(1, formation.trackPositionCount) }, (_, idx) => [
                        String(idx),
                        `${idx + 1}号位`,
                      ]),
                    )}
                    value={String(selectedSlot.sourceSlotIndex)}
                    disabled={!supports('formation.setSlotSource')}
                    onValueChange={(value) => void motionStore.setFormationSlotSource(selectedSlot.slotIndex, Number(value))}
                  >
                    <SelectTrigger className="h-8 w-[140px] text-xs" aria-label="动作来源复用槽位">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      {Array.from({ length: Math.max(1, formation.trackPositionCount) }, (_, idx) => (
                        <SelectItem key={idx} value={String(idx)}>
                          {idx + 1}号位
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </PropertyRow>
              )}

              <div className="space-y-2.5 px-3.5 py-3">
                <div className="flex items-center justify-between gap-2 text-xs text-muted-foreground">
                  <span>世界坐标偏移 (米)</span>
                  <span>多角色加载时按 1 号位锚点自动对齐</span>
                </div>

                <Vector3Field
                  labelPrefix="初始位置"
                  value={{ x: activeOffset.x, y: activeOffset.y, z: activeOffset.z }}
                  min={-10}
                  max={10}
                  step={0.01}
                  disabled={!supports('formation.setSlotOffset')}
                  onChange={(pos) => updateOffset({ ...activeOffset, ...pos })}
                  onCommit={(pos) => updateOffset({ ...activeOffset, ...pos })}
                />

                <div className="space-y-1 pt-0.5">
                  <Label className="text-xs text-muted-foreground">旋转角度 Yaw (°)</Label>
                  <DragFloat
                    ariaLabel="槽位朝向旋转 Yaw"
                    className="w-full"
                    inputClassName="h-8 w-full text-xs"
                    value={activeOffset.yawDegrees}
                    min={-360}
                    max={360}
                    step={1}
                    decimals={1}
                    unit="°"
                    disabled={!supports('formation.setSlotOffset')}
                    onChange={(yawDegrees) => updateOffset({ ...activeOffset, yawDegrees })}
                    onCommit={(yawDegrees) => updateOffset({ ...activeOffset, yawDegrees })}
                  />
                </div>

                <div className="flex flex-wrap items-center gap-1.5 pt-0.5">
                  <Button
                    variant="outline"
                    size="sm"
                    className="h-7 px-2.5 text-xs"
                    disabled={!supports('formation.resetSlotOffset')}
                    onClick={() => void motionStore.resetFormationSlotOffset(selectedSlot.slotIndex)}
                  >
                    <RiRestartFill className="size-3.5" />
                    重置位置偏移
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    className="h-7 px-2.5 text-xs"
                    disabled={!supports('formation.saveSlotOffset')}
                    onClick={() => void motionStore.saveFormationSlotOffset(selectedSlot.slotIndex)}
                  >
                    <RiSave3Fill className="size-3.5" />
                    保存当前曲目位置
                  </Button>
                </div>

                {formation.notice && (
                  <p className={cn('text-xs', /失败|无效/.test(formation.notice) ? 'text-destructive' : 'text-primary')}>
                    {formation.notice}
                  </p>
                )}
              </div>
            </PropertySection>

            {/* Facial Weight 2x2 Rotary Knobs */}
            <PropertySection
              title={
                <span className="inline-flex items-center gap-1.5">
                  <RiEmotionHappyFill className="size-3.5 text-primary" />
                  <span>表情幅度</span>
                </span>
              }
              description="支持旋钮拖拽与右下角百分比手动填写 (0%–200%)；仅作用于当前选中角色。"
              badge={!supports('facial.setWeights') ? <PendingBadge /> : undefined}
              actions={
                <>
                  <Button
                    variant="outline"
                    size="sm"
                    className="h-7 px-2 text-xs"
                    disabled={!supports('facial.setWeights')}
                    onClick={() => {
                      const defaults = { overall: 1, brow: 1, eye: 1, mouth: 1 }
                      setFacialDraft(defaults)
                      motionStore.resetFacialWeights()
                    }}
                  >
                    重置表情幅度
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    className="h-7 px-2 text-xs"
                    disabled={!supports('facial.save')}
                    onClick={() => void motionStore.saveFacialWeights()}
                  >
                    保存表情幅度
                  </Button>
                </>
              }
              divided={false}
              bodyClassName="p-3"
            >
              <div className="grid grid-cols-2 gap-2.5">
                {FACIAL_KNOBS.map((knob) => (
                  <RotaryKnobCard
                    key={`${selectedSlot.slotIndex}-${knob.key}`}
                    label={knob.label}
                    hint={knob.hint}
                    value={activeFacialWeights[knob.key]}
                    disabled={!supports('facial.setWeights')}
                    onChange={(next) => {
                      const merged = { ...activeFacialWeights, [knob.key]: next }
                      setFacialDraft(merged)
                      motionStore.setFacialWeights(merged)
                    }}
                    onCommit={(next) => {
                      const merged = { ...activeFacialWeights, [knob.key]: next }
                      setFacialDraft(merged)
                      motionStore.commitFacialWeights(merged)
                    }}
                  />
                ))}
              </div>
              {facial.notice && <p className="mt-2 text-xs text-primary">{facial.notice}</p>}
            </PropertySection>
          </div>

          {/* Advanced Bone Tuning */}
          <CollapsiblePropertySection
            title={
              <span className="inline-flex items-center gap-1.5">
                <RiBodyScanFill className="size-3.5 text-primary" />
                <span>高级动作设置</span>
              </span>
            }
            description="51 块人形骨骼本地/世界坐标角度微调、左右对称联动与单曲/全局持久化"
            badge={!supports('bone.set') ? <PendingBadge /> : undefined}
            summary={boneCount > 0 ? `${boneCount} 项活跃覆盖` : '暂无覆盖'}
          >
            <BoneSection />
          </CollapsiblePropertySection>
        </>
      )}
    </div>
  )
}
