import * as React from 'react'
import { RiArrowRightSFill } from '@remixicon/react'
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from '@/components/ui/collapsible'
import { cn } from '@/lib/utils'

export interface PropertySectionProps {
  title?: React.ReactNode
  description?: React.ReactNode
  actions?: React.ReactNode
  badge?: React.ReactNode
  className?: string
  bodyClassName?: string
  testId?: string
  divided?: boolean
  children: React.ReactNode
}

/**
 * Apple System Settings style grouped property container:
 * flat surface, 1px hairline border, subtle row dividers, zero decorative gradients or heavy shadows.
 */
export function PropertySection({
  title,
  description,
  actions,
  badge,
  className,
  bodyClassName,
  testId,
  divided = true,
  children,
}: PropertySectionProps) {
  return (
    <section className={cn('space-y-1.5', className)} data-testid={testId}>
      {(title || actions || badge || description) && (
        <div className="flex flex-wrap items-end justify-between gap-2 px-0.5">
          <div className="min-w-0 flex-1 space-y-0.5">
            {title && (
              <div className="flex flex-wrap items-center gap-2">
                <h3 className="text-base font-semibold tracking-wide text-foreground">{title}</h3>
                {badge}
              </div>
            )}
            {description && <p className="text-xs text-muted-foreground">{description}</p>}
          </div>
          {actions && <div className="flex shrink-0 flex-wrap items-center gap-1.5">{actions}</div>}
        </div>
      )}
      <div
        className={cn(
          'rounded-lg border bg-card text-card-foreground',
          divided && 'divide-y divide-border/60',
          bodyClassName,
        )}
      >
        {children}
      </div>
    </section>
  )
}

export interface CollapsiblePropertySectionProps {
  title: React.ReactNode
  description?: React.ReactNode
  badge?: React.ReactNode
  summary?: React.ReactNode
  defaultOpen?: boolean
  open?: boolean
  onOpenChange?: (open: boolean) => void
  disabled?: boolean
  className?: string
  bodyClassName?: string
  testId?: string
  children: React.ReactNode
}

/**
 * Progressive-disclosure grouped property panel matching desktop inspector conventions.
 */
export function CollapsiblePropertySection({
  title,
  description,
  badge,
  summary,
  defaultOpen = false,
  open,
  onOpenChange,
  disabled,
  className,
  bodyClassName,
  testId,
  children,
}: CollapsiblePropertySectionProps) {
  return (
    <Collapsible
      defaultOpen={defaultOpen}
      open={open}
      onOpenChange={onOpenChange}
      className={cn('rounded-lg border bg-card text-card-foreground', className)}
      data-testid={testId}
    >
      <CollapsibleTrigger
        disabled={disabled}
        className="group flex w-full items-center justify-between gap-3 px-3.5 py-2.5 text-left transition-colors hover:bg-accent/45 disabled:pointer-events-none disabled:opacity-50"
      >
        <div className="min-w-0 flex-1 space-y-0.5">
          <div className="flex flex-wrap items-center gap-2">
            <RiArrowRightSFill className="size-4 text-muted-foreground transition-transform duration-150 group-data-[panel-open]:rotate-90" />
            <span className="text-base font-semibold text-foreground">{title}</span>
            {badge}
          </div>
          {description && <p className="pl-5 text-xs text-muted-foreground">{description}</p>}
        </div>
        {summary && <span className="shrink-0 text-xs text-muted-foreground">{summary}</span>}
      </CollapsibleTrigger>
      <CollapsibleContent className={cn('border-t border-border/60 px-3.5 py-3', bodyClassName)}>
        {children}
      </CollapsibleContent>
    </Collapsible>
  )
}

export interface PropertyRowProps {
  label: React.ReactNode
  description?: React.ReactNode
  htmlFor?: string
  className?: string
  children: React.ReactNode
}

/**
 * Single attribute row inside a PropertySection (left label/hint, right control).
 */
export function PropertyRow({ label, description, htmlFor, className, children }: PropertyRowProps) {
  return (
    <div className={cn('flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5', className)}>
      <div className="min-w-0 flex-1 space-y-0.5">
        {htmlFor ? (
          <label htmlFor={htmlFor} className="block cursor-pointer text-sm font-medium text-foreground">
            {label}
          </label>
        ) : (
          <div className="text-sm font-medium text-foreground">{label}</div>
        )}
        {description && <p className="text-xs text-muted-foreground">{description}</p>}
      </div>
      <div className="flex shrink-0 flex-wrap items-center gap-2">{children}</div>
    </div>
  )
}
