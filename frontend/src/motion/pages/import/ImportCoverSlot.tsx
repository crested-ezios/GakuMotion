import * as React from 'react'
import { RiDeleteBinLine, RiImageEditFill } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'
import { motionStore, type ImportFormState } from '../../store'
import { isSupportedImageExtension } from '../../validation'
import { AlbumCover } from '../../components/AlbumCover'

export function ImportCoverSlot({ form }: { form: ImportFormState }) {
  const [coverDragOver, setCoverDragOver] = React.useState(false)

  const resolvedCoverPreview =
    form.coverMode === 'clear'
      ? ''
      : form.coverMode === 'custom'
        ? form.coverDataUri
        : form.audioProbe?.hasCover && form.audioProbe.coverDataUri
          ? form.audioProbe.coverDataUri
          : form.coverDataUri

  const handleCoverDrop = (event: React.DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    setCoverDragOver(false)
    const file = event.dataTransfer.files?.[0]
    if (!file) return
    if (!file.type.startsWith('image/') && !isSupportedImageExtension(file.name)) return
    const reader = new FileReader()
    reader.onload = () => {
      if (typeof reader.result === 'string' && reader.result.startsWith('data:image/')) {
        motionStore.openCoverEditorForImport()
        motionStore.setCoverEditorImageUri(reader.result)
      }
    }
    reader.readAsDataURL(file)
  }

  return (
    <div
      onDragOver={(event) => {
        event.preventDefault()
        setCoverDragOver(true)
      }}
      onDragLeave={() => setCoverDragOver(false)}
      onDrop={handleCoverDrop}
      data-testid="import-cover-slot"
      className={cn(
        'flex flex-wrap items-center gap-3 rounded-lg border bg-card p-3 transition-colors',
        coverDragOver ? 'border-primary bg-primary/5' : 'border-border',
      )}
    >
      <button
        type="button"
        onClick={() => motionStore.openCoverEditorForImport()}
        className="relative size-14 shrink-0 overflow-hidden rounded-md border border-border/70 bg-muted/40 transition-opacity hover:opacity-90"
        title="点击裁剪或更换 1:1 封面"
      >
        <AlbumCover
          src={resolvedCoverPreview}
          imgTestId="import-slot-cover-img"
          defaultTestId="import-slot-default-cover"
        />
      </button>
      <div className="min-w-0 flex-1 space-y-0.5">
        <div className="text-base font-medium text-foreground">曲目封面 (1:1)</div>
        <p className="text-xs text-muted-foreground">
          {form.coverMode === 'custom'
            ? '已配置自定义裁切封面；导入时将写入 cover.png。'
            : form.coverMode === 'clear'
              ? '已选择清除封面（将使用默认封面）。'
              : resolvedCoverPreview
                ? '已自动读取曲目/音频内嵌封面，可点击右侧按钮进入 1:1 裁切或重拍。'
                : '可选：拖入图片（.png/.jpg/.webp/.bmp）、选择文件，或从 ScreenshotManager 拍照。'}
        </p>
      </div>
      <div className="flex shrink-0 items-center gap-1.5">
        <Button
          type="button"
          variant="outline"
          size="sm"
          className="h-8 text-xs"
          onClick={() => motionStore.openCoverEditorForImport()}
          data-testid="import-edit-cover-btn"
        >
          <RiImageEditFill className="size-3.5" />
          编辑封面 / 拍照…
        </Button>
        {(resolvedCoverPreview || form.coverMode === 'custom') && (
          <Button
            type="button"
            variant="ghost"
            size="sm"
            className="h-8 px-2 text-xs text-muted-foreground hover:text-destructive"
            onClick={() => motionStore.setImportCover('clear', '')}
            title="移除封面"
          >
            <RiDeleteBinLine className="size-3.5" />
            清除
          </Button>
        )}
      </div>
    </div>
  )
}
