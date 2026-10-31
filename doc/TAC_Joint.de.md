# TAC Joint

TAC Joint verwendet die beiden bereitgestellten STL-Dateien als feste Vorlage.
Die Originaldateien liegen zur Weiterentwicklung unter `resources/cut_connectors/tac_joint/`;
die Anwendung enthält die Netze auch direkt und benötigt keine Dateien vom Desktop.

## Verwendung

1. Ein Objekt auswählen und das Schnittwerkzeug öffnen (`C`).
2. Einen ebenen Schnitt einstellen und „Verbinder hinzufügen“ bzw. „Verbinder bearbeiten“ öffnen.
3. **TAC Joint** auswählen und auf der Schnittfläche platzieren.
4. Werte einstellen, die Verbinder bestätigen und den Schnitt ausführen.

Der Schnitt erzeugt einen passenden Ausschnitt in beiden Hälften und den separat
druckbaren Verbinder. Die blaue Vorschau zeigt die Ausschnittform. Der Verbinder
wird für den Druck flach neben den Bauteilen abgelegt; seine Lage in der
Zusammenbauansicht bleibt erhalten.

| Einstellung | Standard | Bedeutung |
|---|---:|---|
| Größe | 100 % | Originalverbinder: ca. 4,09 × 3,34 × 11 mm |
| Seitliche Toleranz | 0,12 mm | Nennspiel je Seite |
| Toleranz an den Enden | 0,30 mm | Spiel je Ende |
| Schnittversatz | 0 mm | Schnitt durch die Mitte; je 5,5 mm Einstecktiefe ohne Schnittspalt |
| Rotation | 0° | Drehung auf der Schnittfläche, einstellbar bis 360° |

**TAC Joint zurücksetzen** stellt diese Werte wieder her. Bei Mehrfachauswahl
wird nur das jeweils bearbeitete Feld geändert; Zurücksetzen ersetzt alle Werte.

Bei den Standardwerten entsprechen Verbinder und Ausschnitt den ursprünglichen
STL-Geometrien. Die seitliche Toleranz ist ein Nennwert: Sie verändert die
Abmessungen des Ausschnitts, nicht einen überall gleichen Oberflächenabstand an
Rundungen. Beim Skalieren bleiben die eingestellten Toleranzen in Millimetern
erhalten. Zu enge Passungen und fehlende Einstecktiefe werden zurückgewiesen.

## Unterstützte Schnitte und Speicherung

Unterstützt sind ebene Schnitte, auch mit Schnittspalt. Der Spalt verringert die
Einstecktiefe; die originale Länge des Verbinders wird dabei nicht verlängert.
Beide Hälften können als Teile eines Objekts erhalten bleiben. Größe, Passung,
Versatz und Drehung werden in Projekten und Schnittrezepten gespeichert und
lassen sich rückgängig machen.

Geschwungene/gezeichnete Schnitte, Nut-Schnitte, die Kombination mit Flexi,
individuelle Teilzuordnung und Schnittspiegelung sind für TAC Joint gesperrt.
Die Randprüfung verwendet Punkte auf dem Ausschnittnetz; sehr schmale innere
Hohlräume zwischen diesen Punkten können dabei unentdeckt bleiben. Die Prüfung
der Passung zwischen Verbinder und Ausschnitt erfolgt separat geometrisch.

## Weiterentwicklung

Die Geometrie und Validierung liegen in `src/libslic3r/TacJoint.cpp`, die
Schnittintegration in `CutUtils.cpp`, die Bedienung in `GLGizmoCut.cpp`.
`scripts/generate_tac_joint.py --check` prüft die reproduzierbar eingebetteten
STL-Daten. Die zugehörigen Tests tragen das Catch2-Tag `[TACJoint]`.

Für den Build gelten die Plattformanleitungen in der README. Mit aktiviertem
`BUILD_TESTS` wird das Testprogramm über das CMake-Ziel `libslic3r_tests` gebaut.
Der Filter `[TACJoint]` führt die zugehörigen Geometrie-, Schnitt- und
Speichertests aus. Der Generator benötigt nur die Python-Standardbibliothek.

Ein physischer Probedruck und ein Netzwerk-Druckauftrag sind nicht Teil der
automatischen Softwareprüfung.
