import fs from 'node:fs'
import path from 'node:path'

function stripAnsi(text) {
  return String(text ?? '').replace(/\u001b\[[0-9;?]*[ -/]*[@-~]/g, '')
}

function inferMetaFromTest(testCase) {
  const meta = typeof testCase.meta === 'function' ? testCase.meta() : {}
  if (meta && meta.e2eId && meta.e2eTier && meta.e2eFeature) {
    return {
      id: String(meta.e2eId),
      tier: Number(meta.e2eTier),
      feature: String(meta.e2eFeature),
      name: String(meta.e2eName || testCase.name || meta.e2eId),
    }
  }
  const full = String(testCase.name || 'UNKNOWN')
  const match = /^([A-Z0-9-]+):\s*(.+)$/s.exec(full)
  const id = match ? match[1] : full
  const name = match ? match[2] : full
  const tierMatch = /^T([1-4])-/.exec(id)
  const featMatch = /-(F\d+)-/.exec(id)
  return {
    id,
    tier: tierMatch ? Number(tierMatch[1]) : 1,
    feature: featMatch ? featMatch[1] : 'F14',
    name,
  }
}

export class E2EReporter {
  constructor(options = {}) {
    this.outFile = options.outFile
    this.tierFilter = options.tierFilter ?? 'All'
  }

  onTestRunEnd(testModules, unhandledErrors) {
    const records = []
    let moduleErrorIdx = 0

    for (const mod of testModules || []) {
      const modErrors = typeof mod.errors === 'function' ? mod.errors() : []
      if (Array.isArray(modErrors) && modErrors.length > 0) {
        moduleErrorIdx += 1
        const msg = modErrors.map((e) => stripAnsi(e?.message || String(e))).join('; ')
        records.push({
          id: `MOD-ERR-${moduleErrorIdx}`,
          tier: this.tierFilter !== 'All' ? Number(this.tierFilter) : 1,
          feature: 'F14',
          name: `Test module load/transform (${path.basename(mod.moduleId || 'unknown')})`,
          status: 'FAIL',
          detail: msg || 'Unknown module error',
        })
      }

      const tests = mod?.children?.allTests ? mod.children.allTests() : []
      for (const tc of tests) {
        const info = inferMetaFromTest(tc)
        if (this.tierFilter !== 'All' && Number(info.tier) !== Number(this.tierFilter)) {
          continue
        }
        const res = typeof tc.result === 'function' ? tc.result() : { state: 'failed' }
        let status = 'FAIL'
        let detail = ''

        if (res.state === 'passed') {
          status = 'PASS'
          detail = 'OK'
        } else if (res.state === 'skipped') {
          const note = stripAnsi(res.note || '')
          if (note.startsWith('PENDING_IMPL:')) {
            status = 'PENDING_IMPL'
            detail = note.slice('PENDING_IMPL:'.length).trim() || 'Pending M4/M5 implementation'
          } else {
            status = 'FAIL'
            detail = `Unexpected skip: ${note || '(no reason)'}`
          }
        } else if (res.state === 'failed') {
          status = 'FAIL'
          const errs = Array.isArray(res.errors) ? res.errors : []
          detail = errs.map((e) => stripAnsi(e?.message || String(e))).join(' | ') || 'Assertion failed'
        } else {
          status = 'FAIL'
          detail = `Unexpected test state: ${String(res.state)}`
        }

        records.push({
          id: info.id,
          tier: info.tier,
          feature: info.feature,
          name: info.name,
          status,
          detail,
        })
      }
    }

    if (Array.isArray(unhandledErrors) && unhandledErrors.length > 0) {
      unhandledErrors.forEach((err, idx) => {
        records.push({
          id: `UNHANDLED-ERR-${idx + 1}`,
          tier: this.tierFilter !== 'All' ? Number(this.tierFilter) : 1,
          feature: 'F14',
          name: 'Unhandled Vitest runner error',
          status: 'FAIL',
          detail: stripAnsi(err?.message || String(err)),
        })
      })
    }

    const dir = path.dirname(this.outFile)
    if (dir) {
      fs.mkdirSync(dir, { recursive: true })
    }
    fs.writeFileSync(this.outFile, `${JSON.stringify(records, null, 2)}\n`, { encoding: 'utf8' })
  }
}
