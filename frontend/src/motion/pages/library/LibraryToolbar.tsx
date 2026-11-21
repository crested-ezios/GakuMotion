import * as React from 'react'
import {
  RiAddFill,
  RiFilter2Fill,
  RiFolderOpenFill,
  RiMoreFill,
  RiRefreshFill,
  RiSearchLine,
} from '@remixicon/react'
import { Button } from '@/components/ui/button'
import {
  DropdownMenu,
  DropdownMenuCheckboxItem,
  DropdownMenuContent,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { Input } from '@/components/ui/input'
import { Popover, PopoverContent, PopoverTrigger } from '@/components/ui/popover'
import { cn } from '@/lib/utils'
import type { LibraryEntry, LibraryState, LoadedState } from '../../protocol'
import { motionStore, type LibraryFilter, type LibrarySort } from '../../store'
import { PendingBadge } from '../../components/Unsupported'

const FILTER_OPTIONS: ReadonlyArray<{ value: LibraryFilter; label: string }> = [
  { value: 'all', label: '全部曲目' },
  { value: 'group', label: '团曲' },
  { value: 'camera', label: '含镜头' },
  { value: 'audio', label: '含音乐' },
  { value: 'facial', label: '含表情' },
  { value: 'motionOnly', label: '纯动作' },
]

export function filterAndSortEntries(
  entries: LibraryEntry[],
  search: string,
  filter: LibraryFilter,
  sort: LibrarySort,
): LibraryEntry[] {
  const needle = search.trim().toLowerCase()
  const filtered = entries.filter((entry) => {
    if (
      needle &&
      !entry.title.toLowerCase().includes(needle) &&
      !entry.id.toLowerCase().includes(needle) &&
      !(entry.subtitle ?? '').toLowerCase().includes(needle)
    ) {
      return false
    }
    if (filter === 'group') return Boolean(entry.isGroup)
    if (filter === 'camera') return entry.hasCamera
    if (filter === 'audio') return entry.hasAudio
    if (filter === 'facial') return entry.hasFacial
    if (filter === 'motionOnly') return entry.hasMotion && !entry.hasCamera && !entry.hasAudio && !entry.hasFacial
    return true
  })
  if (sort === 'default') return filtered
  const copy = [...filtered]
  if (sort === 'title') copy.sort((a, b) => a.title.localeCompare(b.title, 'zh-CN'))
  else if (sort === 'durationDesc') copy.sort((a, b) => b.durationSeconds - a.durationSeconds)
  else if (sort === 'durationAsc') copy.sort((a, b) => a.durationSeconds - b.durationSeconds)
  return copy
}

export function LibraryToolbar({
  library,
  loaded,
  visibleCount,
  search,
  filter,
  dropNotice,
  playlistNotice,
  busy,
  supports,
}: {
  library: LibraryState
  loaded: LoadedState
  visibleCount: number
  search: string
  filter: LibraryFilter
  dropNotice: string
  playlistNotice: string
  busy: boolean
  supports: (method: string) => boolean
}) {
  const [moreMenuOpen, setMoreMenuOpen] = React.useState(false)

  return (
    <>
      {/* Header Strip: Single primary CTA + icon overflow menu */}
      <div className="flex flex-wrap items-center justify-between gap-2.5" onClick={(event) => event.stopPropagation()}>
        <div className="flex items-baseline gap-2.5">
          <h1 className="text-title font-semibold tracking-tight text-foreground">演出曲库</h1>
          <span
            className="rounded-[4px] bg-muted/70 px-2 py-0.5 text-xs tabular-nums text-muted-foreground"
            data-testid="library-count"
          >
            显示 {visibleCount} / {library.entries.length} 个曲目
          </span>
        </div>

        <div className="flex items-center gap-2">
          <Button
            variant="default"
            size="sm"
            className="h-[var(--gaku-control-height,34px)] px-3.5 text-sm"
            disabled={busy}
            onClick={() => void motionStore.openImportNew([])}
          >
            <RiAddFill className="size-4" />
            添加到曲库
          </Button>

          <Popover open={moreMenuOpen} onOpenChange={setMoreMenuOpen}>
            <PopoverTrigger asChild>
              <Button
                type="button"
                variant="secondary"
                size="icon"
                className="size-[var(--gaku-control-height,34px)]"
                aria-label="更多曲库操作"
                title="刷新、文件夹与重载"
              >
                <RiMoreFill className="size-4" />
              </Button>
            </PopoverTrigger>
            <PopoverContent side="bottom" align="end" className="w-48 p-1">
              <button
                type="button"
                disabled={busy || !supports('library.refresh')}
                onClick={() => {
                  setMoreMenuOpen(false)
                  void motionStore.refreshLibrary()
                }}
                className="flex w-full items-center gap-2 rounded px-2.5 py-1.5 text-left text-sm text-foreground transition-colors hover:bg-accent disabled:pointer-events-none disabled:opacity-45"
              >
                <RiRefreshFill className={cn('size-3.5 text-muted-foreground', library.refreshing && 'animate-spin')} />
                <span>{library.refreshing ? '正在刷新...' : '刷新曲库'}</span>
              </button>
              <button
                type="button"
                disabled={!supports('library.openFolder')}
                onClick={() => {
                  setMoreMenuOpen(false)
                  void motionStore.openLibraryFolder()
                }}
                className="flex w-full items-center gap-2 rounded px-2.5 py-1.5 text-left text-sm text-foreground transition-colors hover:bg-accent disabled:pointer-events-none disabled:opacity-45"
              >
                <RiFolderOpenFill className="size-3.5 text-muted-foreground" />
                <span>打开曲库文件夹</span>
              </button>
              <button
                type="button"
                disabled={loaded.motionKey === '' || loaded.sourceMissing || busy || !supports('player.reload')}
                onClick={() => {
                  setMoreMenuOpen(false)
                  void motionStore.reloadMotion()
                }}
                className="flex w-full items-center gap-2 rounded px-2.5 py-1.5 text-left text-sm text-foreground transition-colors hover:bg-accent disabled:pointer-events-none disabled:opacity-45"
              >
                <RiRefreshFill className="size-3.5 text-muted-foreground" />
                <span>重新加载当前曲目</span>
              </button>
            </PopoverContent>
          </Popover>

          {!supports('library.import') && <PendingBadge />}
        </div>
      </div>

      {/* Notices */}
      {(dropNotice || library.notice || (playlistNotice && playlistNotice !== library.notice) || loaded.sourceMissing || loaded.sourceChanged) && (
        <div className="space-y-1" onClick={(event) => event.stopPropagation()}>
          {dropNotice && (
            <p className="rounded-md border border-warning/35 bg-warning/10 px-2.5 py-1.5 text-xs text-warning" data-testid="library-drop-notice">
              {dropNotice}
            </p>
          )}
          {library.notice && (
            <p className="rounded-md bg-card px-2.5 py-1.5 text-xs text-muted-foreground">{library.notice}</p>
          )}
          {playlistNotice && playlistNotice !== library.notice && (
            <p
              className={cn(
                'rounded-md border px-2.5 py-1.5 text-xs',
                /失败|错误/.test(playlistNotice)
                  ? 'border-destructive/40 bg-destructive/10 text-destructive'
                  : 'border-primary/30 bg-primary/10 text-primary',
              )}
            >
              {playlistNotice}
            </p>
          )}
          {loaded.sourceMissing && (
            <p className="rounded-md border border-warning/40 bg-warning/10 px-2.5 py-1.5 text-xs text-warning">
              当前表演的源文件已移除；已加载内容保留，无法重新加载。
            </p>
          )}
          {!loaded.sourceMissing && loaded.sourceChanged && (
            <p className="rounded-md border border-warning/40 bg-warning/10 px-2.5 py-1.5 text-xs text-warning">
              当前曲目的文件已更新，待重新加载。
            </p>
          )}
        </div>
      )}

      {/* Search + Funnel Dropdown Filter Toolbar */}
      <div className="flex items-center gap-2" onClick={(event) => event.stopPropagation()}>
        <div className="relative min-w-[220px] flex-1 sm:max-w-[280px]">
          <RiSearchLine className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
          <Input
            className="h-[var(--gaku-control-height,34px)] rounded-full pl-9 pr-14 text-sm"
            placeholder="搜索..."
            aria-label="搜索曲目"
            value={search}
            onChange={(event) => motionStore.setLibrarySearch(event.target.value)}
          />
          {search !== '' && (
            <Button
              type="button"
              variant="ghost"
              size="sm"
              className="absolute right-1.5 top-1/2 h-6 -translate-y-1/2 px-2 text-xs"
              onClick={() => motionStore.setLibrarySearch('')}
            >
              清空
            </Button>
          )}
        </div>

        <DropdownMenu>
          <DropdownMenuTrigger asChild>
            <Button
              type="button"
              variant={filter !== 'all' ? 'secondary' : 'ghost'}
              size="icon"
              className={cn(
                'relative size-[var(--gaku-control-height,34px)]',
                filter !== 'all' ? 'text-primary' : 'text-muted-foreground hover:text-foreground',
              )}
              aria-label="轨道筛选"
              title="轨道筛选"
            >
              <RiFilter2Fill className="size-4" />
              {filter !== 'all' && (
                <span aria-hidden="true" className="absolute right-1.5 top-1.5 size-1.5 rounded-full bg-primary" />
              )}
            </Button>
          </DropdownMenuTrigger>
          <DropdownMenuContent align="end">
            <DropdownMenuLabel>按轨道筛选</DropdownMenuLabel>
            <DropdownMenuSeparator />
            {FILTER_OPTIONS.map((opt) => (
              <DropdownMenuCheckboxItem
                key={opt.value}
                checked={filter === opt.value}
                onCheckedChange={() =>
                  motionStore.setLibraryFilter(filter === opt.value && opt.value !== 'all' ? 'all' : opt.value)
                }
              >
                {opt.label}
              </DropdownMenuCheckboxItem>
            ))}
          </DropdownMenuContent>
        </DropdownMenu>
      </div>
    </>
  )
}
