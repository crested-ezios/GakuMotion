import { RiCloseFill, RiPushpinFill, RiUnpinFill } from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { motionStore, useMotionStore } from '../store'
import { validTimeline } from '../time'
import { PrecisionSeekSection } from './precision/PrecisionSeekSection'
import { PrecisionSpeedClockSection } from './precision/PrecisionSpeedClockSection'

export function PrecisionControlPanel({ onClose }: { onClose: () => void }) {
  const player = useMotionStore((state) => state.player)
  const clock = useMotionStore((state) => state.clock)
  const loaded = useMotionStore((state) => state.loaded)
  const libraryEmpty = useMotionStore((state) => state.library.entries.length === 0)
  const navigationError = useMotionStore((state) => state.navigationError)
  const seekStepSeconds = useMotionStore((state) => state.seekStepSeconds)
  const pinned = useMotionStore((state) => state.precisionPanelPinned)
  const capabilities = useMotionStore((state) => state.capabilities)

  const supports = (method: string) => capabilities.includes(method)
  const hasClip = validTimeline(player.totalFrames, player.fps)

  return (
    <div className="w-[440px] max-w-[calc(100vw-24px)] space-y-3.5 text-xs" data-testid="precision-control-panel">
      {/* Header */}
      <div className="flex items-center justify-between gap-2 border-b border-border/70 pb-2.5">
        <div className="flex items-center gap-2">
          <span className="text-md font-semibold text-foreground">精细播放控制</span>
          {hasClip && (
            <Badge variant="outline" className="rounded-[4px] bg-muted/60 text-micro tabular-nums text-muted-foreground">
              {Number(player.fps.toPrecision(6))} fps
            </Badge>
          )}
        </div>
        <div className="flex items-center gap-1">
          <Button
            type="button"
            variant={pinned ? 'secondary' : 'ghost'}
            size="icon"
            className="size-7"
            aria-label={pinned ? '取消固定面板' : '固定面板'}
            title={pinned ? '取消固定（点击外部自动收起）' : '固定面板（切换页面不关闭）'}
            onClick={() => motionStore.setPrecisionPanelPinned(!pinned)}
          >
            {pinned ? <RiUnpinFill className="size-3.5 text-primary" /> : <RiPushpinFill className="size-3.5" />}
          </Button>
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-7"
            aria-label="关闭精细控制"
            onClick={onClose}
          >
            <RiCloseFill className="size-4" />
          </Button>
        </div>
      </div>

      <PrecisionSeekSection
        player={player}
        loaded={loaded}
        hasClip={hasClip}
        seekStepSeconds={seekStepSeconds}
        navigationError={navigationError}
        supports={supports}
      />

      <PrecisionSpeedClockSection
        speed={player.speed}
        clock={clock}
        loaded={loaded}
        libraryEmpty={libraryEmpty}
        supports={supports}
      />

      {/* Shortcuts summary */}
      <div className="border-t border-border/60 pt-2 text-micro text-muted-foreground">
        快捷键：Space 播放/暂停 · ←/→ 快退快进 · Shift+←/→ 逐帧 · F9/F10 全局热键
      </div>
    </div>
  )
}
