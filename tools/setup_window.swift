// setup_window.swift <log> <pid> — first-launch window of the source-only app (make_app.sh --from-source). Every second it
// rereads the last "[prepare] NN %  step" line of the build_steam.sh log and shows the four steps, the time left
// and the battery state, in the Mac's language (9 languages, English otherwise). Exit codes read by the launcher:
// 0 = "Play" (boot built), 3 = "Try Again" (build_steam.sh resumes where it stopped), 1 = window closed (the
// preparation keeps going). The icon is the one the launcher read from the player's t6sp.exe (<log folder>/icon.png).
import AppKit
import IOKit.ps
import SwiftUI

let args = CommandLine.arguments
guard args.count == 3, let p = pid_t(args[2]) else { exit(2) }
let pid = p
let journal = args[1]
let dossier = (journal as NSString).deletingLastPathComponent
// The last mode the conversion goes to (scripts/build_steam.sh, issue #3)
let demande = (try? String(contentsOfFile: dossier + "/boot_cache/conversion_target", encoding: .utf8))?
    .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
let cible: String = demande == "sps" || demande == "zms" ? demande : "mps"

struct Etape { let cle: String; let debut, fin, minutes: Double }
// Bounds = the percentages build_steam.sh prints; minutes = an M1 with 4 performance cores
// (fresh lift, 2026-10-25; Steam: full run without interruption, 2026-11-01, then the forking remill of 2026-11-02:
// campaign measured at 61 min, Zombies and multiplayer = their 11-01 times minus the same lift saving, not measured).
let etapes = [
    Etape(cle: "e1", debut: 0, fin: 10, minutes: 6), Etape(cle: "s2", debut: 10, fin: 40, minutes: 61),
    Etape(cle: "s3", debut: 40, fin: 70, minutes: 36), Etape(cle: "s4", debut: 70, fin: 100, minutes: 50),
]

let textes: [String: [String: String]] = [
    "sous_titre": ["fr": "Première ouverture sur ce Mac", "en": "First launch on this Mac", "de": "Erster Start auf diesem Mac", "es": "Primera apertura en este Mac", "it": "Primo avvio su questo Mac", "pt": "Primeira abertura neste Mac", "pl": "Pierwsze uruchomienie na tym Macu", "ru": "Первый запуск на этом Mac", "ja": "このMacでの初回起動"],
    "pret": ["fr": "Prêt à jouer", "en": "Ready to play", "de": "Bereit zum Spielen", "es": "Listo para jugar", "it": "Pronto per giocare", "pt": "Pronto para jogar", "pl": "Gotowe do gry", "ru": "Готово к игре", "ja": "プレイの準備完了"],
    "intro": ["fr": "Votre jeu est un programme Windows pour processeur Intel. Il est traduit une seule fois pour votre puce Apple ; ensuite, il s'ouvre directement.", "en": "Your game is a Windows program for Intel processors. It gets translated once for your Apple chip; after that, it opens straight away.", "de": "Ihr Spiel ist ein Windows-Programm für Intel-Prozessoren. Es wird einmalig für Ihren Apple-Chip übersetzt; danach startet es sofort.", "es": "Su juego es un programa de Windows para procesadores Intel. Se traduce una sola vez para su chip de Apple; después se abre directamente.", "it": "Il Suo gioco è un programma Windows per processori Intel. Viene tradotto una sola volta per il Suo chip Apple; poi si apre subito.", "pt": "Seu jogo é um programa do Windows para processadores Intel. Ele é traduzido uma única vez para o seu chip Apple; depois, abre direto.", "pl": "Gra jest programem dla Windows na procesory Intel. Zostanie jednorazowo przetłumaczona na układ Apple tego Maca; potem uruchamia się od razu.", "ru": "Ваша игра — программа для Windows на процессорах Intel. Она переводится один раз для вашего чипа Apple; после этого запускается сразу.", "ja": "このゲームはIntelプロセッサ向けのWindowsプログラムです。お使いのAppleチップ向けに一度だけ変換すれば、次回からすぐに起動します。"],
    "s2": ["fr": "Campagne", "en": "Campaign", "de": "Kampagne", "es": "Campaña", "it": "Campagna", "pt": "Campanha", "pl": "Kampania", "ru": "Кампания", "ja": "キャンペーン"],
    "s2d": ["fr": "Traduction, optimisation et assemblage de ce mode", "en": "Translating, optimizing and assembling this mode", "de": "Dieser Modus wird übersetzt, optimiert und zusammengesetzt", "es": "Traduciendo, optimizando y ensamblando este modo", "it": "Traduzione, ottimizzazione e assemblaggio di questa modalità", "pt": "Traduzindo, otimizando e montando este modo", "pl": "Tłumaczenie, optymalizacja i składanie tego trybu", "ru": "Перевод, оптимизация и сборка этого режима", "ja": "このモードを翻訳、最適化、組み立て中"],
    "s3": ["fr": "Zombies", "en": "Zombies", "de": "Zombies", "es": "Zombis", "it": "Zombi", "pt": "Zumbis", "pl": "Zombie", "ru": "Зомби", "ja": "ゾンビ"],
    "s3d": ["fr": "Traduction, optimisation et assemblage de ce mode", "en": "Translating, optimizing and assembling this mode", "de": "Dieser Modus wird übersetzt, optimiert und zusammengesetzt", "es": "Traduciendo, optimizando y ensamblando este modo", "it": "Traduzione, ottimizzazione e assemblaggio di questa modalità", "pt": "Traduzindo, otimizando e montando este modo", "pl": "Tłumaczenie, optymalizacja i składanie tego trybu", "ru": "Перевод, оптимизация и сборка этого режима", "ja": "このモードを翻訳、最適化、組み立て中"],
    "s4": ["fr": "Multijoueur", "en": "Multiplayer", "de": "Mehrspieler", "es": "Multijugador", "it": "Multigiocatore", "pt": "Multijogador", "pl": "Tryb wieloosobowy", "ru": "Мультиплеер", "ja": "マルチプレイヤー"],
    "s4d": ["fr": "Traduction, optimisation et assemblage de ce mode", "en": "Translating, optimizing and assembling this mode", "de": "Dieser Modus wird übersetzt, optimiert und zusammengesetzt", "es": "Traduciendo, optimizando y ensamblando este modo", "it": "Traduzione, ottimizzazione e assemblaggio di questa modalità", "pt": "Traduzindo, otimizando e montando este modo", "pl": "Tłumaczenie, optymalizacja i składanie tego trybu", "ru": "Перевод, оптимизация и сборка этого режима", "ja": "このモードを翻訳、最適化、組み立て中"],
    "e1": ["fr": "Outils de traduction", "en": "Translation tools", "de": "Übersetzungswerkzeuge", "es": "Herramientas de traducción", "it": "Strumenti di traduzione", "pt": "Ferramentas de tradução", "pl": "Narzędzia tłumaczenia", "ru": "Инструменты перевода", "ja": "変換ツール"],
    "e1d": ["fr": "Compilation de Remill, le traducteur x86 → ARM, sur votre Mac", "en": "Remill, the x86 → ARM translator, is being built on your Mac", "de": "Remill, der x86-→-ARM-Übersetzer, wird auf Ihrem Mac gebaut", "es": "Compilando Remill, el traductor x86 → ARM, en su Mac", "it": "Compilazione di Remill, il traduttore x86 → ARM, sul Suo Mac", "pt": "Compilando o Remill, o tradutor x86 → ARM, no seu Mac", "pl": "Remill, tłumacz x86 → ARM, kompiluje się na tym Macu", "ru": "Remill, транслятор x86 → ARM, собирается на вашем Mac", "ja": "x86 → ARM 変換ツール Remill をこのMacでビルド中"],
    "batterie": ["fr": "Branchez votre Mac : sur batterie, la préparation est beaucoup plus lente.", "en": "Plug in your Mac: on battery, preparation is much slower.", "de": "Schließen Sie Ihren Mac ans Netzteil an: Im Akkubetrieb dauert die Vorbereitung viel länger.", "es": "Conecte su Mac a la corriente: con batería, la preparación es mucho más lenta.", "it": "Colleghi il Mac all'alimentazione: a batteria la preparazione è molto più lenta.", "pt": "Conecte seu Mac à energia: na bateria, a preparação é muito mais lenta.", "pl": "Zalecane jest podłączenie Maca do zasilania: na baterii przygotowanie trwa znacznie dłużej.", "ru": "Подключите Mac к сети: от батареи подготовка идёт намного медленнее.", "ja": "Macを電源に接続してください。バッテリー駆動では準備に大幅に時間がかかります。"],
    "fermer": ["fr": "Vous pouvez fermer cette fenêtre : la préparation continue, et le Mac ne se met pas en veille. Rouvrez Black Ops II pour la suivre.", "en": "You can close this window: preparation keeps going and your Mac won't sleep. Reopen Black Ops II to check on it.", "de": "Sie können dieses Fenster schließen: Die Vorbereitung läuft weiter und der Mac geht nicht in den Ruhezustand. Öffnen Sie Black Ops II erneut, um den Fortschritt zu sehen.", "es": "Puede cerrar esta ventana: la preparación continúa y el Mac no entrará en reposo. Vuelva a abrir Black Ops II para seguirla.", "it": "Può chiudere questa finestra: la preparazione continua e il Mac non va in stop. Riapra Black Ops II per seguirla.", "pt": "Você pode fechar esta janela: a preparação continua e o Mac não entra em repouso. Abra o Black Ops II de novo para acompanhar.", "pl": "To okno można zamknąć: przygotowanie trwa dalej, a Mac nie przejdzie w uśpienie. Postęp widać po ponownym otwarciu Black Ops II.", "ru": "Это окно можно закрыть: подготовка продолжится, а Mac не уйдёт в сон. Откройте Black Ops II снова, чтобы следить за ней.", "ja": "このウインドウは閉じても大丈夫です。準備は続き、Macはスリープしません。進み具合はBlack Ops IIを再度開くと確認できます。"],
    "fini": ["fr": "La traduction est faite", "en": "Translation complete", "de": "Übersetzung abgeschlossen", "es": "Traducción completada", "it": "Traduzione completata", "pt": "Tradução concluída", "pl": "Tłumaczenie zakończone", "ru": "Перевод завершён", "ja": "変換が完了しました"],
    "fini_texte": ["fr": "Les prochaines fois, Black Ops II s'ouvrira directement.", "en": "From now on, Black Ops II opens straight away.", "de": "Ab jetzt startet Black Ops II sofort.", "es": "A partir de ahora, Black Ops II se abrirá directamente.", "it": "D'ora in poi Black Ops II si aprirà subito.", "pt": "A partir de agora, o Black Ops II abre direto.", "pl": "Od teraz Black Ops II uruchamia się od razu.", "ru": "Теперь Black Ops II будет запускаться сразу.", "ja": "次回からBlack Ops IIはすぐに起動します。"],
    "jouer_campagne": ["fr": "Jouer à la campagne", "en": "Play the campaign", "de": "Kampagne spielen", "es": "Jugar la campaña", "it": "Gioca la campagna", "pt": "Jogar a campanha", "pl": "Zagraj w kampanię", "ru": "Играть в кампанию", "ja": "キャンペーンをプレイ"],
    "jouer_campagne_texte": ["fr": "La campagne est prête. Jouez-y maintenant : les autres modes seront convertis quand vous les choisirez dans le menu du jeu.", "en": "The campaign is ready. Play it now: the other modes get converted when you pick them in the game's menu.", "de": "Die Kampagne ist bereit. Spielen Sie sie jetzt: Die anderen Modi werden umgewandelt, wenn Sie sie im Spielmenü auswählen.", "es": "La campaña está lista. Juégala ya: los demás modos se convertirán cuando los elijas en el menú del juego.", "it": "La campagna è pronta. Giocala subito: le altre modalità verranno convertite quando le sceglierai nel menu del gioco.", "pt": "A campanha está pronta. Jogue agora: os outros modos serão convertidos quando você escolhê-los no menu do jogo.", "pl": "Kampania jest gotowa. Można w nią zagrać od razu: pozostałe tryby zostaną przekonwertowane po wybraniu ich w menu gry.", "ru": "Кампания готова. Играйте сейчас: остальные режимы будут преобразованы, когда вы выберете их в меню игры.", "ja": "キャンペーンの準備ができました。今すぐプレイできます。ほかのモードは、ゲームのメニューで選んだときに変換されます。"],
    "jouer": ["fr": "Jouer", "en": "Play", "de": "Spielen", "es": "Jugar", "it": "Gioca", "pt": "Jogar", "pl": "Graj", "ru": "Играть", "ja": "プレイ"],
    "echec": ["fr": "La préparation s'est arrêtée", "en": "Preparation stopped", "de": "Die Vorbereitung wurde angehalten", "es": "La preparación se detuvo", "it": "La preparazione si è interrotta", "pt": "A preparação parou", "pl": "Przygotowanie zostało przerwane", "ru": "Подготовка остановилась", "ja": "準備が停止しました"],
    "echec_garde": ["fr": "Ce qui est déjà fait est gardé : « Réessayer » reprend où elle s'est arrêtée.", "en": "Nothing done so far is lost: “Try Again” picks up where it stopped.", "de": "Bisherige Arbeit bleibt erhalten: „Erneut versuchen“ macht dort weiter, wo es aufgehört hat.", "es": "No se pierde nada de lo hecho: «Reintentar» continúa donde se detuvo.", "it": "Il lavoro già fatto non va perso: «Riprova» riprende da dove si era fermata.", "pt": "Nada do que já foi feito se perde: “Tentar novamente” continua de onde parou.", "pl": "Nic z dotychczasowej pracy nie przepada: „Spróbuj ponownie” wznawia od miejsca przerwania.", "ru": "Уже сделанное сохраняется: «Повторить» продолжит с места остановки.", "ja": "ここまでの処理は保持されます。「再試行」で中断したところから再開します。"],
    "echec_muet": ["fr": "La préparation s'est arrêtée sans dire pourquoi.", "en": "Preparation stopped without giving a reason.", "de": "Die Vorbereitung wurde ohne Angabe von Gründen beendet.", "es": "La preparación se detuvo sin indicar el motivo.", "it": "La preparazione si è interrotta senza indicare il motivo.", "pt": "A preparação parou sem informar o motivo.", "pl": "Przygotowanie zatrzymało się bez podania przyczyny.", "ru": "Подготовка остановилась без объяснения причины.", "ja": "理由が示されないまま準備が停止しました。"],
    "journal": ["fr": "Voir le journal", "en": "Show Log", "de": "Protokoll anzeigen", "es": "Ver registro", "it": "Mostra registro", "pt": "Ver registro", "pl": "Pokaż dziennik", "ru": "Показать журнал", "ja": "ログを表示"],
    "reessayer": ["fr": "Réessayer", "en": "Try Again", "de": "Erneut versuchen", "es": "Reintentar", "it": "Riprova", "pt": "Tentar novamente", "pl": "Spróbuj ponownie", "ru": "Повторить", "ja": "再試行"],
    "estimation": ["fr": "estimation en cours…", "en": "estimating…", "de": "wird geschätzt…", "es": "calculando…", "it": "stima in corso…", "pt": "calculando…", "pl": "szacowanie…", "ru": "оценка…", "ja": "残り時間を計算中…"],
    "moins_minute": ["fr": "moins d'une minute", "en": "less than a minute left", "de": "weniger als eine Minute", "es": "menos de un minuto", "it": "meno di un minuto", "pt": "menos de um minuto", "pl": "mniej niż minuta", "ru": "меньше минуты", "ja": "残り1分未満"],
    "min": ["fr": "environ %d min restantes", "en": "about %d min left", "de": "noch etwa %d Min.", "es": "unos %d min restantes", "it": "circa %d min rimanenti", "pt": "cerca de %d min restantes", "pl": "jeszcze ok. %d min", "ru": "осталось около %d мин", "ja": "残り約%d分"],
    "h": ["fr": "environ %d h %02d restantes", "en": "about %d h %02d min left", "de": "noch etwa %d Std. %02d Min.", "es": "unas %d h %02d min restantes", "it": "circa %d h %02d min rimanenti", "pt": "cerca de %d h %02d min restantes", "pl": "jeszcze ok. %d h %02d min", "ru": "осталось около %d ч %02d мин", "ja": "残り約%d時間%02d分"],
    "r_disque": ["fr": "Il faut au moins 8 Go libres sur le disque (il en reste %d). Libérez de la place, puis réessayez.", "en": "At least 8 GB of free disk space is needed (%d GB left). Free up some space, then try again.", "de": "Es werden mindestens 8 GB freier Speicher benötigt (noch %d GB frei). Bitte schaffen Sie Platz und versuchen Sie es erneut.", "es": "Se necesitan al menos 8 GB libres en el disco (quedan %d). Libere espacio y vuelva a intentarlo.", "it": "Servono almeno 8 GB liberi sul disco (ne restano %d). Liberi spazio e riprovi.", "pt": "São necessários pelo menos 8 GB livres no disco (restam %d). Libere espaço e tente de novo.", "pl": "Potrzeba co najmniej 8 GB wolnego miejsca na dysku (zostało %d). Należy zwolnić miejsce i spróbować ponownie.", "ru": "Нужно минимум 8 ГБ свободного места на диске (осталось %d). Освободите место и повторите.", "ja": "ディスクに8GB以上の空き容量が必要です（残り%dGB）。空き容量を増やしてから再試行してください。"],
    "r_outils": ["fr": "Il manque des outils de développement. Installez Homebrew (brew.sh), puis collez cette commande dans Terminal :", "en": "Some developer tools are missing. Install Homebrew (brew.sh), then paste this command into Terminal:", "de": "Es fehlen Entwicklerwerkzeuge. Installieren Sie Homebrew (brew.sh) und fügen Sie dann diesen Befehl im Terminal ein:", "es": "Faltan herramientas de desarrollo. Instale Homebrew (brew.sh) y pegue este comando en Terminal:", "it": "Mancano alcuni strumenti di sviluppo. Installi Homebrew (brew.sh), poi incolli questo comando nel Terminale:", "pt": "Faltam ferramentas de desenvolvimento. Instale o Homebrew (brew.sh) e cole este comando no Terminal:", "pl": "Brakuje narzędzi programistycznych. Należy zainstalować Homebrew (brew.sh), a potem wkleić to polecenie w Terminalu:", "ru": "Не хватает инструментов разработчика. Установите Homebrew (brew.sh), затем вставьте эту команду в Терминал:", "ja": "開発ツールが不足しています。Homebrew（brew.sh）をインストールしてから、次のコマンドをターミナルに貼り付けてください："],
    "r_enveloppe": ["fr": "C'est bien la version Steam à jour, mais elle a encore son enveloppe SteamStub : seule l'enveloppe doit être retirée (README, Installation).", "en": "This is the right, up-to-date Steam version, but it still has its SteamStub wrapper: only the wrapper must be removed (README, Install).", "de": "Das ist die richtige, aktuelle Steam-Version, aber sie hat noch ihre SteamStub-Hülle: Nur die Hülle muss entfernt werden (README, Installation).", "es": "Es la versión de Steam correcta y actualizada, pero aún tiene su envoltorio SteamStub: solo hay que quitar el envoltorio (README, instalación).", "it": "È la versione Steam giusta e aggiornata, ma ha ancora il suo involucro SteamStub: va rimosso solo l'involucro (README, installazione).", "pt": "É a versão da Steam certa e atualizada, mas ainda tem o invólucro SteamStub: só o invólucro precisa ser removido (README, instalação).", "pl": "To właściwa, aktualna wersja ze Steama, ale nadal ma opakowanie SteamStub: trzeba usunąć tylko opakowanie (README, instalacja).", "ru": "Это нужная, актуальная версия из Steam, но на ней ещё обёртка SteamStub: снять нужно только обёртку (README, установка).", "ja": "正しい最新のSteam版ですが、まだSteamStubのラッパーが付いています。ラッパーだけを外してください（README、インストール）。"],
    "r_exe": ["fr": "Ce t6sp.exe n'est pas la version que le portage sait traduire : il faut celui de Steam, à jour.", "en": "This t6sp.exe is not the version the port can translate: it needs the up-to-date Steam one.", "de": "Diese t6sp.exe ist nicht die Version, die der Port übersetzen kann: Benötigt wird die aktuelle Steam-Version.", "es": "Este t6sp.exe no es la versión que el port sabe traducir: hace falta el de Steam, actualizado.", "it": "Questo t6sp.exe non è la versione che il port sa tradurre: serve quello di Steam, aggiornato.", "pt": "Este t6sp.exe não é a versão que o port sabe traduzir: é preciso o da Steam, atualizado.", "pl": "Ten t6sp.exe to nie wersja, którą port potrafi przetłumaczyć: potrzebny jest aktualny plik ze Steama.", "ru": "Этот t6sp.exe — не та версия, которую умеет переводить порт: нужен актуальный файл из Steam.", "ja": "このt6sp.exeは移植版が変換できるバージョンではありません。最新のSteam版が必要です。"],
    "d_fichiers": ["fr": "Fichiers compilés : %@ / %@", "en": "Files compiled: %@ / %@", "de": "Kompilierte Dateien: %@ / %@", "es": "Archivos compilados: %@ / %@", "it": "File compilati: %@ / %@", "pt": "Arquivos compilados: %@ / %@", "pl": "Skompilowane pliki: %@ / %@", "ru": "Скомпилировано файлов: %@ / %@", "ja": "コンパイル済みファイル：%@ / %@"],
    "signaler": ["fr": "Signaler le problème", "en": "Report the problem", "de": "Problem melden", "es": "Informar del problema", "it": "Segnala il problema", "pt": "Relatar o problema", "pl": "Zgłoś problem", "ru": "Сообщить о проблеме", "ja": "問題を報告"],
    "copier": ["fr": "Copier", "en": "Copy", "de": "Kopieren", "es": "Copiar", "it": "Copia", "pt": "Copiar", "pl": "Kopiuj", "ru": "Копировать", "ja": "コピー"],
]

// The Mac's language (fr, en, de, es, it, pt, pl, ru, ja), English otherwise.
let langue: String = {
    let l = String((Locale.preferredLanguages.first ?? "en").prefix(2))
    return textes["jouer"]![l] != nil ? l : "en"
}()
func t(_ cle: String) -> String { textes[cle]?[langue] ?? textes[cle]?["en"] ?? cle }
final class Modele: ObservableObject {
    @Published var pct = 0.0
    @Published var echec: String?
    @Published var commande: String?   // to paste into Terminal (missing tools)
    @Published var fini = false
    @Published var campagnePrete = false   // Steam: the campaign is linked, playable before the other two modes
    @Published var surBatterie = false
    @Published var detail: String?     // what the running step is doing right now, read from its own log
    @Published var restant = t("estimation")
    private var entree: [Int: Date] = [:]   // when each step was seen starting (window open)
    private var facteur = 1.0               // actual / nominal speed, learnt on steps seen from start to end
    private var brut = -1.0                 // last percentage build_steam.sh printed
    private var depuis = Date()             // when it printed it (the log's modification time)

    func etape(_ p: Double) -> Int { etapes.lastIndex(where: { p >= $0.debut && p < $0.fin }) ?? (p >= 100 ? etapes.count : 0) }
    var indice: Int { etape(pct) }

    func lire() {
        let lignes = ((try? String(contentsOfFile: journal, encoding: .utf8)) ?? "").split(separator: "\n")
        if let l = lignes.last(where: { $0.hasPrefix("[prepare] ") }) {
            let reste = l.dropFirst("[prepare] ".count)
            if reste.hasPrefix("❌") {
                expliquer(String(reste.dropFirst().trimmingCharacters(in: .whitespaces)))
            } else if let n = Double(reste.prefix(while: { $0.isNumber })) {
                if n != brut {
                    brut = n
                    depuis = (try? FileManager.default.attributesOfItem(atPath: journal)[.modificationDate] as? Date) ?? Date()
                }
                let avant = indice
                pct = avancer()
                if indice != avant {
                    if let d = entree[avant], avant < etapes.count {
                        let reel = Date().timeIntervalSince(d) / 60
                        if reel > 1 { facteur = 0.5 * facteur + 0.5 * reel / etapes[avant].minutes }
                    }
                    entree[indice] = Date()
                }
            }
        }
        campagnePrete = FileManager.default.fileExists(atPath: dossier + "/boot_cache/sps/.fini")
        surBatterie = IOPSGetProvidingPowerSourceType(nil)?.takeUnretainedValue() as String? == kIOPSBatteryPowerValue
        detail = detailler(lignes)
        estimer()
        NSApp.dockTile.badgeLabel = fini || echec != nil ? nil : "\(Int(pct)) %"
        if kill(pid, 0) != 0 && echec == nil && !fini {
            if FileManager.default.isExecutableFile(atPath: dossier + "/boot_cache/\(cible)/boot") {
                fini = true
                NSSound(named: "Glass")?.play()
                NSApp.requestUserAttention(.criticalRequest)
            } else {   // died without a ❌ line (build_steam.sh refusal, crash): show its last words
                echec = t("echec_muet") + (lignes.last.map { "\n\n" + $0 } ?? "")
            }
        }
    }

    // build_steam.sh refusals are in English: the common ones are translated, the others shown as they are.
    private func expliquer(_ brut: String) {
        if brut.hasPrefix("less than 8 GB free"),
           let n = Int(brut.drop(while: { $0 != "(" }).dropFirst().prefix(while: { $0.isNumber })) {
            echec = String(format: t("r_disque"), n)
        } else if brut.hasPrefix("missing: brew") {
            echec = t("r_outils"); commande = "brew install llvm@17 cmake ninja libvpx unicorn"
        } else if brut.hasPrefix("missing: pip3") {
            echec = t("r_outils"); commande = "pip3 install capstone"
        } else if brut.contains("still has its SteamStub wrapper") {
            echec = t("r_enveloppe")
        } else if brut.contains("is not the Steam build this port translates") {
            echec = t("r_exe")
        } else { echec = brut }
    }

    // Each step's tool counts its own progress in its log (build_steam.sh sends them to boot_cache/): show that count.
    private func detailler(_ lignes: [Substring]) -> String? {
        indice == 0 ? compte(queue(dossier + "/boot_cache/remill.log"), #"\[(\d+)/(\d+)\]"#, "d_fichiers") : nil
    }

    private func queue(_ f: String) -> String {   // the last 16 KB: some of these logs grow to megabytes
        guard let h = FileHandle(forReadingAtPath: f) else { return "" }
        defer { try? h.close() }
        let n = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: n > 16384 ? n - 16384 : 0)
        return String(decoding: h.readDataToEndOfFile(), as: UTF8.self)
    }

    private func compte(_ texte: String, _ motif: String, _ cle: String) -> String? {   // last "a/b" → localised "…: a / b"
        guard let m = try? NSRegularExpression(pattern: motif).matches(in: texte, range: NSRange(texte.startIndex..., in: texte)).last,
              let a = Range(m.range(at: 1), in: texte).flatMap({ Int(texte[$0]) }),
              let b = Range(m.range(at: 2), in: texte).flatMap({ Int(texte[$0]) }) else { return nil }
        return String(format: t(cle), a.formatted(), b.formatted())
    }

    // The script prints one line per step: in between, move on the clock (nominal minutes × learnt speed), never past
    // 95 % of what is left of the step, so the bar keeps moving without lying about the end.
    private func avancer() -> Double {
        let i = etape(brut)
        guard i < etapes.count else { return brut }
        let e = etapes[i]
        let minutes = e.minutes * (e.fin - brut) / (e.fin - e.debut) * facteur
        return brut + (e.fin - brut) * min(0.95, Date().timeIntervalSince(depuis) / 60 / max(minutes, 0.1))
    }

    private func estimer() {
        let i = indice
        guard i < etapes.count else { restant = ""; return }
        let e = etapes[i]
        let fait = (pct - e.debut) / (e.fin - e.debut)
        let min = (e.minutes * (1 - fait) + etapes[(i + 1)...].reduce(0) { $0 + $1.minutes }) * facteur
        restant = min < 1 ? t("moins_minute") : min < 60 ? String(format: t("min"), Int(min.rounded()))
            : String(format: t("h"), Int(min) / 60, Int(min) % 60)
    }
}

// --- Look: dark, full-bleed, the game's orange; Liquid Glass panels on macOS 26+, blur material before that.
let orange = Color(red: 1.0, green: 0.47, blue: 0.12)
let braise = Color(red: 0.55, green: 0.10, blue: 0.04)

extension View {
    @ViewBuilder func verre(_ rayon: CGFloat = 24) -> some View {
        if #available(macOS 26, *) {
            glassEffect(.regular, in: RoundedRectangle(cornerRadius: rayon, style: .continuous))
        } else {
            background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: rayon, style: .continuous))
        }
    }
}

// Two slow glows drifting on black. Radial gradients, not blurs: cheap while the Mac is busy translating.
struct Fond: View {
    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 20)) { tl in
            let s = tl.date.timeIntervalSinceReferenceDate
            ZStack {
                Color.black
                RadialGradient(colors: [orange.opacity(0.45), .clear], center: .center, startRadius: 0, endRadius: 340)
                    .frame(width: 680, height: 680).offset(x: 230 + 50 * cos(s / 7), y: -170 + 35 * sin(s / 5))
                RadialGradient(colors: [braise.opacity(0.7), .clear], center: .center, startRadius: 0, endRadius: 320)
                    .frame(width: 640, height: 640).offset(x: -260 + 60 * sin(s / 9), y: 220 + 30 * cos(s / 6))
                LinearGradient(colors: [.clear, .black.opacity(0.6)], startPoint: .top, endPoint: .bottom)
            }
        }
        .ignoresSafeArea()
    }
}

// Netflix-style primary button: white, bold, black text.
struct BoutonBlanc: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .bold)).foregroundStyle(.black)
            .padding(.horizontal, 26).padding(.vertical, 11)
            .background(.white.opacity(configuration.isPressed ? 0.75 : 1), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
    }
}

// Secondary button: a glass capsule.
struct BoutonVerre: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 14, weight: .semibold)).foregroundStyle(.white)
            .padding(.horizontal, 18).padding(.vertical, 9)
            .verre(12).opacity(configuration.isPressed ? 0.7 : 1)
    }
}

// One segment per step, filled with the step's share of the percentage. All the filled length is one gradient,
// masked by the segments; a soft wave runs along the running step, so it visibly moves even while the step
// prints nothing for half an hour.
struct Segments: View {
    let pct: Double; let indice: Int
    let ecart: CGFloat = 8
    func part(_ i: Int) -> Double { min(max((pct - etapes[i].debut) / (etapes[i].fin - etapes[i].debut), 0), 1) }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack {
                HStack(spacing: ecart) { ForEach(etapes.indices, id: \.self) { _ in Capsule().fill(.white.opacity(0.12)) } }
                GeometryReader { g in
                    let l = (g.size.width - ecart * CGFloat(etapes.count - 1)) / CGFloat(etapes.count)
                    LinearGradient(colors: [orange, Color(red: 1, green: 0.7, blue: 0.3)], startPoint: .leading, endPoint: .trailing)
                        .mask(alignment: .leading) {
                            HStack(spacing: ecart) {
                                ForEach(etapes.indices, id: \.self) { i in
                                    Capsule().frame(width: l * part(i)).frame(width: l, alignment: .leading)
                                }
                            }
                        }
                        .shadow(color: orange.opacity(0.6), radius: 5)
                    if indice < etapes.count {   // a soft wave runs along the running step only
                        let large = l * part(indice)
                        TimelineView(.animation(minimumInterval: 1.0 / 30)) { tl in
                            let x = tl.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.2) / 2.2
                            LinearGradient(colors: [.clear, .white.opacity(0.4), .clear], startPoint: .leading, endPoint: .trailing)
                                .frame(width: 70).offset(x: -70 + CGFloat(x) * (large + 70))
                        }
                        .frame(width: large, alignment: .leading).clipShape(Capsule())
                        .offset(x: CGFloat(indice) * (l + ecart))
                    }
                }
            }
            .frame(height: 5)
            HStack(alignment: .top, spacing: ecart) {
                ForEach(etapes.indices, id: \.self) { i in
                    Text(t(etapes[i].cle)).font(.system(size: 11, weight: i == indice ? .semibold : .regular))
                        .foregroundStyle(.white.opacity(i < indice ? 0.55 : i == indice ? 1 : 0.35))
                        .lineLimit(1).minimumScaleFactor(0.7).frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
        .animation(.easeOut(duration: 0.6), value: pct)
    }
}

// A pre-filled GitHub issue (opened in the browser, the player reads it before sending): reason, Mac, end of the log.
// The home folder is replaced by ~ so the user name does not leave the Mac.
func ticket(_ raison: String) -> URL {
    var b = [CChar](repeating: 0, count: 128); var n = b.count
    sysctlbyname("machdep.cpu.brand_string", &b, &n, nil, 0)
    let fin = ((try? String(contentsOfFile: journal, encoding: .utf8)) ?? "").split(separator: "\n").suffix(40)
        .joined(separator: "\n").replacingOccurrences(of: NSHomeDirectory(), with: "~")
    let corps = """
        **What happened:** \(raison)

        **Mac:** \(String(cString: b)), \(ProcessInfo.processInfo.operatingSystemVersionString)

        **Last lines of preparation.log:**
        ```
        \(fin)
        ```
        """
    var u = URLComponents(string: "https://github.com/adrbn/bo2-mac/issues/new")!
    u.queryItems = [URLQueryItem(name: "title", value: "Preparation failed: " + raison.prefix(80)), URLQueryItem(name: "body", value: corps)]
    return u.url!
}

struct Vue: View {
    @ObservedObject var m: Modele
    var body: some View {
        VStack(alignment: .leading, spacing: 30) {
            entete
            Group {
                if let raison = m.echec { panneauEchec(raison) } else if m.fini { panneauFini } else { panneauEnCours }
            }
            .transition(.opacity.combined(with: .scale(scale: 0.98)))
        }
        .padding(.horizontal, 44).padding(.top, 52).padding(.bottom, 36)
        .frame(width: 720, alignment: .leading)
        .background { Fond() }   // behind, so it never sizes the window
        .animation(.spring(duration: 0.5), value: m.fini)
        .animation(.spring(duration: 0.5), value: m.echec)
        .foregroundStyle(.white)
        .environment(\.colorScheme, .dark)
    }

    var entete: some View {
        HStack(spacing: 20) {
            Image(nsImage: NSApp.applicationIconImage).resizable().interpolation(.high)
                .frame(width: 76, height: 76)
                .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                .shadow(color: orange.opacity(0.55), radius: 22)
            VStack(alignment: .leading, spacing: 4) {
                Text("Black Ops II").font(.system(size: 30, weight: .bold))
                Text(t(m.fini ? "pret" : "sous_titre")).font(.system(size: 15)).foregroundStyle(.white.opacity(0.6))
            }
        }
    }

    var panneauEnCours: some View {
        VStack(alignment: .leading, spacing: 22) {
            Text(t("intro")).font(.system(size: 15)).foregroundStyle(.white.opacity(0.75))
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 24) {
                HStack(alignment: .firstTextBaseline) {
                    HStack(alignment: .firstTextBaseline, spacing: 4) {
                        Text("\(Int(m.pct))").font(.system(size: 72, weight: .thin, design: .rounded))
                            .monospacedDigit().contentTransition(.numericText())
                            .animation(.easeOut, value: Int(m.pct))
                        Text("%").font(.system(size: 26, weight: .light, design: .rounded))
                    }
                    Spacer()
                    if m.indice < etapes.count {
                        VStack(alignment: .trailing, spacing: 4) {
                            Text(t(etapes[m.indice].cle)).font(.system(size: 18, weight: .semibold))
                            Text(t(etapes[m.indice].cle + "d")).font(.system(size: 13)).foregroundStyle(.white.opacity(0.6))
                                .multilineTextAlignment(.trailing)
                            if let d = m.detail {
                                Text(d).font(.system(size: 13, weight: .medium)).monospacedDigit().foregroundStyle(orange)
                                    .contentTransition(.numericText()).animation(.easeOut, value: d)
                            }
                        }
                    }
                }
                Segments(pct: m.pct, indice: m.indice)
                HStack(spacing: 16) {
                    Label(m.restant, systemImage: "clock").foregroundStyle(.white.opacity(0.7))
                    Spacer()
                    if m.surBatterie {
                        Label(t("batterie"), systemImage: "powerplug.fill").foregroundStyle(orange)
                            .multilineTextAlignment(.trailing)
                    }
                }.font(.system(size: 13, weight: .medium))
            }
            .padding(26).verre()
            if m.campagnePrete {   // 4 = stop the conversion and play the campaign (make_app.sh); it resumes at the next launch
                HStack(spacing: 16) {
                    Text(t("jouer_campagne_texte")).font(.system(size: 13)).foregroundStyle(.white.opacity(0.7))
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer()
                    Button { exit(4) } label: { Label(t("jouer_campagne"), systemImage: "play.fill") }.buttonStyle(BoutonBlanc())
                }
            }
            Text(t("fermer")).font(.system(size: 12)).foregroundStyle(.white.opacity(0.45))
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    var panneauFini: some View {
        VStack(alignment: .leading, spacing: 26) {
            HStack(spacing: 14) {
                Image(systemName: "checkmark.circle.fill").font(.system(size: 34)).foregroundStyle(orange)
                VStack(alignment: .leading, spacing: 4) {
                    Text(t("fini")).font(.system(size: 24, weight: .bold))
                    Text(t("fini_texte")).font(.system(size: 15)).foregroundStyle(.white.opacity(0.7))
                }
            }
            Button { exit(0) } label: { Label(t("jouer"), systemImage: "play.fill") }
                .buttonStyle(BoutonBlanc()).keyboardShortcut(.defaultAction)
        }
        .padding(.top, 10)
    }

    func panneauEchec(_ raison: String) -> some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 14) {
                Label(t("echec"), systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: 20, weight: .bold)).foregroundStyle(Color(red: 1, green: 0.42, blue: 0.38))
                Text(raison).font(.system(size: 15)).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                if let c = m.commande {
                    HStack(spacing: 12) {
                        Text(c).font(.system(size: 13, design: .monospaced)).textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer()
                        Button {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(c, forType: .string)
                        } label: { Label(t("copier"), systemImage: "doc.on.doc") }
                            .buttonStyle(BoutonVerre())
                    }
                    .padding(12).background(.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
                Text(t("echec_garde")).font(.system(size: 13)).foregroundStyle(.white.opacity(0.6))
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(26).frame(maxWidth: .infinity, alignment: .leading).verre()
            HStack {
                Button(t("journal")) { NSWorkspace.shared.open(URL(fileURLWithPath: journal)) }.buttonStyle(BoutonVerre())
                Button { NSWorkspace.shared.open(ticket(raison)) } label: { Label(t("signaler"), systemImage: "ladybug") }
                    .buttonStyle(BoutonVerre())
                Spacer()
                Button { exit(3) } label: { Label(t("reessayer"), systemImage: "arrow.clockwise") }
                    .buttonStyle(BoutonBlanc()).keyboardShortcut(.defaultAction)
            }
        }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
if let icone = NSImage(contentsOfFile: dossier + "/icon.png") { app.applicationIconImage = icone }
let modele = Modele()
let win = NSWindow(contentRect: .zero, styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: false)
win.titlebarAppearsTransparent = true
win.titleVisibility = .hidden
win.isMovableByWindowBackground = true
win.appearance = NSAppearance(named: .darkAqua)
win.backgroundColor = .black
win.contentView = NSHostingView(rootView: Vue(m: modele))
win.setContentSize(win.contentView!.fittingSize)
win.center()
win.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
modele.lire()
Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
    modele.lire()
    let taille = win.contentView!.fittingSize   // the panel changes height (running / done / failed)
    if taille != win.contentLayoutRect.size { win.setContentSize(taille) }
}
NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: win, queue: nil) { _ in exit(1) }
app.run()
