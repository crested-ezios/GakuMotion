import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { createRequire } from 'node:module'
import { pathToFileURL } from 'node:url'
import { E2EReporter } from './e2e_reporter.mjs'

const suiteDir = import.meta.dirname
const repoRoot = path.resolve(suiteDir, '..', '..', '..')
const frontendDir = path.join(repoRoot, 'frontend')
const frontendPkgPath = path.join(frontendDir, 'package.json')
const virtualImporter = path.join(frontendDir, '__e2e_virtual_importer__.ts')

const req = createRequire(frontendPkgPath)
const { defineConfig } = await import(pathToFileURL(req.resolve('vitest/config')).href)
const reactModule = await import(pathToFileURL(req.resolve('@vitejs/plugin-react')).href)
const reactPlugin = reactModule.default ?? reactModule

const tier = process.env.GKM_E2E_TIER || 'All'
const buildDir = process.env.GKM_E2E_BUILD_DIR || path.join(repoRoot, 'build', 'e2e_refactor', 'frontend')
const outJson = process.env.GKM_E2E_OUT_JSON || path.join(buildDir, 'suite_results.json')

fs.mkdirSync(buildDir, { recursive: true })

function frontendDepsPlugin() {
  return {
    name: 'gkm-e2e-frontend-deps',
    enforce: 'pre',
    async resolveId(source, importer, options) {
      if (
        !source ||
        source.startsWith('.') ||
        source.startsWith('/') ||
        source.startsWith('\0') ||
        source.startsWith('node:') ||
        /^[A-Za-z]:/.test(source) ||
        source.startsWith('@/') ||
        source === 'vitest' ||
        source.startsWith('vitest/') ||
        source.startsWith('@vitest/')
      ) {
        return null
      }
      if (importer && importer.startsWith(frontendDir)) {
        return null
      }
      const resolved = await this.resolve(source, virtualImporter, { ...options, skipSelf: true })
      return resolved ?? null
    },
  }
}

const includeByTier =
  tier === '1'
    ? ['tier1_*.test.ts*']
    : tier === '2'
      ? ['tier2_*.test.ts*']
      : tier === '3'
        ? ['tier3_*.test.ts*']
        : tier === '4'
          ? ['tier4_*.test.ts*']
          : ['tier[1-4]_*.test.ts*']

export default defineConfig({
  root: suiteDir,
  cacheDir: path.join(buildDir || os.tmpdir(), 'vite-cache'),
  plugins: [frontendDepsPlugin(), reactPlugin()],
  resolve: {
    alias: {
      '@': path.join(frontendDir, 'src'),
    },
  },
  server: {
    fs: {
      allow: [suiteDir, repoRoot],
    },
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: [path.join(suiteDir, 'setup.ts')],
    css: false,
    fsModuleCache: false,
    passWithNoTests: false,
    testTimeout: 15000,
    include: includeByTier,
    reporters: ['default', new E2EReporter({ outFile: outJson, tierFilter: tier })],
  },
})
