import { BridgeError, type LibraryEntry, type LibraryState } from '../../protocol'
import type { SliceBundle, StoreContext } from '../context'

export interface LibraryCoverHooks {
  onLibraryChangedCovers: (
    library: LibraryState,
    previousEntries: LibraryEntry[],
    editingKey: string | null,
  ) => Record<string, string> | null
  invalidateCoverCacheKey: (key: string) => void
}

export function createLibrarySlice(
  ctx: StoreContext,
): SliceBundle<ReturnType<typeof buildActions>> & LibraryCoverHooks {
  const coverPending = new Set<string>()
  let coverGeneration = 0
  let coverQueue: string[] = []
  let coverWorkersRunning = 0
  let refreshCoverBustPending = false

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

      ensureTrackCover: async (key: string): Promise<void> => {
        if (!key) return
        const state = ctx.getState()
        if (Object.prototype.hasOwnProperty.call(state.coverCache, key) || coverPending.has(key)) return
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
          const uri = res.hasCover && res.coverDataUri ? res.coverDataUri : ''
          ctx.set({ coverCache: { ...cur.coverCache, [key]: uri } })
        } catch {
          // Leave cache clean on transient bridge error so it can retry later.
        } finally {
          coverPending.delete(key)
        }
      },

      preloadLibraryCovers: (entries: LibraryEntry[], priorityIds: string[] = []): void => {
        if (!ctx.supports('library.getCover') || entries.length === 0) return
        const state = ctx.getState()
        const entryById = new Map(entries.map((item) => [item.id, item]))
        const orderedIds: string[] = []
        const seen = new Set<string>()
        const enqueueIfNeeded = (id: string | null | undefined) => {
          if (!id || seen.has(id)) return
          seen.add(id)
          const entry = entryById.get(id)
          if (!entry || (!entry.hasCover && !entry.hasAudio)) return
          if (
            Object.prototype.hasOwnProperty.call(state.coverCache, id) ||
            coverPending.has(id) ||
            coverQueue.includes(id)
          ) {
            return
          }
          orderedIds.push(id)
        }

        enqueueIfNeeded(state.loaded.motionKey)
        enqueueIfNeeded(state.selectedLibraryKey)
        for (const id of priorityIds) enqueueIfNeeded(id)
        for (const entry of entries) enqueueIfNeeded(entry.id)

        if (orderedIds.length === 0) return
        coverQueue.push(...orderedIds)

        const CONCURRENCY = 5
        const spawnWorker = () => {
          coverWorkersRunning += 1
          void (async () => {
            try {
              while (ctx.isStarted() && coverQueue.length > 0) {
                const nextId = coverQueue.shift()
                if (!nextId) continue
                await actions.ensureTrackCover(nextId)
              }
            } finally {
              coverWorkersRunning -= 1
            }
          })()
        }
        while (coverWorkersRunning < CONCURRENCY && coverQueue.length > 0) {
          spawnWorker()
        }
      },
    }
  }

  return {
    actions,
    onLibraryChangedCovers: (library, previousEntries, editingKey) => {
      const state = ctx.getState()
      if (library.revision === state.library.revision) return null
      coverGeneration += 1
      coverPending.clear()
      coverQueue = []
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
    invalidateCoverCacheKey: (key: string) => {
      coverGeneration += 1
      coverPending.delete(key)
    },
    reset: () => {
      coverPending.clear()
      coverQueue = []
      coverWorkersRunning = 0
      coverGeneration += 1
      refreshCoverBustPending = false
    },
  }
}

export type LibraryActions = ReturnType<typeof createLibrarySlice>['actions']
