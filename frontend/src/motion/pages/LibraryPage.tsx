import * as React from 'react'
import {
  RiAddFill,
  RiArrowDownFill,
  RiArrowUpDownFill,
  RiArrowUpFill,
  RiDeleteBinFill,
  RiEyeFill,
  RiImageEditFill,
  RiPauseFill,
  RiPencilFill,
  RiPlayFill,
} from '@remixicon/react'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog'
import { Button } from '@/components/ui/button'
import type { LibraryEntry, PlaybackStatus } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { ContextMenu, type ContextMenuEntry } from '../components/ContextMenu'
import { CoverEditorModal } from '../components/CoverEditorModal'
import { ImportDialog } from './ImportDialog'
import { LibraryRow } from './library/LibraryRow'
import { LibraryToolbar, filterAndSortEntries } from './library/LibraryToolbar'

const ROW_HEIGHT = 56
const VIRTUALIZE_THRESHOLD = 200

export function LibraryPage() {
  const library = useMotionStore((state) => state.library)
  const loaded = useMotionStore((state) => state.loaded)
  const playerState = useMotionStore((state) => state.player.state)
  const selectedKey = useMotionStore((state) => state.selectedLibraryKey)
  const search = useMotionStore((state) => state.librarySearch)
  const filter = useMotionStore((state) => state.libraryFilter)
  const sort = useMotionStore((state) => state.librarySort)
  const pendingDelete = useMotionStore((state) => state.pendingDelete)
  const playlistNotice = useMotionStore((state) => state.playlist.notice)
  const dropNotice = useMotionStore((state) => state.dropNotice)
  const importFormOpen = useMotionStore((state) => state.importForm.open)
  const importUiVisible = useMotionStore((state) => state.importUi.visible)
  const coverEditorOpen = useMotionStore((state) => state.coverEditor.open)
  const dialogBusy = useMotionStore((state) => state.dialogBusy)
  const injectionAllowed = useMotionStore((state) => state.permission.injectionAllowed)
  const capabilities = useMotionStore((state) => state.capabilities)
  const importBusy = useMotionStore((state) => state.importProgress?.active === true)

  const [contextMenu, setContextMenu] = React.useState<{
    open: boolean
    x: number
    y: number
    entry: LibraryEntry | null
  }>({ open: false, x: 0, y: 0, entry: null })

  const supports = (method: string) => capabilities.includes(method)
  const busy = library.refreshing || importBusy

  React.useEffect(() => {
    motionStore.retainDropWatch()
    return () => motionStore.releaseDropWatch()
  }, [capabilities])

  React.useEffect(() => {
    motionStore.syncDropTarget()
  }, [injectionAllowed, importFormOpen, importUiVisible, coverEditorOpen, dialogBusy, busy, pendingDelete])

  const visibleEntries = React.useMemo(
    () => filterAndSortEntries(library.entries, search, filter, sort),
    [library.entries, search, filter, sort],
  )

  // Lightweight windowing for large libraries.
  const scrollRef = React.useRef<HTMLDivElement>(null)
  const [scrollTop, setScrollTop] = React.useState(0)
  const [viewportHeight, setViewportHeight] = React.useState(480)
  React.useEffect(() => {
    const element = scrollRef.current
    if (!element) return
    const observer = new ResizeObserver(() => setViewportHeight(element.clientHeight))
    observer.observe(element)
    setViewportHeight(element.clientHeight)
    return () => observer.disconnect()
  }, [visibleEntries.length])

  const virtualize = visibleEntries.length > VIRTUALIZE_THRESHOLD
  const startIndex = virtualize ? Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - 5) : 0
  const endIndex = virtualize
    ? Math.min(visibleEntries.length, Math.ceil((scrollTop + viewportHeight) / ROW_HEIGHT) + 5)
    : visibleEntries.length
  const rendered = visibleEntries.slice(startIndex, endIndex)

  const deleteChanged = motionStore.deleteGuardChanged()

  const loadedStatusFor = (entry: LibraryEntry): PlaybackStatus | null => {
    if (loaded.motionKey !== entry.id) return null
    return playerState
  }

  const handlePlayEntry = (entry: LibraryEntry) => {
    if (loaded.motionKey === entry.id && (playerState === 'paused' || playerState === 'preview')) {
      void motionStore.play()
    } else {
      void motionStore.selectMotion(entry.id, true)
    }
  }

  const cycleTitleSort = () => {
    motionStore.setLibrarySort(sort === 'title' ? 'default' : 'title')
  }

  const cycleDurationSort = () => {
    if (sort === 'durationDesc') motionStore.setLibrarySort('durationAsc')
    else if (sort === 'durationAsc') motionStore.setLibrarySort('default')
    else motionStore.setLibrarySort('durationDesc')
  }

  const contextItems = React.useMemo<ContextMenuEntry[]>(() => {
    const entry = contextMenu.entry
    if (!entry) return []
    const isPlaying = loaded.motionKey === entry.id && playerState === 'playing'
    return [
      isPlaying
        ? {
            id: 'pause',
            label: '暂停播放',
            icon: <RiPauseFill className="size-4" />,
            disabled: busy || !supports('player.pause'),
            onSelect: () => void motionStore.pause(),
          }
        : {
            id: 'play',
            label: '播放此曲目',
            icon: <RiPlayFill className="size-4" />,
            disabled: busy || !supports('library.select'),
            onSelect: () => handlePlayEntry(entry),
          },
      {
        id: 'preview',
        label: '加载预览',
        icon: <RiEyeFill className="size-4" />,
        disabled: busy || !supports('library.select'),
        onSelect: () => void motionStore.selectMotion(entry.id, false),
      },
      {
        id: 'edit',
        label: '管理轨道与同步…',
        icon: <RiPencilFill className="size-4" />,
        disabled: busy || !supports('library.startEdit'),
        onSelect: () => void motionStore.openImportEdit(entry.id),
      },
      {
        id: 'cover',
        label: '编辑封面…',
        icon: <RiImageEditFill className="size-4" />,
        disabled: busy,
        onSelect: () => motionStore.openCoverEditorForTrack(entry.id, 'library'),
      },
      { type: 'separator', id: 'sep-1' },
      {
        id: 'delete',
        label: '从曲库删除…',
        icon: <RiDeleteBinFill className="size-4" />,
        destructive: true,
        disabled: busy || !supports('library.delete'),
        onSelect: () => motionStore.requestDelete(entry.id),
      },
    ]
  }, [contextMenu.entry, loaded.motionKey, playerState, busy, capabilities])

  return (
    <div
      className="flex h-full flex-col gap-3"
      data-testid="library-page"
      onClick={() => motionStore.selectLibrary(null)}
    >
      <LibraryToolbar
        library={library}
        loaded={loaded}
        visibleCount={visibleEntries.length}
        search={search}
        filter={filter}
        dropNotice={dropNotice}
        playlistNotice={playlistNotice}
        busy={busy}
        supports={supports}
      />

      {/* Implicit Borderless List (horizontal padding prevents left/right card shadow clipping) */}
      {library.entries.length === 0 ? (
        <div
          className="flex flex-1 flex-col items-center justify-center gap-3 rounded-lg border border-dashed bg-card/40 p-8 text-center"
          onClick={(event) => event.stopPropagation()}
        >
          <p className="text-sm font-medium text-foreground">曲库为空</p>
          <p className="max-w-sm text-xs text-muted-foreground">
            点击“添加到曲库”或直接将 VMD / GKCamera / 音频文件（.mp3/.flac/.wav 等）拖入窗口；也可将动作文件夹放入曲库后点击右上角菜单刷新。
          </p>
          <Button size="sm" disabled={busy} onClick={() => void motionStore.openImportNew([])}>
            <RiAddFill className="size-4" />
            导入首个演出
          </Button>
        </div>
      ) : visibleEntries.length === 0 ? (
        <div
          className="flex flex-1 flex-col items-center justify-center gap-2 rounded-lg border border-dashed bg-card/40 p-8 text-center"
          onClick={(event) => event.stopPropagation()}
        >
          <p className="text-sm text-muted-foreground">没有匹配的演出曲目</p>
          <Button
            variant="outline"
            size="sm"
            onClick={() => {
              motionStore.setLibrarySearch('')
              motionStore.setLibraryFilter('all')
            }}
          >
            重置搜索与筛选
          </Button>
        </div>
      ) : (
        <div
          ref={scrollRef}
          role="table"
          aria-label="演出曲库列表"
          className="-mx-2 min-h-0 flex-1 overflow-y-auto px-2"
          onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}
          onClick={() => motionStore.selectLibrary(null)}
          data-testid="library-table-scroll"
        >
          {/* Minimal Implicit Header */}
          <div
            role="row"
            onClick={(event) => event.stopPropagation()}
            className="sticky top-0 z-10 flex items-center gap-3.5 bg-background/95 px-3 py-2 text-xs font-medium text-muted-foreground backdrop-blur-sm"
          >
            <div role="columnheader" className="w-8 shrink-0 text-center">
              #
            </div>
            <div role="columnheader" className="min-w-0 flex-1">
              <button
                type="button"
                onClick={cycleTitleSort}
                className="inline-flex items-center gap-1 rounded px-1 py-0.5 text-xs font-medium text-muted-foreground transition-colors hover:text-foreground"
              >
                <span>演出曲目</span>
                {sort === 'title' ? (
                  <RiArrowUpFill className="size-3.5 text-primary" />
                ) : (
                  <RiArrowUpDownFill className="size-3.5 opacity-55" />
                )}
              </button>
            </div>
            <div role="columnheader" className="hidden w-[180px] shrink-0 sm:block">
              包含内容
            </div>
            <div role="columnheader" className="w-16 shrink-0 text-right">
              <button
                type="button"
                onClick={cycleDurationSort}
                className="inline-flex items-center justify-end gap-1 rounded px-1 py-0.5 text-xs font-medium text-muted-foreground transition-colors hover:text-foreground"
              >
                <span>时长</span>
                {sort === 'durationDesc' ? (
                  <RiArrowDownFill className="size-3.5 text-primary" />
                ) : sort === 'durationAsc' ? (
                  <RiArrowUpFill className="size-3.5 text-primary" />
                ) : (
                  <RiArrowUpDownFill className="size-3.5 opacity-55" />
                )}
              </button>
            </div>
          </div>

          <div role="rowgroup" className="space-y-1 py-1.5">
            {virtualize && startIndex > 0 && <div aria-hidden style={{ height: startIndex * ROW_HEIGHT }} />}
            {rendered.map((entry, idx) => (
              <LibraryRow
                key={entry.id}
                entry={entry}
                index={startIndex + idx}
                selected={selectedKey === entry.id}
                loadedStatus={loadedStatusFor(entry)}
                busy={busy}
                canSelect={supports('library.select')}
                canPause={supports('player.pause')}
                canEdit={supports('library.startEdit')}
                canDelete={supports('library.delete')}
                style={virtualize ? { height: ROW_HEIGHT } : undefined}
                onSelect={() => motionStore.selectLibrary(entry.id)}
                onDoubleClick={() => void motionStore.selectMotion(entry.id, true)}
                onContextMenu={(event) => {
                  event.preventDefault()
                  motionStore.selectLibrary(entry.id)
                  setContextMenu({ open: true, x: event.clientX, y: event.clientY, entry })
                }}
                onPlay={() => handlePlayEntry(entry)}
                onPause={() => void motionStore.pause()}
                onPreview={() => void motionStore.selectMotion(entry.id, false)}
                onEdit={() => void motionStore.openImportEdit(entry.id)}
                onDelete={() => motionStore.requestDelete(entry.id)}
              />
            ))}
            {virtualize && endIndex < visibleEntries.length && (
              <div aria-hidden style={{ height: (visibleEntries.length - endIndex) * ROW_HEIGHT }} />
            )}
          </div>
        </div>
      )}

      {/* Row Right-Click Context Menu */}
      <ContextMenu
        open={contextMenu.open}
        x={contextMenu.x}
        y={contextMenu.y}
        items={contextItems}
        onClose={() => setContextMenu((prev) => ({ ...prev, open: false }))}
        testId="library-context-menu"
      />

      {/* Single Delete Confirmation */}
      <AlertDialog
        open={pendingDelete !== null}
        onOpenChange={(open) => {
          if (!open) motionStore.cancelDelete()
        }}
      >
        <AlertDialogContent
          className="flex max-h-[85vh] w-[calc(100vw-2rem)] max-w-lg flex-col gap-0 overflow-hidden p-0"
          data-testid="delete-dialog"
        >
          <AlertDialogHeader className="shrink-0 border-b border-border/60 px-5 pt-5 pb-3">
            <AlertDialogTitle>删除选中确认</AlertDialogTitle>
          </AlertDialogHeader>
          <div className="min-h-0 flex-1 overflow-y-auto px-5 py-4">
            <AlertDialogDescription render={<div />} className="min-w-0 space-y-2.5 text-sm text-muted-foreground">
              <p className="min-w-0 break-all [overflow-wrap:anywhere]">
                删除曲目：<span className="font-medium text-foreground">{pendingDelete?.key}</span>
              </p>
              <div className="min-w-0 space-y-1">
                <p>将永久删除整个曲目目录及其中全部文件：</p>
                <p className="min-w-0 rounded-md border border-border/60 bg-muted/40 px-2.5 py-1.5 font-mono text-xs text-foreground break-all [overflow-wrap:anywhere]">
                  {pendingDelete?.directory}
                </p>
              </div>
              <p>此操作无法撤销，文件不会进入回收站。确定删除选中曲目？</p>
              {deleteChanged && <p className="text-warning">曲库已变化，请取消后重新选择曲目。</p>}
            </AlertDialogDescription>
          </div>
          <AlertDialogFooter className="shrink-0 border-t border-border/60 bg-background px-5 py-3.5">
            <AlertDialogCancel onClick={() => motionStore.cancelDelete()}>取消删除</AlertDialogCancel>
            <AlertDialogAction
              disabled={deleteChanged || busy}
              onClick={(event) => {
                event.preventDefault()
                void motionStore.confirmDelete()
              }}
            >
              永久删除
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      <ImportDialog />
      <CoverEditorModal owner="library" />
    </div>
  )
}
