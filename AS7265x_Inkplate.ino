#include <Inkplate.h>
#include <Wire.h>
#include "SparkFun_AS7265X.h"

#include "OpenSansSB_24px.h"
#include "OpenSansSB_40px.h"

// =====================
// Inkplate 5 V2 (podle výrobce)
// =====================
Inkplate display(INKPLATE_1BIT);

AS7265X sensor;

// =====================
unsigned long lastUpdate = 0;
const unsigned long refreshInterval = 10000;

// Spektrum 410–940 nm
#define CHANNELS 18
float spectrum[CHANNELS];

// Gain řízení
int gainLevels[4] = {
    AS7265X_GAIN_1X,
    AS7265X_GAIN_37X,
    AS7265X_GAIN_16X,
    AS7265X_GAIN_64X
};

float gainText[4] = {1.0, 3.7, 16.0, 64.0};
int gainIndex = 2; // start 16x

const float LOW_THRESHOLD = 200;
const float HIGH_THRESHOLD = 8000;

// =====================
// Výpočty
// =====================
float sumRange(int start, int end)
{
    float s = 0;
    for (int i = start; i <= end; i++) s += spectrum[i];
    return s;
}

float estimateCCT_fromSpectrum(float *s)
{
  /*
  | CCT | Source | Character |
  |-----|--------|-----------|
  | 1800 K | Candle | Very warm, reddish |
  | 2700 K | Incandescent / halogen | Warm white |
  | 3000 K | Warm white LED | Slightly warm |
  | 3500 K | Neutral warm | Office, retail |
  | 4000 K | Cool white | Neutral, clinical |
  | 5000 K | Horizon daylight | Bright neutral |
  | 6500 K | Overcast sky / D65 | Standard daylight reference |
  | 8000 K | Heavily overcast / open shade | Noticeably blue |
  | 10000 K+ | Clear blue sky (no sun) | Very blue |
  */

  float X =
      0.154 * s[0] +   // 410 nm – blue lobe rising
      0.344 * s[1] +   // 435 nm – blue lobe peak flank
      0.230 * s[2] +   // 460 nm – blue lobe descending
      0.091 * s[3] +   // 485 nm – valley between lobes
      0.021 * s[4] +   // 510 nm – valley
      0.120 * s[5] +   // 535 nm – red lobe rising
      0.384 * s[6] +   // 560 nm
      0.765 * s[7] +   // 585 nm
      1.000 * s[8] +   // 610 nm – red lobe peak
      0.753 * s[9] +   // 645 nm
      0.366 * s[10] +  // 680 nm
      0.144 * s[11];   // 705 nm

  // Y = photopic V(λ), single lobe peaking ~555 nm
  float Y =
      0.004 * s[0] +   // 410 nm
      0.020 * s[1] +   // 435 nm
      0.060 * s[2] +   // 460 nm
      0.139 * s[3] +   // 485 nm
      0.503 * s[4] +   // 510 nm
      0.862 * s[5] +   // 535 nm
      0.954 * s[6] +   // 560 nm
      0.870 * s[7] +   // 585 nm
      0.631 * s[8] +   // 610 nm
      0.301 * s[9] +   // 645 nm
      0.107 * s[10] +  // 680 nm
      0.032 * s[11];   // 705 nm

  // Z = almost entirely blue, negligible above 560 nm
  float Z =
      0.455 * s[0] +   // 410 nm
      1.000 * s[1] +   // 435 nm – Z peak
      0.972 * s[2] +   // 460 nm
      0.522 * s[3] +   // 485 nm
      0.112 * s[4] +   // 510 nm
      0.017 * s[5] +   // 535 nm
      0.002 * s[6] +   // 560 nm
      0.000 * s[7] +   // 585 nm
      0.000 * s[8] +   // 610 nm
      0.000 * s[9] +   // 645 nm
      0.000 * s[10] +  // 680 nm
      0.000 * s[11];   // 705 nm

  float sum = X + Y + Z;
  if (sum <= 0) return 0;

  // Chromaticity coordinates
  float x = X / sum;
  float y = Y / sum;

  // McCamy approximation (valid ~2000–10000 K)
  // n is the slope from the chromaticity point to the epicentre (0.3320, 0.1858)
  float n = (x - 0.3320f) / (0.1858f - y);

  float CCT = 449.0f * n * n * n
            + 3525.0f * n * n
            + 6823.3f * n
            + 5520.33f;

  // Clamp to physically reasonable range
  // McCamy becomes unreliable outside 2000–12000 K
  if (CCT < 1000)  CCT = 1000;
  if (CCT > 12000) CCT = 12000;

  return CCT;
}

float calculateM_EDI_ratio()
{
  float melanopic =
    0.00 * spectrum[0] +   // 410 nm
    0.15 * spectrum[1] +   // 435 nm
    0.65 * spectrum[2] +   // 460 nm
    1.00 * spectrum[3] +   // 485 nm (peak)
    0.45 * spectrum[4] +   // 510 nm
    0.05 * spectrum[5];    // 535 nm

  // SPRÁVNÉ V(λ) aproximace pro AS7265x
  float photopic =
      0.000*spectrum[0] + 0.031*spectrum[1] + 0.060*spectrum[2] + 0.191*spectrum[3] + 
      0.503*spectrum[4] + 0.862*spectrum[5] + 0.954*spectrum[6] + 0.826*spectrum[7] +
      0.538*spectrum[8] + 0.268*spectrum[9] + 0.095*spectrum[10] + 0.025*spectrum[11];
  
  if (photopic <= 0) return 0;

  /*    
    Reference values:
  //   D65 daylight:      ~0.906
  //   Cool white LED:    ~0.75–0.85
  //   Warm white LED:    ~0.45–0.55
  //   Tungsten / candle: ~0.25–0.35
  */
  float mp_ratio = melanopic / photopic;

  return mp_ratio;
}

// =====================
// Auto gain
// =====================
void adjustGain(float intensity)
{
    if (intensity < LOW_THRESHOLD && gainIndex < 3)
    {
        gainIndex++;
        sensor.setGain(gainLevels[gainIndex]);
        Serial.println("Gain increased");
    }
    else if (intensity > HIGH_THRESHOLD && gainIndex > 0)
    {
        gainIndex--;
        sensor.setGain(gainLevels[gainIndex]);
        Serial.println("Gain decreased");
    }
}

// =====================
// Serial
// =====================
void printSerial(float blueRatio, float coldIndex,
                 float M_EDI_ratio, float CCT, float intensity)
{
    Serial.println("---- Measurement ----");

    Serial.print("Gain: ");
    Serial.print(gainText[gainIndex]);
    Serial.println("x");

    Serial.print("Blue %: ");
    Serial.println(blueRatio * 100, 2);

    Serial.print("Cold index: ");
    Serial.println(coldIndex, 3);

    /*Serial.print("M-EDI ratio: ");
    Serial.println(M_EDI_ratio, 3);*/

    Serial.print("CCT: ");
    Serial.print(CCT);
    Serial.println(" K");

    Serial.print("Intensity: ");
    Serial.println(intensity);

    Serial.println("---------------------");
}

// =====================
// Graf spektra
// =====================
void drawSpectrum()
{
    int graphX = 20;
    int graphY = 40;
    int graphW = 1000;
    int graphH = 520;

    // oblast grafu
    display.drawRect(graphX, graphY, graphW, graphH + 80, BLACK);

    const int wavelengths[CHANNELS] = {
        410,435,460,485,510,535,
        560,585,610,645,680,705,
        730,760,810,860,900,940
    };

    // maximum pro normalizaci
    float maxVal = 0;
    for (int i = 0; i < CHANNELS; i++)
        if (spectrum[i] > maxVal) maxVal = spectrum[i];

    if (maxVal < 1) maxVal = 1;

    int barW = graphW / CHANNELS;

    // text malé velikosti
    display.setFont(&OpenSansSB_24px);

    for (int i = 0; i < CHANNELS; i++)
    {
        int h = (spectrum[i] / maxVal) * (graphH - 40);
        int x = graphX + i * barW;
        int y = graphY + graphH - h;

        // sloupec
        display.fillRect(x + 2, y, barW - 4, h, BLACK);

        // název kanálu (nm)
        int textY = graphY + graphH + 30;
        display.setCursor(x + 1, textY);
        display.print(wavelengths[i]);

        // změřená hodnota pod nm
        display.setCursor(x + 1, textY + 40);

        int value = (int)spectrum[i];

        // zkrácené zobrazení (aby se vešlo)
        if (value > 999)
            display.print(value / 1000), display.print("k");
        else
            display.print(value);
    }

    // nadpis
    display.setFont(&OpenSansSB_24px);
    display.setCursor(150, 30);
    display.print("Vicekanalovy spektralni analyzator osvetleni (410-940 nm)");

    display.setCursor(300, 680);
    display.print("chiptron.cz ve spolupraci s botland.cz");
}


// =====================
// Obrazovka
// =====================
void drawScreen(float blueRatio, float coldIndex,
                float M_EDI_ratio, float CCT, float intensity)
{
    display.clearDisplay();

    // 1) Spektrum vlevo
    drawSpectrum();

    // 2) Pravý panel
    int panelX = 1040;
    int y = 50;

    display.setFont(&OpenSansSB_40px);

    display.setCursor(panelX, y);
    display.print("Blue:");
    display.setCursor(panelX, y + 50);
    display.print(blueRatio * 100, 1);
    display.print(" %");

    y += 120;

    display.setCursor(panelX, y);
    display.print("Cold idx:");
    display.setCursor(panelX, y + 50);
    display.print(coldIndex, 2);

    /*y += 120;

    display.setCursor(panelX, y);
    display.print("M-EDI ratio:");
    display.setCursor(panelX, y + 50);
    display.print(M_EDI_ratio, 2);*/

    y += 120;

    display.setCursor(panelX, y);
    display.print("CCT:");
    display.setCursor(panelX, y + 50);
    display.print((int)CCT);
    display.print(" K");

    y += 120;

    display.setCursor(panelX, y);
    display.print("Intensity:");
    display.setCursor(panelX, y + 50);
    display.print(intensity, 0);

    y += 120;

    display.setCursor(panelX, y);
    display.print("Gain:");
    display.setCursor(panelX, y + 50);
    display.print(gainText[gainIndex]);
    display.print("x");

    display.display();
}


// =====================
// Čtení senzoru
// =====================
void readSensor()
{
    sensor.takeMeasurements();
    delay(1000);

    // AS72653
    spectrum[0] = sensor.getCalibratedA(); // 410
    spectrum[1] = sensor.getCalibratedB(); // 435
    spectrum[2] = sensor.getCalibratedC(); // 460
    spectrum[3] = sensor.getCalibratedD(); // 485
    spectrum[4] = sensor.getCalibratedE(); // 510
    spectrum[5] = sensor.getCalibratedF(); // 535

    // AS7265X
    spectrum[6]  = sensor.getCalibratedG(); // 560
    spectrum[7]  = sensor.getCalibratedH(); // 585
    spectrum[8]  = sensor.getCalibratedR(); // 610
    spectrum[9]  = sensor.getCalibratedI(); // 645
    spectrum[10] = sensor.getCalibratedS(); // 680
    spectrum[11] = sensor.getCalibratedJ(); // 705

    // AS7265X
    spectrum[12] = sensor.getCalibratedT(); // 730
    spectrum[13] = sensor.getCalibratedU(); // 760
    spectrum[14] = sensor.getCalibratedV(); // 810
    spectrum[15] = sensor.getCalibratedW(); // 860
    spectrum[16] = sensor.getCalibratedK(); // 900
    spectrum[17] = sensor.getCalibratedL(); // 940

}


// =====================
// Setup
// =====================
void setup()
{
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(100000);

    display.begin();
    display.setRotation(0);   // Landscape 1280x720
    display.clearDisplay();
    display.display();

    if (!sensor.begin())
    {
        Serial.println("AS7265x not found!");
        while (1);
    }
    sensor.disableIndicator();

    sensor.setMeasurementMode(2); // MEASUREMENT_MODE_6CHAN_ONESHOT
    sensor.setGain(gainLevels[gainIndex]);
    sensor.setIntegrationCycles(20);

    Serial.println("System ready");
}

// =====================
// Loop
// =====================
void loop()
{
    if (millis() - lastUpdate > refreshInterval)
    {
        lastUpdate = millis();

        readSensor();

        float blue  = sumRange(0, 3);   // 410 - 485 nm
        float green = sumRange(4, 7);   // 510 - 585 nm
        float red   = sumRange(8, 11);  // 610 - 705 nm
        float total = sumRange(0, 17);  // cele spektrum 410–940 nm

        if (total == 0) total = 1;
        if (red == 0) red = 1;

        // Blue ration
        float visible = sumRange(0, 11);
        float blueRatio = blue / visible;

        // Cold index
        float coldIndex = blue / red;
        
        // M-EDI ratio - need to clarify
        float M_EDI_ratio = calculateM_EDI_ratio();

        // CCT
        float CCT = estimateCCT_fromSpectrum(spectrum);

        // Intensity
        float intensity = total;

        adjustGain(intensity);

        printSerial(blueRatio, coldIndex, M_EDI_ratio, CCT, intensity);
        drawScreen(blueRatio, coldIndex, M_EDI_ratio, CCT, intensity);
    }
}
