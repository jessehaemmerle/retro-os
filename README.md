# RetroOS

Ein vollständig eigenes Betriebssystem für x86-64-Rechner – mit eigenem
64-Bit-Kernel, präemptivem Scheduler, Prozessen in Ring 3, eigenen
Treibern, eigenem Fenstersystem, einem dauerhaften Dateisystem auf der
Festplatte, einem TCP/IP-Stapel mit TLS 1.3 und einem Browser, der HTML,
CSS, Bilder und JavaScript versteht.

Unter der Haube ist RetroOS auf heutiger Hardware zu Hause: USB-Tastatur,
-Maus und -Sticks am xHCI-Controller, auch hinter einem Verteiler,
NVMe-SSDs und SATA-Platten, GPT-Partitionen, APIC und MSI statt des
Interruptcontrollers von 1976. Retro ist allein die Oberfläche – so soll
es auch sein.

Kein Linux-Unterbau, keine libc, kein fremdes GUI-Toolkit, keine
Netzwerk- oder Kryptobibliothek, keine Browser-Engine. Übernommen wurde
allein der Bootloader
([Limine](https://github.com/limine-bootloader/limine)), der den Kernel im
Long Mode startet und einen linearen Framebuffer bereitstellt.

![RetroOS: Dateimanager auf der Festplatte, Browser mit einer Seite aus dem Netz](docs/screenshot.png)

```
┌──────────────────────────────────────────────────────────────────────┐
│  Desktop · Taskleiste · Startmenü                                    │
│  Dateimanager · Browser · Editor · Konsole · Info · Installieren     │
├──────────────────────────────────────────────────────────────────────┤
│  Fenstersystem (Fenster, Menüs, Dialoge, Bedienelemente)             │
├───────────────────────────────┬──────────────────────────────────────┤
│  Grafik: Backbuffer, Schrift  │  Browser: Dokumentbaum · CSS ·       │
│  Bilder: PNG JPEG GIF BMP     │  Umbruch · JavaScript                │
│  Inflate (DEFLATE)            ├──────────────────────────────────────┤
├───────────────────────────────┤  HTTP/1.1 · TLS 1.3                  │
│  Installation auf Festplatte  │  Kryptografie: SHA-2 · HMAC · HKDF · │
│  Dateibaum: RAM + FAT32       │  ChaCha20 · AES-GCM · X25519 · RSA · │
├───────────────────────────────┤  ECDSA · X.509 · 152 Wurzeln         │
│  Ring 3: Fenster · Sockets    ├──────────────────────────────────────┤
│  Scheduler auf mehreren Kernen│  TCP · UDP · DHCP · DNS · ICMP       │
├───────────────────────────────┤  IPv4 · ARP · Ethernet               │
│  Seitenverwaltung · Heap      │                                      │
├───────────────────────────────┼──────────────────────────────────────┤
│  Partitionen: GPT · MBR       │  Netzwerkkarten: virtio · Intel igb  │
│  Blockgeräte: NVMe · AHCI ·   │  e1000e · e1000 · Realtek 8169/8139  │
│  ATA · USB-Speicher           ├──────────────────────────────────────┤
│                               │  USB: xHCI · Verteiler · HID-Tastatur│
│                               │  und -Maus · Massenspeicher (SCSI)   │
├───────────────────────────────┴──────────────────────────────────────┤
│  PS/2 (falls vorhanden) · RTC · UART · PCIe · ACPI                   │
├──────────────────────────────────────────────────────────────────────┤
│  Unterbrechungen: lokaler APIC · IOAPIC · MSI/MSI-X · 8259A ersatz-  │
│  weise · Zeitgeber des APIC, sonst PIT                               │
├──────────────────────────────────────────────────────────────────────┤
│  Kern: GDT · IDT · TSS je Kern · Systemaufrufe · Spinlocks           │
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

> Auf echter Hardware kommt RetroOS mit USB-Tastatur und -Maus an einem
> xHCI-Controller zurecht – also mit dem, was ein heutiges Notebook
> mitbringt. Ist noch ein PS/2-Anschluss da, wird er ebenfalls benutzt;
> fehlt er, sagen das die ACPI-Tabellen und RetroOS klopft gar nicht erst
> an. Als Datenträger dienen NVMe-SSDs, SATA-Platten am AHCI-Controller
> oder ältere IDE-Laufwerke, jeweils mit GPT- oder MBR-Partitionstabelle.
> Beim Netzwerk sucht sich RetroOS aus, was auf dem PCI-Bus steckt:
> virtio-net in virtuellen Maschinen, Intel igb (I210, I350, 82576),
> Intel e1000e (82574L bis I219), die älteren 8254x sowie Realtek
> RTL8169/8168/8111 und RTL8139.

## Auf die Festplatte installieren

Vom Stick oder von der CD läuft RetroOS auch ohne Installation – die
Dateien liegen dann aber nur im Arbeitsspeicher. Wer das System behalten
will, startet **Installieren** auf dem Desktop (oder `installieren` in
der Konsole):

| Schritt | Was passiert |
| --- | --- |
| Ziel wählen | Alle Datenträger, die groß genug sind; der Startdatenträger selbst wird abgelehnt |
| Bestätigen | Zeigt die geplante Aufteilung – danach ist die Platte leer |
| Schreiben | GUID-Tabelle, zwei FAT32-Abschnitte, Bootloader, Kernel, Startsektor |

Angelegt wird der Aufbau, den ein heutiger Rechner erwartet:

```
Sektor 0        Schutzeintrag, damit alte Werkzeuge stillhalten
Sektor 1–33     die GUID-Tabelle
Sektor 34–…     zweiter Teil des Bootloaders (nur BIOS-Rechner lesen ihn)
Sektor 2048     EFI-Abschnitt, 64 MiB: BOOTX64.EFI, Kernel, limine.conf
danach          Ablage, der ganze Rest – hängt als /Festplatte im Baum
```

Ein UEFI-Rechner findet `\EFI\BOOT\BOOTX64.EFI` von selbst; ein
BIOS-Rechner liest den ersten Sektor. Beide Wege landen bei derselben
Kerneldatei. Danach kann das Startmedium weg: Der Rechner bootet von der
Platte, und was unter `/Festplatte` angelegt wird, liegt beim nächsten
Start wieder da.

Mindestgröße sind 72 MiB. Auf kleinen Platten fällt der EFI-Abschnitt
kleiner aus, denn FAT32 braucht mindestens 65525 Cluster.

## Bedienung

| Aktion | Wirkung |
| --- | --- |
| Doppelklick auf ein Desktop-Symbol | Programm starten |
| Start-Knopf unten links | Menü mit allen Programmen |
| Titelleiste ziehen | Fenster verschieben |
| Ecke unten rechts ziehen | Fenster vergrößern |
| `_` / `X` in der Titelleiste | Fenster ablegen / schließen |
| Klick in der Taskleiste | Fenster holen oder ablegen |
| Startmenü → Sperren | Bildschirm sperren, Fenster bleiben stehen |

**Dateimanager:** Doppelklick öffnet Ordner und Dateien, die rechte Maustaste
öffnet das Kontextmenü. Über die Tastatur: Pfeiltasten wählen aus, `Eingabe`
öffnet, `Rücktaste` geht eine Ebene höher, `F2` benennt um, `Entf` löscht,
`F5` aktualisiert. Der Zweig `/Festplatte` liegt auf dem Datenträger, alles
andere im Arbeitsspeicher.

**Browser:** Adresse eintippen und `Eingabe`. Verweise, Knöpfe und
Eingabefelder sind bedienbar, `Rücktaste` geht zurück, `F5` lädt neu. Der
grüne Pfeil in der Leiste lädt die Adresse als Datei herunter, statt sie
anzuzeigen; was der Browser ohnehin nicht darstellen kann – ein Archiv,
ein Programm, ein PDF –, landet von selbst dort. Alles Heruntergeladene
liegt in `Downloads`, auf der Festplatte, wenn eine eingehängt ist.

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

**Tabelle:** ein Gitter aus 26 Spalten und 100 Zeilen. Wer tippt,
schreibt in die Zelle unter dem Rahmen; `Eingabe` geht eine Zeile
hinunter, `Tabulator` eine Spalte weiter, `F2` hängt an den vorhandenen
Inhalt an. Ein Gleichheitszeichen macht aus der Eingabe eine Formel:

| Formel | Ergebnis |
| --- | --- |
| `=B2*C2` | Bezüge auf andere Zellen, `$A$1` geht auch |
| `=SUMME(D2:D9)` | über einen Bereich, ebenso `MITTELWERT`, `MIN`, `MAX`, `ANZAHL` |
| `=RUNDEN(A1; 2)` | dazu `ABS`, `WURZEL` und `WENN(Bedingung; dann; sonst)` |
| `=A1>10` | Vergleiche liefern 1 oder 0 |

Gerechnet wird in Festkomma mit vier Nachkommastellen – der Kernel hat
keine Gleitkommaeinheit. Ein Kreisbezug endet nicht in einer Schleife,
sondern in `#KREIS`; die übrigen Fehler heißen `#FORMEL`, `#BEZUG`,
`#DIV/0`, `#NAME` und `#WERT`. Gespeichert wird als CSV mit Strichpunkten,
Formeln bleiben dabei Formeln – die Auszeichnung *fett* allerdings nicht,
die kennt CSV nicht.

**Schreiben:** Absätze mit Formatvorlagen – Textkörper, zwei
Überschriften, Aufzählung, Zitat –, dazu **fett** und unterstrichen an
einzelnen Zeichen und Ausrichtung je Absatz. Ausgewählt wird mit der Maus
oder mit `Umschalt` und den Pfeiltasten, `Strg`+`B` und `Strg`+`U` zeichnen
die Auswahl aus. Kursiv fehlt: Der Zeichensatz hat nur einen aufrechten
und einen fetten Schnitt. Gespeichert wird HTML, und zwar solches, das der
Browser dieses Systems unverändert anzeigt.

**Vortrag:** Folien mit Titel und Punkten, links die Übersicht, rechts die
gewählte in Vierdrittel-Format. Getippt wird unmittelbar in der Folie;
`Eingabe` legt eine Zeile an, `Tabulator` springt weiter, `Strg`+`Auf`/`Ab`
verschiebt die Folie in der Reihenfolge. Drei Anordnungen: Titelfolie,
Aufzählung, Zitat. `F5` führt vor – über den ganzen Bildschirm, ohne
Rahmen und ohne Taskleiste; Pfeiltasten und Leertaste blättern, `Escape`
kommt zurück. Passt eine Folie nicht, wird die Schrift kleiner statt der
Text abgeschnitten. Das Dateiformat ist Text und in drei Zeilen erklärt:
`#` beginnt eine Folie, `!` nennt ihre Anordnung, `-` eine Zeile.

**Programmieren:** ein Editor, der ausführt, was in ihm steht.
Schlüsselwörter, Zeichenketten, Zahlen und Anmerkungen bekommen Farbe,
links stehen Zeilennummern, unten die Ausgabe. `F5` führt aus, `Strg`+`S`
speichert. Gerechnet wird mit demselben JavaScript-Deuter, mit dem der
Browser die Skripte einer Seite abarbeitet; `console.log()` schreibt ins
untere Feld, ein Fehler erscheint dort rot mit seiner Zeile. Eine
`.js`-Datei im Dateimanager öffnet sich hier statt im Editor.

**Systemmonitor:** Drei Ansichten hinter drei Reitern. *Programme* zeigt,
was in Ring 3 läuft – mit Nummer, Benutzer, belegtem Speicher und dem
Anteil an der Rechenzeit; *Programm beenden* schießt das Ausgewählte ab,
bei einem fremden Programm allerdings nur als Verwalter. *Threads* zeigt
dasselbe für den Kern samt Vorrang und dem Prozessorkern, auf dem der
Thread zuletzt lief. *System* fasst Speicher, Heap, Dateibaum und
Protokoll zusammen. Die Anteile lassen sich nicht ablesen, nur messen:
Der Monitor merkt sich die Zähler des Schedulers und rechnet jede Sekunde
den Zuwachs aus – vor der ersten Messung steht darum ein Strich und keine
Null.

**Protokoll:** Zeigt den Ring der letzten Meldungen. Die Leiste oben ist
zugleich Filter und Zähler: Wie viele Warnungen es gibt, sieht man, bevor
man danach sucht. Die Liste hängt am Ende, solange man sie nicht anfasst;
wer nach oben scrollt, hält sie an, bis er wieder unten ist. *Speichern*
legt sie als Textdatei ins Heimatverzeichnis, *Leeren* darf nur ein
Verwalter.

**Aufgaben:** Oben eintippen, `Eingabe` drücken – fertig. Ein Klick auf
das Kästchen setzt den Haken, die Knöpfe rechts ändern Wichtigkeit und
Termin oder räumen Erledigtes weg. Die Liste steht in
`Aufgaben.txt` im Heimatverzeichnis und ist so gebaut, dass man sie auch
im Editor bearbeiten kann; eine nackte Zeile Text genügt als Aufgabe.
Sortiert wird nach dem, was als Nächstes ansteht: Offenes vor Erledigtem,
Termine vor Terminlosem, früher vor später, dann die Wichtigkeit.
Überschrittene Termine stehen rot.

**Benutzer:** Legt Konten an, setzt Passwörter, vergibt und nimmt
Verwalterrechte und sperrt Anmeldungen. Ohne Verwalterrecht sieht man
dieselbe Liste, kann darin aber nur das eigene Passwort ändern – wer sonst
noch an diesem Rechner arbeitet, ist keine Geheimsache, fremde Konten
anzufassen schon. Geändert wird zunächst nur im Speicher; erst
*Speichern* schreibt `/Festplatte/benutzer.conf`. Im Startmenü stehen
darunter **Sperren** (die Sitzung bleibt stehen, nur der Bildschirm ist
zu), **Benutzer wechseln** und **Abmelden**. Im Dateimanager zeigt
*Eigenschaften* im Kontextmenü Eigentümer und Rechte und lässt sie ändern,
wenn einem der Eintrag gehört.

**Papierkorb:** Gelöschtes verschwindet nicht sofort, sondern wandert nach
`/Papierkorb` – aus dem Dateimanager, aus der Konsole, von der Festplatte
wie aus dem Arbeitsspeicher. Der Korb merkt sich, wo jedes Stück herkam;
*Wiederherstellen* legt es dorthin zurück und legt den Ordner neu an, falls
er inzwischen fehlt. Endgültig wird es erst beim zweiten Löschen oder beim
Leeren. Der Korb liegt im Arbeitsspeicher: Was darin liegt, ist nach einem
Neustart weg.

**Fenster:** Neben Schließen und Ablegen gibt es jetzt Maximieren – als
Knopf, per Doppelklick auf die Titelleiste oder mit `Alt`+`Eingabe`.
`Alt`+`←` und `Alt`+`→` docken ein Fenster an die linke oder rechte
Hälfte an, `Alt`+`Tab` holt reihum das nächste nach vorne, `Alt`+`F4`
schließt. Ein maximiertes Fenster lässt sich nicht verschieben – es
würde beim ersten Zucken vom Bildschirm rutschen.

**Rechner:** Ziffernblock und Tastatur tun dasselbe. Gerechnet wird in
Festkomma mit sechs Nachkommastellen, also gibt `0,1 + 0,2` genau `0,3`.
Und wie jeder Taschenrechner kennt er keinen Vorrang: `2 + 3 * 4 =` sind
zwanzig. Wer vierzehn will, tippt `rechne` in die Konsole.

**Bilder:** Doppelklick auf ein PNG, JPEG, GIF oder BMP im Dateimanager.
Das Bild wird eingepasst, das Mausrad und `+`/`−` vergrößern, `0` passt
wieder ein, `1` zeigt in Originalgröße. Die Pfeiltasten blättern durch
die übrigen Bilder desselben Ordners.

**Bildschirmfoto:** Im Startmenü oder mit `foto` in der Konsole. Die
Aufnahme landet als PNG im eigenen Bilder-Ordner.

**Hintergrund:** Neben den fünf Verläufen lässt sich ein eigenes Bild
einsetzen – in den Einstellungen unter *Hintergrundbild*, das die Bilder
aus `/Medien` und aus dem eigenen `Bilder`-Ordner durchblättert, oder im
Dateimanager mit der rechten Maustaste auf ein Bild und *Als Hintergrund*.
PNG, JPEG, GIF und BMP; das Bild wird füllend skaliert und mittig
beschnitten. Der Sperrbildschirm zeigt dasselbe Bild abgedunkelt.

**Konsole:** `hilfe` zeigt alle Befehle nach Gebiet geordnet, `hilfe <befehl>`
und `man <befehl>` erklären einen einzelnen. Die Pfeiltasten holen die letzten
zwanzig Zeilen zurück. Fast jeder Befehl hat neben dem deutschen Namen den
gewohnten englischen als Zweitnamen (`kopiere`/`cp`, `suche`/`grep`). Neben
`ls`, `cd`, `cat`, `mkdir`, `touch`, `schreib`, `rm` und `edit` gibt es:

| Befehl | Wirkung |
| --- | --- |
| `kopiere <von> <nach>` / `cp` | Datei oder ganzen Ordner kopieren |
| `verschiebe <von> <nach>` / `mv` | verschieben oder umbenennen |
| `kopf [-n] <datei>` / `head` | die ersten Zeilen zeigen |
| `ende [-n] <datei>` / `tail` | die letzten Zeilen zeigen |
| `zaehle <datei>` / `wc` | Zeilen, Wörter und Zeichen zählen |
| `sortiere [-r] <datei>` / `sort` | Zeilen sortiert ausgeben |
| `vergleiche <a> <b>` / `diff` | zwei Dateien Zeile für Zeile vergleichen |
| `hex <datei> [anzahl]` | Bytes als Hexdump mit Klartextspalte |
| `suche <text> [pfad]` / `grep` | Dateien nach Text durchsuchen |
| `finde [pfad] <muster>` / `find` | Dateien nach Namensmuster suchen (`*`, `?`) |
| `baum [pfad]` / `tree` | Ordner als Baum zeichnen |
| `groesse [pfad]` / `du` | Platzverbrauch, nach Größe geordnet |
| `info <pfad>` / `stat` | Art, Größe, Rechte, Eigentümer, Änderungszeit |
| `pruefsumme <datei>` / `sha256` | SHA-256 über den Dateiinhalt |
| `wo <name>` / `which` | zeigt, ob ein Name eingebaut ist oder als Programm vorliegt |
| `beende <nummer>` / `kill` | einen Prozess beenden (fremde nur als Verwalter) |
| `warte <ms>` / `sleep` | eine Weile nichts tun |
| `kalender [monat] [jahr]` / `cal` | Monatskalender, der heutige Tag in Klammern |
| `rechne <ausdruck>` / `expr` | rechnen mit `+ - * / %` und Klammern |
| `verlauf` / `history` | die zuletzt eingegebenen Zeilen |
| `man <befehl>` | ausführliche Erklärung eines Befehls |
| `starte <programm>` | ein Ring-3-Programm ausführen |
| `programme` | die eingebauten Programme auflisten |
| `threads` | laufende Threads mit Zustand und Rechenzeit |
| `platte` | Laufwerke und eingehängtes Dateisystem |
| `usb` | Geräte am USB-Bus mit Klasse und Geschwindigkeit |
| `formatieren wirklich [Name]` | Datenträger neu mit FAT32 formatieren |
| `netz` | IP-Adresse, Gateway, Namensserver, Paketzähler |
| `ping <ziel>` | Erreichbarkeit prüfen |
| `aufloesen <name>` | Namen in eine Adresse wandeln |
| `holen <adresse> [datei]` | Seite abrufen und wahlweise speichern |
| `starte server [port] [wurzel]` | Webserver – die Ablage vom Wirtsrechner aus durchsehen |
| `prozesse` | laufende Programme mit Verwandtschaft und geteiltem Speicher |
| `papierkorb [zurueck <n>\|leeren]` | Geloeschtes ansehen, zurueckholen, endgueltig entfernen |
| `schrift [name]` | Schriftarten mit Lizenz auflisten oder sofort umschalten |
| `sprache [de\|en]` / `lang` | Sprache und Tastaturbelegung zeigen oder umschalten |
| `bildschirm [1280x800\|2x\|auto]` / `display` | Auflösung und Vergrößerung zeigen oder setzen |
| `wer` | angemeldeter Benutzer, seine Nummer, sein Heim und seine Gruppen |
| `gruppen` | Gruppen mit Nummer und Mitgliedern |
| `benutzer [neu\|loeschen\|passwort\|verwalter <name> [wert]]` | Konten zeigen und verwalten |
| `rechte <datei> [modus]` | Rechte zeigen oder setzen (`750` oder `rwxr-x---`) |
| `besitzer <datei> [name[:gruppe]]` | Eigentümer zeigen oder setzen |
| `sperren` | Bildschirm sperren |
| `maus` / `mouse` | Zeigegerät: was erkannt wurde und ob Bytes ankommen |
| `foto` / `screenshot` | Bildschirmfoto als PNG im eigenen Bilder-Ordner |
| `firewall [an\|aus\|standard\|regel\|weg\|leeren\|speichern]` | Paketfilter zeigen und regeln |
| `pruefspur [alle\|abgewiesen\|speichern]` | Sicherheitsereignisse ansehen |
| `kaefig [<profil> <programm> [text]]` | Profile zeigen oder ein Programm eingesperrt starten |
| `protokoll [alle\|warnung\|fehler\|speichern\|leeren]` | Systemprotokoll ansehen, sichern, leeren |
| `aufgaben [neu <text>\|fertig <n>\|weg <n>\|wichtig <n> <stufe>\|termin <n> <datum>]` | Aufgabenliste führen |
| `neustart` / `leeren` | Rechner neu starten, Bildschirm leeren |

Über das Startmenü lässt sich der Rechner auch abschalten – über ACPI,
also so, wie es ein Betriebssystem tut.

![Systeminformation: APIC, drei Datenträger und USB-Geräte hinter einem Verteiler – auf einem Rechner ohne PS/2](docs/systeminfo.png)

## Was drinsteckt

| Bereich | Umsetzung |
| --- | --- |
| **Start** | Limine-Protokoll, Higher-Half-Kernel bei `0xffffffff80000000` |
| **CPU** | eigene GDT mit TSS, IDT mit 48 Vektoren, Ausnahmebehandlung mit Panik-Ausgabe |
| **Interrupts** | lokaler APIC, IOAPIC samt Umlegungen aus der MADT, MSI und MSI-X für PCIe; 8259A als Rückfallebene |
| **Systemtakt** | Zeitgeber des lokalen APIC mit 1000 Hz, gegen den PIT ausgezählt; sonst der PIT selbst |
| **Scheduler** | präemptiv, Zeitscheiben von 20 ms, drei Prioritäten, Schlafen und Warten |
| **Prozesse** | ELF64-Lader, eigener Adressraum je Prozess, Ring 3, Systemaufrufe über `SYSCALL`/`SYSRET`; Abspalten mit Kopie beim Schreiben, Warten auf Kinder, Prozessgruppen an einer Konsole |
| **Speicher** | Bitmap-Allokator für Seitenrahmen, Besitzerzähler je Seite, vierstufige Seitentabellen, Heap mit Blockverschmelzung |
| **Busse** | PCI und PCIe über Konfigurationsmechanismus 1, 64-Bit-Adressbereiche, Fähigkeitenliste |
| **Datenträger** | NVMe über PCIe mit eigenen Warteschlangen, AHCI (SATA, DMA), ATA-PIO, USB-Speicher über SCSI |
| **Partitionen** | GPT samt Sicherungstabelle, MBR mit erweiterten Abschnitten, roher Datenträger |
| **Installation** | Schreibt eine eigene GUID-Tabelle, formatiert EFI-Abschnitt und Ablage, kopiert Bootloader und Kernel und setzt den Startsektor für BIOS-Rechner |
| **Dateisystem** | FAT32 mit langen Dateinamen – lesen, schreiben, anlegen, umbenennen, löschen, formatieren |
| **USB** | xHCI-Controller: Befehls-, Ereignis- und Übertragungsringe, Geräteaufzählung über mehrere Verteiler hinweg, Unterbrechungs- und Massenendpunkte |
| **Eingabe** | PS/2-Tastatur und -Maus am 8042 samt Erkennung, ob an Port 2 überhaupt eine hängt; USB-Tastatur und -Maus im Boot-Protokoll; vier Belegungen (de/us/uk/ch) inkl. AltGr |
| **Grafik** | 32-Bit-Framebuffer, Backbuffer, Clipping, Verläufe, 3D-Kanten, frei skalierbare Bitmapschrift |
| **Bilder** | eigener DEFLATE-Entpacker, PNG (alle Farbtypen, Adam7), JPEG (Grundverfahren), GIF, BMP |
| **Netzwerkkarten** | virtio-net (alte und neue Bauform), Intel igb, e1000e und 8254x, Realtek RTL8169/8168/8111 und RTL8139 – hinter einer gemeinsamen Schnittstelle, der erste passende Treiber bekommt die Karte |
| **Netzwerk** | Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP/1.1 |
| **TCP** | Fenstersteuerung, Umsortierung, langsamer Start, Überlastvermeidung, schnelle Wiederholung; als Client *und* als Server mit Warteschlange, geschlossene Ports antworten mit RST |
| **Kryptografie** | SHA-256/384/512, HMAC, HKDF, ChaCha20-Poly1305, AES-128/256-GCM, X25519, RSA, ECDSA P-256 |
| **TLS** | TLS 1.3 als Client, X.509-Ketten gegen 152 eingebaute Wurzelzertifikate |
| **Browser** | Dokumentbaum, CSS-Kaskade, Kastenmodell, Bilder, JavaScript |
| **Energie** | ACPI: RSDP, XSDT, FADT, DSDT mit `_S5_`-Auswertung zum Abschalten |
| **Oberfläche** | Fensterstapel, Fokus, Verschieben, Größe ändern, Taskleiste, Popup-Menüs, Dialoge |
| **Protokoll** | Ring über die letzten 512 Meldungen mit Zeit, Dringlichkeit und Herkunft; alles, was `kprintf` schreibt, landet zeilenweise darin – der ganze Startvorgang ist danach im Fenster nachlesbar |
| **Systemmonitor** | Programme, Threads und Maschine in drei Ansichten; Rechenzeitanteile werden jede Sekunde gemessen, Speicher je Programm gezählt, fremde Programme beendet nur ein Verwalter |
| **Aufgaben** | Liste je Benutzer mit Haken, Wichtigkeit und Termin, sortiert nach dem, was als Nächstes ansteht; als Textdatei im Heimatverzeichnis |
| **IPC** | Röhren zwischen Eltern und Kind (Ringpuffer im Kern, werden beim Abspalten vererbt) und geteilter Speicher (derselbe Seitenrahmen in mehreren Adressräumen, `PTE_SHARED` hält ihn aus Kopie-beim-Schreiben und Abräumen heraus) |
| **Sprache** | 46 Prüfungen: Sortierung und Lückenlosigkeit der Tabelle, jeder Eintrag wird auch gefunden, gleiche Platzhalter auf beiden Seiten, Umschalten und Rückfall aufs Deutsche |
| **Rechner** | 56 Prüfungen: Grundrechnen und Ketten, Festkomma und Runden, jeder Übergang des Zustandsautomaten, Teilen durch null, Überlauf, Prozent und Wurzel |
| **PNG schreiben** | 40 Prüfungen: geschriebene Bilder mit dem eigenen Leser zurückgelesen, Punkt für Punkt – dazu Aufbau, Prüfsumme und die Blockgrenze bei 65535 Bytes |
| **Bildschirm** | 152 Prüfungen: Zerlegen von `1280x800` samt Grenzfällen, wie weit sich vergrößern lässt, Grafikspeicher, Fenster zurück in einen kleiner gewordenen Schirm |
| **Konsole** | 104 Prüfungen: Namensmuster samt Rücksetzen, Rechenausdrücke mit Vorrang und Grenzfällen, Wochentage nach Zeller, Kalenderspalten, Vollständigkeit der Befehlstabelle |
| **Paketfilter** | Regeltabelle je Richtung mit Protokoll, Adresse samt Maske und Portbereich; erste passende Regel entscheidet, sonst die Grundeinstellung. Hängt in `ip_receive` und `ip_send_via` – kein Protokoll darüber weiß davon |
| **Rollen** | Sechs Fähigkeiten (Konten, Netz, Platte, Protokoll, Strom, Einstellungen) statt „Verwalter ja/nein"; eine Rolle ist ein Name für eine Menge davon |
| **Käfig** | Pro Programm ein Profil: erlaubte Syscall-Gruppen, ein Wurzelpfad im Dateibaum, eine Speichergrenze und was bei einem Verstoß geschieht. Lässt sich nur enger machen – auch vom Programm selbst, per `sys_sandbox()` |
| **Prüfspur** | Wer hat was woran versucht und mit welchem Ausgang – Anmeldungen, abgewiesene Zugriffe, gebrauchte Rechte, Konten- und Filteränderungen; wird fortgeschrieben, nicht ersetzt, und lässt sich nicht leeren |
| **Benutzer** | Mehrere Konten mit Nummer, Gruppe, Heimatverzeichnis und Verwalterrecht; das Passwort liegt als 4096-fach wiederholter HMAC-SHA256 über einem eigenen Salz in `/Festplatte/benutzer.conf` |
| **Rechte** | Eigentümer, Gruppe und neun Bits je Eintrag, dazu das Klebebit; geprüft beim Nachschlagen, Aufzählen, Lesen, Schreiben, Anlegen, Umbenennen und Löschen |
| **Anmeldung** | Anmeldebildschirm beim Start, Sperren, Abmelden und Benutzerwechsel; nach drei Fehlversuchen eine Zwangspause |
| **Schriften** | Zehn freie Monospace-Schriften in der 8×16-Zelle – DejaVu, Liberation, JetBrains, IBM Plex, Fira, Source Code Pro, Inconsolata, Ubuntu, Unifont und VT323; umschaltbar im laufenden Betrieb |
| **Symbole** | Lucide (ISC) in 16 und 32 Punkt, aus den SVG-Vorlagen erzeugt und mit dunkler Umrandung versehen, damit sie auf hellem wie dunklem Grund lesen |
| **Papierkorb** | Gelöschtes wandert nach `/Papierkorb` und merkt sich, wo es herkam; Wiederherstellen, endgültiges Löschen, Leeren |
| **Downloads** | Was der Browser nicht anzeigen kann, legt er unter `Downloads` ab – ebenso alles, was der Knopf in der Leiste holt |
| **Kerne** | Alle Kerne des Rechners werden gestartet; eigene GDT, TSS und Zeitgeber je Kern, gemeinsame Daten unter Warteschlangensperren |
| **Ring 3** | Eigene Fenster und TCP-Verbindungen über Systemaufrufe – ein Benutzerprogramm kann zeichnen, ins Netz und selbst zuhören |
| **Tabellenkalkulation** | Gitter, Festkommarechnung, Formeln mit Bereichen und Funktionen, Erkennung von Kreisbezügen, CSV |
| **Textverarbeitung** | Absatzformate, fett und unterstrichen je Zeichen, Ausrichtung, Umbruch, Speichern und Laden als HTML |
| **Präsentation** | Folien mit drei Anordnungen, Übersichtsleiste, Vollbild ohne Fensterrahmen, Schrift passt sich der Folie an |
| **Webserver** | `starte server` liefert die Ablage über HTTP aus; ein Ring-3-Programm, das lauscht, annimmt und je Verbindung ein Kind abspaltet |
| **Fenster** | Maximieren über Knopf, Doppelklick oder `Alt`+`Eingabe`; Andocken an eine Bildschirmhälfte mit `Alt`+`←`/`→`; `Alt`+`Tab` reihum, `Alt`+`F4` schließt |
| **Zubehör** | Rechner mit Festkomma-Arithmetik, Bildbetrachter für PNG/JPEG/GIF/BMP, Bildschirmfoto als PNG |
| **Bildschirm** | Auflösung zur Laufzeit umschaltbar, wo es die Bochs-Schnittstelle gibt (QEMU, VirtualBox, Bochs); ganzzahlige Vergrößerung 1×–4× überall, automatisch nach Bildschirmgröße |
| **Sprache** | Deutsch und Englisch, systemweit und im laufenden Betrieb umschaltbar; 741 Einträge, aus `data/sprache-en.txt` erzeugt |
| **Einstellungen** | Sprache, Tastaturbelegung (de/us/uk/ch), Zeitzone, Rechnername, Hintergrund samt eigenem Bild und Schriftart in `/Festplatte/retroos.conf` |
| **Zwischenablage** | Kopieren und Einfügen zwischen Editor, Konsole und Browser |
| **Programme** | Dateimanager, Browser, Texteditor, Konsole, Systeminformation, Systemmonitor, Protokoll, Aufgaben, Installation, Einstellungen, Benutzer, Papierkorb, Tabelle, Schreiben, Vortrag, Programmieren, elf Ring-3-Programme |

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

### Alt und neu nebeneinander

RetroOS sucht sich zur Laufzeit aus, welchen Weg es nimmt – ohne dass
dafür zwei Fassungen gebaut werden müssten:

| Aufgabe | bevorzugt | Rückfallebene |
| --- | --- | --- |
| Unterbrechungen | lokaler APIC und IOAPIC | 8259A-PIC |
| Unterbrechung eines PCIe-Geräts | MSI oder MSI-X | Leitung im IOAPIC bzw. PIC |
| Systemtakt | Zeitgeber des lokalen APIC | PIT |
| Eingabe | USB-HID am xHCI | PS/2 am 8042 |
| Datenträger | NVMe | AHCI, dann ATA-PIO, dann USB-Speicher |
| Aufteilung | GPT | MBR, sonst roher Datenträger |

Ob es einen PS/2-Anschluss gibt, verrät die FADT in ihren
Boot-Kennzeichen; wo die Angabe fehlt, klopft RetroOS vorsichtig an und
wartet mit begrenzter Geduld. Das ist kein Schönheitsfehler, sondern
notwendig: An einem Rechner ohne 8042 liest man am Statusport lauter
Einsen, und ein Treiber, der darauf wartet, dass sie verschwinden,
wartet für immer.

Der Zeitgeber des lokalen APIC läuft mit einem Takt, der nirgends
verzeichnet ist. Er wird deshalb beim Start einmal gegen Kanal 2 des PIT
ausgezählt – die einzige Stelle, an der der alte Baustein noch gebraucht
wird, und selbst die nur zum Nachmessen.

Am USB-Bus hängt selten alles unmittelbar an der Wurzel: In einem
Notebook sitzt zwischen Chipsatz und Tastatur meist noch ein Verteiler.
Damit der Controller ein Gerät dahinter überhaupt ansprechen kann,
braucht er eine Wegbeschreibung – vier Bit je Ebene, bis zu fünf Ebenen
tief. RetroOS zählt deshalb rekursiv auf: Wird ein Verteiler gefunden,
werden dessen Anschlüsse auf demselben Weg abgesucht.

### Zehn Schriften in einer Zelle

Jedes Zeichen sitzt in einem festen Kasten von 8×16 Pixeln. Das ist keine
Bequemlichkeit, sondern die Grundlage der halben Oberfläche: Konsole,
Editor, Tabelle und Fenster rechnen ihre Spalten und Zeilen aus genau
diesen beiden Zahlen aus. Eine andere Schrift zu wählen heißt darum nicht,
das Layout neu zu vermessen, sondern nur, andere Punkte in dieselbe Zelle
zu setzen – kein Programm muss davon wissen, und der Wechsel kostet eine
Zuweisung.

Die zehn Vorlagen liegen als woff2 unter `third_party/fonts`, auf Latin-1
verkleinert und zusammen gut 130 KB groß. `scripts/gen_font.py` rastert
sie einmal auf dem Entwicklungsrechner; im Kernel steht davon nur noch
eine Tabelle aus Bytes. Umschalten lässt es sich in den Einstellungen –
das Fenster zeichnet sich sofort in der neuen Schrift und ist damit seine
eigene Vorschau – oder in der Konsole mit `schrift`.

### Zwei Sprachen in einem System, das auf Deutsch geschrieben ist

RetroOS ist deutsch bis in die Bezeichner hinein. Eine zweite Sprache
einzuziehen hieß darum nicht, Texte gegen Nummern zu tauschen, sondern
den deutschen Text selbst zum Schlüssel zu machen: `tr("Einstellungen")`
liefert „Settings", wenn Englisch eingestellt ist, und sonst genau das,
was hineinging.

Das hat zwei Vorteile, die den einen Nachteil aufwiegen. Der Quelltext
bleibt lesbar – wer ihn liest, sieht den Text, den der Benutzer sieht,
und nicht `STR_SETTINGS_TITLE`. Und ein fehlender Eintrag ist kein
Absturz und kein leeres Feld, sondern eine deutsche Zeile in einem
englischen Fenster: unschön, aber bedienbar. Der Nachteil ist die Suche
bei jedem Aufruf; sie läuft binär über eine sortierte Tabelle und kostet
ein Dutzend Vergleiche, während das Zeichnen derselben Zeile hunderte
Pixel kostet.

**Übersetzt wird beim Zeichnen, nicht beim Anlegen.** Nur so wechselt ein
offenes Fenster die Sprache mit, statt sie bis zum Neustart zu behalten.
Fenstertitel, Menüeinträge, Knopfbeschriftungen und Statuszeilen gehen
ohnehin durch eine Handvoll Funktionen – dort steht das `tr()` ein Mal
und wirkt überall. Umgekehrt bleibt alles deutsch, was in eine **Datei**
geht: Die Wichtigkeit einer Aufgabe steht als `hoch` in der Liste, die
Anordnung einer Folie als `Titelfolie`. Eine Datei, deren Format von der
eingestellten Sprache abhängt, wäre auf dem nächsten Rechner nicht mehr
lesbar. Gelesen wird beides.

Die Tabelle selbst steht nicht im Quelltext, sondern in
`data/sprache-en.txt` – ein Paar je Zeile, durch einen Tabulator
getrennt. `make sprache` sortiert sie und erzeugt daraus `lang_data.c`.
Sortiert, weil die Suche binär ist; und weil eine von Hand eingefügte
Zeile das lautlos zerstören würde, besteht eine Prüfung im Testlauf
darauf. Eine zweite vergleicht die Platzhalter beider Hälften: Aus
„%s von %s belegt" darf nicht „%s in use" werden, sonst liest
`ksnprintf` irgendwann eine Zahl als Zeiger.

Die **Tastatur wandert mit** der Sprache – aber nur, solange sie noch die
ist, die zur alten Sprache gehörte. Wer sich bewusst eine andere Belegung
gesucht hat, behält sie; sonst wäre ein Blick in die englische Oberfläche
jedes Mal der Verlust seiner Umlaute. Zur Auswahl stehen vier Belegungen:
deutsch, amerikanisch, britisch und schweizerisch. Die britische ist
fast die amerikanische, vier Tasten sitzen anders – und das Pfundzeichen
fehlt, weil der eingebaute Zeichensatz nur bis 127 reicht; Umschalt+3
liefert darum das Doppelkreuz.

Nicht übersetzt ist, was auch auf Deutsch niemand liest: die Meldungen
des Kerns auf der seriellen Schnittstelle, die Einträge im Systemprotokoll
und in der Prüfspur. Sie gehören der Fehlersuche und nicht der Bedienung.

### PNG schreiben, ohne einen Packer zu schreiben

Zum Lesen gehört ein vollständiger DEFLATE-Entpacker – der steht seit
den Bildern im Browser da. Zum Schreiben braucht es ihn nicht: DEFLATE
kennt einen Blocktyp, der gar nicht packt, sondern die Bytes
unverändert weiterreicht, und den versteht jeder Leser.

Ein Bildschirmfoto wird damit etwa so groß wie das Bild selbst – 2,9 MB
bei 1280×800. Dafür sind es zweihundert Zeilen statt zweitausend, und
ein Packer, den niemand prüft, wäre die schlechtere Wahl: Ein Fehler
darin fällt erst auf, wenn ein fremdes Programm die Datei nicht mehr
lesen kann.

Genau das ist beinahe passiert. Der Schreiber rechnete die Prüfsumme
jedes Abschnitts von Hand mit `0xFFFFFFFF` an und drehte am Ende noch
einmal die Bits – beides steckt in `crc32_update()` aber schon drin. Der
eigene Leser prüft die Summen nicht und nahm die Dateien anstandslos;
erst die Prüfung, die sie nachrechnet, brachte es heraus. Ein
Bildschirmfoto, das RetroOS anzeigt und sonst niemand, wäre eine
unangenehme Art, das zu erfahren.

Geprüft wird darum zweifach: Bilder werden geschrieben und mit dem
eigenen Leser Punkt für Punkt zurückgelesen – auch an der Blockgrenze
bei 65535 Bytes und mit einem Zeilenabstand, der nicht die Breite ist –,
und die Prüfsumme wird unabhängig nachgerechnet.

### Auflösung wechseln, nachdem der Bootloader gegangen ist

Der Bootloader setzt einen Grafikmodus und verschwindet. Danach ist
Schluss: kein BIOS-Aufruf mehr, kein GOP, nichts. Wer die Auflösung
später noch ändern will, muss die Karte selbst ansprechen.

Für genau diesen Fall haben Bochs, QEMU und VirtualBox eine gemeinsame
kleine Schnittstelle geerbt: ein Indexregister auf `0x01CE`, ein
Datenregister auf `0x01CF`, ein Dutzend Register dahinter. Abschalten,
Breite und Höhe setzen, wieder einschalten – und weil der lineare
Speicher dabei liegen bleibt, wo er war, muss nur noch der Framebuffer
seine neuen Maße erfahren. Es ist die einzige Adresse, die ohnehin schon
abgebildet ist; die physische aus dem PCI-Register wäre hier ein Zeiger
ins Leere.

Was die Karte wirklich gesetzt hat, wird nachgelesen und nicht
geglaubt: Wer einen Modus nicht kann, setzt einen anderen und meldet
ihn – und wer das nicht prüft, zeichnet danach an der falschen Stelle.
Auf echter Hardware gibt es die Schnittstelle nicht; dort bleibt es bei
dem, was der Bootloader eingestellt hat, und das Einstellungsfenster
schreibt *(fest)* neben die Auflösung, statt einen Knopf anzubieten,
der nichts tut.

### Vergrößern, ohne dass ein Programm davon weiß

Die Vergrößerung ist etwas ganz anderes als die Auflösung, und sie geht
überall. Der Backbuffer wird dabei kleiner als der Bildschirm – bei
zweifacher Vergrößerung halb so breit und halb so hoch –, und beim
Ausgeben wird jeder Punkt zu einem Quadrat. Die ganze Oberfläche rechnet
weiter in Punkten dieses Backbuffers und merkt nichts davon.

Genau darum ist es so billig zu haben: Ein Zeichen bleibt 8×16 Punkte
groß, jedes Fenster rechnet weiter wie bisher, und auf einem
4K-Bildschirm ist die Schrift trotzdem wieder zu lesen. Kein Programm
musste dafür angefasst werden.

Vergrößert wird **ganzzahlig**. Alles andere hieße, zwischen Punkten zu
mitteln, und aus einer gestochenen Kante würde Matsch – bei einer
Oberfläche, die aus ein Pixel breiten Linien besteht, ist das kein
Schönheitsfehler, sondern das Ende der Lesbarkeit. Das Aufblasen selbst
kostet fast nichts: Die erste Zeile wird Punkt für Punkt verbreitert,
die übrigen sind nur noch `memcpy` davon.

Die Grenze ist die Arbeitsfläche und nicht der Geschmack: Unter 640×400
logischen Punkten passt kein Fenster mehr sinnvoll hin. `1024×768`
lässt darum gar keine Vergrößerung zu, `1920×1200` drei Stufen. Ohne
eigene Einstellung wird nach der Bildschirmhöhe entschieden – ab
etwa 1400 Zeilen doppelt, ab 2000 dreifach –, und die Automatik geht
dabei nie über das Mögliche hinaus.

Nach jeder Änderung stimmt nichts mehr, was sich die Oberfläche über
die Bildschirmgröße gemerkt hat: Der Zeiger wird neu begrenzt, und jedes
Fenster kommt in die Fläche zurück. Ein Fenster, das gerade noch passt,
behält dabei seine Größe – kleiner wird es nur, wenn es anders nicht
geht.

### Ein Bild statt eines Verlaufs

Die fünf eingebauten Verläufe kosten nichts und sind sofort da. Ein Bild
ist etwas anderes: Es muss geladen, entpackt und auf die
Bildschirmgröße gebracht werden, und danach liegt es als ein Stück im
Speicher. Genau einmal – der Hintergrund wird bei jeder Bewegung eines
Fensters neu gezeichnet, und ein PNG bei jedem Mausschubser aufs Neue zu
entpacken wäre die sicherste Art, die Oberfläche zäh zu machen.

Skaliert wird **füllend und mittig beschnitten**: Das Bild bedeckt die
Fläche ganz und behält sein Seitenverhältnis; was übersteht, wird beim
Zeichnen abgeschnitten, was ohnehin geschieht. Der Verlauf wird trotzdem
zuerst gemalt – Bilder dürfen durchsichtig sein, und was durchscheint,
soll der Verlauf sein und nicht das Bild vom letzten Bildaufbau.

Ein Pfad, hinter dem kein Bild mehr liegt, fällt beim nächsten Start aus
den Einstellungen heraus, statt sie zu vergiften: Sonst stünde bei jedem
Start derselbe tote Pfad da und der Verlauf käme nie zurück.

### Was das System von sich erzählt

Bis vor kurzem gingen alle Meldungen des Kerns auf die serielle
Schnittstelle. In einer virtuellen Maschine ist das bequem; auf einem
richtigen Rechner ohne Kabel war der ganze Startvorgang danach weg – und
gerade dort will man wissen, warum die Platte nicht gefunden wurde.

Jetzt liegen sie zusätzlich in einem Ring im Arbeitsspeicher: die letzten
512, mit Zeit, Dringlichkeit und Herkunft. Ein Ring und keine wachsende
Liste, denn ein Protokoll, das den Speicher auffrisst, ist schlimmer als
eines, das die ältesten Zeilen vergisst; wie viele herausgefallen sind,
steht in der Fußzeile.

Hineingeschrieben wird auf zwei Wegen. `log_write()` nimmt Dringlichkeit
und Herkunft gleich mit – so melden sich An- und Abmeldungen,
Fehlversuche, gestartete und beendete Programme, geänderte Rechte. Der
zweite Weg ist ein Trick: Was `kprintf` auf die serielle Schnittstelle
schreibt, wird zeilenweise mitgeschnitten. Damit steht der komplette
Startvorgang im Fenster, ohne dass eine einzige der bestehenden
`kprintf`-Zeilen angefasst werden musste. Da die Meldungen des Kerns nach
dem Muster `Bereich : Text` gebaut sind, wandert das Wort vor dem
Doppelpunkt in die Spalte für die Herkunft, und Wörter wie „kein" oder
„nicht" färben die Zeile als Warnung ein. Das ist grob geraten – aber es
macht aus hundert unveränderten Zeilen ein lesbares Protokoll.

Die Sperre um den Ring wird mit abgeschalteten Unterbrechungen genommen:
Auch ein Treiber im Interrupt darf etwas melden, und ohne das käme der
Kern an sich selbst nicht vorbei. Gehalten wird sie nur für das
Umkopieren eines Eintrags; formatiert wird davor, denn `ksnprintf` nimmt
keine Sperre.

### Wer darf was

Bis vor kurzem gehörte dieser Rechner immer genau einem Menschen: Wer
davorsaß, durfte alles. Für einen Stick im Laufwerk stimmt das auch –
sobald das System aber auf einer Festplatte liegt und mehrere Leute daran
arbeiten, braucht es Konten.

Jeder Eintrag im Dateibaum trägt jetzt einen Eigentümer, eine Gruppe und
neun Bits nach dem Muster `rwxrwxrwx`. Bei Ordnern bedeuten sie etwas
anderes als bei Dateien: `r` erlaubt das Aufzählen, `w` das Anlegen und
Löschen darin, `x` das Hindurchgehen. Ein Ordner mit `x`, aber ohne `r`
lässt sich also durchqueren, wenn man den Namen kennt, gibt aber seinen
Inhalt nicht preis. Dazu kommt das Klebebit: Im Papierkorb und unter
`/Temp` darf jeder ablegen, aber nur der Eigentümer eines Eintrags ihn
wieder wegnehmen.

Geprüft wird an den sechs Stellen, an denen der Dateibaum tatsächlich
etwas tut – Nachschlagen, Aufzählen, Lesen, Schreiben, Anlegen, Löschen –,
nicht in jedem Programm einzeln. Ein Ring-3-Programm läuft unter der
Nummer dessen, der es aufgerufen hat, und kann darum nie mehr als er.
Wenn das System seine eigenen Dateien führt – Einstellungen sichern, den
Papierkorb pflegen, die Benutzerdatenbank schreiben –, hebt es die Prüfung
für die Dauer dieser Arbeit auf; der Zähler dafür sitzt im Thread, nicht
in einer globalen Variablen, sonst wäre er auf mehreren Kernen falsch.

Das Passwort selbst wird nirgends gespeichert. In `benutzer.conf` steht
ein zufälliges Salz und der 4096-fach wiederholte HMAC-SHA256 darüber. Aus
dem Wert lässt sich das Passwort nicht zurückrechnen, zwei Leute mit
demselben Passwort bekommen verschiedene Einträge, und wer die Datei
erbeutet, zahlt die Rechenzeit für jeden einzelnen Versuch. Die Datei
selbst gehört root und ist `-rw-------`.

FAT32 hat für all das kein Feld. Statt das Dateisystem zu erweitern – das
könnte dann kein anderes System mehr lesen – liegt daneben eine Liste
`/Festplatte/rechte.conf`, Pfad für Pfad. Sie enthält nur, was von der
Vorgabe abweicht, und wird beim Einlesen jedes Ordners angewandt.

Solange es keine gespeicherte Datenbank gibt – von der CD, vom Stick, oder
gleich nach der Installation –, meldet sich RetroOS ohne Nachfrage als
root an. Einen Anmeldebildschirm vor ein System zu setzen, das ohnehin
jedem gehört, der die Scheibe einlegt, wäre Theater. Sobald unter
**Benutzer** das erste Konto angelegt und gespeichert ist, fragt der
nächste Start nach Name und Passwort.

### Wie zwei Programme miteinander reden

Bis vor kurzem gab es dafür nur das Netz: Zwei Programme auf demselben
Rechner brauchten einen Dreiwegehandschlag, um sich ein Wort zu sagen.
Jetzt gibt es zwei direkte Wege, und sie sind absichtlich verschieden.

Eine **Röhre** ist ein Strom von Bytes mit einem Schreiber und einem
Leser. `sys_pipe()` liefert zwei gewöhnliche Dateinummern; `sys_read`
und `sys_write` können damit umgehen, und ein abgespaltenes Kind erbt
sie. Sie kostet einen Ringpuffer im Kern und kopiert zweimal – dafür
muss sich niemand um Sperren kümmern, und das Ende der Röhre sagt dem
Leser von selbst, dass Schluss ist: Erst wenn der letzte Schreiber
verschwunden ist, meldet das Lesen null zurück. Genau deshalb schließt
der Elternteil in `roehre.c` sein eigenes Schreibende, bevor er liest.

**Geteilter Speicher** ist derselbe Seitenrahmen in zwei Adressräumen.
Er kopiert gar nicht und ist damit die schnellste Art, große Mengen
weiterzureichen – dafür müssen sich beide Seiten selbst einigen, wer
wann hineinschreibt. Jeder Bereich hat seinen festen Platz im
Adressraum, also ist die Adresse in beiden Programmen dieselbe und
Zeiger darin lassen sich sogar weiterreichen.

Der Haken dabei ist die Kopie beim Schreiben: Ein abgespaltenes Kind
bekäme sonst geteilte Seiten, die beim ersten Schreiben still zu
privaten würden – zwei getrennte Bereiche, und niemand hätte es
gemerkt. Darum tragen sie `PTE_SHARED`. Das Bit hält sie aus dem
Abspalten heraus und aus dem Abräumen des Adressraums; wer den Bereich
im Kind will, blendet ihn dort ausdrücklich ein. `starte roehre` zeigt
beides nebeneinander.

### Der Käfig um ein Programm

Rechte sagen, was ein Benutzer darf. Ein Käfig sagt, was ein einzelnes
**Programm** darf – und das ist etwas anderes. Ein Bildbetrachter läuft
unter meinem Namen und darf damit alles, was ich darf; er braucht davon
aber nichts außer der einen Datei, die ich ihm hinhalte. Genau diese
Lücke schließt der Käfig: Er nimmt einem Programm Fähigkeiten weg, die
sein Benutzer sehr wohl hätte.

Vier Dinge werden beschränkt. **Systemaufrufe** in Gruppen – einzelne
Nummern wären genauer und in der Bedienung unbrauchbar, niemand stellt
dreißig Schalter richtig ein. Ein **Wurzelpfad**, unter dem alles liegen
muss. Eine **Speicherobergrenze**, denn „kein Netz, keine Dateien" wäre
sonst immer noch genug, um den Rechner zuzuschütten. Und das
**Abspalten**, weil sonst jedes Kind ein neuer Anlauf wäre.

Wer über die Wurzel hinausgreift, bekommt „nicht gefunden" und nicht
„verboten": Die bloße Auskunft, dass eine Datei existiert, ist schon
eine Auskunft. Die Grenze ist eine Textrechnung und keine Wanderung
durch den Dateibaum – eine Grenze, die vom Zustand des Baums abhängt,
wäre in dem Augenblick falsch, in dem jemand einen Ordner umbenennt.
Sie legt Pfade dabei richtig zusammen: `..` geht wirklich eine Ebene
zurück, und `/Benutzer/annalise` liegt nicht unter `/Benutzer/anna`.

| Profil | darf | Wurzel | Speicher | Verstoß |
| --- | --- | --- | --- | --- |
| `offen` | alles | – | – | – |
| `netz` | lesen, Netz, Fenster, Röhren | – | 16 MB | Fehler |
| `heim` | lesen, schreiben, Fenster, Abspalten, Röhren | das eigene Heim | 16 MB | Fehler |
| `streng` | rechnen, ausgeben, Röhren | – | 4 MB | Programm endet |

Der Käfig lässt sich **nur enger machen, nie weiter** – weder von außen
noch vom Programm selbst. Das ist die Eigenschaft, an der alles hängt:
Könnte ein eingesperrtes Programm `offen` wählen, wäre die ganze
Einrichtung eine Bitte. Deshalb darf ein Programm sich getrost selbst
einsperren, sobald es alles beisammen hat, was es braucht – genau der
Zug, den seccomp in Linux möglich macht: Der Webserver etwa braucht,
sobald er lauscht, weder Dateien anzulegen noch sich zu vermehren, und
wer ihn danach übernimmt, bekommt weniger.

Vererbt wird er beim Abspalten; ein Kind kommt nicht dadurch frei, dass
es ein Kind ist. Geprüft wird an genau einer Stelle, ganz vorn in
`syscall_dispatch` – jede Prüfung weiter unten könnte man vergessen,
diese nicht, weil kein Aufruf an ihr vorbeikommt. Jeder Verstoß steht in
der Prüfspur, und der Systemmonitor zeigt neben jedem Programm, in
welchem Käfig es sitzt.

`starte kaefig netz` führt es vor: dasselbe Programm probiert vor und
nach dem Einsperren dieselben Dinge und zeigt, was der Käfig davon
übrig lässt.

### Was durchs Netz darf

Bisher nahm RetroOS jedes Paket an, das an seine Adresse ging. Für einen
Rechner am Netz ist das zu wenig: Wer einen Webserver betreibt, will
Port 8080 offen haben und sonst nichts.

Der Filter ist eine Liste von Regeln je Richtung. Geprüft wird von oben
nach unten, die erste passende entscheidet, sonst gilt die
Grundeinstellung. Das ist das Modell von nftables und der
Windows-Firewall, und es ist deshalb so verbreitet, weil man eine
Regelliste von oben lesen und dabei laut mitsprechen kann.

Eine Regel trifft auf die **Gegenstelle** zu, nicht auf „Quelle" und
„Ziel": Bei einem eingehenden Paket ist das der Absender, bei einem
ausgehenden der Empfänger. Das spart die Hälfte der Felder und die immer
wiederkehrende Frage, welche Seite gerade gemeint ist. Genauso ist der
Port immer der eigene.

Der Filter sitzt an genau zwei Stellen – in `ip_receive()`, bevor das
Paket an ICMP, UDP oder TCP geht, und in `ip_send_via()`, bevor es die
Karte erreicht. Alles darüber muss nichts davon wissen. Weggeworfene
Pakete werden gezählt, aber nur jedes hundertste kommt ins Protokoll:
Ein Scan würde den Ring sonst in Sekunden leerlaufen lassen.

### Rollen statt eines einzigen Schalters

„Verwalter ja/nein" war zu grob. Wer den Paketfilter pflegen soll,
braucht keinen Zugriff auf die Passwörter, und wer die Platte
formatiert, muss nicht das Protokoll leeren dürfen. Darum hängt an jedem
Benutzer eine Menge von **Fähigkeiten** – Konten, Netz, Platte,
Protokoll, Strom, Einstellungen –, und eine **Rolle** ist nichts weiter
als ein Name für eine solche Menge. Wer alle hat, ist Verwalter; das
alte Kennzeichen ist damit nicht verschwunden, sondern zum Sonderfall
geworden.

Vier Rollen genügen für einen Rechner dieser Größe: `verwalter`,
`netzwerk`, `wartung` und `benutzer`. Wer eine fünfte braucht, setzt die
Fähigkeiten einzeln – dann heißt die Rolle `eigen`, und das ist
ehrlicher als ein Name, der nichts bedeutet.

Die **Prüfspur** ist das Gegenstück zum Protokoll. Das Protokoll sagt,
was das System getan hat; die Prüfspur sagt, wer es veranlasst hat und
ob er durfte. Das sind zwei verschiedene Fragen, und deshalb sind es
zwei verschiedene Listen: Ein Protokoll darf man leeren, wenn es
unübersichtlich wird – eine Prüfspur darf das gerade nicht, sonst wäre
sie wertlos. Auf der Platte wird sie fortgeschrieben, nicht ersetzt, und
lesen darf sie nur, wer die Fähigkeit am Protokoll hat.

Aufgeschrieben wird, was für die Sicherheit zählt: Anmeldungen und
Fehlversuche, abgewiesene Schreib- und Löschzugriffe, gebrauchte und
verweigerte Rechte, Änderungen an Konten und am Paketfilter. Nicht
aufgeschrieben wird der Alltag – eine Prüfspur, in der jeder
Dateizugriff steht, liest niemand mehr.

### Eine Tabelle, zwei Hilfen

Die Konsole kennt an die sechzig Befehle. Was sie tun, steht genau
einmal im Quelltext: in einer Tabelle, die zu jedem Befehl den Namen,
den englischen Zweitnamen, sein Gebiet, die Aufrufform, eine Zeile
Erklärung und wahlweise einen längeren Text führt. `hilfe` geht sie
nach Gebieten durch, `man` sucht einen Eintrag heraus. Zwei Hilfen aus
einer Quelle – dann kann keine der beiden veralten, während die andere
stimmt. Eine Prüfung im Testlauf geht die Tabelle durch und besteht
darauf, dass jeder Eintrag vollständig ist, sein Gebiet wirklich
existiert, die Aufrufform mit dem Namen anfängt und kein Name und kein
Zweitname zweimal vorkommt.

Die deutschen Namen sind gemeint, die englischen sind der Frieden mit
den Fingern: Wer zwanzig Jahre `grep` getippt hat, tippt `grep`, und
`suche` steht daneben für alle anderen. Beide gehen an dieselbe Stelle.

Zwei Kleinigkeiten stecken tiefer, als sie aussehen. Das Namensmuster
in `finde` läuft **ohne Rekursion** – ein `*` merkt sich seine Stelle
und den Text dahinter und setzt beim Scheitern eine Stelle weiter auf;
der Kernstapel ist klein, und `***a***b` soll ihn nicht sprengen.
Und `rechne` ist ein richtiger Zerteiler mit Summe, Produkt und Faktor,
kein Ablaufen von links nach rechts: `2 + 3 * 4` sind 14, nicht 20.
Geteilt durch null gibt eine Meldung und keinen Ausnahmefehler.

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
| **Tabellenkalkulation** | 94 Prüfungen: Zahlen, Bezüge, jeder Rechenschritt, alle Funktionen, Kreisbezüge, CSV hin und zurück |
| **Textverarbeitung** | 62 Prüfungen: Bearbeiten, Auszeichnungen, HTML schreiben und wieder lesen, fremdes HTML, Grenzen |
| **Dokumentbaum** | 83 Prüfungen zu Zerteiler, Kaskade, Umbruch, Skripten am Baum und Zeitgebern |
| **Aufgaben** | 96 Prüfungen: Anlegen und Entfernen, Reihenfolge, Termine samt Schaltjahren, Datei hin und zurück, von Hand geschriebene Listen |
| **Systemprotokoll** | 50 Prüfungen: Ring samt Überlauf, Dringlichkeiten, Zerlegung der `kprintf`-Zeilen, Sichern |
| **Rechte und Benutzer** | 155 Prüfungen: Rechtebits samt Reihenfolge, Klebebit, Textform hin und zurück, Rollen und Fähigkeiten, Passwort-Prüfwerte, `benutzer.conf` schreiben und lesen, beschädigte Dateien |
| **Käfig** | 78 Prüfungen: alle vier Profile, die Einbahnstraße (nur enger, nie weiter), die Pfadgrenze samt `..` und Namen, die mit der Wurzel anfangen, Zuordnung der Systemaufrufe |
| **Paketfilter** | 67 Prüfungen: jede Achse einer Regel einzeln, Reihenfolge und Ausnahmen, Textform, Datei hin und zurück |

Die Testbilder erzeugt `make testbilder` neu (benötigt Pillow).

## Aufbau des Quelltexts

```
kernel/
  include/            Öffentliche Header aller Bausteine
  linker.ld           Speicheraufteilung des Kernels
  src/
    main.c            Startreihenfolge des Systems
    arch/             GDT, IDT, Interrupt-Stubs, APIC, IOAPIC, PIC, Zeitgeber,
                      ACPI, Systemaufrufe, Kerne und ihr Start
    mm/               Seitenverwaltung, Adressräume und Heap
    sched/            Threads und präemptiver Scheduler auf allen Kernen
    proc/             ELF64-Lader, Prozesse in Ring 3, Käfig, Röhren,
                      geteilter Speicher, Fenster für Programme
    drivers/          Framebuffer, Moduswechsel, PS/2, Tastatur, Maus,
                      RTC, UART, PCI,
                      NVMe, AHCI, ATA, Blockgeräte, xHCI, USB-HID,
                      USB-Speicher, virtio-net, igb, e1000e, e1000,
                      RTL8169, RTL8139
    fs/               Dateibaum, FAT32, Partitionstabellen, Startbestand,
                      Installation auf Festplatte, Papierkorb,
                      Benutzer und Rechte
    net/              Kartenauswahl, Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS,
                      TCP, TLS, HTTP, Paketfilter
    crypto/           SHA-2, HKDF, ChaCha20, AES, X25519, Großzahlen,
                      RSA, P-256, ASN.1, X.509, Wurzelzertifikate
    gfx/              DEFLATE, PNG lesen und schreiben, JPEG, GIF, BMP,
                      Skalieren und Zeichnen
    lib/              Zeichenketten, Ausgabe, Systemprotokoll, Prüfspur,
                      Sprachtabelle, 128-Bit-Division
    gui/              Grafik, Schrift, Symbole, Fenstersystem, Desktop,
                      Aufloesung und Vergroesserung, Hintergrundbild,
                      Anmeldebildschirm, Bedienelemente
    js/               Zerteiler, Deuter, Bibliothek und Anbindung an den Baum
    apps/             Dateimanager, Browser, Installation, Einstellungen,
                      Benutzerverwaltung, Systemmonitor, Protokoll, Aufgaben,
                      Dokumentbaum, HTML-Leser, CSS, Umbruch, Editor,
                      Programmieren, Tabelle, Schreiben, Vortrag,
                      Konsole samt Werkzeugkasten, Rechner, Bildbetrachter,
                      Bildschirmfoto, Systeminformation, Dialoge
userland/             Ring-3-Programme samt kleiner Laufzeitbibliothek
data/                 Wurzelzertifikate, Beispielbilder, Sprachtabelle
third_party/lucide/   Symbolvorlagen (ISC) samt Lizenz
third_party/fonts/    Schriftvorlagen (OFL u. a.) samt Lizenzen
tests/                Testsammlungen für den Entwicklungsrechner
boot/limine.conf      Bootloader-Eintrag
scripts/              Bootloader holen, Schrift, Symbole, Zertifikate,
                      Bilder und Sprachtabelle erzeugen
```

Erzeugte und eingecheckte Dateien: die Schriften in
`kernel/src/gui/font_data.c` (`python3 scripts/gen_font.py`, braucht
`fonttools`, `brotli` und `pillow`), die Symbole
in `kernel/src/gui/icon_data.c` (`python3 scripts/gen_icons.py`, braucht
`cairosvg` und `pillow`), die Wurzelzertifikate in
`data/wurzelzertifikate.der` (`python3 scripts/gen_trust_store.py`) und die
Beispielbilder in `data/` (`python3 scripts/gen_bilder.py`).

## Bekannte Grenzen

**Viele kurzlebige Prozesse auf mehreren Kernen.** Wer rasch hintereinander
Programme startet und beendet – `starte gabeln 8` etwa, oder der Webserver
unter Last –, bringt RetroOS auf zwei und mehr Kernen nach einigen Sekunden
zum Stehen: ein Kernel-Stapel wird beschrieben, während sein Thread noch
darauf steht. Der Fehler steckt im Abbau von Threads und ist älter als das
Abspalten; er lässt sich auch ohne `fork` auslösen, indem man auf vier Kernen
sechsmal `starte hallo` hintereinander eingibt. Mit einem Kern
(`-smp 1`, die Voreinstellung) tritt er nicht auf. Bis das behoben ist,
gehören mehrere Kerne und starker Prozesswechsel nicht zusammen.

## In VirtualBox

Zwei Einstellungen entscheiden darüber, ob RetroOS dort überhaupt
hochkommt und ob sich der Zeiger bewegt.

**Der Gast muss 64 Bit sein.** *Allgemein → Basis → Version* auf eine
64-Bit-Variante stellen, am besten `Other/Unknown (64-bit)`. Sonst zeigt
VirtualBox der VM eine 32-Bit-CPU, und der Bootloader bricht ab:

```
PANIC: limine: This CPU does not support 64-bit mode.
```

Stehen im Ausklappmenü gar keine 64-Bit-Einträge, kommt VirtualBox nicht
an die Hardware-Virtualisierung: VT-x/AMD-V im Firmware-Setup des Wirts
einschalten, und unter Windows Hyper-V, *Windows Hypervisor Platform*,
WSL2 und die Speicherintegrität abschalten – die belegen VT-x sonst
selbst (`bcdedit /set hypervisorlaunchtype off`, danach neu starten).

Für die Auflösung gilt: `VBoxVGA` und `VBoxSVGA` bringen die
Bochs-Schnittstelle mit, RetroOS kann dort also umschalten. `VMSVGA`
kann das nicht – dort bleibt es bei dem, was der Bootloader gesetzt hat,
und das Einstellungsfenster schreibt *(fest)* daneben.

**Das Zeigergerät muss eine PS/2-Maus sein.** *System → Mainboard →
Zeigegerät* auf `PS/2-Maus` stellen. Ab Werk steht dort ein
USB-Tablett, und VirtualBox hängt es an einen OHCI-Controller – RetroOS
spricht xHCI und OHCI nicht, der Mausanschluss bliebe leer.

Ob es daran liegt, sagt der Befehl `maus` in der Konsole: Er zeigt, ob
ein PS/2-Controller da ist, ob sich an Port 2 jemand gemeldet hat und
wie viele Bytes bisher angekommen sind – über den Interrupt und über
die Abfrage aus der Hauptschleife. Stehen beide Zähler auf null, kommt
gar nichts an, und die Einstellung ist die Ursache.

## Fehlersuche

Der Kernel schreibt seine Meldungen auf die serielle Schnittstelle, nicht auf
den Bildschirm – dort läuft die Oberfläche. In QEMU erscheinen sie dank
`-serial stdio` direkt im Terminal:

```
=====================================
  RetroOS 1.0 - Systemstart
=====================================
Bildschirm  : 1280x800, 32 Bit
PMM         : 506 MiB verwaltet, 506 MiB frei
Heap        : 260 KiB bereit
Adressraum  : Kernel-Tabelle bei 0x000000001fea1000, Ausfuehrsperre aktiv
ACPI        : PM1a 0x604, S5 = 0/0
APIC        : 1 Kern, 1 IOAPIC mit 24 Eingaengen
APIC-Timer  : 1000 Hz, 62636 Takte je Millisekunde
PS/2        : laut ACPI nicht vorhanden
PCI         : 8 Geraete gefunden
  00:02.0  1b36:0010  NVMe-Controller
  00:03.0  1b36:000d  USB-Controller
  00:04.0  1af4:1000  Netzwerkkarte
Datentraeger: nvme0 - QEMU NVMe Ctrl, 256 MiB
USB         : xHCI mit 8 Anschluessen, 8 Steckplaetzen, MSI
USB         : Anschluss 5, 0627:0001, Klasse 3.1.1, High Speed
USB         : Tastatur angemeldet
USB         : Anschluss 6, 0627:0001, Klasse 3.1.2, High Speed
USB         : Maus angemeldet
Dateisystem : 28 Eintraege, 202974 Bytes
Datentraeger: GPT, Abschnitt 2 ab Sektor 34816
Datentraeger: RETROOS eingehaengt unter /Festplatte (235 MiB frei)
Scheduler   : bereit, Zeitscheibe 20 ms
Systemaufrufe: bereit (14 Nummern)
Zertifikate : 152 Wurzeln geladen
Netzwerk    : virtio-net (1.0), 52:54:00:12:34:56
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

Wer die modernen Wege ausprobieren will, gibt QEMU die passenden Geräte
mit – ganz ohne PS/2, mit USB-Eingabe und einer NVMe-SSD:

```sh
qemu-system-x86_64 -M q35,i8042=off -m 512M -cdrom retroos.iso -boot d \
  -device qemu-xhci,id=xhci \
  -device usb-hub,bus=xhci.0,port=2 \
  -device usb-kbd,bus=xhci.0,port=2.1 \
  -device usb-mouse,bus=xhci.0,port=2.2 \
  -drive file=stick.img,if=none,id=stick,format=raw \
  -device usb-storage,bus=xhci.0,port=3,drive=stick \
  -drive file=platte.img,if=none,id=nvm0,format=raw \
  -device nvme,serial=RETRO0001,drive=nvm0 \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -serial stdio
```

## Lizenz

MIT – siehe [LICENSE](LICENSE). Der beim Bauen geladene Bootloader Limine
steht unter der BSD-2-Clause-Lizenz. Die Symbolvorlagen stammen von Lucide
(ISC), die Schriftvorlagen stehen unter der SIL Open Font License 1.1, der
Ubuntu Font Licence 1.0 beziehungsweise der Bitstream-Vera-Lizenz – die
vollen Texte liegen unter `third_party/`.
