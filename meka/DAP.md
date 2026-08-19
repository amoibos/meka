# MEKA DAP - Debug Adapter Protocol

## Übersicht

MEKA kann nun über das **Debug Adapter Protocol (DAP)** ferngesteuert werden. DAP ist der Standard, den auch VS Code, Eclipse und andere IDEs verwenden. Zusätzlich gibt es einen Python REST-Proxy, über den KI-Tools (z.B. Claude) den Emulator steuern können.

```
AI/Claude → REST API (meka_proxy.py) → TCP/DAP → MEKA
VS Code/Eclipse → TCP/DAP direkt → MEKA
```

---

## 1. Schnellstart

### Emulator mit DAP starten

```bash
mekaw.exe -DAP_PORT 4711 game.sms
```

- Der DAP-Server lauscht nur auf `localhost` (keine externen Verbindungen)
- Port `0` = DAP deaktiviert (Standard)
- Debugger wird automatisch aktiviert, falls nicht schon per `-DEBUG` aktiv

### Proxy starten (für KI)

```bash
python meka_proxy.py --dap-port 4711 --http-port 8123
```

Der Proxy verbindet sich mit MEKA's DAP-Server und stellt REST-Endpunkte auf `http://localhost:8123` bereit.

---

## 2. DAP direkt nutzen (VS Code, etc.)

### Mit VS Code verbinden

Eine `launch.json`-Konfiguration:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "meka",
            "request": "attach",
            "name": "MEKA Z80 Debugger",
            "port": 4711
        }
    ]
}
```

### DAP Wire Protocol

Die DAP-Kommunikation verwendet das Standard-DAP-Format:

**Request:**
```
Content-Length: 54\r\n
\r\n
{"seq":1,"type":"request","command":"continue","arguments":{"threadId":1}}
```

**Response:**
```
Content-Length: 70\r\n
\r\n
{"seq":2,"type":"response","request_seq":1,"success":true,"command":"continue"}
```

**Event:**
```
Content-Length: 55\r\n
\r\n
{"seq":3,"type":"event","event":"stopped","body":{"reason":"breakpoint"}}
```

---

## 3. REST API (meka_proxy.py)

Der Proxy stellt folgende Endpunkte bereit (identisch zum `emulicious_proxy.py`):

### GET - Zustand abfragen

| Endpoint | Parameter | Beschreibung |
|----------|-----------|-------------|
| `/registers` | - | Alle Z80-Register als JSON (`{"PC":"0x1234","SP":"0xDFF0",...}`) |
| `/memory` | `?addr=C000&len=128` | Speicherdump (hex) ab Adresse |
| `/vram` | `?addr=1800&len=32` | VRAM auslesen |
| `/vdp` | - | VDP-Register (R0-R15) als JSON |
| `/stacktrace` | - | Call-Stack (Rücksprungadressen vom Stack) |
| `/status` | - | `{"paused":true,"registers":{...}}` (wenn pausiert) |
| `/events` | - | Letzte 20 DAP-Events |
| `/console` | `?n=50` | Letzte N Zeilen Proxy-Log |
| `/eval` | `?expr=PC` | Ausdruck auswerten (Register, Hex-Adressen, Symbole) |
| `/disassemble` | `?addr=0&count=8` | Disassembly ab Adresse |

### POST - Steuerung

| Endpoint | Parameter | Beschreibung |
|----------|-----------|-------------|
| `/pause` | - | Emulation anhalten, gibt Register zurück |
| `/continue` | - | Emulation fortsetzen bis nächster Breakpoint |
| `/step` | - | Step-over (eine Instruktion), gibt Register zurück |
| `/stepin` | - | Step-into (in CALL hinein) |
| `/breakpoint` | `?addr=0124` | Breakpoint setzen (Hex-Adresse) |
| `/breakpoint` | `?label=Main` | Breakpoint per Symbolname |
| `/clear_breakpoints` | - | Alle Breakpoints löschen |
| `/wait` | `?timeout=30` | Warten bis Breakpoint/Pause feuert |

### DELETE

| Endpoint | Parameter | Beschreibung |
|----------|-----------|-------------|
| `/breakpoint` | `?addr=0124` | Einzelnen Breakpoint löschen |

### Antwortformate

**GET /registers:**
```json
{
  "PC": "0x0124",
  "SP": "0xDFF0",
  "AF": "0x3C02",
  "A": "0x3C",
  "BC": "0x0001",
  "B": "0x00",
  "C": "0x01",
  "DE": "0x2000",
  "HL": "0xC000",
  "IX": "0x0000",
  "IY": "0x0000",
  "AF'": "0xFFFF",
  "BC'": "0x0000",
  "DE'": "0x0000",
  "HL'": "0x0000",
  "I": "0x00",
  "R": "0x2A",
  "IFF": "0x01"
}
```

**GET /memory?addr=C000&len=8:**
```json
{
  "addr": "$C000",
  "length": 8,
  "hex": "3C 0E 08 CD D0 01 C3 21",
  "bytes": ["$3C","$0E","$08","$CD","$D0","$01","$C3","$21"]
}
```

**POST /step → Antwort:**
```json
{
  "PC": "0x0127",
  "SP": "0xDFF0",
  "AF": "0x3C02",
  ...
}
```

---

## 4. Verwendung mit KI (Claude)

Die KI ruft den Proxy wie in `emulicious_proxy.py` über `webfetch` auf:

```python
# Beispiel: Register lesen
GET http://localhost:8123/registers

# Beispiel: Memory lesen
GET http://localhost:8123/memory?addr=C000&len=32

# Beispiel: Step ausführen
POST http://localhost:8123/step
  → Gibt die neuen Registerwerte zurück

# Beispiel: Breakpoint setzen und warten
POST http://localhost:8123/breakpoint?addr=0124
POST http://localhost:8123/wait?timeout=10
```

---

## 5. CLI Referenz

```
mekaw.exe -DAP_PORT <port> [ROM]  Startet MEKA mit DAP-Server auf <port>
mekaw.exe -HELP                  Zeigt alle CLI-Optionen
```

### Weitere nützliche Flags

```
-DEBUG          Debug-Modus aktivieren
-DEBUG_SCRIPT   Skript mit Debugger-Kommandos ausführen
-LOG <file>     Ausgabe in Log-Datei
```

---

## 6. Architektur

```
┌──────────────────────────────────────────────────┐
│ MEKA Emulator                                    │
│                                                  │
│  Main_Loop() → RunZ80_Debugging()                │
│       ↓                                         │
│  Debugger_Hook() → Machine_Debug_Start()         │
│       ↓                                         │
│  Debugger_DAP_HaltCallback()                     │
│       ↓                                         │
│  ┌──────────────────────┐                        │
│  │ DAP_Server (Thread)  │← TCP :4711            │
│  │  - AcceptLoop()      │                       │
│  │  - HandleClient()    │                       │
│  │  - SendEvent()       │                       │
│  └──────┬───────────────┘                        │
│         ↓                                       │
│  ┌──────────────────────┐                        │
│  │ DAP_Handler          │                        │
│  │  - continue/pause    │                        │
│  │  - step/stepIn/out   │                        │
│  │  - stackTrace/scopes │                        │
│  │  - evaluate/memory   │                        │
│  │  - breakpoints       │                        │
│  └──────┬───────────────┘                        │
│         ↓                                       │
│  MEKA Debugger (debugger.cpp)                    │
│   - Z80 Register                                  │
│   - Memory (RAM/ROM/VRAM)                        │
│   - Breakpoints                                  │
│   - Expression Evaluator                         │
└──────────────────────────────────────────────────┘
         ↕ TCP/DAP
┌──────────────────────────────────────────────────┐
│ meka_proxy.py (Python)                           │
│  REST API auf :8123                              │
└──────────────────────────────────────────────────┘
         ↕ HTTP
    KI (Claude) / VS Code / Externe Tools
```

---

## 7. Unterstützte DAP Features

| Feature | Status |
|---------|--------|
| Breakpoints (Adresse) | ✅ |
| Function Breakpoints (Symbol) | ✅ |
| Conditional Breakpoints | ❌ |
| Step Over / Step Into / Step Out | ✅ |
| Pause / Continue | ✅ |
| Registers (Z80) | ✅ |
| VDP Registers | ✅ |
| Memory Read | ✅ |
| Disassembly | ✅ |
| Expression Evaluation | ✅ |
| Set Variable (Register ändern) | ✅ |
| Stack Trace | ✅ |
| Launch (ROM laden) | ✅ |
| Reverse Debugging | ❌ |
| Data Breakpoints (Watchpoints) | ❌ |
