# ZOnlinE SYSTIMER diagnosztika

[English documentation](README.md)

Válogatott ESP32-S3 időzítőforrások és megőrzött mérések az [espressif/esp-idf#19333](https://github.com/espressif/esp-idf/issues/19333) hibajegyhez. A csomagból a vizsgálók a privát alkalmazásrepóhoz való hozzáférés nélkül is ellenőrizhetik a diagnosztikai kódot és dekódolhatják a közzétett időzítőadatokat.

**Ez kódvizsgálati pillanatkép, nem fordítható firmware, minimális reprodukáló program vagy igazolt javítás.** A számláló felső szavának ugrásait módosított SysTick/HAL kóddal rögzítettük. A kiváltó ok ismeretlen; a mérések nem bizonyítanak szilíciumhibát vagy hibát a módosítatlan ESP-IDF-ben.

## Tartalom és eredet

- [REPORT.md](REPORT.md): mérések, korlátok és kérdések az Espressif számára, angolul.
- A gyökérkönyvtár `zonline_*.c` és `zonline_*.h` fájljai: SysTick/HAL felülírások, számlálóolvasó, védelmi és helyreállítási szabályok, valamint a hibák és mintavételek megőrzése, a helyi fejlécfüggőségekkel együtt.
- [scripts/decode_first_fault.py](scripts/decode_first_fault.py) és [scripts/decode_systimer_probe.py](scripts/decode_systimer_probe.py): az eredeti offline dekódolók.
- [evidence/timer_evidence.json](evidence/timer_evidence.json): a `918346d6` rögzítési boot már közzétett, kiválasztott diagnosztikai mezői.
- [evidence/selected_fault_records.txt](evidence/selected_fault_records.txt): hat rögzítési boot 12, már közzétett formázott rekordja, a későbbi kiírás bootazonosítói nélkül.
- [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json): minden másolt forrás eredeti Git blobazonosítója és SHA-256 lenyomata. A [SHA256SUMS](SHA256SUMS) a csomag összes többi fájljának lenyomatát tartalmazza.

A 11 forrásfájl a privát `pczolee/Renault-Zoe-ZOnliNe` repó `124b85c0cdfdc478a226368d3fc6f642ed6b7bfe` commitjából, a `v2.6.716-test1 (20261029)` firmware-ből származó, bájtra azonos másolat. Az eredeti commit az eredetet jelöli; ebben a külön repóban nem szerepel commitként. Az eredeti elérési utak és szerzői jogi/licencjelzések megmaradtak. Részletek: [NOTICE_HU.md](NOTICE_HU.md).

A jelentett környezet: Arduino-ESP32 3.3.7, futás közben jelzett ESP-IDF `v5.5.2-729-g87912cd291`, ESP32-S3-WROOM-1-N16R8 modul, 240 MHz CPU, 16 MHz SYSTIMER és 1 kHz tick. A szilíciumrevízió még nincs rögzítve.

## A megőrzött adatok dekódolása

A repó gyökérkönyvtárából, Python 3 használatával; külső Python-csomag nem szükséges:

```sh
python3 scripts/decode_first_fault.py evidence/timer_evidence.json
python3 scripts/decode_systimer_probe.py evidence/timer_evidence.json
```

Várt eredmény: `verified_capsules: 2` és `verified_probes: 2`, magonként egy-egy rekord. Mindkét próba 32 elfogadott mintát jelez; `sample_hi_and == sample_hi_or == 567` (`0x237`), és az ellenőrzőösszeg összekapcsolja a próbát a hozzá tartozó első hibával.

**A megőrzött első hiba/próba pár és a legutóbbi tárolt core dump összetartozása nem igazolt.** Ezért marad meg a `latest_dump_association: "unverified"` jelölés. A 716-os formátum újrahasznosít korábban `unit1_load_*` nevű mezőket; a tényleges UNIT1 LOAD regiszterek nincsenek megőrizve. A dekódoló történeti `crc` mezőneve a kapszula ellenőrzőösszegét jelöli; nem azonos a tárolt core dump CRC32 ellenőrzésével.

A szöveges napló ember számára olvasható összefoglaló, nem a dekódolók által várt nyers `ZFF708`/`ZSP716` soros adatfolyam. Nyers kapszulaszavak csak az utolsó eseményhez szerepelnek.

## Ellenőrzések és korlátok

A közzététel előkészítésekor mind a 11 másolt fájlt ellenőriztük az eredeti Git bloblenyomatával. A dekódolók sikeresen ellenőrizték a kiválasztott JSON mindkét elsőhibáját és mindkét próbáját; a hozzájuk tartozó ellenőrzőösszeg-kapcsolatot is ellenőriztük. A kiválasztott rekordok és a JSON egyeznek a hibajegyben már közzétett adatokkal.

Ez a csomagolt adatok és a források eredetének ellenőrzése. A közzétételhez nem készült új firmware-fordítás vagy hardverteszt. A módosítatlan, aktuális IDF-fel végzett alapteszt, a minimális reprodukáló program és a két mag hozzáférését szabályozó kísérlet még hátravan. A pillanatkép UNIT1-olvasásai nincsenek a két mag között sorosítva; ez vizsgálandó lehetőség, nem bizonyított hibaforrás.

A csomag nem tartalmazza a teljes alkalmazást, az SDK/fordítás integrációját, hozzáférési adatokat, az alkalmazás GPS/hálózati/járműnaplóit, ELF/MAP binárisokat vagy nyers RAM-dumpot. Az SDK-fejlécek és az alkalmazás integrációs hívásai hiányoznak, ezért ezekből a fájlokból önmagukban nem építhető újra az eredeti firmware.
