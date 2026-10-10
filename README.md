# Spectometer_AS7265x_Inkplate

Projekt vznikl ve spolupráci **chiptron.cz** a **botland.pl**.

Jednoduchý spektrální analyzátor osvětlení postavený na senzoru **AS7265x** a e-paper displeji **Inkplate 5 V2**. Zařízení měří světlo v rozsahu **410–940 nm**, analyzuje jeho spektrální složení a zobrazuje výsledky přehledně na displeji i v Serial monitoru.

Celý projekt najdete na https://chiptron.cz/projekt-vicekanaloveho-spektralniho-analyzatoru-verejneho-osvetleni/

---

## Hlavní vlastnosti

- Měření spektra v **18 kanálech (410–940 nm)**
- Grafické zobrazení spektra na e-paper displeji: parciální refresh, před kterým se najednou smažou na bílo jen měnící se oblasti (sloupce, hodnoty, pravý panel), a plný refresh jednou za 20 měření
- Výpočet světelných parametrů:
  - **Blue** – podíl modré složky (410–485 nm) ve viditelné části (410–760 nm)
  - **Cold index** – poměr modré (410–485 nm) a červené (610–705 nm)
  - **CCT** – náhradní teplota chromatičnosti (zobrazí se jen tam, kde má smysl)
  - **Duv** – vzdálenost od Planckovy křivky (kladné = do zelena, záporné = do fialova)
  - **Ev** – orientační odhad osvětlenosti v luxech
  - **NIR** – podíl blízké infračervené složky (810–940 nm)
- **Automatická expozice** (gain 1×–64× a integrační čas 14–556 ms). Začíná vždy od nejnižší citlivosti a zvyšuje ji jen tehdy, když kanály vzrostou úměrně, jinak se vrátí o krok zpět
- Průměrování 3 vzorků, měření každých **10 sekund**
- Výstup do **Serial monitoru** a volitelně **CSV log na microSD** (`spektrum.csv`)

---

## Hardware

- Inkplate 5 V2
- SparkFun AS7265x Spectral Sensor
- I2C připojení (400 kHz)
- microSD karta (volitelně, pro logování)

---

## Struktura repozitáře

```
AS7265x_Inkplate/        sketch pro Arduino IDE (otevřete AS7265x_Inkplate.ino)
  SpectralMath.h         výpočty (bez závislosti na Arduinu)
  OpenSansSB_*.h         fonty
tools/compute_weights.py výpočet vah XYZ a ověření přesnosti CCT
test/                    test výpočtů na PC
```

Arduino IDE vyžaduje, aby se složka sketche jmenovala stejně jako `.ino` soubor, proto je sketch v podsložce `AS7265x_Inkplate/`. Funguje to i při stažení repozitáře jako ZIP.

---

## Použité knihovny

Nainstalujte pomocí Arduino Library Manager:

- InkplateLibrary (testováno s verzí 11.2.0; starší verze mají jiné API pro RTC)
- SparkFun AS7265x
- Wire (součást Arduino)

Použité fonty (Open Sans SemiBold, převedené do formátu Adafruit GFX):
- `OpenSansSB_20px`
- `OpenSansSB_24px`
- `OpenSansSB_40px`

Nastavení (perioda měření, počet vzorků, logování na SD, kalibrace luxů) je na začátku sketche.

---

## Rozsah měření

| Oblast | Rozsah |
|---|---|
| Viditelné světlo | 410–760 nm |
| Blízké infračervené (NIR) | 810–940 nm |
| Celkem | 18 kanálů |

---

## Jak se počítá

**Váhy kanálů.** Středy kanálů nejsou rozmístěné rovnoměrně (25 až 50 nm), takže každý kanál se při sčítání pásem násobí šířkou intervalu, který zastupuje.

**CCT a Duv.** Ze spektra se odhadnou souřadnice CIE 1931 XYZ, z nich chromatičnost a pak se hledá nejbližší bod na Planckově křivce (CIE 1960 uv). Váhy pro XYZ jsou spočítané metodou nejmenších čtverců na modelu senzoru (kanály s FWHM 20 nm) a ověřené na standardních iluminantech CIE 15:2018, které ve fitu nebyly:

| Zdroj | Chyba CCT |
|---|---|
| Žárovka, Planckův zářič 1 800–10 000 K | do 1 % |
| Bílé LED (CIE LED-B1 až B5, V1, V2) | do 2 % |
| Vysokotlaký sodík (HP1, HP2) | do 2 % |
| Metalhalogenid (HP3–HP5), zářivky, RGB a hybridní LED | až 10 % |

Výrobní tolerance středů kanálů (±10 nm podle datasheetu) přidává další nejistotu, při posunu o ±5 nm je medián chyby CCT 1,3 %. CCT se zobrazí jen pro |Duv| ≤ 0,05 a rozsah 1 000–15 000 K, jinak `---`.

**Ev (lx).** Odhad z fotopicky vážené složky Y za předpokladu, že kalibrované hodnoty AS7265x jsou v µW/cm² (tolerance výrobce ±12 %). Senzor nemá kosinový difuzor, takže hodnota platí jen při kolmém dopadu. Pro přesná čísla změřte zdroj luxmetrem a dorovnejte konstantu `LUX_CAL`.

**Expozice.** Kalibrované hodnoty AS7265x platí pro gain 16× a integrační čas 166 ms a s jinou expozicí se mění. Sketch je proto přepočítává na tyto referenční podmínky, aby šly hodnoty porovnávat i po změně gainu. Při stálém osvětlení by se Ev ani spektrum neměly měnit, když autoexpozice přepne gain; pokud se mění, pošlete prosím výpis ze Serial monitoru.

**Korekce kanálů.** Tři čipy AS7265x mají každý vlastní tovární kalibraci a u testovaného kusu měřil čip AS72651 (610, 680, 730, 760, 810, 860 nm) asi 2,7× víc než ostatní. Projevilo se to výraznou špičkou na 610 nm, CCT kolem 2 800 K a Duv −0,024 u studené bílé LED. Po korekci vychází asi 5 000 K a Duv −0,002. Korekce je v poli `CHANNEL_CORRECTION` na začátku sketche a platí jen pro tento kus. U jiného senzoru nastavte všechny hodnoty na 1.0, zkontrolujte, jestli spektrum nemá skoky mezi kanály různých čipů, a případně koeficient přepočítejte podle návodu v části [Kalibrace doma](#kalibrace-doma).

## Kalibrace doma

Kalibrace má dva nezávislé kroky. Korekce čipů ovlivňuje tvar spektra a s ním CCT, Duv, Blue % a Cold index. `LUX_CAL` mění jen odhad osvětlenosti v luxech. Bez luxmetru stačí udělat první krok.

### Příprava

- **Difuzor nad senzorem.** Bez něj čipy pod úhlem stíní, takže kalibrace bez difuzoru platí jen pro měření bez difuzoru. Stačí pečicí papír, pár vrstev teflonové pásky na závity nebo kotouček z bílého PLA tlustý 0,4–0,6 mm.
- **Automatická expozice:** `FIXED_EXPOSURE_STEP = -1`.
- **Senzor i zdroj pevně na místě.** Nedržte nic v ruce.
- **Otevřený Serial monitor** (115200 Bd). Hodnoty se čtou z řádku `Spektrum [uW/cm2 @16x/166ms]:`. Pořadí kanálů je 410, 435, 460, 485, 510, 535, 560, 585, **610**, **645**, **680**, **705**, 730, 760, 810, 860, 900, 940 nm.

### Krok 1: korekce čipu AS72651 (`K_AS72651`)

Kanály 610, 680, 730, 760, 810 a 860 nm jsou na jiném čipu než jejich sousedé. Když má tento čip jinou tovární kalibraci, spektrum na těchto vlnových délkách skáče.

1. Ve sketchi nastavte `K_AS72651 = 1.0f` a nahrajte ho.
2. Jako zdroj použijte **halogenovou nebo klasickou žárovku**, ve vzdálenosti 30–50 cm, přes difuzor. Její spektrum je hladké, takže se na něm každý skok pozná. LED ani zářivky se na tento krok nehodí, mají v tomto rozsahu vlastní píky a propady.
3. Nechte proběhnout aspoň 5 měření a z každého si zapište hodnoty kanálů 585, 610, 645, 680 a 705 nm.
4. Pro každé měření spočítejte:

   ```
   k610 = 0,975 / ( 610 / ((585 + 645) / 2) )
   k680 = 1,02  / ( 680 / ((645 + 705) / 2) )
   ```

   Čísla 0,975 a 1,02 jsou poměry, které by mělo dát hladké spektrum žárovky (2 700–3 100 K, na přesné teplotě skoro nezáleží).
5. `K_AS72651` je průměr všech `k610` a `k680`. Pokud se `k610` a `k680` liší o víc než 20 %, má problém spíš jeden kanál než celý čip. Pak je lepší upravit jednotlivé položky v poli `CHANNEL_CORRECTION`.
6. Zapište hodnotu do sketche, nahrajte ho a zkontrolujte, že jsou splněné tyto podmínky:
   - spektrum žárovky plynule stoupá od modré k červené, bez zubu na 610 nm,
   - CCT vychází 2 600–3 200 K,
   - Duv je mezi −0,005 a +0,005.

Pro informaci: u testovaného kusu vyšlo `K_AS72651 = 0,371`. Bylo to ale ze směsi LED a denního světla a bez difuzoru, takže jde jen o hrubý odhad.

### Krok 2: luxy (`LUX_CAL`)

Potřebujete luxmetr. Aplikace v telefonu nestačí, chybují o desítky procent.

1. Nechte `LUX_CAL = 1.0f`.
2. Čidlo luxmetru položte těsně vedle senzoru, ve stejné výšce a se stejnou orientací.
3. Měřte pod rozptýleným světlem, například stropním osvětlením místnosti. Bodový zdroj zblízka se nehodí.
4. Spočítejte `LUX_CAL = Ev z luxmetru / Ev (odhad) ze Serial monitoru` a vezměte průměr z několika měření.
5. Výsledek ověřte pod jiným zdrojem, třeba denním světlem u okna. Rozdíl do ±10 % je na tento senzor dobrý výsledek.

### Kontrolní hodnoty

| Zdroj | CCT | Duv | Blue | Cold index |
|---|---|---|---|---|
| Vysokotlaká sodíková výbojka | 1 900–2 200 K | 0 až +0,01 | 6 % | 0,15 |
| Žárovka | 2 700 K | 0 | 6 % | 0,14 |
| Halogenová žárovka | 3 000 K | 0 | 8 % | 0,19 |
| Teplá bílá LED | 2 700–3 000 K | −0,005 až +0,003 | 11–13 % | 0,23–0,29 |
| Neutrální bílá LED | 4 000 K | −0,005 až +0,003 | 21 % | 0,62 |
| Denní světlo se sluncem (D50) | 5 000 K | +0,003 | 22 % | 0,66 |
| Studená bílá LED | 5 000 K | ±0,005 | 26 % | 0,95 |
| Zatažená obloha (D65) | 6 500 K | +0,003 | 30 % | 1,08 |
| Studená bílá LED | 6 500 K | ±0,005 | 33 % | 1,45 |
| Zataženo, stín (D75) | 7 500 K | +0,003 | 34 % | 1,32 |
| Jasná modrá obloha bez slunce | 10 000 K | +0,003 | 40 % | 1,79 |

Hodnoty Blue a Cold index jsou spočítané modelem senzoru (`tools/compute_weights.py`) ze standardních spekter CIE. Nejde o laboratorní měření. Skutečná obloha se mění s počasím a denní dobou a okenní sklo může Blue snížit o 1–2 procentní body. U LED stejné CCT závisí Cold index na luminoforu, proto je u studené LED 6 500 K vyšší než u denního světla 6 500 K.

Kalibrace platí jen s difuzorem, se kterým byla změřená. Po jeho výměně ji zopakujte.

## Zobrazení na Inkplate

Displej je orientován na šířku (1280×720).

**Levá část**
- Sloupcový graf spektra (NIR 810–940 nm šrafovaně)
- Vlnová délka a změřená hodnota pod každým sloupcem

**Pravý panel**
- Blue %, Cold index, CCT, Duv, Ev, NIR %
- Stav: gain a integrační čas, teplota senzoru, napětí baterie, saturace, SD log

---

## Spolupráce

Projekt vznikl ve spolupráci:

- **chiptron.cz**
- **botland.pl**
