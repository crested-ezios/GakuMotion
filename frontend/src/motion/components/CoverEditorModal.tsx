import * as React from 'react'
import { RiCameraFill, RiDeleteBinLine, RiImageAddFill, RiLoader4Line } from '@remixicon/react'
import { Alert, AlertDescription } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { motionStore, useMotionStore } from '../store'
import { isSupportedImageExtension } from '../validation'
import { CoverCropCanvas } from './cover/CoverCropCanvas'
import { clampPan, exportCroppedCoverDataUri, type NaturalSize } from './cover/coverCropMath'

export function CoverEditorModal({ owner }: { owner: 'library' | 'transport' | 'import' }) {
  const editor = useMotionStore((state) => state.coverEditor)
  const capabilities = useMotionStore((state) => state.capabilities)
  const dialogBusy = useMotionStore((state) => state.dialogBusy)
  const dropVisual = useMotionStore((state) => state.drop.visual)
  const isOwnerOpen = editor.open && editor.owner === owner

  const supports = (method: string) => capabilities.includes(method)
  const canUseHostFileDialog = supports('dialog.pickFile') && supports('library.readImagePreview')
  const screenshotSupported = supports('screenshot.capture')
  const caps = editor.screenshotCaps
  const canCaptureScreenshot =
    screenshotSupported &&
    !editor.capturing &&
    !editor.saving &&
    (caps === null ? false : caps.available && caps.runtimeReady && caps.cameraAvailable)

  const screenshotTooltip = !screenshotSupported
    ? '当前原生桥接不支持 ScreenshotManager 拍照联动'
    : caps === null
      ? '正在检测 ScreenshotManager 服务状态...'
      : !caps.available
        ? caps.notice || '未检测到 ScreenshotManager 服务'
        : !caps.runtimeReady
          ? caps.notice || 'ScreenshotManager 运行时尚未就绪'
          : !caps.cameraAvailable
            ? caps.notice || '当前场景暂无可用摄影机'
            : '截取当前游戏画面并载入 1:1 裁切器'

  const fileInputRef = React.useRef<HTMLInputElement>(null)
  const imgRef = React.useRef<HTMLImageElement>(null)

  const [natural, setNatural] = React.useState<NaturalSize>({ width: 0, height: 0 })
  const [zoom, setZoom] = React.useState(1)
  const [pan, setPan] = React.useState({ x: 0, y: 0 })
  const [dragOver, setDragOver] = React.useState(false)

  React.useEffect(() => {
    setZoom(1)
    setPan({ x: 0, y: 0 })
    setNatural({ width: 0, height: 0 })
    setDragOver(false)
  }, [editor.sourceImageUri, editor.open])

  React.useEffect(() => {
    if (!isOwnerOpen) return
    motionStore.retainDropWatch()
    return () => motionStore.releaseDropWatch()
  }, [isOwnerOpen, capabilities])

  React.useEffect(() => {
    if (isOwnerOpen) motionStore.syncDropTarget()
  }, [isOwnerOpen, editor.saving, editor.capturing, dialogBusy])

  const readLocalFile = (file: File) => {
    if (!file.type.startsWith('image/') && !isSupportedImageExtension(file.name)) {
      motionStore.setCoverEditorError('仅支持拖入图片文件（.png / .jpg / .jpeg / .webp / .bmp）。')
      return
    }
    const reader = new FileReader()
    reader.onload = () => {
      if (typeof reader.result === 'string' && reader.result.startsWith('data:image/')) {
        motionStore.setCoverEditorImageUri(reader.result)
      } else {
        motionStore.setCoverEditorError('无法解析所选图片文件。')
      }
    }
    reader.onerror = () => {
      motionStore.setCoverEditorError('读取所选图片文件失败。')
    }
    reader.readAsDataURL(file)
  }

  const handleDragOver = (event: React.DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    event.stopPropagation()
    if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy'
    if (!dragOver) setDragOver(true)
  }

  const handleDragLeave = (event: React.DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    event.stopPropagation()
    const nextTarget = event.relatedTarget as Node | null
    if (!nextTarget || !event.currentTarget.contains(nextTarget)) {
      setDragOver(false)
    }
  }

  const handleDrop = (event: React.DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    event.stopPropagation()
    setDragOver(false)
    if (editor.saving || editor.capturing) return
    const file = event.dataTransfer?.files?.[0]
    if (file) readLocalFile(file)
  }

  const handlePickImage = () => {
    if (canUseHostFileDialog) {
      void motionStore.browseFiles('cover-image')
    } else {
      fileInputRef.current?.click()
    }
  }

  const handleZoomChange = (nextZoom: number) => {
    const clampedZoom = Math.min(Math.max(nextZoom, 1), 4)
    setZoom(clampedZoom)
    setPan((prev) => clampPan(prev.x, prev.y, natural, clampedZoom))
  }

  const handleSave = async () => {
    if (!editor.sourceImageUri) return
    const outputDataUri = exportCroppedCoverDataUri(imgRef.current, editor.sourceImageUri, natural, zoom, pan)
    await motionStore.saveCoverEditor(outputDataUri)
  }

  const nativeDragActive = isOwnerOpen && dropVisual !== 'none'

  return (
    <Dialog open={isOwnerOpen} onOpenChange={(next) => { if (!next && !editor.saving) motionStore.closeCoverEditor() }}>
      <DialogContent
        className="flex max-h-[85vh] w-[calc(100vw-2rem)] max-w-xl flex-col gap-0 overflow-hidden p-0"
        data-testid="cover-editor-modal"
        onDragEnter={handleDragOver}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
      >
        <DialogHeader className="shrink-0 border-b border-border/60 px-5 pt-5 pb-3 pr-10">
          <DialogTitle>编辑曲目封面（1:1 正方形）</DialogTitle>
          <DialogDescription className="break-all [overflow-wrap:anywhere]">
            {editor.trackTitle ? `目标曲目：${editor.trackTitle}` : '选择本地图片或从 ScreenshotManager 实时拍照，拖动并缩放裁切 1:1 封面。'}
          </DialogDescription>
        </DialogHeader>

        <div className="min-h-0 flex-1 space-y-4 overflow-y-auto px-5 py-4">
          <input
            ref={fileInputRef}
            type="file"
            accept=".png,.jpg,.jpeg,.webp,.bmp,image/png,image/jpeg,image/webp,image/bmp"
            className="hidden"
            data-testid="cover-file-input"
            onChange={(event) => {
              const file = event.target.files?.[0]
              if (file) readLocalFile(file)
              event.target.value = ''
            }}
          />

          <div className="flex flex-wrap items-center gap-2">
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={editor.saving || editor.capturing || dialogBusy}
              onClick={handlePickImage}
              data-testid="cover-pick-file-btn"
            >
              <RiImageAddFill className="size-4" />
              选择本地图片…
            </Button>

            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={!canCaptureScreenshot}
              title={screenshotTooltip}
              onClick={() => void motionStore.captureScreenshotForCover()}
              data-testid="cover-capture-screenshot-btn"
            >
              {editor.capturing ? (
                <RiLoader4Line className="size-4 animate-spin" />
              ) : (
                <RiCameraFill className="size-4" />
              )}
              {editor.capturing ? '拍照中…' : '从 ScreenshotManager 拍照'}
            </Button>

            {editor.sourceImageUri && (
              <Button
                type="button"
                variant="ghost"
                size="sm"
                className="ml-auto text-destructive hover:bg-destructive/10 hover:text-destructive"
                disabled={editor.saving || editor.capturing}
                onClick={() => void motionStore.clearCoverFromEditor()}
                data-testid="cover-clear-btn"
              >
                <RiDeleteBinLine className="size-4" />
                恢复默认封面
              </Button>
            )}
          </div>

          {!canCaptureScreenshot && screenshotTooltip && (
            <p className="text-xs text-muted-foreground" data-testid="cover-screenshot-notice">
              拍照联动：{screenshotTooltip}
            </p>
          )}

          {editor.error && (
            <Alert variant="destructive">
              <AlertDescription className="break-all [overflow-wrap:anywhere]">{editor.error}</AlertDescription>
            </Alert>
          )}

          <CoverCropCanvas
            sourceImageUri={editor.sourceImageUri}
            natural={natural}
            zoom={zoom}
            pan={pan}
            dragOver={dragOver}
            nativeDragActive={nativeDragActive}
            imgRef={imgRef}
            onNaturalLoad={setNatural}
            onZoomChange={handleZoomChange}
            onPanChange={setPan}
            onReset={() => {
              setZoom(1)
              setPan({ x: 0, y: 0 })
            }}
            onDragOver={handleDragOver}
            onDragLeave={handleDragLeave}
            onDrop={handleDrop}
          />
        </div>

        <DialogFooter className="shrink-0 border-t border-border/60 bg-card px-5 py-3.5">
          <Button
            type="button"
            variant="outline"
            disabled={editor.saving}
            onClick={() => motionStore.closeCoverEditor()}
          >
            取消
          </Button>
          <Button
            type="button"
            disabled={!editor.sourceImageUri || editor.saving || editor.capturing}
            onClick={() => void handleSave()}
            data-testid="cover-save-btn"
          >
            {editor.saving ? '正在保存…' : '保存封面'}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
