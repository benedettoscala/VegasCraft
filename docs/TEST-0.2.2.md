# Prova in gioco — 0.2.2

Checklist aggiornata il **3 ottobre 2026** per **0.2.2**, protocollo **20**. Le prove elencate sono da eseguire: non costituiscono una conferma del funzionamento in gioco. I risultati automatici e le verifiche precedenti sono in [VALIDATION.md](VALIDATION.md).

## Preparazione

1. Conserva un salvataggio di prova e una copia del mondo Minecraft. I due salvataggi sono separati: un quickload di New Vegas non annulla blocchi, scavi o oggetti Minecraft.
2. Chiudi entrambi i giochi e installa `dist/VegasCraft-0.2.2.zip` con MO2/Vortex, oppure estrailo nella cartella **Data**. Devono esserci `NVSE/Plugins/VegasCraft.dll`, `NVSE/Plugins/VegasCraft.ini`, `NVSE/Plugins/VegasCraft/VegasCraftCore.dll` e il bundle Minecraft nella stessa sottocartella del core.
3. Per un launcher personale installa `dist/vegascraft-fabric-0.2.2.jar`, Minecraft 26.3, Fabric Loader 0.19.5+, Fabric API e Java 25 a 64 bit. Aggiungi `--enable-native-access=ALL-UNNAMED`. DLL e jar devono provenire dallo stesso build.
4. Avvia New Vegas tramite xNVSE, da MO2 se lo usi. Completa l'accesso Microsoft e i download Prism al primo avvio. Carica un esterno, per esempio Goodsprings, e attendi il mondo mirror.
5. Controlla `VegasCraft.log`: `plugin initialization complete`, `core ready`, collegamento Minecraft e controllo del giocatore (`MC alive=1`, `inWorld=1`, `puppet=1`). Non avviare un secondo FalloutNV sullo stesso bridge.

Configurazioni e percorsi sono in [CONFIGURATION.md](CONFIGURATION.md). Per l'aggiornamento normale riavvia entrambi i giochi; il solo core può essere sostituito durante lo sviluppo seguendo [DEVELOPMENT.md](DEVELOPMENT.md).

## Movimento, menu e interazioni

- WASD, mouse e Spazio: cammina, guarda, salta. Con i binding predefiniti adattati, **Shift corre** e **Ctrl si abbassa**. Prova pendenze, scale e bordi senza teletrasporti ripetuti.
- **E** apre l'inventario Minecraft. Chiudilo e usa **G** per porte, dialoghi e contenitori nativi; **H** per attendere. Prova un interno e il ritorno all'esterno.
- **Esc** apre la pausa nativa durante il gameplay o chiude una schermata Minecraft. **O** apre le opzioni Minecraft; **T** apre la chat. Prova anche caratteri accentati e il layout di tastiera in uso.
- Siediti su una sedia o usa un letto nativo. Controlla che New Vegas gestisca l'animazione e che il controllo Minecraft ritorni dopo esserti alzato/svegliato.
- Premi **F5** per prima persona, terza dietro e terza davanti. Controlla il ritorno in prima persona, la visuale vicino ai muri, il cielo e la comparsa dell'avatar.
- Durante il controllo Minecraft, salute/AP/mirino nativi devono sparire; bussola, messaggi e salute nemici restano. Apri un menu nativo e controlla il ripristino.

## Pip-Boy e inventario

Apri con **Tab**. Attendi che il modello e la pagina si stabilizzino; controlla proporzioni, leggibilità, posizione del cursore e precisione dei clic, anche ai bordi e con diverse risoluzioni/scale GUI. Chiudi e riapri più volte.

Regressione 0.2.2: verifica che ITEMS e STATS ricevano i dati anche quando un oggetto non ha un'icona leggibile. Con una missione attiva, confronta nome, obiettivi e tracciamento in DATA con il Pip-Boy nativo. Distingui «Waiting for New Vegas…» (nessun pacchetto ricevuto) da una lista vuota (pacchetto ricevuto senza missioni attive). Controlla che nel log non compaiano `inventory: faulted` o `pipdata: faulted` durante queste prove.

| Sezione / tasto | Verifica |
| --- | --- |
| **F1 — STATS** | Status, S.P.E.C.I.A.L., Skills, Perks, General: confronta valori e descrizioni con lo schermo nativo; controlla salute arti, radiazioni, peso, XP e dati Hardcore quando applicabili |
| **F2 — ITEMS** | Weapons, Apparel, Aid, Misc, Ammo: elenco completo, icone, quantità, valore/peso/condizione e scorrimento |
| **F3 — DATA** | Quests e obiettivi, selezione/tracciamento, Misc e testo note, Radio (clic su una stazione in portata per sintonizzarla, «Turn off» per spegnerla), World Map e marcatori |
| **F4 in ITEMS** | Inventario Minecraft e crafting 3x3; ritorno alla lista nativa via F4 o pulsante |
| **NV in STATS/DATA** | Passaggio alla sezione nativa; usa questa per la Local Map e per i contenuti delle note non disponibili nella pagina Minecraft |

Prove di inventario:

1. Confronta oggetti/quantità con New Vegas, equipaggia/togli un abito, usa un consumabile e lascia un oggetto non di missione. Controlla l'effetto nel gioco nativo e l'aggiornamento della lista.
2. Assegna hotkey native 1-8. I link devono apparire solo negli slot Minecraft liberi corrispondenti; un oggetto Minecraft già presente non deve essere espulso.
3. Nella pagina ITEMS prendi un link con il cursore e posalo nella hotbar. Prova clic tenuti e rilascio; controlla che un oggetto Minecraft spostato trovi posto e che hotkey/quantità restino coerenti.
4. Seleziona i link con 1-9/rotella; usa un Aid con clic destro. Getta un link e controlla il drop nativo. Verifica che un link non entri nei contenitori/crafting Minecraft.
5. Crea un oggetto nel crafting 3x3 del Pip-Boy, cambia sezione e chiudi: materiali e contenuto della griglia non devono andare persi.

Nella mappa mondiale controlla immagine, posizione/direzione del giocatore, marcatori, rotella per zoom e trascinamento per spostare la vista. Seleziona una destinazione idonea e usa **Travel** su un salvataggio di prova: controlla il viaggio nativo e la risincronizzazione. La radio Minecraft mostra informazioni; la sintonizzazione richiede **NV**. La mappa locale non ha ancora una resa Minecraft.

## Armi, combattimento e braccia

- Con un'arma da fuoco nativa: clic sinistro spara, destro mira, **R** ricarica. Controlla animazioni, munizioni, cambio slot e ritorno a un blocco Minecraft.
- Prova colpi su un bersaglio nativo e un mob Minecraft; verifica danno/salute e l'effetto sui blocchi. Ripeti senza un'arma nativa per il combattimento Minecraft.
- Prova un'arma melee nativa: modello, swing e feedback Minecraft. Ripeti con F5 e controlla arma/avatar in terza persona.
- Le braccia Minecraft in prima persona con un'arma da fuoco nativa sono **nascoste per impostazione predefinita**. Per la prova opzionale scrivi `showArms 1` in `config/vegascraft_arms.txt`: controlla dita dietro l'arma, mira/FOV, reload e armi con mesh animate. Riporta poi `showArms 0`.
- Confronta fluidità a mani vuote, con arma, Pip-Boy chiuso e aperto. Annota risoluzione, distanza di rendering e un'eventuale regressione misurabile.

## Blocchi, terreno e luci

1. Piazza un blocco, salici sopra e rompilo. Controlla profondità rispetto alla geometria nativa, collisioni, crepe, oggetto lasciato e ripristino del movimento.
2. Piazza torce, glowstone, lanterne, lava o fuoco in un'area buia. Confronta terreno, rocce, oggetti e NPC. Rimuovi gli emettitori e controlla che non rimangano luci; ripeti dopo cambio cella/quickload.
3. Con **Terrain destruction: On** nel menu O, mina solo terreno esterno naturale. Controlla buco, pareti, bordi tra celle e continuità delle texture. Scendi nel buco e piazza blocchi all'interno; prova uno scavo profondo.
4. Osserva un NPC sul terreno scavato e accanto ai blocchi piazzati. Il filtro dei contatti nativi è presente, ma gli NPC **non hanno supporto completo sui pavimenti Minecraft**; annota cadute e spostamenti laterali.
5. Disattiva la distruzione: nuovi scavi/esplosioni non devono aprire altri buchi, mentre quelli già presenti restano. Riattiva e prova anche un'esplosione in un punto sicuro del salvataggio di prova.
6. Ricarica New Vegas, cambia cella, riavvia entrambi i giochi e verifica il reinvio degli scavi dal mondo Minecraft. Non aspettarti il rollback degli scavi caricando un vecchio save nativo.
7. Controlla alberi, piante, strutture, animali e mostri sul terreno esportato. Prova `mobs=false`, attendendo almeno circa 10 secondi, e verifica la rimozione dei mob gestiti dallo spawner.

Limiti attuali: scavo di oggetti arbitrari e rimozione dell'erba incompleti; risposta del landscape alle luci da verificare; interni/worldspace non Mojave condividono la dimensione `elsewhere`. Non considerarli isolati tra loro.

## Sviluppo e multiplayer opzionali

Per lo sviluppo prova il deploy del core durante una sessione: cerca `core: reloaded`, verifica replay di blocchi/scavi, input, HUD e ripristino degli oggetti nativi. Cambi loader/ABI/protocollo o jar richiedono i riavvii indicati in [DEVELOPMENT.md](DEVELOPMENT.md).

Il multiplayer è sperimentale e non validato. L'host apre **O → Open to LAN**, e4mc fornisce l'indirizzo; l'ospite usa **T**, `/join <indirizzo>`, poi `/leave`. Ogni partecipante deve avere il proprio host New Vegas. Controlla connessione/disconnessione, blocchi, scavi decisi dall'host, danni e salvataggi separati. Per un `join` salvato nel file di configurazione, cancellalo e riavvia Minecraft per tornare al mondo locale.

## Registrazione del risultato

Indica revisione/build, launcher, mod native aggiuntive, configurazioni cambiate, cella/save di prova, prima azione che fallisce, risultato atteso e osservato. Conserva `VegasCraft.log`, `nvse.log` e Minecraft `logs/latest.log`; allega immagini/video per difetti visivi e segnala quali passi sono riusciti.

Al 3 ottobre: **5 test nativi passati**, inclusa la nuova prova dei dati Pip-Boy, build Fabric riuscito con **21 test eseguiti e passati**, test del bridge x86/x64 riuscito. Questi controlli non sostituiscono questa checklist nel gioco reale.
