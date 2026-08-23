# macbook_smc_config

Utility a riga di comando per leggere e scrivere le chiavi dell'Apple SMC
(System Management Controller) direttamente dalle porte I/O su MacBook con Linux.

## Descrizione

`smc-tool` e' un piccolo programma C che comunica con l'SMC degli Intel Mac
senza passare dal modulo del kernel `applesmc`: accede direttamente alle porte
I/O `0x300-0x31F`, usando lo stesso protocollo implementato dal driver
`applesmc` del kernel Linux.

Permette di:

- **leggere** il valore di una chiave SMC (`get`);
- **scrivere** un valore numerico 0-100 in una chiave SMC, con verifica
  automatica tramite rilettura (`set`).

Caso d'uso tipico: impostare la velocita' minima delle ventole (chiavi tipo
`F0Mn`) su MacBook dove i driver standard non bastano.

> **ATTENZIONE**: scrivere chiavi SMC errate puo' rendere il Mac instabile o,
> in casi estremi, non piu' avviabile. Usa questo strumento solo se sai
> esattamente cosa stai facendo e sempre a tuo rischio.

## Requisiti

- Linux su Mac Intel (il codice usa `ioperm()`, `inb()`, `outb()`: solo x86)
- Privilegi di root (accesso diretto alle porte I/O)
- GCC o clang; nessuna dipendenza oltre alla libc standard

## Compilazione

```sh
gcc -O2 -Wall -Wextra -o smc src/smc-tool.c
```

## Uso

```sh
sudo ./smc get F0Mn      # legge la chiave (stampa valore decimale ed esadecimale)
sudo ./smc set F0Mn 40   # scrive 40 nella chiave e rilegge per verifica
```

- La chiave deve essere di **esattamente 4 caratteri**.
- Il valore di `set` deve essere compreso tra **0 e 100**
  (`strtol` con base 0: accetta anche `0x28`).
- Exit code: `0` successo, `1` errore I/O o permessi, `2` errore di sintassi.
- Dopo una scrittura il valore viene rileto: se differisce viene stampato
  l'avviso `[ATTENZIONE: il valore letto e' diverso]`.

## Come funziona (protocollo)

L'SMC risponde sulle porte:

| Porta   | Ruolo                            |
|---------|----------------------------------|
| `0x300` | porta dati (`DATA_PORT`)         |
| `0x304` | porta comandi/stato (`CMD_PORT`) |

Bit di stato letti da `0x304`:

| Bit  | Nome                | Significato                                  |
|------|---------------------|----------------------------------------------|
| 0x01 | `ST_AWAITING_DATA`  | un byte e' pronto per la lettura             |
| 0x02 | `ST_IB_CLOSED`      | input buffer chiuso: si puo' inviare         |
| 0x04 | `ST_BUSY`           | l'SMC sta elaborando un comando              |

Comandi supportati: `0x10` (READ) e `0x11` (WRITE).

Sequenza di lettura di una chiave:

1. attesa che l'SMC sia libero (`smc_sane`; se resta busy invia un READ di flush);
2. invio del comando READ;
3. invio della chiave, un byte alla volta;
4. invio della lunghezza attesa;
5. lettura dei byte quando `AWAITING_DATA | BUSY` e' attivo;
6. drain degli eventuali byte residui e attesa fine elaborazione.

La scrittura segue lo stesso schema con il comando WRITE.
Ogni attesa usa backoff esponenziale (partendo da 8 us, fino a 24 tentativi),
quindi un timeout totale dell'ordine dei ~100 ms.

## Struttura del codice (`src/smc-tool.c`)

| Funzione        | Descrizione |
|-----------------|-------------|
| `wait_status`   | Esegue polling dello stato finche' `(status & mask) == val`, con backoff esponenziale; ritorna `-ETIMEDOUT` al timeout |
| `send_byte`     | Attende IB chiuso e poi BUSY attivo, quindi scrive un byte sulla porta indicata |
| `send_command`  | Attende IB chiuso e invia un byte di comando a `0x304` |
| `smc_sane`      | Garantisce che l'SMC non sia busy; se necessario invia un comando READ per sbloccarlo |
| `send_argument` | Invia i 4 caratteri della chiave sulla porta dati |
| `read_smc`      | Lettura completa di `len` byte dalla chiave indicata, con drain finale |
| `write_smc`     | Scrittura di `len` byte nella chiave indicata |
| `main`          | Parsing argomenti (`get`/`set`), check root, `ioperm(0x300, 32)`, esecuzione e stampa risultati |

Limiti attuali:

- gestisce solo chiavi da **1 byte**, valori **0-100**;
- ignora il descrittore di tipo della chiave (flag/formato SMC);
- nessuna whitelist delle chiavi considerate sicure;
- nessun build system, test o packaging.

## Roadmap - attivita' prima della pubblicazione su GitHub

Checklist di cio' che serve per rendere il progetto utilizzabile da altre persone
(non inclusi gli step di versionamento):

- [ ] **Makefile** con target `all`, `clean`, `install` e `PREFIX` configurabile
- [ ] **Licenza** open source (es. GPL-2.0, coerente col protocollo ispirato al driver kernel `applesmc`)
- [ ] **`.gitignore`** (binari, oggetti, output di build)
- [ ] Rimuovere il binario precompilato `exec/smc` dal repository e distribuirlo nelle GitHub Releases
- [ ] Sezione README con **modelli supportati/testati** e avvisi di sicurezza (rischio brick)
- [ ] Migliorie CLI: opzioni `--help` e `--version`, messaggi d'errore consistenti
- [ ] Supporto chiavi **multibyte** e lettura del descrittore di tipo SMC
- [ ] Whitelist delle chiavi modificabili oppure opzione esplicita `--force`
- [ ] **Test**: unit test della logica di parsing + test di integrazione guardati (skip senza hardware/root)
- [ ] Flag di compilazione rigorosi (`-Wall -Wextra -Werror`) e sanitizers
- [ ] **CI GitHub Actions**: build automatica (gcc/clang) su Ubuntu a ogni push/PR
- [ ] Pagina **man** (`smc.1`) ed help testuale completo
- [ ] `CONTRIBUTING.md` e `SECURITY.md` (divulgazione responsabile)
- [ ] Versionamento semantico, tag e release binarie
- [ ] Documentare alternative a root: capabilities (`setcap cap_sys_rawio+ep`) o regole udevi


## Licenza

Progetto rilasciato sotto licenza **GPL-2.0-only** — vedi [LICENSE](LICENSE).
Opzionale, il badge nella prima riga del README (quello grigio/verde che vedi sui repo):
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
