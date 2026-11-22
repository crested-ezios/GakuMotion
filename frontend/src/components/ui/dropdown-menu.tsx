import * as React from 'react'
import { Menu as BaseMenu } from '@base-ui/react/menu'
import { RiCheckFill } from '@remixicon/react'
import { cn } from '@/lib/utils'

const DropdownMenu = BaseMenu.Root
const DropdownMenuTrigger = BaseMenu.Trigger
const DropdownMenuGroup = BaseMenu.Group

export interface DropdownMenuContentProps extends React.ComponentPropsWithoutRef<typeof BaseMenu.Popup> {
  side?: React.ComponentPropsWithoutRef<typeof BaseMenu.Positioner>['side']
  align?: React.ComponentPropsWithoutRef<typeof BaseMenu.Positioner>['align']
  sideOffset?: number
}

const DropdownMenuContent = React.forwardRef<HTMLDivElement, DropdownMenuContentProps>(
  ({ className, side = 'bottom', align = 'center', sideOffset = 4, ...props }, ref) => (
    <BaseMenu.Portal>
      <BaseMenu.Positioner side={side} align={align} sideOffset={sideOffset} className="z-50 outline-none">
        <BaseMenu.Popup
          ref={ref}
          className={cn(
            'min-w-[9rem] overflow-hidden rounded-lg border bg-popover p-1 text-popover-foreground shadow-lg data-[open]:animate-fade-in outline-none',
            className,
          )}
          {...props}
        />
      </BaseMenu.Positioner>
    </BaseMenu.Portal>
  ),
)
DropdownMenuContent.displayName = 'DropdownMenuContent'

export interface DropdownMenuCheckboxItemProps
  extends Omit<React.ComponentPropsWithoutRef<typeof BaseMenu.CheckboxItem>, 'onCheckedChange'> {
  onCheckedChange?: (checked: boolean) => void
}

const DropdownMenuCheckboxItem = React.forwardRef<HTMLElement, DropdownMenuCheckboxItemProps>(
  ({ className, children, checked, onCheckedChange, closeOnClick = true, ...props }, ref) => (
    <BaseMenu.CheckboxItem
      ref={ref}
      checked={checked}
      closeOnClick={closeOnClick}
      onCheckedChange={onCheckedChange ? (next) => onCheckedChange(next) : undefined}
      className={cn(
        'relative flex cursor-pointer select-none items-center rounded-md py-1.5 pl-7 pr-2.5 text-sm outline-none transition-colors data-[highlighted]:bg-accent data-[highlighted]:text-accent-foreground data-[disabled]:pointer-events-none data-[disabled]:opacity-50',
        className,
      )}
      {...props}
    >
      <span className="absolute left-2 flex size-3.5 items-center justify-center">
        <BaseMenu.CheckboxItemIndicator>
          <RiCheckFill className="size-3.5 text-primary" />
        </BaseMenu.CheckboxItemIndicator>
      </span>
      {children}
    </BaseMenu.CheckboxItem>
  ),
)
DropdownMenuCheckboxItem.displayName = 'DropdownMenuCheckboxItem'

const DropdownMenuItem = React.forwardRef<
  HTMLElement,
  React.ComponentPropsWithoutRef<typeof BaseMenu.Item>
>(({ className, ...props }, ref) => (
  <BaseMenu.Item
    ref={ref}
    className={cn(
      'relative flex cursor-pointer select-none items-center rounded-md px-2.5 py-1.5 text-sm outline-none transition-colors data-[highlighted]:bg-accent data-[highlighted]:text-accent-foreground data-[disabled]:pointer-events-none data-[disabled]:opacity-50',
      className,
    )}
    {...props}
  />
))
DropdownMenuItem.displayName = 'DropdownMenuItem'

const DropdownMenuLabel = React.forwardRef<HTMLDivElement, React.HTMLAttributes<HTMLDivElement>>(
  ({ className, ...props }, ref) => (
    <div ref={ref} className={cn('px-2 py-1 text-xs font-medium text-muted-foreground', className)} {...props} />
  ),
)
DropdownMenuLabel.displayName = 'DropdownMenuLabel'

const DropdownMenuSeparator = React.forwardRef<
  HTMLDivElement,
  React.ComponentPropsWithoutRef<typeof BaseMenu.Separator>
>(({ className, ...props }, ref) => (
  <BaseMenu.Separator ref={ref} className={cn('-mx-1 my-1 h-px bg-border/60', className)} {...props} />
))
DropdownMenuSeparator.displayName = 'DropdownMenuSeparator'

export {
  DropdownMenu,
  DropdownMenuCheckboxItem,
  DropdownMenuContent,
  DropdownMenuGroup,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
}
