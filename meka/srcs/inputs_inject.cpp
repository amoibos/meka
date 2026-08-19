//-----------------------------------------------------------------------------
// MEKA - inputs_inject.cpp
// Thread-safe keyboard injection for DAP / automation
//-----------------------------------------------------------------------------

#include "shared.h"
#include "inputs_inject.h"
#include "inputs_u.h"
#include "keyinfo.h"

#include <mutex>
#include <vector>
#include <cstring>
#include <cctype>

//-----------------------------------------------------------------------------
// Pending injection queue (DAP thread -> main thread)
//-----------------------------------------------------------------------------

static std::mutex                       g_inject_mutex;
static std::vector<std::pair<int, bool>> g_inject_immediate;
static std::vector<std::pair<int, double>> g_inject_releases;

//-----------------------------------------------------------------------------
// Key name resolution
//-----------------------------------------------------------------------------

static const struct { const char* alias; const char* name; } KeyAliases[] =
{
    { "SPACE",      "Space"     },
    { "ENTER",      "Enter"     },
    { "RETURN",     "Enter"     },
    { "ESC",        "Escape"    },
    { "ESCAPE",     "Escape"    },
    { "UP",         "Up"        },
    { "DOWN",       "Down"      },
    { "LEFT",       "Left"      },
    { "RIGHT",      "Right"     },
    { "BS",         "Backspace" },
    { "BACKSPACE",  "Backspace" },
    { "TAB",        "Tab"       },
    { "DEL",        "Delete"    },
    { "DELETE",     "Delete"    },
    { "HOME",       "Home"      },
    { "END",        "End"       },
    { "PGUP",       "Page Up"   },
    { "PGDN",       "Page Down" },
    { "INSERT",     "Insert"    },
    { nullptr,      nullptr     },
};

int Inputs_InjectResolveKey(const char* key)
{
    if (!key || !key[0])
        return -1;

    // Numeric scancode
    if (key[0] >= '0' && key[0] <= '9')
    {
        char* end = nullptr;
        long val = strtol(key, &end, 0);
        if (end && *end == '\0' && val >= 0 && val < ALLEGRO_KEY_MAX)
            return (int)val;
    }

    // Alias table (case-insensitive)
    for (int i = 0; KeyAliases[i].alias; i++)
    {
        if (stricmp(key, KeyAliases[i].alias) == 0)
        {
            const t_key_info* ki = KeyInfo_FindByName(KeyAliases[i].name);
            return ki ? ki->scancode : -1;
        }
    }

    // Single printable character -> "A".."Z"
    if (key[1] == '\0' && isprint((unsigned char)key[0]))
    {
        char upper[2] = { (char)toupper((unsigned char)key[0]), '\0' };
        const t_key_info* ki = KeyInfo_FindByName(upper);
        if (ki)
            return ki->scancode;
    }

    // Direct MEKA key name ("Space", "Left", ...)
    const t_key_info* ki = KeyInfo_FindByName(key);
    if (ki)
        return ki->scancode;

    return -1;
}

void Inputs_InjectKey(int scancode, bool down)
{
    if (scancode < 0 || scancode >= ALLEGRO_KEY_MAX)
        return;
    std::lock_guard<std::mutex> lock(g_inject_mutex);
    g_inject_immediate.push_back({ scancode, down });
}

void Inputs_InjectKeyPress(int scancode, int duration_ms)
{
    if (scancode < 0 || scancode >= ALLEGRO_KEY_MAX)
        return;
    if (duration_ms < 1)
        duration_ms = 1;

    Inputs_InjectKey(scancode, true);

    std::lock_guard<std::mutex> lock(g_inject_mutex);
    const double release_at = al_get_time() + duration_ms / 1000.0;
    g_inject_releases.push_back({ scancode, release_at });
}

void Inputs_InjectProcess()
{
    std::vector<std::pair<int, bool>> immediate;

    {
        std::lock_guard<std::mutex> lock(g_inject_mutex);
        immediate.swap(g_inject_immediate);

        const double now = al_get_time();
        for (size_t i = 0; i < g_inject_releases.size(); )
        {
            if (now >= g_inject_releases[i].second)
            {
                immediate.push_back({ g_inject_releases[i].first, false });
                g_inject_releases.erase(g_inject_releases.begin() + i);
            }
            else
                i++;
        }
    }

    for (const auto& ev : immediate)
    {
        if (ev.second)
        {
            if (g_keyboard_state[ev.first] < 0.0f)
                g_keyboard_state[ev.first] = 0.0f;
        }
        else
        {
            g_keyboard_state[ev.first] = -1.0f;
        }
    }
}

//-----------------------------------------------------------------------------
