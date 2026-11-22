import * as React from 'react'
import {
  RiRewindStartFill,
  RiSkipBackMiniFill,
  RiSkipForwardFill,
  RiSkipForwardMiniFill,
} from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { cn } from '@/lib/utils'
import type { LoadedState, PlayerState } from '../../protocol'
import { motionStore } from '../../store'
import { formatTransportTime, framePosition } from '../../time'
import { PendingBadge } from '../Unsupported'

const SEEK_STEP_OPTIONS = [1, 5, 10, 15] as const

export function PrecisionSeekSection({
  player,
  loaded,
  hasClip,
  seekStepSeconds,
  navigationError,
  supports,
}: {
  player: PlayerState
  loaded: LoadedState
  hasClip: boolean
  seekStepSeconds: number
  navigationError: string | null
  supports: (method: string) => boolean
}) {
  const framePos = hasClip ? framePosition(player.time, player.fps, player.totalFrames) : 0
  const maxFrame = hasClip ? player.totalFrames - 1 : 0

  const [timeText, setTimeText] = React.useState('00:00.000')
  const [editingTime, setEditingTime] = React.useState(false)
  const [frameText, setFrameText] = React.useState('0')
  const [editingFrame, setEditingFrame] = React.useState(false)
  const identity = `${loaded.motionKey}:${loaded.generation}:${loaded.injectionSession}`

  React.useEffect(() => {
    setEditingTime(false)
    setEditingFrame(false)
  }, [identity])

  React.useEffect(() => {
    if (!editingTime) setTimeText(formatTransportTime(player.time))
  }, [player.time, editingTime])

  React.useEffect(() => {
    if (!editingFrame) setFrameText(String(Math.floor(framePos)))
  }, [framePos, editingFrame])

  const commitTime = () => {
    setEditingTime(false)
    void motionStore.seekTime(timeText)
  }

  const commitFrame = () => {
    setEditingFrame(false)
    void motionStore.seekFrame(frameText)
  }

  return (
    <>
      {/* Live frame readout */}
      <div className="rounded-lg border bg-muted/45 px-3 py-2">
        {hasClip ? (
          <div className="flex items-center justify-between gap-2">
            <span className="text-xs tabular-nums text-foreground" data-testid="transport-frame-info">
              {framePos === Math.floor(framePos)
                ? `当前位置：动作帧 ${Math.floor(framePos)} / ${maxFrame} · ${Number(player.fps.toPrecision(6))} fps`
                : `当前位置：帧间 ${framePos.toFixed(3)} / ${maxFrame} · ${Number(player.fps.toPrecision(6))} fps`}
            </span>
          </div>
        ) : (
          <span className="text-muted-foreground">尚未加载演出曲目</span>
        )}
      </div>

      {/* Frame stepping & seek step */}
      <div className="space-y-2">
        <div className="flex items-center justify-between gap-2">
          <span className="font-medium text-foreground">逐帧微调</span>
          {!supports('player.stepFrame') && <PendingBadge />}
        </div>
        <div className="grid grid-cols-4 gap-1.5">
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2 text-xs"
            disabled={!hasClip || !supports('player.seekFrame')}
            onClick={() => void motionStore.seekFrame('0')}
          >
            <RiRewindStartFill className="size-3.5" />
            首帧
          </Button>
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2 text-xs"
            disabled={!hasClip || !supports('player.stepFrame')}
            onClick={() => void motionStore.stepFrame(-1)}
          >
            <RiSkipBackMiniFill className="size-4" />
            上一帧
          </Button>
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2 text-xs"
            disabled={!hasClip || !supports('player.stepFrame')}
            onClick={() => void motionStore.stepFrame(1)}
          >
            下一帧
            <RiSkipForwardMiniFill className="size-4" />
          </Button>
          <Button
            type="button"
            variant="outline"
            size="sm"
            className="h-8 gap-1 px-2 text-xs"
            disabled={!hasClip || !supports('player.seekFrame')}
            onClick={() => void motionStore.seekFrame(String(maxFrame))}
          >
            末帧
            <RiSkipForwardFill className="size-3.5" />
          </Button>
        </div>

        <div className="flex items-center justify-between gap-2 pt-1">
          <span className="text-muted-foreground">主控快进/快退步长</span>
          <div className="flex items-center gap-1" role="group" aria-label="快进快退步长">
            {SEEK_STEP_OPTIONS.map((step) => (
              <button
                key={step}
                type="button"
                aria-pressed={seekStepSeconds === step}
                onClick={() => motionStore.setSeekStepSeconds(step)}
                className={cn(
                  'rounded-md border px-2 py-0.5 text-xs tabular-nums transition-colors',
                  seekStepSeconds === step
                    ? 'border-primary bg-primary/15 font-medium text-primary'
                    : 'border-border bg-background/60 text-muted-foreground hover:text-foreground',
                )}
              >
                {step}s
              </button>
            ))}
          </div>
        </div>
      </div>

      {/* Exact time & frame seek */}
      <div className="space-y-2 border-t border-border/60 pt-3">
        <span className="block font-medium text-foreground">精确时间与动作帧定位</span>
        <div className="grid grid-cols-2 gap-2">
          <div className="space-y-1">
            <label className="text-xs text-muted-foreground" htmlFor="precision-time-input">
              时间 (秒或 mm:ss.mmm)
            </label>
            <div className="flex gap-1">
              <Input
                id="precision-time-input"
                className="h-8 px-2 text-xs tabular-nums"
                aria-label="时间定位"
                value={timeText}
                disabled={!hasClip || !supports('player.seek')}
                onFocus={() => setEditingTime(true)}
                onChange={(event) => setTimeText(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') commitTime()
                  else if (event.key === 'Escape') {
                    setEditingTime(false)
                    setTimeText(formatTransportTime(player.time))
                  }
                }}
              />
              <Button
                type="button"
                variant="secondary"
                size="sm"
                className="h-8 shrink-0 px-2.5 text-xs"
                disabled={!hasClip || !supports('player.seek')}
                onClick={commitTime}
              >
                定位
              </Button>
            </div>
          </div>

          <div className="space-y-1">
            <label className="text-xs text-muted-foreground" htmlFor="precision-frame-input">
              动作帧 (0 – {maxFrame})
            </label>
            <div className="flex gap-1">
              <Input
                id="precision-frame-input"
                className="h-8 px-2 text-xs tabular-nums"
                aria-label="动作帧定位"
                value={frameText}
                disabled={!hasClip || !supports('player.seekFrame')}
                onFocus={() => setEditingFrame(true)}
                onChange={(event) => setFrameText(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') commitFrame()
                  else if (event.key === 'Escape') {
                    setEditingFrame(false)
                    setFrameText(String(Math.floor(framePos)))
                  }
                }}
              />
              <Button
                type="button"
                variant="secondary"
                size="sm"
                className="h-8 shrink-0 px-2.5 text-xs"
                disabled={!hasClip || !supports('player.seekFrame')}
                onClick={commitFrame}
              >
                定位
              </Button>
            </div>
          </div>
        </div>

        {navigationError && (
          <div
            className="rounded-md border border-destructive/40 bg-destructive/10 px-2.5 py-1.5 text-xs text-destructive"
            role="alert"
            data-testid="transport-navigation-error"
          >
            {navigationError}
          </div>
        )}
      </div>
    </>
  )
}
