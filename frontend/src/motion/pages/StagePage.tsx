import * as React from 'react'
import {
  RiGroup2Fill,
  RiPaletteFill,
  RiRefreshFill,
  RiSearchFill,
  RiSparkling2Fill,
  RiStackFill,
  RiSunFill,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { cn } from '@/lib/utils'
import { ensembleSlotName } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { CollapsiblePropertySection, PropertyRow, PropertySection } from '../components/PropertyGroup'
import { PendingBadge } from '../components/Unsupported'

const COLOR_PRESETS: Array<{ label: string; rgb: [number, number, number] }> = [
  { label: '纯白', rgb: [1, 1, 1] },
  { label: '中灰', rgb: [0.5, 0.5, 0.5] },
  { label: '纯黑', rgb: [0, 0, 0] },
  { label: '绿幕', rgb: [0, 1, 0] },
  { label: '蓝幕', rgb: [0, 0.35, 0.85] },
]

function rgbToHex(rgb: [number, number, number]): string {
  return `#${rgb.map((channel) => Math.round(Math.min(Math.max(channel, 0), 1) * 255).toString(16).padStart(2, '0')).join('')}`
}

function hexToRgb(hex: string): [number, number, number] {
  const value = hex.replace('#', '')
  return [0, 2, 4].map((offset) => parseInt(value.slice(offset, offset + 2), 16) / 255) as [number, number, number]
}

export function StagePage() {
  const stage = useMotionStore((state) => state.stage)
  const actors = useMotionStore((state) => state.actors)
  const ensemble = useMotionStore((state) => state.ensemble)
  const rawNodes = useMotionStore((state) => state.stageNodes)
  const safeNodes = Array.isArray(rawNodes) ? rawNodes : []
  const supported = useMotionStore((state) => state.capabilities.includes('stage.set'))
  const scanSupported = useMotionStore((state) => state.capabilities.includes('stage.scan'))
  const actorSelectSupported = useMotionStore((state) => state.capabilities.includes('actors.selectTarget'))
  const [scanning, setScanning] = React.useState(false)
  const [nodeFilter, setNodeFilter] = React.useState('')

  const settings = stage.settings
  const sceneBgEnabled = !settings.hideBackground
  const customNodeActive = settings.customNodeControl
  const disabled = !supported
  const selectedActor = actors.actors.find((actor) => actor.selected)

  const triggerScan = React.useCallback(async () => {
    if (!scanSupported || scanning) return
    setScanning(true)
    try {
      await motionStore.scanStageNodes()
    } finally {
      setScanning(false)
    }
  }, [scanSupported, scanning])

  const filteredNodes = React.useMemo(() => {
    const needle = nodeFilter.trim().toLowerCase()
    if (!needle) return safeNodes
    return safeNodes.filter(
      (node) => node.displayName.toLowerCase().includes(needle) || node.name.toLowerCase().includes(needle),
    )
  }, [safeNodes, nodeFilter])

  return (
    <div className="mx-auto max-w-5xl space-y-4" data-testid="stage-page">
      {/* Stage Context & Roster Overview */}
      <div className="grid items-start gap-4 lg:grid-cols-2">
        <PropertySection
          title={
            <span className="inline-flex items-center gap-1.5">
              <RiSparkling2Fill className="size-3.5 text-primary" />
              <span>当前舞台环境</span>
            </span>
          }
          description="实时显示摄影舞台状态、背景渲染模式与实验替换标记"
        >
          <PropertyRow label="当前摄影场景">
            <span className="rounded-[4px] border border-border bg-muted/60 px-2 py-0.5 text-xs text-foreground">
              {stage.sceneName || '默认摄影场景'}
            </span>
          </PropertyRow>
          <PropertyRow label="渲染与背景模式">
            <Badge variant={settings.lightingEffectsEnabled ? 'success' : 'muted'} className="rounded-[4px]">
              {settings.lightingEffectsEnabled ? '光影开启' : '光影关闭'}
            </Badge>
            <Badge variant={sceneBgEnabled ? 'outline' : 'warning'} className="rounded-[4px]">
              {sceneBgEnabled ? '原景模式' : '纯色幕布'}
            </Badge>
          </PropertyRow>
          {(stage.statusNotice || settings.loadReplacementKey !== '') && (
            <div className="space-y-1.5 px-3.5 py-2.5 text-xs">
              {stage.statusNotice && (
                <p className={cn(/失败|错误/.test(stage.statusNotice) ? 'text-destructive' : 'text-muted-foreground')}>
                  舞台状态: {stage.statusNotice}
                </p>
              )}
              {settings.loadReplacementKey !== '' && (
                <p className="rounded border border-warning/35 bg-warning/10 px-2.5 py-1.5 text-warning">
                  已保留实验舞台选择；请在 [开发] 页的“摄影舞台替换”中修改或恢复原生背景。
                </p>
              )}
            </div>
          )}
        </PropertySection>

        <PropertySection
          title={
            <span className="inline-flex items-center gap-1.5">
              <RiGroup2Fill className="size-3.5 text-primary" />
              <span>舞台角色阵容</span>
            </span>
          }
          description={ensemble.active ? '当前处于多人舞台编排模式' : '切换当前单人舞台驱动角色；支持扩展多人同台演出'}
          actions={
            <Button variant="outline" size="sm" className="h-7 px-2 text-xs" onClick={() => motionStore.setPage('motion')}>
              前往编排
            </Button>
          }
        >
          {ensemble.active ? (
            <div className="grid grid-cols-2 divide-x divide-y divide-border/60 sm:grid-cols-3">
              {ensemble.settings.slots.map((slot, index) => (
                <div
                  key={index}
                  className={cn(
                    'px-3 py-2.5 text-xs',
                    slot.enabled ? 'bg-card' : 'bg-muted/25 opacity-60',
                  )}
                >
                  <div className="font-semibold text-foreground">{ensembleSlotName(index)}</div>
                  <div className="truncate text-muted-foreground">{slot.actorHint || '未绑定'}</div>
                </div>
              ))}
            </div>
          ) : (
            <>
              <PropertyRow label="驱动角色" description={`当前场景共识别 ${actors.actors.length} 名角色`}>
                <Select
                  value={selectedActor ? String(selectedActor.token) : undefined}
                  disabled={actors.actors.length === 0 || !actorSelectSupported}
                  onValueChange={(value) => void motionStore.selectActor(Number(value))}
                >
                  <SelectTrigger className="h-8 w-[180px] text-xs" aria-label="舞台驱动角色">
                    <SelectValue placeholder={actors.targetLabel || '尚未识别角色'} />
                  </SelectTrigger>
                  <SelectContent>
                    {actors.actors.map((actor) => (
                      <SelectItem key={actor.token} value={String(actor.token)}>
                        {actor.label}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </PropertyRow>
              <PropertyRow label="当前绑定状态">
                <span className="rounded-[4px] border border-border bg-muted/60 px-2 py-0.5 text-xs text-muted-foreground">
                  已绑定：{actors.boundLabel || '尚未绑定'}
                </span>
              </PropertyRow>
            </>
          )}
        </PropertySection>
      </div>

      {/* Stage Visual Controls */}
      <PropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiSunFill className="size-3.5 text-primary" />
            <span>舞台视觉控制</span>
          </span>
        }
        description="控制光影节点、场景背景与抠像纯色幕布；背景隐藏状态会自动联动摄像机 Clear 行为"
        badge={!supported ? <PendingBadge /> : undefined}
      >
        {customNodeActive && (
          <div className="bg-warning/10 px-3.5 py-2 text-xs text-warning">
            高级自定义节点已启用，以下自动管理选项暂时锁定。
          </div>
        )}

        <div
          className="flex items-center justify-between gap-3 px-3.5 py-2.5"
          title="控制场景光影节点与角色阴影渲染；关闭场景背景时不会关闭此项"
        >
          <div className="space-y-0.5">
            <Label htmlFor="stage-lighting" className="cursor-pointer text-sm font-medium">
              光影效果
            </Label>
            <span className="block text-xs text-muted-foreground">控制场景灯光节点及角色锚定阴影联动</span>
          </div>
          <Checkbox
            id="stage-lighting"
            checked={settings.lightingEffectsEnabled}
            disabled={disabled || customNodeActive}
            onCheckedChange={(checked) => motionStore.patchStage({ lightingEffectsEnabled: checked === true })}
          />
        </div>

        <div
          className="flex items-center justify-between gap-3 px-3.5 py-2.5"
          title="关闭时自动隐藏场景网格并应用纯色背景"
        >
          <div className="space-y-0.5">
            <Label htmlFor="stage-background" className="cursor-pointer text-sm font-medium">
              场景背景
            </Label>
            <span className="block text-xs text-muted-foreground">关闭后隐藏三维背景并启用纯色抠像幕布</span>
          </div>
          <Checkbox
            id="stage-background"
            checked={sceneBgEnabled}
            disabled={disabled || customNodeActive}
            onCheckedChange={(checked) => {
              const hide = checked !== true
              motionStore.patchStage({
                hideBackground: hide,
                greenScreenActive: hide,
                overrideCameraClear: hide,
              })
            }}
          />
        </div>

        <div className="flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5">
          <div className="flex items-center gap-2">
            <RiPaletteFill className="size-3.5 text-primary" />
            <span className="text-sm font-medium">背景颜色</span>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <input
              type="color"
              aria-label="背景颜色"
              className="h-7 w-10 cursor-pointer rounded border border-input bg-card disabled:cursor-not-allowed disabled:opacity-50"
              value={rgbToHex([settings.greenScreenColor[0], settings.greenScreenColor[1], settings.greenScreenColor[2]])}
              disabled={disabled || customNodeActive || sceneBgEnabled}
              onChange={(event) => {
                const rgb = hexToRgb(event.target.value)
                motionStore.patchStage({ greenScreenColor: [rgb[0], rgb[1], rgb[2], 1] })
              }}
            />
            <div className="flex flex-wrap items-center gap-1">
              <span className="mr-0.5 text-xs text-muted-foreground">预置:</span>
              {COLOR_PRESETS.map((preset) => (
                <button
                  key={preset.label}
                  type="button"
                  title={`${preset.label} (${preset.rgb.join(', ')})`}
                  aria-label={`背景预置 ${preset.label}`}
                  className="flex items-center gap-1.5 rounded-[4px] border border-border bg-background/70 px-2 py-1 text-xs transition-colors hover:border-primary/50 disabled:cursor-not-allowed disabled:opacity-50"
                  disabled={disabled || customNodeActive || sceneBgEnabled}
                  onClick={() =>
                    motionStore.patchStage({ greenScreenColor: [preset.rgb[0], preset.rgb[1], preset.rgb[2], 1] })
                  }
                >
                  <span className="size-3 rounded-[2px] border border-black/20" style={{ backgroundColor: rgbToHex(preset.rgb) }} />
                  <span>{preset.label}</span>
                </button>
              ))}
            </div>
          </div>
        </div>
      </PropertySection>

      {/* Advanced Scene Nodes */}
      <CollapsiblePropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiStackFill className="size-3.5 text-primary" />
            <span>高级场景编辑</span>
          </span>
        }
        description="手工接管场景层级，逐项控制 Mesh、灯光与背景节点可见性"
        badge={!supported ? <PendingBadge /> : undefined}
        summary={safeNodes.length > 0 ? `已扫描 ${safeNodes.length} 个节点` : '未扫描'}
      >
        <div className="space-y-3">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div className="flex items-center gap-2">
              <Checkbox
                id="stage-custom-nodes"
                checked={settings.customNodeControl}
                disabled={disabled}
                onCheckedChange={(checked) => {
                  const enabled = checked === true
                  motionStore.patchStage({ customNodeControl: enabled })
                  if (enabled && safeNodes.length === 0 && scanSupported) void triggerScan()
                }}
              />
              <Label htmlFor="stage-custom-nodes">自定义场景节点 [开关]</Label>
            </div>

            {settings.customNodeControl && (
              <div className="flex flex-wrap items-center gap-2">
                <div className="relative">
                  <RiSearchFill className="pointer-events-none absolute left-2 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground" />
                  <Input
                    className="h-7 w-44 pl-7 text-xs"
                    placeholder="过滤场景节点..."
                    aria-label="过滤场景节点"
                    value={nodeFilter}
                    onChange={(event) => setNodeFilter(event.target.value)}
                  />
                </div>
                <Button
                  variant="outline"
                  size="sm"
                  className="h-7 text-xs"
                  disabled={!scanSupported || scanning}
                  onClick={() => void triggerScan()}
                >
                  <RiRefreshFill className={cn('size-3.5', scanning && 'animate-spin')} />
                  {scanning ? '正在扫描…' : '扫描场景节点'}
                </Button>
                {!scanSupported && <PendingBadge />}
              </div>
            )}
          </div>

          {settings.customNodeControl ? (
            <>
              <p className="text-xs text-warning">
                启用后将停止 GakuMotion 对场景节点的自动管理。仅建议在需要手工定制场景时使用。
              </p>
              <div className="max-h-72 divide-y divide-border/40 overflow-y-auto rounded-md border bg-background/60 p-1.5" data-testid="stage-node-tree">
                {filteredNodes.length === 0 ? (
                  <p className="py-4 text-center text-xs text-muted-foreground">尚未扫描或没有候选节点。</p>
                ) : (
                  filteredNodes.map((node) => {
                    const parentInactive = safeNodes.some(
                      (candidate) => candidate.index === node.parentIndex && !candidate.currentlyActive,
                    )
                    return (
                      <div
                        key={node.index}
                        className="flex items-center gap-2 rounded px-2 py-1 hover:bg-accent/45"
                        style={{ paddingLeft: `${8 + node.depth * 14}px` }}
                      >
                        <Checkbox
                          checked={node.currentlyActive}
                          disabled={node.isColliderLocked || parentInactive || !scanSupported}
                          onCheckedChange={(checked) => void motionStore.toggleStageNode(node.index, checked === true)}
                        />
                        <span className="text-xs text-foreground">{node.displayName}</span>
                        {node.isLightingRoot && <Badge variant="secondary" className="rounded-[4px]">光影</Badge>}
                        {node.isIdentifiedBackground && <Badge variant="secondary" className="rounded-[4px]">背景</Badge>}
                        {node.isColliderLocked && <Badge variant="outline" className="rounded-[4px]">碰撞锁定</Badge>}
                      </div>
                    )
                  })
                )}
              </div>
            </>
          ) : (
            <p className="text-xs text-muted-foreground">
              开启自定义场景节点后，可手工逐项勾选/隐藏场景里的特定 Mesh 或物件。
            </p>
          )}
        </div>
      </CollapsiblePropertySection>
    </div>
  )
}
