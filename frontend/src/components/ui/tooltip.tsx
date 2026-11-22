import * as React from 'react'
import { Tooltip as BaseTooltip } from '@base-ui/react/tooltip'
import { cn } from '@/lib/utils'

export interface TooltipProviderProps extends React.ComponentPropsWithoutRef<typeof BaseTooltip.Provider> {
  delayDuration?: number
}

function TooltipProvider({ delayDuration, delay, ...props }: TooltipProviderProps) {
  return <BaseTooltip.Provider delay={delayDuration ?? delay} {...props} />
}

const Tooltip = BaseTooltip.Root
const TooltipTrigger = BaseTooltip.Trigger

export interface TooltipContentProps extends React.ComponentPropsWithoutRef<typeof BaseTooltip.Popup> {
  side?: React.ComponentPropsWithoutRef<typeof BaseTooltip.Positioner>['side']
  align?: React.ComponentPropsWithoutRef<typeof BaseTooltip.Positioner>['align']
  sideOffset?: number
}

const TooltipContent = React.forwardRef<HTMLDivElement, TooltipContentProps>(
  ({ className, side = 'top', align = 'center', sideOffset = 4, ...props }, ref) => (
    <BaseTooltip.Portal>
      <BaseTooltip.Positioner side={side} align={align} sideOffset={sideOffset} className="z-50 outline-none">
        <BaseTooltip.Popup
          ref={ref}
          className={cn(
            'overflow-hidden rounded-md border bg-popover px-2.5 py-1 text-xs text-popover-foreground shadow-md animate-in fade-in-0 zoom-in-95',
            className,
          )}
          {...props}
        />
      </BaseTooltip.Positioner>
    </BaseTooltip.Portal>
  ),
)
TooltipContent.displayName = 'TooltipContent'

export { Tooltip, TooltipTrigger, TooltipContent, TooltipProvider }
