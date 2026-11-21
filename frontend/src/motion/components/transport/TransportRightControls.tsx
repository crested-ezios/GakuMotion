import * as React from 'react'
import {
  RiCameraFill,
  RiCheckFill,
  RiEqualizerFill,
  RiMusic2Fill,
  RiSpeedUpFill,
  RiVolumeDownFill,
  RiVolumeMuteFill,
  RiVolumeUpFill,
} from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { Popover, PopoverContent, PopoverTrigger } from '@/components/ui/popover'
import { Slider } from '@/components/ui/slider'
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/ui/tooltip'
import { cn } from '@/lib/utils'
import { CAMERA_MODES, PLAYBACK_SPEEDS, type AudioEffectState } from '../../protocol'
import { motionStore } from '../../store'
import { PrecisionControlPanel } from '../PrecisionControlPanel'
import { PendingBadge } from '../Unsupported'

function IconTooltip({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <Tooltip>
      <TooltipTrigger asChild>{children}</TooltipTrigger>
      <TooltipContent side="top">{label}</TooltipContent>
    </Tooltip>
  )
}

export function TransportRightControls({
  audioEffect,
  cameraModeIndex,
  cameraPending,
  speed,
  volume,
  musicVolumeSupport,
  musicVolumeNotice,
  precisionOpen,
  precisionPinned,
  navigationError,
  supports,
}: {
  audioEffect: AudioEffectState
  cameraModeIndex: number
  cameraPending: boolean
  speed: number
  volume: number
  musicVolumeSupport: number
  musicVolumeNotice: string
  precisionOpen: boolean
  precisionPinned: boolean
  navigationError: string | null
  supports: (method: string) => boolean
}) {
  const [audioFxOpen, setAudioFxOpen] = React.useState(false)
  const [cameraOpen, setCameraOpen] = React.useState(false)
  const [speedOpen, setSpeedOpen] = React.useState(false)
  const [volumeOpen, setVolumeOpen] = React.useState(false)
  const lastVolumeRef = React.useRef<number>(100)

  const volumePercent = Math.round(volume * 100)
  React.useEffect(() => {
    if (volumePercent > 0) lastVolumeRef.current = volumePercent
  }, [volumePercent])

  const activeEffectPreset = React.useMemo(
    () => audioEffect.presets.find((p) => p.id === audioEffect.activeId) ?? audioEffect.presets[0] ?? { id: 'flat', name: '原声', category: 'flat', description: '' },
    [audioEffect.presets, audioEffect.activeId],
  )
  const effectModified = audioEffect.activeId !== 'flat'
  const showVolumeNotice = musicVolumeSupport < 0 || musicVolumeNotice !== ''
  const VolumeIcon = volumePercent === 0 ? RiVolumeMuteFill : volumePercent < 50 ? RiVolumeDownFill : RiVolumeUpFill
  const speedModified = Math.abs(speed - 1) > 0.01

  return (
    <div className="flex min-w-0 flex-1 items-center justify-end gap-2">
      {/* Audio Effect Preset Badge */}
      <div className="flex items-center gap-1">
        <Popover open={audioFxOpen} onOpenChange={setAudioFxOpen}>
          <PopoverTrigger asChild>
            <button
              type="button"
              aria-label="音效预设"
              data-testid="audio-effect-badge"
              disabled={!supports('audioEffect.set')}
              title={audioEffect.available ? `音效：${activeEffectPreset.name}` : '未加载 GakuAudio 音效插件'}
              className={cn(
                'inline-flex h-7 items-center gap-1.5 whitespace-nowrap rounded-[4px] border px-2.5 text-xs font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring disabled:pointer-events-none disabled:opacity-50',
                effectModified && audioEffect.available
                  ? 'border-primary/40 bg-primary/10 text-primary hover:border-primary/60'
                  : 'border-border bg-background/70 text-muted-foreground hover:border-foreground/30 hover:text-foreground',
              )}
            >
              <RiMusic2Fill className="size-3.5 shrink-0" />
              <span>{activeEffectPreset.name}</span>
            </button>
          </PopoverTrigger>
          <PopoverContent side="top" align="end" className="w-60 p-1">
            <div className="flex items-center justify-between px-2.5 py-1.5 text-micro font-medium text-muted-foreground">
              <span>音效环境预设 (GakuAudio)</span>
              {!audioEffect.available && <span className="text-amber-500">未加载</span>}
            </div>
            {audioEffect.presets.map((preset) => {
              const active = audioEffect.activeId === preset.id
              return (
                <button
                  key={preset.id}
                  type="button"
                  role="menuitemradio"
                  aria-checked={active}
                  disabled={!audioEffect.available}
                  onClick={() => {
                    setAudioFxOpen(false)
                    void motionStore.setAudioEffect(preset.id)
                  }}
                  className={cn(
                    'flex w-full items-center justify-between gap-2 rounded px-2.5 py-1.5 text-left text-sm transition-colors hover:bg-accent disabled:pointer-events-none disabled:opacity-50',
                    active ? 'font-semibold text-primary' : 'text-foreground',
                  )}
                >
                  <div className="min-w-0">
                    <div className="truncate">{preset.name}</div>
                    {preset.description && (
                      <div className="truncate text-micro text-muted-foreground">{preset.description}</div>
                    )}
                  </div>
                  {active && <RiCheckFill className="size-3.5 shrink-0 text-primary" />}
                </button>
              )
            })}
          </PopoverContent>
        </Popover>
      </div>

      {/* Compact Camera Mode Badge (NetEase right-side badge aesthetic) */}
      <div className="flex items-center gap-1">
        <Popover open={cameraOpen} onOpenChange={setCameraOpen}>
          <PopoverTrigger asChild>
            <button
              type="button"
              aria-label="镜头快捷模式"
              data-testid="camera-mode-badge"
              disabled={!supports('camera.set')}
              className="inline-flex h-7 items-center gap-1.5 whitespace-nowrap rounded-[4px] border border-border bg-background/70 px-2.5 text-xs font-medium text-muted-foreground transition-colors hover:border-foreground/30 hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring disabled:pointer-events-none disabled:opacity-50"
            >
              <RiCameraFill className="size-3.5 shrink-0" />
              <span>{CAMERA_MODES[cameraModeIndex] ?? 'VMD'}</span>
            </button>
          </PopoverTrigger>
          <PopoverContent side="top" align="end" className="w-40 p-1">
            <div className="px-2.5 py-1.5 text-micro font-medium text-muted-foreground">镜头模式</div>
            {CAMERA_MODES.map((label, index) => {
              const active = cameraModeIndex === index
              return (
                <button
                  key={label}
                  type="button"
                  role="menuitemradio"
                  aria-checked={active}
                  onClick={() => {
                    setCameraOpen(false)
                    void motionStore.setCameraQuickMode(index)
                  }}
                  className={cn(
                    'flex w-full items-center justify-between rounded px-2.5 py-1.5 text-left text-sm transition-colors hover:bg-accent',
                    active ? 'font-semibold text-primary' : 'text-foreground',
                  )}
                >
                  <span>{label}</span>
                  {active && <RiCheckFill className="size-3.5 text-primary" />}
                </button>
              )
            })}
          </PopoverContent>
        </Popover>
        {cameraPending && (
          <span className="size-2 animate-pulse rounded-full bg-primary" title="正在应用镜头设置" />
        )}
        {!supports('camera.set') && <PendingBadge />}
      </div>

      {/* Speed Icon Only (Tooltip shows current value) */}
      <Popover open={speedOpen} onOpenChange={setSpeedOpen}>
        <IconTooltip label={`播放倍速: ${speed.toFixed(2)}x`}>
          <PopoverTrigger asChild>
            <Button
              type="button"
              variant={speedModified ? 'secondary' : 'ghost'}
              size="icon"
              aria-label="播放速度"
              className={cn(
                'size-8 rounded-md',
                speedModified ? 'text-primary' : 'text-muted-foreground hover:text-foreground',
              )}
            >
              <RiSpeedUpFill className="size-[18px]" />
            </Button>
          </PopoverTrigger>
        </IconTooltip>
        <PopoverContent side="top" align="end" className="w-60 space-y-2.5 p-3">
          <div className="flex items-center justify-between text-sm">
            <span className="font-semibold">播放倍速</span>
            <span className="font-semibold tabular-nums text-primary">{speed.toFixed(2)}x</span>
          </div>
          <div className="grid grid-cols-3 gap-1.5">
            {PLAYBACK_SPEEDS.map((presetSpeed) => {
              const active = Math.abs(speed - presetSpeed) < 0.01
              return (
                <button
                  key={presetSpeed}
                  type="button"
                  disabled={!supports('player.setSpeed')}
                  onClick={() => void motionStore.setSpeed(presetSpeed)}
                  className={cn(
                    'rounded border px-2 py-1 text-xs tabular-nums transition-colors disabled:opacity-50',
                    active
                      ? 'border-primary bg-primary text-primary-foreground'
                      : 'border-border bg-background/60 text-muted-foreground hover:text-foreground',
                  )}
                >
                  {presetSpeed.toFixed(1)}x
                </button>
              )
            })}
          </div>
          <div className="flex items-center gap-2 pt-0.5">
            <Slider
              aria-label="快捷倍速"
              min={0.1}
              max={3}
              step={0.05}
              disabled={!supports('player.setSpeed')}
              value={[speed]}
              onValueChange={([value]) => void motionStore.setSpeed(Math.round(value * 100) / 100)}
            />
          </div>
        </PopoverContent>
      </Popover>

      {/* Volume Icon Only + Vertical Volume Popover */}
      <Popover open={volumeOpen} onOpenChange={setVolumeOpen}>
        <IconTooltip label={`音乐音量: ${volumePercent}%`}>
          <PopoverTrigger asChild>
            <Button
              type="button"
              variant={showVolumeNotice ? 'outline' : 'ghost'}
              size="icon"
              aria-label="音量控制"
              className={cn(
                'size-8 rounded-md',
                showVolumeNotice ? 'text-warning' : 'text-muted-foreground hover:text-foreground',
              )}
            >
              <VolumeIcon className="size-[18px]" />
            </Button>
          </PopoverTrigger>
        </IconTooltip>
        <PopoverContent
          side="top"
          align="center"
          className={cn(
            'flex flex-col items-center gap-2 p-2.5',
            showVolumeNotice ? 'w-44' : 'w-14',
          )}
        >
          <div className="flex h-28 w-full items-center justify-center py-1">
            <Slider
              orientation="vertical"
              aria-label="音乐音量"
              min={0}
              max={100}
              step={1}
              disabled={!supports('player.setVolume')}
              value={[volumePercent]}
              onValueChange={([value]) => motionStore.setVolume(value)}
              onValueCommit={([value]) => motionStore.commitVolume(value)}
            />
          </div>
          <span className="text-xs font-medium tabular-nums text-muted-foreground">
            {volumePercent}%
          </span>
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-8 shrink-0 rounded-md"
            aria-label={volumePercent === 0 ? '取消静音' : '静音'}
            disabled={!supports('player.setVolume')}
            onClick={() => {
              const next = volumePercent === 0 ? lastVolumeRef.current || 100 : 0
              motionStore.commitVolume(next)
            }}
          >
            <VolumeIcon className="size-4" />
          </Button>
          {showVolumeNotice && (
            <p className="border-t border-border/60 pt-1.5 text-center text-xs leading-snug text-warning">
              {musicVolumeSupport < 0
                ? '当前音乐后端不支持独立音量，音乐仍以原音量播放。'
                : musicVolumeNotice}
            </p>
          )}
        </PopoverContent>
      </Popover>

      {/* Precision Control Drawer Trigger */}
      <Popover open={precisionOpen} onOpenChange={(next) => motionStore.setPrecisionPanelOpen(next)}>
        <PopoverTrigger asChild>
          <Button
            type="button"
            variant={precisionOpen ? 'default' : 'outline'}
            size="icon"
            className="relative size-8 rounded-md"
            aria-label="精细控制"
            title="精细控制与专业定位面板"
            data-testid="precision-control-trigger"
          >
            <RiEqualizerFill className="size-4" />
            {(precisionPinned || navigationError) && !precisionOpen && (
              <span
                aria-hidden="true"
                className={cn(
                  'absolute -right-0.5 -top-0.5 size-2 rounded-full',
                  navigationError ? 'bg-destructive' : 'bg-primary',
                )}
              />
            )}
          </Button>
        </PopoverTrigger>
        <PopoverContent side="top" align="end" pinned={precisionPinned} className="p-3.5">
          <PrecisionControlPanel onClose={() => motionStore.setPrecisionPanelOpen(false)} />
        </PopoverContent>
      </Popover>
    </div>
  )
}
