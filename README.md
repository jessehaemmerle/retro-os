# RetroOS

Ein vollständig eigenes Betriebssystem für x86-64-Rechner – mit eigenem
64-Bit-Kernel, präemptivem Scheduler, Prozessen in Ring 3, eigenen
Treibern, eigenem Fenstersystem, einem dauerhaften Dateisystem auf der
Festplatte, einem TCP/IP-Stapel mit TLS 1.3 und einem Browser, der HTML,
CSS, Bilder und JavaScript versteht.

Kein Linux-Unterbau, keine libc, kein fremdes GUI-Toolkit, keine
Netzwerk- oder Kryptobibliothek, keine Browser-Engine. Übernommen wurde
allein der Bootloader
([Limine](https://github.com/limine-bootloader/limine)), der den Kernel im
Long Mode startet und einen linearen Framebuffer bereitstellt.

![RetroOS: Dateimanager auf der Festplatte, Browser mit einer Seite aus dem Netz](docs/screenshot.png)

```
┌──────────────────────────────────────────────────────────────────────┐
│  Desktop · Taskleiste · Startmenü                                    │
│  Dateimanager · Browser · Editor · Konsole · Systeminformation       │
├──────────────────────────────────────────────────────────────────────┤
│  Fenstersystem (Fenster, Menüs, Dialoge, Bedienelemente)             │
├───────────────────────────────┬──────────────────────────────────────┤
│  Grafik: Backbuffer, Schrift  │  Browser: Dokumentbaum · CSS ·       │
│  Bilder: PNG JPEG GIF BMP     │  Umbruch · JavaScript                │
│  Inflate (DEFLATE)            ├──────────────────────────────────────┤
├───────────────────────────────┤  HTTP/1.1 · TLS 1.3                  │
│  Dateibaum: RAM + FAT32       │  Kryptografie: SHA-2 · HMAC · HKDF · │
├───────────────────────────────┤  ChaCha20 · AES-GCM · X25519 · RSA · │
│  Prozesse in Ring 3           │  ECDSA · X.509 · 152 Wurzeln         │
│  Scheduler · Threads          ├──────────────────────────────────────┤
├───────────────────────────────┤  TCP · UDP · DHCP · DNS · ICMP       │
│  Seitenverwaltung · Heap      │  IPv4 · ARP · Ethernet               │
├───────────────────────────────┼──────────────────────────────────────┤
│  Blockgeräte: AHCI · ATA      │  Netzwerkkarte: Intel e1000          │
├───────────────────────────────┴──────────────────────────────────────┤
│  PS/2-Tastatur · PS/2-Maus · PIT · RTC · UART · PCI · ACPI           │
├──────────────────────────────────────────────────────────────────────┤
│  Kern: GDT · IDT · TSS · PIC · Systemaufrufe (SYSCALL/SYSRET)        │
└──────────────────────────────────────────────────────────────────────┘
```

## Bauen und starten

Vorausgesetzt werden `gcc`, `binutils`, `make`, `xorriso` und `git`
(zum Nachladen des Bootloaders). Zum Ausprobieren zusätzlich
`qemu-system-x86`.

```sh
sudo apt install build-essential xorriso qemu-system-x86 ovmf dosfstools
make                # baut den Kernel und retroos.iso
make run            # startet in QEMU – mit Festplatte und Netzwerk
make run-uefi       # dasselbe, aber per UEFI gebootet
make run-plain      # ohne Festplatte und ohne Netzwerk
```

`make run` legt beim ersten Aufruf `build/festplatte.img` an (256 MiB, FAT32)
und hängt eine Netzwerkkarte an. Die Platte bleibt bei `make clean` erhalten.

Das erzeugte `retroos.iso` ist ein Hybrid-Image: es bootet per BIOS *und*
per UEFI, aus einem virtuellen Laufwerk ebenso wie von einem USB-Stick.

```sh
sudo dd if=retroos.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

> Auf echter Hardware erwartet RetroOS eine PS/2-Tastatur und -Maus (die
> meisten Notebooks emulieren das für ihre eingebauten Geräte; sonst hilft
> die BIOS-Einstellung „USB Legacy Support"), einen SATA-Controller im
> AHCI-Modus oder einen IDE-Controller sowie eine Intel-Netzwerkkarte der
> Reihe 8254x/8257x.

## Bedienung

| Aktion | Wirkung |
| --- | --- |
| Doppelklick auf ein Desktop-Symbol | Programm starten |
| Start-Knopf unten links | Menü mit allen Programmen |
| Titelleiste ziehen | Fenster verschieben |
| Ecke unten rechts ziehen | Fenster vergrößern |
| `_` / `X` in der Titelleiste | Fenster ablegen / schließen |
| Klick in der Taskleiste | Fenster holen oder ablegen |

**Dateimanager:** Doppelklick öffnet Ordner und Dateien, die rechte Maustaste
öffnet das Kontextmenü. Über die Tastatur: Pfeiltasten wählen aus, `Eingabe`
öffnet, `Rücktaste` geht eine Ebene höher, `F2` benennt um, `Entf` löscht,
`F5` aktualisiert. Der Zweig `/Festplatte` liegt auf dem Datenträger, alles
andere im Arbeitsspeicher.

**Browser:** Adresse eintippen und `Eingabe`. Verweise, Knöpfe und
Eingabefelder sind bedienbar, `Rücktaste` geht zurück, `F5` lädt neu.

| Adresse | Bedeutung |
| --- | --- |
| `https://rechner/pfad` | verschlüsselte Seite aus dem Netz |
| `http://rechner/pfad` | unverschlüsselte Seite aus dem Netz |
| `datei:/Dokumente/beispiel.html` | Datei aus dem Dateisystem |
| `start:` | eingebaute Startseite |

Die Statuszeile nennt bei jeder Seite, ob und womit verschlüsselt wurde.
`datei:/Dokumente/pruefung.html` ist ein Selbsttest der Darstellung:
Schriftgrößen, Kästen, Tabellen, Bilder in drei Formaten und ein Skript
mit Knöpfen.

**Editor:** normales Tippen, `Strg`+`S` speichert – auch auf die Festplatte.

**Konsole:** `hilfe` zeigt alle Befehle. Neben `ls`, `cd`, `cat`, `mkdir`,
`touch`, `schreib`, `rm` und `edit` gibt es:

| Befehl | Wirkung |
| --- | --- |
| `starte <programm>` | ein Ring-3-Programm ausführen |
| `programme` | die eingebauten Programme auflisten |
| `threads` | laufende Threads mit Zustand und Rechenzeit |
| `platte` | Laufwerke und eingehängtes Dateisystem |
| `formatieren wirklich [Name]` | Datenträger neu mit FAT32 formatieren |
| `netz` | IP-Adresse, Gateway, Namensserver, Paketzähler |
| `ping <ziel>` | Erreichbarkeit prüfen |
| `aufloesen <name>` | Namen in eine Adresse wandeln |
| `holen <adresse> [datei]` | Seite abrufen und wahlweise speichern |
| `neustart` / `leeren` | Rechner neu starten, Bildschirm leeren |

Über das Startmenü lässt sich der Rechner auch abschalten – über ACPI,
also so, wie es ein Betriebssystem tut.

![Systeminformation und Startmenü](docs/systeminfo.png)

## Was drinsteckt

| Bereich | Umsetzung |
| --- | --- |
| **Start** | Limine-Protokoll, Higher-Half-Kernel bei `0xffffffff80000000` |
| **CPU** | eigene GDT mit TSS, IDT mit 48 Vektoren, Ausnahmebehandlung mit Panik-Ausgabe |
| **Interrupts** | 8259A-PIC auf Vektor 32–47 umgelegt, PIT als 1000-Hz-Systemtakt |
| **Scheduler** | präemptiv, Zeitscheiben von 20 ms, drei Prioritäten, Schlafen und Warten |
| **Prozesse** | ELF64-Lader, eigener Adressraum je Prozess, Ring 3, Systemaufrufe über `SYSCALL`/`SYSRET` |
| **Speicher** | Bitmap-Allokator für Seitenrahmen, vierstufige Seitentabellen, Heap mit Blockverschmelzung |
| **Busse** | PCI-Erkennung über Konfigurationsmechanismus 1 |
| **Datenträger** | AHCI-Treiber (SATA, DMA) mit ATA-PIO als Rückfallebene |
| **Dateisystem** | FAT32 mit langen Dateinamen – lesen, schreiben, anlegen, umbenennen, löschen, formatieren |
| **Eingabe** | 8042-Controller, Tastatur mit deutscher Belegung inkl. AltGr, Maus mit Scrollrad |
| **Grafik** | 32-Bit-Framebuffer, Backbuffer, Clipping, Verläufe, 3D-Kanten, frei skalierbare Bitmapschrift |
| **Bilder** | eigener DEFLATE-Entpacker, PNG (alle Farbtypen, Adam7), JPEG (Grundverfahren), GIF, BMP |
| **Netzwerk** | Intel-e1000-Treiber, Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP/1.1 |
| **TCP** | Fenstersteuerung, Umsortierung, langsamer Start, Überlastvermeidung, schnelle Wiederholung |
| **Kryptografie** | SHA-256/384/512, HMAC, HKDF, ChaCha20-Poly1305, AES-128/256-GCM, X25519, RSA, ECDSA P-256 |
| **TLS** | TLS 1.3 als Client, X.509-Ketten gegen 152 eingebaute Wurzelzertifikate |
| **Browser** | Dokumentbaum, CSS-Kaskade, Kastenmodell, Bilder, JavaScript |
| **Energie** | ACPI: RSDP, XSDT, FADT, DSDT mit `_S5_`-Auswertung zum Abschalten |
| **Oberfläche** | Fensterstapel, Fokus, Verschieben, Größe ändern, Taskleiste, Popup-Menüs, Dialoge |
| **Programme** | Dateimanager, Browser, Texteditor, Konsole, Systeminformation, sechs Ring-3-Programme |

### Der Browser im Einzelnen

Der Ablauf entspricht dem eines richtigen Browsers:

1. Die Seite wird geholt – über HTTP oder über TLS 1.3 – und zu einem
   **Dokumentbaum** zerlegt. Fehlende Schlusszeichen, verschachtelte
   Absätze und rohe Bereiche wie `<script>` behandelt der Leser so, wie
   es die Praxis verlangt.
2. Eingebundene **Formatvorlagen, Skripte und Bilder** werden nachgeladen –
   jedes als eigener Auftrag an einen Arbeits-Thread, damit die Oberfläche
   bedienbar bleibt.
3. Die **Kaskade** gewichtet Element-, Klassen- und Kennungsselektoren,
   Nachfahrenbeziehungen, das `style`-Attribut und `!important`. Eigene
   Eigenschaften (`--name` und `var()`) werden vererbt, `calc()`, `min()`,
   `max()` und `clamp()` ausgerechnet.
4. Die **Skripte** laufen und dürfen den Baum verändern.
5. Der Baum wird nach dem **Kastenmodell** umgebrochen: Blöcke
   untereinander, Inline-Inhalt in Zeilen, mit Außen- und Innenabständen,
   Rahmen, Ausrichtung, schwebenden Kästen und Tabellen.

Ändert ein Skript später etwas – durch einen Klick, eine Eingabe oder
einen Zeitgeber – werden die Schritte drei bis fünf wiederholt.

Der JavaScript-Deuter versteht den üblichen Sprachumfang: Variablen mit
`var`, `let` und `const`, Funktionen, Abschlüsse, Pfeilfunktionen,
Vorgabewerte und Restparameter, Objekte, Felder, Zerlegung, Klassen mit
Vererbung, Vorlagen mit `${…}`, Ausnahmen sowie die eingebauten Objekte
`Object`, `Array`, `String`, `Number`, `Math`, `JSON`, `Date` und
`console`. Vom Dokument aus erreichbar sind unter anderem
`getElementById`, `querySelector`, `createElement`, `appendChild`,
`innerHTML`, `textContent`, `classList`, `style`, `addEventListener`,
`setTimeout` und `setInterval`.

Zahlen sind Festkommazahlen mit 16 Nachkommastellen statt Gleitkommazahlen:
Der Kern wird ohne Gleitkommaeinheit übersetzt, damit ein Kontextwechsel
keine Registersätze retten muss. Der Bereich reicht von etwa plus/minus
140 Billionen bei einer Genauigkeit von 1/65536 – für Seitenskripte,
Zeitangaben und Koordinaten mehr als genug.

### Wie die Teile zusammenspielen

Der Dateibaum kennt zwei Sorten von Knoten. Alles unterhalb von
`/Festplatte` spiegelt Einträge eines FAT32-Datenträgers: Ordner werden
gelesen, sobald jemand hinsieht, und jede Änderung geht sofort auf die
Platte. Der Rest liegt im Arbeitsspeicher und ist nach einem Neustart wieder
im Auslieferungszustand. Nach außen sieht man den Unterschied nicht – der
Dateimanager, der Editor und die Konsole benutzen dieselben Funktionen.

Die Datenträger sind mit anderen Systemen austauschbar: was RetroOS
schreibt, liest Linux oder Windows ohne Weiteres, samt langer Dateinamen.

Die Oberfläche bleibt während des Ladens bedienbar, weil der Browser
seine Aufträge an einen eigenen Thread abgibt. Damit das ohne Verklemmung
geht, gilt im Kern eine einfache Regel: Wer die Umschaltung sperrt, darf
nicht blockieren. Ein Verstoß dagegen endet in einer Panik statt in einem
stehenden Bild – so wurde der Fehler gefunden, der ARP-Auflösung unter
gesperrter Umschaltung betrieb.

Anwendungen laufen wahlweise als Teil des Kernels – die Fensterprogramme –
oder als eigener Prozess in Ring 3 mit eigenem Adressraum. Ein Programm,
das dort abstürzt, reißt nichts mit: `starte absturz` zeigt es vor.

## Tests

Die Bausteine, die sich ohne Bildschirm prüfen lassen, haben eine
Testsammlung, die auf dem Entwicklungsrechner läuft:

```sh
cd tests && make
```

| Sammlung | Was geprüft wird |
| --- | --- |
| **Kryptografie** | 68 Prüfungen gegen FIPS 180-4, RFC 4231, RFC 5869, RFC 8439, FIPS 197, NIST GCM, RFC 7748, RFC 8448 und echte OpenSSL-Signaturen |
| **Bilder** | 13 Prüfungen: PNG in allen Farbtypen und Bittiefen samt Adam7, GIF verschränkt, BMP, JPEG gegen libjpeg |
| **JavaScript** | 55 kleine Programme mit erwarteter Ausgabe |
| **Dokumentbaum** | 78 Prüfungen zu Zerteiler, Kaskade, Umbruch, Skripten am Baum und Zeitgebern |

Die Testbilder erzeugt `make testbilder` neu (benötigt Pillow).

## Aufbau des Quelltexts

```
kernel/
  include/            Öffentliche Header aller Bausteine
  linker.ld           Speicheraufteilung des Kernels
  src/
    main.c            Startreihenfolge des Systems
    arch/             GDT, IDT, Interrupt-Stubs, PIC, PIT, ACPI, Systemaufrufe
    mm/               Seitenverwaltung, Adressräume und Heap
    sched/            Threads und präemptiver Scheduler
    proc/             ELF64-Lader und Prozesse in Ring 3
    drivers/          Framebuffer, PS/2, Tastatur, Maus, RTC, UART,
                      PCI, AHCI, ATA, Blockgeräte, e1000
    fs/               Dateibaum, FAT32, Startbestand des RAM-Teils
    net/              Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, TLS, HTTP
    crypto/           SHA-2, HKDF, ChaCha20, AES, X25519, Großzahlen,
                      RSA, P-256, ASN.1, X.509, Wurzelzertifikate
    gfx/              DEFLATE, PNG, JPEG, GIF, BMP, Skalieren und Zeichnen
    gui/              Grafik, Schrift, Symbole, Fenstersystem, Desktop, Bedienelemente
    js/               Zerteiler, Deuter, Bibliothek und Anbindung an den Baum
    apps/             Dateimanager, Browser, Dokumentbaum, HTML-Leser,
                      CSS, Umbruch, Editor, Konsole, Systeminformation, Dialoge
userland/             Ring-3-Programme samt kleiner Laufzeitbibliothek
data/                 Wurzelzertifikate und Beispielbilder
tests/                Testsammlungen für den Entwicklungsrechner
boot/limine.conf      Bootloader-Eintrag
scripts/              Bootloader holen, Schrift, Zertifikate und Bilder erzeugen
```

Erzeugte und eingecheckte Dateien: die Schrift in
`kernel/src/gui/font_data.c` (`python3 scripts/gen_font.py`), die
Wurzelzertifikate in `data/wurzelzertifikate.der`
(`python3 scripts/gen_trust_store.py`) und die Beispielbilder in `data/`
(`python3 scripts/gen_bilder.py`).

## Fehlersuche

Der Kernel schreibt seine Meldungen auf die serielle Schnittstelle, nicht auf
den Bildschirm – dort läuft die Oberfläche. In QEMU erscheinen sie dank
`-serial stdio` direkt im Terminal:

```
=====================================
  RetroOS 1.0 - Systemstart
=====================================
Bildschirm  : 1280x800, 32 Bit
PMM         : 508 MiB verwaltet, 508 MiB frei
Heap        : 260 KiB bereit
Adressraum  : Kernel-Tabelle bei 0x000000001ff46000
ACPI        : PM1a 0x604, S5 = 0/0
PCI         : 6 Geraete gefunden
  00:31.2  8086:2922  SATA-Controller (AHCI)
Datentraeger: sata0 - QEMU HARDDISK, 128 MiB
Maus        : Typ 3, 4-Byte-Pakete
Dateisystem : 32 Eintraege, 474351 Bytes
Datentraeger: RETROOS eingehaengt unter /Festplatte (125 MiB frei)
Scheduler   : bereit, Zeitscheibe 20 ms
Systemaufrufe: bereit (14 Nummern)
Zertifikate : 152 Wurzeln geladen
Netzwerk    : Intel 82540EM, 52:54:00:12:34:56
Netzwerk    : 10.0.2.15, Gateway 10.0.2.2, DNS 10.0.2.3
Oberflaeche : bereit
```

Eine CPU-Ausnahme hält das System an und gibt Vektor, Fehlercode, `RIP`,
`RSP` und `CR2` aus. Was ein Skript auf `console.log` schreibt, landet
ebenfalls auf der seriellen Schnittstelle.

Zum Ausprobieren des Browsers genügt ein beliebiger Webserver auf dem
Wirtsrechner; unter QEMUs Benutzer-Netzwerk ist er aus RetroOS heraus als
`10.0.2.2` erreichbar:

```sh
cd /pfad/zu/seiten && python3 -m http.server 8000
# in RetroOS:  http://10.0.2.2:8000
```

## Lizenz

MIT – siehe [LICENSE](LICENSE). Der beim Bauen geladene Bootloader Limine
steht unter der BSD-2-Clause-Lizenz.
