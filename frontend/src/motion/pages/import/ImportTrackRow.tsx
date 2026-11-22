import {
  RiCameraFill,
  RiEmotionHappyFill,
  RiMusic2Fill,
  RiSparkling2Fill,
  type RemixiconComponentType,
} from '@remixicon/react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { cn } from '@/lib/utils'
import { IMPORT_SLOT, isSlotPlaceholder, vmdBadgeText, type TrackTiming } from '../../protocol'
import { motionStore, useMotionStore, type ImportSlotKey, type ImportSlotState } from '../../store'
import { DragFloat } from '../../components/DragFloat'

const SLOT_META: Record<ImportSlotKey, { label: string; hint: string; icon: RemixiconComponentType }> = {
  motion: { label: '动作', hint: '身体骨骼动作 (.vmd)', icon: RiSparkling2Fill },
  facial: { label: '表情', hint: '口型与眼眉表情 (.vmd)', icon: RiEmotionHappyFill },
  camera: { label: '镜头', hint: 'VMD 镜头或烘焙 .gkcamera', icon: RiCameraFill },
  audio: { label: '音频', hint: '本地音频 (.mp3 / .flac / .wav / .ogg / .m4a / .aac / .opus)', icon: RiMusic2Fill },
}

function CandidateBadge({ path }: { path: string }) {
  const probe = useMotionStore((state) => (isSlotPlaceholder(path) ? null : state.importForm.probes[path] ?? null))
  if (!probe) return null
  return (
    <Badge variant="outline" className="rounded-[4px] border-border bg-muted/60 px-1.5 py-0 text-micro font-medium text-muted-foreground">
      {vmdBadgeText(probe)}
    </Badge>
  )
}

export function ImportTrackRow({
  slotKey,
  slotState,
  slotAriaLabel,
  timing,
  timingAriaPrefix,
  showTiming,
  busy,
  dialogSupported,
  onSelectChange,
  onBrowse,
  onTimingChange,
}: {
  slotKey: ImportSlotKey
  slotState: ImportSlotState
  slotAriaLabel: string
  timing?: TrackTiming
  timingAriaPrefix?: string
  showTiming: boolean
  busy: boolean
  dialogSupported: boolean
  onSelectChange: (selection: number) => void
  onBrowse: () => void
  onTimingChange?: (patch: Partial<TrackTiming>) => void
}) {
  const meta = SLOT_META[slotKey]
  const SlotIcon = meta.icon
  const pick = slotState.candidates[slotState.selection] ?? ''
  const hasDiskPath = !isSlotPlaceholder(pick)
  const timingPrefix = timingAriaPrefix ?? meta.label
  const handleTimingChange = onTimingChange ?? ((patch: Partial<TrackTiming>) => motionStore.setImportTiming(slotKey, patch))

  return (
    <div
      className={cn(
        'grid gap-2.5 p-3 sm:items-center',
        showTiming ? 'sm:grid-cols-[150px_minmax(0,1fr)_196px]' : 'sm:grid-cols-[150px_minmax(0,1fr)]',
      )}
    >
      <div className="min-w-0 space-y-0.5">
        <div className="flex flex-wrap items-center gap-1.5">
          <SlotIcon className="size-3.5 shrink-0 text-primary" />
          <span className="text-base font-medium">{meta.label}</span>
          <CandidateBadge path={pick} />
        </div>
        <p className="text-xs text-muted-foreground">{meta.hint}</p>
      </div>

      <div className="min-w-0 space-y-1">
        <div className="flex min-w-0 gap-1.5">
          <Select
            items={Object.fromEntries(
              slotState.candidates.map((candidate, candidateIndex) => [String(candidateIndex), candidate]),
            )}
            value={String(slotState.selection)}
            onValueChange={(value) => onSelectChange(Number(value))}
          >
            <SelectTrigger
              className="h-8 min-w-0 flex-1 overflow-hidden text-xs [&>span]:min-w-0 [&>span]:truncate"
              aria-label={slotAriaLabel}
            >
              <SelectValue placeholder={IMPORT_SLOT.empty} />
            </SelectTrigger>
            <SelectContent className="max-w-[min(36rem,calc(100vw-2rem))]">
              {slotState.candidates.map((candidate, candidateIndex) => (
                <SelectItem key={candidate} value={String(candidateIndex)} className="break-all [overflow-wrap:anywhere]">
                  {candidate}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Button
            variant="outline"
            size="sm"
            className="h-8 shrink-0 px-2.5 text-xs"
            disabled={busy || !dialogSupported}
            onClick={onBrowse}
          >
            浏览…
          </Button>
        </div>
        {hasDiskPath && (
          <p className="min-w-0 break-all [overflow-wrap:anywhere] text-xs leading-snug text-muted-foreground" title={pick}>
            {pick}
          </p>
        )}
      </div>

      {showTiming && timing && (
        <div className="grid grid-cols-2 gap-1.5">
          <div className="space-y-0.5 text-xs text-muted-foreground">
            <span>速率 (倍)</span>
            <DragFloat
              ariaLabel={`${timingPrefix}速率`}
              inputClassName="h-8 w-full px-2 text-xs"
              value={timing.speed}
              min={0}
              max={10}
              step={0.01}
              decimals={2}
              onChange={(next) => handleTimingChange({ speed: next })}
              onCommit={(next) => handleTimingChange({ speed: next })}
            />
          </div>
          <div className="space-y-0.5 text-xs text-muted-foreground">
            <span>偏移 (秒)</span>
            <DragFloat
              ariaLabel={`${timingPrefix}偏移`}
              inputClassName="h-8 w-full px-2 text-xs"
              value={timing.offsetSeconds}
              min={-3600}
              max={3600}
              step={0.01}
              decimals={2}
              onChange={(next) => handleTimingChange({ offsetSeconds: next })}
              onCommit={(next) => handleTimingChange({ offsetSeconds: next })}
            />
          </div>
        </div>
      )}
    </div>
  )
}
