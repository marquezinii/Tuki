# Tuki

Ajustes universais de mouse para Windows 10/11. Funciona com qualquer mouse, porque age no nível do Windows (hook `WH_MOUSE_LL`) e não no driver do fabricante.

## Funções

**Rolagem**
- Direção: normal, invertida, **sempre para baixo** ou **sempre para cima** (qualquer giro vai para o mesmo lado)
- Velocidade (0,25× a 5×), com acúmulo de frações para rolagem lenta precisa
- Aceleração: giros rápidos andam mais
- Filtro de tremor: descarta o "salto" ao contrário que rodas gastas dão
- Shift + roda rola para os lados; inversão da rolagem lateral

**Botões**: roda, lateral de trás e lateral da frente podem virar clique, voltar/avançar, clique duplo, copiar/colar/desfazer, fechar aba, mídia, volume, área de trabalho, visão de tarefas, captura de tela ou ser desativados. Troca de botão esquerdo/direito.

**Cliques**: filtro de clique fantasma (debounce) por botão, com área de teste que mede o intervalo entre cliques.

**Ponteiro**: velocidade e "aprimorar precisão" do Windows.

**Aplicativos**: lista de exceções (ex.: jogos). Nesses apps o Tuki não altera nada.

**Geral**: iniciar com o Windows, atalho global **Ctrl+Alt+F12** para pausar, ícone na bandeja com troca rápida de direção, contadores.

## Arquitetura

| Parte | Tecnologia | Por quê |
|---|---|---|
| Motor (`src/engine.*`) | C++20 puro, sem Win32 | lógica testável; decide o que fazer com cada evento |
| Hook (`src/hook.*`) | Win32, thread dedicada de prioridade crítica | o Windows remove hooks lentos; sem GC, o callback leva microssegundos. A injeção (`SendInput`) roda em outra thread |
| App (`src/main.cpp`) | Win32 | bandeja, atalho global, instância única |
| Interface (`ui/`) | HTML/CSS/JS em WebView2 | criada só quando a janela abre e liberada ao fechar (~20 MB residente) |

Eventos injetados pelo Tuki são marcados (`dwExtraInfo = 'TUKI'`) para não serem processados de novo.

Configurações: `%APPDATA%\Tuki\config.json`.

## Compilar

Requisitos: Visual Studio Build Tools (C++), SDK do WebView2 e `nlohmann/json` em `C:\Tools` (ou `$env:TUKI_TOOLS`).

```powershell
.\build.ps1 -Test     # compila, roda os testes do motor e gera build\Tuki.exe
```

Teste ponta a ponta (com o Tuki rodando em modo "sempre para baixo" e botão da roda desativado): `build\e2e_mouse.exe`.

Prévia da interface no navegador (com simulador): `python -m http.server 5178 --directory ui`.

## Limitação conhecida

Janelas abertas como administrador não recebem os ajustes enquanto o Tuki roda sem privilégios (regra de segurança do Windows, UIPI). Para elas, execute o Tuki como administrador.
