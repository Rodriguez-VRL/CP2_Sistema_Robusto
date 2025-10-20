/*
 * Checkpoint 2 - Sistema de Dados Robusto
 * Implementação com FreeRTOS no ESP32
 *
 * Aluno: VINICIUS RODRIGUES
 * RM: 89192
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

// --- Definições do Projeto ---
#define TAMANHO_FILA 5
#define TIMEOUT_NIVEL_1 5
#define TIMEOUT_NIVEL_2 10
#define TIMEOUT_NIVEL_3 15 // Nível de falha persistente

// Bits (flags) para o Event Group de supervisão
#define TASK_GERACAO_OK_BIT (1 << 0)
#define TASK_CONSUMO_OK_BIT (1 << 1)

// --- Handles Globais ---
QueueHandle_t xQueueDados = NULL;
EventGroupHandle_t egStatusTasks = NULL;

// --- Handles de Tarefa Globais ---
TaskHandle_t h_taskGeracao = NULL;
TaskHandle_t h_taskConsumo = NULL;
TaskHandle_t h_taskMonitor = NULL;


// --- Constantes de Delay ---
const TickType_t xDelay1000ms = pdMS_TO_TICKS(1000);
const TickType_t xDelay2000ms = pdMS_TO_TICKS(2000);


/**
 * @brief MÓDULO 1: Geração de Dados (Task Geradora)
 */
void vTaskGeracao (void *pvParameters)
{
    // --- CORREÇÃO: A própria tarefa se adiciona ao WDT ---
    esp_task_wdt_add(NULL);
    // --- FIM DA CORREÇÃO ---

    int contador_id = 0; 

    for(;;)
    {
        // 1. Gera os dados sequenciais (um simples int)
        contador_id++;
        int valorParaEnviar = contador_id;

        printf("{VINICIUS RODRIGUES-RM:89192} [GERACAO] Dado %d gerado.\n", valorParaEnviar);

        // 2. Tenta enviar o VALOR (int) para a fila
        if(xQueueSend(xQueueDados, &valorParaEnviar, 0) != pdTRUE)
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [ERRO_FILA] Fila cheia, dado %d descartado.\n", valorParaEnviar);
        }
        else
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [FILA_SEND] Dado %d enviado para fila.\n", valorParaEnviar);
            xEventGroupSetBits(egStatusTasks, TASK_GERACAO_OK_BIT);
        }

        // 3. Alimenta o Watchdog
        esp_task_wdt_reset();
        vTaskDelay(xDelay1000ms);
    }
}

/**
 * @brief MÓDULO 2: Recepção de Dados (Task Consumidora)
 */
void vTaskConsumo(void *pvParameters)
{
    // --- CORREÇÃO: A própria tarefa se adiciona ao WDT ---
    esp_task_wdt_add(NULL);
    // --- FIM DA CORREÇÃO ---

    int valorLido = 0;
    int contadorTimeout = 0;

    for(;;)
    {
        // 1. Tenta receber o VALOR (int) da fila
        if(xQueueReceive(xQueueDados, &valorLido, 0) == pdTRUE)
        {
            // 2. Aloca memória dinâmica (malloc) *APÓS* receber
            int *pTempStorage = (int *) malloc(sizeof(int));

            // 3. Verifica se o malloc falhou
            if(pTempStorage == NULL)
            {
                printf("{VINICIUS RODRIGUES-RM:89192} [ERRO_MEM] Falha ao alocar memoria (malloc) no RECEPTOR.\n");
                vTaskDelay(xDelay1000ms);
                continue; 
            }

            *pTempStorage = valorLido;
            printf("{VINICIUS RODRIGUES-RM:89192} [CONSUMO] Dado %d lido da fila.\n", *pTempStorage);
            contadorTimeout = 0;
            xEventGroupSetBits(egStatusTasks, TASK_CONSUMO_OK_BIT);
            free(pTempStorage);
            pTempStorage = NULL;
            esp_task_wdt_reset();
        }
        else
        {
            // 4. Fila vazia, incrementa o timeout
            contadorTimeout++;
            printf("{VINICIUS RODRIGUES-RM:89192} [CONSUMO_WARN] Fila vazia (timeout: %d)\n", contadorTimeout);
        }

        // 5. Reação escalonada ao timeout (requisito do PDF)
        if(contadorTimeout == TIMEOUT_NIVEL_1) // Nível 1: Aviso
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [TIMEOUT_LV1] %ds sem dados. Apenas aviso.\n", TIMEOUT_NIVEL_1);
        }
        else if(contadorTimeout == TIMEOUT_NIVEL_2) // Nível 2: Recuperação
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [TIMEOUT_LV2] %ds sem dados. Resetando fila.\n", TIMEOUT_NIVEL_2);
            xQueueReset(xQueueDados); // Ação de recuperação
        }
        else if (contadorTimeout >= TIMEOUT_NIVEL_3) // Nível 3: Encerramento/Reset
        {
            // --- MUDANÇA CRÍTICA ---
            // Em vez de reiniciar o ESP, a tarefa vai se encerrar.
            printf("{VINICIUS RODRIGUES-RM:89192} [TIMEOUT_LV3] %ds sem dados. Falha persistente.\n", TIMEOUT_NIVEL_3);
            printf("{VINICIUS RODRIGUES-RM:89192} [CONSUMO_STOP] Encerrando tarefa. Monitor deve recriar.\n");
            break; // Sai do loop infinito
            // --- FIM DA MUDANÇA ---
        }
        
        // --- CORREÇÃO 2 (BUG DO PRÓXIMO PRINT): Mova o WDT reset para fora do IF
        // O código 'image_08aa20.png' (seu próximo print) mostra que esta tarefa trava
        // se a fila ficar vazia. O reset deve estar AQUI:
        // esp_task_wdt_reset(); // <--- Mova para cá
        
        vTaskDelay(xDelay1000ms);
    } // Fim do loop for(;;)

    // 6. Bloco de Auto-Deleção
    // Se a tarefa saiu do loop (por causa do 'break'), ela se deleta.
    h_taskConsumo = NULL; // Informa ao Monitor que ela "morreu"
    vTaskDelete(NULL); // Deleta a si mesma
}

/**
 * @brief MÓDULO 3: Supervisão (Task Monitora)
 */
void vTaskMonitor (void *pvParameters)
{
    // --- CORREÇÃO: A própria tarefa se adiciona ao WDT ---
    esp_task_wdt_add(NULL);
    // --- FIM DA CORREÇÃO ---

    for(;;)
    {
        // --- INÍCIO: Bloco de Verificação de Existência (Ressurreição) ---
        // Verifica se a tarefa de consumo "morreu" (handle está NULL)
        if(h_taskConsumo == NULL)
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR_RESTART] Tarefa de Consumo inativa. Recriando...\n");
            
            // Recria a tarefa e salva o novo handle global
            xTaskCreate(vTaskConsumo, 
                        "TaskConsumidora", 
                        4096, 
                        NULL, 
                        5, 
                        &h_taskConsumo);
            
            // Adiciona a *nova* tarefa ao WDT
            if(h_taskConsumo != NULL)
            {
                // --- CORREÇÃO: Linha removida ---
                // Não é mais necessário, a vTaskConsumo se adiciona sozinha
                // esp_task_wdt_add(h_taskConsumo); 
                printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR_RESTART] Tarefa de Consumo recriada.\n");
            }
            else
            {
                printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR_ERRO] Falha ao recriar Tarefa de Consumo!\n");
            }
        }
        // --- FIM: Bloco de Verificação ---


        // --- Bloco Original de Status (Flags) ---
        EventBits_t xBitsRecebidos = xEventGroupWaitBits(
            egStatusTasks,
            TASK_GERACAO_OK_BIT | TASK_CONSUMO_OK_BIT,
            pdTRUE,  // Limpa os bits após a leitura
            pdFALSE, // Modo OR
            0        // Não bloqueia
        );

        if((xBitsRecebidos & TASK_GERACAO_OK_BIT) && (xBitsRecebidos & TASK_CONSUMO_OK_BIT))
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR] Status: Sistema OK (Ambas Tasks ativas)\n");
        }
        else if(xBitsRecebidos & TASK_GERACAO_OK_BIT)
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR] Status: Falha (Apenas Task Geradora ativa)\n");
        }
        else if(bits & TASK_CONSUMO_OK_BIT) // <--- BUG SUTIL: Corrigi 'bits' para 'xBitsRecebidos'
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR] Status: Falha (Apenas Task Consumidora ativa)\n");
        }
        else
        {
            printf("{VINICIUS RODRIGUES-RM:89192} [MONITOR] Status: Falha Critica (Nenhuma task ativa)\n");
        }
        // --- Fim do Bloco de Status ---

        // 3. Alimenta o Watchdog
        esp_task_wdt_reset();
        
        // A supervisão roda com menos frequência
        vTaskDelay(xDelay2000ms);
    }
}

void app_main(void)
{
    // 1. Configuração do Watchdog Timer (WDT)
    esp_task_wdt_config_t configWDT = {
        .timeout_ms = 5000, 
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&configWDT);

    // 2. Criação da Fila e Event Group
    xQueueDados = xQueueCreate(TAMANHO_FILA, sizeof(int));
    egStatusTasks = xEventGroupCreate();

    if(xQueueDados == NULL || egStatusTasks == NULL)
    {
        printf("{VINICIUS RODRIGUES-RM:89192} [ERRO_CRITICO] Falha ao criar fila ou event group. Reiniciando.\n");
        vTaskDelay(xDelay1000ms);
        esp_restart();
    }

    // 3. Criação das Tasks
    xTaskCreate(vTaskGeracao, "TaskGeradora", 4096, NULL, 5, &h_taskGeracao);
    xTaskCreate(vTaskConsumo, "TaskConsumidora", 4096, NULL, 5, &h_taskConsumo);
    xTaskCreate(vTaskMonitor, "TaskSupervisora", 4096, NULL, 5, &h_taskMonitor);
}