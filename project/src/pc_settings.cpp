#include "platform/native_host_window.h"
// In-game PC settings for the Xbox 360 ports; see pc_settings.h.
//
// Text is drawn with GDI (Segoe UI, so accents and arrows render) into a DIB that becomes a
// texture only when the menu changes - the same screen as the FIFA Street 1 port's options.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include "platform/native_shader_compiler.h"
#include <dxgi1_6.h>

#include "pc_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace pcs {
namespace {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------- settings

GameInfo g_info;
Settings g_opt;
Settings g_boot;  // what this process was started with
wchar_t g_ini[MAX_PATH];
bool g_use_ini = true;
bool g_configured = false;

constexpr int kMaxEffects = 8;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
std::atomic<uint32_t> g_effect_mask{0xff};
std::atomic<bool> g_effect_remote{false};
std::atomic<int64_t> g_effect_checked{-1000000};
std::atomic_flag g_effect_reading = ATOMIC_FLAG_INIT;

std::wstring EffectKey(int i) {
  const char* key = g_info.effects[i].key;
  return std::wstring(key, key + std::strlen(key));  // ini keys are ASCII
}

void PublishEffects() {
  char text[16];
  std::snprintf(text, sizeof(text), "%u", unsigned(g_effect_mask.load()));
  SetEnvironmentVariableA("PCS_FX_MASK", text);
}

bool ReadEffects(uint32_t& mask) {
  char text[16];
  DWORD n = GetEnvironmentVariableA("PCS_FX_MASK", text, sizeof(text));
  if (!n || n >= sizeof(text)) return false;
  char* end = nullptr;
  unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end || value > 255) return false;
  mask = uint32_t(value);
  return true;
}

// ------------------------------------------------------------------- menu languages

// The ini keeps 0 Portuguese and 1 English as before; -1 follows the game's language.
enum UiLang { kUiPt, kUiEn, kUiEs, kUiFr, kUiDe, kUiIt, kUiNl, kUiRu, kUiLangCount };
const wchar_t* const k_ui_lang_name[kUiLangCount] = {L"Português", L"English", L"Español", L"Français",
                                                     L"Deutsch",   L"Italiano", L"Nederlands", L"Русский"};

int UiLangOfGame(int xc_language) {
  switch (xc_language) {  // XC_LANGUAGE
    case 1: return kUiEn;
    case 3: return kUiDe;
    case 4: return kUiFr;
    case 5: return kUiEs;
    case 6: return kUiIt;
    case 9: return kUiPt;
    case 12: return kUiRu;
  }
  return -1;
}

int UiLangOfWindows() {
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_PORTUGUESE: return kUiPt;
    case LANG_SPANISH: return kUiEs;
    case LANG_FRENCH: return kUiFr;
    case LANG_GERMAN: return kUiDe;
    case LANG_ITALIAN: return kUiIt;
    case LANG_DUTCH: return kUiNl;
    case LANG_RUSSIAN: return kUiRu;
  }
  return kUiEn;
}

int UiLangAuto() {
  int l = UiLangOfGame(g_opt.language);
  return l >= 0 ? l : UiLangOfWindows();
}

int UiLang() { return g_opt.ui_language >= 0 ? g_opt.ui_language : UiLangAuto(); }

// The other languages, found by the English text (format specifiers kept).
struct Translation {
  const wchar_t* en;
  const wchar_t* other[kUiLangCount - kUiEs];  // es, fr, de, it, nl, ru
};
const Translation k_tr[] = {
    {L"Monitor", {L"Monitor", L"Moniteur", L"Monitor", L"Monitor", L"Monitor", L"Монитор"}},
    {L"Volume", {L"Volumen", L"Volume", L"Lautstärke", L"Volume", L"Volume", L"Громкость"}},
    {L"Console default", {L"Predeterminado de la consola", L"Langue de la console", L"Konsolenstandard", L"Predefinita della console", L"Standaard van de console", L"По умолчанию консоли"}},
    {L"On", {L"Activado", L"Activé", L"An", L"Attivo", L"Aan", L"Вкл."}},
    {L"Off", {L"Desactivado", L"Désactivé", L"Aus", L"Disattivato", L"Uit", L"Выкл."}},
    {L"Display mode", {L"Modo de pantalla", L"Mode d'affichage", L"Anzeigemodus", L"Modalità schermo", L"Weergavemodus", L"Режим экрана"}},
    {L"Windowed", {L"Ventana", L"Fenêtré", L"Fenster", L"Finestra", L"Venster", L"Оконный"}},
    {L"Borderless fullscreen", {L"Pantalla completa sin bordes", L"Plein écran sans bordure", L"Randloses Vollbild", L"Schermo intero senza bordi", L"Randloos volledig scherm", L"Полноэкранный без рамки"}},
    {L"Exclusive fullscreen (when focused)", {L"Pantalla completa exclusiva (al enfocar la ventana)", L"Plein écran exclusif (quand la fenêtre est active)", L"Exklusives Vollbild (wenn das Fenster aktiv ist)", L"Schermo intero esclusivo (con la finestra attiva)", L"Exclusief volledig scherm (als het venster actief is)", L"Эксклюзивный полноэкранный (при активном окне)"}},
    {L"Exclusive unavailable: borderless", {L"Exclusiva no disponible: sin bordes", L"Exclusif indisponible : sans bordure", L"Exklusiv nicht verfügbar: randlos", L"Esclusivo non disponibile: senza bordi", L"Exclusief niet beschikbaar: randloos", L"Эксклюзивный недоступен: без рамки"}},
    {L"Exclusive fullscreen", {L"Pantalla completa exclusiva", L"Plein écran exclusif", L"Exklusives Vollbild", L"Schermo intero esclusivo", L"Exclusief volledig scherm", L"Эксклюзивный полноэкранный"}},
    {L"Refresh rate", {L"Frecuencia de actualización", L"Fréquence de rafraîchissement", L"Bildwiederholrate", L"Frequenza di aggiornamento", L"Verversingssnelheid", L"Частота обновления"}},
    {L"Monitor refresh rate (%d Hz)", {L"La del monitor (%d Hz)", L"Celle du moniteur (%d Hz)", L"Die des Monitors (%d Hz)", L"Quella del monitor (%d Hz)", L"Die van de monitor (%d Hz)", L"Как у монитора (%d Гц)"}},
    {L"Vertical sync (VSync)", {L"Sincronización vertical (VSync)", L"Synchronisation verticale (VSync)", L"Vertikale Synchronisation (VSync)", L"Sincronizzazione verticale (VSync)", L"Verticale synchronisatie (VSync)", L"Вертикальная синхронизация (VSync)"}},
    {L"Frame limit", {L"Límite de fotogramas", L"Limite d'images", L"Bildratenbegrenzung", L"Limite di fotogrammi", L"Beeldlimiet", L"Ограничение кадров"}},
    {L"Monitor rate (%d fps)", {L"La del monitor (%d fps)", L"Celle du moniteur (%d fps)", L"Die des Monitors (%d fps)", L"Quella del monitor (%d fps)", L"Die van de monitor (%d fps)", L"Как у монитора (%d к/с)"}},
    {L"Unlimited", {L"Sin límite", L"Illimité", L"Unbegrenzt", L"Illimitato", L"Onbeperkt", L"Без ограничения"}},
    {L"Internal resolution", {L"Resolución interna", L"Résolution interne", L"Interne Auflösung", L"Risoluzione interna", L"Interne resolutie", L"Внутреннее разрешение"}},
    {L"Monitor resolution", {L"Resolución del monitor", L"Résolution du moniteur", L"Monitorauflösung", L"Risoluzione del monitor", L"Monitorresolutie", L"Разрешение монитора"}},
    {L"Upscaling", {L"Escalado", L"Mise à l'échelle", L"Hochskalierung", L"Upscaling", L"Opschaling", L"Масштабирование"}},
    {L"Aspect ratio", {L"Relación de aspecto", L"Format d'image", L"Seitenverhältnis", L"Proporzioni", L"Beeldverhouding", L"Соотношение сторон"}},
    {L"Stretch to the screen", {L"Estirar a la pantalla", L"Étirer à l'écran", L"Auf den Bildschirm strecken", L"Allarga allo schermo", L"Uitrekken tot het scherm", L"Растянуть на экран"}},
    {L"16:9 (original, with bars)", {L"16:9 (original, con barras)", L"16:9 (original, avec bandes)", L"16:9 (original, mit Balken)", L"16:9 (originale, con bande)", L"16:9 (origineel, met balken)", L"16:9 (оригинал, с полосами)"}},
    {L"Anti-aliasing", {L"Antialiasing", L"Anticrénelage", L"Kantenglättung", L"Antialiasing", L"Anti-aliasing", L"Сглаживание"}},
    {L"Multisampling (MSAA)", {L"Multimuestreo (MSAA)", L"Multi-échantillonnage (MSAA)", L"Multisampling (MSAA)", L"Multicampionamento (MSAA)", L"Multisampling (MSAA)", L"Мультисэмплинг (MSAA)"}},
    {L"Anisotropic filtering", {L"Filtrado anisotrópico", L"Filtrage anisotrope", L"Anisotrope Filterung", L"Filtro anisotropico", L"Anisotrope filtering", L"Анизотропная фильтрация"}},
    {L"Game default", {L"El del juego", L"Celui du jeu", L"Wie im Spiel", L"Quello del gioco", L"Zoals in het spel", L"Как в игре"}},
    {L"Graphics API", {L"API gráfica", L"API graphique", L"Grafik-API", L"API grafica", L"Grafische API", L"Графический API"}},
    {L"Graphics card", {L"Tarjeta gráfica", L"Carte graphique", L"Grafikkarte", L"Scheda video", L"Videokaart", L"Видеокарта"}},
    {L"Automatic (Windows default)", {L"Automática (predeterminada de Windows)", L"Automatique (par défaut de Windows)", L"Automatisch (Windows-Standard)", L"Automatica (predefinita di Windows)", L"Automatisch (standaard van Windows)", L"Автоматически (по умолчанию Windows)"}},
    {L"Show FPS", {L"Mostrar FPS", L"Afficher les FPS", L"FPS anzeigen", L"Mostra FPS", L"FPS tonen", L"Показывать FPS"}},
    {L"Yes", {L"Sí", L"Oui", L"Ja", L"Sì", L"Ja", L"Да"}},
    {L"No", {L"No", L"Non", L"Nein", L"No", L"Nee", L"Нет"}},
    {L"Menu language", {L"Idioma del menú", L"Langue du menu", L"Menüsprache", L"Lingua del menu", L"Menutaal", L"Язык меню"}},
    {L"Automatic (%ls)", {L"Automático (%ls)", L"Automatique (%ls)", L"Automatisch (%ls)", L"Automatica (%ls)", L"Automatisch (%ls)", L"Автоматически (%ls)"}},
    {L"Game language", {L"Idioma del juego", L"Langue du jeu", L"Spielsprache", L"Lingua del gioco", L"Speltaal", L"Язык игры"}},
    {L"Back to the game", {L"Volver al juego", L"Retour au jeu", L"Zurück zum Spiel", L"Torna al gioco", L"Terug naar het spel", L"Вернуться в игру"}},
    {L"Restart game to apply *", {L"Reiniciar el juego para aplicar *", L"Redémarrer le jeu pour appliquer *", L"Spiel neu starten, um anzuwenden *", L"Riavvia il gioco per applicare *", L"Spel herstarten om toe te passen *", L"Перезапустить игру для применения *"}},
    {L"Quit game", {L"Salir del juego", L"Quitter le jeu", L"Spiel beenden", L"Esci dal gioco", L"Spel afsluiten", L"Выйти из игры"}},
    {L"Windowed, borderless fullscreen (quick Alt+Tab) or exclusive fullscreen.", {L"Ventana, pantalla completa sin bordes (Alt+Tab rápido) o pantalla completa exclusiva.", L"Fenêtré, plein écran sans bordure (Alt+Tab rapide) ou plein écran exclusif.", L"Fenster, randloses Vollbild (schnelles Alt+Tab) oder exklusives Vollbild.", L"Finestra, schermo intero senza bordi (Alt+Tab rapido) o schermo intero esclusivo.", L"Venster, randloos volledig scherm (snel Alt+Tab) of exclusief volledig scherm.", L"Окно, полноэкранный без рамки (быстрый Alt+Tab) или эксклюзивный полноэкранный режим."}},
    {L"Monitor used to display the game.", {L"Monitor en el que se muestra el juego.", L"Moniteur utilisé pour afficher le jeu.", L"Monitor, auf dem das Spiel angezeigt wird.", L"Monitor su cui viene mostrato il gioco.", L"Monitor waarop het spel wordt getoond.", L"Монитор, на котором показывается игра."}},
    {L"Monitor refresh rate in exclusive fullscreen.", {L"Frecuencia del monitor en pantalla completa exclusiva.", L"Fréquence du moniteur en plein écran exclusif.", L"Bildwiederholrate des Monitors im exklusiven Vollbild.", L"Frequenza del monitor a schermo intero esclusivo.", L"Verversingssnelheid van de monitor in exclusief volledig scherm.", L"Частота монитора в эксклюзивном полноэкранном режиме."}},
    {L"Synchronizes frames with the monitor to prevent tearing, with slightly more latency.", {L"Sincroniza los fotogramas con el monitor para evitar cortes en la imagen, con algo más de latencia.", L"Synchronise les images avec le moniteur pour éviter le déchirement, avec un peu plus de latence.", L"Synchronisiert die Bilder mit dem Monitor gegen Tearing, mit etwas mehr Latenz.", L"Sincronizza i fotogrammi con il monitor per evitare il tearing, con un po' più di latenza.", L"Synchroniseert de beelden met de monitor tegen tearing, met iets meer vertraging.", L"Синхронизирует кадры с монитором, чтобы не было разрывов, с чуть большей задержкой."}},
    {L"60 as on the console, the monitor's rate (smoother on 120/144 Hz screens) or no limit. 30 saves power.", {L"60 como en la consola, la del monitor (más fluido en pantallas de 120/144 Hz) o sin límite. 30 ahorra energía.", L"60 comme sur la console, celle du moniteur (plus fluide sur les écrans 120/144 Hz) ou illimité. 30 économise l'énergie.", L"60 wie auf der Konsole, die des Monitors (flüssiger auf 120/144-Hz-Bildschirmen) oder unbegrenzt. 30 spart Energie.", L"60 come su console, quella del monitor (più fluido su schermi a 120/144 Hz) o senza limite. 30 risparmia energia.", L"60 zoals op de console, die van de monitor (vloeiender op 120/144 Hz-schermen) of onbeperkt. 30 bespaart energie.", L"60 как на консоли, частота монитора (плавнее на экранах 120/144 Гц) или без ограничения. 30 экономит энергию."}},
    {L"This game advances one step per frame: above 60 it would run too fast. 30 saves power on laptops.", {L"Este juego avanza un paso por fotograma: por encima de 60 iría demasiado rápido. 30 ahorra energía en portátiles.", L"Ce jeu avance d'un pas par image : au-delà de 60, il irait trop vite. 30 économise l'énergie sur les portables.", L"Dieses Spiel rechnet einen Schritt pro Bild: über 60 liefe es zu schnell. 30 spart Energie auf Laptops.", L"Questo gioco avanza di un passo per fotogramma: oltre 60 andrebbe troppo veloce. 30 risparmia energia sui portatili.", L"Dit spel rekent één stap per beeld: boven 60 zou het te snel lopen. 30 bespaart energie op laptops.", L"Игра делает один шаг за кадр: выше 60 она шла бы слишком быстро. 30 экономит энергию на ноутбуках."}},
    {L"Higher rendering resolutions give a sharper image and increase GPU load.", {L"Resoluciones más altas dan una imagen más nítida y exigen más a la tarjeta gráfica.", L"Une résolution plus élevée donne une image plus nette et sollicite davantage la carte graphique.", L"Höhere Auflösungen ergeben ein schärferes Bild und belasten die Grafikkarte stärker.", L"Risoluzioni più alte danno un'immagine più nitida e impegnano di più la scheda video.", L"Een hogere resolutie geeft een scherper beeld en belast de videokaart meer.", L"Более высокое разрешение даёт более чёткое изображение и сильнее нагружает видеокарту."}},
    {L"How the picture is enlarged when the internal resolution is below the screen's. AMD FSR 1.0 rebuilds edges and sharpens.", {L"Cómo se amplía la imagen cuando la resolución interna es menor que la de la pantalla. AMD FSR 1.0 reconstruye los bordes y aumenta la nitidez.", L"Façon d'agrandir l'image quand la résolution interne est inférieure à celle de l'écran. AMD FSR 1.0 reconstruit les contours et accentue la netteté.", L"Wie das Bild vergrößert wird, wenn die interne Auflösung unter der des Bildschirms liegt. AMD FSR 1.0 rekonstruiert Kanten und schärft nach.", L"Come viene ingrandita l'immagine quando la risoluzione interna è inferiore a quella dello schermo. AMD FSR 1.0 ricostruisce i bordi e aumenta la nitidezza.", L"Hoe het beeld wordt vergroot als de interne resolutie lager is dan die van het scherm. AMD FSR 1.0 herstelt randen en verscherpt.", L"Как увеличивается изображение, когда внутреннее разрешение ниже экранного. AMD FSR 1.0 восстанавливает края и повышает резкость."}},
    {L"The game is 16:9. On other screen shapes: with bars or stretched.", {L"El juego es 16:9. En pantallas de otro formato: con barras o estirado.", L"Le jeu est en 16:9. Sur d'autres formats d'écran : avec bandes ou étiré.", L"Das Spiel ist 16:9. Auf anderen Bildschirmformaten: mit Balken oder gestreckt.", L"Il gioco è in 16:9. Su schermi di altro formato: con bande o allargato.", L"Het spel is 16:9. Op schermen met een andere vorm: met balken of uitgerekt.", L"Игра в формате 16:9. На экранах другой формы: с полосами или растянуто."}},
    {L"FXAA smooths jagged edges with minimal performance cost.", {L"FXAA suaviza los bordes dentados con un coste mínimo.", L"Le FXAA adoucit les bords crénelés pour un coût minime.", L"FXAA glättet gezackte Kanten bei minimalem Leistungsbedarf.", L"FXAA ammorbidisce i bordi seghettati con un costo minimo.", L"FXAA maakt gekartelde randen glad tegen minimale kosten.", L"FXAA сглаживает зубчатые края почти без потерь производительности."}},
    {L"Smooths the edges of the 3D scene by drawing it with several samples per pixel. Sharper than FXAA, more work for the graphics card.", {L"Suaviza los bordes de la escena 3D dibujándola con varias muestras por píxel. Más nítido que FXAA, más trabajo para la tarjeta gráfica.", L"Adoucit les bords de la scène 3D en la dessinant avec plusieurs échantillons par pixel. Plus net que le FXAA, plus de travail pour la carte graphique.", L"Glättet die Kanten der 3D-Szene mit mehreren Samples pro Pixel. Schärfer als FXAA, mehr Arbeit für die Grafikkarte.", L"Ammorbidisce i bordi della scena 3D disegnandola con più campioni per pixel. Più nitido dell'FXAA, più lavoro per la scheda video.", L"Strijkt de randen van de 3D-scène glad door meerdere samples per pixel te tekenen. Scherper dan FXAA, meer werk voor de videokaart.", L"Сглаживает края 3D-сцены, рисуя её с несколькими выборками на пиксель. Чётче, чем FXAA, но сильнее нагружает видеокарту."}},
    {L"Sharpens textures viewed at an angle, such as the ground.", {L"Hace nítidas las texturas vistas en ángulo, como el suelo.", L"Rend nettes les textures vues en biais, comme le sol.", L"Schärft schräg betrachtete Texturen, etwa den Boden.", L"Rende nitide le texture viste di sbieco, come il terreno.", L"Maakt schuin bekeken texturen scherp, zoals de grond.", L"Делает чёткими текстуры под углом, например землю."}},
    {L"Direct3D 11. Vulkan needs the vulkan folder (DXVK) next to the game.", {L"Direct3D 11. Vulkan necesita la carpeta vulkan (DXVK) junto al juego.", L"Direct3D 11. Vulkan nécessite le dossier vulkan (DXVK) à côté du jeu.", L"Direct3D 11. Vulkan braucht den Ordner vulkan (DXVK) neben dem Spiel.", L"Direct3D 11. Vulkan richiede la cartella vulkan (DXVK) accanto al gioco.", L"Direct3D 11. Vulkan heeft de map vulkan (DXVK) naast het spel nodig.", L"Direct3D 11. Для Vulkan нужна папка vulkan (DXVK) рядом с игрой."}},
    {L"DirectX 11 or Vulkan (through DXVK). If something looks wrong or stutters, try the other one. Restarts the game.", {L"DirectX 11 o Vulkan (mediante DXVK). Si algo se ve mal o da tirones, prueba la otra. Reinicia el juego.", L"DirectX 11 ou Vulkan (via DXVK). Si quelque chose s'affiche mal ou saccade, essayez l'autre. Redémarre le jeu.", L"DirectX 11 oder Vulkan (über DXVK). Wenn etwas falsch aussieht oder ruckelt, probiere die andere. Startet das Spiel neu.", L"DirectX 11 o Vulkan (tramite DXVK). Se qualcosa appare sbagliato o scatta, prova l'altra. Riavvia il gioco.", L"DirectX 11 of Vulkan (via DXVK). Als iets er verkeerd uitziet of hapert, probeer de andere. Herstart het spel.", L"DirectX 11 или Vulkan (через DXVK). Если что-то выглядит неправильно или подтормаживает, попробуйте другой. Перезапускает игру."}},
    {L"Graphics card used to render the game.", {L"Tarjeta gráfica que dibuja el juego.", L"Carte graphique qui affiche le jeu.", L"Grafikkarte, die das Spiel darstellt.", L"Scheda video che disegna il gioco.", L"Videokaart die het spel tekent.", L"Видеокарта, которая отрисовывает игру."}},
    {L"Shows frames per second in the corner of the screen.", {L"Muestra los fotogramas por segundo en la esquina de la pantalla.", L"Affiche les images par seconde dans le coin de l'écran.", L"Zeigt die Bilder pro Sekunde in der Bildschirmecke.", L"Mostra i fotogrammi al secondo nell'angolo dello schermo.", L"Toont de beelden per seconde in de hoek van het scherm.", L"Показывает кадры в секунду в углу экрана."}},
    {L"Language of this settings menu. Automatic follows the game's language. Changes apply immediately.", {L"Idioma de este menú. Automático sigue el idioma del juego. El cambio es inmediato.", L"Langue de ce menu. Automatique suit la langue du jeu. Le changement est immédiat.", L"Sprache dieses Menüs. Automatisch folgt der Spielsprache. Die Änderung gilt sofort.", L"Lingua di questo menu. Automatica segue la lingua del gioco. La modifica è immediata.", L"Taal van dit menu. Automatisch volgt de taal van het spel. De wijziging geldt meteen.", L"Язык этого меню. «Автоматически» следует языку игры. Изменение применяется сразу."}},
    {L"Language of the game's text and commentary (as available on the disc).", {L"Idioma de los textos y la narración del juego (lo que tenga el disco).", L"Langue des textes et des commentaires du jeu (selon le disque).", L"Sprache der Texte und des Kommentars im Spiel (je nach Disc).", L"Lingua dei testi e della telecronaca del gioco (secondo il disco).", L"Taal van de teksten en het commentaar van het spel (zoals op de disc).", L"Язык текста и комментариев игры (что есть на диске)."}},
    {L"Overall game volume.", {L"Volumen general del juego.", L"Volume général du jeu.", L"Gesamtlautstärke des Spiels.", L"Volume generale del gioco.", L"Algemeen volume van het spel.", L"Общая громкость игры."}},
    {L"Saves settings and returns to the game.", {L"Guarda los ajustes y vuelve al juego.", L"Enregistre les réglages et revient au jeu.", L"Speichert die Einstellungen und kehrt zum Spiel zurück.", L"Salva le impostazioni e torna al gioco.", L"Slaat de instellingen op en keert terug naar het spel.", L"Сохраняет настройки и возвращает в игру."}},
    {L"Restarts the game to apply settings marked with *.", {L"Reinicia el juego para aplicar las opciones marcadas con *.", L"Redémarre le jeu pour appliquer les options marquées d'un *.", L"Startet das Spiel neu, um die mit * markierten Optionen anzuwenden.", L"Riavvia il gioco per applicare le opzioni contrassegnate con *.", L"Herstart het spel om de met * gemarkeerde opties toe te passen.", L"Перезапускает игру, чтобы применить параметры, отмеченные *."}},
    {L"Saves settings and quits the game.", {L"Guarda los ajustes y cierra el juego.", L"Enregistre les réglages et quitte le jeu.", L"Speichert die Einstellungen und beendet das Spiel.", L"Salva le impostazioni ed esce dal gioco.", L"Slaat de instellingen op en sluit het spel af.", L"Сохраняет настройки и закрывает игру."}},
    {L"PC SETTINGS", {L"AJUSTES DE PC", L"PARAMÈTRES PC", L"PC-EINSTELLUNGEN", L"IMPOSTAZIONI PC", L"PC-INSTELLINGEN", L"НАСТРОЙКИ ПК"}},
    {L"* APPLIES AFTER RESTARTING", {L"* SE APLICA AL REINICIAR", L"* S'APPLIQUE AU REDÉMARRAGE", L"* GILT NACH DEM NEUSTART", L"* SI APPLICA AL RIAVVIO", L"* GELDT NA HERSTARTEN", L"* ПРИМЕНЯЕТСЯ ПОСЛЕ ПЕРЕЗАПУСКА"}},
    {L"SELECT", {L"SELECCIONAR", L"SÉLECTIONNER", L"AUSWÄHLEN", L"SELEZIONA", L"SELECTEREN", L"ВЫБРАТЬ"}},
    {L"BACK", {L"VOLVER", L"RETOUR", L"ZURÜCK", L"INDIETRO", L"TERUG", L"НАЗАД"}},
    {L"ENTER / CLICK: SELECT   ESC / F1: BACK", {L"ENTER / CLIC: SELECCIONAR   ESC / F1: VOLVER", L"ENTRÉE / CLIC : SÉLECTIONNER   ÉCHAP / F1 : RETOUR", L"ENTER / KLICK: AUSWÄHLEN   ESC / F1: ZURÜCK", L"INVIO / CLIC: SELEZIONA   ESC / F1: INDIETRO", L"ENTER / KLIK: SELECTEREN   ESC / F1: TERUG", L"ENTER / ЩЕЛЧОК: ВЫБРАТЬ   ESC / F1: НАЗАД"}},
    {L"settings", {L"ajustes", L"paramètres", L"Einstellungen", L"impostazioni", L"instellingen", L"настройки"}},
    {L"%ls  •  PC  •  video, display and system  •  game paused", {L"%ls  •  PC  •  vídeo, pantalla y sistema  •  juego en pausa", L"%ls  •  PC  •  vidéo, affichage et système  •  jeu en pause", L"%ls  •  PC  •  Video, Anzeige und System  •  Spiel pausiert", L"%ls  •  PC  •  video, schermo e sistema  •  gioco in pausa", L"%ls  •  PC  •  video, scherm en systeem  •  spel gepauzeerd", L"%ls  •  ПК  •  видео, экран и система  •  игра на паузе"}},
    {L"VIDEO", {L"VÍDEO", L"VIDÉO", L"VIDEO", L"VIDEO", L"VIDEO", L"ВИДЕО"}},
    {L"SYSTEM", {L"SISTEMA", L"SYSTÈME", L"SYSTEM", L"SISTEMA", L"SYSTEEM", L"СИСТЕМА"}},
    {L"* applies after restarting", {L"* se aplica al reiniciar", L"* s'applique au redémarrage", L"* gilt nach dem Neustart", L"* si applica al riavvio", L"* geldt na herstarten", L"* применяется после перезапуска"}},
    {L"SELECT (Enter / click)", {L"SELECCIONAR (Enter / clic)", L"SÉLECTIONNER (Entrée / clic)", L"AUSWÄHLEN (Enter / Klick)", L"SELEZIONA (Invio / clic)", L"SELECTEREN (Enter / klik)", L"ВЫБРАТЬ (Enter / щелчок)"}},
    {L"BACK (Esc / F1)", {L"VOLVER (Esc / F1)", L"RETOUR (Échap / F1)", L"ZURÜCK (Esc / F1)", L"INDIETRO (Esc / F1)", L"TERUG (Esc / F1)", L"НАЗАД (Esc / F1)"}},
    {L"◀ ▶  change (right click goes back)      Alt+Enter  fullscreen", {L"◀ ▶  cambiar (el botón derecho retrocede)      Alt+Enter  pantalla completa", L"◀ ▶  modifier (clic droit pour revenir)      Alt+Entrée  plein écran", L"◀ ▶  ändern (Rechtsklick geht zurück)      Alt+Enter  Vollbild", L"◀ ▶  cambia (il tasto destro torna indietro)      Alt+Invio  schermo intero", L"◀ ▶  wijzigen (rechtsklik gaat terug)      Alt+Enter  volledig scherm", L"◀ ▶  изменить (правый щелчок — назад)      Alt+Enter  полный экран"}},
    // Effects of the games (pc_game_info.h).
    {L"Vignette", {L"Viñeta", L"Vignettage", L"Vignette", L"Vignettatura", L"Vignettering", L"Виньетка"}},
    {L"Darkening around the edges of the image.", {L"Oscurecimiento de los bordes de la imagen.", L"Assombrissement des bords de l'image.", L"Abdunkelung der Bildränder.", L"Scurimento dei bordi dell'immagine.", L"Donkere randen rond het beeld.", L"Затемнение по краям изображения."}},
    {L"Cinematic brightness and contrast", {L"Brillo y contraste cinematográficos", L"Luminosité et contraste cinématographiques", L"Kinoreife Helligkeit und Kontrast", L"Luminosità e contrasto cinematografici", L"Filmische helderheid en contrast", L"Кинематографичные яркость и контраст"}},
    {L"The game's final brightness and contrast adjustment.", {L"El ajuste final de brillo y contraste del juego.", L"Le réglage final de luminosité et de contraste du jeu.", L"Die abschließende Helligkeits- und Kontrastanpassung des Spiels.", L"La regolazione finale di luminosità e contrasto del gioco.", L"De laatste aanpassing van helderheid en contrast door het spel.", L"Финальная настройка яркости и контраста в игре."}},
    {L"Bloom", {L"Resplandor (bloom)", L"Flou lumineux (bloom)", L"Bloom", L"Bloom", L"Bloom", L"Свечение (bloom)"}},
    {L"Glow around bright lights and surfaces.", {L"Brillo alrededor de luces y superficies claras.", L"Halo autour des lumières et des surfaces claires.", L"Leuchten um helle Lichter und Flächen.", L"Alone attorno a luci e superfici chiare.", L"Gloed rond felle lichten en oppervlakken.", L"Ореол вокруг ярких огней и поверхностей."}},
    {L"Motion blur", {L"Desenfoque de movimiento", L"Flou de mouvement", L"Bewegungsunschärfe", L"Sfocatura di movimento", L"Bewegingsonscherpte", L"Размытие в движении"}},
    {L"Blur on fast movement of the camera or objects.", {L"Desenfoque en los movimientos rápidos de la cámara o los objetos.", L"Flou lors des mouvements rapides de la caméra ou des objets.", L"Unschärfe bei schnellen Kamera- oder Objektbewegungen.", L"Sfocatura nei movimenti rapidi della telecamera o degli oggetti.", L"Onscherpte bij snelle bewegingen van de camera of objecten.", L"Размытие при быстром движении камеры или объектов."}},
    {L"Depth of field", {L"Profundidad de campo", L"Profondeur de champ", L"Tiefenschärfe", L"Profondità di campo", L"Scherptediepte", L"Глубина резкости"}},
    {L"Background blur that focuses the view.", {L"Desenfoque del fondo que centra la vista.", L"Flou de l'arrière-plan qui concentre le regard.", L"Hintergrundunschärfe, die den Blick lenkt.", L"Sfocatura dello sfondo che concentra lo sguardo.", L"Onscherpe achtergrond die de blik richt.", L"Размытие фона, которое фокусирует взгляд."}},
};

const wchar_t* T(const wchar_t* pt, const wchar_t* en) {
  int l = UiLang();
  if (l == kUiPt) return pt;
  if (l == kUiEn) return en;
  for (const Translation& t : k_tr)
    if (std::wcscmp(t.en, en) == 0) return t.other[l - kUiEs];
  return en;
}

// Internal resolution presets: lines drawn (the games render 720); 0 = the monitor's.
struct ResPreset { int h; const wchar_t* pt; const wchar_t* en; };
const ResPreset k_res[] = {
    {720, L"720p (original)", L"720p (original)"},
    {1080, L"1080p (Full HD)", L"1080p (Full HD)"},
    {1440, L"1440p (QHD)", L"1440p (QHD)"},
    {2160, L"2160p (4K)", L"2160p (4K)"},
    {0, L"Resolução do monitor", L"Monitor resolution"},
    {4320, L"4320p (8K)", L"4320p (8K)"},
    // Below the original, for slower cards: the window keeps its size and FSR can enlarge.
    {540, L"540p", L"540p"},
    {360, L"360p", L"360p"},
};
constexpr int kNumRes = int(sizeof(k_res) / sizeof(k_res[0]));
// Menu order (the ini keeps the index above: later presets were appended).
const int k_res_order[] = {7, 6, 0, 1, 2, 3, 5, 4};
const int k_fps[] = {60, kFpsMonitor, 0, 30};
const int k_fps_fixed[] = {60, 30};
const int k_aniso[] = {0, 2, 4, 8, 16};
const int k_msaa[] = {0, 2, 4, 8};

int Clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int IniGet(const wchar_t* key, int def) {
  if (!g_use_ini) return def;
  return int(GetPrivateProfileIntW(L"opcoes", key, def, g_ini));
}

void IniPut(const wchar_t* key, int v) {
  wchar_t s[16];
  swprintf(s, 16, L"%d", v);
  WritePrivateProfileStringW(L"opcoes", key, s, g_ini);
}

void SaveIni() {
  if (!g_use_ini) return;
  IniPut(L"modo_tela", g_opt.display);
  IniPut(L"monitor", g_opt.monitor);
  IniPut(L"taxa_hz", g_opt.refresh);
  IniPut(L"vsync", g_opt.vsync);
  IniPut(L"limite_fps", g_opt.fps);
  IniPut(L"resolucao", g_opt.scale);
  IniPut(L"proporcao", g_opt.aspect);
  IniPut(L"ampliacao", g_opt.upscale);
  IniPut(L"fxaa", g_opt.aa);
  IniPut(L"anisotropico", g_opt.aniso);
  IniPut(L"msaa", g_opt.msaa);
  IniPut(L"gpu", g_opt.adapter);
  IniPut(L"api_grafica", g_opt.api);
  IniPut(L"idioma", g_opt.language);
  IniPut(L"volume", g_opt.volume);
  IniPut(L"idioma_menu", g_opt.ui_language);
  IniPut(L"mostrar_fps", g_opt.show_fps);
  for (int i = 0; i < g_info.effect_count; ++i)
    IniPut(EffectKey(i).c_str(), EffectEnabled(i) ? 1 : 0);
}

bool LanguageListed(int id) {
  for (int i = 0; i < g_info.language_count; ++i)
    if (g_info.languages[i].id == id) return true;
  return false;
}

const wchar_t* LanguageName(int id) {
  for (int i = 0; i < g_info.language_count; ++i)
    if (g_info.languages[i].id == id) return g_info.languages[i].name;
  return T(L"Padrão do console", L"Console default");
}

int EnvInt(const char* name, int def) {
  const char* e = std::getenv(name);
  return e && *e ? std::atoi(e) : def;
}

// ------------------------------------------------------------------- monitors and GPUs

struct Monitor {
  RECT rc, work;
  wchar_t device[32];
#ifndef _WIN32
  SDL_DisplayID id = 0;
  int sdk_index = 0;
#endif
  wchar_t name[96];
};
std::vector<Monitor> g_mon;
std::vector<int> g_rates;
std::vector<std::wstring> g_gpu_name;
std::vector<int> g_gpu_index;
bool g_gpus_listed = false;

#ifdef _WIN32
BOOL CALLBACK MonitorCb(HMONITOR h, HDC, LPRECT, LPARAM) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(h, &mi)) return TRUE;
  Monitor m{};
  m.rc = mi.rcMonitor;
  m.work = mi.rcWork;
  wcsncpy(m.device, mi.szDevice, CCHDEVICENAME - 1);
  DISPLAY_DEVICEW dd{};
  dd.cb = sizeof(dd);
  if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0])
    swprintf(m.name, 96, L"%ls (%ldx%ld)", dd.DeviceString, long(m.rc.right - m.rc.left),
             long(m.rc.bottom - m.rc.top));
  else
    swprintf(m.name, 96, L"%ldx%ld", long(m.rc.right - m.rc.left), long(m.rc.bottom - m.rc.top));
  // The primary monitor first, so index 0 is the Windows default.
  if (mi.dwFlags & MONITORINFOF_PRIMARY)
    g_mon.insert(g_mon.begin(), m);
  else
    g_mon.push_back(m);
  return TRUE;
}

#endif

void MonitorsRefresh() {
  g_mon.clear();
#ifdef _WIN32
  EnumDisplayMonitors(nullptr, nullptr, MonitorCb, 0);
#else
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  for (int i = 0; displays && i < count; ++i) {
    SDL_Rect rect{}, work{};
    if (!SDL_GetDisplayBounds(displays[i], &rect)) continue;
    if (!SDL_GetDisplayUsableBounds(displays[i], &work)) work = rect;
    Monitor m{};
    m.id = displays[i];
    m.sdk_index = i;
    m.rc = {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    m.work = {work.x, work.y, work.x + work.w, work.y + work.h};
    swprintf(m.name, 96, L"%d (%dx%d)", i + 1, rect.w, rect.h);
    g_mon.push_back(m);
  }
  SDL_free(displays);
#endif
  if (g_mon.empty()) {
    Monitor m{};
#ifdef _WIN32
    SetRect(&m.rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
#else
    m.rc = {0, 0, 1280, 720};
#endif
    m.work = m.rc;
    wcscpy(m.name, L"?");
    g_mon.push_back(m);
  }
}

const Monitor& MonitorCurrent() {
  if (g_mon.empty()) MonitorsRefresh();
  return g_mon[size_t(g_opt.monitor) < g_mon.size() ? size_t(g_opt.monitor) : 0];
}

int MonitorCurrentHz() {
#ifdef _WIN32
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(MonitorCurrent().device, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    return int(dm.dmDisplayFrequency);
#else
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(MonitorCurrent().id))
    if (mode->refresh_rate > 1) return int(mode->refresh_rate + 0.5f);
#endif
  return 60;
}

// The display's rate for "monitor rate" pacing, cached (asked of Windows at most every 2 s).
std::atomic<int> g_display_hz{60};
std::atomic<int64_t> g_display_hz_at{0};

int DisplayHz() {
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  if (now - g_display_hz_at.load() > 2000) {
    g_display_hz_at = now;
    int hz = g_opt.display == 2 && g_opt.refresh ? g_opt.refresh : MonitorCurrentHz();
    g_display_hz = std::clamp(hz, 30, 1000);
  }
  return g_display_hz.load();
}

// Frames per second the game should run at (0 = no limit), from the setting.
int TargetFps() {
  if (g_opt.fps == kFpsMonitor) return DisplayHz();
  return g_opt.fps;
}

void RatesRefresh() {
  const Monitor& m = MonitorCurrent();
  g_rates.clear();
#ifdef _WIN32
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsW(m.device, i, &dm); ++i) {
    if (LONG(dm.dmPelsWidth) != m.rc.right - m.rc.left || LONG(dm.dmPelsHeight) != m.rc.bottom - m.rc.top ||
        dm.dmDisplayFrequency <= 1)
      continue;
    int hz = int(dm.dmDisplayFrequency);
    if (std::find(g_rates.begin(), g_rates.end(), hz) == g_rates.end()) g_rates.push_back(hz);
  }
#else
  int count = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(m.id, &count);
  for (int i = 0; modes && i < count; ++i) {
    const auto* mode = modes[i];
    if (mode->w != m.rc.right - m.rc.left || mode->h != m.rc.bottom - m.rc.top || mode->refresh_rate <= 1) continue;
    const int hz = int(mode->refresh_rate + 0.5f);
    if (std::find(g_rates.begin(), g_rates.end(), hz) == g_rates.end()) g_rates.push_back(hz);
  }
  SDL_free(modes);
#endif
  std::sort(g_rates.begin(), g_rates.end());
}

// Graphics API. Vulkan is DXVK: vulkan\d3d11.dll and vulkan\dxgi.dll next to the executable,
// the same D3D11/DXGI interfaces on Vulkan, so one renderer serves both. The SDK runtime links
// the system dxgi.dll, so DXVK's pair is loaded by full path and only reached through these
// pointers; the device is always made on a DXVK adapter (DXVK's D3D11CreateDevice with no
// adapter would create its factory through whichever dxgi.dll the loader binds by name).
struct GpuApi {
  bool vulkan = false;
  decltype(&CreateDXGIFactory1) factory = &CreateDXGIFactory1;
  decltype(&D3D11CreateDevice) device = &D3D11CreateDevice;
};

bool VulkanInstalled(wchar_t* dir) {
#ifndef _WIN32
  if (dir) *dir = 0;
  return true;  // DXVK native is a required linked dependency.
#else
  GetModuleFileNameW(nullptr, dir, MAX_PATH);
  wchar_t* slash = wcsrchr(dir, L'\\');
  wcscpy(slash ? slash + 1 : dir, L"vulkan\\");
  wchar_t probe[MAX_PATH];
  swprintf(probe, MAX_PATH, L"%lsd3d11.dll", dir);
  return GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES;
#endif
}

const GpuApi& Api() {
  static const GpuApi api = [] {
    GpuApi a;
#ifndef _WIN32
    // All handles passed to DXVK here are SDL3 windows.
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);
    a.vulkan = true;
    return a;
#else
    if (g_boot.api != 1) return a;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!VulkanInstalled(dir)) {
      REXLOG_WARN("[pc] Vulkan chosen but the vulkan folder is missing: using Direct3D 11");
      return a;
    }
    swprintf(path, MAX_PATH, L"%lsdxgi.dll", dir);
    HMODULE dxgi = LoadLibraryW(path);
    swprintf(path, MAX_PATH, L"%lsd3d11.dll", dir);
    HMODULE d3d = dxgi ? LoadLibraryW(path) : nullptr;
    auto factory = dxgi ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    auto device = d3d ? reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d, "D3D11CreateDevice")) : nullptr;
    if (!factory || !device) {
      REXLOG_WARN("[pc] DXVK did not load ({}): using Direct3D 11", uint32_t(GetLastError()));
      return a;
    }
    a.vulkan = true;
    a.factory = factory;
    a.device = device;
    REXLOG_INFO("[pc] graphics API: Vulkan (DXVK)");
    return a;
#endif
  }();
  return api;
}

void GpusRefresh() {
  g_gpus_listed = true;
  g_gpu_name.clear();
  g_gpu_index.clear();
  IDXGIFactory1* f = nullptr;
  if (FAILED(Api().factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return;
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d;
    if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      g_gpu_name.emplace_back(d.Description);
      g_gpu_index.push_back(int(i));
    }
    a->Release();
  }
  f->Release();
}

// Physical pixels of the internal resolution preset.
void InternalSize(int scale, uint32_t& w, uint32_t& h) {
  int lines = k_res[Clampi(scale, 0, kNumRes - 1)].h;
  if (!lines) lines = int(MonitorCurrent().rc.bottom - MonitorCurrent().rc.top);
  lines = Clampi(lines, 360, 4320);
  h = uint32_t(lines);
  w = (uint32_t(float(lines) * float(g_info.native_width) / float(g_info.native_height) + 0.5f) + 1) & ~1u;
}

// The window in windowed mode: the internal size, but never below the original.
void WindowSize(int scale, uint32_t& w, uint32_t& h) {
  InternalSize(scale, w, h);
  if (h < uint32_t(g_info.native_height)) {
    w = uint32_t(g_info.native_width);
    h = uint32_t(g_info.native_height);
  }
}

// ----------------------------------------------------------------------- menu state

enum Item {
  IT_DISPLAY, IT_MONITOR, IT_REFRESH, IT_VSYNC, IT_FPS, IT_SCALE, IT_UPSCALE, IT_ASPECT, IT_AA, IT_MSAA, IT_ANISO,
  IT_EFFECT0, IT_EFFECT_END = IT_EFFECT0 + kMaxEffects,
  IT_API = IT_EFFECT_END, IT_GPU, IT_SHOW_FPS, IT_UI_LANG, IT_LANG, IT_VOLUME, IT_RESUME, IT_RESTART, IT_QUIT,
  IT_COUNT
};

struct Event {
  enum Kind { kKey, kMove, kClick, kWheel } kind;
  int value;  // key: VK; click: 1 left / 2 right; wheel: +1 up / -1 down
};

std::mutex g_mu;  // g_events, g_opt changes from the UI thread
std::deque<Event> g_events;
std::atomic<bool> g_request{false}, g_open{false}, g_toggle_fullscreen{false};
std::atomic<bool> g_display_dirty{true};
int g_sel = IT_RESUME;
bool g_dirty = true;
int g_exclusive_failed = 0;  // 1 refused, 2 waiting for the focus
rex::ui::Window* g_window = nullptr;
HWND g_hwnd = nullptr;

struct Rect { int x0, y0, x1, y1; bool in(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; } };
Rect g_rows[IT_COUNT];
Rect g_value_left[IT_COUNT];  // the "◀" end of a selected value
Rect g_hint_a, g_hint_b;

// The "PC settings" button over the game's own menus.
std::atomic<int64_t> g_game_menus_until{0};  // ms (steady clock) until which the button shows
Rect g_hint_rect{};                          // where it is, in back-buffer pixels (mouse clicks)
int g_hint_shown_lang = -1;
bool g_hint_enabled = true;

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool HintVisible() { return g_hint_enabled && !g_open && SteadyMs() < g_game_menus_until.load(); }


bool RestartPending() {
  return g_opt.adapter != g_boot.adapter || g_opt.language != g_boot.language ||
         g_opt.api != g_boot.api;
}

bool ItemRestart(int it) {
  switch (it) {
    case IT_GPU: return g_opt.adapter != g_boot.adapter;
    case IT_API: return g_opt.api != g_boot.api;
    case IT_LANG: return g_opt.language != g_boot.language;
  }
  return false;
}

bool ItemVisible(int it) {
  if (it >= IT_EFFECT0 && it < IT_EFFECT_END) return it - IT_EFFECT0 < g_info.effect_count;
  switch (it) {
    case IT_SCALE: case IT_UPSCALE: return g_info.internal_resolution;
    case IT_MSAA: return g_info.msaa;
    case IT_LANG: return g_info.language_count > 1;
    case IT_RESTART: return RestartPending();
  }
  return true;
}

int Cycle(const int* list, int n, int cur, int d) {
  int i = 0;
  while (i < n && list[i] != cur) ++i;
  if (i == n) return list[0];
  return list[(i + n + d) % n];
}

void Change(int it, int d) {
  if (it >= IT_EFFECT0 && it < IT_EFFECT_END) {
    if (!ItemVisible(it)) return;
    EffectEnabled(it - IT_EFFECT0);  // refresh a DLL copy before changing its value
    g_effect_mask.fetch_xor(1u << (it - IT_EFFECT0));
    PublishEffects();
    g_dirty = true;
    return;
  }
  std::vector<int> list;
  switch (it) {
    case IT_DISPLAY:
      g_opt.display = (g_opt.display + 3 + d) % 3;
      g_display_dirty = true;
      break;
    case IT_MONITOR:
      MonitorsRefresh();
      g_opt.monitor = (g_opt.monitor + int(g_mon.size()) + d) % int(g_mon.size());
      RatesRefresh();
      g_display_dirty = true;
      break;
    case IT_REFRESH:
      list.push_back(0);
      list.insert(list.end(), g_rates.begin(), g_rates.end());
      g_opt.refresh = Cycle(list.data(), int(list.size()), g_opt.refresh, d);
      if (g_opt.display == 2) g_display_dirty = true;
      break;
    case IT_VSYNC: g_opt.vsync = !g_opt.vsync; break;
    case IT_FPS:
      g_opt.fps = (g_info.real_time || g_info.unlocked_fps) ? Cycle(k_fps, 4, g_opt.fps, d) : Cycle(k_fps_fixed, 2, g_opt.fps, d);
      g_display_hz_at = 0;
      break;
    case IT_SCALE: g_opt.scale = Cycle(k_res_order, kNumRes, g_opt.scale, d); break;
    case IT_UPSCALE: g_opt.upscale = !g_opt.upscale; break;
    case IT_ASPECT: g_opt.aspect = !g_opt.aspect; break;
    case IT_AA: g_opt.aa = !g_opt.aa; break;
    case IT_MSAA: g_opt.msaa = Cycle(k_msaa, 4, g_opt.msaa, d); break;
    case IT_ANISO: g_opt.aniso = Cycle(k_aniso, 5, g_opt.aniso, d); break;
    case IT_GPU:
      if (!g_gpus_listed) GpusRefresh();
      list.push_back(-1);
      list.insert(list.end(), g_gpu_index.begin(), g_gpu_index.end());
      g_opt.adapter = Cycle(list.data(), int(list.size()), g_opt.adapter, d);
      break;
    case IT_API: {
      wchar_t dir[MAX_PATH];
#ifdef _WIN32
      if (g_opt.api || VulkanInstalled(dir)) g_opt.api = !g_opt.api;
#else
      g_opt.api = 1;
#endif
      break;
    }
    case IT_SHOW_FPS: g_opt.show_fps = !g_opt.show_fps; break;
    case IT_UI_LANG: g_opt.ui_language = (g_opt.ui_language + 1 + kUiLangCount + 1 + d) % (kUiLangCount + 1) - 1; break;
    case IT_LANG:
      list.push_back(0);
      for (int i = 0; i < g_info.language_count; ++i) list.push_back(g_info.languages[i].id);
      g_opt.language = Cycle(list.data(), int(list.size()), g_opt.language, d);
      break;
    case IT_VOLUME: g_opt.volume = Clampi(g_opt.volume + 10 * d, 0, 100); break;
  }
  g_dirty = true;
}

IDXGISwapChain1* g_swap = nullptr;  // the swap chain of the frame being shown

void LeaveExclusive() {
  BOOL fs = FALSE;
  if (g_swap && SUCCEEDED(g_swap->GetFullscreenState(&fs, nullptr)) && fs)
    g_swap->SetFullscreenState(FALSE, nullptr);
}

void RestartGame() {
  SaveIni();
  LeaveExclusive();
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  REXLOG_INFO("[pc] restarting to apply resolution {}, gpu {}, language {}, api {}", g_opt.scale, g_opt.adapter,
              g_opt.language, g_opt.api);
  if (CreateProcessW(exe, GetCommandLineW(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
  }
  REXLOG_ERROR("[pc] restart failed ({})", uint32_t(GetLastError()));
}

void Step(int d) {
  do g_sel = (g_sel + IT_COUNT + d) % IT_COUNT;
  while (!ItemVisible(g_sel));
  g_dirty = true;
}

void Activate(int it) {
  if (it == IT_RESUME) {
    g_open = false;
  } else if (it == IT_RESTART) {
    RestartGame();
  } else if (it == IT_QUIT) {
    SaveIni();
    LeaveExclusive();
    REXLOG_INFO("[pc] quit from the settings menu");
    ExitProcess(0);
  } else {
    Change(it, 1);
  }
}

void MenuKey(int vk) {
  switch (vk) {
    case VK_UP: Step(-1); break;
    case VK_DOWN: Step(1); break;
    case VK_LEFT: Change(g_sel, -1); break;
    case VK_RIGHT: Change(g_sel, 1); break;
    case VK_RETURN: case VK_SPACE: Activate(g_sel); break;
    case VK_ESCAPE: case VK_F1: case VK_BACK: g_open = false; break;
  }
}

POINT CursorClient() {
  POINT p{-1, -1};
  if (g_hwnd && GetCursorPos(&p)) ScreenToClient(g_hwnd, &p);
  return p;
}

int RowAt(int x, int y) {
  for (int it = 0; it < IT_COUNT; ++it)
    if (ItemVisible(it) && g_rows[it].in(x, y)) return it;
  return -1;
}

void MenuMouse(const Event& e) {
  POINT p = CursorClient();
  int row = RowAt(p.x, p.y);
  switch (e.kind) {
    case Event::kMove:
      if (row >= 0 && row != g_sel) { g_sel = row; g_dirty = true; }
      break;
    case Event::kClick:
      if (g_hint_b.in(p.x, p.y)) { g_open = false; break; }
      if (g_hint_a.in(p.x, p.y)) { Activate(g_sel); break; }
      if (row < 0) {
        if (e.value == 2) g_open = false;  // right click outside the list: back
        break;
      }
      g_sel = row;
      g_dirty = true;
      if (e.value == 2 || g_value_left[row].in(p.x, p.y))
        Change(row, -1);
      else
        Activate(row);
      break;
    case Event::kWheel:
      Step(e.value > 0 ? -1 : 1);
      break;
    default: break;
  }
}

// -------------------------------------------------------------------------- input

// XInput, loaded at run time (its header's button macros clash with the SDK's input enums).
enum : WORD {
  kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010,
  kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000,
};
struct PadState {
  DWORD packet;
  WORD buttons;
  BYTE lt, rt;
  SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, PadState*);

DWORD PadGetState(DWORD i, PadState* st) {
  static XInputGetStateFn fn = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  return fn ? fn(i, st) : ERROR_DEVICE_NOT_CONNECTED;
}

// Every connected pad's buttons OR'd, the left stick as a d-pad. A slot without a pad is
// probed only now and then: XInputGetState on an empty slot is slow.
bool g_pad_seen[4];
unsigned g_pad_probe;

WORD PadsRead(bool probe_all) {
  WORD b = 0;
  ++g_pad_probe;
  for (DWORD i = 0; i < 4; ++i) {
    if (!g_pad_seen[i] && !probe_all && (g_pad_probe % 240u) != i * 60u) continue;
    PadState st{};
    if (PadGetState(i, &st) != ERROR_SUCCESS) {
      g_pad_seen[i] = false;
      continue;
    }
    g_pad_seen[i] = true;
    b |= st.buttons;
    if (st.ly > 16000) b |= kPadUp;
    if (st.ly < -16000) b |= kPadDown;
    if (st.lx > 16000) b |= kPadRight;
    if (st.lx < -16000) b |= kPadLeft;
  }
  return b;
}

// Window input on the UI thread. Above the SDK's keyboard/mouse controller (its z-order is
// lower), so while the menu is open nothing reaches the game.
class InputListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    int vk = int(e.virtual_key());
    if ((vk == VK_RETURN && e.is_alt_pressed()) || vk == VK_F11) {
      if (!e.prev_state()) g_toggle_fullscreen = true;
      e.set_handled(true);
      return;
    }
    if (!g_open && !g_request) {
      if (vk == VK_F1) {
        if (!e.prev_state()) g_request = true;
        e.set_handled(true);
      }
      return;
    }
    e.set_handled(true);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_events.size() < 64) g_events.push_back({Event::kKey, vk});
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    int vk = int(e.virtual_key());
    if (g_open || vk == VK_F1 || vk == VK_F11) e.set_handled(true);
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (g_open) e.set_handled(true);
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Push(e, Event::kClick); }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (g_open) e.set_handled(true);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override { Push(e, Event::kMove); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override { Push(e, Event::kWheel); }

 private:
  void Push(rex::ui::MouseEvent& e, Event::Kind kind) {
    if (!g_open) {
      if (kind == Event::kClick && e.button() == rex::ui::MouseEvent::Button::kLeft && HintVisible() &&
          g_hint_rect.in(e.x(), e.y())) {
        e.set_handled(true);
        g_request = true;
      }
      return;
    }
    e.set_handled(true);
    int value = 0;
    if (kind == Event::kClick) {
      if (e.button() == rex::ui::MouseEvent::Button::kLeft) value = 1;
      else if (e.button() == rex::ui::MouseEvent::Button::kRight) value = 2;
      else return;
    } else if (kind == Event::kWheel) {
      if (!e.scroll_y()) return;
      value = e.scroll_y() > 0 ? 1 : -1;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (kind == Event::kMove && !g_events.empty() && g_events.back().kind == Event::kMove) return;
    if (g_events.size() < 64) g_events.push_back({kind, value});
  }
};
InputListener g_listener;

void CallInUi(std::function<void()> f) {
  if (g_window) g_window->app_context().CallInUIThread(std::move(f));
}

// ------------------------------------------------------------------ display mode

// UI thread: windowed size and position, or the SDK's borderless fullscreen on the monitor.
void ApplyWindowMode(int display, int monitor_index) {
  if (!g_window || !g_hwnd) return;
  MonitorsRefresh();
  const Monitor& m = g_mon[size_t(monitor_index) < g_mon.size() ? size_t(monitor_index) : 0];
#ifdef _WIN32
  HMONITOR cur = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(cur, &mi);
  bool same_monitor = wcscmp(mi.szDevice, m.device) == 0;
  UINT dpi = GetDpiForWindow(g_hwnd);
  if (!dpi) dpi = 96;
  if (display == 0) {
    if (g_window->IsFullscreen()) g_window->SetFullscreen(false);
    uint32_t w, h;
    WindowSize(g_opt.scale, w, h);
    int aw = int(m.work.right - m.work.left) * 9 / 10, ah = int(m.work.bottom - m.work.top) * 9 / 10;
    int ww = int(w), wh = int(h);
    if (ww > aw) { wh = wh * aw / ww; ww = aw; }
    if (wh > ah) { ww = ww * ah / wh; wh = ah; }
    g_window->SetDesiredLogicalSize(uint32_t(ww) * 96 / dpi, uint32_t(wh) * 96 / dpi);
    RECT r;
    GetWindowRect(g_hwnd, &r);
    int x = m.work.left + ((m.work.right - m.work.left) - (r.right - r.left)) / 2;
    int y = std::max<int>(m.work.top, m.work.top + ((m.work.bottom - m.work.top) - (r.bottom - r.top)) / 2);
    SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  } else {
    if (!same_monitor) {
      if (g_window->IsFullscreen()) g_window->SetFullscreen(false);
      RECT r;
      GetWindowRect(g_hwnd, &r);
      SetWindowPos(g_hwnd, nullptr, m.work.left + ((m.work.right - m.work.left) - (r.right - r.left)) / 2,
                   m.work.top + ((m.work.bottom - m.work.top) - (r.bottom - r.top)) / 2, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!g_window->IsFullscreen()) g_window->SetFullscreen(true);
  }
#else
  auto* window = reinterpret_cast<SDL_Window*>(g_hwnd);
  if (g_window->IsFullscreen()) g_window->SetFullscreen(false);
  g_window->SetMonitor(m.sdk_index);
  if (display == 0) {
    uint32_t width = 0, height = 0;
    WindowSize(g_opt.scale, width, height);
    const float density = std::max(SDL_GetWindowPixelDensity(window), 1.0f);
    float scale = std::min({1.0f, float(m.work.right - m.work.left) * 0.9f / width,
                          float(m.work.bottom - m.work.top) * 0.9f / height});
    const int w = std::max(1, int(width * scale / density));
    const int h = std::max(1, int(height * scale / density));
    g_window->SetDesiredLogicalSize(w, h);
    SDL_SetWindowPosition(window, m.work.left + (m.work.right - m.work.left - w) / 2,
                          m.work.top + (m.work.bottom - m.work.top - h) / 2);
  } else g_window->SetFullscreen(true);
#endif
  g_window->SetCursorVisibility(display != 0 && !g_open ? rex::ui::Window::CursorVisibility::kAutoHidden
                                                        : rex::ui::Window::CursorVisibility::kVisible);
}

// Render thread, with the swap chain: exclusive fullscreen in or out.
bool g_want_exclusive = false;
unsigned g_fs_check = 0;

void ApplyExclusive(IDXGISwapChain1* swap) {
  BOOL fs = FALSE;
  if (FAILED(swap->GetFullscreenState(&fs, nullptr))) return;
  if (!g_want_exclusive) {
    if (fs) swap->SetFullscreenState(FALSE, nullptr);
    return;
  }
  if (fs) return;
  if (GetForegroundWindow() != g_hwnd) {
    if (g_exclusive_failed != 2) { g_exclusive_failed = 2; g_dirty = true; }
    return;
  }
  const Monitor& m = MonitorCurrent();
  DXGI_MODE_DESC md{};
  md.Width = UINT(m.rc.right - m.rc.left);
  md.Height = UINT(m.rc.bottom - m.rc.top);
  md.RefreshRate.Numerator = UINT(g_opt.refresh ? g_opt.refresh : MonitorCurrentHz());
  md.RefreshRate.Denominator = 1;
  md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap->ResizeTarget(&md);
  HRESULT hr = swap->SetFullscreenState(TRUE, nullptr);
  if (FAILED(hr)) {
    g_exclusive_failed = hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? 2 : 1;
    g_dirty = true;
    REXLOG_WARN("[pc] exclusive fullscreen failed {:08X}: staying borderless", uint32_t(hr));
    if (g_exclusive_failed == 1) g_want_exclusive = false;
    return;
  }
  md.RefreshRate.Numerator = 0;  // keep the mode the output picked; just match the size
  md.RefreshRate.Denominator = 0;
  swap->ResizeTarget(&md);
  g_exclusive_failed = 0;
  g_dirty = true;
  REXLOG_INFO("[pc] exclusive fullscreen {}x{} @ {} Hz", md.Width, md.Height,
              g_opt.refresh ? g_opt.refresh : MonitorCurrentHz());
}

void ApplyDisplay(IDXGISwapChain1* swap) {
  if (g_toggle_fullscreen.exchange(false)) {
    g_opt.display = g_opt.display ? 0 : 1;
    g_display_dirty = true;
    g_dirty = true;
    SaveIni();
  }
  // Back from Alt+Tab in exclusive mode: enter it again once the window is in front.
  if (g_opt.display == 2 && g_want_exclusive && ++g_fs_check % 30 == 0) {
    BOOL fs = FALSE;
    if (SUCCEEDED(swap->GetFullscreenState(&fs, nullptr)) && !fs && GetForegroundWindow() == g_hwnd)
      ApplyExclusive(swap);
  }
  if (!g_display_dirty.exchange(false)) return;
  int display = g_opt.display, monitor = g_opt.monitor;
  g_want_exclusive = display == 2;
  if (!g_want_exclusive) ApplyExclusive(swap);  // leave it before the window changes
  CallInUi([display, monitor] { ApplyWindowMode(display, monitor); });
  if (g_want_exclusive) {
    // After the SDK made the window borderless on the right monitor.
    for (int i = 0; i < 50; ++i) {
      if (g_window && g_window->IsFullscreen()) break;
      Sleep(10);
    }
    ApplyExclusive(swap);
  }
}

// ---------------------------------------------------------------------- drawing

template <typename T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps_pic = nullptr;
  ID3D11PixelShader* ps_fxaa = nullptr;
  ID3D11PixelShader* ps_overlay = nullptr;
  ID3D11PixelShader* ps_easu = nullptr;
  ID3D11PixelShader* ps_rcas = nullptr;
  ID3D11BlendState* blend_alpha = nullptr;
  ID3D11SamplerState* linear = nullptr;
  ID3D11SamplerState* point = nullptr;
  ID3D11RasterizerState* raster = nullptr;
  ID3D11DepthStencilState* no_depth = nullptr;
  ID3D11Buffer* cb = nullptr;
  bool failed = false;
} g_gpu;

const char k_shaders[] =
    "Texture2D<float4> pic:register(t0); SamplerState lin:register(s0);"
    "cbuffer P:register(b0){float4 uvmax; float4 opts;};"  // opts.x: swap red/blue
    "struct V{float4 p:SV_Position; float2 uv:TEXCOORD0;};"
    "V vs(uint id:SV_VertexID){V o; float2 q=float2(id==1?2:0,id==2?2:0);"
    "o.p=float4(q.x*2-1,1-q.y*2,0,1); o.uv=q; return o;}"
    "float3 S(float2 uv){float3 c=pic.SampleLevel(lin,min(uv,uvmax.zw),0).rgb; return opts.x>0.5?c.bgr:c;}"
    "float4 ps(V i):SV_Target{return float4(S(i.uv*uvmax.xy),1);}"
    // FXAA (Lottes' console variant): blend along the edge direction, only where the result
    // stays within the local range.
    "float4 ps_fxaa(V i):SV_Target{"
    "float w,h; pic.GetDimensions(w,h); float2 rc=1/float2(w,h); float2 uv=i.uv*uvmax.xy;"
    "float3 nw=S(uv+float2(-1,-1)*rc),ne=S(uv+float2(1,-1)*rc),"
    "sw=S(uv+float2(-1,1)*rc),se=S(uv+float2(1,1)*rc),m=S(uv);"
    "const float3 L=float3(0.299,0.587,0.114);"
    "float lnw=dot(nw,L),lne=dot(ne,L),lsw=dot(sw,L),lse=dot(se,L),lm=dot(m,L);"
    "float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));"
    "float2 d=float2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));"
    "float red=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0);"
    "d=clamp(d/(min(abs(d.x),abs(d.y))+red),-8.0,8.0)*rc;"
    "float3 a=0.5*(S(uv+d*(1.0/3.0-0.5))+S(uv+d*(2.0/3.0-0.5)));"
    "float3 b=a*0.5+0.25*(S(uv-d*0.5)+S(uv+d*0.5));"
    "float lb=dot(b,L);"
    "return float4((lb<lmin||lb>lmax)?a:b,1);}"
    "float4 ps_overlay(V i):SV_Target{return pic.SampleLevel(lin,i.uv,0);}"
    // AMD FidelityFX Super Resolution 1.0 (MIT; Copyright (c) 2021 Advanced Micro Devices),
    // the 32-bit path of ffx_fsr1.h. EASU: uvmax.xy source size, uvmax.zw output size.
    "float Lu(float3 c){return c.b*0.5+(c.r*0.5+c.g);}"
    "float3 Ld(int2 p,float2 n){return pic.Load(int3(clamp(p,int2(0,0),int2(n)-1),0)).rgb;}"
    "void Es(inout float2 dir,inout float len,float w,float a,float b,float c,float d,float e){"
    "float lx=max(abs(d-c),abs(c-b)); lx=lx>0?1/lx:0; float dx=d-b; dir.x+=dx*w;"
    "lx=saturate(abs(dx)*lx); len+=lx*lx*w;"
    "float ly=max(abs(e-c),abs(c-a)); ly=ly>0?1/ly:0; float dy=e-a; dir.y+=dy*w;"
    "ly=saturate(abs(dy)*ly); len+=ly*ly*w;}"
    "void Et(inout float3 ac,inout float aw,float2 o,float2 dir,float2 len2,float lob,float clp,float3 c){"
    "float2 v=float2(o.x*dir.x+o.y*dir.y,o.x*(-dir.y)+o.y*dir.x)*len2; float d2=min(dot(v,v),clp);"
    "float wb=0.4*d2-1,wa=lob*d2-1; wb*=wb; wa*=wa; wb=1.5625*wb-0.5625; float w=wb*wa; ac+=c*w; aw+=w;}"
    "float4 ps_easu(V i):SV_Target{float2 n=uvmax.xy;"
    "float2 pp=(floor(i.p.xy)+0.5)*n/uvmax.zw-0.5; float2 fp=floor(pp); pp-=fp; int2 q=int2(fp);"
    "float3 b=Ld(q+int2(0,-1),n),c=Ld(q+int2(1,-1),n),e=Ld(q+int2(-1,0),n),f=Ld(q,n),"
    "g=Ld(q+int2(1,0),n),h=Ld(q+int2(2,0),n),k0=Ld(q+int2(-1,1),n),j=Ld(q+int2(0,1),n),"
    "k=Ld(q+int2(1,1),n),l=Ld(q+int2(2,1),n),m=Ld(q+int2(0,2),n),o=Ld(q+int2(1,2),n);"
    "float bL=Lu(b),cL=Lu(c),eL=Lu(e),fL=Lu(f),gL=Lu(g),hL=Lu(h),iL=Lu(k0),jL=Lu(j),kL=Lu(k),lL=Lu(l),"
    "nL=Lu(m),oL=Lu(o); float2 dir=0; float len=0;"
    "Es(dir,len,(1-pp.x)*(1-pp.y),bL,eL,fL,gL,jL); Es(dir,len,pp.x*(1-pp.y),cL,fL,gL,hL,kL);"
    "Es(dir,len,(1-pp.x)*pp.y,fL,iL,jL,kL,nL); Es(dir,len,pp.x*pp.y,gL,jL,kL,lL,oL);"
    "float dr=dot(dir,dir); bool z=dr<1.0/32768.0; dr=z?1:rsqrt(dr); dir.x=z?1:dir.x; dir*=dr;"
    "len=len*0.5; len*=len; float st=dot(dir,dir)/max(abs(dir.x),abs(dir.y));"
    "float2 len2=float2(1+(st-1)*len,1-0.5*len); float lob=0.5+((1.0/4.0-0.04)-0.5)*len; float clp=1/lob;"
    "float3 ac=0; float aw=0;"
    "Et(ac,aw,float2(0,-1)-pp,dir,len2,lob,clp,b); Et(ac,aw,float2(1,-1)-pp,dir,len2,lob,clp,c);"
    "Et(ac,aw,float2(-1,1)-pp,dir,len2,lob,clp,k0); Et(ac,aw,float2(0,1)-pp,dir,len2,lob,clp,j);"
    "Et(ac,aw,float2(0,0)-pp,dir,len2,lob,clp,f); Et(ac,aw,float2(-1,0)-pp,dir,len2,lob,clp,e);"
    "Et(ac,aw,float2(1,1)-pp,dir,len2,lob,clp,k); Et(ac,aw,float2(2,1)-pp,dir,len2,lob,clp,l);"
    "Et(ac,aw,float2(2,0)-pp,dir,len2,lob,clp,h); Et(ac,aw,float2(1,0)-pp,dir,len2,lob,clp,g);"
    "Et(ac,aw,float2(1,2)-pp,dir,len2,lob,clp,o); Et(ac,aw,float2(0,2)-pp,dir,len2,lob,clp,m);"
    "float3 mn=min(min(f,g),min(j,k)),mx=max(max(f,g),max(j,k));"
    "return float4(min(mx,max(mn,ac/aw)),1);}"
    // RCAS: uvmax.zw the upscaled size, opts.yz the viewport origin, opts.w the sharpness.
    "float4 ps_rcas(V i):SV_Target{float2 n=uvmax.zw; int2 p=int2(i.p.xy-opts.yz);"
    "float3 b=Ld(p+int2(0,-1),n),d=Ld(p+int2(-1,0),n),e=Ld(p,n),f=Ld(p+int2(1,0),n),h=Ld(p+int2(0,1),n);"
    "float bL=Lu(b),dL=Lu(d),eL=Lu(e),fL=Lu(f),hL=Lu(h);"
    "float nz=0.25*(bL+dL+fL+hL)-eL;"
    "float rg=max(max(max(bL,dL),max(eL,fL)),hL)-min(min(min(bL,dL),min(eL,fL)),hL);"
    "nz=saturate(abs(nz)/max(rg,1.0/65536.0)); nz=-0.5*nz+1;"
    "float3 mn4=min(min(b,d),min(f,h)),mx4=max(max(b,d),max(f,h));"
    "float3 hmin=mn4/max(4*mx4,1.0/65536.0),hmax=(1-mx4)/min(4*mn4-4,-1.0/65536.0);"
    "float3 lr=max(-hmin,hmax); float lobe=max(-(0.25-1.0/16.0),min(max(lr.r,max(lr.g,lr.b)),0))*opts.w*nz;"
    "return float4((lobe*(b+d+f+h)+e)/(4*lobe+1),1);}";

using CompileFn = decltype(&D3DCompile);

bool GpuSetup(ID3D11Device* dev) {
  if (g_gpu.dev == dev) return !g_gpu.failed;
  g_gpu = Gpu{};
  g_gpu.dev = dev;
  g_gpu.failed = true;
  static CompileFn compile = [] {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
#else
    return &D3DCompile;
#endif
  }();
  if (!compile) return false;
  auto blob = [&](const char* entry, const char* target) -> ID3DBlob* {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(k_shaders, sizeof(k_shaders) - 1, "pc_settings", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
      REXLOG_ERROR("[pc] shader {}: {}", entry, static_cast<const char*>(err->GetBufferPointer()));
      err->Release();
    }
    return SUCCEEDED(hr) ? code : nullptr;
  };
  ID3DBlob* b = blob("vs", "vs_5_0");
  if (!b || FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_gpu.vs))) return false;
  b->Release();
  struct { const char* entry; ID3D11PixelShader** out; } ps[] = {
      {"ps", &g_gpu.ps_pic}, {"ps_fxaa", &g_gpu.ps_fxaa}, {"ps_overlay", &g_gpu.ps_overlay},
      {"ps_easu", &g_gpu.ps_easu}, {"ps_rcas", &g_gpu.ps_rcas}};
  for (auto& p : ps) {
    b = blob(p.entry, "ps_5_0");
    if (!b || FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.out))) return false;
    b->Release();
  }
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(dev->CreateBlendState(&bd, &g_gpu.blend_alpha))) return false;
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(dev->CreateSamplerState(&sd, &g_gpu.linear))) return false;
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (FAILED(dev->CreateSamplerState(&sd, &g_gpu.point))) return false;
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(dev->CreateRasterizerState(&rd, &g_gpu.raster))) return false;
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(dev->CreateDepthStencilState(&dd, &g_gpu.no_depth))) return false;
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = 32;
  cbd.Usage = D3D11_USAGE_DEFAULT;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(dev->CreateBuffer(&cbd, nullptr, &g_gpu.cb))) return false;
  g_gpu.failed = false;
  return true;
}

// A GDI surface that becomes a texture (the menu screen, the FPS counter).
struct Surface {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  uint32_t* bits = nullptr;
  int w = 0, h = 0;
  ID3D11Device* dev = nullptr;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;

  bool Resize(ID3D11Device* d, int nw, int nh) {
    if (nw == w && nh == h && d == dev && srv) return true;
    Release(srv);
    Release(tex);
    if (bmp) { DeleteObject(bmp); bmp = nullptr; bits = nullptr; }
    if (!dc) dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!bmp) return false;
    SelectObject(dc, bmp);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(nw);
    td.Height = UINT(nh);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &tex)) || FAILED(d->CreateShaderResourceView(tex, nullptr, &srv)))
      return false;
    w = nw;
    h = nh;
    dev = d;
    return true;
  }
  void Upload(ID3D11DeviceContext* ctx) { ctx->UpdateSubresource(tex, 0, nullptr, bits, UINT(w) * 4, 0); }
};

Surface g_menu, g_fps_surface, g_hint_surface;
HFONT g_font, g_font_bold, g_font_small, g_font_title;
int g_font_lh = 0;

HFONT MakeFont(int px, int weight, bool italic) {
  return CreateFontW(-px, 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void Fonts(int lh) {
  if (lh == g_font_lh) return;
  for (HFONT f : {g_font, g_font_bold, g_font_small, g_font_title})
    if (f) DeleteObject(f);
  g_font = MakeFont(lh * 60 / 100, FW_NORMAL, false);
  g_font_bold = MakeFont(lh * 62 / 100, FW_BOLD, false);
  g_font_small = MakeFont(lh * 48 / 100, FW_NORMAL, false);
  g_font_title = MakeFont(lh * 175 / 100, FW_HEAVY, true);
  g_font_lh = lh;
}

HDC g_dc;  // the surface being painted

void Text(const wchar_t* s, int x0, int y0, int x1, int y1, COLORREF c, HFONT f, UINT align) {
  RECT r;
  SetRect(&r, x0, y0, x1, y1);
  SelectObject(g_dc, f);
  SetTextColor(g_dc, c);
  DrawTextW(g_dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void Fill(int x0, int y0, int x1, int y1, COLORREF c) {
  RECT r;
  HBRUSH b = CreateSolidBrush(c);
  SetRect(&r, x0, y0, x1, y1);
  FillRect(g_dc, &r, b);
  DeleteObject(b);
}

// A slanted band, the shape EA's front ends use for headers.
void Band(int x0, int y0, int x1, int y1, int slant, COLORREF c) {
  POINT p[4] = {{x0, y0}, {x1 + slant, y0}, {x1, y1}, {x0, y1}};
  HBRUSH b = CreateSolidBrush(c);
  HPEN pen = CreatePen(PS_SOLID, 1, c);
  SelectObject(g_dc, b);
  SelectObject(g_dc, pen);
  Polygon(g_dc, p, 4);
  DeleteObject(b);
  DeleteObject(pen);
}

// A controller button glyph and its caption, e.g. (A) SELECIONAR. Returns the x after it.
int ButtonHint(int x, int y, int lh, wchar_t letter, COLORREF c, const wchar_t* caption, Rect* hit) {
  wchar_t l[2] = {letter, 0};
  int d = lh * 7 / 10;
  HBRUSH b = CreateSolidBrush(c);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
  SelectObject(g_dc, b);
  SelectObject(g_dc, pen);
  Ellipse(g_dc, x, y + (lh - d) / 2, x + d, y + (lh - d) / 2 + d);
  DeleteObject(b);
  DeleteObject(pen);
  Text(l, x, y, x + d, y + lh, RGB(255, 255, 255), g_font_small, DT_CENTER);
  SelectObject(g_dc, g_font_small);
  SIZE sz;
  GetTextExtentPoint32W(g_dc, caption, int(wcslen(caption)), &sz);
  Text(caption, x + d + lh / 5, y, x + d + lh / 5 + sz.cx + 4, y + lh, RGB(225, 228, 235), g_font_small, DT_LEFT);
  if (hit) *hit = {x, y, x + d + lh / 5 + sz.cx + 4, y + lh};
  return x + d + lh / 5 + sz.cx + lh;
}

void ItemText(int it, wchar_t* label, wchar_t* value, bool* greyed) {
  *greyed = false;
  value[0] = 0;
  if (it >= IT_EFFECT0 && it < IT_EFFECT_END && ItemVisible(it)) {
    const Effect& effect = g_info.effects[it - IT_EFFECT0];
    swprintf(label, 128, L"%ls", T(effect.name_pt, effect.name_en));
    wcscpy(value, EffectEnabled(it - IT_EFFECT0) ? T(L"Ligado", L"On") : T(L"Desligado", L"Off"));
    return;
  }
  switch (it) {
    case IT_DISPLAY:
      wcscpy(label, T(L"Modo de tela", L"Display mode"));
      wcscpy(value, g_opt.display == 0   ? T(L"Janela", L"Windowed")
                    : g_opt.display == 1 ? T(L"Tela cheia (sem bordas)", L"Borderless fullscreen")
                    : g_exclusive_failed == 2
                        ? T(L"Tela cheia exclusiva (ao focar a janela)", L"Exclusive fullscreen (when focused)")
                    : g_exclusive_failed ? T(L"Exclusiva recusada pelo Windows: sem bordas", L"Exclusive unavailable: borderless")
                                         : T(L"Tela cheia exclusiva", L"Exclusive fullscreen"));
      break;
    case IT_MONITOR:
      wcscpy(label, T(L"Monitor", L"Monitor"));
      swprintf(value, 128, L"%d: %ls", (size_t(g_opt.monitor) < g_mon.size() ? g_opt.monitor : 0) + 1,
               MonitorCurrent().name);
      break;
    case IT_REFRESH:
      wcscpy(label, T(L"Taxa de atualização", L"Refresh rate"));
      if (g_opt.refresh)
        swprintf(value, 128, L"%d Hz", g_opt.refresh);
      else
        swprintf(value, 128, T(L"A do monitor (%d Hz)", L"Monitor refresh rate (%d Hz)"), MonitorCurrentHz());
      *greyed = g_opt.display != 2;
      break;
    case IT_VSYNC:
      wcscpy(label, T(L"Sincronia vertical (VSync)", L"Vertical sync (VSync)"));
      wcscpy(value, g_opt.vsync ? T(L"Ligada", L"On") : T(L"Desligada", L"Off"));
      break;
    case IT_FPS:
      wcscpy(label, T(L"Limite de quadros", L"Frame limit"));
      if (g_opt.fps == 60) wcscpy(value, L"60 fps (original)");
      else if (g_opt.fps == kFpsMonitor) swprintf(value, 128, T(L"A do monitor (%d fps)", L"Monitor rate (%d fps)"), DisplayHz());
      else if (g_opt.fps) swprintf(value, 128, L"%d fps", g_opt.fps);
      else wcscpy(value, T(L"Sem limite", L"Unlimited"));
      break;
    case IT_SCALE: {
      wcscpy(label, T(L"Resolução interna", L"Internal resolution"));
      uint32_t w, h;
      InternalSize(g_opt.scale, w, h);
      const ResPreset& p = k_res[Clampi(g_opt.scale, 0, kNumRes - 1)];
      swprintf(value, 128, L"%ls (%ux%u)", T(p.pt, p.en), w, h);
      break;
    }
    case IT_UPSCALE:
      wcscpy(label, T(L"Ampliação da imagem", L"Upscaling"));
      wcscpy(value, g_opt.upscale ? L"AMD FSR 1.0" : L"Bilinear");
      break;
    case IT_ASPECT:
      wcscpy(label, T(L"Proporção da imagem", L"Aspect ratio"));
      wcscpy(value, g_opt.aspect ? T(L"Esticar até a tela", L"Stretch to the screen")
                                 : T(L"16:9 (original, com barras)", L"16:9 (original, with bars)"));
      break;
    case IT_AA:
      wcscpy(label, T(L"Antisserrilhamento", L"Anti-aliasing"));
      wcscpy(value, g_opt.aa ? L"FXAA" : T(L"Desligado", L"Off"));
      break;
    case IT_MSAA:
      wcscpy(label, T(L"Multiamostragem (MSAA)", L"Multisampling (MSAA)"));
      if (g_opt.msaa) swprintf(value, 128, L"%dx", g_opt.msaa);
      else wcscpy(value, T(L"Desligada", L"Off"));
      break;
    case IT_ANISO:
      wcscpy(label, T(L"Filtro anisotrópico", L"Anisotropic filtering"));
      if (g_opt.aniso) swprintf(value, 128, L"%dx", g_opt.aniso);
      else wcscpy(value, T(L"O do jogo", L"Game default"));
      break;
    case IT_API:
      wcscpy(label, T(L"API gráfica", L"Graphics API"));
      wcscpy(value, g_opt.api ? L"Vulkan (DXVK)" : L"DirectX 11");
      {
        wchar_t dir[MAX_PATH];
        *greyed = !g_opt.api && !VulkanInstalled(dir);
      }
      break;
    case IT_GPU:
      wcscpy(label, T(L"Placa de vídeo", L"Graphics card"));
      wcscpy(value, T(L"Automática (padrão do Windows)", L"Automatic (Windows default)"));
      for (size_t i = 0; i < g_gpu_index.size(); ++i)
        if (g_gpu_index[i] == g_opt.adapter) wcsncpy(value, g_gpu_name[i].c_str(), 127);
      break;
    case IT_SHOW_FPS:
      wcscpy(label, T(L"Mostrar FPS", L"Show FPS"));
      wcscpy(value, g_opt.show_fps ? T(L"Sim", L"Yes") : T(L"Não", L"No"));
      break;
    case IT_UI_LANG:
      wcscpy(label, T(L"Idioma do menu", L"Menu language"));
      if (g_opt.ui_language >= 0) wcscpy(value, k_ui_lang_name[g_opt.ui_language]);
      else swprintf(value, 128, T(L"Automático (%ls)", L"Automatic (%ls)"), k_ui_lang_name[UiLangAuto()]);
      break;
    case IT_LANG:
      wcscpy(label, T(L"Idioma do jogo", L"Game language"));
      wcscpy(value, LanguageName(g_opt.language));
      break;
    case IT_VOLUME:
      wcscpy(label, T(L"Volume", L"Volume"));
      swprintf(value, 128, L"%d%%", g_opt.volume);
      break;
    case IT_RESUME: wcscpy(label, T(L"Voltar ao jogo", L"Back to the game")); break;
    case IT_RESTART: wcscpy(label, T(L"Reiniciar o jogo para aplicar *", L"Restart game to apply *")); break;
    case IT_QUIT: wcscpy(label, T(L"Sair do jogo", L"Quit game")); break;
  }
}

const wchar_t* ItemHelp(int it) {
  if (it >= IT_EFFECT0 && it < IT_EFFECT_END && ItemVisible(it)) {
    const Effect& effect = g_info.effects[it - IT_EFFECT0];
    return T(effect.help_pt, effect.help_en);
  }
  switch (it) {
    case IT_DISPLAY: return T(L"Janela, tela cheia sem bordas (troca rápida com Alt+Tab) ou tela cheia exclusiva.", L"Windowed, borderless fullscreen (quick Alt+Tab) or exclusive fullscreen.");
    case IT_MONITOR: return T(L"Em qual monitor o jogo aparece.", L"Monitor used to display the game.");
    case IT_REFRESH: return T(L"Frequência do monitor na tela cheia exclusiva.", L"Monitor refresh rate in exclusive fullscreen.");
    case IT_VSYNC: return T(L"Espera o monitor para mostrar cada quadro: sem cortes na imagem, um pouco mais de atraso.", L"Synchronizes frames with the monitor to prevent tearing, with slightly more latency.");
    case IT_FPS:
      if (g_info.real_time || g_info.unlocked_fps)
        return T(L"60 como no console, a taxa do monitor (mais fluido em telas de 120/144 Hz) ou sem limite. 30 economiza energia.",
                 L"60 as on the console, the monitor's rate (smoother on 120/144 Hz screens) or no limit. 30 saves power.");
      return T(L"Este jogo avança um passo por quadro: acima de 60 ele ficaria acelerado. 30 economiza energia em notebooks.",
               L"This game advances one step per frame: above 60 it would run too fast. 30 saves power on laptops.");
    case IT_SCALE: return T(L"Resolução em que o jogo é desenhado: maior = imagem mais nítida, mais trabalho para a placa.", L"Higher rendering resolutions give a sharper image and increase GPU load.");
    case IT_UPSCALE: return T(L"Como a imagem é ampliada quando a resolução interna é menor que a da tela. AMD FSR 1.0 reconstrói as bordas e aumenta a nitidez.", L"How the picture is enlarged when the internal resolution is below the screen's. AMD FSR 1.0 rebuilds edges and sharpens.");
    case IT_ASPECT: return T(L"O jogo é 16:9. Em telas de outro formato, com barras ou esticado.", L"The game is 16:9. On other screen shapes: with bars or stretched.");
    case IT_AA: return T(L"FXAA suaviza as bordas serrilhadas com custo quase nulo.", L"FXAA smooths jagged edges with minimal performance cost.");
    case IT_MSAA: return T(L"Suaviza as bordas da cena 3D desenhando-a com várias amostras por pixel. Mais nítido que o FXAA, mais trabalho para a placa.", L"Smooths the edges of the 3D scene by drawing it with several samples per pixel. Sharper than FXAA, more work for the graphics card.");
    case IT_ANISO: return T(L"Deixa nítidas as texturas vistas em ângulo, como o chão.", L"Sharpens textures viewed at an angle, such as the ground.");
    case IT_API: {
      wchar_t dir[MAX_PATH];
      if (!VulkanInstalled(dir)) return T(L"Direct3D 11. Para Vulkan, falta a pasta vulkan (DXVK) ao lado do jogo.", L"Direct3D 11. Vulkan needs the vulkan folder (DXVK) next to the game.");
      return T(L"DirectX 11 ou Vulkan (pelo DXVK). Se algo aparecer errado ou travar, experimente a outra. Reinicia o jogo.", L"DirectX 11 or Vulkan (through DXVK). If something looks wrong or stutters, try the other one. Restarts the game.");
    }
    case IT_GPU: return T(L"Placa de vídeo usada para desenhar o jogo.", L"Graphics card used to render the game.");
    case IT_SHOW_FPS: return T(L"Mostra os quadros por segundo no canto da tela.", L"Shows frames per second in the corner of the screen.");
    case IT_UI_LANG: return T(L"Idioma deste menu. Automático segue o idioma do jogo. A mudança é imediata.", L"Language of this settings menu. Automatic follows the game's language. Changes apply immediately.");
    case IT_LANG: return T(L"Idioma dos textos e narração do jogo (o que o disco tiver).", L"Language of the game's text and commentary (as available on the disc).");
    case IT_VOLUME: return T(L"Volume geral do jogo.", L"Overall game volume.");
    case IT_RESUME: return T(L"Salva e volta para onde você estava.", L"Saves settings and returns to the game.");
    case IT_RESTART: return T(L"Fecha e abre o jogo de novo com as opções marcadas com *.", L"Restarts the game to apply settings marked with *.");
    case IT_QUIT: return T(L"Salva as configurações e fecha o jogo.", L"Saves settings and quits the game.");
  }
  return L"";
}

constexpr COLORREF kBg = RGB(10, 16, 30);
constexpr uint32_t kBgPixel = 0x0A101Eu;  // the same colour as a DIB pixel
constexpr uint32_t kBgAlpha = 0xE0000000u;

// The optional theme is copied once; the game loader retains no pointers into archives.
const Theme* MenuTheme() {
  static const Theme* source = nullptr;
  static Theme loaded;
  const char* enabled = std::getenv("PCS_THEME");
  if (!g_info.theme || (enabled && *enabled == '0')) return nullptr;
  if (UiLang() == kUiRu) return nullptr;  // the games' Latin fonts have no Cyrillic: Segoe UI
  if (source != g_info.theme) {
    source = g_info.theme;
    loaded = *source;
    if (loaded.load) loaded.load(loaded);
  }
  const auto& atlas = loaded.font.atlas;
  return loaded.font.glyphs.empty() || atlas.width <= 0 || atlas.height <= 0 ||
         atlas.rgba.size() != size_t(atlas.width) * atlas.height ? nullptr : &loaded;
}

void ThemePixel(int x, int y, uint32_t c) {
  if (x < 0 || y < 0 || x >= g_menu.w || y >= g_menu.h) return;
  uint32_t& dst = g_menu.bits[y * g_menu.w + x];
  unsigned a = c >> 24, da = dst >> 24;
  unsigned out = a + da * (255 - a) / 255;
  if (!out) return;
  uint32_t rgb = 0;
  for (int shift : {0, 8, 16}) {
    unsigned v = (((c >> shift) & 255) * a +
                  ((dst >> shift) & 255) * da * (255 - a) / 255) / out;
    rgb |= v << shift;
  }
  dst = rgb | (out << 24);
}

void ThemeFill(int x, int y, int w, int h, uint32_t c) {
  for (int yy = std::max(0, y); yy < std::min(g_menu.h, y + h); ++yy)
    for (int xx = std::max(0, x); xx < std::min(g_menu.w, x + w); ++xx)
      ThemePixel(xx, yy, c);
}

void ThemeBlit(const ThemeImage& im, int x, int y, int w, int h) {
  if (im.width <= 0 || im.height <= 0 || w <= 0 || h <= 0 ||
      im.rgba.size() != size_t(im.width) * im.height) return;
  for (int yy = 0; yy < h; ++yy)
    for (int xx = 0; xx < w; ++xx)
      ThemePixel(x + xx, y + yy, im.rgba[(yy * im.height / h) * im.width + xx * im.width / w]);
}

// Preserve the native dotted corners; repeat the straight section at its original scale.
void ThemeFrame(const ThemeImage& im, int x, int y, int w, int h) {
  if (im.width < 128 || im.height <= 0 || im.rgba.size() != size_t(im.width) * im.height || h <= 0) return;
  int cap = std::max(1, 64 * h / im.height);
  for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
    int sx = xx < cap ? xx * 64 / cap : xx >= w - cap ? (w - 1 - xx) * 64 / cap :
             64 + ((xx - cap) * im.height / h) % (im.width - 64);
    ThemePixel(x + xx, y + yy, im.rgba[(yy * im.height / h) * im.width + sx]);
  }
}

const ThemeGlyph* Glyph(const ThemeFont& f, wchar_t c) {
  for (const auto& g : f.glyphs) if (g.code == c) return &g;
  // An accented letter the game's font lacks: its bare letter (É -> E).
  wchar_t parts[4];
  if (FoldStringW(MAP_COMPOSITE, &c, 1, parts, 4) > 1 && parts[0] != c)
    for (const auto& g : f.glyphs) if (g.code == parts[0]) return &g;
  for (const auto& g : f.glyphs) if (g.code == '?') return &g;
  return nullptr;
}

int ThemeText(const Theme& t, const ThemeFont& font, const wchar_t* text, int x, int y, int width,
              float scale, uint32_t color, bool right = false, bool paint = true) {
  std::wstring str(text);
  if (!str.empty()) CharUpperBuffW(str.data(), DWORD(str.size()));
  auto measure = [&](const std::wstring& v) {
    float n = 0;
    for (wchar_t c : v) if (auto g = Glyph(font, c)) n += g->advance * scale;
    return int(std::ceil(n));
  };
  if (measure(str) > width) {
    while (!str.empty() && measure(str + L"...") > width) str.pop_back();
    str += L"...";
  }
  int size = measure(str);
  if (!paint) return size;
  float pen = float(right ? x + width - size : x);
  for (wchar_t c : str) {
    const auto* g = Glyph(font, c);
    if (!g) continue;
    int gw = int(std::round(g->width * scale)), gh = int(std::round(g->height * scale));
    for (int yy = 0; yy < gh; ++yy) for (int xx = 0; xx < gw; ++xx) {
      int ax = g->x + xx * g->width / gw, ay = g->y + yy * g->height / gh;
      if (ax >= font.atlas.width || ay >= font.atlas.height) continue;
      uint32_t a = font.atlas.rgba[ay * font.atlas.width + ax] & 0xff000000;
      int px = int(std::round(pen)) + int(std::round(g->offset_x * scale)) + xx;
      if (px >= x && px < x + width)
        ThemePixel(px, y + int(std::round(g->offset_y * scale)) + yy, a | color);
    }
    pen += g->advance * scale;
  }
  return size;
}

void RenderThemedMenu(ID3D11DeviceContext* ctx, const Theme& t) {
  // Uniform scaling also preserves the original proportions on ultrawide windows.
  float scale = std::min(g_menu.w / 1280.f, g_menu.h / 720.f);
  auto S = [&](int n) { return int(std::round(n * scale)); };
  int ox = (g_menu.w - S(1280)) / 2, oy = (g_menu.h - S(720)) / 2;
  int x = ox + S((1280 - t.panel_width) / 2), w = S(t.panel_width);
  int y = oy + S(t.top), panel_y = y, lh = std::max(1, S(t.row_height)), pad = S(t.padding);
  int row_h = std::max(1, lh - S(t.row_gap));
  float ts = scale * t.text_scale;
  const ThemeFont& title_font = t.title_font.glyphs.empty() ? t.font : t.title_font;
  const uint32_t title_color = t.title_text ? t.title_text : t.text;
  std::fill(g_menu.bits, g_menu.bits + size_t(g_menu.w) * g_menu.h, 0xa8060905u);
  auto dots = [&](int dy) {
    if (!t.dots) return;
    for (int dx = x; dx < x + w; dx += std::max(2, S(5)))
      ThemeFill(dx, dy, std::max(1, S(2)), std::max(1, S(2)), 0xff000000 | t.accent);
  };
  const wchar_t* title = T(L"CONFIGURAÇÕES DO PC", L"PC SETTINGS");
  float title_scale = scale * 1.25f * t.title_scale;
  int title_width = ThemeText(t, title_font, title, 0, 0, w, title_scale, title_color, false, false);
  int title_x = t.title_left ? x : x + w - title_width - S(24);
  ThemeFrame(t.panel_top, title_x - S(4), y - S(54), title_width + S(28), S(54));
  if (t.title_outline) {
    // EA's front ends outline the big title with the dark ink of the rows.
    for (int dy : {-1, 0, 1}) for (int dx : {-1, 0, 1}) {
      if (!dx && !dy) continue;
      ThemeText(t, title_font, title, x + dx, y - S(46) + dy, w - S(8), title_scale,
                t.title_outline, !t.title_left);
    }
  }
  ThemeText(t, title_font, title, x, y - S(46), w - S(8), title_scale, title_color, !t.title_left);
  if (t.title_rule) ThemeFill(x, y - S(10), w, std::max(1, S(2)), 0xff000000 | t.accent);
  dots(y - S(8));
  for (int it = 0; it < IT_COUNT; ++it) {
    g_rows[it] = {}; g_value_left[it] = {};
    if (!ItemVisible(it)) continue;
    if (it == IT_RESUME) { y += S(12); dots(y - S(5)); }
    wchar_t label[128], value[160]; bool grey;
    ItemText(it, label, value, &grey);
    bool sel = it == g_sel;
    ThemeFill(x, y, w, row_h, 0xb0000000 | t.panel);
    if (sel) {
      if (!t.highlight.rgba.empty()) {
        ThemeFill(x, y, w, row_h, 0xe0000000 | t.accent);
        // Repeat at the original aspect ratio instead of stretching the stripes.
        int tile = std::max(1, row_h * t.highlight.width / t.highlight.height);
        for (int dx = 0; dx < w; ++dx) for (int dy = 0; dy < row_h; ++dy)
          ThemePixel(x + dx, y + dy, t.highlight.rgba[(dy * t.highlight.height / row_h) *
                     t.highlight.width + (dx % tile) * t.highlight.width / tile]);
      }
      else {
        ThemeFill(x, y, w, row_h, 0xff000000 | t.accent);
        if (!t.flat_highlight)
          for (int dx = 0; dx < w; dx += std::max(2, S(4)))
            ThemeFill(x + dx, y, std::max(1, S(1)), row_h, 0x30202510);
      }
    }
    uint32_t color = sel ? t.selected_text : grey ? t.muted : (t.row_text ? t.row_text : t.text);
    ThemeText(t, t.font, label, x + pad, y - S(2), w / 2 - 2 * pad, ts, color);
    if (*value) {
      std::wstring v = value;
      std::wstring prefix = sel ? L"<  " : L"";
      std::wstring suffix = ItemRestart(it) ? L" *" : L"";
      if (sel) suffix += L"  >";
      auto value_width = [&](std::wstring text) {
        if (!text.empty()) CharUpperBuffW(text.data(), DWORD(text.size()));
        float n = 0; for (wchar_t c : text) if (auto g = Glyph(t.font, c)) n += g->advance * ts;
        return int(std::ceil(n));
      };
      if (value_width(prefix + v + suffix) > w / 2 - pad) {
        while (!v.empty() && value_width(prefix + v + L"..." + suffix) > w / 2 - pad) v.pop_back();
        v += L"...";
      }
      v = prefix + v + suffix;
      int tw = ThemeText(t, t.font, v.c_str(), x + w / 2, y - S(2), w / 2 - pad, ts, color, true);
      if (sel) g_value_left[it] = {x + w - pad - tw - S(5), y, x + w - pad - tw + S(22), y + lh};
    }
    g_rows[it] = {x, y, x + w, y + lh};
    y += lh;
  }
  ThemeFrame(t.panel_bottom, x - S(4), y, w + S(8), S(16));
  dots(y + S(5));
  if (t.dots)
    for (int yy = panel_y - S(8); yy <= y + S(5); yy += std::max(2, S(5))) {
      ThemeFill(x - S(4), yy, std::max(1, S(2)), std::max(1, S(2)), 0xff000000 | t.accent);
      ThemeFill(x + w + S(2), yy, std::max(1, S(2)), std::max(1, S(2)), 0xff000000 | t.accent);
    }
  // Help and restart notices have separate lines so neither can overwrite the other.
  ThemeText(t, t.font, ItemHelp(g_sel), x, y + S(15), w, ts * .78f, t.text);
  if (RestartPending())
    ThemeText(t, t.font, T(L"* VALE DEPOIS DE REINICIAR", L"* APPLIES AFTER RESTARTING"), x,
              y + S(35), w, ts * .78f, t.accent, true);
  int by = oy + S(638);
  ThemeFill(0, by - S(8), g_menu.w, S(58), 0xd810130e);
  auto hint = [&](const ThemeImage& icon, const wchar_t* caption, int hx, Rect& hit) {
    int d = S(38), tw = ThemeText(t, t.font, caption, 0, 0, w, ts, t.text, false, false);
    if (!icon.rgba.empty()) ThemeBlit(icon, hx, by, d, d);
    else {
      int radius = d / 2;
      for (int iy = 0; iy < d; ++iy) for (int ix = 0; ix < d; ++ix) {
        int dist = (ix - radius) * (ix - radius) + (iy - radius) * (iy - radius);
        if (dist <= radius * radius && dist >= (radius - 2) * (radius - 2))
          ThemePixel(hx + ix, by + iy, 0xff000000 | t.accent);
      }
      ThemeText(t, t.font, &hit == &g_hint_a ? L"A" : L"B", hx + S(6), by, d, ts, t.text);
    }
    ThemeText(t, t.font, caption, hx + d + S(8), by + S(8), tw + 1, ts, t.text);
    hit = {hx, by, hx + d + S(8) + tw, by + d};
    return hit.x1;
  };
  const wchar_t* ac = T(L"SELECIONAR", L"SELECT");
  const wchar_t* bc = T(L"VOLTAR", L"BACK");
  int aw = ThemeText(t, t.font, ac, 0, 0, w, ts, t.text, false, false);
  int bw = ThemeText(t, t.font, bc, 0, 0, w, ts, t.text, false, false);
  int hx = x + w - aw - bw - S(124);
  hx = hint(t.button_a, ac, hx, g_hint_a);
  hint(t.button_b, bc, hx + S(32), g_hint_b);
  ThemeText(t, t.font, T(L"ENTER / CLIQUE: SELECIONAR   ESC / F1: VOLTAR", L"ENTER / CLICK: SELECT   ESC / F1: BACK"),
            x, by + S(12), std::max(0, g_hint_a.x0 - x - S(12)), ts * .65f, t.muted);
  g_menu.Upload(ctx);
}

// The menu surface as a 1280x720 canvas for a game's own draw function.
class MenuCanvas final : public Canvas {
 public:
  MenuCanvas() {
    s_ = std::min(g_menu.w / 1280.f, g_menu.h / 720.f);
    ox_ = (g_menu.w - 1280.f * s_) / 2;
    oy_ = (g_menu.h - 720.f * s_) / 2;
  }
  int X(float x) const { return int(std::lround(ox_ + x * s_)); }
  int Y(float y) const { return int(std::lround(oy_ + y * s_)); }

  void Fill(int x, int y, int w, int h, uint32_t argb) override {
    int x0 = X(float(x)), y0 = Y(float(y));
    ThemeFill(x0, y0, X(float(x + w)) - x0, Y(float(y + h)) - y0, argb);
  }
  void Polygon(const int* xy, int points, uint32_t argb) override {
    if (points < 3) return;
    std::vector<float> px(points), py(points);
    float top = 1e9f, bottom = -1e9f;
    for (int i = 0; i < points; ++i) {
      px[i] = ox_ + xy[2 * i] * s_;
      py[i] = oy_ + xy[2 * i + 1] * s_;
      top = std::min(top, py[i]);
      bottom = std::max(bottom, py[i]);
    }
    for (int y = std::max(0, int(std::floor(top))); y < std::min(g_menu.h, int(std::ceil(bottom))); ++y) {
      float fy = y + 0.5f, left = 1e9f, right = -1e9f;
      for (int i = 0; i < points; ++i) {
        int j = (i + 1) % points;
        if ((py[i] <= fy) == (py[j] <= fy)) continue;
        float fx = px[i] + (fy - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
        left = std::min(left, fx);
        right = std::max(right, fx);
      }
      for (int x = std::max(0, int(std::lround(left))); x < std::min(g_menu.w, int(std::lround(right))); ++x)
        ThemePixel(x, y, argb);
    }
  }
  int Measure(const ThemeFont& f, const std::wstring& s, float scale, bool upper) override {
    std::wstring str = Case(s, upper);
    float n = 0;
    for (wchar_t c : str) if (auto g = Glyph(f, c)) n += g->advance * scale;
    return int(std::ceil(n));
  }
  int Text(const ThemeFont& f, const std::wstring& s, int x, int y, int width, float scale, uint32_t argb,
           int align, bool upper) override {
    std::wstring str = Case(s, upper);
    if (Measure(f, str, scale, false) > width) {
      while (!str.empty() && Measure(f, str + L"...", scale, false) > width) str.pop_back();
      str += L"...";
    }
    int size = Measure(f, str, scale, false);
    float pen = float(align == 1 ? x + width - size : align == 2 ? x + (width - size) / 2 : x);
    const float k = scale * s_;
    for (wchar_t c : str) {
      const ThemeGlyph* g = Glyph(f, c);
      if (!g) continue;
      int gw = int(std::round(g->width * k)), gh = int(std::round(g->height * k));
      int bx = X(pen) + int(std::round(g->offset_x * k)), by = Y(float(y)) + int(std::round(g->offset_y * k));
      for (int yy = 0; yy < gh; ++yy)
        for (int xx = 0; xx < gw; ++xx) {
          int ax = g->x + xx * g->width / std::max(1, gw), ay = g->y + yy * g->height / std::max(1, gh);
          if (ax >= f.atlas.width || ay >= f.atlas.height) continue;
          uint32_t a = f.atlas.rgba[size_t(ay) * f.atlas.width + ax] >> 24;
          a = a * (argb >> 24) / 255;
          if (a) ThemePixel(bx + xx, by + yy, (a << 24) | (argb & 0xFFFFFF));
        }
      pen += g->advance * scale;
    }
    return size;
  }
  void Image(const ThemeImage& im, int x, int y, int w, int h) override {
    int x0 = X(float(x)), y0 = Y(float(y));
    ThemeBlit(im, x0, y0, X(float(x + w)) - x0, Y(float(y + h)) - y0);
  }

 private:
  static std::wstring Case(const std::wstring& s, bool upper) {
    std::wstring r = s;
    if (upper && !r.empty()) CharUpperBuffW(r.data(), DWORD(r.size()));
    return r;
  }
  float s_ = 1, ox_ = 0, oy_ = 0;
};

// A game's own screen (Theme::draw): the shared state as rows, its layout from the game.
void RenderGameMenu(ID3D11DeviceContext* ctx, const Theme& t) {
  std::fill(g_menu.bits, g_menu.bits + size_t(g_menu.w) * g_menu.h, 0u);
  MenuView view;
  view.title = T(L"CONFIGURAÇÕES DO PC", L"PC SETTINGS");
  view.subtitle = g_info.title;
  view.select = T(L"SELECIONAR", L"SELECT");
  view.back = T(L"VOLTAR", L"BACK");
  view.keys = T(L"ENTER / CLIQUE: SELECIONAR   ESC / F1: VOLTAR", L"ENTER / CLICK: SELECT   ESC / F1: BACK");
  if (RestartPending()) view.restart_note = T(L"* VALE DEPOIS DE REINICIAR", L"* APPLIES AFTER RESTARTING");
  for (int it = 0; it < IT_COUNT; ++it) {
    g_rows[it] = {};
    g_value_left[it] = {};
    if (!ItemVisible(it)) continue;
    MenuRow row;
    row.id = it;
    wchar_t label[128], value[160];
    ItemText(it, label, value, &row.greyed);
    row.label = label;
    row.value = value;
    row.selected = it == g_sel;
    row.restart = ItemRestart(it);
    if (it == IT_DISPLAY) row.section = T(L"VÍDEO", L"VIDEO");
    if (it == IT_SHOW_FPS) row.section = T(L"SISTEMA", L"SYSTEM");
    if (row.selected) {
      view.help_title = row.label;
      view.help = ItemHelp(it);
    }
    view.rows.push_back(std::move(row));
  }
  MenuCanvas canvas;
  t.draw(t, canvas, view);
  g_hint_a = g_hint_b = {};
  for (const MenuView::Hit& h : view.hits) {
    Rect r{canvas.X(float(h.x0)), canvas.Y(float(h.y0)), canvas.X(float(h.x1)), canvas.Y(float(h.y1))};
    if (h.id == -1) g_hint_a = r;
    else if (h.id == -2) g_hint_b = r;
    else if (h.id >= 0 && h.id < IT_COUNT) (h.value_left ? g_value_left[h.id] : g_rows[h.id]) = r;
  }
  g_menu.Upload(ctx);
}

void RenderMenu(ID3D11DeviceContext* ctx) {
  if (const Theme* theme = MenuTheme()) {
    if (theme->draw) RenderGameMenu(ctx, *theme);
    else RenderThemedMenu(ctx, *theme);
    return;
  }
  const int w = g_menu.w, h = g_menu.h;
  const int lh = std::max(12, h / 28);
  Fonts(lh);
  g_dc = g_menu.dc;
  SelectObject(g_dc, g_menu.bmp);
  SetBkMode(g_dc, TRANSPARENT);
  const int cw = std::min(w * 7 / 10, lh * 34);
  const int x0 = (w - cw) / 2, x1 = x0 + cw;
  Fill(0, 0, w, h, kBg);
  Band(0, lh * 7 / 10, x0 + lh * 12, lh * 3, lh, RGB(30, 44, 70));
  Text(T(L"configurações", L"settings"), x0, lh / 2, x1, lh * 3, RGB(250, 160, 30), g_font_title, DT_LEFT);
  wchar_t sub[200];
  swprintf(sub, 200, T(L"%ls  •  PC  •  vídeo, tela e sistema  •  jogo pausado", L"%ls  •  PC  •  video, display and system  •  game paused"),
           g_info.title);
  Text(sub, x0, lh * 3, x1, lh * 4, RGB(170, 185, 210), g_font_small, DT_LEFT);
  Fill(x0, lh * 4, x1, lh * 4 + 2, RGB(250, 160, 30));
  int y = lh * 4 + lh / 2;
  for (int it = 0; it < IT_COUNT; ++it) {
    g_rows[it] = {0, 0, 0, 0};
    g_value_left[it] = {0, 0, 0, 0};
    if (it == IT_DISPLAY || it == IT_SHOW_FPS) {
      Text(it == IT_DISPLAY ? T(L"VÍDEO", L"VIDEO") : T(L"SISTEMA", L"SYSTEM"), x0, y, x1, y + lh * 8 / 10,
           RGB(250, 160, 30), g_font_small, DT_LEFT);
      y += lh * 8 / 10;
    }
    if (it == IT_RESUME) y += lh / 3;
    if (!ItemVisible(it)) continue;
    wchar_t label[128], value[160];
    bool greyed, sel = it == g_sel;
    ItemText(it, label, value, &greyed);
    if (sel) Band(x0 - lh / 3, y + 1, x1, y + lh - 1, lh / 2, RGB(222, 214, 52));
    Text(label, x0 + lh / 3, y, x0 + cw / 2, y + lh, sel ? RGB(15, 20, 30) : greyed ? RGB(115, 122, 135) : RGB(230, 233, 240),
         sel ? g_font_bold : g_font, DT_LEFT);
    if (value[0]) {
      wchar_t v[220];
      bool arrows = sel;
      swprintf(v, 220, arrows ? L"◀  %ls%ls  ▶" : L"%ls%ls", value, ItemRestart(it) ? L" *" : L"");
      Text(v, x0 + cw / 2, y, x1 - lh / 3, y + lh, sel ? RGB(15, 20, 30) : greyed ? RGB(115, 122, 135) : RGB(190, 205, 230),
           sel ? g_font_bold : g_font, DT_RIGHT);
      if (arrows) {
        SelectObject(g_dc, g_font_bold);
        SIZE sz;
        GetTextExtentPoint32W(g_dc, v, int(wcslen(v)), &sz);
        int vx = x1 - lh / 3 - sz.cx;
        g_value_left[it] = {vx - lh / 2, y, vx + lh, y + lh};
      }
    }
    g_rows[it] = {x0 - lh / 3, y, x1 + lh / 2, y + lh};
    y += lh;
  }
  // Help for the selected line, then the button hints.
  Fill(x0, h - lh * 3, x1, h - lh * 3 + 1, RGB(60, 75, 100));
  Text(ItemHelp(g_sel), x0, h - lh * 3 + lh / 6, x1, h - lh * 2 + lh / 6, RGB(200, 210, 225), g_font_small, DT_LEFT);
  if (RestartPending())
    Text(T(L"* vale depois de reiniciar", L"* applies after restarting"), x0, h - lh * 3 + lh / 6, x1,
         h - lh * 2 + lh / 6, RGB(255, 190, 110), g_font_small, DT_RIGHT);
  int bx = x0, by = h - lh * 2 + lh / 4;
  bx = ButtonHint(bx, by, lh, L'A', RGB(40, 160, 60), T(L"SELECIONAR (Enter / clique)", L"SELECT (Enter / click)"), &g_hint_a);
  bx = ButtonHint(bx, by, lh, L'B', RGB(200, 40, 40), T(L"VOLTAR (Esc / F1)", L"BACK (Esc / F1)"), &g_hint_b);
  Text(T(L"◀ ▶  alterar (botão direito volta)      Alt+Enter  tela cheia",
         L"◀ ▶  change (right click goes back)      Alt+Enter  fullscreen"),
       bx, by, x1, by + lh, RGB(170, 180, 200), g_font_small, DT_LEFT);
  GdiFlush();
  const int n = w * h;
  for (int i = 0; i < n; ++i) {
    uint32_t c = g_menu.bits[i] & 0xFFFFFFu;
    g_menu.bits[i] = c | (c == kBgPixel ? kBgAlpha : 0xFF000000u);
  }
  g_menu.Upload(ctx);
}

// The FPS counter: a small box, repainted when the number changes.
int g_fps_shown = -1;
void RenderHint(ID3D11DeviceContext* ctx, int lh) {
  int h = lh * 13 / 10, w = lh * 15;
  if (!g_hint_surface.Resize(g_gpu.dev, w, h)) return;
  if (g_hint_shown_lang == UiLang()) return;
  g_hint_shown_lang = UiLang();
  g_dc = g_hint_surface.dc;
  SelectObject(g_dc, g_hint_surface.bmp);
  SetBkMode(g_dc, TRANSPARENT);
  Fill(0, 0, w, h, RGB(0, 0, 0));
  Band(0, lh / 10, w, h - lh / 10, lh / 2, RGB(30, 44, 70));
  Fill(lh / 3, lh / 10, lh / 3 + lh / 8, h - lh / 10, RGB(250, 160, 30));
  int x = ButtonHint(lh * 3 / 4, (h - lh) / 2, lh, L'\x25C2', RGB(90, 96, 110), L"+", nullptr);
  x = ButtonHint(x - lh * 3 / 4, (h - lh) / 2, lh, L'\x2261', RGB(90, 96, 110), L"", nullptr);
  Text(T(L"CONFIGURAÇÕES DO PC", L"PC SETTINGS"), x - lh / 2, 0, w - lh * 2, h, RGB(255, 255, 255), g_font_bold, DT_LEFT);
  Text(L"F1", w - lh * 2, 0, w - lh / 2, h, RGB(170, 185, 210), g_font_small, DT_RIGHT);
  GdiFlush();
  for (int i = 0; i < w * h; ++i) {
    uint32_t c = g_hint_surface.bits[i] & 0xFFFFFFu;
    g_hint_surface.bits[i] = c ? (c | 0xE6000000u) : 0u;
  }
  g_hint_surface.Upload(ctx);
}

void RenderFps(ID3D11DeviceContext* ctx, int fps, int lh) {
  int w = lh * 4, h = lh;
  if (!g_fps_surface.Resize(g_gpu.dev, w, h)) return;
  if (fps == g_fps_shown) return;
  g_fps_shown = fps;
  g_dc = g_fps_surface.dc;
  SelectObject(g_dc, g_fps_surface.bmp);
  SetBkMode(g_dc, TRANSPARENT);
  HFONT f = MakeFont(lh * 70 / 100, FW_BOLD, false);
  Fill(0, 0, w, h, RGB(0, 0, 0));
  wchar_t s[32];
  swprintf(s, 32, L"%d FPS", fps);
  Text(s, 0, 0, w, h, fps >= 55 ? RGB(120, 230, 120) : fps >= 28 ? RGB(240, 210, 90) : RGB(240, 90, 80), f, DT_CENTER);
  DeleteObject(f);
  GdiFlush();
  for (int i = 0; i < w * h; ++i) {
    uint32_t c = g_fps_surface.bits[i] & 0xFFFFFFu;
    g_fps_surface.bits[i] = c | (c ? 0xFF000000u : 0xA0000000u);
  }
  g_fps_surface.Upload(ctx);
}

void DrawQuad(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp,
              ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, ID3D11SamplerState* samp, bool blend) {
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  D3D11_RECT sc{0, 0, 16384, 16384};
  ctx->RSSetScissorRects(1, &sc);
  ctx->RSSetState(g_gpu.raster);
  ctx->OMSetBlendState(blend ? g_gpu.blend_alpha : nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(g_gpu.no_depth, 0);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(g_gpu.vs, nullptr, 0);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->PSSetShader(ps, nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, &g_gpu.cb);
  ctx->PSSetShaderResources(0, 1, &srv);
  ctx->PSSetSamplers(0, 1, &samp);
  ctx->Draw(3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  ctx->PSSetShaderResources(0, 1, &none);
}

// A colour target the FSR passes draw into and read back.
struct Target {
  ID3D11Device* dev = nullptr;
  ID3D11Texture2D* tex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;

  bool Resize(ID3D11Device* d, int nw, int nh) {
    if (d == dev && nw == w && nh == h && srv) return true;
    Release(srv);
    Release(rtv);
    Release(tex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(nw);
    td.Height = UINT(nh);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &tex)) || FAILED(d->CreateRenderTargetView(tex, nullptr, &rtv)) ||
        FAILED(d->CreateShaderResourceView(tex, nullptr, &srv))) {
      Release(srv);
      Release(rtv);
      Release(tex);
      return false;
    }
    dev = d;
    w = nw;
    h = nh;
    return true;
  }
};

Target g_fsr_source, g_fsr_easu;

// AMD FSR 1.0 from the picture (cb already holds its constants) to the viewport: the picture at
// its own size (red/blue fixed, FXAA if chosen), EASU up to the viewport size, RCAS into the
// back buffer. False when the picture is not smaller than the viewport or a target failed.
bool DrawFsr(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, const Picture& pic,
             float tw, float th) {
  int sw = int(pic.u1 * tw + 0.5f), sh = int(pic.v1 * th + 0.5f);
  int vw = int(vp.Width), vh = int(vp.Height);
  if (sw < 16 || sh < 16 || (vw <= sw && vh <= sh)) return false;
  if (!g_fsr_source.Resize(g_gpu.dev, sw, sh) || !g_fsr_easu.Resize(g_gpu.dev, vw, vh)) return false;
  D3D11_VIEWPORT src{0, 0, float(sw), float(sh), 0, 1};
  DrawQuad(ctx, g_fsr_source.rtv, src, g_opt.aa ? g_gpu.ps_fxaa : g_gpu.ps_pic, pic.srv, g_gpu.linear, false);
  float easu[8] = {float(sw), float(sh), float(vw), float(vh), 0, 0, 0, 0};
  ctx->UpdateSubresource(g_gpu.cb, 0, nullptr, easu, 0, 0);
  D3D11_VIEWPORT up{0, 0, float(vw), float(vh), 0, 1};
  DrawQuad(ctx, g_fsr_easu.rtv, up, g_gpu.ps_easu, g_fsr_source.srv, g_gpu.point, false);
  // Sharpness 0.2 stops (AMD's default), as a linear amount: exp2(-0.2).
  float rcas[8] = {0, 0, float(vw), float(vh), 0, vp.TopLeftX, vp.TopLeftY, 0.8705506f};
  ctx->UpdateSubresource(g_gpu.cb, 0, nullptr, rcas, 0, 0);
  DrawQuad(ctx, rtv, vp, g_gpu.ps_rcas, g_fsr_easu.srv, g_gpu.point, false);
  return true;
}

int g_fps_now = 0;

// Diagnostics: PCS_AUTOTEST=<seconds>[:<script>] opens the menu by itself after that many
// seconds of frames and plays the script, one letter every 6 menu frames: U D L R arrows,
// E Enter, X Esc, C capture, W wait 20 frames. PCS_SHOT_EVERY=<seconds> captures the
// presented frame now and then. Captures (24-bit BMP) go to PCS_SHOT_DIR, default the
// executable's folder, as pcs-<tag>.bmp.
std::string g_capture_tag;
std::string g_auto_script = "WCDDDDRWCX";
double g_auto_at = -1;
size_t g_auto_step = 0;
int g_auto_wait = 0, g_auto_shots = 0;
bool g_autotest_session = false;

void Capture(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const std::string& tag) {
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (!res) return;
  D3D11_TEXTURE2D_DESC td;
  static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (SUCCEEDED(g_gpu.dev->CreateTexture2D(&td, nullptr, &staging))) {
    ctx->CopyResource(staging, res);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      const char* dir = std::getenv("PCS_SHOT_DIR");
      std::string path;
      if (dir && *dir) {
        path = std::string(dir) + "/";
      } else {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        path.assign(exe, n);
        path = path.substr(0, path.find_last_of("\\/") + 1);
      }
      path += "pcs-" + tag + ".bmp";
      uint32_t w = td.Width, h = td.Height, row = (w * 3 + 3) & ~3u;
      std::vector<uint8_t> bmp(54 + size_t(row) * h, 0);
      auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&bmp[o], &v, 4); };
      bmp[0] = 'B';
      bmp[1] = 'M';
      put32(2, uint32_t(bmp.size()));
      put32(10, 54);
      put32(14, 40);
      put32(18, w);
      put32(22, uint32_t(-int32_t(h)));
      bmp[26] = 1;
      bmp[28] = 24;
      bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* dst = &bmp[54 + size_t(y) * row];
        for (uint32_t x = 0; x < w; ++x) {
          dst[x * 3 + 0] = src[x * 4 + (bgra ? 0 : 2)];
          dst[x * 3 + 1] = src[x * 4 + 1];
          dst[x * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
        }
      }
      ctx->Unmap(staging, 0);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bmp.data(), 1, bmp.size(), f);
        std::fclose(f);
        REXLOG_INFO("[pc] captured {}", path);
      }
    }
    staging->Release();
  }
  res->Release();
}

void AutotestStep() {
  if (g_auto_wait > 0) {
    --g_auto_wait;
    return;
  }
  if (g_auto_step >= g_auto_script.size()) return;
  char c = g_auto_script[g_auto_step++];
  g_auto_wait = c == 'W' ? 20 : 6;
  switch (c) {
    case 'U': MenuKey(VK_UP); break;
    case 'D': MenuKey(VK_DOWN); break;
    case 'L': MenuKey(VK_LEFT); break;
    case 'R': MenuKey(VK_RIGHT); break;
    case 'E': MenuKey(VK_RETURN); break;
    case 'X': MenuKey(VK_ESCAPE); break;
    case 'C': g_capture_tag = "menu" + std::to_string(++g_auto_shots); break;
  }
}

// The whole presented frame: picture, menu, counter.
bool Compose(ID3D11DeviceContext* ctx, const Picture& pic, const BackBufferFn& back_buffer) {
  ID3D11RenderTargetView* rtv = nullptr;
  uint32_t bw = 0, bh = 0;
  if (!back_buffer(&rtv, &bw, &bh) || !rtv || bw < 16 || bh < 16) return false;
  float black[4] = {0, 0, 0, 1};
  ctx->ClearRenderTargetView(rtv, black);
  if (pic.srv) {
    D3D11_VIEWPORT vp{0, 0, float(bw), float(bh), 0, 1};
    if (!g_opt.aspect && pic.width > 0 && pic.height > 0) {
      float s = std::min(float(bw) / pic.width, float(bh) / pic.height);
      vp.Width = std::floor(pic.width * s + 0.5f);
      vp.Height = std::floor(pic.height * s + 0.5f);
      vp.TopLeftX = std::floor((float(bw) - vp.Width) * 0.5f);
      vp.TopLeftY = std::floor((float(bh) - vp.Height) * 0.5f);
    }
    // Keep bilinear taps inside the used part of the texture.
    ID3D11Resource* res = nullptr;
    pic.srv->GetResource(&res);
    float tw = 1, th = 1;
    if (res) {
      D3D11_TEXTURE2D_DESC td;
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
      tw = float(td.Width);
      th = float(td.Height);
      res->Release();
    }
    float cb[8] = {pic.u1, pic.v1, pic.u1 - 0.5f / tw, pic.v1 - 0.5f / th, pic.swap_rb ? 1.0f : 0.0f, 0, 0, 0};
    ctx->UpdateSubresource(g_gpu.cb, 0, nullptr, cb, 0, 0);
    if (!(g_opt.upscale && g_info.internal_resolution && DrawFsr(ctx, rtv, vp, pic, tw, th)))
      DrawQuad(ctx, rtv, vp, g_opt.aa ? g_gpu.ps_fxaa : g_gpu.ps_pic, pic.srv, g_gpu.linear, false);
  }
  D3D11_VIEWPORT full{0, 0, float(bw), float(bh), 0, 1};
  if (g_open) {
    if (g_menu.w != int(bw) || g_menu.h != int(bh) || g_menu.dev != g_gpu.dev) g_dirty = true;
    if (!g_menu.Resize(g_gpu.dev, int(bw), int(bh))) return true;
    if (g_dirty) {
      RenderMenu(ctx);
      g_dirty = false;
    }
    DrawQuad(ctx, rtv, full, g_gpu.ps_overlay, g_menu.srv, g_gpu.point, true);
    if (!g_capture_tag.empty()) {
      Capture(ctx, rtv, g_capture_tag);
      g_capture_tag.clear();
    }
  } else if (g_opt.show_fps) {
    int lh = std::max(14, int(bh) / 30);
    RenderFps(ctx, g_fps_now, lh);
    if (g_fps_surface.srv) {
      D3D11_VIEWPORT vp{float(lh / 2), float(lh / 2), float(g_fps_surface.w), float(g_fps_surface.h), 0, 1};
      DrawQuad(ctx, rtv, vp, g_gpu.ps_overlay, g_fps_surface.srv, g_gpu.point, true);
    }
  }
  if (HintVisible()) {
    int lh = std::max(14, int(bh) / 30);
    Fonts(lh);
    RenderHint(ctx, lh);
    if (g_hint_surface.srv) {
      float x = float(bw) - float(g_hint_surface.w) - float(lh), y = float(lh) * 0.6f;
      D3D11_VIEWPORT vp{x, y, float(g_hint_surface.w), float(g_hint_surface.h), 0, 1};
      DrawQuad(ctx, rtv, vp, g_gpu.ps_overlay, g_hint_surface.srv, g_gpu.point, true);
      g_hint_rect = {int(x), int(y), int(x) + g_hint_surface.w, int(y) + g_hint_surface.h};
    }
  } else {
    g_hint_rect = {0, 0, 0, 0};
  }
  if (!g_open && !g_capture_tag.empty()) {
    Capture(ctx, rtv, g_capture_tag);
    g_capture_tag.clear();
  }
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  return true;
}

// ------------------------------------------------------------------------ pacing

bool g_tearing = false;
int g_slow_vsync = 0;
bool g_vsync_broken = false;
Clock::time_point g_next_frame{};
Clock::time_point g_fps_window = Clock::now();
int g_fps_frames = 0;
double g_fps_worst_ms = 0;
Clock::time_point g_last_present = Clock::now();
bool g_fps_log = false;

void PresentAndPace(IDXGISwapChain1* swap, bool menu) {
  bool visible = g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
  BOOL exclusive = FALSE;
  swap->GetFullscreenState(&exclusive, nullptr);
  UINT interval = g_opt.vsync && visible && !g_vsync_broken ? 1 : 0;
  UINT flags = !interval && g_tearing && !exclusive ? DXGI_PRESENT_ALLOW_TEARING : 0;
  auto t0 = Clock::now();
  if (visible) {
    HRESULT hr = swap->Present(interval, flags);
    if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING) {
      static int logs = 0;
      if (logs++ < 10) REXLOG_WARN("[pc] Present failed {:08X}", uint32_t(hr));
    }
  }
  auto t1 = Clock::now();
  // Vsync waits must never stall the game: DXGI can block for seconds when DWM is not
  // composing the window. After a few of those, frames are paced on the CPU instead.
  if (interval && t1 - t0 > std::chrono::milliseconds(250) && ++g_slow_vsync >= 3 && !g_vsync_broken) {
    g_vsync_broken = true;
    REXLOG_WARN("[pc] vsync present blocked for {} ms: pacing on the CPU instead",
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
  }
  int fps = menu ? 60 : TargetFps();
  if (!visible) fps = 60;
  if (fps > 0) {
    auto period = std::chrono::nanoseconds(1000000000LL / fps);
    auto now = Clock::now();
    if (now - g_next_frame > period * 4) g_next_frame = now;  // fell behind (loading)
    g_next_frame += period;
    // Half a millisecond of slack: a game paced by its own 60 Hz vblank never waits here.
    auto target = g_next_frame - std::chrono::microseconds(500);
    if (target > now) {
      if (target - now > std::chrono::milliseconds(2))
        std::this_thread::sleep_for(target - now - std::chrono::milliseconds(2));
      while (Clock::now() < target) std::this_thread::yield();
    }
  }
  if (menu) return;
  auto now = Clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - g_last_present).count();
  g_last_present = now;
  g_fps_worst_ms = std::max(g_fps_worst_ms, ms);
  ++g_fps_frames;
  double window = std::chrono::duration<double>(now - g_fps_window).count();
  if (window >= 1.0) {
    g_fps_now = int(g_fps_frames / window + 0.5);
    static int seconds = 0;
    if (g_fps_log && ++seconds % 5 == 0)
      REXLOG_INFO("[pc] fps {} (worst frame {:.1f} ms)", g_fps_now, g_fps_worst_ms);
    g_fps_frames = 0;
    g_fps_worst_ms = 0;
    g_fps_window = now;
  }
}

// --------------------------------------------------------------------------- setup

bool g_attached = false;
bool g_mouse_look = false;  // UI thread

void Attach() {
  if (g_attached) return;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->display_window()) return;
  g_attached = true;
  g_window = runtime->display_window();
  g_hwnd = static_cast<HWND>(native_host::WindowHandle(g_window));
  g_fps_log = EnvInt("PCS_FPS_LOG", 0) != 0;
  MonitorsRefresh();
  RatesRefresh();
  CallInUi([] {
    g_window->AddInputListener(&g_listener, 1000000);
    if (g_info.window_title && *g_info.window_title) g_window->SetTitle(g_info.window_title);
  });
  // The first frame applies the display mode (the SDK opened the window from the ini).
  g_display_dirty = true;
}

void MenuLoop(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain1* swap, const Picture& pic,
              const BackBufferFn& back_buffer) {
  constexpr WORD kDirs = kPadUp | kPadDown | kPadLeft |
                         kPadRight;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_events.clear();
  }
  g_open = true;
  g_request = false;
  MonitorsRefresh();
  RatesRefresh();
  if (!g_gpus_listed) GpusRefresh();
  g_sel = IT_RESUME;
  g_dirty = true;
  // Free the pointer if the SDK's mouse look holds it (BF2: mnk_mouse), for the menu.
  CallInUi([] {
    g_mouse_look = rex::input::mnk::IsMouseLookActive();
    if (g_mouse_look) rex::input::mnk::SetMouseLookActive(false);
    if (!g_window) return;
    g_window->SetRelativeMouseMode(false);
    g_window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  });
  REXLOG_INFO("[pc] settings menu open: game paused");
  WORD prev = PadsRead(true), rep_b = 0;  // the buttons that opened it do not count
  DWORD rep_t = 0;
  int ui_language = g_opt.ui_language;
  while (g_open) {
    std::deque<Event> events;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      events.swap(g_events);
    }
    for (const Event& e : events) {
      if (!g_open) break;
      if (e.kind == Event::kKey) MenuKey(e.value);
      else MenuMouse(e);
    }
    DWORD now = GetTickCount();
    WORD b = PadsRead(false);
    WORD edge = b & WORD(~prev);
    if (edge & kDirs) {
      rep_b = edge & kDirs;
      rep_t = now + 350;
    } else if ((b & kDirs) && (b & kDirs) == rep_b && LONG(now - rep_t) >= 0) {
      edge |= rep_b;
      rep_t = now + 110;
    }
    if (!(b & kDirs)) rep_b = 0;
    prev = b;
    if (edge & kPadUp) MenuKey(VK_UP);
    if (edge & kPadDown) MenuKey(VK_DOWN);
    if (edge & kPadLeft) MenuKey(VK_LEFT);
    if (edge & kPadRight) MenuKey(VK_RIGHT);
    if (edge & kPadA) MenuKey(VK_RETURN);
    if (edge & (kPadB | kPadStart | kPadBack)) MenuKey(VK_ESCAPE);
    if (g_autotest_session) AutotestStep();
    if (ui_language != g_opt.ui_language) { ui_language = g_opt.ui_language; g_dirty = true; }
    ApplyDisplay(swap);
    if (Compose(ctx, pic, back_buffer)) PresentAndPace(swap, true);
    else Sleep(15);
  }
  // A button still held would reach the game as a fresh press.
  for (int i = 0; i < 100 && PadsRead(false); ++i) Sleep(15);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_events.clear();
  }
  SaveIni();
  g_dirty = true;
  g_autotest_session = false;
  g_next_frame = Clock::now();
  g_last_present = Clock::now();
  int display = g_opt.display;
  CallInUi([display] {
    if (g_mouse_look) rex::input::mnk::SetMouseLookActive(true);
    if (g_window)
      g_window->SetCursorVisibility(display ? rex::ui::Window::CursorVisibility::kAutoHidden
                                            : rex::ui::Window::CursorVisibility::kVisible);
  });
  REXLOG_INFO("[pc] settings menu closed: display {}, vsync {}, fps {}, fxaa {}, msaa {}, aniso {}, aspect {}, volume {}{}",
              g_opt.display, g_opt.vsync, g_opt.fps, g_opt.aa, g_opt.msaa, g_opt.aniso, g_opt.aspect, g_opt.volume,
              RestartPending() ? " (restart pending)" : "");
  (void)dev;
}

}  // namespace

// ------------------------------------------------------------------------- public

void Configure(const GameInfo& info, bool sdk_flags) {
  g_info = info;
  g_info.effect_count = info.effects ? Clampi(info.effect_count, 0, kMaxEffects) : 0;
  const char* e = std::getenv("PCS_OPTIONS");
  g_use_ini = !(e && *e == '0');
  GetModuleFileNameW(nullptr, g_ini, MAX_PATH);
  wchar_t* slash = wcsrchr(g_ini, L'\\');
  wcscpy(slash ? slash + 1 : g_ini, info.ini_name);
  g_opt.display = Clampi(IniGet(L"modo_tela", 0), 0, 2);
  g_opt.monitor = Clampi(IniGet(L"monitor", 0), 0, 15);
  g_opt.refresh = Clampi(IniGet(L"taxa_hz", 0), 0, 1000);
  g_opt.vsync = IniGet(L"vsync", 1) != 0;
  g_opt.fps = IniGet(L"limite_fps", 60);
  if (g_opt.fps != 30 && g_opt.fps != 0 && g_opt.fps != kFpsMonitor) g_opt.fps = 60;
  g_opt.scale = info.internal_resolution ? Clampi(IniGet(L"resolucao", 0), 0, kNumRes - 1) : 0;
  g_opt.aspect = Clampi(IniGet(L"proporcao", 0), 0, 1);
  g_opt.aa = Clampi(IniGet(L"fxaa", 0), 0, 1);
  g_opt.aniso = IniGet(L"anisotropico", 0);
  if (std::find(std::begin(k_aniso), std::end(k_aniso), g_opt.aniso) == std::end(k_aniso)) g_opt.aniso = 0;
  g_opt.msaa = info.msaa ? IniGet(L"msaa", 0) : 0;
  g_opt.adapter = Clampi(IniGet(L"gpu", -1), -1, 15);
  g_opt.api = Clampi(IniGet(L"api_grafica", 0), 0, 1);
  g_opt.language = IniGet(L"idioma", 0);
  if (!LanguageListed(g_opt.language)) g_opt.language = 0;
  g_opt.volume = Clampi(IniGet(L"volume", 100), 0, 100);
  g_opt.ui_language = Clampi(IniGet(L"idioma_menu", -1), -1, kUiLangCount - 1);
  g_opt.upscale = Clampi(IniGet(L"ampliacao", 0), 0, 1);
  g_opt.show_fps = Clampi(IniGet(L"mostrar_fps", 0), 0, 1);
  g_hint_enabled = IniGet(L"botao_menu", 1) != 0;
  // Test runs: environment wins over the ini.
  if (info.internal_resolution) g_opt.scale = Clampi(EnvInt("PCS_RESOLUTION", g_opt.scale), 0, kNumRes - 1);
  g_opt.vsync = EnvInt("PCS_VSYNC", g_opt.vsync) != 0;
  if (!info.real_time && !info.unlocked_fps && g_opt.fps != 30) g_opt.fps = 60;
  if (int fps = EnvInt("PCS_FPS", -2); fps >= -1) g_opt.fps = fps;  // tests: any rate, any game
  g_opt.aa = EnvInt("PCS_FXAA", g_opt.aa) != 0;
  g_opt.upscale = EnvInt("PCS_UPSCALE", g_opt.upscale) != 0;
  g_opt.ui_language = Clampi(EnvInt("PCS_UI_LANGUAGE", g_opt.ui_language), -1, kUiLangCount - 1);
  g_opt.aniso = EnvInt("PCS_ANISO", g_opt.aniso);
  if (info.msaa) g_opt.msaa = EnvInt("PCS_MSAA", g_opt.msaa);
  if (std::find(std::begin(k_msaa), std::end(k_msaa), g_opt.msaa) == std::end(k_msaa)) g_opt.msaa = 0;
  g_opt.show_fps = EnvInt("PCS_SHOW_FPS", g_opt.show_fps) != 0;
  g_opt.display = Clampi(EnvInt("PCS_DISPLAY", g_opt.display), 0, 2);
  g_opt.monitor = Clampi(EnvInt("PCS_MONITOR", g_opt.monitor), 0, 15);
  uint32_t effects = 0xff;
  for (int i = 0; i < g_info.effect_count; ++i) {
    if (!IniGet(EffectKey(i).c_str(), g_info.effects[i].default_on ? 1 : 0)) effects &= ~(1u << i);
  }
  // A later DLL configuration inherits the already-published live state.
  if (!sdk_flags) ReadEffects(effects);
  if (const char* fx = std::getenv("PCS_FX"); fx && *fx) {
    std::string list(fx);
    size_t start = 0;
    while (start < list.size()) {
      size_t end = list.find(',', start);
      std::string entry = list.substr(start, end - start);
      for (int i = 0; i < g_info.effect_count; ++i) {
        std::string key = std::string(g_info.effects[i].key) + "=";
        if (entry == key + "0") effects &= ~(1u << i);
        else if (entry == key + "1") effects |= 1u << i;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  g_effect_mask = effects;
  PublishEffects();
  g_effect_remote = !sdk_flags;
  if (const char* api = std::getenv("PCS_API"); api && *api) g_opt.api = *api == 'v' || *api == 'V' || *api == '1';
  if (int lang = EnvInt("PCS_LANGUAGE", -1); lang == 0 || LanguageListed(lang)) g_opt.language = lang;
  // The game-menu entry is published for the other modules of the process (env wins).
  if (!GetEnvironmentVariableA("PCS_GAME_MENU", nullptr, 0))
    SetEnvironmentVariableA("PCS_GAME_MENU", IniGet(L"menu_no_jogo", 1) ? "1" : "0");
#ifndef _WIN32
  g_opt.api = 1;  // Linux has only the native Vulkan backend.
#endif
  g_boot = g_opt;
  g_configured = true;
  if (!sdk_flags) return;

  // SDK window: opened at the right size and mode; the game in the chosen language.
  uint32_t w, h;
  WindowSize(g_opt.scale, w, h);
  const Monitor& m = MonitorCurrent();
  int aw = int(m.work.right - m.work.left) * 9 / 10, ah = int(m.work.bottom - m.work.top) * 9 / 10;
  int ww = int(w), wh = int(h);
  if (ww > aw) { wh = wh * aw / ww; ww = aw; }
  if (wh > ah) { ww = ww * ah / wh; wh = ah; }
  // Logical units: the SDK scales them by the window's DPI.
  HDC screen = GetDC(nullptr);
  int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
  if (screen) ReleaseDC(nullptr, screen);
  if (dpi <= 0) dpi = 96;
  rex::cvar::SetFlagByName("window_width", std::to_string(ww * 96 / dpi));
  rex::cvar::SetFlagByName("window_height", std::to_string(wh * 96 / dpi));
  rex::cvar::SetFlagByName("fullscreen", g_opt.display ? "true" : "false");
  if (g_opt.language) rex::cvar::SetFlagByName("user_language", std::to_string(g_opt.language));

  // Keyboard as the controller (the SDK's keyboard/mouse driver, merged with any real pad).
  // EA SPORTS FC 27 default "WASD" layout, basic actions only; buttons as in FIFA on Xbox
  // (A pass, B shoot, X lob/cross, Y through pass, RT sprint, LB change player). Semicolon is
  // the key right of L (Ç on ABNT2). Every other bind is cleared: the SDK defaults reuse K, P
  // and the arrows for other buttons.
  if (info.football_keyboard && IniGet(L"teclado", 1) != 0) {
    static const char* const kBinds[][2] = {
        {"keybind_lstick_up", "W,Up"},          {"keybind_lstick_down", "S,Down"},
        {"keybind_lstick_left", "A,Left"},      {"keybind_lstick_right", "D,Right"},
        {"keybind_a", "L,Return"},              // ground pass / press; menus: confirm
        {"keybind_b", "Semicolon,Backspace"},   // shoot / tackle; menus: back
        {"keybind_x", "K"},                     // lob pass / cross / sliding tackle
        {"keybind_y", "O"},                     // through pass / goalkeeper rush
        {"keybind_right_trigger", "P"},         // sprint
        {"keybind_left_shoulder", "I"},         // change player
        {"keybind_start", "Escape,Return"},     // pause; Enter also passes "PRESS START"
        {"keybind_back", "Tab"},
        {"keybind_left_trigger", ""},           {"keybind_right_shoulder", ""},
        {"keybind_lstick_press", ""},           {"keybind_rstick_press", ""},
        {"keybind_rstick_up", ""},              {"keybind_rstick_down", ""},
        {"keybind_rstick_left", ""},            {"keybind_rstick_right", ""},
        {"keybind_dpad_up", ""},                {"keybind_dpad_down", ""},
        {"keybind_dpad_left", ""},              {"keybind_dpad_right", ""},
        {"keybind_guide", ""}};
    for (const auto& b : kBinds) rex::cvar::SetFlagByName(b[0], b[1]);
    rex::cvar::SetFlagByName("mnk_mode", "true");
  }
}

const Settings& Get() { return g_opt; }

float ResolutionScale() {
  if (!g_configured || !g_info.internal_resolution) return 1.0f;
  uint32_t w, h;
  InternalSize(g_opt.scale, w, h);  // live: the renderers re-read it every frame
  return std::clamp(float(h) / float(g_info.native_height), 0.5f, 6.0f);
}

bool EffectEnabled(int index) {
  if (index < 0 || index >= kMaxEffects) return true;
  if (g_effect_remote.load()) {
    int64_t now = SteadyMs();
    if (now - g_effect_checked.load() >= 250 && !g_effect_reading.test_and_set()) {
      // Recheck under the nonblocking guard: concurrent render threads share one poll.
      if (now - g_effect_checked.load() >= 250) {
        uint32_t mask;
        if (ReadEffects(mask)) g_effect_mask = mask;
        g_effect_checked = SteadyMs();
      }
      g_effect_reading.clear();
    }
  }
  return (g_effect_mask.load() & (1u << index)) != 0;
}

int Anisotropy() { return g_opt.aniso; }

int MsaaSamples() {
  if (!g_configured || !g_info.msaa) return 1;
  return g_opt.msaa > 1 ? g_opt.msaa : 1;  // live: the renderers re-read it every frame
}

uint32_t SupportedSamples(ID3D11Device* device, uint32_t want, std::initializer_list<DXGI_FORMAT> formats) {
  for (; want > 1; want /= 2) {
    bool ok = true;
    for (DXGI_FORMAT f : formats) {
      UINT quality = 0;
      if (FAILED(device->CheckMultisampleQualityLevels(f, want, &quality)) || !quality) {
        ok = false;
        break;
      }
    }
    if (ok) return want;
  }
  return 1;
}

uint32_t SwapChainFlags() {
  static uint32_t flags = [] {
    BOOL allow = FALSE;
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(Api().factory(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
      if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) allow = FALSE;
      f5->Release();
    }
    g_tearing = allow != FALSE;
    return allow ? uint32_t(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
  }();
  return flags;
}

IDXGIAdapter1* Adapter() {
  if (g_boot.adapter < 0 && !Api().vulkan) return nullptr;
  IDXGIFactory1* f = nullptr;
  if (FAILED(Api().factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return nullptr;
  IDXGIAdapter1* a = nullptr;
  if (FAILED(f->EnumAdapters1(UINT(std::max(g_boot.adapter, 0)), &a))) {
    a = nullptr;
    if (Api().vulkan) f->EnumAdapters1(0, &a);  // the card is gone: DXVK's first one
  }
  f->Release();
  return a;
}

bool Vulkan() { return Api().vulkan; }

long CreateDevice(unsigned flags, ID3D11Device** device, ID3D11DeviceContext** context) {
  IDXGIAdapter1* adapter = Adapter();
  D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = Api().device(adapter, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, device, nullptr, context);
  if (hr == E_INVALIDARG)  // runtimes that do not know 11_1
    hr = Api().device(adapter, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, device, nullptr, context);
  if (FAILED(hr) && Api().vulkan)
    REXLOG_ERROR("[pc] DXVK could not create a device ({:08X}): is there a Vulkan driver?", uint32_t(hr));
  if (adapter) adapter->Release();
  return hr;
}

float Volume() { return g_open ? 0.0f : float(g_opt.volume) / 100.0f; }

double GuestRefreshHz() {
  static const double forced = [] {
    const char* e = std::getenv("PCS_VBLANK_HZ");
    double v = e && *e ? std::atof(e) : 0.0;
    return v >= 20.0 && v <= 1000.0 ? v : 0.0;
  }();
  if (forced > 0.0) return forced;
  // The module that shows the menu decides and publishes the rate in the environment, for the
  // other module of the process (Street 4: the menu lives in the game DLL, the vblank in the
  // exe). Refreshed four times a second.
  static std::atomic<int64_t> checked{-1000000};
  static std::atomic<double> hz{60.0};
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  if (now - checked.load() > 250) {
    checked = now;
    double local = 60.0;
    if ((g_info.real_time || g_info.unlocked_fps) && !g_open) {
      int fps = TargetFps();
      // Keep the compatibility display interrupt from imposing 60/500 Hz in uncapped mode
      // (BF2's opt-in). This is an interrupt service rate, not a promise of rendered frames.
      double unlimited = g_info.unlocked_fps ? 1000.0 : 500.0;
      local = fps == 0 ? unlimited : fps < 60 ? 60.0 : double(fps);
    }
    char text[32];
    if (g_attached) {
      std::snprintf(text, sizeof(text), "%g", local);
      SetEnvironmentVariableA("PCS_GUEST_HZ", text);
      hz = local;
    } else {
      DWORD n = GetEnvironmentVariableA("PCS_GUEST_HZ", text, sizeof(text));
      double shared = n && n < sizeof(text) ? std::atof(text) : 0.0;
      hz = shared >= 20.0 ? shared : local;
    }
  }
  return hz.load();
}

bool MenuOpen() { return g_open; }

void RequestMenu() { g_request = true; }

void SetGameMenus(bool on_menus) {
  if (on_menus) g_game_menus_until = SteadyMs() + 500;
  else g_game_menus_until = 0;
}

bool GameMenuEntry() {
  static const bool on = [] {
    char text[8];
    DWORD n = GetEnvironmentVariableA("PCS_GAME_MENU", text, sizeof(text));
    return !(n && n < sizeof(text) && text[0] == '0');
  }();
  return on;
}

void Present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain1* swap_chain,
             const Picture& picture, const BackBufferFn& back_buffer) {
  if (!device || !context || !swap_chain) return;
  Attach();
  g_swap = swap_chain;
  SwapChainFlags();
  if (!GpuSetup(device)) {
    swap_chain->Present(0, 0);
    return;
  }
  ApplyDisplay(swap_chain);
  // Back+Start on any pad opens the menu.
  static WORD prev = 0;
  WORD b = PadsRead(false);
  constexpr WORD kCombo = kPadBack | kPadStart;
  if ((b & kCombo) == kCombo && (prev & kCombo) != kCombo) g_request = true;
  prev = b;
  static const auto start = Clock::now();
  static double shot_every = [] {
    const char* e = std::getenv("PCS_SHOT_EVERY");
    return e && *e ? std::atof(e) : 0.0;
  }();
  static double next_shot = shot_every;
  double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  if (shot_every > 0 && elapsed >= next_shot) {
    g_capture_tag = "t" + std::to_string(int(elapsed));
    next_shot += shot_every;
  }
  if (g_auto_at < 0) {
    const char* e = std::getenv("PCS_AUTOTEST");
    g_auto_at = e && *e ? std::atof(e) : 1e30;
    if (const char* colon = e ? std::strchr(e, ':') : nullptr; colon && colon[1]) g_auto_script = colon + 1;
  }
  if (elapsed >= g_auto_at) {
    g_auto_at = 1e30;
    g_autotest_session = true;
    g_auto_step = 0;
    g_auto_wait = 0;
    g_request = true;
    REXLOG_INFO("[pc] autotest: opening the settings menu (script {})", g_auto_script);
  }
  if (g_request && !g_open) MenuLoop(device, context, swap_chain, picture, back_buffer);
  if (Compose(context, picture, back_buffer)) PresentAndPace(swap_chain, false);
}

}  // namespace pcs
