import { cn } from '@/lib/utils'

interface DefaultAlbumCoverProps {
  className?: string
  testId?: string
}

export function DefaultAlbumCover({ className, testId }: DefaultAlbumCoverProps) {
  return (
    <div
      data-testid={testId}
      className={cn(
        'relative flex size-full items-center justify-center overflow-hidden bg-gradient-to-br from-slate-800 via-slate-900 to-zinc-950 text-slate-200 dark:from-zinc-800 dark:via-zinc-900 dark:to-black',
        className,
      )}
    >
      <svg
        viewBox="0 0 64 64"
        fill="none"
        aria-hidden="true"
        className="size-[76%] opacity-90"
      >
        <circle cx="32" cy="32" r="26" fill="currentColor" fillOpacity="0.08" stroke="currentColor" strokeOpacity="0.28" strokeWidth="1.5" />
        <circle cx="32" cy="32" r="19" stroke="currentColor" strokeOpacity="0.14" strokeWidth="1" />
        <circle cx="32" cy="32" r="13" stroke="currentColor" strokeOpacity="0.14" strokeWidth="1" />
        <circle cx="32" cy="32" r="7.5" fill="currentColor" fillOpacity="0.18" stroke="currentColor" strokeOpacity="0.45" strokeWidth="1.5" />
        <circle cx="32" cy="32" r="2.5" fill="currentColor" fillOpacity="0.85" />
        <path
          d="M36 16v14.2a4.2 4.2 0 1 1-2-3.57V19.5l9-2.1v4.1l-7 1.65"
          stroke="currentColor"
          strokeOpacity="0.72"
          strokeWidth="1.8"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
    </div>
  )
}

interface AlbumCoverProps {
  src?: string
  className?: string
  imgTestId?: string
  defaultTestId?: string
}

export function AlbumCover({ src, className, imgTestId, defaultTestId }: AlbumCoverProps) {
  if (src) {
    return (
      <img
        src={src}
        alt=""
        data-testid={imgTestId}
        className={cn('size-full object-cover', className)}
      />
    )
  }
  return <DefaultAlbumCover className={className} testId={defaultTestId} />
}
