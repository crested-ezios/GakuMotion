import { isSupportedImageExtension, isSupportedLibraryFile } from '../../validation'
import type { SliceBundle, StoreContext } from '../context'
import { normalizeDropPeek } from '../normalizersPhase2'
import type { DialogTarget } from '../types'

export function createDialogDropSlice(ctx: StoreContext): SliceBundle<ReturnType<typeof buildActions>> {
  let dialogTimer: ReturnType<typeof setInterval> | null = null
  let dialogId = 0
  let dialogTarget: DialogTarget | null = null
  let dropTimer: ReturnType<typeof setInterval> | null = null
  let dropWatchCount = 0
  let dropTargetActive = false

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
    if (!ready) return
    try {
      const peek = normalizeDropPeek(await ctx.client().request('drop.peek'))
      ctx.set({ drop: peek })
      const poll = await ctx.client().request('drop.poll')
      const files = Array.isArray(poll?.files) ? poll.files : []
      if (files.length > 0) actions.handleDropFiles(files)
    } catch {
      // Transient polling failures are non-fatal; the next tick retries.
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
        if (dropTimer || !ctx.supports('drop.setTarget') || !ctx.supports('drop.poll')) return
        dropTimer = setInterval(() => void tickDrop(), 200)
        void tickDrop()
      },

      stopDropWatch: (): void => {
        if (dropTimer) {
          clearInterval(dropTimer)
          dropTimer = null
        }
        if (dropTargetActive) {
          dropTargetActive = false
          void ctx.client().request('drop.setTarget', { enabled: false }).catch(() => {})
        }
        if (ctx.getState().drop.visual !== 'none') {
          ctx.set({ drop: { visual: 'none', fileCount: 0, accepted: 0, hostElevated: false, hostBlocked: false } })
        }
      },

      syncDropTarget: (): void => {
        if (!dropTimer) return
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
    if (dropTimer) {
      clearInterval(dropTimer)
      dropTimer = null
    }
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
