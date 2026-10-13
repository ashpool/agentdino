<p align="center">
  <img src="logo.png" alt="Agentdino" width="400">
</p>

An Arduino UNO R4 WiFi that shows Claude Code's status on its 12x8 LED matrix, one lane per Claude session. Claude Code hooks send commands to the board over USB serial or UDP.

## How it works

```mermaid
flowchart LR
    subgraph Laptop
        CC[Claude Code] -- "hook event + JSON on stdin" --> SP["signal.pl<br/>key = session_id[0:4]"]
    end
    SP -- "USB serial line<br/>(default, if board plugged in)" --> FW
    SP -. "UDP :4210 broadcast or AGENTDINO_HOST<br/>(fallback or wireless)" .-> FW
    subgraph Board["Arduino UNO R4 WiFi"]
        FW["signal.ino<br/>parse '&lt;key&gt;:&lt;cmds&gt;'"] --> L["6 lanes, one per session"]
        L --> M[/"12x8 LED matrix"/]
    end
```

Each lane follows the session's hooks:

```mermaid
stateDiagram-v2
    [*] --> FREE
    FREE --> WORKING: W (UserPromptSubmit, PostToolUse)
    WORKING --> WORKING: S / s (subagent dots), W (heartbeat)
    WORKING --> INPUT_NEEDED: I (permission prompt)
    INPUT_NEEDED --> WORKING: W
    WORKING --> IDLE: D (Stop, dances for 2 s)
    INPUT_NEEDED --> IDLE: D
    IDLE --> WORKING: W (next prompt)
    IDLE --> FREE: O or 1 h
    WORKING --> WORKING: E (flash)
    WORKING --> FREE: O (SessionEnd) or 10 min silent
    INPUT_NEEDED --> FREE: O or 10 min unanswered
```

See [AGENTS.md](AGENTS.md) for the protocol, hook wiring, setup and troubleshooting.
