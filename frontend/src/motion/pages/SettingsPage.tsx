import { RiCpuFill, RiFlaskFill, RiFolderOpenFill, RiKeyboardBoxFill } from '@remixicon/react'
import { Button } from '@/components/ui/button'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Switch } from '@/components/ui/switch'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { PHYSICS_BACKENDS } from '../protocol'
import { motionStore, useMotionStore } from '../store'
import { CollapsiblePropertySection, PropertyRow, PropertySection } from '../components/PropertyGroup'
import { PendingBadge } from '../components/Unsupported'

const HOTKEY_ROWS: Array<[string, string, string]> = [
  ['Ctrl + M', '唤出控制面板', '打开或唤回 GakuMotion 控制窗口'],
  ['F9', '播放 / 暂停 / 继续', '控制当前曲目；暂停后从当前位置继续'],
  ['F10', '停止并恢复游戏状态', '停止表演，复位进度并恢复原生角色控制'],
  [', / .', '上一曲 / 下一曲', '切换播放列表中的曲目'],
  ['- / =', '后退 / 前进 5 秒', '相对当前位置跳转'],
  ['Shift + F11', '重新加载骨骼微调', '从磁盘重新载入骨骼覆盖设置'],
  ['Ctrl + F11', '切换骨骼微调启用', '开启或暂时旁路骨骼覆盖'],
]

const PANEL_SHORTCUTS: Array<[string, string]> = [
  ['Space', '播放 / 暂停（非输入框焦点）'],
  ['← / →', '按设定步长快退 / 快进'],
  ['Shift + ← / →', '逐帧后退 / 前进'],
  ['Ctrl + K 或 /', '聚焦曲库搜索框'],
]

export function SettingsPage() {
  const physicsBackend = useMotionStore((state) => state.physicsBackend)
  const developerMode = useMotionStore((state) => state.developerMode)
  const motionsRoot = useMotionStore((state) => state.motionsRoot)
  const pluginVersion = useMotionStore((state) => state.pluginVersion)
  const bridgeKind = useMotionStore((state) => state.bridgeKind)
  const capabilities = useMotionStore((state) => state.capabilities)
  const physicsSupported = capabilities.includes('settings.setPhysics')
  const openFolderSupported = capabilities.includes('library.openFolder')

  return (
    <div className="mx-auto max-w-4xl space-y-4" data-testid="settings-page">
      {/* Physics & Runtime */}
      <PropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiCpuFill className="size-3.5 text-primary" />
            <span>物理引擎与演出仿真</span>
          </span>
        }
        description="选择在本次运行中生效；重启或控制文件变更后读取 control.json 的设置。"
        badge={!physicsSupported ? <PendingBadge /> : undefined}
      >
        <PropertyRow label="物理引擎" description="控制裙摆、发丝与饰品布料物理的计算后端">
          <Select
            items={Object.fromEntries(PHYSICS_BACKENDS.map((label, index) => [String(index), label]))}
            value={String(physicsBackend)}
            disabled={!physicsSupported}
            onValueChange={(value) => void motionStore.setPhysicsBackend(Number(value))}
          >
            <SelectTrigger className="h-8 w-[248px] text-xs" aria-label="物理引擎">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {PHYSICS_BACKENDS.map((label, index) => (
                <SelectItem key={label} value={String(index)}>
                  {label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        </PropertyRow>

        {physicsBackend === 2 && (
          <div className="bg-warning/10 px-3.5 py-2 text-xs text-warning">
            已选择原生物理实验；原生接口不可用时回退到内建物理。选择后端不代表原生物理已实际运行。
          </div>
        )}
        {physicsBackend === 1 && (
          <div className="bg-muted/35 px-3.5 py-2 text-xs text-muted-foreground">
            内建弹簧骨为保底方案，不包含官方碰撞模型。
          </div>
        )}
      </PropertySection>

      {/* Keyboard Shortcuts (Progressive disclosure) */}
      <CollapsiblePropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiKeyboardBoxFill className="size-3.5 text-primary" />
            <span>常用快捷键</span>
          </span>
        }
        description="全局热键在输入框聚焦时自动抑制，防止打字误触发播放或切歌"
        defaultOpen
        summary="7 项全局热键 · 4 项面板快捷键"
      >
        <div className="space-y-3.5">
          <div className="overflow-hidden rounded-md border bg-background/60">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead className="w-[120px]">快捷键</TableHead>
                  <TableHead className="w-[170px]">功能</TableHead>
                  <TableHead>说明</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {HOTKEY_ROWS.map(([key, func, desc]) => (
                  <TableRow key={key}>
                    <TableCell>
                      <kbd className="rounded border border-border bg-muted px-1.5 py-0.5 font-mono text-xs text-primary">
                        {key}
                      </kbd>
                    </TableCell>
                    <TableCell className="text-sm font-medium">{func}</TableCell>
                    <TableCell className="text-xs text-muted-foreground">{desc}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </div>

          <div className="space-y-1.5">
            <p className="text-xs font-medium text-muted-foreground">面板内快捷操作（WebView2 窗口聚焦时）</p>
            <div className="grid gap-1.5 sm:grid-cols-2">
              {PANEL_SHORTCUTS.map(([key, desc]) => (
                <div key={key} className="flex items-center justify-between gap-2 rounded-md border bg-background/50 px-3 py-1.5 text-xs">
                  <kbd className="rounded border border-border bg-muted px-1.5 py-0.5 font-mono text-xs text-foreground">
                    {key}
                  </kbd>
                  <span className="text-muted-foreground">{desc}</span>
                </div>
              ))}
            </div>
          </div>
        </div>
      </CollapsiblePropertySection>

      {/* Workspace & Developer Options */}
      <PropertySection
        title={
          <span className="inline-flex items-center gap-1.5">
            <RiFlaskFill className="size-3.5 text-primary" />
            <span>存储目录与开发者选项</span>
          </span>
        }
        description="管理本地动作库位置与实验室调试入口"
      >
        <PropertyRow
          label="曲库根目录"
          description={<code className="font-mono text-xs">{motionsRoot || 'gakumas-local/motions'}</code>}
        >
          <Button
            variant="outline"
            size="sm"
            className="h-8 text-xs"
            disabled={!openFolderSupported}
            onClick={() => void motionStore.openLibraryFolder()}
          >
            <RiFolderOpenFill className="size-3.5" />
            打开文件夹
          </Button>
        </PropertyRow>

        <PropertyRow
          label="开发者模式"
          htmlFor="developer-mode"
          description="开启后在侧边栏显示 [开发] 页，包含诊断、鞋底测量、探针与实验性开关。会话内有效，不持久化。"
        >
          <span className="rounded-[4px] border border-border bg-muted/60 px-1.5 py-0.5 font-mono text-micro text-muted-foreground">
            {bridgeKind} {pluginVersion || ''}
          </span>
          <Switch
            id="developer-mode"
            checked={developerMode}
            onCheckedChange={(checked) => motionStore.setDeveloperMode(checked)}
          />
        </PropertyRow>
      </PropertySection>
    </div>
  )
}
