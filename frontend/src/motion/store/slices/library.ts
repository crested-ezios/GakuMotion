import { BridgeError, type LibraryEntry, type LibraryState } from '../../protocol'
import type { SliceBundle, StoreContext } from '../context'

export interface LibraryCoverHooks {
  onLibraryChangedCovers: (
    library: LibraryState,
    previousEntries: LibraryEntry[],
    editingKey: string | null,
  ) => Record<string, string> | null
  onCoverReady: (key: string, hasCover: boolean, coverDataUri: string) => void
  invalidateCoverCacheKey: (key: string) => void
}

export function createLibrarySlice(
  ctx: StoreContext,
): SliceBundle<ReturnType<typeof buildActions>> & LibraryCoverHooks {
  const COVER_CONCURRENCY = 2
  const coverPending = new Set<string>()
  const coverAwaitingReady = new Set<string>()
  const coverQueued = new Set<string>()
  const coverWaiters = new Map<string, Array<() => void>>()
  let coverGeneration = 0
  let coverQueue: string[] = []
  let coverWorkersRunning = 0
  let refreshCoverBustPending = false

  function resolveCoverWaiters(key: string): void {
    const callbacks = coverWaiters.get(key)
    if (!callbacks) return
    coverWaiters.delete(key)
    for (const cb of callbacks) cb()
  }

  function clearCoverQueueState(): void {
    coverPending.clear()
    coverAwaitingReady.clear()
    coverQueued.clear()
    const keys = [...coverWaiters.keys()]
    coverQueue = []
    for (const key of keys) resolveCoverWaiters(key)
  }

  async function runCoverFetch(key: string): Promise<void> {
    const state = ctx.getState()
    if (
      Object.prototype.hasOwnProperty.call(state.coverCache, key) ||
      coverPending.has(key) ||
      coverAwaitingReady.has(key)
    ) {
      return
    }
    const entry = state.library.entries.find((item) => item.id === key)
    if (!entry) return
    if (!entry.hasCover && !entry.hasAudio) {
      ctx.set({ coverCache: { ...state.coverCache, [key]: '' } })
      return
    }
    if (!ctx.supports('library.getCover')) return
    const generation = coverGeneration
    coverPending.add(key)
    try {
      const res = await ctx.client().request('library.getCover', { key })
      if (generation !== coverGeneration) return
      const cur = ctx.getState()
      if (!cur.library.entries.some((item) => item.id === key)) return
      if (res.pending) {
        coverAwaitingReady.add(key)
        return
      }
      coverAwaitingReady.delete(key)
      const uri = res.hasCover && res.coverDataUri ? res.coverDataUri : ''
      ctx.set({ coverCache: { ...cur.coverCache, [key]: uri } })
    } catch {
      // Leave cache clean on transient bridge error so it can retry later.
    } finally {
      coverPending.delete(key)
    }
  }

  function pumpCoverQueue(): void {
    while (coverWorkersRunning < COVER_CONCURRENCY && coverQueue.length > 0) {
      coverWorkersRunning += 1
      void (async () => {
        try {
          while (coverQueue.length > 0) {
            const nextId = coverQueue.shift()
            if (!nextId) continue
            coverQueued.delete(nextId)
            try {
              await runCoverFetch(nextId)
            } finally {
              resolveCoverWaiters(nextId)
            }
          }
        } finally {
          coverWorkersRunning -= 1
          if (coverQueue.length > 0 && coverWorkersRunning < COVER_CONCURRENCY) {
            pumpCoverQueue()
          }
        }
      })()
    }
  }

  const actions = buildActions()

  function buildActions() {
    return {
      refreshLibrary: async (): Promise<void> => {
        refreshCoverBustPending = true
        try {
          await ctx.client().request('library.refresh')
          ctx.set({ actionError: null })
        } catch (error) {
          refreshCoverBustPending = false
          ctx.reportActionError(error)
        }
      },

      openLibraryFolder: async (): Promise<void> => {
        try {
          await ctx.client().request('library.openFolder')
        } catch (error) {
          ctx.reportActionError(error)
        }
      },

      selectMotion: async (key: string, autoplay: boolean): Promise<void> => {
        try {
          await ctx.client().request('library.select', { key, autoplay, revision: ctx.getState().library.revision })
          ctx.set({ selectedLibraryKey: key, actionError: null })
        } catch (error) {
          if (error instanceof BridgeError && error.code === 'stale_revision') {
            ctx.set({ actionError: '曲库已变化，请刷新后重试。' })
          } else {
            ctx.reportActionError(error)
          }
        }
      },

      requestDelete: (key: string): void => {
        const state = ctx.getState()
        const entry = state.library.entries.find((candidate) => candidate.id === key)
        if (!entry || state.library.refreshing) return
        ctx.set({
          pendingDelete: { key, directory: entry.directory, revision: state.library.revision },
          deleteStale: false,
        })
      },

      cancelDelete: (): void => ctx.set({ pendingDelete: null, deleteStale: false }),

      deleteGuardChanged: (): boolean => {
        const state = ctx.getState()
        const pending = state.pendingDelete
        if (!pending) return false
        if (state.deleteStale) return true
        if (pending.revision !== state.library.revision) return true
        const entry = state.library.entries.find((candidate) => candidate.id === pending.key)
        return !entry || entry.directory !== pending.directory
      },

      confirmDelete: async (): Promise<void> => {
        const pending = ctx.getState().pendingDelete
        if (!pending || actions.deleteGuardChanged()) return
        try {
          await ctx.client().request('library.delete', { key: pending.key, revision: pending.revision })
          ctx.set({ pendingDelete: null, deleteStale: false, actionError: null })
        } catch (error) {
          if (error instanceof BridgeError && error.code === 'stale_revision') {
            ctx.set({ deleteStale: true })
          } else {
            ctx.reportActionError(error)
          }
        }
      },

      ensureTrackCover: (key: string): Promise<void> => {
        if (!key) return Promise.resolve()
        const state = ctx.getState()
        if (
          Object.prototype.hasOwnProperty.call(state.coverCache, key) ||
          coverPending.has(key) ||
          coverAwaitingReady.has(key)
        ) {
          return Promise.resolve()
        }
        const entry = state.library.entries.find((item) => item.id === key)
        if (!entry) return Promise.resolve()
        if (!entry.hasCover && !entry.hasAudio) {
          ctx.set({ coverCache: { ...state.coverCache, [key]: '' } })
          return Promise.resolve()
        }
        if (!ctx.supports('library.getCover')) return Promise.resolve()
        return new Promise<void>((resolve) => {
          const list = coverWaiters.get(key)
          if (list) list.push(resolve)
          else coverWaiters.set(key, [resolve])
          if (!coverQueued.has(key)) {
            coverQueued.add(key)
            coverQueue.push(key)
          }
          pumpCoverQueue()
        })
      },

      preloadLibraryCovers: (entries: LibraryEntry[], priorityIds: string[] = []): void => {
        if (!ctx.supports('library.getCover') || entries.length === 0) return
        const state = ctx.getState()
        const entryById = new Map(entries.map((item) => [item.id, item]))
        const seen = new Set<string>()
        const enqueueIfNeeded = (id: string | null | undefined) => {
          if (!id || seen.has(id)) return
          seen.add(id)
          const entry = entryById.get(id)
          if (!entry || (!entry.hasCover && !entry.hasAudio)) return
          void actions.ensureTrackCover(id)
        }

        enqueueIfNeeded(state.loaded.motionKey)
        enqueueIfNeeded(state.selectedLibraryKey)
        for (const id of priorityIds) enqueueIfNeeded(id)
        for (const entry of entries) enqueueIfNeeded(entry.id)
      },
    }
  }

  return {
    actions,
    onLibraryChangedCovers: (library, previousEntries, editingKey) => {
      const state = ctx.getState()
      if (library.revision === state.library.revision) return null
      coverGeneration += 1
      clearCoverQueueState()
      if (refreshCoverBustPending) {
        refreshCoverBustPending = false
        return {}
      }
      const prevById = new Map(previousEntries.map((item) => [item.id, item]))
      const nextById = new Map(library.entries.map((item) => [item.id, item]))
      const filtered: Record<string, string> = {}
      for (const [key, uri] of Object.entries(state.coverCache)) {
        const nextEntry = nextById.get(key)
        if (!nextEntry) continue
        const prevEntry = prevById.get(key)
        const metaChanged =
          !prevEntry ||
          Boolean(prevEntry.hasCover) !== Boolean(nextEntry.hasCover) ||
          (prevEntry.audioFile ?? '') !== (nextEntry.audioFile ?? '') ||
          editingKey === key
        if (!metaChanged) filtered[key] = uri
      }
      return filtered
    },
    onCoverReady: (key: string, hasCover: boolean, coverDataUri: string) => {
      if (!key) return
      coverAwaitingReady.delete(key)
      coverPending.delete(key)
      const cur = ctx.getState()
      if (!cur.library.entries.some((item) => item.id === key)) return
      const uri = hasCover && coverDataUri ? coverDataUri : ''
      ctx.set({ coverCache: { ...cur.coverCache, [key]: uri } })
    },
    invalidateCoverCacheKey: (key: string) => {
      coverGeneration += 1
      coverPending.delete(key)
      coverAwaitingReady.delete(key)
      coverQueued.delete(key)
      coverQueue = coverQueue.filter((item) => item !== key)
      resolveCoverWaiters(key)
    },
    reset: () => {
      clearCoverQueueState()
      coverWorkersRunning = 0
      coverGeneration += 1
      refreshCoverBustPending = false
    },
  }
}

export type LibraryActions = ReturnType<typeof createLibrarySlice>['actions']
