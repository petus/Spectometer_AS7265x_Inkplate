/*
Jednoduchý spektrální analyzátor osvětlení postavený na senzoru AS7265x a e-paper displeji Inkplate 5 V2.
Zařízení měří světlo v rozsahu 410–940 nm, analyzuje jeho spektrální složení a zobrazuje výsledky přehledně na displeji i v Serial monitoru.

Celý projekt najdete na https://chiptron.cz/projekt-vicekanaloveho-spektralniho-analyzatoru-verejneho-osvetleni/
* 2026 chiptron.cz

Knihovny: InkplateLibrary 11.x (testováno s 11.2.0), SparkFun AS7265x
Deska: Soldered Inkplate 5 V2
*/

#include <Inkplate.h>
#include <Wire.h>
#include "SparkFun_AS7265X.h"

#include "SpectralMath.h"
#include "OpenSansSB_20px.h"
#include "OpenSansSB_24px.h"
#include "OpenSansSB_40px.h"

struct Rect { int x, y, w, h; };  // oblast displeje (nahoře kvůli prototypům, které generuje Arduino IDE)

// =====================
// Nastavení
// =====================
const uint32_t MEASURE_INTERVAL_MS = 10000;  // perioda měření
const uint8_t  SAMPLES_PER_MEASUREMENT = 3;  // průměrování (po ustálení expozice)
const uint8_t  FULL_REFRESH_EVERY = 20;      // plný refresh e-papíru po N parciálních
const uint8_t  CLEAR_PASSES = 2;             // kolikrát před parciálním refreshem smazat měnící se oblasti (víc = méně duchů, déle)
const uint32_t I2C_CLOCK_HZ = 400000;        // AS7265x i Inkplate zvládají fast mode

// Logování na microSD (CSV). 0 = vypnuto.
#define ENABLE_SD_LOG 1
const char *LOG_FILE = "spektrum.csv";

// Kalibrované hodnoty AS7265x platí pro GAIN 16x a INT_T 166 ms (datasheet)
// a se změnou gainu/integrace se mění. Pro srovnatelnost mezi expozicemi je
// přepočítáváme na tyto referenční podmínky.
const float CAL_REF_GAIN = 16.0f;
const float CAL_REF_INT_MS = 166.0f;
const float INT_CYCLE_MS = 2.78f;

// Odhad osvětlenosti: Ev [lx] = 683 lm/W * Y [uW/cm2] * 0.01 * LUX_CAL.
// Předpokládá, že kalibrované hodnoty jsou ozáření v pásmu kanálu v uW/cm2
// (tolerance výrobce ±12 %) a že senzor míří kolmo na zdroj (deska nemá
// kosinový difuzor). LUX_CAL dorovnejte podle luxmetru.
const float LUX_CAL = 1.0f;

// Korekce kanálů pro KONKRÉTNÍ kus senzoru (násobí kalibrované hodnoty).
// Tento kus: čip AS72651 (610, 680, 730, 760, 810, 860 nm) měří ~2,7× víc než
// ostatní dva čipy. Ověřeno při 16x/56 ms i 64x/556 ms, je to tedy kalibrace
// z výroby, ne expozice. Hodnota 0,371 = 1 / průměr poměru 610 nm k sousedům
// 585 a 645 nm ze 4 měření (studená bílá LED + denní světlo, 9. 10. 2026).
// Měřeno BEZ difuzoru. Po přidání difuzoru je potřeba korekci změřit znovu.
// Pro jiný kus senzoru nastavte všude 1.0f a změřte znovu, postup je v README (Kalibrace doma).
const float K_AS72651 = 0.371f;
const float CHANNEL_CORRECTION[spectral::CHANNELS] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,             // 410–535 nm (AS72653)
    1.0f, 1.0f, K_AS72651, 1.0f, K_AS72651, 1.0f,   // 560 585 610 645 680 705
    K_AS72651, K_AS72651, K_AS72651, K_AS72651,     // 730 760 810 860
    1.0f, 1.0f                                      // 900 940 (AS72652)
};

// =====================
// Inkplate 5 Gen2
// =====================
Inkplate display(INKPLATE_1BIT);
AS7265X sensor;

// =====================
// Expozice (gain × integrační čas)
// =====================
struct ExposureStep
{
    uint8_t gainCode;
    float gain;
    uint8_t cycles;  // integrační čas = cycles * 2.78 ms
};

const ExposureStep EXPOSURE[] = {
    {AS7265X_GAIN_1X,  1.0f,  5},    // velmi silné světlo (přímé slunce, svítilna zblízka)
    {AS7265X_GAIN_1X,  1.0f,  20},
    {AS7265X_GAIN_37X, 3.7f,  20},
    {AS7265X_GAIN_16X, 16.0f, 20},
    {AS7265X_GAIN_64X, 64.0f, 20},
    {AS7265X_GAIN_64X, 64.0f, 50},
    {AS7265X_GAIN_64X, 64.0f, 100},
    {AS7265X_GAIN_64X, 64.0f, 200},  // pro slabé veřejné osvětlení
};
const int EXPOSURE_STEPS = sizeof(EXPOSURE) / sizeof(EXPOSURE[0]);

// Hranice vztažené k plnému rozsahu ADC (ten závisí na integračním čase)
const float SAT_FRACTION    = 0.95f;  // nad tím je kanál saturovaný (ADC na maximu)
const float TARGET_FRACTION = 0.50f;  // kam míříme s nejsilnějším kanálem

// Pojistka proti přesvícení: po zvýšení expozice musí kanály vzrůst úměrně.
// Když některý vzroste výrazně méně (nebo klesne), expozice se vrátí zpět.
// Nezávisí tak na tom, jakou hodnotu senzor v saturaci skutečně vrací.
const uint16_t VERIFY_MIN_COUNTS = 200;  // slabší kanály se neověřují (šum)
const float    VERIFY_MIN_RATIO  = 0.6f; // naměřeno / očekáváno; méně = přesvícený kanál
const uint16_t MIN_SIGNAL_COUNTS = 100;  // méně = měření je jen šum

int exposureIndex = 0;

// Pevná expozice pro diagnostiku: -1 = automatika, 0..7 = index v EXPOSURE[].
// 3 = 16x / 56 ms, stejně jako původní firmware.
const int FIXED_EXPOSURE_STEP = -1;

// =====================
// Stav měření
// =====================
using spectral::CHANNELS;

struct Measurement
{
    bool ok;               // senzor odpověděl
    bool saturated;        // aspoň jeden kanál v saturaci
    bool lowSignal;        // i při max. expozici příliš málo světla
    uint16_t maxRaw;
    uint16_t raw[CHANNELS];    // surové hodnoty z posledního vzorku (diagnostika)
    float spectrum[CHANNELS];  // normalizované na 16x / 166 ms, ~uW/cm2
    spectral::Metrics m;
    float illuminanceLux;
    float sensorTempC;
    float batteryV;
};

Measurement meas;
bool sensorOk = false;
bool sdOk = false;
uint32_t lastMeasure = 0;
uint32_t updateCount = 0;

// =====================
// Pomocné funkce expozice
// =====================
uint32_t fullScale(const ExposureStep &e)
{
    uint32_t fs = (uint32_t)e.cycles * 1024UL;  // datasheet: +1024 counts na cyklus
    return fs > 65535UL ? 65535UL : fs;
}

float exposureFactor(const ExposureStep &e)
{
    return e.gain * e.cycles;
}

float integrationMs(const ExposureStep &e)
{
    return e.cycles * INT_CYCLE_MS;
}

void applyExposure(int idx)
{
    exposureIndex = idx;
    sensor.setGain(EXPOSURE[idx].gainCode);
    sensor.setIntegrationCycles(EXPOSURE[idx].cycles);
}

// Nejvyšší expozice, při které by maximum vyšlo pod cílem
int bestExposureFor(uint16_t maxRaw, int current)
{
    if (maxRaw < 1) return EXPOSURE_STEPS - 1;
    int best = 0;
    for (int i = 0; i < EXPOSURE_STEPS; i++)
    {
        float predicted = maxRaw * exposureFactor(EXPOSURE[i]) / exposureFactor(EXPOSURE[current]);
        if (predicted <= TARGET_FRACTION * fullScale(EXPOSURE[i])) best = i;
    }
    return best;
}

// =====================
// Čtení senzoru
// =====================

// Jedno měření ve 6kanálovém one-shot režimu. Vlastní čekání místo
// sensor.takeMeasurements(): knihovna čeká jen 1,5× integrační čas, ale
// režim 3 potřebuje dva integrační cykly, takže by vypršela a vrátila stará data.
bool acquire(uint16_t raw[CHANNELS], float cal[CHANNELS])
{
    sensor.setMeasurementMode(AS7265X_MEASUREMENT_MODE_6CHAN_ONE_SHOT);

    const uint32_t timeoutMs = (uint32_t)(2.0f * integrationMs(EXPOSURE[exposureIndex]) * 1.5f) + 500;
    const uint32_t start = millis();
    while (!sensor.dataAvailable())
    {
        if (millis() - start > timeoutMs) return false;
        delay(5);
    }

    // Pořadí odpovídá spectral::WAVELENGTHS
    // Čipy se v pořadí vlnových délek prokládají (SparkFun getX()):
    //   AS72653: A B C D E F = 410 435 460 485 510 535 nm
    //   AS72652: G H I J K L = 560 585 645 705 900 940 nm
    //   AS72651: R S T U V W = 610 680 730 760 810 860 nm
    // 410–535 nm
    raw[0]  = sensor.getA();  cal[0]  = sensor.getCalibratedA();
    raw[1]  = sensor.getB();  cal[1]  = sensor.getCalibratedB();
    raw[2]  = sensor.getC();  cal[2]  = sensor.getCalibratedC();
    raw[3]  = sensor.getD();  cal[3]  = sensor.getCalibratedD();
    raw[4]  = sensor.getE();  cal[4]  = sensor.getCalibratedE();
    raw[5]  = sensor.getF();  cal[5]  = sensor.getCalibratedF();
    // 560–705 nm
    raw[6]  = sensor.getG();  cal[6]  = sensor.getCalibratedG();
    raw[7]  = sensor.getH();  cal[7]  = sensor.getCalibratedH();
    raw[8]  = sensor.getR();  cal[8]  = sensor.getCalibratedR();
    raw[9]  = sensor.getI();  cal[9]  = sensor.getCalibratedI();
    raw[10] = sensor.getS();  cal[10] = sensor.getCalibratedS();
    raw[11] = sensor.getJ();  cal[11] = sensor.getCalibratedJ();
    // 730–940 nm
    raw[12] = sensor.getT();  cal[12] = sensor.getCalibratedT();
    raw[13] = sensor.getU();  cal[13] = sensor.getCalibratedU();
    raw[14] = sensor.getV();  cal[14] = sensor.getCalibratedV();
    raw[15] = sensor.getW();  cal[15] = sensor.getCalibratedW();
    raw[16] = sensor.getK();  cal[16] = sensor.getCalibratedK();
    raw[17] = sensor.getL();  cal[17] = sensor.getCalibratedL();
    return true;
}

uint16_t maxOf(const uint16_t *v)
{
    uint16_t m = 0;
    for (int i = 0; i < CHANNELS; i++)
        if (v[i] > m) m = v[i];
    return m;
}

// Ověří, že po zvýšení expozice z prevIdx na curIdx vzrostly kanály úměrně.
// Vrací index přesvíceného kanálu, nebo -1, když je vše v pořádku.
int overexposedChannel(const uint16_t *prev, int prevIdx, const uint16_t *cur, int curIdx)
{
    const float k = exposureFactor(EXPOSURE[curIdx]) / exposureFactor(EXPOSURE[prevIdx]);
    const float fs = fullScale(EXPOSURE[curIdx]);
    for (int i = 0; i < CHANNELS; i++)
    {
        if (cur[i] >= SAT_FRACTION * fs) return i;  // ADC na maximu
        if (prev[i] >= VERIFY_MIN_COUNTS && cur[i] < VERIFY_MIN_RATIO * prev[i] * k) return i;
    }
    return -1;
}

// Automatická expozice podle RAW hodnot nejsilnějšího kanálu.
// Začíná vždy od nejnižší citlivosti (tam je přesvícení nejméně pravděpodobné) a citlivost
// zvyšuje jen tehdy, když nové měření odpovídá předchozímu přepočtenému
// na vyšší expozici. Jinak se vrátí o krok zpět.
bool autoExpose()
{
    if (FIXED_EXPOSURE_STEP >= 0 && FIXED_EXPOSURE_STEP < EXPOSURE_STEPS)
    {
        if (exposureIndex != FIXED_EXPOSURE_STEP) applyExposure(FIXED_EXPOSURE_STEP);
        return true;
    }

    uint16_t prev[CHANNELS], cur[CHANNELS];
    float cal[CHANNELS];

    int idx = 0;
    applyExposure(idx);
    if (!acquire(prev, cal)) return false;

    for (int iter = 0; iter < EXPOSURE_STEPS; iter++)
    {
        const int next = bestExposureFor(maxOf(prev), idx);
        if (next <= idx) break;

        applyExposure(next);
        if (!acquire(cur, cal)) return false;

        const int bad = overexposedChannel(prev, idx, cur, next);
        if (bad >= 0)
        {
            Serial.printf("Expozice %.1fx/%u cyklu: kanal %d nm presviceny (%u, ocekavano ~%.0f), zustavam na %.1fx/%u\n",
                          EXPOSURE[next].gain, EXPOSURE[next].cycles, spectral::WAVELENGTHS[bad], cur[bad],
                          prev[bad] * exposureFactor(EXPOSURE[next]) / exposureFactor(EXPOSURE[idx]),
                          EXPOSURE[idx].gain, EXPOSURE[idx].cycles);
            applyExposure(idx);
            break;
        }
        idx = next;
        memcpy(prev, cur, sizeof(prev));
    }
    return true;
}

bool measure(Measurement &out)
{
    out.ok = false;
    out.saturated = false;
    out.lowSignal = false;
    out.maxRaw = 0;

    if (!autoExpose()) return false;

    const ExposureStep &e = EXPOSURE[exposureIndex];
    const float fs = fullScale(e);
    const float norm = (CAL_REF_GAIN / e.gain) * (CAL_REF_INT_MS / integrationMs(e));

    float sum[CHANNELS] = {0};
    uint16_t raw[CHANNELS];
    float cal[CHANNELS];

    for (int n = 0; n < SAMPLES_PER_MEASUREMENT; n++)
    {
        if (!acquire(raw, cal)) return false;
        const uint16_t mx = maxOf(raw);
        if (mx > out.maxRaw) out.maxRaw = mx;
        if (mx >= SAT_FRACTION * fs) out.saturated = true;
        for (int i = 0; i < CHANNELS; i++) sum[i] += cal[i];
    }
    memcpy(out.raw, raw, sizeof(out.raw));

    for (int i = 0; i < CHANNELS; i++)
    {
        float v = sum[i] / SAMPLES_PER_MEASUREMENT * norm * CHANNEL_CORRECTION[i];
        out.spectrum[i] = v > 0 ? v : 0;  // ve tmě vrací senzor i mírně záporná čísla
    }

    out.lowSignal = (exposureIndex == EXPOSURE_STEPS - 1) && (out.maxRaw < MIN_SIGNAL_COUNTS);
    out.m = spectral::compute(out.spectrum);
    out.illuminanceLux = 683.0f * out.m.Y * 0.01f * LUX_CAL;
    out.sensorTempC = sensor.getTemperatureAverage();
    out.batteryV = display.readBattery();
    out.ok = true;
    return true;
}

// =====================
// Serial
// =====================
void printSerial(const Measurement &r)
{
    const ExposureStep &e = EXPOSURE[exposureIndex];
    Serial.println("---- Measurement ----");
    if (!r.ok)
    {
        Serial.println("Chyba: senzor neodpovedel");
        return;
    }
    Serial.printf("Expozice: gain %.1fx, integrace %.0f ms, max raw %u / %lu%s%s\n",
                  e.gain, integrationMs(e), r.maxRaw, (unsigned long)fullScale(e),
                  r.saturated ? "  SATURACE" : "", r.lowSignal ? "  MALO SVETLA" : "");
    Serial.printf("Blue %%: %.2f\n", r.m.blueRatio * 100);
    Serial.printf("Cold index: %.3f\n", r.m.coldIndex);
    if (r.m.cctValid)
        Serial.printf("CCT: %.0f K (Duv %+.4f)\n", r.m.cct, r.m.duv);
    else
        Serial.printf("CCT: --- (x=%.4f y=%.4f, Duv %+.4f, mimo rozsah CCT)\n", r.m.x, r.m.y, r.m.duv);
    Serial.printf("Ev (odhad): %.1f lx\n", r.illuminanceLux);
    Serial.printf("NIR %%: %.1f\n", r.m.nirRatio * 100);
    Serial.printf("Teplota senzoru: %.1f C, baterie %.2f V\n", r.sensorTempC, r.batteryV);
    Serial.print("Raw (posledni vzorek):");
    for (int i = 0; i < CHANNELS; i++)
    {
        Serial.print(' ');
        Serial.print(r.raw[i]);
    }
    Serial.println();
    Serial.print("Spektrum [uW/cm2 @16x/166ms]:");
    for (int i = 0; i < CHANNELS; i++)
    {
        Serial.print(' ');
        Serial.print(r.spectrum[i], 2);
    }
    Serial.println();
    Serial.println("---------------------");
}

// =====================
// microSD log
// =====================
#if ENABLE_SD_LOG
SdFile logFile;
const uint8_t SD_RETRY_EVERY = 6;  // bez karty zkusit znovu každých N měření (~1 min)
uint8_t sdRetryCounter = 0;

bool initSd()
{
    sdOk = display.sdCardInit();
    if (!sdOk) display.sdCardSleep();  // vypnout napájení slotu, když tam karta není
    return sdOk;
}

void logToSd(const Measurement &r)
{
    if (!sdOk)
    {
        // karta mohla být vložena až za běhu
        if (++sdRetryCounter < SD_RETRY_EVERY) return;
        sdRetryCounter = 0;
        if (!initSd()) return;
        Serial.println("SD karta nalezena, loguji do spektrum.csv");
    }
    if (!r.ok) return;

    const bool exists = display.getSdFat().exists(LOG_FILE);
    if (!logFile.open(LOG_FILE, FILE_WRITE))
    {
        // karta vytažena nebo plná: přestat hlásit "SD log: ano" a zkusit později znovu
        Serial.println("SD: nelze otevrit soubor, logovani pozastaveno");
        sdOk = false;
        display.sdCardSleep();
        return;
    }
    if (!exists)
    {
        logFile.print("time,rtc_valid,gain,int_ms,max_raw,saturated,low_signal,temp_c,battery_v,"
                      "blue_pct,cold_index,cct_k,duv,cct_valid,ev_lx,nir_pct");
        for (int i = 0; i < CHANNELS; i++)
        {
            logFile.print(",nm");
            logFile.print(spectral::WAVELENGTHS[i]);
        }
        logFile.println();
    }

    const bool rtcValid = display.rtc.isSet();
    const uint32_t t = rtcValid ? display.rtc.getEpoch() : millis() / 1000;
    const ExposureStep &e = EXPOSURE[exposureIndex];

    char line[256];
    snprintf(line, sizeof(line), "%lu,%d,%.1f,%.0f,%u,%d,%d,%.1f,%.2f,%.2f,%.4f,%.0f,%.4f,%d,%.2f,%.2f",
             (unsigned long)t, rtcValid, e.gain, integrationMs(e), r.maxRaw, r.saturated, r.lowSignal,
             r.sensorTempC, r.batteryV, r.m.blueRatio * 100, r.m.coldIndex, r.m.cct, r.m.duv,
             r.m.cctValid, r.illuminanceLux, r.m.nirRatio * 100);
    logFile.print(line);
    for (int i = 0; i < CHANNELS; i++)
    {
        logFile.print(',');
        logFile.print(r.spectrum[i], 3);
    }
    logFile.println();
    logFile.close();
}
#endif

// =====================
// Kreslení
// =====================
const int SCREEN_W = 1280;  // Inkplate 5 V2, landscape
const int SCREEN_H = 720;

// Geometrie grafu (sdílí ji kreslení i mazání před parciálním refreshem)
const int GRAPH_X = 20, GRAPH_Y = 45;
const int GRAPH_W = 1000, GRAPH_H = 600;
const int BAR_TOP = GRAPH_Y + 20;
const int BAR_BOTTOM = GRAPH_Y + 495;            // osa X
const int VALUE_BASELINE = BAR_BOTTOM + 62;      // řádek s hodnotami pod sloupci
const int VALUE_ROW_TOP = VALUE_BASELINE - 20;   // font 20 px: horní a dolní okraj s rezervou
const int VALUE_ROW_BOTTOM = VALUE_BASELINE + 7;

// Pravý panel
const int PANEL_X = 1040;
const int PANEL_Y0 = 70;          // účaří prvního popisku
const int PANEL_STEP = 92;        // rozestup položek
const int PANEL_VALUE_DY = 44;    // účaří hodnoty pod popiskem
const int PANEL_ITEMS = 6;
const int STATUS_Y0 = 628;        // stavový blok, 3 řádky
const int STATUS_DY = 28;

void printCentered(const char *text, int cx, int baselineY)
{
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, baselineY, &x1, &y1, &w, &h);
    display.setCursor(cx - (int)w / 2 - x1, baselineY);
    display.print(text);
}

// Krátký zápis hodnoty kanálu, aby se vešel pod 55 px široký sloupec
void formatChannelValue(float v, char *buf, size_t len)
{
    if (v >= 10000)     snprintf(buf, len, "%.0fk", v / 1000.0f);
    else if (v >= 1000) snprintf(buf, len, "%.1fk", v / 1000.0f);
    else if (v >= 10)   snprintf(buf, len, "%.0f", v);
    else                snprintf(buf, len, "%.1f", v);
}

// Šrafovaný obdélník (diagonální pruhy 2 px každých 6 px) s obrysem
void fillHatched(int x, int y, int w, int h)
{
    display.drawRect(x, y, w, h, BLACK);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if ((c + r) % 6 < 2) display.drawPixel(x + c, y + r, BLACK);
}

void drawSpectrum(const Measurement &r)
{
    const int graphX = GRAPH_X, graphY = GRAPH_Y;
    const int graphW = GRAPH_W, graphH = GRAPH_H;
    const int barBottom = BAR_BOTTOM;
    const int barMaxH = BAR_BOTTOM - BAR_TOP;

    display.drawRect(graphX, graphY, graphW, graphH, BLACK);

    float maxVal = 0;
    for (int i = 0; i < CHANNELS; i++)
        if (r.spectrum[i] > maxVal) maxVal = r.spectrum[i];

    const int barW = graphW / CHANNELS;                      // 55 px
    const int x0 = graphX + (graphW - barW * CHANNELS) / 2;  // vycentrovat zbytek

    display.setFont(&OpenSansSB_20px);
    for (int i = 0; i < CHANNELS; i++)
    {
        const int x = x0 + i * barW;
        const int cx = x + barW / 2;
        const int h = (maxVal > 0) ? (int)(r.spectrum[i] / maxVal * barMaxH + 0.5f) : 0;

        if (h > 0)
        {
            if (i >= spectral::NIR_FIRST)
                fillHatched(x + 4, barBottom - h, barW - 8, h);  // NIR 810–940 nm
            else
                display.fillRect(x + 4, barBottom - h, barW - 8, h, BLACK);
        }

        char buf[12];
        snprintf(buf, sizeof(buf), "%d", spectral::WAVELENGTHS[i]);
        printCentered(buf, cx, barBottom + 32);

        formatChannelValue(r.spectrum[i], buf, sizeof(buf));
        printCentered(buf, cx, VALUE_BASELINE);
    }

    // osa a popisek jednotek
    display.drawFastHLine(graphX + 5, barBottom, graphW - 10, BLACK);
    printCentered("uW/cm2 (prepocteno na 16x / 166 ms), srafovane = NIR", graphX + graphW / 2, barBottom + 95);
}

void drawMetric(int x, int &y, const char *label, const char *value)
{
    display.setFont(&OpenSansSB_24px);
    display.setCursor(x, y);
    display.print(label);
    display.setFont(&OpenSansSB_40px);
    display.setCursor(x, y + PANEL_VALUE_DY);
    display.print(value);
    y += PANEL_STEP;
}

void drawScreen(const Measurement &r)
{
    display.clearDisplay();
    display.setTextColor(BLACK);

    display.setFont(&OpenSansSB_24px);
    printCentered("Vicekanalovy spektralni analyzator osvetleni (410-940 nm)", 20 + 500, 32);

    display.setFont(&OpenSansSB_20px);
    printCentered("chiptron.cz ve spolupraci s botland.cz", 20 + 500, 700);

    const int panelX = PANEL_X;
    int y = PANEL_Y0;
    char buf[32];

    if (!r.ok)
    {
        display.setFont(&OpenSansSB_40px);
        printCentered("Senzor neodpovida", 20 + 500, 340);
        display.setFont(&OpenSansSB_24px);
        printCentered("Zkontrolujte pripojeni AS7265x (I2C)", 20 + 500, 390);
    }
    else
    {
        drawSpectrum(r);

        if (isnan(r.m.blueRatio)) snprintf(buf, sizeof(buf), "---");
        else snprintf(buf, sizeof(buf), "%.1f %%", r.m.blueRatio * 100);
        drawMetric(panelX, y, "Blue", buf);

        if (isnan(r.m.coldIndex)) snprintf(buf, sizeof(buf), "---");
        else snprintf(buf, sizeof(buf), "%.2f", r.m.coldIndex);
        drawMetric(panelX, y, "Cold index", buf);

        if (r.m.cctValid) snprintf(buf, sizeof(buf), "%.0f K", r.m.cct);
        else snprintf(buf, sizeof(buf), "---");
        drawMetric(panelX, y, "CCT", buf);

        if (isnan(r.m.duv)) snprintf(buf, sizeof(buf), "---");
        else snprintf(buf, sizeof(buf), "%+.4f", r.m.duv);
        drawMetric(panelX, y, "Duv", buf);

        snprintf(buf, sizeof(buf), r.illuminanceLux < 100 ? "%.1f" : "%.0f", r.illuminanceLux);
        drawMetric(panelX, y, "Ev odhad [lx]", buf);

        snprintf(buf, sizeof(buf), "%.1f %%", r.m.nirRatio * 100);
        drawMetric(panelX, y, "NIR", buf);
    }

    // stavový řádek
    const ExposureStep &e = EXPOSURE[exposureIndex];
    display.setFont(&OpenSansSB_20px);
    display.setCursor(panelX, STATUS_Y0);
    display.printf("Gain %.1fx  %.0f ms", e.gain, integrationMs(e));
    display.setCursor(panelX, STATUS_Y0 + STATUS_DY);
    if (isnan(r.sensorTempC)) display.printf("Bat %.2f V", r.batteryV);
    else display.printf("%.0f C  Bat %.2f V", r.sensorTempC, r.batteryV);
    display.setCursor(panelX, STATUS_Y0 + 2 * STATUS_DY);
    if (r.saturated)      display.print("! SATURACE");
    else if (r.lowSignal) display.print("! MALO SVETLA");
#if ENABLE_SD_LOG
    else                  display.print(sdOk ? "SD log: ano" : "SD log: ne");
#endif
}

// Parciální refresh budí jen změněné pixely a přechod černá -> bílá nedotáhne,
// takže zůstávají duchy. Proto se před vykreslením nových dat smažou na bílo
// oblasti, které se mění (sloupce, hodnoty pod nimi, hodnoty v pravém panelu,
// stavový blok); nadpis, popisky, osa a rámeček zůstávají a neproblikávají.
//
// Knihovna (11.x) si v DMemoryNew drží, co je podle ní na panelu (1 = černá,
// 1 bit na pixel, řádek po řádku), a budí jen rozdíly proti kreslicímu bufferu.
// Když v DMemoryNew oblast nastavíme na černou, smazání prožene každý pixel
// v ní, a jde to i opakovat.
void forceRegionBlack(int x, int y, int w, int h)
{
    const int bytesPerRow = E_INK_WIDTH / 8;
    const int bx0 = x / 8, bx1 = (x + w - 1) / 8;  // celé bajty; okolní bílé pixely se jen znovu "obílí"
    for (int row = y; row < y + h; row++)
        memset(display.DMemoryNew + row * bytesPerRow + bx0, 0xFF, bx1 - bx0 + 1);
}

// Oblasti, které se před parciálním refreshem mažou: sloupce grafu, hodnoty
// pod nimi, řádky s hodnotami v pravém panelu a stavový blok. Popisky,
// rámeček, osa, nadpis a patička se nemění, takže se nemažou.
int changingRegions(Rect *out)
{
    int n = 0;
    const int x = GRAPH_X + 1, w = GRAPH_W - 2;  // uvnitř rámečku
    out[n++] = {x, BAR_TOP, w, BAR_BOTTOM - BAR_TOP};                 // sloupce, osa zůstane
    out[n++] = {x, VALUE_ROW_TOP, w, VALUE_ROW_BOTTOM - VALUE_ROW_TOP}; // hodnoty pod sloupci
    for (int k = 0; k < PANEL_ITEMS; k++)
    {
        const int baseline = PANEL_Y0 + k * PANEL_STEP + PANEL_VALUE_DY;
        out[n++] = {PANEL_X - 4, baseline - 32, SCREEN_W - PANEL_X + 4, 42};
    }
    out[n++] = {PANEL_X - 4, STATUS_Y0 - 20, SCREEN_W - PANEL_X + 4, 2 * STATUS_DY + 28};
    return n;
}

// Všechny oblasti se smažou NAJEDNOU: CLEAR_PASSES parciálních refreshů celkem,
// ne pro každou oblast zvlášť.
void wipeChanging()
{
    Rect r[PANEL_ITEMS + 3];
    const int n = changingRegions(r);

    for (int i = 0; i < n; i++)
        display.fillRect(r[i].x, r[i].y, r[i].w, r[i].h, WHITE);  // v bufferu je pořád předchozí snímek

    for (uint8_t pass = 0; pass < CLEAR_PASSES; pass++)
    {
        for (int i = 0; i < n; i++) forceRegionBlack(r[i].x, r[i].y, r[i].w, r[i].h);
        display.partialUpdate(false, true);  // leaveOn: nevypínat napájení panelu mezi kroky
    }
}

void refreshDisplay(const Measurement &r)
{
    if (updateCount % FULL_REFRESH_EVERY == 0)
    {
        drawScreen(r);
        display.display();  // plný refresh
    }
    else
    {
        wipeChanging();
        drawScreen(r);
        display.partialUpdate();
    }
    updateCount++;
}

// =====================
// Setup
// =====================
bool initSensor()
{
    if (!sensor.begin()) return false;  // begin() volá Wire.begin() a resetuje takt I2C
    Wire.setClock(I2C_CLOCK_HZ);
    sensor.disableIndicator();
    applyExposure(exposureIndex);
    return true;
}

void setup()
{
    Serial.begin(115200);

    display.begin();
    display.setRotation(0);  // Landscape 1280x720
    display.clearDisplay();
    display.display();

    sensorOk = initSensor();
    if (!sensorOk) Serial.println("AS7265x not found!");

#if ENABLE_SD_LOG
    initSd();
    Serial.println(sdOk ? "SD karta OK, loguji do spektrum.csv" : "SD karta nenalezena, zkusim ji najit kazdou minutu");
#endif

    Serial.println("System ready");
    lastMeasure = millis() - MEASURE_INTERVAL_MS;  // první měření hned
}

// =====================
// Loop
// =====================
void loop()
{
    if (millis() - lastMeasure < MEASURE_INTERVAL_MS) return;
    lastMeasure = millis();

    if (!sensorOk)
    {
        sensorOk = initSensor();
        if (!sensorOk) Serial.println("AS7265x not found, zkusim znovu");
    }

    if (sensorOk && !measure(meas))
    {
        sensorOk = false;  // příště zkusit znovu inicializovat
    }
    if (!sensorOk)
    {
        meas.ok = false;
        meas.saturated = meas.lowSignal = false;
        meas.sensorTempC = NAN;
        meas.batteryV = display.readBattery();
    }

    printSerial(meas);
#if ENABLE_SD_LOG
    logToSd(meas);
#endif
    refreshDisplay(meas);
}
