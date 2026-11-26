import {
  RiAlertFill,
  RiDiscFill,
  RiMapPin2Fill,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { useMotionStore } from '../store'

// Studio context header bar: active track, active scene, and resource warnings.
export function MotionStatusBar() {
  const playerTitle = useMotionStore((state) => state.player.title)
  const loadedMotionKey = useMotionStore((state) => state.loaded.motionKey)
  const sourceMissing = useMotionStore((state) => state.loaded.sourceMissing)
  const sourceChanged = useMotionStore((state) => state.loaded.sourceChanged)
  const selectedTitle = useMotionStore((state) => {
    if (state.loaded.motionKey || !state.selectedLibraryKey) return ''
    return state.library.entries.find((entry) => entry.id === state.selectedLibraryKey)?.title ?? ''
  })
  const stageSceneName = useMotionStore((state) => state.stage.sceneName)

  let trackName = '未加载曲目'
  if (loadedMotionKey) {
    trackName = playerTitle
  } else if (selectedTitle) {
    trackName = selectedTitle
  }

  return (
    <header
      className="flex min-h-[42px] shrink-0 flex-wrap items-center justify-end gap-2.5 bg-background px-5 py-2 text-xs"
      data-testid="motion-status-bar"
    >
      {(sourceMissing || sourceChanged) && (
        <Badge variant="warning" className="gap-1 rounded-[4px]">
          <RiAlertFill className="size-3.5" />
          <span>{sourceMissing ? '源文件缺失' : '源文件已变更'}</span>
        </Badge>
      )}

      <span className="hidden items-center gap-1.5 text-muted-foreground lg:inline-flex">
        <RiDiscFill className="size-3.5 shrink-0" />
        <span className="max-w-[200px] truncate">曲目: {trackName}</span>
      </span>

      {stageSceneName && (
        <span className="hidden items-center gap-1 text-muted-foreground sm:inline-flex">
          <RiMapPin2Fill className="size-3.5" />
          <span className="max-w-[140px] truncate">{stageSceneName}</span>
        </span>
      )}
    </header>
  )
}
