# Spectometer_AS7265x_Inkplate

Projekt vznikl ve spolupráci **chiptron.cz** a **botland.pl**.

Jednoduchý spektrální analyzátor osvětlení postavený na senzoru **AS7265x** a e-paper displeji **Inkplate 5 V2**. Zařízení měří světlo v rozsahu **410–940 nm**, analyzuje jeho spektrální složení a zobrazuje výsledky přehledně na displeji i v Serial monitoru.

---

## Hlavní vlastnosti

- Měření spektra v **18 kanálech (410–940 nm)**
- Grafické zobrazení spektra na e-paper displeji
- Výpočet světelných parametrů:
  - **Blue ratio** – podíl modré složky
  - **Cold index** – poměr modré a červené
  - **CCT (Correlated Color Temperature)** – teplota chromatičnosti
  - **Celková intenzita osvětlení**
- **Automatické řízení zesílení (Auto Gain)**
- Periodické měření každých **10 sekund**
- Výstup dat do **Serial monitoru**

---

## Hardware

- Inkplate 5 V2
- SparkFun AS7265x Spectral Sensor
- I2C připojení (100 kHz)

---

## Použité knihovny

Nainstalujte pomocí Arduino Library Manager:

- Inkplate
- SparkFun AS7265x
- Wire (součást Arduino)

Použité fonty:
- `OpenSansSB_24px`
- `OpenSansSB_40px`

---

## Rozsah měření

| Oblast | Rozsah |
|---|---|
| Viditelné světlo | 410–705 nm |
| Blízké infračervené (NIR) | 730–940 nm |
| Celkem | 18 kanálů |

---

## Zobrazení na Inkplate

Displej je orientován na šířku (1280×720).

**Levá část**
- Sloupcový graf spektra
- Hodnoty jednotlivých kanálů (nm)

**Pravý panel**
- Blue %
- Cold index
- CCT
- Intensity
- Aktuální gain

---

## Spolupráce

Projekt vznikl ve spolupráci:

- **chiptron.cz**
- **botland.pl**
