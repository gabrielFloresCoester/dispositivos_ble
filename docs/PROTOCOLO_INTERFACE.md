# Contrato BLE do Gateway (SIM Connect) para a interface web

Tudo o que a página precisa saber. As UUIDs ficam hardcoded no JS — é
assim que qualquer app Web Bluetooth funciona, então o "Unknown
Characteristic" do nRF Connect não importa aqui.

## Serviço e characteristics

Serviço: `00001523-1212-efde-1523-785feabcd123`

| Characteristic | UUID (sufixo) | Propriedades | Formato |
|---|---|---|---|
| Actuator Raw Data | `...1527` | notify | `[slot] [resposta crua do atuador]` |
| Actuator Command | `...1528` | write | `[slot] [comando cru]` |
| Actuator Manage | `...1529` | write (8 B) | `[cmd] [addr_type] [MAC×6]` |
| **Actuator Status** | `...152a` | **read + notify** | ver abaixo |
| **Discovered Actuators** | `...152b` | **read + notify** | ver abaixo |
| **Gateway Control** | `...152c` | **write** | `[cmd] [arg…]` |

As três em negrito são novas nesta revisão.

O `requestDevice()` filtra por `...1523`; lembre de listar o serviço em
`optionalServices` também, senão o Chrome bloqueia o acesso ao GATT.

## Actuator Status (`...152a`)

**Read** devolve 19 registros de 41 bytes (779 bytes), em ordem de
slot. Passa do MTU mínimo, mas o GATT resolve via Read Blob e o
`readValue()` remonta transparente. É o sync inicial: chame ao conectar
e sempre que a página recarregar.

**Notify** manda **um** registro de 41 bytes — só o slot que mudou.
Notify não tem fragmentação, então os 779 de uma vez seriam truncados
em silêncio com MTU pequeno. (Um único registro de 41 bytes também
precisa de MTU ≥ 44 para não truncar — na prática nunca é problema,
porque o MTU já sobe para 247 logo após a conexão, bem antes de você
assinar qualquer notify.)

Registro (41 bytes), idêntico nos dois casos:

```
[0]     slot       0..18
[1]     state      0=vazio 1=aguardando 2=conectado 3=PRONTO 4=erro
[2]     addr_type  0=público 1=random
[3..8]  MAC        6 bytes, ordem interna do stack (invertida)
[9..40] nome       32 bytes, UTF-8, zero-padded à direita — é a mesma
                    TAG que o coleta_ble mostra (Local Name do
                    advertisement BLE). Pode vir vazio (zeros): nem
                    todo atuador anuncia nome, e o campo só é
                    preenchido depois que o Gateway o vê no scan pelo
                    menos uma vez. Trate como campo de tamanho fixo —
                    corte no primeiro 0x00, não assuma C-string.
```

**A diferença entre `2` e `3` importa.** Em `2` a conexão existe mas o
discovery ainda roda e o TX não foi assinado — um comando enviado
nessa janela é perdido sem erro. Só habilite o botão de comando em
`3` (PRONTO). O firmware já recusa (`-ENOTCONN`) e registra no log,
mas a UI não deveria nem oferecer.

## Discovered Actuators (`...152b`)

Atuadores que o Gateway viu no scan. É o que permite oferecer "encontrei
estes por perto, clique para adicionar" em vez de pedir um MAC
digitado à mão.

**Read** devolve todos os conhecidos (até 12 × 41 bytes).
**Notify** manda um registro quando um atuador novo aparece, quando o
nome dele é aprendido/muda, ou quando o RSSI de um conhecido muda mais
de 8 dBm (com no mínimo 3 s entre avisos de RSSI do mesmo endereço —
sem isso viraria metralhadora, já que um atuador anuncia várias vezes
por segundo; mudança de nome não tem esse limite, porque é rara).

```
[0]      addr_type  0=público 1=random
[1..6]   MAC        6 bytes, ordem invertida
[7]      rssi       int8, dBm (valor negativo)
[8]      flags      bit0 = já está na allow-list
[9..40]  nome       32 bytes, mesmo formato do registro de Status
```

Use `flags & 1` para marcar como "já adicionado" em vez de sumir com a
entrada — assim o operador vê que o atuador continua por perto.

## Ordem dos bytes do MAC

Os 6 bytes seguem a ordem interna do stack (`bt_addr_le_t.a.val`),
**invertida** em relação ao `AA:BB:CC:DD:EE:FF` que aparece no nRF
Connect. Essa conversão é responsabilidade do JavaScript, transparente
para o usuário:

```js
const macParaBytes = (mac) =>
  Uint8Array.from(mac.split(':').map(h => parseInt(h, 16)).reverse());

const bytesParaMac = (bytes) =>
  Array.from(bytes).reverse()
    .map(b => b.toString(16).padStart(2, '0').toUpperCase())
    .join(':');
```

## Gateway Control (`...152c`)

| Bytes | Efeito |
|---|---|
| `01` | Limpa o cache de descoberta |
| `02` | Força reinício do scan |
| `03 <slot>` | Desconecta o slot sem tirá-lo da allow-list |
| `04 <slot> <0\|1>` | Modo do enlace: 0 = lento, 1 = rápido |

O `03` é o "reset" útil quando um link fica estranho durante a demo: o
Gateway reconecta sozinho no próximo scan.

O `04` é o que torna viável baixar curvas com muitos atuadores
conectados — veja a seção seguinte.

## Modo rápido: obrigatório antes de coletas em lote

Todo enlace com atuador nasce **lento** (intervalo de conexão de
120–200 ms). Isso é o que permite 16 atuadores dividirem um único
rádio sem se atropelar, e é perfeitamente adequado para polling
periódico.

Mas a 200 ms de intervalo, um round trip leva ~400 ms — e uma curva
custa 2 round trips por registro. Uma coleta de 200 registros levaria
quase 3 minutos.

Por isso, **antes de iniciar uma sequência de curvas ou eventos**:

```js
await gatewayCtrl.writeValue(new Uint8Array([0x04, slot, 1]));  // rápido
// ... roda a sequência ...
await gatewayCtrl.writeValue(new Uint8Array([0x04, slot, 0]));  // lento
```

Com 15–30 ms, o mesmo download cai para algo em torno de 15 segundos.

Duas regras:

- **Um slot rápido por vez.** O rádio absorve um link acelerado sem
  prejudicar os outros; vários simultaneamente, não.
- **Sempre volte para lento no `finally`.** Se você esquecer, o Gateway
  reverte sozinho após 3 minutos e registra um aviso no log — mas até
  lá aquele link está consumindo tempo de rádio dos demais.

## Escala: quantos atuadores

O SoftDevice Controller da Nordic tem teto de **20 conexões simultâneas
em todos os papéis**. Com os Kits B e C aposentados:

```
20 total  −1 interface  =  19 atuadores
```

O firmware está configurado com `MAX_ACTUATORS = 19`, ou seja o teto
fica exatamente cheio. Se um atuador cair e voltar a anunciar antes de
o objeto de conexão antigo ser reciclado, a reconexão falha com
`-ENOMEM` e o Gateway tenta de novo na varredura seguinte — atraso de
alguns segundos, sem consequência.

Sobre a frequência de comunicação: **o enlace não morre por falta de
tráfego da aplicação.** O controlador troca pacotes vazios a cada
intervalo de conexão automaticamente; o supervision timeout só expira
quando eventos consecutivos são *perdidos* (fora de alcance,
interferência). Polling a cada 5 s ou a cada 5 min é indiferente para a
sobrevivência do link — não há necessidade de "keepalive" na aplicação.

## Persistência

A allow-list é gravada em memória não-volátil (ZMS) a cada
adição/remoção e recarregada no boot. Depois de um reboot os slots
reaparecem em estado `1` (aguardando) e reconectam sozinhos quando o
scan encontrar cada atuador — sem precisar redigitar nada.

Os índices de slot são preservados na ordem em que foram gravados, mas
**não confie neles como identificador estável**: chaveie sua UI pelo
MAC, não pelo número do slot.

## Regra crítica: um comando em voo por slot

O protocolo proCo **não tem ID de correlação** — a resposta que chega
não diz a qual comando pertence. O `coleta_ble` só funciona porque é um
cliente único e sequencial.

Consequência para a página: **serialize por slot**. Uma fila de
promises por slot, um comando em voo por vez, com timeout. Se você
disparar polling de posição/torque em paralelo com uma sequência de
curvas no mesmo atuador, as respostas se intercalam e as duas leituras
saem corrompidas — sem erro visível, só dados errados.

Slots diferentes podem operar em paralelo sem problema (o prefixo de
slot desambigua).

Pelo mesmo motivo, o Gateway **não** faz polling autônomo nesta versão.
Quem dirige é a interface, tanto o periódico quanto o sob demanda.

## Como interpretar as respostas

Notify da Raw Data: `[slot] [frame proCo…]`. Os índices do frame,
portanto, deslocam **um byte** em relação ao que o `coleta_ble` vê:

| Campo | No `coleta_ble` | No notify do Gateway |
|---|---|---|
| Tipo (ACK/dados) | `byte[2]` | `byte[3]` |
| Início do payload | `byte[8]` | `byte[9]` |

Filtro de resposta válida (equivalente ao `on_notify` do `client.py`):
`byte[3]` deve ser `0x05` (ACK) ou `0x06` (dados). Descarte o resto.

## Sequências de leitura

Comandos idênticos aos de `ble/protocol.py`. Todos vão prefixados pelo
slot na Actuator Command.

### Leitura direta (posição/torque, alarmes, dados gerais, firmware)

1. Escreva o comando de leitura.
2. Aguarde o notify com `byte[3] == 0x06`.
3. Payload a partir de `byte[9]`.

Bom para o polling periódico. Sugestão: 1–2 s de intervalo para
posição/torque, 5–10 s para dados gerais e alarmes.

### Leitura sequencial (curvas e eventos)

Reproduz o `ler_registros()` do `reader.py`. Note o **duplo READ**
depois do START — não é engano, é como o firmware do atuador responde.

```
START_LAST  → aguarda ACK   (byte[3] == 0x05)
CMD_READ    → aguarda dados (descarta)
CMD_READ    → aguarda dados → parse do primeiro registro

repetir:
  CMD_NEXT  → aguarda ACK
  CMD_READ  → aguarda dados → parse

parar quando:
  - o índice se repetir (fim do buffer circular), ou
  - o índice for menor/igual ao último já salvo (leitura incremental)
```

O campo de índice são os 4 primeiros bytes do payload, little-endian
(`uint32`), tanto em curva quanto em evento.

Antes de começar, vale ler os Dados Gerais: os campos
`index_reg_curv_torq` e `absol_index_reg_event` dizem quantos registros
existem, o que permite mostrar uma barra de progresso real em vez de um
spinner indefinido.

## Contexto seguro (Web Bluetooth)

Só `https://`, `http://localhost` e `http://127.0.0.1` contam como
contexto seguro. **`http://192.168.x.x` não funciona** — a API fica
indisponível.

- Notebook rodando a página e falando com o Gateway: `python -m http.server`
  e acesse `http://localhost:8000`. Resolvido.
- Celular acessando a página servida pelo notebook: precisa de HTTPS de
  verdade, ou port forwarding via `chrome://inspect`.

Vale decidir isso antes da demo, porque muda o roteiro.

## Sobre o nome vir vazio ("Sem nome")

Se isso acontecer com um atuador específico, o motivo mais provável é
que o firmware dele não anuncia **nenhum** Local Name — nem no
advertisement principal, nem no scan response. Nesse caso o
`coleta_ble` mostraria "Sem nome" também (é o mesmo fallback do
`scanner.py`), então não é bug do Gateway.

Se o nome **piscar**, alternando com "Sem nome", a causa é
`set_name_field()` voltar a sobrescrever o campo com vazio: o scan
response traz o nome e o advertisement principal não, então os dois se
alternam várias vezes por segundo. A regra é que um pacote sem nome
significa "não trouxe informação", nunca "apague o nome" — só a
remoção/reciclagem da entrada limpa o campo, via `memset` explícito.

Se o nome vier **cortado**, confira `ACTUATOR_NAME_MAX_LEN` (hoje 32) e
os buffers `char name[ACTUATOR_NAME_MAX_LEN + 1]` no `main.c` — o `+1`
existe para o terminador da C-string e, sem ele, o último caractere de
um nome que ocupe o campo inteiro se perde.

Se isso acontecer com **todos** os atuadores, é sinal de que o Gateway
parou de receber os pacotes de SCAN RESPONSE — verifique se
`bt_le_scan_cb_register(&scan_recv_callbacks)` continua sendo chamado
no `main()` (ver comentário em `scan_recv_cb` no `main.c`). Esse
callback existe justamente porque, em muitos produtos BLE, o nome vem
num pacote separado do que carrega a UUID do serviço — o `bt_scan`
sozinho não veria esse segundo pacote.

## Sobre a enxurrada de "Failed to start advertiser" (err -12 / status 0x0D)

Se isso reaparecer no RTT, especialmente logo após o boot com vários
atuadores reconectando ao mesmo tempo: status HCI `0x0D` é "Connection
Rejected due to Limited Resources" — o controlador recusou habilitar a
advertising porque está ocupado estabelecendo/derrubando conexões
simultâneas com os atuadores. Isso já foi corrigido, mas o motivo vale
registrar caso a lógica de advertising mude de novo no futuro:

Havia **dois disparadores concorrentes** de `bt_le_adv_start()`:
`on_disconnected()` chamava direto (cedo demais — o objeto de conexão
ainda não tinha sido liberado, o que já bastava para o `-ENOMEM`, e a
própria documentação do Zephyr confirma isso: "the stack still has one
reference to the connection object" nesse callback) e `.recycled`
chamava de novo, corretamente, um pouco depois.

A correção: a chamada prematura em `on_disconnected()` foi removida —
só `.recycled` reinicia a advertising agora, que é o padrão
recomendado desde a NCS 3.0 — e uma retentativa com backoff (300 ms →
até 2 s) foi adicionada para o caso genuíno de recurso esgotado
transitoriamente, em vez de desistir silenciosamente na primeira
falha.

(Uma primeira versão desta correção também adicionava
`BT_LE_ADV_OPT_ONE_TIME`, para impedir o host de tentar retomar a
advertising sozinho em paralelo. Removemos essa parte: o compilador
deste projeto não reconhece essa flag na combinação de Kconfig/versão
usada, e por ser um valor de enum — não uma macro de pré-processador —
não dá para protegê-la com `#if defined()`. As outras duas mudanças já
resolvem a causa confirmada do problema.)

## Sobre "err -120" (EALREADY) em advertising e em discovery

`-120` é `EALREADY` — "operação já em andamento". Apareceu em dois
lugares diferentes, com duas causas diferentes:

**Em `Advertising failed to start`**: o `advertising_start()` usava
`k_work_reschedule()`, que **cancela e reagenda** um timer pendente.
Durante uma rajada de (re)conexões — vários atuadores conectando perto
um do outro no tempo —, cada disparo de `.recycled` chamava
`advertising_start()` de novo, cancelando o backoff de 2 s que já
estava esperando e forçando outra tentativa imediata, que falhava de
novo pelo mesmo motivo (recurso do controlador ainda ocupado) — e
como a tentativa anterior às vezes nem tinha terminado de processar,
a nova esbarrava nela e voltava com `-120` em vez de `-ENOMEM`.
Corrigido trocando para `k_work_schedule()`, que só agenda se **não**
houver nada pendente — uma retentativa em andamento não é mais
cancelada por chamadas externas.

**Em `bt_gatt_dm_start (slot N) falhou`**: essa é uma restrição
**documentada** do módulo GATT Discovery Manager da própria Nordic —
"Only one discovery procedure can be started simultaneously". O
`bt_gatt_dm` é uma instância única e global, não uma por conexão. Com
vários atuadores conectando perto um do outro no tempo, duas
descobertas quase sempre se sobrepõem, e a segunda a chamar
`bt_gatt_dm_start()` falha na hora com `-120` — sem nenhuma tentativa
nova automática depois, deixando aquele slot preso em erro. Corrigido
com uma fila de descoberta (`discovery_queue*`, `actuator_client.c`):
se uma descoberta já está em andamento quando outro atuador conecta,
ele entra na fila em vez de chamar `bt_gatt_dm_start()` na hora; assim
que a descoberta atual termina — sucesso, serviço não encontrado ou
erro —, o próximo da fila é iniciado automaticamente.

Os `err -120` que aparecem colados a essas mensagens são resultado do
mesmo tumulto (uma tentativa de advertising disparada bem no instante
em que outra conexão estava sendo processada) — não são um problema
separado, e não devem mais aparecer com a correção acima.

## Caso em aberto: trava de advertising de ~30s após remoção, autorrecuperável

Observado uma vez em campo: depois de remover um atuador já conectado,
a advertising ficou recusando com status 0x0D por **34 segundos**
seguidos — bem mais que o padrão de "rajada de conexão" que os backoffs
acima resolvem — enquanto um outro atuador (já `PRONTO`) continuava
respondendo comandos normalmente o tempo todo. Depois desse período,
**destravou sozinha**, sem reiniciar o Gateway.

O que já foi descartado como causa:
- Não é a enxurrada de retries sem backoff (essa já tem correção, e o
  cadenciamento de 300ms→2s foi confirmado funcionando nesse mesmo
  log).
- Não é o modo rápido preso num atuador (confirmado que o botão de
  modo não tinha sido usado nesse teste).
- Não é vazamento permanente de recurso (autorrecuperou sem reboot).

O que ainda não foi confirmado: o mecanismo exato no controlador. A
hipótese mais provável é contenção de agenda de rádio (o link ativo
disputando espaço com a tentativa de advertising), mas isso não foi
verificado com uma ferramenta que mostre o estado interno do
controlador - só temos evidência indireta.

**Instrumentação adicionada para a próxima vez que isso acontecer:**
a mensagem de retentativa agora inclui quantos atuadores estão
conectados naquele instante:

```
Advertising sem recurso no controlador agora - 1 atuador(es)
conectado(s) neste instante - tentativa N, de novo em M ms
```

Se isso voltar a acontecer, o número ajuda a distinguir "muitas
conexões genuinamente disputando recurso" de outra coisa. Se o padrão
se repetir de forma consistente (mesma situação disparando toda vez),
vale ativar `CONFIG_BT_HCI_CORE_DEBUG=y` no `prj.conf` para ver o
estado de cada conexão no nível HCI - os logs atuais não têm esse
detalhe.

Por ora, o comportamento é aceitável: atrasa uma reconexão da
interface por até um minuto num cenário específico, mas não trava
nada, não perde dados dos atuadores, e se resolve sozinho.

## Erros comuns (leia antes de abrir chamado consigo mesmo)

**"Mandei o comando e a `1527` não respondeu nada."**
Quase sempre é o notify da `1527` que não foi habilitado. As
characteristics são independentes: assinar a `152a` e a `152b` não
assina a `1527`. Sem a assinatura, o Gateway recebe a resposta do atuador e
não tem para onde repassá-la. O log mostra:

```
Comando enviado ao slot N, mas a Actuator Raw Data (UUID ...1527)
NAO esta assinada - a resposta sera descartada.
```

E, quando a resposta chega:

```
Dados do slot N descartados: a interface nao assinou a Actuator Raw Data
```

**"A `152c` não mostra nada."**
Ela é write-only, não tem propriedade de leitura — não há valor para
exibir. Se o `01` (limpar descoberta) produz `Cache de descoberta
limpo` no log, ela está funcionando.

**"A `152a` mostra menos slots do que eu tenho."**
O buffer de leitura precisa comportar `MAX_ACTUATORS × 41` bytes. Se
você subir `MAX_ACTUATORS`, suba `MY_LBS_STATUS_BUF_LEN` em `my_lbs.h`
junto — senão a leitura vem truncada em silêncio.

**"Escrevi na `1528` e nada aconteceu."**
Confira o primeiro byte: ele é o **slot**, não parte do comando. Para o
slot 0 e o comando de posição, o write completo é
`00 01 00 03 0A 80 40 80 00` (nove bytes). O log confirma o repasse:

```
Comando de 8 bytes repassado ao slot 0
-> RX do atuador  01 00 03 0A 80 40 80 00
```

## Checklist de validação (nesta ordem)

0. **Assine o notify das TRÊS characteristics** — `1527` (Raw Data),
   `152a` (Status) e `152b` (Discovery) — antes de qualquer outra coisa.
   Esquecer a `1527` é o erro mais comum, e o sintoma não aponta para a
   causa.
1. **MTU.** Conecte a interface e procure no RTT:
   `MTU atualizado (...): tx=... rx=... (payload útil = N bytes)`.
   Devem aparecer **duas** linhas com endereços diferentes: uma da
   interface, uma do atuador. Se alguma ficar em 20, confira as configs
   de MTU do `prj.conf`.
2. Ligue um atuador sem adicioná-lo. Ele deve aparecer na Discovered
   Actuators com RSSI, e o Gateway **não** deve tentar conectar.
3. Adicione pelo endereço. Acompanhe o Status: `1` → `2` → `3`.
4. Com o slot em `3`, peça posição/torque (`01 00 03 0A 80 40 80 00`).
5. **Peça uma curva.** É aqui que o MTU aparece: a resposta tem 72
   bytes. Se chegar com 20, volte ao passo 1.
6. Remova o atuador. Status vai a `0`, ele desconecta e **não** volta a
   ser incomodado mesmo continuando por perto.
7. Repita com dois atuadores simultâneos, verificando que o prefixo de
   slot separa as respostas corretamente.

## Limitações conhecidas

- **Sem autenticação ou pareamento.** Qualquer um por perto pode
  escrever nas characteristics do Gateway — inclusive adicionar, remover e
  comandar atuadores. Aceitável para demonstração, não para campo.
  Quando for a hora, o caminho é `CONFIG_BT_SMP` com bonding e
  permissões `BT_GATT_PERM_WRITE_ENCRYPT` nas characteristics de
  escrita.
- **Reconexão em massa é lenta.** Se o Gateway reiniciar com 16 atuadores
  na lista, ele reconecta um de cada vez conforme o scan os encontra.
  Conte com alguns minutos até todos voltarem a `PRONTO`.
- **Os índices de slot mudam** entre reboots ou remoções. Chaveie a
  interface pelo MAC.
