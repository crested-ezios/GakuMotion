import type { SliceBundle, StoreContext } from '../context'
import { emptyCoverEditor } from '../types'

export function createCoverEditorSlice(
  ctx: StoreContext,
  invalidateCoverCacheKey: (key: string) => void,
): SliceBundle<ReturnType<typeof buildActions>> {
  const actions = buildActions()

  function buildActions() {
    return {
      openCoverEditorForTrack: (key: string, owner: 'library' | 'transport' = 'library'): void => {
        const state = ctx.getState()
        const entry = state.library.entries.find((item) => item.id === key)
        if (!entry) return
        const cached = state.coverCache[key] ?? ''
        ctx.set({
          coverEditor: {
            ...emptyCoverEditor(),
            open: true,
            owner,
            mode: 'track',
            trackKey: entry.id,
            trackTitle: entry.title,
            sourceImageUri: cached,
          },
        })
        ctx.store.syncDropTarget()
        if (!cached && (entry.hasCover || entry.hasAudio)) {
          void ctx.store.ensureTrackCover(entry.id).then(() => {
            const cur = ctx.getState().coverEditor
            if (cur.open && cur.mode === 'track' && cur.trackKey === entry.id && !cur.sourceImageUri) {
              const loaded = ctx.getState().coverCache[entry.id] ?? ''
              if (loaded) ctx.set({ coverEditor: { ...ctx.getState().coverEditor, sourceImageUri: loaded } })
            }
          })
        }
        void actions.refreshScreenshotCapabilities()
      },

      openCoverEditorForImport: (): void => {
        const form = ctx.getState().importForm
        if (!form.open) return
        const initial =
          form.coverMode === 'custom'
            ? form.coverDataUri
            : form.coverMode === 'auto'
              ? form.audioProbe?.hasCover && form.audioProbe.coverDataUri
                ? form.audioProbe.coverDataUri
                : form.coverDataUri
              : ''
        ctx.set({
          coverEditor: {
            ...emptyCoverEditor(),
            open: true,
            owner: 'import',
            mode: 'import',
            trackKey: form.editKey,
            trackTitle: form.title || '新导入曲目',
            sourceImageUri: initial,
          },
        })
        ctx.store.syncDropTarget()
        void actions.refreshScreenshotCapabilities()
      },

      closeCoverEditor: (): void => {
        ctx.set({ coverEditor: emptyCoverEditor() })
        ctx.store.syncDropTarget()
      },

      setCoverEditorImageUri: (uri: string): void => {
        ctx.set({
          coverEditor: {
            ...ctx.getState().coverEditor,
            sourceImageUri: uri,
            error: '',
          },
        })
      },

      setCoverEditorError: (error: string): void => {
        const editor = ctx.getState().coverEditor
        if (!editor.open) return
        ctx.set({
          coverEditor: {
            ...editor,
            error,
          },
        })
      },

      refreshScreenshotCapabilities: async (): Promise<void> => {
        if (!ctx.supports('screenshot.capabilities')) {
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              screenshotCaps: {
                available: false,
                runtimeReady: false,
                cameraAvailable: false,
                windowWidth: 0,
                windowHeight: 0,
                notice: '当前桥接不支持 ScreenshotManager 拍照联动',
              },
            },
          })
          return
        }
        try {
          const caps = await ctx.client().request('screenshot.capabilities')
          if (!ctx.getState().coverEditor.open) return
          ctx.set({ coverEditor: { ...ctx.getState().coverEditor, screenshotCaps: caps } })
        } catch {
          if (!ctx.getState().coverEditor.open) return
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              screenshotCaps: {
                available: false,
                runtimeReady: false,
                cameraAvailable: false,
                windowWidth: 0,
                windowHeight: 0,
                notice: 'ScreenshotManager 联动服务暂不可用',
              },
            },
          })
        }
      },

      captureScreenshotForCover: async (): Promise<void> => {
        if (!ctx.supports('screenshot.capture') || ctx.getState().coverEditor.capturing) return
        ctx.set({ coverEditor: { ...ctx.getState().coverEditor, capturing: true, error: '' } })
        try {
          const res = await ctx.client().request('screenshot.capture')
          if (!ctx.getState().coverEditor.open) return
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              capturing: false,
              sourceImageUri: res.dataUri,
              error: '',
            },
          })
        } catch (error) {
          if (!ctx.getState().coverEditor.open) return
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              capturing: false,
              error: error instanceof Error ? error.message : String(error),
            },
          })
        }
      },

      saveCoverEditor: async (croppedPngDataUri: string): Promise<void> => {
        const editor = ctx.getState().coverEditor
        if (!editor.open || editor.saving || !croppedPngDataUri) return
        if (editor.mode === 'import') {
          ctx.store.setImportCover('custom', croppedPngDataUri)
          actions.closeCoverEditor()
          return
        }
        if (!ctx.supports('library.setCover')) {
          ctx.set({ coverEditor: { ...editor, error: '当前原生桥接不支持保存封面（library.setCover）' } })
          return
        }
        ctx.set({ coverEditor: { ...editor, saving: true, error: '' } })
        try {
          const res = await ctx.client().request('library.setCover', {
            key: editor.trackKey,
            coverDataUri: croppedPngDataUri,
          })
          const thumb = res.coverDataUri || croppedPngDataUri
          invalidateCoverCacheKey(editor.trackKey)
          ctx.set({
            coverCache: { ...ctx.getState().coverCache, [editor.trackKey]: thumb },
            coverEditor: emptyCoverEditor(),
            actionError: null,
          })
        } catch (error) {
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              saving: false,
              error: error instanceof Error ? error.message : String(error),
            },
          })
        }
      },

      clearCoverFromEditor: async (): Promise<void> => {
        const editor = ctx.getState().coverEditor
        if (!editor.open || editor.saving) return
        if (editor.mode === 'import') {
          ctx.store.setImportCover('clear', '')
          actions.closeCoverEditor()
          return
        }
        if (!ctx.supports('library.setCover')) {
          ctx.set({ coverEditor: { ...editor, error: '当前原生桥接不支持清除封面（library.setCover）' } })
          return
        }
        ctx.set({ coverEditor: { ...editor, saving: true, error: '' } })
        try {
          await ctx.client().request('library.setCover', { key: editor.trackKey, clear: true })
          invalidateCoverCacheKey(editor.trackKey)
          ctx.set({
            coverCache: { ...ctx.getState().coverCache, [editor.trackKey]: '' },
            coverEditor: emptyCoverEditor(),
            actionError: null,
          })
        } catch (error) {
          ctx.set({
            coverEditor: {
              ...ctx.getState().coverEditor,
              saving: false,
              error: error instanceof Error ? error.message : String(error),
            },
          })
        }
      },

      loadCoverPreviewFromPath: async (path: string): Promise<void> => {
        if (!path) return
        if (!ctx.supports('library.readImagePreview')) {
          actions.setCoverEditorError('当前原生桥接不支持读取本地图片预览（library.readImagePreview）')
          return
        }
        try {
          const { dataUri } = await ctx.client().request('library.readImagePreview', { path })
          if (dataUri) actions.setCoverEditorImageUri(dataUri)
        } catch (error) {
          if (ctx.getState().coverEditor.open) {
            ctx.set({
              coverEditor: {
                ...ctx.getState().coverEditor,
                error: error instanceof Error ? error.message : String(error),
              },
            })
          } else {
            ctx.reportActionError(error)
          }
        }
      },
    }
  }

  return { actions }
}

export type CoverEditorActions = ReturnType<typeof createCoverEditorSlice>['actions']
