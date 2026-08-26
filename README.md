# RetroOS

Ein kleines, vollständig eigenes Betriebssystem für x86-64-Rechner – mit
eigenem 64-Bit-Kernel, eigenen Treibern, eigenem Fenstersystem, einem
Dateimanager, einem dauerhaften Dateisystem auf der Festplatte, einem
TCP/IP-Netzwerkstapel und einem Webbrowser.

Kein Linux-Unterbau, keine libc, kein fremdes GUI-Toolkit, keine
Netzwerkbibliothek. Übernommen wurde allein der Bootloader
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
│  Grafik (Backbuffer, Schrift) │  HTTP · HTML-Leser                   │
├───────────────────────────────┤  TCP · UDP · DHCP · DNS · ICMP       │
│  Dateibaum: RAM + FAT32       │  IPv4 · ARP · Ethernet               │
├───────────────────────────────┼──────────────────────────────────────┤
│  Blockgeräte: AHCI · ATA      │  Netzwerkkarte: Intel e1000          │
├───────────────────────────────┴──────────────────────────────────────┤
│  PS/2-Tastatur · PS/2-Maus · PIT · RTC · UART · PCI                  │
├──────────────────────────────────────────────────────────────────────┤
│  Kern: GDT · IDT · PIC · Seitenverwaltung · Heap                     │
└──────────────────────────────────────────────────────────────────────┘
```

## Bauen und starten

Vorausgesetzt werden `gcc`, `binutils`, `make`, `xorriso` und `git`
(zum Nachladen des Bootloaders). Zum Ausprobieren zusätzlich `qemu-system-x86`.

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

**Browser:** Adresse eintippen und `Eingabe`. Verweise sind anklickbar,
`Rücktaste` geht zurück, `F5` lädt neu. Drei Adressarten werden verstanden:

| Adresse | Bedeutung |
| --- | --- |
| `http://rechner/pfad` | Seite aus dem Netz |
| `datei:/Dokumente/beispiel.html` | Datei aus dem Dateisystem |
| `start:` | eingebaute Startseite |

**Editor:** normales Tippen, `Strg`+`S` speichert – auch auf die Festplatte.

**Konsole:** `hilfe` zeigt alle Befehle. Neben `ls`, `cd`, `cat`, `mkdir`,
`touch`, `schreib`, `rm` und `edit` gibt es:

| Befehl | Wirkung |
| --- | --- |
| `platte` | Laufwerke und eingehängtes Dateisystem |
| `formatieren wirklich [Name]` | Datenträger neu mit FAT32 formatieren |
| `netz` | IP-Adresse, Gateway, Namensserver, Paketzähler |
| `ping <ziel>` | Erreichbarkeit prüfen |
| `aufloesen <name>` | Namen in eine Adresse wandeln |
| `holen <adresse> [datei]` | Seite abrufen und wahlweise speichern |

![Systeminformation und Startmenü](docs/systeminfo.png)

## Was drinsteckt

| Bereich | Umsetzung |
| --- | --- |
| **Start** | Limine-Protokoll, Higher-Half-Kernel bei `0xffffffff80000000` |
| **CPU** | eigene GDT, IDT mit 48 Vektoren, Ausnahmebehandlung mit Panik-Ausgabe |
| **Interrupts** | 8259A-PIC auf Vektor 32–47 umgelegt, PIT als 1000-Hz-Systemtakt |
| **Speicher** | Bitmap-Allokator für Seitenrahmen, Heap mit Freispeicherliste und Blockverschmelzung |
| **Busse** | PCI-Erkennung über Konfigurationsmechanismus 1 |
| **Datenträger** | AHCI-Treiber (SATA, DMA) mit ATA-PIO als Rückfallebene |
| **Dateisystem** | FAT32 mit langen Dateinamen – lesen, schreiben, anlegen, umbenennen, löschen, formatieren |
| **Eingabe** | 8042-Controller, Tastatur mit deutscher Belegung inkl. AltGr, Maus mit Scrollrad |
| **Grafik** | 32-Bit-Framebuffer, Backbuffer, Clipping, Verläufe, 3D-Kanten, 8×16-Bitmapschrift (Latin-1) |
| **Netzwerk** | Intel-e1000-Treiber, Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP/1.1 |
| **Oberfläche** | Fensterstapel, Fokus, Verschieben, Größe ändern, Taskleiste, Popup-Menüs, Dialoge |
| **Programme** | Dateimanager, Browser, Texteditor, Konsole, Systeminformation |

### Wie die Teile zusammenspielen

Der Dateibaum kennt zwei Sorten von Knoten. Alles unterhalb von
`/Festplatte` spiegelt Einträge eines FAT32-Datenträgers: Ordner werden
gelesen, sobald jemand hinsieht, und jede Änderung geht sofort auf die
Platte. Der Rest liegt im Arbeitsspeicher und ist nach einem Neustart wieder
im Auslieferungszustand. Nach außen sieht man den Unterschied nicht – der
Dateimanager, der Editor und die Konsole benutzen dieselben Funktionen.

Die Datenträger sind mit anderen Systemen austauschbar: was RetroOS
schreibt, liest Linux oder Windows ohne Weiteres, samt langer Dateinamen.

### Bewusste Vereinfachungen

RetroOS ist ein überschaubares System, kein Unix-Ersatz. Die wichtigsten
Grenzen, damit klar ist, was es *nicht* tut:

- **Ein Adressraum, kein Scheduler.** Alles läuft im Kernel-Modus. Es gibt
  keine Prozesse und keine Ring-3-Trennung; die Programme sind Teil des
  Kernels und werden von der Ereignisschleife der Oberfläche angesteuert –
  das Modell klassischer Heimcomputer-Systeme.
- **Kein HTTPS.** Der Browser spricht HTTP. Für HTTPS wäre eine vollständige
  TLS-Umsetzung mit Zertifikatsprüfung nötig; ein großer Teil des heutigen
  Webs ist damit nicht erreichbar.
- **Der Browser ist ein Textbrowser.** Er versteht Überschriften, Absätze,
  Listen, Verweise, Trennlinien und fetten Text. Kein CSS, kein JavaScript,
  keine Bilder – Bilder erscheinen als Platzhalter.
- **Laden blockiert die Oberfläche.** Ohne Nebenläufigkeit steht das Bild,
  während eine Seite geholt wird; der Zustand „Lade …" wird vorher gezeichnet.
- **TCP ohne Überlastregelung.** Verbindungsaufbau, Bestätigungen und
  erneute Versuche sind da; Fenstersteuerung und das Sortieren
  außerhalb der Reihenfolge eingetroffener Segmente nicht.
- **Das Herunterfahren** nutzt die bekannten Kurzwege der Emulatoren statt
  eines ACPI-Interpreters.

## Aufbau des Quelltexts

```
kernel/
  include/            Öffentliche Header aller Bausteine
  linker.ld           Speicheraufteilung des Kernels
  src/
    main.c            Startreihenfolge des Systems
    arch/             GDT, IDT, Interrupt-Stubs, PIC, PIT, Bootloader, Abschalten
    mm/               Seitenverwaltung und Heap
    drivers/          Framebuffer, PS/2, Tastatur, Maus, RTC, UART,
                      PCI, AHCI, ATA, Blockgeräte, e1000
    fs/               Dateibaum, FAT32, Startbestand des RAM-Teils
    net/              Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP
    gui/              Grafik, Schrift, Symbole, Fenstersystem, Desktop, Bedienelemente
    apps/             Dateimanager, Browser, HTML-Leser, Editor, Konsole,
                      Systeminformation, Dialoge
boot/limine.conf      Bootloader-Eintrag
scripts/              Bootloader holen, Schrift erzeugen, QEMU starten
```

Die Schrift in `kernel/src/gui/font_data.c` ist erzeugt und eingecheckt;
`python3 scripts/gen_font.py` baut sie bei Bedarf neu (benötigt Pillow).

## Fehlersuche

Der Kernel schreibt seine Meldungen auf die serielle Schnittstelle, nicht auf
den Bildschirm – dort läuft die Oberfläche. In QEMU erscheinen sie dank
`-serial stdio` direkt im Terminal:

```
=====================================
  RetroOS 1.0 - Systemstart
=====================================
Bildschirm  : 1280x800, 32 Bit
PMM         : 509 MiB verwaltet, 509 MiB frei
Heap        : 260 KiB bereit
PCI         : 6 Geraete gefunden
  00:31.2  8086:2922  SATA-Controller (AHCI)
Datentraeger: sata0 - QEMU HARDDISK, 128 MiB
Maus        : Typ 3, 4-Byte-Pakete
Dateisystem : 17 Eintraege, 1370 Bytes
Datentraeger: RETROOS eingehaengt unter /Festplatte (126 MiB frei)
Netzwerk    : Intel 82540EM, 52:54:00:12:34:56
Netzwerk    : 10.0.2.15, Gateway 10.0.2.2, DNS 10.0.2.3
Oberflaeche : bereit
```

Eine CPU-Ausnahme hält das System an und gibt Vektor, Fehlercode, `RIP`,
`RSP` und `CR2` aus.

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
