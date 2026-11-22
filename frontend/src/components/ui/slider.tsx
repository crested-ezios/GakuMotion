import * as React from 'react'
import { Slider as BaseSlider } from '@base-ui/react/slider'
import { cn } from '@/lib/utils'

export interface SliderProps
  extends Omit<
    React.ComponentPropsWithoutRef<typeof BaseSlider.Root<number[]>>,
    'value' | 'defaultValue' | 'onValueChange' | 'onValueCommitted'
  > {
  value?: number[]
  defaultValue?: number[]
  onValueChange?: (value: number[]) => void
  onValueCommit?: (value: number[]) => void
}

function toArray(val: number | readonly number[]): number[] {
  return Array.isArray(val) ? [...val] : [val as number]
}

const Slider = React.forwardRef<HTMLDivElement, SliderProps>(
  ({ className, 'aria-label': ariaLabel, disabled, onValueChange, onValueCommit, ...props }, ref) => (
    <BaseSlider.Root
      ref={ref}
      aria-label={ariaLabel}
      disabled={disabled}
      onValueChange={onValueChange ? (val) => onValueChange(toArray(val)) : undefined}
      onValueCommitted={onValueCommit ? (val) => onValueCommit(toArray(val)) : undefined}
      className={cn(
        'relative flex w-full touch-none select-none items-center data-[orientation=vertical]:h-full data-[orientation=vertical]:w-auto data-[orientation=vertical]:flex-col data-[disabled]:opacity-50',
        className,
      )}
      {...props}
    >
      <BaseSlider.Control className="relative flex w-full items-center data-[orientation=vertical]:h-full data-[orientation=vertical]:w-auto data-[orientation=vertical]:flex-col">
        <BaseSlider.Track className="relative h-1.5 w-full grow overflow-hidden rounded-full bg-secondary data-[orientation=vertical]:h-full data-[orientation=vertical]:w-1.5">
          <BaseSlider.Indicator className="absolute h-full bg-primary data-[orientation=vertical]:w-full" />
          <BaseSlider.Thumb
            aria-label={ariaLabel}
            inputRef={(el) => {
              if (!el) return
              if (disabled) el.setAttribute('data-disabled', '')
              else el.removeAttribute('data-disabled')
            }}
            className="block size-3.5 rounded-full border border-primary/60 bg-background shadow-sm transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-1 focus-visible:ring-offset-background disabled:pointer-events-none"
          />
        </BaseSlider.Track>
      </BaseSlider.Control>
    </BaseSlider.Root>
  ),
)
Slider.displayName = 'Slider'

export { Slider }
