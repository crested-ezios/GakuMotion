import * as React from 'react'
import { Checkbox as BaseCheckbox } from '@base-ui/react/checkbox'
import { Check } from 'lucide-react'
import { cn } from '@/lib/utils'

export interface CheckboxProps
  extends Omit<React.ComponentPropsWithoutRef<typeof BaseCheckbox.Root>, 'onCheckedChange'> {
  onCheckedChange?: (checked: boolean) => void
}

const Checkbox = React.forwardRef<HTMLElement, CheckboxProps>(
  ({ className, onCheckedChange, ...props }, ref) => (
    <BaseCheckbox.Root
      ref={ref}
      nativeButton
      render={<button type="button" />}
      onCheckedChange={onCheckedChange ? (checked) => onCheckedChange(checked) : undefined}
      className={cn(
        'peer size-4 shrink-0 rounded-sm border border-input shadow-sm focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-1 focus-visible:ring-offset-background disabled:cursor-not-allowed disabled:opacity-50 data-[checked]:border-primary data-[checked]:bg-primary data-[checked]:text-primary-foreground transition-colors',
        className,
      )}
      {...props}
    >
      <BaseCheckbox.Indicator className={cn('flex items-center justify-center text-current')}>
        <Check className="size-3.5" strokeWidth={3} />
      </BaseCheckbox.Indicator>
    </BaseCheckbox.Root>
  ),
)
Checkbox.displayName = 'Checkbox'

export { Checkbox }
