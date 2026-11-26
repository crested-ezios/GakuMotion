import type { DropPeek } from '../../protocol'
import { isSupportedImageExtension, isSupportedLibraryFile } from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeDropPeek } from '../normalizersPhase2'
import type { DialogTarget } from '../types'

const DROP_IDLE_POLL_MS = 300
const DROP_ACTIVE_POLL_MS = 150
const DROP_HIDDEN_POLL_MS = 1000

function sameDropPeek(a: DropPeek, b: DropPeek): boolean {
  return (
    a.visual === b.visual &&
    a.fileCount === b.fileCount &&
    a.accepted === b.accepted &&
    a.hostElevated === b.hostElevated &&
    a.hostBlocked === b.hostBlocked
  )
}

export function createDialogDropSlice(ctx: StoreContext): SliceBundle<ReturnType<typeof buildActions>> {
  let dialogTimer: ReturnType<typeof setInterval> | null = null
  let dialogId = 0
  let dialogTarget: DialogTarget | null = null
  let dropTimer: ReturnType<typeof setTimeout> | null = null
  let dropWatchRunning = false
  let dropTickInFlight = false
  let dropResyncRequested = false
  let wasDropActive = false
  let idlePollCounter = 0
  let visibilityListening = false
  let dropWatchCount = 0
  let dropTargetActive = false

  const isDocumentHidden = (): boolean =>
    typeof document !== 'undefined' && document.hidden === true

  const clearDropTimer = (): void => {
    if (dropTimer) {
      clearTimeout(dropTimer)
      dropTimer = null
    }
  }

  const scheduleNextDropTick = (delayMs: number): void => {
    clearDropTimer()
    if (!dropWatchRunning) return
    dropTimer = setTimeout(() => {
      dropTimer = null
      void tickDrop()
    }, delayMs)
  }

  const onVisibilityChange = (): void => {
    if (!dropWatchRunning || isDocumentHidden()) return
    scheduleNextDropTick(0)
  }

  const detachVisibilityListener = (): void => {
    if (visibilityListening && typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', onVisibilityChange)
      visibilityListening = false
    }
  }

  const finishDialog = (): void => {
    if (dialogTimer) {
      clearInterval(dialogTimer)
      dialogTimer = null
    }
    ctx.set({ dialogBusy: false })
  }

  const pollDialog = async (): Promise<void> => {
    if (!ctx.getState().dialogBusy) return
    try {
      const result = await ctx.client().request('dialog.poll', { dialogId })
      if (result.state === 'pending') return
      finishDialog()
      if (result.state === 'accepted' && result.paths.length > 0) {
        const target = dialogTarget
        dialogTarget = null
        if (target === 'library-add') await ctx.store.openImportNew(result.paths)
        else if (target === 'append') await ctx.store.importAppendFiles(result.paths)
        else if (target === 'video') ctx.set({ studioVideoPath: result.paths[0] })
        else if (target === 'cover-image') await ctx.store.loadCoverPreviewFromPath(result.paths[0])
        else if (target?.startsWith('position-motion:')) {
          const posIndex = Number(target.slice('position-motion:'.length))
          await ctx.store.importPickPositionFile(posIndex, 'motion', result.paths[0])
        } else if (target?.startsWith('position-facial:')) {
          const posIndex = Number(target.slice('position-facial:'.length))
          await ctx.store.importPickPositionFile(posIndex, 'facial', result.paths[0])
        } else if (target === 'motion' || target === 'facial' || target === 'camera' || target === 'audio') {
          await ctx.store.importPickSlotFile(target, result.paths[0])
        }
      } else if (result.state === 'failed' && result.notice !== '') {
        ctx.set({ actionError: result.notice })
      }
    } catch (error) {
      finishDialog()
      ctx.reportActionError(error)
    }
  }

  const tickDrop = async (): Promise<void> => {
    if (!dropWatchRunning) return
    if (dropTickInFlight) {
      dropResyncRequested = true
      return
    }
    if (isDocumentHidden()) {
      scheduleNextDropTick(DROP_HIDDEN_POLL_MS)
      return
    }

    dropTickInFlight = true
    let nextDelay = DROP_IDLE_POLL_MS
    try {
      const ready = actions.canAcceptDrop()
      if (ready !== dropTargetActive) {
        dropTargetActive = ready
        if (!ready && ctx.getState().drop.visual !== 'none') {
          ctx.set({ drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false } })
        }
        try {
          await ctx.client().request('drop.setTarget', { enabled: ready })
        } catch {
          dropTargetActive = false
        }
      }
      if (ready && dropWatchRunning) {
        try {
          const peek = normalizeDropPeek(await ctx.client().request('drop.peek'))
          if (!dropWatchRunning) return
          if (!sameDropPeek(ctx.getState().drop, peek)) {
            ctx.set({ drop: peek })
          }
          const dragOrPending = peek.visual !== 'none' || peek.fileCount > 0
          // Poll drop.poll immediately during an active drag / pending batch (and on the tick
          // immediately following one); when idle, back off drop.poll to every other tick.
          const shouldPoll = dragOrPending || wasDropActive || idlePollCounter % 2 === 0
          idlePollCounter = dragOrPending ? 0 : (idlePollCounter + 1) % 2
          wasDropActive = dragOrPending
          if (dragOrPending) nextDelay = DROP_ACTIVE_POLL_MS
          if (shouldPoll) {
            const poll = await ctx.client().request('drop.poll')
            const files = Array.isArray(poll?.files) ? poll.files : []
            if (files.length > 0) actions.handleDropFiles(files)
          }
        } catch {
          // Transient polling failures are non-fatal; the next tick retries.
        }
      } else {
        wasDropActive = false
        idlePollCounter = 0
      }
    } finally {
      dropTickInFlight = false
      if (dropWatchRunning) {
        const delay = dropResyncRequested ? 0 : nextDelay
        dropResyncRequested = false
        scheduleNextDropTick(delay)
      } else {
        dropResyncRequested = false
      }
    }
  }

  const actions = buildActions()

  function buildActions() {
    return {
      browseFiles: async (target: DialogTarget): Promise<void> => {
        if (ctx.getState().dialogBusy || !ctx.supports('dialog.pickFile')) return
        const kind =
          target === 'library-add' || target === 'append'
            ? 0
            : target === 'motion' || target.startsWith('position-motion:')
              ? 1
              : target === 'facial' || target.startsWith('position-facial:')
                ? 2
                : target === 'camera'
                  ? 3
                  : target === 'audio'
                    ? 4
                    : target === 'cover-image'
                      ? 6
                      : 5
        const multiple = target === 'library-add' || target === 'append'
        try {
          const picked = await ctx.client().request('dialog.pickFile', { kind, multiple })
          dialogId = picked.dialogId
          dialogTarget = target
          ctx.set({ dialogBusy: true })
          dialogTimer = setInterval(() => void pollDialog(), 200)
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      canAcceptDrop: (): boolean => {
        const state = ctx.getState()
        if (!state.permission.injectionAllowed) return false
        if (state.importProgress?.active === true) return false
        if (state.dialogBusy || state.pendingDelete !== null) return false
        if (state.coverEditor.open) {
          return !state.coverEditor.saving && !state.coverEditor.capturing
        }
        return (
          state.page === 'library' &&
          !state.library.refreshing &&
          !state.importForm.open
        )
      },

      retainDropWatch: (): void => {
        dropWatchCount += 1
        actions.startDropWatch()
        actions.syncDropTarget()
      },

      releaseDropWatch: (): void => {
        dropWatchCount = Math.max(0, dropWatchCount - 1)
        if (dropWatchCount === 0) {
          actions.stopDropWatch()
        }
      },

      startDropWatch: (): void => {
        if (dropWatchRunning || !ctx.supports('drop.setTarget') || !ctx.supports('drop.poll')) return
        dropWatchRunning = true
        if (typeof document !== 'undefined' && !visibilityListening) {
          document.addEventListener('visibilitychange', onVisibilityChange)
          visibilityListening = true
        }
        void tickDrop()
      },

      stopDropWatch: (): void => {
        dropWatchRunning = false
        dropResyncRequested = false
        wasDropActive = false
        idlePollCounter = 0
        clearDropTimer()
        detachVisibilityListener()
        if (dropTargetActive) {
          dropTargetActive = false
          void ctx.client().request('drop.setTarget', { enabled: false }).catch(() => {})
        }
        if (ctx.getState().drop.visual !== 'none') {
          ctx.set({ drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false } })
        }
      },

      syncDropTarget: (): void => {
        if (!dropWatchRunning) return
        if (actions.canAcceptDrop() !== dropTargetActive) {
          void tickDrop()
        }
      },

      handleDropFiles: (files: string[]): void => {
        if (ctx.getState().coverEditor.open) {
          const imagePath = files.find((file) => isSupportedImageExtension(file))
          if (!imagePath) {
            ctx.store.setCoverEditorError('仅支持拖入图片文件（.png / .jpg / .jpeg / .webp / .bmp）。')
            return
          }
          void ctx.store.loadCoverPreviewFromPath(imagePath)
          return
        }
        const accepted: string[] = []
        let ignored = 0
        for (const file of files) {
          if (!isSupportedLibraryFile(file)) {
            ignored += 1
            continue
          }
          if (!accepted.includes(file)) accepted.push(file)
        }
        if (accepted.length === 0) {
          ctx.set({ dropNotice: '未找到可添加的文件。请拖入 VMD、GKCamera 或音频文件（.mp3/.flac/.wav/.ogg/.m4a/.aac/.opus）；不支持拖入文件夹。' })
          return
        }
        ctx.set({ dropNotice: ignored > 0 ? `已忽略 ${ignored} 个不支持或无法读取的文件。` : '' })
        void ctx.store.openImportNew(accepted)
      },
    }
  }

  const stopWatchers = () => {
    if (dialogTimer) {
      clearInterval(dialogTimer)
      dialogTimer = null
    }
    dropWatchRunning = false
    dropResyncRequested = false
    wasDropActive = false
    idlePollCounter = 0
    clearDropTimer()
    detachVisibilityListener()
    dropWatchCount = 0
    dropTargetActive = false
    dialogTarget = null
  }

  return {
    actions,
    stopWatchers,
    reset: () => {
      stopWatchers()
      dialogId = 0
    },
  }
}

export type DialogDropActions = ReturnType<typeof createDialogDropSlice>['actions']
