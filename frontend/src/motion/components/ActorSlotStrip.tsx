import * as React from 'react'
import { RiAddLine, RiArrowLeftSLine, RiArrowRightSLine, RiUser3Line } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'
import { motionStore, useMotionStore } from '../store'

const CARD_WIDTH_PX = 136
const CARD_GAP_PX = 8
const CARD_STEP_PX = CARD_WIDTH_PX + CARD_GAP_PX

export function ActorSlotStrip() {
  const formation = useMotionStore((state) => state.formation)
  const portraitCache = useMotionStore((state) => state.portraitCache)
  const capabilities = useMotionStore((state) => state.capabilities)
  const slots = formation.slots

  const viewportRef = React.useRef<HTMLDivElement>(null)
  const [viewportWidth, setViewportWidth] = React.useState(0)
  const [windowStart, setWindowStart] = React.useState(0)
  const dragSourceRef = React.useRef<number | null>(null)
  const [dragOverSlot, setDragOverSlot] = React.useState<number | null>(null)
  const [draggingSlot, setDraggingSlot] = React.useState<number | null>(null)

  const measureViewport = React.useCallback((overrideWidth?: number) => {
    const el = viewportRef.current
    if (!el) return
    const measured =
      typeof overrideWidth === 'number' && overrideWidth > 0
        ? overrideWidth
        : el.clientWidth || el.getBoundingClientRect().width
    if (measured > 0) {
      setViewportWidth(measured)
    }
  }, [])

  React.useLayoutEffect(() => {
    measureViewport()
    const el = viewportRef.current
    const onResize = () => measureViewport()
    window.addEventListener('resize', onResize)
    let observer: ResizeObserver | null = null
    if (el && typeof ResizeObserver !== 'undefined') {
      observer = new ResizeObserver((entries) => {
        const entryWidth = entries[0]?.contentRect?.width
        measureViewport(entryWidth)
      })
      observer.observe(el)
    }
    return () => {
      window.removeEventListener('resize', onResize)
      observer?.disconnect()
    }
  }, [measureViewport, slots.length])

  const visibleCount =
    viewportWidth > 0
      ? Math.max(1, Math.floor((viewportWidth + CARD_GAP_PX) / CARD_STEP_PX))
      : Math.max(1, slots.length)
  const maxStart = Math.max(0, slots.length - visibleCount)
  const clampedStart = Math.min(Math.max(0, windowStart), maxStart)

  React.useEffect(() => {
    if (windowStart > maxStart) {
      setWindowStart(maxStart)
    }
  }, [maxStart, windowStart])

  React.useEffect(() => {
    const sel = formation.selectedSlotIndex
    setWindowStart((prev) => {
      const safePrev = Math.min(Math.max(0, prev), maxStart)
      if (sel < safePrev) return sel
      if (sel >= safePrev + visibleCount) return Math.min(maxStart, sel - visibleCount + 1)
      return safePrev
    })
  }, [formation.selectedSlotIndex, maxStart, visibleCount])

  React.useEffect(() => {
    for (const slot of slots) {
      if (slot.occupied && slot.character) {
        void motionStore.ensureActorPortrait(slot.character)
      }
    }
  }, [slots, capabilities])

  const canSelect = capabilities.includes('formation.selectSlot')
  const prevDisabled = clampedStart <= 0
  const nextDisabled = clampedStart >= maxStart

  return (
    <div className="flex items-center gap-2" data-testid="actor-slot-strip">
      <Button
        type="button"
        variant="outline"
        size="icon"
        className="size-9 shrink-0 rounded-full"
        aria-label="上一槽位"
        title="向左查看槽位"
        disabled={prevDisabled}
        onClick={() => setWindowStart((prev) => Math.max(0, prev - 1))}
      >
        <RiArrowLeftSLine className="size-5" />
      </Button>

      <div
        ref={viewportRef}
        data-testid="actor-slot-strip-viewport"
        className="min-w-0 flex-1 overflow-hidden py-1"
      >
        <div
          className="flex items-stretch gap-2 transition-transform duration-200 ease-out"
          style={{ transform: `translateX(-${clampedStart * CARD_STEP_PX}px)` }}
        >
          {slots.map((slot) => {
            const selected = slot.slotIndex === formation.selectedSlotIndex
            const charKey = slot.character.trim().toLowerCase()
            const portraitUri = charKey ? portraitCache[charKey] ?? null : null
            const actorName = slot.shortName || slot.displayName || (slot.character ? slot.character : '未命名角色')
            const slotTitle = `${slot.slotIndex + 1}号位`
            const isDragging = draggingSlot === slot.slotIndex
            const isDragTarget = dragOverSlot === slot.slotIndex && draggingSlot !== slot.slotIndex

            return (
              <div
                key={slot.slotIndex}
                role="button"
                tabIndex={0}
                aria-pressed={selected}
                aria-label={`${slotTitle} ${slot.occupied ? actorName : '空槽位'}`}
                data-testid={`actor-slot-card-${slot.slotIndex}`}
                data-slot-index={slot.slotIndex}
                draggable={slot.occupied}
                onClick={() => {
                  if (canSelect) void motionStore.selectFormationSlot(slot.slotIndex)
                }}
                onKeyDown={(event) => {
                  if ((event.key === 'Enter' || event.key === ' ') && canSelect) {
                    event.preventDefault()
                    void motionStore.selectFormationSlot(slot.slotIndex)
                  }
                }}
                onDragStart={(event) => {
                  if (!slot.occupied) {
                    event.preventDefault()
                    return
                  }
                  dragSourceRef.current = slot.slotIndex
                  setDraggingSlot(slot.slotIndex)
                  try {
                    event.dataTransfer.setData('text/plain', String(slot.slotIndex))
                    event.dataTransfer.effectAllowed = 'move'
                  } catch {
                    // Synthetic JSDOM drag events may not supply a DataTransfer object.
                  }
                }}
                onDragOver={(event) => {
                  event.preventDefault()
                  if (dragSourceRef.current !== null && dragOverSlot !== slot.slotIndex) {
                    setDragOverSlot(slot.slotIndex)
                  }
                }}
                onDragLeave={() => {
                  if (dragOverSlot === slot.slotIndex) setDragOverSlot(null)
                }}
                onDrop={(event) => {
                  event.preventDefault()
                  let from = dragSourceRef.current
                  if (from === null) {
                    try {
                      const raw = event.dataTransfer?.getData('text/plain')
                      if (raw !== '') {
                        const parsed = Number(raw)
                        if (Number.isInteger(parsed)) from = parsed
                      }
                    } catch {
                      // Ignore invalid DataTransfer access.
                    }
                  }
                  dragSourceRef.current = null
                  setDraggingSlot(null)
                  setDragOverSlot(null)
                  if (from !== null && from !== slot.slotIndex) {
                    void motionStore.swapFormationSlots(from, slot.slotIndex)
                  }
                }}
                onDragEnd={() => {
                  dragSourceRef.current = null
                  setDraggingSlot(null)
                  setDragOverSlot(null)
                }}
                style={{ width: `${CARD_WIDTH_PX}px` }}
                className={cn(
                  'group relative flex shrink-0 cursor-pointer select-none flex-col overflow-hidden rounded-lg border transition-all focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
                  slot.isOverflow ? 'border-dashed' : 'border-solid',
                  slot.occupied
                    ? 'bg-card'
                    : 'bg-muted/25 hover:bg-muted/40',
                  selected
                    ? 'border-primary ring-2 ring-primary/30 shadow-sm'
                    : slot.isOverflow
                      ? 'border-amber-500/60 hover:border-amber-500'
                      : 'border-border hover:border-primary/50',
                  isDragging && 'opacity-50 scale-[0.98]',
                  isDragTarget && 'ring-2 ring-primary/60 bg-primary/5',
                )}
              >
                {/* Header bar with slot label & overflow badge */}
                <div className="flex items-center justify-between gap-1 border-b border-border/50 bg-muted/35 px-2 py-1">
                  <span
                    className={cn(
                      'text-xs font-semibold tabular-nums',
                      selected ? 'text-primary' : 'text-foreground',
                    )}
                  >
                    {slotTitle}
                  </span>
                  {slot.isOverflow && (
                    <span className="rounded bg-amber-500/15 px-1 py-0.5 text-micro font-medium leading-none text-amber-600 dark:text-amber-300">
                      溢出
                    </span>
                  )}
                </div>

                {/* Portrait / translucent '+' empty slot */}
                <div className="relative flex h-24 w-full items-center justify-center overflow-hidden bg-muted/30">
                  {slot.occupied ? (
                    portraitUri ? (
                      <img
                        src={portraitUri}
                        alt={actorName}
                        className="h-full w-full object-cover object-top"
                        draggable={false}
                      />
                    ) : (
                      <div className="flex flex-col items-center justify-center gap-1 text-muted-foreground">
                        <div className="flex size-10 items-center justify-center rounded-full border border-border/70 bg-background/80">
                          <RiUser3Line className="size-5 text-primary/70" />
                        </div>
                      </div>
                    )
                  ) : (
                    <div className="flex flex-col items-center justify-center gap-1 text-muted-foreground/75">
                      <RiAddLine className="size-7" />
                      <span className="text-micro">空槽位</span>
                    </div>
                  )}
                </div>

                {/* Character name footer */}
                <div className="border-t border-border/40 bg-card/90 px-2 py-1.5 text-center">
                  <span className="block truncate text-xs font-medium text-foreground">
                    {slot.occupied ? actorName : '等待角色'}
                  </span>
                </div>
              </div>
            )
          })}
        </div>
      </div>

      <Button
        type="button"
        variant="outline"
        size="icon"
        className="size-9 shrink-0 rounded-full"
        aria-label="下一槽位"
        title="向右查看槽位"
        disabled={nextDisabled}
        onClick={() => setWindowStart((prev) => Math.min(maxStart, prev + 1))}
      >
        <RiArrowRightSLine className="size-5" />
      </Button>
    </div>
  )
}
