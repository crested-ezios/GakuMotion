import fs from 'node:fs'
import path from 'node:path'

export type TsE2EStatus = 'PASS' | 'FAIL' | 'PENDING_IMPL'

export interface TsE2ERecord {
  id: string
  tier: number
  feature: string
  name: string
  status: TsE2EStatus
  detail: string
}

const collected: TsE2ERecord[] = []

export function recordTsE2E(record: TsE2ERecord): void {
  const idx = collected.findIndex((item) => item.id === record.id)
  if (idx >= 0) {
    collected[idx] = record
  } else {
    collected.push(record)
  }
}

export function flushTsE2EReport(): void {
  const outPath = process.env.GKE2E_TS_REPORT_JSON
  if (!outPath) return
  fs.mkdirSync(path.dirname(outPath), { recursive: true })
  fs.writeFileSync(outPath, JSON.stringify(collected, null, 2) + '\n', 'utf8')
}

export interface TsCaseHandle {
  skip: (reason?: string) => void
}

export async function runProbeCase(
  id: string,
  tier: number,
  feature: string,
  name: string,
  ctx: TsCaseHandle,
  probe: () => Promise<string | null> | string | null,
  execute: () => Promise<string | void> | string | void,
): Promise<void> {
  const pendingReason = await probe()
  if (pendingReason !== null) {
    recordTsE2E({
      id,
      tier,
      feature,
      name,
      status: 'PENDING_IMPL',
      detail: pendingReason,
    })
    ctx.skip(pendingReason)
    return
  }

  try {
    const detail = (await execute()) ?? ''
    recordTsE2E({
      id,
      tier,
      feature,
      name,
      status: 'PASS',
      detail,
    })
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    recordTsE2E({
      id,
      tier,
      feature,
      name,
      status: 'FAIL',
      detail: message,
    })
    throw err
  }
}
