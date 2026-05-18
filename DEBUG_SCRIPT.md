# Debug Script Feature

Erlaubt es, beim Start eine Textdatei mit Debugger-Befehlen zu übergeben. Der Emulator führt diese automatisch aus, sobald die Maschine an einem Breakpoint hält. Ausgaben landen im Debugger-Log.

## Nutzung

```
mekaw.exe game.sms -DEBUG_SCRIPT myscript.txt
```

**Beispiel-Skript:**
```
# Kommentare mit # werden ignoriert
B C000
C
P AF
P BC HL
M C000 32
QUIT
```

Verfügbare Befehle sind dieselben wie im interaktiven Debugger. `QUIT` / `EXIT` / `Q` beendet den Emulator.

---

## Implementierung (Vorlage für andere Projekte)

Das Grundprinzip ist universell anwendbar für jeden Emulator mit interaktivem Debugger:

### Voraussetzungen

- Bestehender Debugger mit Breakpoints und einer Funktion, die Befehle als String verarbeitet (hier: `Debugger_InputParseCommand(char*)`)
- Eine `Update()`-Funktion die einmal pro Frame aufgerufen wird, wenn die Maschine pausiert ist
- CLI-Argument-Parser

### Schritt 1 — CLI-Flag

In der Kommandozeilen-Verarbeitung einen neuen Parameter `--debug-script <datei>` registrieren und den Dateinamen in der globalen Umgebungsstruktur speichern:

```c
// In der Umgebungsstruktur:
char* debug_script_filename;  // NULL wenn kein Skript angegeben

// Im CLI-Parser:
// "-DEBUG_SCRIPT" → liest nächstes Argument als Dateiname
g_env.debug_script_filename = strdup(argv[i]);
```

### Schritt 2 — Befehlswarteschlange in der Debugger-Struktur

```cpp
// In der Debugger-Hauptstruktur (t_debugger o.ä.):
std::vector<std::string>  script_commands;
size_t                    script_command_index;  // = 0 initialisieren
```

### Schritt 3 — Skript einlesen

```cpp
void Debugger_Script_Load(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f) { /* Fehlermeldung */ return; }

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        // Zeilenenden entfernen
        for (char* p = line + strlen(line) - 1;
             p >= line && (*p == '\n' || *p == '\r'); p--)
            *p = '\0';

        // Führende Leerzeichen überspringen
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        // Leerzeilen und Kommentare ignorieren
        if (*p == '#' || *p == '\0') continue;

        Debugger.script_commands.push_back(p);
    }
    fclose(f);
}
```

### Schritt 4 — Befehle ausführen (in der Update-Schleife)

In der Funktion, die pro Frame bei pausierter Maschine aufgerufen wird:

```cpp
void Debugger_Update()
{
    // ... bestehender Code ...

    // Nächsten Skript-Befehl ausführen wenn Maschine an Breakpoint hält
    if (Debugger.script_command_index < Debugger.script_commands.size()
        && (machine_flags & MACHINE_DEBUGGING))
    {
        const std::string& cmd = Debugger.script_commands[Debugger.script_command_index++];
        char buf[512];
        strncpy(buf, cmd.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        log_printf("$ %s\n", buf);
        Debugger_InputParseCommand(buf);  // ← bestehende Befehlsverarbeitung
    }
}
```

**Warum ein Befehl pro Frame?**  
`C` (Continue) setzt `MACHINE_DEBUGGING` zurück und lässt die Maschine laufen. Beim nächsten Breakpoint ist `MACHINE_DEBUGGING` wieder gesetzt und der nächste Befehl wird ausgeführt. Die Warteschlange hält automatisch an, bis der nächste Halt kommt.

### Schritt 5 — QUIT-Befehl

In der bestehenden Befehlsverarbeitung ergänzen:

```cpp
if (!strcmp(cmd, "QUIT") || !strcmp(cmd, "EXIT") || !strcmp(cmd, "Q"))
{
    force_quit = true;
    return;
}
```

### Schritt 6 — Aktivierung beim Start

Nach dem Laden des ROMs / Spiels:

```cpp
if (env.debug_script_filename)
{
    // Debugger aktivieren (installiert Breakpoint-Hooks, öffnet GUI)
    if (!debugger.active)
        Debugger_Switch();  // oder equivalent

    // Skript laden → füllt die Warteschlange
    Debugger_Script_Load(env.debug_script_filename);

    // GUI-Modus erzwingen damit Update() aufgerufen wird
    force_gui_mode = true;
}
```

---

## Ablauf-Diagramm

```
Start mit -DEBUG_SCRIPT
        │
        ▼
ROM laden + Debugger aktivieren
        │
        ▼
Debugger_Switch() → Breakpoint-Hooks installiert
        │
        ▼
Debugger_Script_Load() → Befehle in Warteschlange
        │
        ▼
Main Loop
        │
  ┌─────┴──────┐
  │  Maschine  │ ← läuft normal
  │  pausiert? │
  └─────┬──────┘
   ja   │
        ▼
Debugger_Update() → nächsten Befehl ausführen
        │
  ┌─────┴────────────────────────┐
  │ B addr → Breakpoint setzen   │
  │ C      → Maschine weiter     │─────→ zurück zu "läuft normal"
  │ M addr → Memory-Dump ins Log │
  │ P expr → Ausdruck auswerten  │
  │ QUIT   → Emulator beenden    │
  └──────────────────────────────┘
```

---

## Ausgabe

Alle `Debugger_Printf()`-Ausgaben landen in `Debug/debuglog.txt`. Das Skript kann damit vollständig headless (mit geschlossenem Fenster nach Ausführung) ausgewertet werden — einfach die Logdatei nach dem Lauf lesen.
