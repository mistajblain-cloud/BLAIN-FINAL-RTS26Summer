## Architecture

Four periodic tasks perform tamper detection, vibration analysis, integrity verification, and event prioritization.
An interrupt-driven button ISR signals a high-priority bottom-half task through a direct task notification. 
THe shared security state is protected by a mutex. Fixed priority schedulability was verified using Rate-Monotonic
scheduling with measured WCETs and utilization analysis.

```mermaid

  flowchart LR
      GPIO19["GPIO 19<br/>ISR Scope Pulse"]

      subgraph ISR_T["Interrupt: IRAM Top Half"]
          ISR["button_isr()<br/>Record ISR timestamp<br/>Increment press count"]
      end

      GPIO18 -->|"Falling-edge interrupt"| ISR
      ISR -->|"Toggle HIGH/LOW"| GPIO19

      subgraph CORE0["Core 0: System Services"]
          C0IDLE["Idle Task<br/>Priority 0"]
          SYS["ESP-IDF Services<br/>Timers / Runtime"]
      end

      subgraph CORE1["Core 1: Real-Time Security Tasks"]

          TA["Task A: tamper_detect()<br/>Period = 10 ms<br/>Priority = 15<br/>Updates tamper state"]

          NOT["btn_task_notif()<br/>Event-driven bottom half<br/>Priority = 12<br/>Processes button tamper/reset"]

          TB["Task B: intrusion_analysis()<br/>Period = 20 ms<br/>Priority = 10<br/>Updates vibration state"]

          TC["Task C: integrity_ver()<br/>Period = 50 ms<br/>Priority = 5<br/>Updates integrity state"]

          TD["Task D: pending_events()<br/>Period = 100 ms<br/>Priority = 2<br/>Reads state snapshot<br/>Prioritizes events"]
      end

      ISR -->|"vTaskNotifyGiveFromISR()"| NOT
      ISR -.->|"portYIELD_FROM_ISR()<br/>when higher_woken = true"| NOT

      MUTEX[("security_mutex")]

      TA -->|"Take / Give"| MUTEX
      NOT -->|"Take / Give"| MUTEX
      TB -->|"Take / Give"| MUTEX
      TC -->|"Take / Give"| MUTEX
      TD -->|"Take / Give"| MUTEX

      STATE[("Shared Security State<br/>security_state<br/>tamper_score / tamper_detected<br/>vibration_score / vibration_alert<br/>integrity_failures / integrity_fault")]

      MUTEX -->|"Protects access"| STATE

      EVENTS[("security_events[64]")]

      TD -->|"Generate and sort<br/>by severity"| EVENTS

      TA -.->|"Can preempt"| NOT
      NOT -.->|"Can preempt"| TB
      NOT -.->|"Can preempt"| TC
      NOT -.->|"Can preempt"| TD
      TB -.->|"Can preempt"| TC
      TB -.->|"Can preempt"| TD
      TC -.->|"Can preempt"| TD
```
