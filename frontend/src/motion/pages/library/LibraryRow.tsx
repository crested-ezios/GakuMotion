import * as React from 'react'
import { RiDeleteBinLine, RiPauseFill, RiPencilLine, RiEyeLine, RiPlayFill } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'
import type { LibraryEntry, PlaybackStatus } from '../../protocol'
import { motionStore, useMotionStore } from '../../store'
import { formatMmSs } from '../../time'
import { AlbumCover } from '../../components/AlbumCover'

export function trackTags(entry: LibraryEntry): string[] {
  const tags: string[] = []
  if (entry.hasMotion) tags.push('动作')
  if (entry.hasFacial) tags.push('表情')
  if (entry.hasCamera) tags.push('镜头')
  if (entry.hasAudio) tags.push('音频')
  return tags
}

export function formatSubtitle(raw?: string): string {
  if (!raw) return ''
  const parts = raw
    .split(',')
    .map((part) => part.trim())
    .filter(Boolean)
  return parts.join(' / ')
}

function PlayingEqualizer() {
  return (
    <span className="inline-flex h-3.5 items-end gap-[2px]" aria-hidden="true">
      <span className="w-[2px] h-full rounded-full bg-primary animate-eq-1" />
      <span className="w-[2px] h-full rounded-full bg-primary animate-eq-2" />
      <span className="w-[2px] h-full rounded-full bg-primary animate-eq-3" />
    </span>
  )
}

export interface LibraryRowProps {
  entry: LibraryEntry
  index: number
  selected: boolean
  loadedStatus: PlaybackStatus | null
  busy: boolean
  canSelect: boolean
  canPause: boolean
  canEdit: boolean
  canDelete: boolean
  style?: React.CSSProperties
  onSelect: () => void
  onDoubleClick: () => void
  onContextMenu: (event: React.MouseEvent<HTMLDivElement>) => void
  onPlay: () => void
  onPause: () => void
  onPreview: () => void
  onEdit: () => void
  onDelete: () => void
}

function LibraryRowCover({
  entryId,
  entryTitle,
  hasCover,
  hasAudio,
}: {
  entryId: string
  entryTitle: string
  hasCover: boolean
  hasAudio: boolean
}) {
  const coverUri = useMotionStore((state) => state.coverCache[entryId] ?? '')
  const hasCachedCover = useMotionStore((state) => Object.prototype.hasOwnProperty.call(state.coverCache, entryId))
  const libraryRevision = useMotionStore((state) => state.library.revision)
  React.useEffect(() => {
    if ((hasCover || hasAudio) && !hasCachedCover) {
      void motionStore.ensureTrackCover(entryId)
    }
  }, [entryId, hasCover, hasAudio, hasCachedCover, libraryRevision])
  return (
    <button
      type="button"
      title="点击编辑封面"
      aria-label={`编辑 ${entryTitle} 封面`}
      onClick={(event) => {
        event.stopPropagation()
        motionStore.openCoverEditorForTrack(entryId, 'library')
      }}
      onDoubleClick={(event) => event.stopPropagation()}
      className="flex size-[var(--gaku-cover-row,42px)] shrink-0 items-center justify-center overflow-hidden rounded-md border border-border/60 bg-muted/40 transition-opacity hover:opacity-90 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
    >
      <AlbumCover
        src={coverUri}
        imgTestId={`library-cover-${entryId}`}
        defaultTestId={`library-default-cover-${entryId}`}
      />
    </button>
  )
}

export function LibraryRow({
  entry,
  index,
  selected,
  loadedStatus,
  busy,
  canSelect,
  canPause,
  canEdit,
  canDelete,
  style,
  onSelect,
  onDoubleClick,
  onContextMenu,
  onPlay,
  onPause,
  onPreview,
  onEdit,
  onDelete,
}: LibraryRowProps) {
  const tags = trackTags(entry)
  const subtitleText = formatSubtitle(entry.subtitle)
  const isPlaying = loadedStatus === 'playing'
  const isPaused = loadedStatus === 'paused' || loadedStatus === 'preview'

  return (
    <div
      role="row"
      aria-selected={selected}
      data-testid={`library-row-${entry.id}`}
      style={style}
      onClick={(event) => {
        event.stopPropagation()
        onSelect()
      }}
      onDoubleClick={(event) => {
        event.stopPropagation()
        onDoubleClick()
      }}
      onContextMenu={onContextMenu}
      className={cn(
        'group relative flex h-[var(--gaku-row-height,56px)] select-none items-center gap-3.5 rounded-lg px-3 text-left transition-all duration-150',
        selected
          ? 'bg-card shadow-[0_2px_8px_rgba(0,0,0,0.06)] dark:shadow-[0_2px_10px_rgba(0,0,0,0.28)]'
          : 'hover:bg-card hover:shadow-[0_2px_8px_rgba(0,0,0,0.06)] dark:hover:shadow-[0_2px_10px_rgba(0,0,0,0.28)]',
      )}
    >
      {/* 状态 / 序号：仅响应 Hover（click 后主动 blur，避免鼠标移开后停留在暂停按钮） */}
      <div
        role="cell"
        className="relative flex w-8 shrink-0 items-center justify-center"
        onClick={(event) => event.stopPropagation()}
        onDoubleClick={(event) => event.stopPropagation()}
      >
        <span
          className={cn(
            'flex items-center justify-center transition-opacity group-hover:opacity-0',
            isPlaying || isPaused ? 'text-primary' : 'text-sm font-medium tabular-nums text-muted-foreground',
          )}
        >
          {isPlaying ? (
            <PlayingEqualizer />
          ) : isPaused ? (
            <RiPlayFill className="size-4" />
          ) : (
            String(index + 1).padStart(2, '0')
          )}
        </span>

        {isPlaying ? (
          <button
            type="button"
            disabled={busy || !canPause}
            aria-label={`暂停 ${entry.title}`}
            title="暂停"
            onClick={(event) => {
              event.currentTarget.blur()
              onPause()
            }}
            className="absolute inset-0 flex items-center justify-center rounded-md text-primary opacity-0 transition-opacity group-hover:opacity-100 focus-visible:opacity-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring disabled:pointer-events-none disabled:opacity-40"
          >
            <RiPauseFill className="size-4" />
          </button>
        ) : (
          <button
            type="button"
            disabled={busy || !canSelect}
            aria-label={`播放 ${entry.title}`}
            title={isPaused ? '继续播放' : '播放'}
            onClick={(event) => {
              event.currentTarget.blur()
              onPlay()
            }}
            className={cn(
              'absolute inset-0 flex items-center justify-center rounded-md opacity-0 transition-opacity group-hover:opacity-100 focus-visible:opacity-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring disabled:pointer-events-none disabled:opacity-40',
              isPaused ? 'text-primary' : 'text-foreground hover:text-primary',
            )}
          >
            <RiPlayFill className="size-4" />
          </button>
        )}
      </div>

      {/* 演出曲目 + 右侧 3 个空心悬停小图标（正在播放时标题与小标题同时高亮为主色） */}
      <div role="cell" className="flex min-w-0 flex-1 items-center justify-between gap-2.5">
        <div className="flex min-w-0 flex-1 items-center gap-3">
          <LibraryRowCover
            entryId={entry.id}
            entryTitle={entry.title}
            hasCover={Boolean(entry.hasCover)}
            hasAudio={Boolean(entry.hasAudio)}
          />
          <div className="min-w-0 flex-1 space-y-0.5">
            <div
              className={cn(
                'truncate text-base font-medium leading-snug',
                isPlaying ? 'text-primary' : 'text-foreground',
              )}
              title={entry.title}
            >
              {entry.title}
            </div>
            <div className="flex min-w-0 items-center gap-1.5 text-xs tabular-nums">
              {entry.isGroup && (
                <span className="inline-flex shrink-0 items-center rounded-lg border border-amber-400/60 bg-amber-500/15 px-1.5 py-0.5 text-micro leading-none text-amber-600 dark:text-amber-300">
                  团曲
                </span>
              )}
              <p
                className={cn(
                  'min-w-0 truncate',
                  isPlaying ? 'text-primary/85' : 'text-muted-foreground',
                )}
              >
                {subtitleText ? `${subtitleText} · ${entry.totalFrames} 帧` : `${entry.id} · ${entry.totalFrames} 帧`}
              </p>
            </div>
          </div>
        </div>

        {/* Hover: 仅保留预览、管理、删除 3 个空心线框小图标 */}
        <div
          className="flex shrink-0 items-center gap-0.5 opacity-0 transition-opacity group-hover:opacity-100 group-focus-within:opacity-100"
          onClick={(event) => event.stopPropagation()}
          onDoubleClick={(event) => event.stopPropagation()}
        >
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-8 text-muted-foreground hover:bg-accent/70 hover:text-foreground"
            disabled={busy || !canSelect}
            aria-label={`预览 ${entry.title}`}
            title="加载预览"
            onClick={onPreview}
          >
            <RiEyeLine className="size-4" />
          </Button>
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-8 text-muted-foreground hover:bg-accent/70 hover:text-foreground"
            disabled={busy || !canEdit}
            aria-label={`编辑 ${entry.title}`}
            title="管理轨道与同步"
            onClick={onEdit}
          >
            <RiPencilLine className="size-4" />
          </Button>
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="size-8 text-muted-foreground hover:bg-destructive/12 hover:text-destructive"
            disabled={busy || !canDelete}
            aria-label={`删除 ${entry.title}`}
            title="删除"
            onClick={onDelete}
          >
            <RiDeleteBinLine className="size-4" />
          </Button>
        </div>
      </div>

      {/* 包含内容 */}
      <div role="cell" className="hidden w-[180px] shrink-0 items-center gap-1.5 sm:flex">
        {tags.length > 0 ? (
          tags.map((tag) => (
            <span
              key={tag}
              className="rounded-[4px] border border-border/75 bg-muted/45 px-1.5 py-0.5 text-micro font-medium leading-none text-muted-foreground"
            >
              {tag}
            </span>
          ))
        ) : (
          <span className="text-xs text-muted-foreground/60">—</span>
        )}
      </div>

      {/* 时长 */}
      <div role="cell" className="w-16 shrink-0 text-right text-sm tabular-nums text-muted-foreground">
        {formatMmSs(entry.durationSeconds)}
      </div>
    </div>
  )
}
