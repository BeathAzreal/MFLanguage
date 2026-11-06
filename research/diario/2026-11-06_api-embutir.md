# Diário — 2026-11-06 — API C para embutir a MFL em outros programas

| Campo | Valor |
|-------|-------|
| Data | 2026-11-06 |
| Responsável | BeathAzreal |
| Ferramentas | Claude Code (assistente de IA), GCC/G++ 13.3, Clang/Clang++ 18.1, CMake 3.28, AddressSanitizer/LeakSanitizer/UBSan, Python 3 (`fractions`) |
| Etapa do roadmap | Integração com jogos (validação, etapa 8) — base técnica |
| Branch | `feat/embedding-api`, criado a partir de `feat/vm` (PR #5 ainda não integrado) |

## 1. Objetivo da sessão

Permitir que outro programa, como o jogo SoldamatGamification, use a MFL
por trás da interface: o jogo passa as escolhas do jogador como grandezas
com unidade, executa um modelo MFL e lê os resultados na unidade que quiser
mostrar. O jogador nunca vê código MFL.

## 2. Procedimento executado

1. **API pública** em `include/mfl/mfl.h`:
   - estado (`MflState`) e scripts compilados (`MflScript`);
   - entradas: `mfl_set_number(S, "speed", 5, "mm/s")`;
   - resultados: `mfl_get_number(S, "energy", "kJ/mm", &q)`;
   - erros: códigos de status mais mensagens no formato `arquivo:linha:coluna`;
   - a saída de `print` vai para uma função do hospedeiro;
   - limite de memória configurável.
2. **Extensões internas:**
   - o verificador aceita entradas pré-declaradas (imutáveis, com
     dimensão conhecida);
   - a VM recebe valores iniciais e devolve os valores finais das variáveis;
   - o parser lê uma unidade isolada a partir de texto (`"W/(m*K)"`).
3. **Exemplo de hospedeiro** (`examples/embedding/heat_input_host.c`):
   - compila o modelo uma vez e o executa para três regulagens de soldagem;
   - mostra o tratamento de erros e captura a saída de `print`.
4. **Testes:**
   - `test_api` (3604 verificações), que usa só o cabeçalho público;
   - teste em C++ do cabeçalho;
   - teste de referência da saída do exemplo.

## 3. Resultados verificados

Saída do exemplo (conferida à mão: P = η·U·I; Q = P/v):

| Regulagem | U [V] | I [A] | v [mm/s] | P [kW] | Q [kJ/mm] |
|-----------|-------|-------|----------|--------|-----------|
| chapa fina | 18 | 120 | 8 | 1,728 | 0,216 |
| conformidade | 20 | 150 | 5 | 2,400 | 0,480 |
| chapa grossa | 28 | 280 | 4 | 6,272 | 1,568 |

| Verificação | Resultado |
|-------------|-----------|
| Build GCC (Debug, Release) e Clang (Release), C e C++, com `-Werror` | Sem warnings |
| CTest: 25 testes | 25/25 nos três builds |
| `test_api`: casos de uso, argumentos inválidos, unidades, resultados, erros de programa, compilar uma vez e rodar muitas, estados independentes, todos os limites de memória de 1 a 5000 bytes | 0 falhas |
| ASan + LeakSanitizer + UBSan: suíte completa e exemplo | Sem erros e sem vazamento de memória |
| Testes diferenciais (50000 expressões; 4000 conversões) | 0 divergências; pior erro 2,3 ulp |
| CI | Ver PR |

## 4. Problemas encontrados

| Problema | Solução |
|----------|---------|
| No exemplo, a primeira mensagem de erro saiu vazia: `printf("%s %s", status(chamada()), mensagem())` depende da ordem de avaliação dos argumentos, que em C não é definida | Chamada feita antes, mensagem lida depois |
| Um teste de memória falhava com "unknown unit 'v'": eu escrevi `150 A / v`, que é a armadilha documentada (o `v` é lido como unidade) | Teste corrigido para `(150 A) / v`. A mensagem da própria linguagem indicava a correção |
| `mfl_eval` recuperava o código de erro comparando o texto da mensagem (frágil) | Função interna que devolve o status diretamente |

### Experimento de precisão (revertido)

**Hipótese:** em `*` e `/`, multiplicar as magnitudes e combinar os fatores
de unidade de forma exata, convertendo para SI só no fim, deixaria os
resultados mais exatos. Exemplo motivador: `(3 mm)·(7 mm)` resultava em
`21.000000000000004 mm^2`.

**Medição** (4500 produtos e quocientes aleatórios de grandezas em
unidades mistas, comparados com aritmética racional exata):

| Entradas | SI primeiro (original) | Magnitudes primeiro (teste) |
|----------|------------------------|-----------------------------|
| Decimais aleatórios | 59,2% com arredondamento correto; pior caso 2,2 ulp | 50,0%; pior caso 2,1 ulp |
| Valores típicos (inteiros e decimais curtos) | **64,5%**; pior caso 2,0 ulp | 59,1%; pior caso 1,9 ulp |

**Conclusão:** a hipótese foi **refutada**. A mudança acertava alguns casos
"redondos", mas piorava a taxa de arredondamento correto em média. Ela foi
revertida, e a VM mantém a conversão para SI primeiro. O caso
`21.000000000000004` fica como limitação conhecida; uma solução de verdade
exigiria adiar a aplicação dos fatores até a conversão final (trabalho
futuro).

> Para o texto da IC/TCC, este é um exemplo de decisão guiada por medição:
> a intuição ("menos conversões é melhor") não se confirmou nos dados.

## 5. Decisões tomadas

Ver ADR-024 e ADR-025 em [`../decisoes.md`](../decisoes.md).

## 6. Uso de IA nesta sessão

Código, testes, experimento e registro gerados com o Claude Code a partir
dos requisitos do autor. Ver [`../uso-de-ia.md`](../uso-de-ia.md).

## 7. Próximos passos

1. Integrar ao SoldamatGamification usando `docs/embedding.md`.
2. v0.2: funções, `if`/`while`/`for`, arrays.
3. Módulo numérico: condução de calor 1D e verificação contra soluções
   analíticas.
