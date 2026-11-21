import { MotionMockBridgeClient } from '../../bridge'
import type { ImportProgress, LibraryState } from '../../protocol'
import type { SliceBundle, StoreContext } from '../context'
import { emptyImportUi, type DropUiState, type ImportCoverMode, type MotionAppState } from '../types'

export interface PendingImportOutcome {
  editKey: string
  existingIds: Set<string>
  coverMode: ImportCoverMode
  coverDataUri: string
  succeededKey: string | null
}

export interface ImportProgressHooks {
  getPendingEditKey: () => string | null
  setPendingOutcome: (next: PendingImportOutcome | null) => void
  maybeFinalizeImportSelection: (library: LibraryState) => void
  onImportProgress: (importProgress: ImportProgress) => void
  startImportUiTimer: () => void
  stopImportUiTimer: () => void
}

export function createImportProgressSlice(
  ctx: StoreContext,
): SliceBundle<ReturnType<typeof buildActions>> & ImportProgressHooks {
  let importUiTimer: ReturnType<typeof setInterval> | null = null
  let pendingImportOutcome: PendingImportOutcome | null = null

  const stopImportUiTimer = (): void => {
    if (importUiTimer) {
      clearInterval(importUiTimer)
      importUiTimer = null
    }
  }

  const tickImportUi = (): void => {
    const state = ctx.getState()
    const ui = state.importUi
    if (!ui.visible) {
      stopImportUiTimer()
      return
    }
    const now = Date.now()
    const progress = ui.lastProgress
    const importBusy = state.importProgress?.active === true
    if (progress) {
      if (progress.active || importBusy) {
        if (ui.awaitingStart) ctx.set({ importUi: { ...ui, awaitingStart: false } })
        return
      }
      if (progress.finished) {
        if (progress.succeeded && now >= ui.minDisplayUntil) {
          ctx.set({ importUi: { ...ui, visible: false } })
          stopImportUiTimer()
        }
        return
      }
      return
    }
    if (!importBusy && (!ui.awaitingStart || now >= ui.minDisplayUntil)) {
      ctx.set({ importUi: { ...ui, visible: false } })
      stopImportUiTimer()
      return
    }
    if (!ui.indeterminate || ui.displayStatus === '') {
      ctx.set({
        importUi: {
          ...ui,
          indeterminate: true,
          displayProgress: 0.5,
          displayStatus: ui.displayStatus !== '' ? ui.displayStatus : '正在导入演出曲目...',
        },
      })
    }
  }

  const startImportUiTimer = (): void => {
    if (importUiTimer) return
    importUiTimer = setInterval(() => tickImportUi(), 100)
  }

  const maybeFinalizeImportSelection = (library: LibraryState): void => {
    const pending = pendingImportOutcome
    if (!pending) return
    let targetKey = ''
    if (pending.succeededKey && library.entries.some((entry) => entry.id === pending.succeededKey)) {
      targetKey = pending.succeededKey
    } else if (pending.editKey !== '') {
      if (pending.succeededKey === null) return
      if (library.entries.some((entry) => entry.id === pending.editKey)) {
        targetKey = pending.editKey
      }
    } else {
      const added = library.entries.find((entry) => !pending.existingIds.has(entry.id))
      if (added) targetKey = added.id
    }
    if (!targetKey) return
    const isNew = pending.editKey === ''
    const customCover = pending.coverMode === 'custom' ? pending.coverDataUri : ''
    pendingImportOutcome = null
    const state = ctx.getState()
    const patch: Partial<MotionAppState> = { selectedLibraryKey: targetKey }
    if (customCover) {
      patch.coverCache = { ...state.coverCache, [targetKey]: customCover }
    }
    ctx.set(patch)
    if (
      isNew &&
      state.loaded.motionKey !== targetKey &&
      state.player.state !== 'playing' &&
      ctx.supports('library.select')
    ) {
      void ctx.store.selectMotion(targetKey, false)
    }
  }

  const onImportProgress = (importProgress: ImportProgress): void => {
    ctx.set({ importProgress })
    const state = ctx.getState()
    const ui = state.importUi
    const now = Date.now()
    const staleSuccess =
      ui.visible && ui.awaitingStart && importProgress.finished && importProgress.succeeded && now < ui.startTime + 250
    if (!staleSuccess && importProgress.finished) {
      if (importProgress.succeeded) {
        if (pendingImportOutcome) {
          pendingImportOutcome.succeededKey = importProgress.importedKey || pendingImportOutcome.editKey || ''
          maybeFinalizeImportSelection(state.library)
        }
      } else {
        pendingImportOutcome = null
      }
    }
    if (!ui.visible) return
    const next = { ...ui, lastProgress: importProgress }
    if (importProgress.active) next.awaitingStart = false
    if (!staleSuccess && importProgress.finished) {
      next.awaitingStart = false
      if (importProgress.succeeded) {
        if (now >= ui.minDisplayUntil) {
          next.visible = false
        } else {
          next.displayProgress = 1
          next.displayStatus = importProgress.statusText !== '' ? importProgress.statusText : '导入完成'
          next.indeterminate = false
        }
      } else {
        next.visible = false
        next.errorModalOpen = true
        next.errorDetail =
          importProgress.errorDetail !== ''
            ? importProgress.errorDetail
            : state.playlist.notice !== ''
              ? state.playlist.notice
              : '原生导入遇到未知错误，请查看导入日志'
      }
    } else if (!staleSuccess) {
      next.displayProgress = importProgress.progress
      next.displayStatus =
        importProgress.statusText !== ''
          ? importProgress.statusText
          : importProgress.stage !== ''
            ? importProgress.stage
            : '正在处理演出数据...'
      next.indeterminate = false
    }
    ctx.set({ importUi: next })
    if (!next.visible) stopImportUiTimer()
  }

  const actions = buildActions()

  function buildActions() {
    return {
      closeImportError: (): void => {
        ctx.set({ importUi: { ...ctx.getState().importUi, errorModalOpen: false, errorDetail: '' } })
      },

      // ---- Preview-only modal & state triggers ----
      previewImportFlow: (outcome: 'success' | 'error'): void => {
        const now = Date.now()
        ctx.set({
          importUi: {
            ...emptyImportUi(),
            visible: true,
            awaitingStart: true,
            startTime: now,
            minDisplayUntil: now + (outcome === 'success' ? 1350 : 550),
            displayProgress: 0.15,
            displayStatus: outcome === 'error' ? '正在启动模拟导入（将触发出错）...' : '正在启动模拟导入流程...',
            lastProgress: null,
          },
          actionError: null,
        })
        startImportUiTimer()
        const bridge = ctx.getBridge()
        if (bridge instanceof MotionMockBridgeClient) {
          bridge.simulateImportProgress(outcome)
        }
      },

      previewImportErrorDirect: (
        detail = "System.IO.IOException: The process cannot access the file 'audio.wav' because it is being used by another process.",
      ): void => {
        stopImportUiTimer()
        ctx.set({
          importUi: {
            ...emptyImportUi(),
            errorModalOpen: true,
            errorDetail: detail,
          },
        })
      },

      previewDeleteDialog: (stale = false): void => {
        const state = ctx.getState()
        const entries = state.library.entries
        const target =
          entries.find((item) => item.id === state.selectedLibraryKey) ??
          entries[0] ??
          null
        if (!target) return
        ctx.set({
          page: 'library',
          selectedLibraryKey: target.id,
          pendingDelete: {
            key: target.id,
            directory: target.directory,
            revision: state.library.revision,
          },
          deleteStale: stale,
        })
      },

      previewDropOverlay: (drop: DropUiState): void => {
        ctx.set({ drop })
        const bridge = ctx.getBridge()
        if (bridge instanceof MotionMockBridgeClient) {
          bridge.setDropPeekPreview(drop)
        }
      },

      previewActionError: (actionError: string | null): void => {
        ctx.set({ actionError })
      },

      previewDisconnect: (message = '模拟原生桥接异常断开（可用于验收断连重连界面）'): void => {
        ctx.set({ ready: true, connected: false, error: message })
      },
    }
  }

  return {
    actions,
    getPendingEditKey: () => pendingImportOutcome?.editKey ?? null,
    setPendingOutcome: (next) => {
      pendingImportOutcome = next
    },
    maybeFinalizeImportSelection,
    onImportProgress,
    startImportUiTimer,
    stopImportUiTimer,
    stopWatchers: stopImportUiTimer,
    reset: () => {
      stopImportUiTimer()
      pendingImportOutcome = null
    },
  }
}

export type ImportProgressActions = ReturnType<typeof createImportProgressSlice>['actions']
