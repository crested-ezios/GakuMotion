import * as React from 'react'
import { RiAddFill, RiAlertFill, RiFolderAddFill, RiInformationFill } from '@remixicon/react'
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
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { motionStore, useMotionStore } from '../store'
import { CoverEditorModal } from '../components/CoverEditorModal'
import { PendingBadge } from '../components/Unsupported'
import { ImportCoverSlot } from './import/ImportCoverSlot'
import { ImportPlacementModal, ImportPositionCards, type PlacementDraft } from './import/ImportPositionCards'
import { ImportTrackRow } from './import/ImportTrackRow'

export function ImportDialog() {
  const open = useMotionStore((state) => state.importForm.open)
  const form = useMotionStore((state) => state.importForm)
  const busy = useMotionStore((state) => state.dialogBusy)
  const unsupportedAudio = useMotionStore((state) => state.importForm.unsupportedAudio)
  const capabilities = useMotionStore((state) => state.capabilities)
  const supports = (method: string) => capabilities.includes(method)
  const importSupported = supports('library.import')
  const dialogSupported = supports('dialog.pickFile')
  const formError = motionStore.importFormError()
  const editing = form.mode === 'edit'

  const [placementModalIndex, setPlacementModalIndex] = React.useState<number | null>(null)
  const [placementDraft, setPlacementDraft] = React.useState<PlacementDraft>({ x: '0', y: '0', z: '0', yawDegrees: '0' })

  React.useEffect(() => {
    if (!open) setPlacementModalIndex(null)
  }, [open])

  const openPlacementModal = (posIndex: number) => {
    const pos = form.positions[posIndex]
    if (!pos) return
    setPlacementDraft({
      x: String(pos.x),
      y: String(pos.y),
      z: String(pos.z),
      yawDegrees: String(pos.yawDegrees),
    })
    setPlacementModalIndex(posIndex)
  }

  const commitPlacementModal = () => {
    if (placementModalIndex === null) return
    const parseNum = (raw: string) => {
      const val = Number.parseFloat(raw)
      return Number.isFinite(val) ? val : 0
    }
    motionStore.setImportPositionPlacement(placementModalIndex, {
      x: parseNum(placementDraft.x),
      y: parseNum(placementDraft.y),
      z: parseNum(placementDraft.z),
      yawDegrees: parseNum(placementDraft.yawDegrees),
    })
    setPlacementModalIndex(null)
  }

  return (
    <>
      <Dialog
        open={open}
        onOpenChange={(next) => {
          if (!next && placementModalIndex === null) motionStore.closeImportForm()
        }}
      >
        <DialogContent
          className="flex max-h-[85vh] w-[calc(100vw-2rem)] max-w-3xl flex-col gap-0 overflow-hidden p-0"
          data-testid="import-dialog"
          onPointerDownOutside={(event) => {
            if (placementModalIndex !== null) event.preventDefault()
          }}
          onInteractOutside={(event) => {
            if (placementModalIndex !== null) event.preventDefault()
          }}
          onEscapeKeyDown={(event) => {
            if (placementModalIndex !== null) event.preventDefault()
          }}
        >
          <DialogHeader className="shrink-0 border-b border-border/60 px-5 pt-5 pb-3 pr-10">
            <DialogTitle>{editing ? '管理选中曲目' : '添加到曲库'}</DialogTitle>
            <DialogDescription>
              支持单人曲与多机位团曲导入：可追加位置并配置相对 1 号位的初始偏移与朝向；共用同一镜头与音频轨道。
            </DialogDescription>
          </DialogHeader>

          <div className="min-h-0 flex-1 space-y-4 overflow-y-auto px-5 py-4">
            {!importSupported && (
              <Alert variant="warning">
                <RiInformationFill className="size-4" />
                <AlertDescription>当前原生桥未提供导入管线（library.import），无法真正导入。</AlertDescription>
              </Alert>
            )}
            {motionStore.isEditingActiveTrack() && (
              <Alert variant="warning">
                <RiAlertFill className="size-4" />
                <AlertDescription>正在编辑当前播放曲目；替换资源前需先停止播放。</AlertDescription>
              </Alert>
            )}

            <div className="grid gap-3 sm:grid-cols-2">
              <div className="min-w-0 space-y-1.5">
                <Label htmlFor="import-title">曲目标题</Label>
                <Input
                  id="import-title"
                  placeholder="自动根据文件名推荐"
                  value={form.title}
                  onChange={(event) => motionStore.setImportTitle(event.target.value)}
                />
              </div>
              <div className="min-w-0 space-y-1.5">
                <Label htmlFor="import-subtitle">副标题 / 作者</Label>
                <Input
                  id="import-subtitle"
                  placeholder="可选，多个作者用半角逗号 , 分隔"
                  value={form.subtitle}
                  onChange={(event) => motionStore.setImportSubtitle(event.target.value)}
                />
              </div>
            </div>

            <ImportCoverSlot form={form} />

            <div className="space-y-3">
              <div className="flex items-center justify-between gap-2">
                <p className="text-base font-medium">演出轨道与位置成员</p>
                <div className="flex items-center gap-2">
                  <Button
                    variant="outline"
                    size="sm"
                    disabled={busy || !dialogSupported}
                    onClick={() => void motionStore.browseFiles('append')}
                  >
                    <RiFolderAddFill className="size-3.5" />
                    追加文件…
                  </Button>
                  <Button
                    type="button"
                    variant="outline"
                    size="sm"
                    disabled={busy}
                    onClick={() => motionStore.addImportPosition()}
                  >
                    <RiAddFill className="size-3.5" />
                    追加位置
                  </Button>
                  {!dialogSupported && <PendingBadge />}
                </div>
              </div>

              <ImportPositionCards
                form={form}
                busy={busy}
                dialogSupported={dialogSupported}
                onOpenPlacementModal={openPlacementModal}
              />

              {/* Shared Camera & Audio card */}
              <div className="overflow-hidden rounded-lg border bg-card" data-testid="import-shared-camera-audio">
                <div className="border-b border-border/60 bg-muted/25 px-3 py-2 text-sm font-semibold text-foreground">
                  共享镜头与音频
                </div>
                <div className="divide-y divide-border/60">
                  <ImportTrackRow
                    slotKey="camera"
                    slotState={form.slots.camera}
                    slotAriaLabel="镜头槽位"
                    timing={form.timing.camera}
                    showTiming
                    busy={busy}
                    dialogSupported={dialogSupported}
                    onSelectChange={(sel) => motionStore.setImportSelection('camera', sel)}
                    onBrowse={() => void motionStore.browseFiles('camera')}
                  />
                  <ImportTrackRow
                    slotKey="audio"
                    slotState={form.slots.audio}
                    slotAriaLabel="音频槽位"
                    timing={form.timing.audio}
                    showTiming
                    busy={busy}
                    dialogSupported={dialogSupported}
                    onSelectChange={(sel) => motionStore.setImportSelection('audio', sel)}
                    onBrowse={() => void motionStore.browseFiles('audio')}
                  />
                </div>
              </div>

              {form.audioProbe?.valid && (
                <div
                  data-testid="import-audio-preview"
                  className="flex min-w-0 items-center gap-3 rounded-lg border border-border/70 bg-muted/30 p-2.5"
                >
                  {form.audioProbe.hasCover && form.audioProbe.coverDataUri && (
                    <img
                      src={form.audioProbe.coverDataUri}
                      alt=""
                      data-testid="import-cover-preview"
                      className="size-12 shrink-0 rounded-md border border-border/60 object-cover"
                    />
                  )}
                  <div className="min-w-0 flex-1 space-y-0.5 text-sm">
                    <div className="truncate font-medium text-foreground">
                      {form.audioProbe.title || '音频元数据'}
                      {form.audioProbe.artist && (
                        <span className="ml-2 text-muted-foreground">· {form.audioProbe.artist}</span>
                      )}
                    </div>
                    <div className="text-xs text-muted-foreground tabular-nums">
                      {form.audioProbe.codec.toUpperCase()} · {Math.round(form.audioProbe.sampleRate / 100) / 10} kHz ·{' '}
                      {form.audioProbe.durationSeconds.toFixed(1)}s
                    </div>
                  </div>
                </div>
              )}

              {unsupportedAudio > 0 && (
                <p className="text-xs text-muted-foreground">
                  {unsupportedAudio} 个不支持的音频文件未自动填入，可用“追加文件”后手动选择。
                </p>
              )}

              <div className="grid gap-3 rounded-lg border bg-card/60 p-3 sm:grid-cols-2">
                <div className="space-y-1.5">
                  <Label htmlFor="import-source-fps" className="text-xs">源动作帧率</Label>
                  <Select
                    value={String(form.sourceFps)}
                    onValueChange={(value) => motionStore.setImportSourceFps(Number(value))}
                  >
                    <SelectTrigger id="import-source-fps" className="h-8 text-xs" aria-label="源动作帧率">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="0">自动检测</SelectItem>
                      <SelectItem value="25">25 fps</SelectItem>
                      <SelectItem value="30">30 fps</SelectItem>
                      <SelectItem value="60">60 fps</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                <div className="space-y-1.5">
                  <Label htmlFor="import-output-fps" className="text-xs">输出采样帧率</Label>
                  <Select
                    value={String(form.fps)}
                    onValueChange={(value) => motionStore.setImportOutputFps(Number(value))}
                  >
                    <SelectTrigger id="import-output-fps" className="h-8 text-xs" aria-label="输出采样帧率">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="0">自动（保持原有）</SelectItem>
                      <SelectItem value="30">30 fps</SelectItem>
                      <SelectItem value="60">60 fps</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                <p className="sm:col-span-2 text-xs text-muted-foreground">
                  自动检测依据身体骨骼与音频；修改已有曲目的帧率会重新烘焙动作。
                </p>
              </div>
            </div>

            {form.activeTrackWarning && (
              <p className="break-all [overflow-wrap:anywhere] text-sm text-destructive">{form.activeTrackWarning}</p>
            )}
            {formError && importSupported && (
              <p className="break-all [overflow-wrap:anywhere] text-xs text-muted-foreground">提示：{formError}</p>
            )}
          </div>

          <DialogFooter className="shrink-0 border-t border-border/60 bg-card px-5 py-3.5">
            <Button variant="outline" onClick={() => motionStore.closeImportForm()}>
              取消
            </Button>
            {motionStore.isEditingActiveTrack() && (
              <Button variant="outline" disabled={form.pendingStopAndSave} onClick={() => void motionStore.stopPlaybackAndSave()}>
                停止并保存
              </Button>
            )}
            <Button
              disabled={!importSupported || !!formError || busy}
              title={importSupported ? undefined : '导入管线待迁移'}
              onClick={() => void motionStore.submitImport()}
            >
              {editing ? '保存修改' : '导入'}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <ImportPlacementModal
        posIndex={placementModalIndex}
        draft={placementDraft}
        onChange={setPlacementDraft}
        onCancel={() => setPlacementModalIndex(null)}
        onCommit={commitPlacementModal}
      />

      <CoverEditorModal owner="import" />
    </>
  )
}
