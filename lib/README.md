# Lokale Bibliotheken

Dieses Verzeichnis ist fuer projektspezifische, lokal eingebundene
PlatformIO-Bibliotheken vorgesehen. Aktuell liegen hier keine lokalen
Bibliotheken.

Externe Abhaengigkeiten werden in `platformio.ini` unter `lib_deps`
deklariert und von PlatformIO verwaltet. Installierte Bibliotheken unter
`.pio/libdeps/` sind Build-Artefakte und werden nicht als Projektdateien
bearbeitet oder dokumentiert.

Fuege eine lokale Bibliothek nur hinzu, wenn sie eine klar abgegrenzte,
wiederverwendbare Komponente ist, die nicht zu den Firmwaremodulen in `src/`
gehoert. Lege jede Komponente in einem eigenen Unterverzeichnis an und
dokumentiere ihre API und Verwendung dort.

Siehe auch das [Entwicklerhandbuch](../README_DEVELOPERS.md#entwicklungsumgebung-und-build).
