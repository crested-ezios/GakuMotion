import {
  RiAlertFill,
  RiDiscFill,
  RiMapPin2Fill,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { useMotionStore } from '../store'

// Studio context header bar: active track, active scene, and resource warnings.
export function MotionStatusBar() {
  const player = useMotionStore((state) => state.player)
  const loaded = useMotionStore((state) => state.loaded)
  const library = useMotionStore((state) => state.library)
  const selectedKey = useMotionStore((state) => state.selectedLibraryKey)
  const stageSceneName = useMotionStore((state) => state.stage.sceneName)

  let trackName = '未加载曲目'
  if (loaded.motionKey) {
    trackName = player.title
  } else if (selectedKey) {
    trackName = library.entries.find((entry) => entry.id === selectedKey)?.title ?? trackName
  }

  return (
    <header
      className="flex min-h-[42px] shrink-0 flex-wrap items-center justify-end gap-2.5 bg-background px-5 py-2 text-xs"
      data-testid="motion-status-bar"
    >
      {(loaded.sourceMissing || loaded.sourceChanged) && (
        <Badge variant="warning" className="gap-1 rounded-[4px]">
          <RiAlertFill className="size-3.5" />
          <span>{loaded.sourceMissing ? '源文件缺失' : '源文件已变更'}</span>
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
