export const CROP_VIEW_SIZE = 272
export const OUTPUT_SIZE = 512

export interface NaturalSize {
  width: number
  height: number
}

export function clampPan(
  panX: number,
  panY: number,
  natural: NaturalSize,
  zoom: number,
): { x: number; y: number } {
  if (natural.width <= 0 || natural.height <= 0) return { x: 0, y: 0 }
  const baseScale = Math.max(CROP_VIEW_SIZE / natural.width, CROP_VIEW_SIZE / natural.height)
  const scaledW = natural.width * baseScale * zoom
  const scaledH = natural.height * baseScale * zoom
  const maxX = Math.max(0, (scaledW - CROP_VIEW_SIZE) / 2)
  const maxY = Math.max(0, (scaledH - CROP_VIEW_SIZE) / 2)
  return {
    x: Math.min(Math.max(panX, -maxX), maxX),
    y: Math.min(Math.max(panY, -maxY), maxY),
  }
}

export function computeCropLayout(natural: NaturalSize, zoom: number, pan: { x: number; y: number }) {
  const baseScale =
    natural.width > 0 && natural.height > 0
      ? Math.max(CROP_VIEW_SIZE / natural.width, CROP_VIEW_SIZE / natural.height)
      : 1
  const scaledW = natural.width * baseScale * zoom
  const scaledH = natural.height * baseScale * zoom
  const clampedPan = clampPan(pan.x, pan.y, natural, zoom)
  return { baseScale, scaledW, scaledH, clampedPan }
}

export function exportCroppedCoverDataUri(
  img: HTMLImageElement | null,
  fallbackUri: string,
  natural: NaturalSize,
  zoom: number,
  pan: { x: number; y: number },
): string {
  if (!img || natural.width <= 0 || natural.height <= 0) return fallbackUri
  try {
    const canvas = document.createElement('canvas')
    canvas.width = OUTPUT_SIZE
    canvas.height = OUTPUT_SIZE
    const ctx = canvas.getContext('2d')
    if (!ctx) return fallbackUri
    const baseScale = Math.max(CROP_VIEW_SIZE / natural.width, CROP_VIEW_SIZE / natural.height)
    const totalScale = baseScale * zoom
    const scaledW = natural.width * totalScale
    const scaledH = natural.height * totalScale
    const clampedPan = clampPan(pan.x, pan.y, natural, zoom)
    const imgLeft = (CROP_VIEW_SIZE - scaledW) / 2 + clampedPan.x
    const imgTop = (CROP_VIEW_SIZE - scaledH) / 2 + clampedPan.y
    const sx = Math.max(0, -imgLeft / totalScale)
    const sy = Math.max(0, -imgTop / totalScale)
    const sw = Math.min(natural.width - sx, CROP_VIEW_SIZE / totalScale)
    const sh = Math.min(natural.height - sy, CROP_VIEW_SIZE / totalScale)
    ctx.imageSmoothingEnabled = true
    ctx.imageSmoothingQuality = 'high'
    ctx.drawImage(img, sx, sy, sw, sh, 0, 0, OUTPUT_SIZE, OUTPUT_SIZE)
    const encoded = canvas.toDataURL('image/png')
    return encoded.startsWith('data:image/png') ? encoded : fallbackUri
  } catch {
    return fallbackUri
  }
}
