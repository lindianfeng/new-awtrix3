/**
 * Ports of src/Games (AwtrixSays.cpp + SlotMachine.cpp + GameManager.cpp).
 * Original AwtrixSays was a Simon-style memory game; SlotMachine spun three
 * reels of icons. These are kept playable but visually simpler than the
 * originals to fit the same 32x8 display without the AwtrixFont icon set.
 */
#include "awtrix_games.h"
#include "awtrix_globals.h"
#include "DisplayManager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdlib>
#include <cstring>

#define TAG "game"

static int s_game = 0; /* 0=none, 1=AwtrixSays, 2=Slot */
static uint64_t s_t0 = 0;

/* — AwtrixSays state — */
static int s_seq[16];
static int s_seqLen = 0;
static int s_showIdx = 0;
static int s_inputIdx = 0;
static uint64_t s_lastFlash = 0;

/* — SlotMachine state — */
static int s_reels[3] = {0, 0, 0};
static int s_reelStop[3] = {0, 0, 0};

static uint64_t _now() { return esp_timer_get_time() / 1000; }

void awtrix_game_awtrix_says_start(void)
{
    s_game = 1;
    s_seqLen = 1;
    s_seq[0] = std::rand() % 4;
    s_showIdx = 0;
    s_inputIdx = 0;
    s_lastFlash = 0;
    s_t0 = _now();
    CONFIG.gameActive = true;
    DisplayManager::get().setGameActive(true);
    ESP_LOGI(TAG, "AwtrixSays started");
}

void awtrix_game_slot_machine_start(void)
{
    s_game = 2;
    for (int i = 0; i < 3; i++)
    {
        s_reels[i] = std::rand() % 10;
        s_reelStop[i] = 0;
    }
    s_t0 = _now();
    CONFIG.gameActive = true;
    DisplayManager::get().setGameActive(true);
    ESP_LOGI(TAG, "SlotMachine started");
}

static void exit_game()
{
    s_game = 0;
    CONFIG.gameActive = false;
    DisplayManager::get().setGameActive(false);
    auto* m = DisplayManager::get().getMatrix();
    if (m)
    {
        m->clear();
        m->show();
    }
}

static void tick_says()
{
    auto* m = DisplayManager::get().getMatrix();
    if (!m) return;
    m->clear();
    /* show flash sequence then wait for player input via buttons */
    uint64_t now = _now();
    if (s_showIdx < s_seqLen)
    {
        if (now - s_lastFlash >= 600)
        {
            s_lastFlash = now;
            s_showIdx++;
        }
        int curIdx = (s_showIdx > 0 ? s_showIdx - 1 : 0);
        uint32_t colors[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};
        m->fillRect(s_seq[curIdx] * 8, 0, 8, 8, colors[s_seq[curIdx]]);
    }
    else
    {
        m->setCursor(2, 1);
        m->setTextColor(0xFFFFFF);
        m->print("PLAY!");
    }
    m->show();
    /* auto-time-out after 30s */
    if (now - s_t0 > 30000) exit_game();
}

static void tick_slot()
{
    auto* m = DisplayManager::get().getMatrix();
    if (!m) return;
    m->clear();
    uint64_t now = _now();
    /* spin */
    for (int i = 0; i < 3; i++)
    {
        if (!s_reelStop[i] && (now - s_t0) > (uint64_t)(800 + i * 600))
            s_reelStop[i] = 1;
        if (!s_reelStop[i]) s_reels[i] = std::rand() % 10;
    }
    /* render three digit-like glyphs */
    for (int i = 0; i < 3; i++)
    {
        char ch[2] = {(char)('0' + s_reels[i]), 0};
        m->setCursor(4 + i * 9, 1);
        m->setTextColor(0xFFAA00);
        m->print(ch);
    }
    m->show();
    if (s_reelStop[0] && s_reelStop[1] && s_reelStop[2] && (now - s_t0) > 3500)
        exit_game();
}

void awtrix_game_tick(void)
{
    if (s_game == 1) tick_says();
    else if (s_game == 2) tick_slot();
}

bool awtrix_game_active(void) { return s_game != 0; }

/* Route a single newline-terminated controller line received over TCP:8080
 * to the active game. Mirrors the original GameManager::ControllerInput
 * which accepted single-character commands like "L"/"R"/"S"/"X" plus
 * numeric digits for AwtrixSays. */
void awtrix_game_controller_input(const char* line)
{
    if (!line || !*line || s_game == 0) return;
    ESP_LOGI(TAG, "controller: %s", line);

    /* Universal exit. */
    if (line[0] == 'X' || strncmp(line, "EXIT", 4) == 0)
    {
        exit_game();
        return;
    }

    if (s_game == 1)
    {
        /* AwtrixSays: digit 0-3 is a colored-pad press; advance s_inputIdx. */
        if (line[0] >= '0' && line[0] <= '3')
        {
            int p = line[0] - '0';
            if (s_inputIdx < s_seqLen)
            {
                if (p == s_seq[s_inputIdx])
                {
                    s_inputIdx++;
                    if (s_inputIdx == s_seqLen)
                    {
                        /* round complete → grow sequence */
                        if (s_seqLen < (int)(sizeof(s_seq) / sizeof(s_seq[0])))
                        {
                            s_seq[s_seqLen++] = std::rand() % 4;
                        }
                        s_showIdx = 0;
                        s_inputIdx = 0;
                        s_t0 = _now();
                    }
                }
                else
                {
                    /* miss → game over */
                    exit_game();
                }
            }
        }
    }
    else if (s_game == 2)
    {
        /* SlotMachine: "S" or "0"/"1"/"2" stops the next / chosen reel. */
        if (line[0] == 'S')
        {
            for (int i = 0; i < 3; i++)
            {
                if (!s_reelStop[i])
                {
                    s_reelStop[i] = 1;
                    break;
                }
            }
        }
        else if (line[0] >= '0' && line[0] <= '2')
        {
            s_reelStop[line[0] - '0'] = 1;
        }
    }
}
