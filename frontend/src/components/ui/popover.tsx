import * as React from 'react'
import { Popover as BasePopover } from '@base-ui/react/popover'
import { cn } from '@/lib/utils'

interface PopoverPinnedContextValue {
  setPinned: (pinned: boolean) => void
}

const PopoverPinnedContext = React.createContext<PopoverPinnedContextValue | null>(null)

export interface PopoverProps {
  open?: boolean
  defaultOpen?: boolean
  onOpenChange?: (open: boolean) => void
  className?: string
  children: React.ReactNode
}

function isProtectedOverlayTarget(target: Node): boolean {
  if (!(target instanceof Element)) return false
  return (
    target.closest('[role="listbox"]') !== null ||
    target.closest('[data-popover-ignore-outside]') !== null
  )
}

/**
 * Popover built on `@base-ui/react/popover`, preserving pinned drawer
 * semantics and ignoring outside presses inside nested portaled listboxes.
 */
export function Popover({ open, defaultOpen = false, onOpenChange, className, children }: PopoverProps) {
  const pinnedRef = React.useRef(false)
  const ctxValue = React.useMemo<PopoverPinnedContextValue>(
    () => ({
      setPinned: (next) => {
        pinnedRef.current = next
      },
    }),
    [],
  )

  return (
    <PopoverPinnedContext.Provider value={ctxValue}>
      <BasePopover.Root
        open={open}
        defaultOpen={defaultOpen}
        modal={false}
        onOpenChange={(next, eventDetails) => {
          if (!next && pinnedRef.current && (eventDetails.reason === 'outside-press' || eventDetails.reason === 'focus-out')) {
            eventDetails.cancel()
            return
          }
          if (!next && eventDetails.reason === 'outside-press') {
            const rawEvent = eventDetails.event as { target?: EventTarget | null } | undefined
            const target = rawEvent?.target as Node | null | undefined
            if (target && isProtectedOverlayTarget(target)) {
              eventDetails.cancel()
              return
            }
          }
          onOpenChange?.(next)
        }}
      >
        {className ? <div className={cn('relative inline-flex', className)}>{children}</div> : children}
      </BasePopover.Root>
    </PopoverPinnedContext.Provider>
  )
}

export const PopoverTrigger = BasePopover.Trigger

export interface PopoverContentProps extends React.ComponentPropsWithoutRef<typeof BasePopover.Popup> {
  side?: React.ComponentPropsWithoutRef<typeof BasePopover.Positioner>['side']
  align?: React.ComponentPropsWithoutRef<typeof BasePopover.Positioner>['align']
  sideOffset?: number
  pinned?: boolean
}

export const PopoverContent = React.forwardRef<HTMLDivElement, PopoverContentProps>(
  ({ side = 'top', align = 'end', sideOffset = 8, pinned = false, className, children, ...props }, ref) => {
    const ctx = React.useContext(PopoverPinnedContext)
    React.useEffect(() => {
      ctx?.setPinned(pinned)
      return () => ctx?.setPinned(false)
    }, [ctx, pinned])

    return (
      <BasePopover.Portal>
        <BasePopover.Positioner side={side} align={align} sideOffset={sideOffset} className="z-50 outline-none">
          <BasePopover.Popup
            ref={ref}
            className={cn(
              'rounded-lg border bg-popover p-3 text-popover-foreground shadow-lg animate-fade-in outline-none',
              className,
            )}
            {...props}
          >
            {children}
          </BasePopover.Popup>
        </BasePopover.Positioner>
      </BasePopover.Portal>
    )
  },
)
PopoverContent.displayName = 'PopoverContent'
