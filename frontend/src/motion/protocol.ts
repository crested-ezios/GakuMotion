// Typed mirror of the GakuMotion web-panel bridge (gakumotion.web-panel/v1).
// The native service is authoritative; this file is the frontend contract.
// See frontend/docs/motion-bridge-protocol.md for the wire format and semantics.

import { BridgeError, PROTOCOL_VERSION, type RequestOptions } from '@/bridge/protocol'

export { BridgeError, PROTOCOL_VERSION }
export type { RequestOptions }

export type * from './protocol/coreTypes'
export type * from './protocol/phase2Types'
export type * from './protocol/rpc'
export * from './protocol/defaults'
export * from './protocol/bones'
