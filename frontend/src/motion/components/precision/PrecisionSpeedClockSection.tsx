import {
  RiRefreshFill,
  RiRestartFill,
  RiSkipBackFill,
  RiSkipForwardFill,
  RiSpeedUpFill,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Slider } from '@/components/ui/slider'
import { cn } from '@/lib/utils'
import { PLAYBACK_SPEEDS, type ClockSource, type ClockState, type LoadedState } from '../../protocol'
import { motionStore } from '../../store'

export function PrecisionSpeedClockSection({
  speed,
  clock,
  loaded,
  libraryEmpty,
  supports,
}: {
  speed: number
  clock: ClockState
  loaded: LoadedState
  libraryEmpty: boolean
  supports: (method: string) => boolean
}) {
  const stepSpeed = (delta: number) => {
    const next = Math.min(Math.max(Math.round((speed + delta) * 100) / 100, 0.1), 3)
    void motionStore.setSpeed(next)
  }

  return (
    <>
      {/* Fine playback speed */}
      <div className="space-y-2 border-t border-border/60 pt-3">
        <div className="flex items-center justify-between">
          <span className="flex items-center gap-1.5 font-medium text-foreground">
            <RiSpeedUpFill className="size-3.5 text-primary" />
            精细倍速调节
          </span>
          <span className="text-xs font-semibold tabular-nums text-primary">
            {speed.toFixed(2)}x
          </span>
        </div>

        <div className="flex items-center gap-2">
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-7 px-2 text-micro tabular-nums"
            disabled={!supports('player.setSpeed') || speed <= 0.1}
            onClick={() => stepSpeed(-0.05)}
          >
            -0.05x
          </Button>
          <Slider
            aria-label="精细倍速"
            className="flex-1"
            min={0.1}
            max={3}
            step={0.05}
            disabled={!supports('player.setSpeed')}
            value={[speed]}
            onValueChange={([value]) => void motionStore.setSpeed(Math.round(value * 100) / 100)}
          />
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-7 px-2 text-micro tabular-nums"
            disabled={!supports('player.setSpeed') || speed >= 3}
            onClick={() => stepSpeed(0.05)}
          >
            +0.05x
          </Button>
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-7"
            aria-label="恢复 1.0x 倍速"
            title="恢复 1.00x"
            disabled={!supports('player.setSpeed') || Math.abs(speed - 1) < 0.001}
            onClick={() => void motionStore.setSpeed(1)}
          >
            <RiRestartFill className="size-3.5" />
          </Button>
        </div>

        <div className="flex flex-wrap gap-1">
          {PLAYBACK_SPEEDS.map((presetSpeed) => {
            const active = Math.abs(speed - presetSpeed) < 0.01
            return (
              <button
                key={presetSpeed}
                type="button"
                disabled={!supports('player.setSpeed')}
                onClick={() => void motionStore.setSpeed(presetSpeed)}
                className={cn(
                  'rounded-md border px-2 py-0.5 text-micro tabular-nums transition-colors disabled:opacity-50',
                  active
                    ? 'border-primary bg-primary text-primary-foreground'
                    : 'border-border/80 bg-background/60 text-muted-foreground hover:text-foreground',
                )}
              >
                {presetSpeed.toFixed(1)}x
              </button>
            )
          })}
        </div>
      </div>

      {/* Play mode & clock source decoupling (Req 1 & Req 3) */}
      <div className="space-y-2 border-t border-border/60 pt-3" data-testid="clock-mode-section">
        <div className="flex items-center justify-between gap-2">
          <span className="font-medium text-foreground">播放与时钟模式</span>
          <div className="flex items-center gap-1">
            {clock.leaseId > 0 && (
              <Badge variant="outline" className="border-amber-500/40 bg-amber-500/10 px-1.5 py-0 text-micro text-amber-600 dark:text-amber-400">
                {clock.leaseOwner || '录制插件'} 接管中
              </Badge>
            )}
            <Badge variant="secondary" className="px-1.5 py-0 text-micro tabular-nums">
              {clock.runMode === 'recording' ? '供录制' : '实时'}
            </Badge>
          </div>
        </div>
        <div className="grid grid-cols-2 gap-1.5">
          <button
            type="button"
            data-testid="run-mode-realtime"
            disabled={!supports('clock.setRunMode') || clock.leaseId > 0}
            onClick={() => void motionStore.setClockRunMode('realtime')}
            className={cn(
              'rounded-md border px-2 py-1 text-xs font-medium transition-colors disabled:opacity-50',
              clock.runMode === 'realtime'
                ? 'border-primary bg-primary text-primary-foreground'
                : 'border-border/80 bg-background/60 text-muted-foreground hover:text-foreground',
            )}
          >
            实时模式
          </button>
          <button
            type="button"
            data-testid="run-mode-recording"
            disabled={!supports('clock.setRunMode') || clock.leaseId > 0}
            onClick={() => void motionStore.setClockRunMode('recording')}
            className={cn(
              'rounded-md border px-2 py-1 text-xs font-medium transition-colors disabled:opacity-50',
              clock.runMode === 'recording'
                ? 'border-primary bg-primary text-primary-foreground'
                : 'border-border/80 bg-background/60 text-muted-foreground hover:text-foreground',
            )}
          >
            供录制模式
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-1" data-testid="clock-source-options">
          {(
            [
              ['audio', '音频主时钟'],
              ['engine_delta', '引擎帧间隔'],
              ['fixed_step', '固定步进'],
              ['external', '外部时钟'],
            ] as Array<[ClockSource, string]>
          ).map(([sourceId, label]) => {
            const active = clock.configuredSource === sourceId
            return (
              <button
                key={sourceId}
                type="button"
                data-testid={`clock-source-${sourceId}`}
                disabled={!supports('clock.configure') || clock.leaseId > 0}
                onClick={() => void motionStore.configureClock({ source: sourceId })}
                className={cn(
                  'rounded-md border px-2 py-0.5 text-micro transition-colors disabled:opacity-50',
                  active
                    ? 'border-primary/60 bg-primary/15 font-medium text-primary'
                    : 'border-border/80 bg-background/60 text-muted-foreground hover:text-foreground',
                )}
              >
                {label}
              </button>
            )
          })}
        </div>
        {(clock.runMode === 'recording' || clock.configuredSource === 'fixed_step') && (
          <div className="flex items-center justify-between gap-2 text-xs text-muted-foreground">
            <span>步进帧率</span>
            <div className="flex items-center gap-1">
              {[30, 60, 120].map((fpsVal) => {
                const active = clock.fpsNumerator === fpsVal && clock.fpsDenominator === 1
                return (
                  <button
                    key={fpsVal}
                    type="button"
                    disabled={!supports('clock.configure') || clock.leaseId > 0}
                    onClick={() => void motionStore.configureClock({ fpsNumerator: fpsVal, fpsDenominator: 1 })}
                    className={cn(
                      'rounded border px-1.5 py-0.5 text-micro tabular-nums transition-colors disabled:opacity-50',
                      active
                        ? 'border-primary bg-primary text-primary-foreground'
                        : 'border-border/80 bg-background/60 text-muted-foreground hover:text-foreground',
                    )}
                  >
                    {fpsVal} fps
                  </button>
                )
              })}
            </div>
          </div>
        )}
        <div className="text-micro text-muted-foreground">
          {clock.runMode === 'recording'
            ? '供录制模式将等待每帧渲染完成后推进步进时钟（禁用实时配乐），可由 VideoExporter 锁步接管。'
            : '当前已与 Windows 挂钟解耦，按引擎/音频时钟推进。'}
        </div>
      </div>

      {/* Secondary track controls (Play mode hot-toggle moved to Player Bar) */}
      <div className="space-y-2 border-t border-border/60 pt-3">
        <div className="flex items-center justify-between gap-2">
          <span className="font-medium text-foreground">曲目切换与重载</span>
        </div>
        <div className="flex flex-wrap items-center gap-1.5">
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2.5 text-xs"
            aria-label="上一曲"
            disabled={libraryEmpty || !supports('player.cycle')}
            onClick={() => void motionStore.cycle(-1)}
          >
            <RiSkipBackFill className="size-3.5" />
            上一曲
          </Button>
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2.5 text-xs"
            aria-label="下一曲"
            disabled={libraryEmpty || !supports('player.cycle')}
            onClick={() => void motionStore.cycle(1)}
          >
            <RiSkipForwardFill className="size-3.5" />
            下一曲
          </Button>
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2.5 text-xs"
            disabled={loaded.motionKey === '' || loaded.sourceMissing || !supports('player.reload')}
            onClick={() => void motionStore.reloadMotion()}
          >
            <RiRefreshFill className="size-3.5" />
            重载当前曲目
          </Button>
        </div>
      </div>
    </>
  )
}
