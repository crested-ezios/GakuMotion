import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(here, '../../..')
const frontendDir = path.join(repoRoot, 'frontend')
const buildDir = process.env.GKE2E_BUILD_DIR || path.join(repoRoot, 'build/e2e_refactor/portrait_bridge')

export default {
  root: frontendDir,
  cacheDir: path.join(buildDir, 'vite-cache'),
  resolve: {
    alias: {
      '@': path.join(frontendDir, 'src'),
    },
  },
  server: {
    fs: {
      allow: [repoRoot],
    },
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: [path.join(frontendDir, 'src/test/setup.ts')],
    css: false,
    dir: here,
    include: ['**/*.test.ts'],
    cache: false,
    fsModuleCache: false,
  },
}
