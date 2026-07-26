## Hazard analysis & standard mapping

| Hazard | Potential effect | Mitigation |
|---|---|---|
| Missed/delayed tamper event | Event may not be processed before its response deadline. | A minimal GPIO ISR records the event and wakes a priority-12 bottom-half task using a direct task notification. |
| Corrupted shared security state | Tasks may observe inconsistent combinations of tamper, vibration, integrity, and overall system state. | A FreeRTOS mutex protects multi-variable updates | 
| Integrity verification failure | Corrupted or unexpected data may be accepted as valid. | Task C calculates CRC-32, compares it with the expected value, records failures, and changes system state to `DEGRADED`. | 
| Mutex unavailable/held too long | Security tasks may block, time out, or skip shared-state updates. | Critical sections are kept short and mutex acquisitions use bounded timeouts. | 
| Excessive pending-event workload | Event prioritization may exceed its execution budget and interfere with higher-priority tasks. | Task D has the lowest priority, a fixed array size, a measured WCET, and a 100 ms period. |

