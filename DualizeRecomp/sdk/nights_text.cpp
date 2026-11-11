// Our Options texts in the game's languages. The help lines go into game
// messages of fixed room (a longer line is cut), so they are kept short:
// Italian has the least, 38 characters.

#include "sdk/nights_text.hpp"

#include "kernel/memory.hpp"
#include "core/cpu.h"
#include "ppc_natives.h"

namespace
{
using dualize::nights::Language;

// The language word in the game's settings.
constexpr uint32_t SETTINGS_LANGUAGE = 0xAC;

struct Text
{
  std::string_view english, japanese, french, spanish, german, italian;
};
constexpr Text TEXTS[] = {
    // The main page.
    {"Language Settings", "言語設定", "Paramètres de langue", "Config. idioma", "Spracheinstellungen",
     "Impostazioni lingua"},
    {"Subtitles", "字幕", "Sous-titres", "Subtítulos", "Untertitel", "Sottotitoli"},
    {"Show the dialogue lines in cutscenes", "ムービー中のセリフを表示します", "Afficher les dialogues des cinématiques",
     "Mostrar los diálogos en las escenas", "Dialoge in Zwischensequenzen anzeigen",
     "Mostra i dialoghi nelle scene animate"},
    {"Graphics Settings", "グラフィック設定", "Paramètres graphiques", "Configuración gráfica", "Grafikeinstellungen",
     "Impostazioni grafiche"},
    {"Rumble", "振動", "Vibrations", "Vibración", "Vibration", "Vibrazione"},
    {"Weather Link", "お天気連動", "Liaison météo", "Conexión meteorológica", "Wetterverbindung",
     "Collegamento clima"},
    // The graphics page (ui/graphics_settings.cpp).
    {"Fullscreen", "フルスクリーン", "Plein écran", "Pantalla completa", "Vollbild", "Schermo intero"},
    {"Fill the screen with a borderless window", "枠なしのウィンドウで画面全体に表示します",
     "Remplir l'écran sans bordure de fenêtre", "Llenar la pantalla sin bordes", "Bildschirm randlos ausfüllen",
     "Riempi lo schermo senza bordi"},
    {"Window Size", "ウィンドウサイズ", "Taille de la fenêtre", "Tamaño de ventana", "Fenstergröße",
     "Dimensioni finestra"},
    {"The window's size on the desktop", "デスクトップ上のウィンドウの大きさです", "Taille de la fenêtre sur le bureau",
     "Tamaño de la ventana en el escritorio", "Größe des Fensters auf dem Desktop", "Dimensioni della finestra"},
    {"Fullscreen fills the display; turn it off", "フルスクリーンをオフにすると変更できます",
     "Désactivez le plein écran pour changer", "Desactiva Pantalla completa para cambiarlo",
     "Zum Ändern Vollbild ausschalten", "Disattiva lo schermo intero"},
    {"Resolution Scale", "解像度スケール", "Échelle de résolution", "Escala de resolución", "Auflösungsskalierung",
     "Scala risoluzione"},
    {"Draw the 3D world sharper than the window", "3Dの世界をウィンドウより高精細に描きます",
     "Rendre le monde 3D plus net que la fenêtre", "Dibujar el mundo 3D más nítido",
     "3D-Welt schärfer als das Fenster zeichnen", "Disegna il mondo 3D più nitido"},
    {"V-Sync", "垂直同期", "V-Sync", "V-Sync", "V-Sync", "V-Sync"},
    {"Present in step with the display", "ディスプレイと同期して表示します", "Synchroniser l'affichage avec l'écran",
     "Sincronizar con la pantalla", "Bildausgabe mit dem Display abgleichen", "Sincronizza con lo schermo"},
    {"Frame Rate", "フレームレート", "Fréquence d'images", "Velocidad de fotogramas", "Bildrate",
     "Frequenza fotogrammi"},
    {"30 is the original; more is interpolated", "30が本来の値、それ以上は補間します",
     "30 est l'original ; plus est interpolé", "30 es el original; más se interpola",
     "30 ist original; mehr wird interpoliert", "30 è l'originale, di più è interpolato"},
    {"MAX", "MAX", "MAX", "MÁX", "MAX", "MAX"},
    {"Anti-Aliasing", "アンチエイリアス", "Anticrénelage", "Antialiasing", "Kantenglättung", "Anti-aliasing"},
    {"Smooth the jagged edges of 3D objects", "3Dオブジェクトのギザギザを滑らかにします",
     "Lisser les bords crénelés des objets 3D", "Suavizar los bordes de los objetos 3D",
     "Treppenkanten von 3D-Objekten glätten", "Smussa i bordi degli oggetti 3D"},
    {"Transparency Anti-Aliasing", "透過アンチエイリアス", "Anticrénelage de transparence",
     "Antialiasing de transparencia", "Transparenz-Kantenglättung", "Anti-aliasing trasparenze"},
    {"Smooth the cut edges of leaves and fences", "葉やフェンスのふちを滑らかにします",
     "Lisser les bords des feuilles et clôtures", "Suavizar bordes de hojas y vallas",
     "Kanten von Blättern und Zäunen glätten", "Smussa i bordi di foglie e recinzioni"},
    {"Ambient Occlusion", "アンビエントオクルージョン", "Occlusion ambiante", "Oclusión ambiental",
     "Umgebungsverdeckung", "Occlusione ambientale"},
    {"Darken creases and corners of the world", "くぼみや隅を暗くして立体感を出します",
     "Assombrir les creux et coins du décor", "Oscurecer pliegues y rincones del mundo",
     "Falten und Ecken der Welt abdunkeln", "Scurisci pieghe e angoli del mondo"},
};
}  // namespace

namespace dualize::nights
{
Language GameLanguage(const Memory& memory)
{
  const uint32_t settings = Load32(memory.Effective(NIGHTS_SETTINGS, 4));
  if (settings == 0)
    return Language::ENGLISH;
  const uint32_t language = Load32(memory.Effective(settings + SETTINGS_LANGUAGE, 4));
  return language < LANGUAGE_COUNT ? static_cast<Language>(language) : Language::ENGLISH;
}

std::string_view Translate(std::string_view english, Language language)
{
  for (const Text& text : TEXTS)
  {
    if (text.english != english)
      continue;
    switch (language)
    {
    case Language::JAPANESE:
      return text.japanese;
    case Language::FRENCH:
      return text.french;
    case Language::SPANISH:
      return text.spanish;
    case Language::GERMAN:
      return text.german;
    case Language::ITALIAN:
      return text.italian;
    default:
      return text.english;
    }
  }
  return english;
}

std::u16string Utf16(std::string_view text)
{
  std::u16string out;
  for (size_t i = 0; i < text.size();)
  {
    const auto c = static_cast<unsigned char>(text[i]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    char32_t code = length == 1 ? c : length == 2 ? c & 0x1F : length == 3 ? c & 0x0F : c & 0x07;
    for (size_t k = 1; k < length && i + k < text.size(); ++k)
      code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    if (code >= 0x10000)
    {
      code -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
    }
    else
    {
      out.push_back(static_cast<char16_t>(code));
    }
    i += length;
  }
  return out;
}
}  // namespace dualize::nights
