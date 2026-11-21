import type { MotionBridgeClient, MotionMethodName } from '../protocol'
import type { MotionStore } from '../store'
import type { MotionAppState } from './types'

export interface StoreContext {
  getState: () => MotionAppState
  set: (partial: Partial<MotionAppState>) => void
  client: () => MotionBridgeClient
  getBridge: () => MotionBridgeClient | null
  isStarted: () => boolean
  supports: (method: MotionMethodName) => boolean
  reportActionError: (error: unknown) => void
  store: MotionStore
}

export interface SliceBundle<TActions> {
  actions: TActions
  stopWatchers?: () => void
  reset?: () => void
}
