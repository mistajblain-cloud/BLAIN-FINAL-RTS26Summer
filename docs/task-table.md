## Tasks & timing (WCET evidence)

| Task | Period T (ms) | WCET C (ms) | U = C/T | Priority | Deadline (ms) |
|------|--------------:|------------:|---------:|---------:|--------------:|
| Security Tamper | 10 | 0.217 | 0.022 | 15 | 10 |
| Intrusion Analysis | 20 | 4.443 | 0.222 | 10 | 20 |
| Integrity Verification | 50 | 2.140 | 0.043 | 5 | 50 |
| Pending Event Prioritization | 100 | 3.099 | 0.031 | 2 | 100 |
| **Total Utilization** | | | **0.318** | | |

Total utilization U = 0.318  
The measured total utilization is U=0.318, which is below both the four-task
Rate-Monotonic bound of 0.757 and the EDF bound of 1.0. Therefore, it is schedalable under both assumptions.

