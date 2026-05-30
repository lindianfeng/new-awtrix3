#include "awtrix_dictionary.h"
#include <string.h>

/* ── Common locales (en/de/fr/es/it/zh/ja) ─────────────────────── */
static const char* DAY_SHORT_EN[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char* DAY_SHORT_DE[7] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
static const char* DAY_SHORT_FR[7] = {"Lun", "Mar", "Mer", "Jeu", "Ven", "Sam", "Dim"};
static const char* DAY_SHORT_ES[7] = {"Lun", "Mar", "Mie", "Jue", "Vie", "Sab", "Dom"};
static const char* DAY_SHORT_IT[7] = {"Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom"};
static const char* DAY_SHORT_ZH[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"}; /* glyphs not in 5x3 font */
static const char* DAY_SHORT_JA[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

static const char* DAY_LONG_EN[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char* DAY_LONG_DE[7] = {"Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"};

static const char* MON_SHORT_EN[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char* MON_SHORT_DE[12] = {
    "Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"
};
static const char* MON_SHORT_FR[12] = {
    "Jan", "Fev", "Mar", "Avr", "Mai", "Jun", "Jul", "Aou", "Sep", "Oct", "Nov", "Dec"
};
static const char* MON_SHORT_ES[12] = {
    "Ene", "Feb", "Mar", "Abr", "May", "Jun", "Jul", "Ago", "Sep", "Oct", "Nov", "Dic"
};
static const char* MON_SHORT_IT[12] = {
    "Gen", "Feb", "Mar", "Apr", "Mag", "Giu", "Lug", "Ago", "Set", "Ott", "Nov", "Dic"
};

static int is_lang(const char* lang, const char* code)
{
    return lang && (strncasecmp(lang, code, 2) == 0);
}

const char* dict_day_short(const char* lang, int idx)
{
    if (idx < 0 || idx > 6) return "?";
    if (is_lang(lang, "de")) return DAY_SHORT_DE[idx];
    if (is_lang(lang, "fr")) return DAY_SHORT_FR[idx];
    if (is_lang(lang, "es")) return DAY_SHORT_ES[idx];
    if (is_lang(lang, "it")) return DAY_SHORT_IT[idx];
    if (is_lang(lang, "zh")) return DAY_SHORT_ZH[idx];
    if (is_lang(lang, "ja")) return DAY_SHORT_JA[idx];
    return DAY_SHORT_EN[idx];
}

const char* dict_day_long(const char* lang, int idx)
{
    if (idx < 0 || idx > 6) return "?";
    if (is_lang(lang, "de")) return DAY_LONG_DE[idx];
    return DAY_LONG_EN[idx];
}

const char* dict_month_short(const char* lang, int idx)
{
    if (idx < 0 || idx > 11) return "?";
    if (is_lang(lang, "de")) return MON_SHORT_DE[idx];
    if (is_lang(lang, "fr")) return MON_SHORT_FR[idx];
    if (is_lang(lang, "es")) return MON_SHORT_ES[idx];
    if (is_lang(lang, "it")) return MON_SHORT_IT[idx];
    return MON_SHORT_EN[idx];
}
