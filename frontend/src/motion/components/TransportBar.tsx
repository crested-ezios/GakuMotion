import * as React from 'react'
import {
  RiOrderPlayFill,
  RiPauseFill,
  RiPlayFill,
  RiRepeatFill,
  RiRepeatOneFill,
  RiRewindFill,
  RiShuffleFill,
  RiSpeedFill,
  RiStopFill,
} from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { Tooltip, TooltipContent, TooltipProvider, TooltipTrigger } from '@/components/ui/tooltip'
import { cn } from '@/lib/utils'
import { PLAY_MODES } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { durationMismatch, validTimeline } from '../time'
import { AlbumCover } from './AlbumCover'
import { CoverEditorModal } from './CoverEditorModal'
import { PendingBadge } from './Unsupported'
import { TransportRightControls } from './transport/TransportRightControls'
import { TransportScrubber } from './transport/TransportScrubber'

function IconTooltip({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <Tooltip>
      <TooltipTrigger asChild>{children}</TooltipTrigger>
      <TooltipContent side="top">{label}</TooltipContent>
    </Tooltip>
  )
}

function playModeIcon(mode: number) {
  if (mode === 1) return RiRepeatOneFill
  if (mode === 2) return RiRepeatFill
  if (mode === 3) return RiShuffleFill
  return RiOrderPlayFill
}

export function TransportBar() {
  const player = useMotionStore((state) => state.player)
  const scrub = useMotionStore((state) => state.scrub)
  const loaded = useMotionStore((state) => state.loaded)
  const libraryEntries = useMotionStore((state) => state.library.entries)
  const libraryRevision = useMotionStore((state) => state.library.revision)
  const musicVolumeSupport = useMotionStore((state) => state.musicVolumeSupport)
  const musicVolumeNotice = useMotionStore((state) => state.musicVolumeNotice)
  const navigationError = useMotionStore((state) => state.navigationError)
  const actionError = useMotionStore((state) => state.actionError)
  const cameraPending = useMotionStore((state) => state.cameraPending)
  const clock = useMotionStore((state) => state.clock)
  const audioEffect = useMotionStore((state) => state.audioEffect)
  const coverCache = useMotionStore((state) => state.coverCache)
  const capabilities = useMotionStore((state) => state.capabilities)
  const seekStepSeconds = useMotionStore((state) => state.seekStepSeconds)
  const precisionOpen = useMotionStore((state) => state.precisionPanelOpen)
  const precisionPinned = useMotionStore((state) => state.precisionPanelPinned)

  const supports = (method: string) => capabilities.includes(method)
  const hasClip = validTimeline(player.totalFrames, player.fps)
  const duration = player.duration > 0 ? player.duration : 0
  const scrubEnabled = hasClip && duration > 0 && supports('player.scrubBegin')
  const transportSupported = supports('player.play')

  const isPlaying = player.state === 'playing'
  const isPaused = player.state === 'paused'
  const isPreview = player.state === 'preview'
  const displayTime = scrub.active ? scrub.value : player.time

  const currentEntry = React.useMemo(
    () => libraryEntries.find((entry) => entry.id === loaded.motionKey) ?? null,
    [libraryEntries, loaded.motionKey],
  )
  const currentEntryId = currentEntry?.id ?? ''
  const hasCachedCover = useMotionStore((state) =>
    currentEntryId ? Object.prototype.hasOwnProperty.call(state.coverCache, currentEntryId) : true,
  )
  React.useEffect(() => {
    if (currentEntry && (currentEntry.hasCover || currentEntry.hasAudio) && !hasCachedCover) {
      void motionStore.ensureTrackCover(currentEntry.id)
    }
  }, [currentEntry?.id, currentEntry?.hasCover, currentEntry?.hasAudio, hasCachedCover, libraryRevision])
  const currentCoverUri = currentEntry?.id ? (coverCache[currentEntry.id] ?? '') : ''
  const currentSubtitle = React.useMemo(() => {
    if (!currentEntry?.subtitle) return ''
    return currentEntry.subtitle
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean)
      .join(' / ')
  }, [currentEntry?.subtitle])

  const cameraModeIndex = motionStore.cameraQuickModeIndex()
  const playModeLabels = PLAY_MODES
  const normalizedPlayMode =
    player.playMode >= 0 && player.playMode < playModeLabels.length ? player.playMode : 1
  const currentPlayModeLabel = playModeLabels[normalizedPlayMode] ?? playModeLabels[0]
  const PlayModeIcon = playModeIcon(normalizedPlayMode)

  const hasDurationMismatch = hasClip && durationMismatch(player.totalFrames, player.fps, player.duration)
  const showSourceNotice = loaded.sourceMissing || loaded.sourceChanged

  return (
    <TooltipProvider delayDuration={300}>
      <footer className="relative z-30 shrink-0 bg-playerBar select-none" data-testid="transport-bar">
        <TransportScrubber
          hasClip={hasClip}
          duration={duration}
          fps={player.fps}
          totalFrames={player.totalFrames}
          displayTime={displayTime}
          scrubActive={scrub.active}
          scrubEnabled={scrubEnabled}
          actionError={actionError}
          showSourceNotice={showSourceNotice}
          sourceMissing={loaded.sourceMissing}
          navigationError={navigationError}
          precisionOpen={precisionOpen}
          hasDurationMismatch={hasDurationMismatch}
        />

        {/* Main 3-Zone Player Bar */}
        <div className="flex min-h-[72px] items-center justify-between gap-4 px-5 py-2.5">
          {/* Left: Album cover + Title + Status + Time + Actor */}
          <div className="flex min-w-0 flex-1 items-center gap-3">
            {currentEntry ? (
              <button
                type="button"
                title="点击编辑曲目封面"
                aria-label={`编辑 ${currentEntry.title} 封面`}
                data-testid="transport-cover-button"
                onClick={() => motionStore.openCoverEditorForTrack(currentEntry.id, 'transport')}
                className={cn(
                  'flex size-[var(--gaku-cover-player,48px)] shrink-0 items-center justify-center overflow-hidden rounded-md border transition-opacity hover:opacity-90 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
                  hasClip
                    ? 'border-primary/25 bg-primary/10 text-primary'
                    : 'border-border bg-muted/60 text-muted-foreground',
                )}
              >
                <AlbumCover
                  src={currentCoverUri}
                  imgTestId="transport-cover-img"
                  defaultTestId="transport-default-cover"
                />
              </button>
            ) : (
              <div
                aria-hidden="true"
                className="flex size-[var(--gaku-cover-player,48px)] shrink-0 items-center justify-center overflow-hidden rounded-md border border-border bg-muted/60 text-muted-foreground"
              >
                <AlbumCover defaultTestId="transport-default-cover" />
              </div>
            )}
            <div className="min-w-0 flex-1 space-y-0.5">
              <div className="flex items-center gap-2">
                <div
                  className="min-w-0 truncate text-base font-semibold leading-snug text-foreground"
                  data-testid="transport-title"
                >
                  {player.title}
                </div>
                {clock.runMode === 'recording' && (
                  <span
                    data-testid="transport-recording-badge"
                    className="shrink-0 rounded bg-amber-500/15 px-1.5 py-0.5 text-micro font-medium text-amber-600 dark:text-amber-400"
                  >
                    供录制
                  </span>
                )}
                {!supports('player.scrubBegin') && <PendingBadge />}
              </div>
              <div className="flex flex-wrap items-center gap-2 text-xs text-muted-foreground">
                <span className="truncate">
                  {currentSubtitle || (hasClip ? '演出已加载' : '')}
                </span>
              </div>
            </div>
          </div>

          {/* Center: PlayMode hot-toggle + Rewind + Play/Pause + Fast-Forward + Stop */}
          <div className="flex shrink-0 items-center gap-1.5">
            <IconTooltip label={`播放模式：${currentPlayModeLabel}（点击切换）`}>
              <Button
                type="button"
                variant="ghost"
                size="icon"
                className="size-9 rounded-md text-muted-foreground hover:bg-accent hover:text-foreground"
                aria-label="切换播放模式"
                data-testid="transport-playmode-toggle"
                disabled={!supports('playlist.setMode')}
                onClick={() => void motionStore.cyclePlayMode()}
              >
                <PlayModeIcon className="size-[18px]" />
              </Button>
            </IconTooltip>

            <IconTooltip label={`快退 ${seekStepSeconds} 秒 (←)`}>
              <Button
                type="button"
                variant="ghost"
                size="icon"
                className="size-9 rounded-md text-foreground/85 hover:bg-accent"
                aria-label="快退"
                disabled={!hasClip || !supports('player.seek')}
                onClick={() => void motionStore.seekRelative(-seekStepSeconds)}
              >
                <RiRewindFill className="size-[18px]" />
              </Button>
            </IconTooltip>

            <IconTooltip label={isPlaying ? '暂停 (Space / F9)' : isPaused || isPreview ? '继续播放 (Space / F9)' : '播放 (Space / F9)'}>
              <Button
                type="button"
                variant="default"
                size="icon"
                className="size-10 rounded-full shadow-sm transition-transform active:scale-95"
                aria-label={isPlaying ? '暂停' : '播放'}
                disabled={!hasClip || !transportSupported}
                onClick={() => void (isPlaying ? motionStore.pause() : motionStore.play())}
              >
                {isPlaying ? <RiPauseFill className="size-5" /> : <RiPlayFill className="size-5 translate-x-[0.5px]" />}
              </Button>
            </IconTooltip>

            <IconTooltip label={`快进 ${seekStepSeconds} 秒 (→)`}>
              <Button
                type="button"
                variant="ghost"
                size="icon"
                className="size-9 rounded-md text-foreground/85 hover:bg-accent"
                aria-label="快进"
                disabled={!hasClip || !supports('player.seek')}
                onClick={() => void motionStore.seekRelative(seekStepSeconds)}
              >
                <RiSpeedFill className="size-[18px]" />
              </Button>
            </IconTooltip>

            <IconTooltip label="停止：归零并恢复游戏动作 (F10)">
              <Button
                type="button"
                variant="ghost"
                size="icon"
                className="size-9 rounded-md text-muted-foreground hover:bg-accent hover:text-foreground"
                aria-label="停止"
                disabled={!hasClip || !supports('player.stop')}
                onClick={() => void motionStore.stop()}
              >
                <RiStopFill className="size-[18px]" />
              </Button>
            </IconTooltip>
            {!transportSupported && <PendingBadge />}
          </div>

          <TransportRightControls
            audioEffect={audioEffect}
            cameraModeIndex={cameraModeIndex}
            cameraPending={cameraPending}
            speed={player.speed}
            volume={player.volume}
            musicVolumeSupport={musicVolumeSupport}
            musicVolumeNotice={musicVolumeNotice}
            precisionOpen={precisionOpen}
            precisionPinned={precisionPinned}
            navigationError={navigationError}
            supports={supports}
          />
        </div>
        <CoverEditorModal owner="transport" />
      </footer>
    </TooltipProvider>
  )
}
