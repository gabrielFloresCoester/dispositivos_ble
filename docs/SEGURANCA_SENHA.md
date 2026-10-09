# Senha de acesso: versão provisória e solução definitiva

**Status (2026-10-01): só a versão PROVISÓRIA existe**, feita para a
demonstração na feira. Ela **não é controle de acesso**. A solução
definitiva abaixo vale para as duas linhas (SIM Connect e fwBLE) e
**precisa ser feita antes de qualquer unidade ir para campo**. Está também
em "Pendências registradas" no `README.md`.

## O que existe hoje (provisório)

`nrf54-app/index.html`, função `askAccessPin()`:

- Ao conectar, a interface lê o `paramDado` (área `0x00800800`) e abre um
  diálogo pedindo a senha de 4 dígitos.
- Confere **no navegador** contra `senha[4]` (offset 0x33) **ou** a senha
  padrão `1934`, igual à IHM do fwBLE (`ifIhmSenhConf()` / `senhaPadrao`
  em `ParamZarI.c`).
- 3 tentativas erradas, ou Cancelar, desconectam. Se não conseguir ler os
  parâmetros, também não entra (falha fechada).

**Por que não protege nada:**

1. O firmware não exige nada. Qualquer cliente BLE (ex.: o app nRF
   Connect) escreve direto na característica RX e comanda o atuador ou
   altera parâmetros.
2. A senha é **lida** do equipamento para ser conferida. Qualquer um lê os
   bytes 0x33–0x36 da área Painel.
3. O enlace BLE não é criptografado, então tudo trafega em claro e pode
   ser capturado por quem estiver perto escutando o rádio.
4. A senha padrão `1934` está no código da página.

Serve para a demonstração mostrar o fluxo de uso, e nada além disso.

## Solução definitiva (comum ao SIM Connect e ao fwBLE)

O que as duas linhas têm em comum é o **serviço GATT do Atuador**
(`06290001-…`, características RX/TX) e o **protocolo acgl/ifFerConfig**
por cima dele. A solução usa só essas duas camadas, então dá para
implementar igual no nRF52832 + SoftDevice S132 (nRF5 SDK 17) e no nRF54
+ Zephyr, e a mesma interface atende as duas.

### Camada 1: criptografia do enlace (pareamento LE Secure Connections)

- Pareamento **LESC "Just Works"**. Nenhuma das duas linhas tem como
  mostrar ou digitar um passkey sem a IHM, e o SIM Connect não tem IHM.
- As características RX/TX passam a exigir enlace criptografado. No Zephyr
  é `BT_GATT_PERM_WRITE_ENCRYPT` / `BT_GATT_PERM_READ_ENCRYPT`; no S132 é
  `SEC_JUST_WORKS` no `ble_gatts_attr_md_t` + Peer Manager com LESC
  (`nrf_ble_lesc`). O Web Bluetooth dispara o pareamento sozinho ao tocar
  numa característica protegida.
- Não protege contra MITM ativo durante o pareamento (limite do Just
  Works), mas acaba com a escuta passiva, que é o risco prático.

**Por que não usar a senha como passkey do pareamento:** passkey fixa é
fraqueza conhecida do BLE. Quem captura um pareamento recupera a passkey,
e depois pode se passar pelo dispositivo.

**Por que não fazer desafio-resposta (HMAC) com a senha:** com 4 dígitos
são 10 mil combinações. Quem capturar um desafio e a resposta testa todas
offline em milissegundos. Sem criptografia do enlace, nenhum esquema
baseado nessa senha se sustenta. Com criptografia, mandar a senha em claro
dentro do enlace já basta.

### Camada 2: autenticação por senha via acgl (comando novo)

- Comando novo na **área Comando** (`0x00800000`), comum às duas linhas,
  por exemplo `IFCC_AUTH`. **O código precisa ser reservado com a equipe
  do fwBLE**, para não colidir com a tabela `ifFerConfigEnuCmd_t`;
  `0x0020` está livre hoje.
- Os 4 dígitos vão no `frame`, logo após o comando (offsets +4..+7 da área).
- Resposta: `GTM_CONFIRM` se a senha confere, `GTM_NEG` se não. O
  `cmdStatus` diferencia "senha errada" de "bloqueado".
- Sessão autenticada **por conexão**, zerada ao desconectar.

### Camada 3: o que fica travado até autenticar (no firmware)

| Livre sem autenticar | Exige autenticação |
|---|---|
| `GTM_REQUEST` de status/sensor/alarmes (`0x00804080`, `0xF00000xx`) | Painel Remoto (`0x00805900`): abre, fecha, para, quita, modo |
| Leitura do `paramDado`, **com a senha mascarada** | Escrita no `paramDado` |
| Escrita de `IFCC_AUTH` na área Comando | `IFCC_SAVE` / `IFCC_RESTORE` e os demais comandos |

- A senha **nunca** volta em leitura, nem autenticado: os bytes 0x33–0x36
  vêm zerados. Divergência deliberada do layout do fwBLE.
- **Contra força bruta:** depois de 5 erros, bloqueio de 60 s, dobrando a
  cada novo bloqueio. Contador em RAM, por conexão e global.

## Decisões em aberto

1. **Senha padrão `1934` pela BLE.** Na IHM ela pressupõe acesso físico ao
   painel. Pela BLE vira uma senha mestra universal de alcance remoto,
   publicada em todo atuador. Recomendação: **não aceitar pela BLE**, ou só
   com o bit `fabPront` = 0 (unidade ainda não prontificada na fábrica).
2. **Gateway legado e o papel Gateway do SIM Connect.** Um Gateway que
   comanda atuadores também precisa se autenticar em cada um. Opções:
   guardar a senha de cada atuador junto da allow-list, ou ter uma
   credencial do Gateway. Até decidir, a trava quebraria o Gateway legado.
3. **Transição na frota.** Unidades fwBLE sem o comando novo respondem
   `GTM_NEG` ao `IFCC_AUTH`. A interface precisa diferenciar "não suporta"
   de "senha errada" (pelo `cmdStatus`) para não travar quem ainda não
   atualizou. Atualizar o fwBLE exige release nova + DFU nas unidades em
   campo.
4. **Senha padrão de fábrica `0000`.** Obrigar a troca no primeiro acesso?

## Roteiro sugerido (depois da feira)

1. Fechar as decisões acima com a equipe do fwBLE e reservar o código do
   `IFCC_AUTH`.
2. SIM Connect: LESC + permissões criptografadas nas características;
   `IFCC_AUTH`, gate e máscara da senha em `actuator_service.c`; bloqueio
   contra força bruta.
3. Interface: trocar `askAccessPin()` por `IFCC_AUTH` e tratar o pareamento
   do sistema operacional.
4. fwBLE: mesmas camadas 1–3 (Peer Manager LESC, `ifFerConfig.c`).
5. Teste de invasão básico: tentar comandar pelo nRF Connect sem senha,
   capturar o tráfego com um sniffer (nRF Sniffer) e confirmar que está
   criptografado.
