import * as React from 'react'
import * as SliderPrimitive from '@radix-ui/react-slider'
import { RiAlertFill } from '@remixicon/react'
import { cn } from '@/lib/utils'
import { motionStore } from '../../store'
import { formatTransportTime, framePosition } from '../../time'

export function TransportScrubber({
  hasClip,
  duration,
  fps,
  totalFrames,
  displayTime,
  scrubActive,
  scrubEnabled,
  actionError,
  showSourceNotice,
  sourceMissing,
  navigationError,
  precisionOpen,
  hasDurationMismatch,
}: {
  hasClip: boolean
  duration: number
  fps: number
  totalFrames: number
  displayTime: number
  scrubActive: boolean
  scrubEnabled: boolean
  actionError: string | null
  showSourceNotice: boolean
  sourceMissing: boolean
  navigationError: string | null
  precisionOpen: boolean
  hasDurationMismatch: boolean
}) {
  const [hoverInfo, setHoverInfo] = React.useState<{ ratio: number; seconds: number } | null>(null)

  const handleTrackPointerMove = (event: React.PointerEvent<HTMLDivElement>) => {
    if (!hasClip || duration <= 0) return
    const rect = event.currentTarget.getBoundingClientRect()
    if (rect.width <= 0) return
    const ratio = Math.min(Math.max((event.clientX - rect.left) / rect.width, 0), 1)
    setHoverInfo({ ratio, seconds: ratio * duration })
  }

  return (
    <>
      {/* Top Edge: Full-width precision timeline scrubber */}
      <div
        className="group relative -mt-1.5 flex h-3 w-full cursor-pointer items-center"
        onPointerMove={handleTrackPointerMove}
        onPointerLeave={() => setHoverInfo(null)}
      >
        <SliderPrimitive.Root
          aria-label="播放进度"
          className="relative flex h-full w-full touch-none select-none items-center data-[disabled]:cursor-not-allowed data-[disabled]:opacity-55"
          min={0}
          max={duration > 0 ? duration : 1}
          step={0.001}
          disabled={!scrubEnabled}
          value={[Math.min(Math.max(displayTime, 0), duration > 0 ? duration : 1)]}
          onValueChange={([value]) => {
            if (!motionStore.getState().scrub.active) motionStore.scrubStart(value)
            else motionStore.scrubMove(value)
          }}
          onValueCommit={([value]) => {
            setTimeout(() => motionStore.scrubCommit(value), 0)
          }}
        >
          <SliderPrimitive.Track className="relative h-[2px] w-full grow overflow-hidden bg-border transition-[height] duration-150 group-hover:h-[4px]">
            <SliderPrimitive.Range className="absolute h-full bg-primary transition-colors" />
          </SliderPrimitive.Track>
          <SliderPrimitive.Thumb
            aria-label="播放进度"
            className={cn(
              'block size-3 rounded-full border-2 border-primary bg-background shadow-sm transition-transform focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring disabled:pointer-events-none',
              scrubActive
                ? 'scale-100 opacity-100'
                : 'scale-75 opacity-0 group-hover:scale-100 group-hover:opacity-100 focus-visible:scale-100 focus-visible:opacity-100',
            )}
          />
        </SliderPrimitive.Root>

        {hoverInfo && hasClip && (
          <div
            aria-hidden="true"
            className="pointer-events-none absolute -top-8 z-40 -translate-x-1/2 whitespace-nowrap rounded-md border bg-popover px-2.5 py-0.5 text-xs font-medium tabular-nums text-popover-foreground shadow-md"
            style={{ left: `${Math.min(Math.max(hoverInfo.ratio * 100, 5), 95)}%` }}
          >
            {formatTransportTime(hoverInfo.seconds)} / {formatTransportTime(duration)} · 帧{' '}
            {Math.floor(framePosition(hoverInfo.seconds, fps, totalFrames))}
          </div>
        )}
      </div>

      {/* Inline diagnostic strip when critical notices/errors are present */}
      {(actionError || showSourceNotice || (navigationError && !precisionOpen) || (hasDurationMismatch && !precisionOpen)) && (
        <div className="flex flex-wrap items-center gap-x-4 gap-y-1 border-b border-border/60 bg-muted/40 px-5 py-1.5 text-xs">
          {showSourceNotice && (
            <span className="flex items-center gap-1 text-warning">
              <RiAlertFill className="size-3.5 shrink-0" />
              {sourceMissing
                ? '曲目源文件已移除：请先恢复文件，再到曲库重新加载当前曲目。'
                : '曲目文件已更新：请到曲库重新加载当前曲目；音乐更新后需要重新加载才能开播。'}
            </span>
          )}
          {hasDurationMismatch && !precisionOpen && (
            <button
              type="button"
              onClick={() => motionStore.setPrecisionPanelOpen(true)}
              className="text-left text-warning underline-offset-2 hover:underline"
              data-testid="transport-duration-warning"
            >
              源动作头部时长与采样帧不一致（点击查看详情）
            </button>
          )}
          {navigationError && !precisionOpen && (
            <span className="text-destructive" role="alert" data-testid="transport-navigation-error">
              {navigationError}
            </span>
          )}
          {actionError && (
            <span className="ml-auto flex items-center gap-2 text-destructive" role="alert" data-testid="transport-action-error">
              <span>{actionError}</span>
              <button
                type="button"
                onClick={() => motionStore.clearActionError()}
                className="text-xs underline underline-offset-2"
              >
                忽略
              </button>
            </span>
          )}
        </div>
      )}
    </>
  )
}
