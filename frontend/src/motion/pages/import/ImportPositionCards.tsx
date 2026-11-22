import { RiArrowDownSLine, RiArrowUpSLine, RiLockFill, RiMapPin2Fill } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { motionStore, type ImportFormState } from '../../store'
import { ImportTrackRow } from './ImportTrackRow'

export function ImportPositionCards({
  form,
  busy,
  dialogSupported,
  onOpenPlacementModal,
}: {
  form: ImportFormState
  busy: boolean
  dialogSupported: boolean
  onOpenPlacementModal: (posIndex: number) => void
}) {
  return (
    <div className="space-y-2.5">
      {form.positions.map((pos, posIndex) => {
        const slotNum = posIndex + 1
        const hasPlacement = pos.x !== 0 || pos.y !== 0 || pos.z !== 0 || pos.yawDegrees !== 0
        return (
          <div
            key={`pos-${posIndex}`}
            data-testid={`import-position-slot-${slotNum}`}
            className="overflow-hidden rounded-lg border bg-card"
          >
            <div className="flex items-center justify-between gap-2 border-b border-border/60 bg-muted/25 px-3 py-2">
              <div className="flex items-center gap-2">
                <span className="text-sm font-semibold text-foreground">{slotNum}号位</span>
                {posIndex === 0 ? (
                  <span
                    aria-label="1号位已锁定"
                    title="1号位为锚点基准位置"
                    className="inline-flex items-center text-muted-foreground"
                  >
                    <RiLockFill className="size-3.5" />
                  </span>
                ) : hasPlacement ? (
                  <span className="rounded-[4px] border border-border bg-background/80 px-1.5 py-0.5 text-micro tabular-nums text-muted-foreground">
                    X:{pos.x} Y:{pos.y} Z:{pos.z} · {pos.yawDegrees}°
                  </span>
                ) : null}
              </div>

              {(form.positions.length > 1 || posIndex >= 1) && (
                <div className="flex items-center gap-1.5">
                  {form.positions.length > 1 && (
                    <div className="flex items-center gap-0.5">
                      <Button
                        type="button"
                        variant="ghost"
                        size="sm"
                        className="h-7 w-7 p-0 text-muted-foreground hover:text-foreground disabled:opacity-35"
                        disabled={busy || posIndex === 0}
                        aria-label={`${slotNum}号位上移`}
                        title="与上一号位交换动作与表情"
                        onClick={() => motionStore.moveImportPosition(posIndex, -1)}
                      >
                        <RiArrowUpSLine className="size-4" />
                      </Button>
                      <Button
                        type="button"
                        variant="ghost"
                        size="sm"
                        className="h-7 w-7 p-0 text-muted-foreground hover:text-foreground disabled:opacity-35"
                        disabled={busy || posIndex === form.positions.length - 1}
                        aria-label={`${slotNum}号位下移`}
                        title="与下一号位交换动作与表情"
                        onClick={() => motionStore.moveImportPosition(posIndex, 1)}
                      >
                        <RiArrowDownSLine className="size-4" />
                      </Button>
                    </div>
                  )}
                  {posIndex >= 1 && (
                    <>
                      <Button
                        type="button"
                        variant="outline"
                        size="sm"
                        className="h-7 px-2.5 text-xs"
                        onClick={() => onOpenPlacementModal(posIndex)}
                      >
                        <RiMapPin2Fill className="size-3.5" />
                        编辑初始位置
                      </Button>
                      <Button
                        type="button"
                        variant="ghost"
                        size="sm"
                        className="h-7 w-7 p-0 text-base text-muted-foreground hover:bg-destructive/10 hover:text-destructive"
                        aria-label={`删除 ${slotNum}号位`}
                        title={`删除 ${slotNum}号位`}
                        onClick={() => motionStore.removeImportPosition(posIndex)}
                      >
                        ×
                      </Button>
                    </>
                  )}
                </div>
              )}
            </div>

            <div className="divide-y divide-border/60">
              <ImportTrackRow
                slotKey="motion"
                slotState={pos.motion}
                slotAriaLabel={posIndex === 0 ? '动作槽位' : `${slotNum}号位动作槽位`}
                timing={form.timing.motion}
                showTiming={posIndex === 0}
                busy={busy}
                dialogSupported={dialogSupported}
                onSelectChange={(sel) => motionStore.setImportPositionSelection(posIndex, 'motion', sel)}
                onBrowse={() => void motionStore.browseFiles(`position-motion:${posIndex}`)}
              />
              <ImportTrackRow
                slotKey="facial"
                slotState={pos.facial}
                slotAriaLabel={posIndex === 0 ? '表情槽位' : `${slotNum}号位表情槽位`}
                timing={form.timing.facial}
                showTiming={posIndex === 0}
                busy={busy}
                dialogSupported={dialogSupported}
                onSelectChange={(sel) => motionStore.setImportPositionSelection(posIndex, 'facial', sel)}
                onBrowse={() => void motionStore.browseFiles(`position-facial:${posIndex}`)}
              />
            </div>
          </div>
        )
      })}
    </div>
  )
}

export interface PlacementDraft {
  x: string
  y: string
  z: string
  yawDegrees: string
}

export function ImportPlacementModal({
  posIndex,
  draft,
  onChange,
  onCancel,
  onCommit,
}: {
  posIndex: number | null
  draft: PlacementDraft
  onChange: (updater: (prev: PlacementDraft) => PlacementDraft) => void
  onCancel: () => void
  onCommit: () => void
}) {
  return (
    <Dialog
      open={posIndex !== null}
      onOpenChange={(next) => {
        if (!next) onCancel()
      }}
    >
      <DialogContent className="sm:max-w-md" data-testid="placement-modal">
        <DialogHeader>
          <DialogTitle>
            编辑初始位置（{posIndex !== null ? `${posIndex + 1}号位` : ''}）
          </DialogTitle>
          <DialogDescription>
            填写相对 1 号位的世界坐标偏移（米）与朝向旋转角度（度，默认 0）。
          </DialogDescription>
        </DialogHeader>
        <div className="grid grid-cols-2 gap-3 py-2">
          <div className="space-y-1">
            <Label htmlFor="pos-modal-x" className="text-xs">X 轴位置 (米)</Label>
            <Input
              id="pos-modal-x"
              type="number"
              step="0.01"
              value={draft.x}
              onChange={(e) => onChange((prev) => ({ ...prev, x: e.target.value }))}
            />
          </div>
          <div className="space-y-1">
            <Label htmlFor="pos-modal-y" className="text-xs">Y 轴位置 (米)</Label>
            <Input
              id="pos-modal-y"
              type="number"
              step="0.01"
              value={draft.y}
              onChange={(e) => onChange((prev) => ({ ...prev, y: e.target.value }))}
            />
          </div>
          <div className="space-y-1">
            <Label htmlFor="pos-modal-z" className="text-xs">Z 轴位置 (米)</Label>
            <Input
              id="pos-modal-z"
              type="number"
              step="0.01"
              value={draft.z}
              onChange={(e) => onChange((prev) => ({ ...prev, z: e.target.value }))}
            />
          </div>
          <div className="space-y-1">
            <Label htmlFor="pos-modal-yaw" className="text-xs">旋转角度 Yaw (°)</Label>
            <Input
              id="pos-modal-yaw"
              type="number"
              step="1"
              value={draft.yawDegrees}
              onChange={(e) => onChange((prev) => ({ ...prev, yawDegrees: e.target.value }))}
            />
          </div>
        </div>
        <DialogFooter>
          <Button type="button" variant="outline" onClick={onCancel}>
            取消
          </Button>
          <Button type="button" onClick={onCommit}>
            确定
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
