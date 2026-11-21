import * as React from 'react'
import { RiRefreshLine, RiZoomInLine } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { Slider } from '@/components/ui/slider'
import { cn } from '@/lib/utils'
import { DefaultAlbumCover } from '../AlbumCover'
import { CROP_VIEW_SIZE, clampPan, computeCropLayout, type NaturalSize } from './coverCropMath'

export function CoverCropCanvas({
  sourceImageUri,
  natural,
  zoom,
  pan,
  dragOver,
  nativeDragActive,
  imgRef,
  onNaturalLoad,
  onZoomChange,
  onPanChange,
  onReset,
  onDragOver,
  onDragLeave,
  onDrop,
}: {
  sourceImageUri: string
  natural: NaturalSize
  zoom: number
  pan: { x: number; y: number }
  dragOver: boolean
  nativeDragActive: boolean
  imgRef: React.RefObject<HTMLImageElement | null>
  onNaturalLoad: (size: NaturalSize) => void
  onZoomChange: (nextZoom: number) => void
  onPanChange: (nextPan: { x: number; y: number }) => void
  onReset: () => void
  onDragOver: (event: React.DragEvent<HTMLDivElement>) => void
  onDragLeave: (event: React.DragEvent<HTMLDivElement>) => void
  onDrop: (event: React.DragEvent<HTMLDivElement>) => void
}) {
  const dragRef = React.useRef<{ pointerId: number; startX: number; startY: number; baseX: number; baseY: number } | null>(
    null,
  )

  const handlePointerDown = (event: React.PointerEvent<HTMLDivElement>) => {
    if (!sourceImageUri || natural.width <= 0) return
    event.currentTarget.setPointerCapture(event.pointerId)
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      baseX: pan.x,
      baseY: pan.y,
    }
  }

  const handlePointerMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current
    if (!drag || drag.pointerId !== event.pointerId) return
    const dx = event.clientX - drag.startX
    const dy = event.clientY - drag.startY
    onPanChange(clampPan(drag.baseX + dx, drag.baseY + dy, natural, zoom))
  }

  const handlePointerUp = (event: React.PointerEvent<HTMLDivElement>) => {
    if (dragRef.current?.pointerId === event.pointerId) {
      dragRef.current = null
    }
  }

  const { scaledW, scaledH, clampedPan } = computeCropLayout(natural, zoom, pan)

  const previewImgStyle: React.CSSProperties =
    natural.width > 0
      ? {
          width: scaledW,
          height: scaledH,
          left: (CROP_VIEW_SIZE - scaledW) / 2 + clampedPan.x,
          top: (CROP_VIEW_SIZE - scaledH) / 2 + clampedPan.y,
        }
      : { width: '100%', height: '100%', objectFit: 'cover' }

  return (
    <div className="flex flex-col items-center gap-4 sm:flex-row sm:items-start">
      {/* 1:1 Interactive Square Cropper */}
      <div className="flex flex-col items-center gap-2.5">
        <div
          data-testid="cover-crop-canvas"
          onPointerDown={handlePointerDown}
          onPointerMove={handlePointerMove}
          onPointerUp={handlePointerUp}
          onPointerCancel={handlePointerUp}
          onWheel={(event) => {
            if (!sourceImageUri) return
            event.preventDefault()
            onZoomChange(zoom + (event.deltaY < 0 ? 0.12 : -0.12))
          }}
          onDragEnter={onDragOver}
          onDragOver={onDragOver}
          onDragLeave={onDragLeave}
          onDrop={onDrop}
          style={{ width: CROP_VIEW_SIZE, height: CROP_VIEW_SIZE }}
          className={cn(
            'relative shrink-0 select-none overflow-hidden rounded-lg border-2 bg-muted/40',
            sourceImageUri ? 'cursor-grab active:cursor-grabbing' : 'cursor-default',
            dragOver || nativeDragActive ? 'border-primary ring-2 ring-primary/30' : 'border-border',
          )}
        >
          {sourceImageUri ? (
            <>
              <img
                ref={imgRef}
                src={sourceImageUri}
                alt="封面裁切预览"
                draggable={false}
                onLoad={(event) => {
                  const img = event.currentTarget
                  onNaturalLoad({ width: img.naturalWidth || 512, height: img.naturalHeight || 512 })
                  onPanChange({ x: 0, y: 0 })
                }}
                style={
                  natural.width > 0
                    ? previewImgStyle
                    : {
                        width: '100%',
                        height: '100%',
                        left: 0,
                        top: 0,
                        objectFit: 'cover',
                      }
                }
                className="pointer-events-none absolute max-w-none"
              />
              {/* 1:1 rule-of-thirds overlay */}
              <div aria-hidden="true" className="pointer-events-none absolute inset-0 border border-white/25">
                <div className="absolute inset-y-0 left-1/3 w-px bg-white/20" />
                <div className="absolute inset-y-0 left-2/3 w-px bg-white/20" />
                <div className="absolute inset-x-0 top-1/3 h-px bg-white/20" />
                <div className="absolute inset-x-0 top-2/3 h-px bg-white/20" />
              </div>
            </>
          ) : (
            <div className="flex size-full flex-col items-center justify-center gap-2 p-4 text-center text-muted-foreground">
              <div className="size-20 overflow-hidden rounded-md border border-border/60">
                <DefaultAlbumCover />
              </div>
              <p className="text-xs">拖入图片到此处，或点击上方“选择本地图片” / “拍照”</p>
            </div>
          )}
        </div>

        {sourceImageUri && (
          <div className="flex w-full max-w-[272px] items-center gap-2">
            <RiZoomInLine className="size-4 shrink-0 text-muted-foreground" />
            <Slider
              aria-label="封面缩放"
              value={[zoom]}
              min={1}
              max={4}
              step={0.02}
              onValueChange={([v]) => onZoomChange(v)}
            />
            <span className="w-11 shrink-0 text-right text-xs tabular-nums text-muted-foreground">
              {zoom.toFixed(2)}x
            </span>
            <Button
              type="button"
              variant="ghost"
              size="icon"
              className="size-7 shrink-0"
              title="重置位置与缩放"
              aria-label="重置裁切"
              onClick={onReset}
            >
              <RiRefreshLine className="size-3.5" />
            </Button>
          </div>
        )}
      </div>

      {/* Live square previews */}
      <div className="flex flex-1 flex-col gap-3 rounded-lg border border-border/70 bg-muted/25 p-3">
        <div className="text-sm font-medium text-foreground">实时 1:1 预览</div>
        <div className="flex items-end gap-3">
          <div className="space-y-1">
            <div className="relative size-20 overflow-hidden rounded-md border border-border/70 bg-muted">
              {sourceImageUri ? (
                <div
                  style={{
                    width: CROP_VIEW_SIZE,
                    height: CROP_VIEW_SIZE,
                    transform: `scale(${80 / CROP_VIEW_SIZE})`,
                    transformOrigin: 'top left',
                  }}
                  className="relative"
                >
                  <img src={sourceImageUri} alt="" style={previewImgStyle} className="pointer-events-none absolute max-w-none" />
                </div>
              ) : (
                <DefaultAlbumCover />
              )}
            </div>
            <div className="text-micro text-muted-foreground">曲目封面 (1:1)</div>
          </div>

          <div className="space-y-1">
            <div className="relative size-8 overflow-hidden rounded border border-border/70 bg-muted">
              {sourceImageUri ? (
                <div
                  style={{
                    width: CROP_VIEW_SIZE,
                    height: CROP_VIEW_SIZE,
                    transform: `scale(${32 / CROP_VIEW_SIZE})`,
                    transformOrigin: 'top left',
                  }}
                  className="relative"
                >
                  <img src={sourceImageUri} alt="" style={previewImgStyle} className="pointer-events-none absolute max-w-none" />
                </div>
              ) : (
                <DefaultAlbumCover />
              )}
            </div>
            <div className="text-micro text-muted-foreground">列表图标</div>
          </div>
        </div>
        <p className="text-xs leading-relaxed text-muted-foreground">
          固定 1:1 正方形裁切，保存后将输出 512×512 PNG（<code className="text-micro">cover.png</code>）到曲目目录。
        </p>
      </div>
    </div>
  )
}
