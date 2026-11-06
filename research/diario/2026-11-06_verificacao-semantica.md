# Diário — 2026-11-06 — Verificação semântica e análise dimensional

| Campo | Valor |
|-------|-------|
| Data | 2026-11-06 |
| Responsável | BeathAzreal |
| Ferramentas | Claude Code (assistente de IA), GCC 13.3, Clang 18.1, CMake 3.28, AddressSanitizer/UBSan, Python (geradores de testes aleatórios) |
| Etapa do roadmap | v0.1 — núcleo, item 3 (nomes, unidades e dimensões) |
| Branch | `feat/semantic` → PR para `dev/mfl-core-v0.1` |
| Antes desta etapa | PR #3 (parser) integrado (`f3ccb34`), CI aprovado nas 4 plataformas |

## 1. Objetivo da sessão

Verificar, **antes de executar**, se um programa MFL faz sentido: nomes
declarados, `let` imutável, unidades conhecidas e **consistência
dimensional** de todas as operações. Esta é a contribuição central da
linguagem: erros como somar volts com ampères ou converter velocidade em
J/mm passam a ser detectados com linha e coluna.

## 2. Procedimento executado

1. **Registro de unidades** (`src/runtime/units.c`) conforme
   [`docs/units.md`](../../docs/units.md):
   - unidades de base SI, derivadas (N, Pa, J, W, C, V, ohm, Hz) e não SI
     (min, h, L, bar);
   - prefixos de p a G;
   - cada unidade é guardada como um fator de escala para o SI e um vetor
     de 7 expoentes inteiros (m, kg, s, A, K, mol, cd).
2. **Verificador** (`src/compiler/checker.c`):
   - percorre a AST e infere o tipo de cada expressão (`Number`,
     `Quantity[...]`, `Bool`, `String`, `Nil`);
   - aplica as regras dimensionais;
   - anota na AST os fatores de escala que o compilador vai usar;
   - um erro não gera mensagens em cascata.
3. **CLI:** `mfl --types arquivo.mfl` lista o tipo e a dimensão inferidos de
   cada variável; `mfl arquivo.mfl` agora executa todas as verificações.
4. **Novo exemplo** `examples/thermal/steel_properties.mfl`: difusividade
   térmica e número de Fourier do aço.
5. **Testes** unitários (`tests/unit/test_checker.c`) e de referência
   (`tests/language/check/`).

## 3. Resultados verificados

Dimensões inferidas para o programa de energia de soldagem
(`mfl --types examples/welding/heat_input.mfl`):

```
10:1 let voltage: Quantity[kg*m^2/(s^3*A)]
11:1 let current: Quantity[A]
12:1 let speed: Quantity[m/s]
13:1 let efficiency: Number
15:1 let energy: Quantity[kg*m/s^2]
```

Conferência manual: V·A/(m/s) = W·s/m = J/m = kg·m/s². Logo
`energy.to(J/mm)` é dimensionalmente válido, e o verificador aceita.

Exemplo térmico (`examples/thermal/steel_properties.mfl`):

| Grandeza | Expressão | Dimensão inferida | Esperado |
|----------|-----------|-------------------|----------|
| Difusividade | α = k/(ρ·c_p) | `m^2/s` | m²/s |
| Número de Fourier | Fo = α·Δt/Δx² | `Number` | adimensional |
| Potência do arco | 0,8·20 V·150 A | `kg*m^2/s^3` | W |
| Vazão de gás | 15 L/min | `m^3/s` | m³/s |

| Verificação | Resultado |
|-------------|-----------|
| Build GCC (Debug e Release) e Clang (Release) com `-Werror` | Sem warnings |
| CTest: 13 testes | 13/13 nos três builds |
| `test_checker`: 1428 verificações (registro de unidades, prefixos, ambiguidades, formatação de dimensões, estouro de expoentes, regras de tipos, mensagens e posições, fatores de escala, limite de erros, todos os limites de memória de 1 a 4000 bytes) | 0 falhas |
| Teste de referência com um nome alterado de propósito | Falhou, como esperado |
| ASan + UBSan (GCC): suíte, todos os `.mfl` do repositório, 300 arquivos aleatórios, 1500 programas com palavras embaralhadas e 3000 programas gerados a partir da gramática | Nenhum erro de memória |
| Programas gerados pela gramática que chegaram ao verificador | 2833 de 3000; todos os recusados vieram com mensagem e posição |
| CI no GitHub Actions | Ver PR |

## 4. Problemas encontrados

| Problema | Solução |
|----------|---------|
| `5 m / (2 s)` dava erro de sintaxe: pela regra do parser, `(` depois de `/` sempre abria um grupo de unidade | O parser passou a olhar dois tokens à frente: `(` só continua a unidade se for seguido de um nome ou de outro `(`. `W/(m*K)` continua funcionando, e `5 m / (2 s)` agora é uma divisão comum |
| Duas colunas esperadas erradas no próprio teste (o nó de `(1 m)` fica na posição do número, não do parêntese) | Expectativas corrigidas |
| O fuzz com palavras embaralhadas quase não exercitava o verificador (1495 de 1500 programas paravam na sintaxe) | Criado um gerador de programas sintaticamente válidos a partir da gramática |
| 167 dos programas "válidos" do gerador falharam na sintaxe | Analisados: todos tinham expoentes encadeados dentro de uma unidade (`min^2^-2`), o que a gramática não permite. Era erro do gerador, não do parser |

## 5. Decisões tomadas

Ver ADR-016 a ADR-019 em [`../decisoes.md`](../decisoes.md).

## 6. Uso de IA nesta sessão

Código, testes e registro gerados com o Claude Code a partir dos
requisitos do autor. Ver [`../uso-de-ia.md`](../uso-de-ia.md).

## 7. Próximos passos

1. Conversão numérica independente de *locale* (ADR-011).
2. Compilador para bytecode e máquina virtual: `let`, aritmética com
   fatores de escala, `.to()`, `print`.
3. Formato de saída de quantidades (spec §9.1) e o teste de conformidade
   `480 J/mm` passando.
