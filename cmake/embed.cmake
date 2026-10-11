# Transforme la page de l'interface en tableau d'octets C++ (évite les limites de taille
# des chaînes de caractères de MSVC). Appelé avec -DIN=<page.html> -DOUT=<entete.h>
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
set(body "")
set(pos 0)
while(pos LESS n)
    string(SUBSTRING "${hex}" ${pos} 128 chunk)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," chunk "${chunk}")
    string(APPEND body "    ${chunk}\n")
    math(EXPR pos "${pos} + 128")
endwhile()
file(WRITE "${OUT}" "// Fichier généré par cmake/embed.cmake : ne pas modifier\n#pragma once\n#include <cstddef>\nnamespace raw {\ninline constexpr unsigned char kUiHtml[] = {\n${body}    0x00};\ninline constexpr size_t kUiHtmlLen = sizeof(kUiHtml) - 1;\n}\n")
