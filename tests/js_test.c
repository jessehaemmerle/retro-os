/* js_test.c - prueft den JavaScript-Deuter mit kleinen Programmen.
 *
 * Jeder Fall besteht aus Quelltext und dem, was console.log ausgeben
 * soll. So wird gleichzeitig das Ergebnis und die Ausgabe geprueft.
 */

#include <stdio.h>
#include <string.h>

#include "js.h"

static int checks, failures;

static void check(const char *name, const char *source, const char *expect)
{
    struct js_context *ctx = js_create();

    checks++;
    if (!ctx) {
        failures++;
        printf("  FEHLER %s - keine Umgebung\n", name);
        return;
    }

    bool ok = js_run(ctx, source, strlen(source));
    const char *out = js_console(ctx);
    char trimmed[4096];

    strncpy(trimmed, out, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    size_t length = strlen(trimmed);

    while (length > 0 && trimmed[length - 1] == '\n')
        trimmed[--length] = '\0';

    if (!ok) {
        failures++;
        printf("  FEHLER %s - %s\n", name,
               js_error(ctx) ? js_error(ctx) : "Ausfuehrung misslungen");
    } else if (strcmp(trimmed, expect) != 0) {
        failures++;
        printf("  FEHLER %s\n         erwartet: %s\n         bekommen: %s\n",
               name, expect, trimmed);
    } else {
        printf("  ok    %s\n", name);
    }
    js_destroy(ctx);
}

int main(void)
{
    printf("\nJavaScript-Deuter\n");

    check("Rechnen", "console.log(1 + 2 * 3 - 4 / 2)", "5");
    check("Klammern", "console.log((1 + 2) * (3 + 4))", "21");
    check("Bruchzahlen", "console.log(0.1 + 0.2)", "0.3");
    check("Teilen", "console.log(7 / 2)", "3.5");
    check("Rest", "console.log(17 % 5)", "2");
    check("Potenz", "console.log(2 ** 10)", "1024");
    check("Vergleiche", "console.log(3 < 4, 4 <= 4, 5 > 6, 'a' < 'b')",
          "true true false true");
    check("Gleichheit", "console.log(1 == '1', 1 === '1', null == undefined)",
          "true false true");
    check("Bitweise", "console.log(6 & 3, 6 | 3, 6 ^ 3, ~5, 1 << 4, 32 >> 2)",
          "2 7 5 -6 16 8");

    check("Zeichenketten",
          "var a = 'Hallo'; var b = 'Welt'; console.log(a + ', ' + b + '!')",
          "Hallo, Welt!");
    check("Zeichenkettenverfahren",
          "var s = '  RetroOS  ';"
          "console.log(s.trim().toUpperCase(), s.trim().length,"
          "            'abc'.charAt(1), 'a-b-c'.split('-').join('+'))",
          "RETROOS 7 b a+b+c");
    check("Ersetzen",
          "console.log('eins zwei eins'.replace('eins', 'drei'))",
          "drei zwei eins");
    check("Ersetzen ueberall",
          "console.log('a.b.c'.split('.').join('/'))", "a/b/c");
    check("Auffuellen",
          "console.log('7'.padStart(3, '0'), 'x'.repeat(4))", "007 xxxx");

    check("Bedingung",
          "var x = 5; if (x > 3) { console.log('gross') } else { console.log('klein') }",
          "gross");
    check("Schleife mit Zaehler",
          "var s = 0; for (var i = 1; i <= 10; i++) s += i; console.log(s)",
          "55");
    check("while", "var i = 0; while (i < 3) { i++ } console.log(i)", "3");
    check("do while", "var i = 10; do { i-- } while (i > 7); console.log(i)", "7");
    check("break und continue",
          "var s = ''; for (var i = 0; i < 10; i++) {"
          "  if (i % 2) continue; if (i > 6) break; s += i } console.log(s)",
          "0246");
    check("Verschachtelt mit Marke",
          "var s = ''; aussen: for (var i = 0; i < 3; i++) {"
          "  for (var j = 0; j < 3; j++) { if (j == 2) continue aussen;"
          "    s += i + '' + j + ' ' } } console.log(s.trim())",
          "00 01 10 11 20 21");
    check("switch",
          "function f(x) { switch (x) { case 1: return 'eins';"
          "  case 2: case 3: return 'zwei oder drei'; default: return 'anders' } }"
          "console.log(f(1), f(3), f(9))",
          "eins zwei oder drei anders");

    check("Funktionen",
          "function quadrat(x) { return x * x } console.log(quadrat(7))", "49");
    check("Rekursion",
          "function fak(n) { return n <= 1 ? 1 : n * fak(n - 1) }"
          "console.log(fak(10))", "3628800");
    check("Fibonacci",
          "function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2) }"
          "console.log(fib(20))", "6765");
    check("Abschluss",
          "function zaehler() { var n = 0; return function () { return ++n } }"
          "var c = zaehler(); c(); c(); console.log(c())", "3");
    check("Pfeilfunktion",
          "var f = (a, b) => a * b; var g = x => x + 1;"
          "console.log(f(6, 7), g(41))", "42 42");
    check("Vorgabewerte",
          "function f(a, b = 10) { return a + b } console.log(f(5), f(5, 1))",
          "15 6");
    check("Restparameter",
          "function summe(...zahlen) { return zahlen.reduce((a, b) => a + b, 0) }"
          "console.log(summe(1, 2, 3, 4))", "10");

    check("Felder",
          "var a = [3, 1, 2]; a.push(4); console.log(a.length, a.join(','))",
          "4 3,1,2,4");
    check("Feldverfahren",
          "var a = [1, 2, 3, 4, 5];"
          "console.log(a.map(x => x * 2).join(','),"
          "            a.filter(x => x % 2).join(','),"
          "            a.reduce((s, x) => s + x, 0),"
          "            a.find(x => x > 3),"
          "            a.indexOf(3))",
          "2,4,6,8,10 1,3,5 15 4 2");
    check("Sortieren",
          "var a = [5, 3, 9, 1]; a.sort((x, y) => x - y); console.log(a.join(','))",
          "1,3,5,9");
    check("Ausschneiden",
          "var a = [1,2,3,4,5]; console.log(a.slice(1, 3).join(','),"
          " a.splice(1, 2).join(','), a.join(','))",
          "2,3 2,3 1,4,5");
    check("Ausbreiten",
          "var a = [1, 2]; var b = [0, ...a, 3]; console.log(b.join(','))",
          "0,1,2,3");

    check("Objekte",
          "var o = { name: 'RetroOS', jahr: 2026 };"
          "console.log(o.name, o['jahr'], Object.keys(o).join(','))",
          "RetroOS 2026 name,jahr");
    check("Verschachtelt",
          "var o = { a: { b: { c: 42 } } }; console.log(o.a.b.c)", "42");
    check("Verfahren am Objekt",
          "var o = { n: 5, doppelt() { return this.n * 2 } };"
          "console.log(o.doppelt())", "10");
    check("Kurzschreibweise",
          "var x = 1, y = 2; var o = { x, y }; console.log(JSON.stringify(o))",
          "{\"x\":1,\"y\":2}");
    check("Zerlegen",
          "var { a, b } = { a: 1, b: 2 }; var [p, q] = [3, 4];"
          "console.log(a, b, p, q)", "1 2 3 4");

    check("for-of",
          "var s = ''; for (const x of [1, 2, 3]) s += x; console.log(s)", "123");
    check("for-in",
          "var s = ''; for (var k in { a: 1, b: 2 }) s += k; console.log(s)",
          "ab");

    check("JSON schreiben",
          "console.log(JSON.stringify({ a: [1, 2], b: 'x', c: true, d: null }))",
          "{\"a\":[1,2],\"b\":\"x\",\"c\":true,\"d\":null}");
    check("JSON lesen",
          "var o = JSON.parse('{\"x\": [1, 2, {\"y\": \"z\"}]}');"
          "console.log(o.x[2].y, o.x.length)", "z 3");

    check("Math",
          "console.log(Math.max(1, 9, 5), Math.min(1, 9, 5), Math.abs(-7),"
          "            Math.floor(3.7), Math.ceil(3.2), Math.round(3.5),"
          "            Math.sqrt(144), Math.pow(3, 4))",
          "9 1 7 3 4 4 12 81");

    check("Ausnahmen",
          "try { throw new Error('kaputt') } catch (e) { console.log('gefangen') }"
          "finally { console.log('fertig') }",
          "gefangen\nfertig");
    check("Werfen und weiterreichen",
          "function f() { throw 'hoppla' }"
          "try { f() } catch (e) { console.log(e) }", "hoppla");

    check("typeof",
          "console.log(typeof 1, typeof 'a', typeof true, typeof undefined,"
          "            typeof {}, typeof [], typeof function(){})",
          "number string boolean undefined object object function");

    check("Klassen",
          "class Tier { constructor(name) { this.name = name }"
          "  ruf() { return this.name + ' macht Geraeusche' } }"
          "var t = new Tier('Katze'); console.log(t.ruf())",
          "Katze macht Geraeusche");
    check("Vererbung",
          "class A { gruss() { return 'A' } }"
          "class B extends A { }"
          "console.log(new B().gruss())", "A");

    check("Erzeuger",
          "function Punkt(x, y) { this.x = x; this.y = y }"
          "Punkt.prototype.summe = function () { return this.x + this.y }"
          "console.log(new Punkt(3, 4).summe())", "7");

    check("Vorlagen",
          "var n = 'Welt'; console.log(`Hallo ${n}, ${1 + 1} mal`)",
          "Hallo Welt, 2 mal");

    check("Kurzschluss",
          "console.log(0 || 'ja', 1 && 'nein', null ?? 'vorgabe')",
          "ja nein vorgabe");

    check("Umwandlungen",
          "console.log(parseInt('42px'), parseFloat('3.14xyz'), Number('7'),"
          "            String(9) + 1, +'8', isNaN('abc'))",
          "42 3.14 7 91 8 true");

    check("Nachkommastellen",
          "console.log((1/3).toFixed(3), (2.5).toFixed(0), (1234.5678).toFixed(2))",
          "0.333 3 1234.57");

    check("Voranstellen und Nachstellen",
          "var i = 5; console.log(i++, i, ++i, i--, i)", "5 6 7 7 6");

    check("Verkettete Aufrufe",
          "console.log([3,1,2].sort().map(x => x * 10).filter(x => x > 10).join('-'))",
          "20-30");

    printf("\n%d Pruefungen, %d Fehler\n\n", checks, failures);
    return failures ? 1 : 0;
}
