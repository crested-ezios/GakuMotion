import * as React from 'react'
import { cn } from '@/lib/utils'
import { clamp } from '../utils'

const KNOB_MIN_DEG = -135
const KNOB_MAX_DEG = 135
const KNOB_SWEEP_DEG = KNOB_MAX_DEG - KNOB_MIN_DEG

function polarToCartesian(cx: number, cy: number, r: number, angleDeg: number) {
  const rad = ((angleDeg - 90) * Math.PI) / 180
  return {
    x: cx + r * Math.cos(rad),
    y: cy + r * Math.sin(rad),
  }
}

function describeArc(cx: number, cy: number, r: number, startAngle: number, endAngle: number): string {
  const start = polarToCartesian(cx, cy, r, endAngle)
  const end = polarToCartesian(cx, cy, r, startAngle)
  const largeArcFlag = endAngle - startAngle <= 180 ? '0' : '1'
  return ['M', start.x.toFixed(2), start.y.toFixed(2), 'A', r, r, 0, largeArcFlag, 0, end.x.toFixed(2), end.y.toFixed(2)].join(' ')
}

export interface RotaryKnobCardProps {
  label: string
  hint?: string
  value: number
  disabled?: boolean
  onChange: (next: number) => void
  onCommit: (next: number) => void
}

export function RotaryKnobCard({
  label,
  hint,
  value,
  disabled = false,
  onChange,
  onCommit,
}: RotaryKnobCardProps) {
  const clampedWeight = clamp(Number.isFinite(value) ? value : 1, 0, 2)
  const percent = Math.round(clampedWeight * 100)
  const [editing, setEditing] = React.useState(false)
  const [text, setText] = React.useState(String(percent))
  const editingRef = React.useRef(false)
  const dragRef = React.useRef<{ startY: number; startPercent: number } | null>(null)

  React.useEffect(() => {
    if (!editingRef.current) {
      setText(String(percent))
    }
  }, [percent])

  const ratio = clamp(percent / 200, 0, 1)
  const angleDeg = KNOB_MIN_DEG + ratio * KNOB_SWEEP_DEG
  const trackPath = React.useMemo(() => describeArc(28, 28, 22, KNOB_MIN_DEG, KNOB_MAX_DEG), [])
  const activePath = React.useMemo(
    () => (angleDeg > KNOB_MIN_DEG + 0.5 ? describeArc(28, 28, 22, KNOB_MIN_DEG, angleDeg) : ''),
    [angleDeg],
  )
  const indicatorTip = React.useMemo(() => polarToCartesian(28, 28, 12, angleDeg), [angleDeg])
  const indicatorBase = React.useMemo(() => polarToCartesian(28, 28, 5, angleDeg), [angleDeg])

  const emitPercent = React.useCallback(
    (nextPct: number, commit: boolean) => {
      const clampedPct = Math.round(clamp(nextPct, 0, 200))
      const weight = clampedPct / 100
      setText(String(clampedPct))
      onChange(weight)
      if (commit) onCommit(weight)
    },
    [onChange, onCommit],
  )

  const commitText = React.useCallback(() => {
    if (!editingRef.current) return
    editingRef.current = false
    setEditing(false)
    const stripped = text.trim().replace(/%$/, '').trim()
    if (stripped === '') {
      setText(String(percent))
      return
    }
    const parsed = Number(stripped)
    if (!Number.isFinite(parsed)) {
      setText(String(percent))
      return
    }
    emitPercent(parsed, true)
  }, [emitPercent, percent, text])

  const startDrag = (event: React.PointerEvent<HTMLDivElement>) => {
    if (disabled) return
    event.preventDefault()
    event.currentTarget.focus()
    dragRef.current = { startY: event.clientY, startPercent: percent }
    let latestPct = percent

    const onMove = (moveEvt: PointerEvent) => {
      if (!dragRef.current) return
      const deltaY = dragRef.current.startY - moveEvt.clientY
      const deltaPct = Math.round(deltaY * (moveEvt.shiftKey ? 0.5 : 1.5))
      latestPct = Math.round(clamp(dragRef.current.startPercent + deltaPct, 0, 200))
      emitPercent(latestPct, false)
    }

    const onUp = () => {
      window.removeEventListener('pointermove', onMove)
      window.removeEventListener('pointerup', onUp)
      if (!dragRef.current) return
      dragRef.current = null
      emitPercent(latestPct, true)
    }

    window.addEventListener('pointermove', onMove)
    window.addEventListener('pointerup', onUp)
  }

  return (
    <div
      data-testid={`rotary-knob-card-${label}`}
      className={cn(
        'flex items-center gap-3 rounded-lg border border-border/70 bg-muted/25 px-3 py-2.5 transition-colors',
        disabled && 'opacity-55',
      )}
    >
      {/* Circular rotary dial */}
      <div
        role="slider"
        tabIndex={disabled ? -1 : 0}
        aria-label={`表情幅度 ${label}`}
        aria-valuemin={0}
        aria-valuemax={200}
        aria-valuenow={percent}
        aria-disabled={disabled}
        onPointerDown={startDrag}
        onDoubleClick={() => {
          if (!disabled) emitPercent(100, true)
        }}
        onWheel={(event) => {
          if (disabled) return
          event.preventDefault()
          const step = event.deltaY < 0 ? 5 : -5
          emitPercent(percent + step, true)
        }}
        onKeyDown={(event) => {
          if (disabled) return
          if (event.key === 'ArrowRight' || event.key === 'ArrowUp') {
            event.preventDefault()
            emitPercent(percent + 5, true)
          } else if (event.key === 'ArrowLeft' || event.key === 'ArrowDown') {
            event.preventDefault()
            emitPercent(percent - 5, true)
          } else if (event.key === 'Home') {
            event.preventDefault()
            emitPercent(0, true)
          } else if (event.key === 'End') {
            event.preventDefault()
            emitPercent(200, true)
          }
        }}
        title="上下拖拽调节 (0%–200%)，双击恢复 100%"
        className={cn(
          'relative size-14 shrink-0 select-none rounded-full focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring',
          disabled ? 'cursor-not-allowed' : 'cursor-ns-resize',
        )}
      >
        <svg viewBox="0 0 56 56" className="size-full overflow-visible" aria-hidden="true">
          <path
            d={trackPath}
            fill="none"
            stroke="currentColor"
            strokeWidth={3.5}
            strokeLinecap="round"
            className="text-border"
          />
          {activePath && (
            <path
              d={activePath}
              fill="none"
              stroke="currentColor"
              strokeWidth={3.5}
              strokeLinecap="round"
              className="text-primary"
            />
          )}
          <circle
            cx="28"
            cy="28"
            r="15.5"
            className="fill-card stroke-border/80"
            strokeWidth={1.5}
          />
          <line
            x1={indicatorBase.x}
            y1={indicatorBase.y}
            x2={indicatorTip.x}
            y2={indicatorTip.y}
            stroke="currentColor"
            strokeWidth={2.2}
            strokeLinecap="round"
            className="text-primary"
          />
        </svg>
      </div>

      {/* Right column: label/hint + bottom-right editable percentage */}
      <div className="flex min-w-0 flex-1 flex-col justify-between gap-2">
        <div className="flex items-center justify-between gap-1.5 rounded bg-muted/50 px-2 py-1">
          <div className="min-w-0">
            <div className="truncate text-xs font-semibold text-foreground">{label}</div>
            {hint && <div className="truncate text-micro text-muted-foreground">{hint}</div>}
          </div>
          <span className="shrink-0 text-micro font-medium tabular-nums text-muted-foreground">
            {percent}%
          </span>
        </div>

        <div className="flex items-center justify-end gap-1">
          <input
            type="text"
            inputMode="numeric"
            aria-label={`${label}表情幅度(%)`}
            disabled={disabled}
            value={editing ? text : String(percent)}
            onFocus={(event) => {
              editingRef.current = true
              setEditing(true)
              setText(String(percent))
              event.currentTarget.select()
            }}
            onChange={(event) => {
              if (!editingRef.current) {
                editingRef.current = true
                setEditing(true)
              }
              setText(event.target.value)
            }}
            onKeyDown={(event) => {
              if (event.key === 'Enter') {
                event.preventDefault()
                commitText()
                event.currentTarget.blur()
              } else if (event.key === 'Escape') {
                event.preventDefault()
                editingRef.current = false
                setEditing(false)
                setText(String(percent))
                event.currentTarget.blur()
              }
            }}
            onBlur={commitText}
            className={cn(
              'h-7 w-16 rounded border border-input bg-background px-1.5 text-right text-xs font-semibold tabular-nums text-primary transition-colors',
              'focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring disabled:cursor-not-allowed disabled:opacity-50',
            )}
          />
          <span className="text-xs font-medium text-muted-foreground">%</span>
        </div>
      </div>
    </div>
  )
}
